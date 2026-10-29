/*
 * Stellarium
 * Copyright (C) 2026 Stellarium contributors
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Suite 500, Boston, MA  02110-1335, USA.
 */

#include "StelPhotoMeter.hpp"
#include "StelOpenGL.hpp"
#include "StelPainter.hpp"

#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLFramebufferObject>
#include <QOpenGLVertexArrayObject>
#include <QDebug>
#include <algorithm>
#include <cmath>

#ifndef GL_PIXEL_PACK_BUFFER
#define GL_PIXEL_PACK_BUFFER 0x88EB
#endif
#ifndef GL_STREAM_READ
#define GL_STREAM_READ 0x88E1
#endif
#ifndef GL_MAP_READ_BIT
#define GL_MAP_READ_BIT 0x0001
#endif

namespace
{
const char* vertexSource = R"(
ATTRIBUTE vec3 vertex;
void main()
{
	gl_Position = vec4(vertex, 1.);
}
)";

// One output texel per segment: the mean luminance of the segment (red), from 12x12 samples spread
// evenly over it, and the brightest of its 4x4 sub-segments (green), for the highlights: a small
// bright object (the Moon in a wide field) fills a sub-segment, but only a part of a segment.
// Blue and alpha hold the mean red and blue (linear), for the automatic white balance.
// Infinities and NaN (overflows of extreme sources) must not spoil the mean.
const char* meterSource = R"(
uniform sampler2D scene;
uniform ivec2 sceneSize;   // the part that is measured (the frame of the sensor)
uniform ivec2 sceneOrigin; // its lower left texel
uniform ivec2 textureSize; // the whole texture
const int GRID = )" "48" R"(;
const int SUB = 4;       // sub-segments per direction
const int SUBSAMPLES = 3; // samples per sub-segment and direction
highp float sane(highp float x) { return (x >= 0.) ? min(x, 1e30) : 0.; }
void main()
{
	ivec2 cell = ivec2(gl_FragCoord.xy);
	highp vec2 cellSize = vec2(sceneSize) / float(GRID);
	highp vec2 origin = vec2(sceneOrigin) + vec2(cell) * cellSize;
	highp vec2 step = cellSize / float(SUB*SUBSAMPLES);
	highp float sum = 0.;
	highp float sumR = 0.;
	highp float sumB = 0.;
	highp float brightest = 0.;
	for (int sj = 0; sj < SUB; ++sj)
		for (int si = 0; si < SUB; ++si)
		{
			highp float subSum = 0.;
			for (int j = 0; j < SUBSAMPLES; ++j)
				for (int i = 0; i < SUBSAMPLES; ++i)
				{
					highp vec2 k = vec2(float(si*SUBSAMPLES + i), float(sj*SUBSAMPLES + j)) + 0.5;
					ivec2 texel = clamp(ivec2(origin + k*step), ivec2(0), textureSize - ivec2(1));
					highp vec3 c = texelFetch(scene, texel, 0).rgb;
					c = vec3(sane(c.r), sane(c.g), sane(c.b));
					subSum += sane(dot(c, vec3(0.2126, 0.7152, 0.0722)));
					sumR += c.r;
					sumB += c.b;
				}
			sum += subSum;
			brightest = max(brightest, subSum / float(SUBSAMPLES*SUBSAMPLES));
		}
	highp float n = float(SUB*SUB*SUBSAMPLES*SUBSAMPLES);
	FRAG_COLOR = vec4(sum / n, brightest, min(sumR / n, 1e30), min(sumB / n, 1e30));
}
)";
static_assert(StelPhotoMeter::GRID == 48, "update the GRID constant in the meter shader");

// AreaMeans: the exact mean luminance of all the pixels of each segment (red): the stars are a few pixels, which the
// samples above would mostly miss
const char* areaSource = R"(
uniform sampler2D scene;
uniform ivec2 sceneSize;
uniform ivec2 sceneOrigin;
uniform ivec2 textureSize;
const int GRID = )" "48" R"(;
highp float sane(highp float x) { return (x >= 0.) ? min(x, 1e30) : 0.; }
void main()
{
	ivec2 cell = ivec2(gl_FragCoord.xy);
	ivec2 lo = sceneOrigin + (cell*sceneSize)/GRID;
	ivec2 hi = max(lo + ivec2(1), sceneOrigin + ((cell + ivec2(1))*sceneSize)/GRID);
	hi = min(hi, textureSize);
	highp float sum = 0.;
	for (int y = lo.y; y < hi.y; ++y)
		for (int x = lo.x; x < hi.x; ++x)
		{
			highp vec3 c = texelFetch(scene, ivec2(x, y), 0).rgb;
			sum += sane(dot(vec3(sane(c.r), sane(c.g), sane(c.b)), vec3(0.2126, 0.7152, 0.0722)));
		}
	highp float n = float(max(1, (hi.x - lo.x)*(hi.y - lo.y)));
	FRAG_COLOR = vec4(min(sum / n, 1e30), 0., 0., 0.);
}
)";

constexpr int BUFFER_SIZE = StelPhotoMeter::GRID * StelPhotoMeter::GRID * 4 * static_cast<int>(sizeof(float));
}

StelPhotoMeter::StelPhotoMeter(Mode m)
	: mode(m)
{
}

StelPhotoMeter::~StelPhotoMeter()
{
	QOpenGLContext* ctx = QOpenGLContext::currentContext();
	if (!ctx)
		return;
	for (GLsync& f : fence)
		if (f)
			ctx->extraFunctions()->glDeleteSync(f);
	if (pbo[0] || pbo[1])
		ctx->extraFunctions()->glDeleteBuffers(2, pbo);
}

void StelPhotoMeter::init()
{
	initTried = true;
	QOpenGLContext* ctx = QOpenGLContext::currentContext();
	if (!ctx)
		return;
	QOpenGLExtraFunctions& gl = *ctx->extraFunctions();

	program.reset(new QOpenGLShaderProgram);
	program->addShaderFromSourceCode(QOpenGLShader::Vertex, StelOpenGL::globalShaderPrefix(StelOpenGL::VERTEX_SHADER) + vertexSource);
	program->addShaderFromSourceCode(QOpenGLShader::Fragment, StelOpenGL::globalShaderPrefix(StelOpenGL::FRAGMENT_SHADER) + (mode == AreaMeans ? areaSource : meterSource));
	program->bindAttributeLocation("vertex", 0);
	if (!StelPainter::linkProg(program.get(), "photographic mode: exposure meter"))
	{
		program.reset();
		qWarning() << "Photographic mode: the exposure meter could not be built, automatic exposure is off";
		return;
	}

	QOpenGLFramebufferObjectFormat format;
	format.setAttachment(QOpenGLFramebufferObject::NoAttachment);
#if QT_CONFIG(opengles2)
	format.setInternalTextureFormat(GL_RGBA16F);
#else
	format.setInternalTextureFormat(GL_RGBA32F);
#endif
	fbo.reset(new QOpenGLFramebufferObject(GRID, GRID, format));

	gl.glGenBuffers(2, pbo);
	for (const GLuint b : pbo)
	{
		gl.glBindBuffer(GL_PIXEL_PACK_BUFFER, b);
		gl.glBufferData(GL_PIXEL_PACK_BUFFER, BUFFER_SIZE, nullptr, GL_STREAM_READ);
	}
	gl.glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
	ok = fbo->isValid() && pbo[0] && pbo[1];
	if (!ok)
		qWarning() << "Photographic mode: no buffers for the exposure meter, automatic exposure is off";
}

void StelPhotoMeter::reset()
{
	pending[0] = pending[1] = false;
}

void StelPhotoMeter::measure(GLuint sceneTexture, int w, int h, double sceneScale, double exposureTime, QOpenGLVertexArrayObject* vao,
                             int x0, int y0, int texWidth, int texHeight)
{
	if (!initTried)
		init();
	if (!ok || !vao || w < GRID || h < GRID)
		return;
	// The previous measurement is still on its way (the GPU is behind, see takeReading()): it is read first, a new
	// one starts after that. So a GPU that lags a few frames gives a reading every few frames, never none.
	if (pending[1 - current])
		return;
	QOpenGLExtraFunctions& gl = *QOpenGLContext::currentContext()->extraFunctions();

	GLint savedViewport[4];
	gl.glGetIntegerv(GL_VIEWPORT, savedViewport);
	GLint savedFbo = 0;
	gl.glGetIntegerv(GL_FRAMEBUFFER_BINDING, &savedFbo);
	const GLboolean blendWasOn = gl.glIsEnabled(GL_BLEND);
	gl.glDisable(GL_BLEND);

	// 1. The segments
	fbo->bind();
	gl.glViewport(0, 0, GRID, GRID);
	program->bind();
	gl.glActiveTexture(GL_TEXTURE0);
	gl.glBindTexture(GL_TEXTURE_2D, sceneTexture);
	program->setUniformValue("scene", 0);
	gl.glUniform2i(program->uniformLocation("sceneSize"), w, h);
	gl.glUniform2i(program->uniformLocation("sceneOrigin"), x0, y0);
	gl.glUniform2i(program->uniformLocation("textureSize"), texWidth > 0 ? texWidth : x0 + w, texHeight > 0 ? texHeight : y0 + h);
	vao->bind();
	gl.glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	vao->release();
	program->release();

	// 2. Start the transfer into a pixel buffer; it is read in a later frame
	gl.glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo[current]);
	gl.glReadPixels(0, 0, GRID, GRID, GL_RGBA, GL_FLOAT, nullptr);
	gl.glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
	if (fence[current])
		gl.glDeleteSync(fence[current]);
	fence[current] = gl.glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
	pending[current] = true;
	pendingInfo[current].sceneScale = sceneScale;
	pendingInfo[current].exposureTime = exposureTime;
	pendingW[current] = w;
	pendingH[current] = h;
	current = 1 - current;

	gl.glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(savedFbo));
	gl.glViewport(savedViewport[0], savedViewport[1], savedViewport[2], savedViewport[3]);
	if (blendWasOn)
		gl.glEnable(GL_BLEND);
}

bool StelPhotoMeter::takeReading(Reading& reading, double clipLevel)
{
	// The buffer filled by the last measure(), in the previous frame. Its transfer is usually done by now; if the
	// GPU is still behind, it is tried again in the next frame instead of waiting for it here (mapping the buffer
	// would block until the GPU is done).
	const int slot = 1 - current;
	if (!ok || !pending[slot])
		return false;
	QOpenGLExtraFunctions& gl = *QOpenGLContext::currentContext()->extraFunctions();
	if (fence[slot])
	{
		const GLenum status = gl.glClientWaitSync(fence[slot], 0, 0);
		if (status == GL_TIMEOUT_EXPIRED)
			return false;
		gl.glDeleteSync(fence[slot]);
		fence[slot] = nullptr;
	}
	gl.glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo[slot]);
	const void* data = gl.glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, BUFFER_SIZE, GL_MAP_READ_BIT);
	bool result = false;
	if (data)
	{
		if (mode == AreaMeans)
		{
			reading = Reading();
			const float* cells = static_cast<const float*>(data);
			reading.cells.resize(static_cast<size_t>(GRID)*GRID);
			for (int k=0; k<GRID*GRID; ++k)
				reading.cells[static_cast<size_t>(k)] = cells[4*k];
			reading.width = pendingW[slot];
			reading.height = pendingH[slot];
		}
		else
			reading = evaluate(static_cast<const float*>(data), pendingW[slot], pendingH[slot], clipLevel);
		reading.sceneScale = pendingInfo[slot].sceneScale;
		reading.exposureTime = pendingInfo[slot].exposureTime;
		gl.glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
		result = true;
	}
	gl.glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
	pending[slot] = false;
	return result;
}

StelPhotoMeter::Reading StelPhotoMeter::evaluate(const float* cells, int w, int h, double clipLevel) const
{
	Reading r;
	const double cw = static_cast<double>(w)/GRID;
	const double ch = static_cast<double>(h)/GRID;
	const double softness = 0.5*(cw+ch); // smooth edges of the circles, about one segment
	// Center-weighted: a circle of 1/3 of the image height (diameter)
	const double centerRadius = h/6.;
	// Spot: 2.5% of the image area
	const double spotRadius = std::sqrt(0.025*w*h/M_PI);

	double sumAll = 0., sumCenter = 0., weightCenter = 0., sumSpot = 0., weightSpot = 0., maxCell = 0.;
	double grey[3] = {0., 0., 0.};
	std::vector<double> segments;
	segments.reserve(GRID*GRID);
	const auto sane = [](double x) { return (x >= 0. && std::isfinite(x)) ? x : 0.; };
	for (int j=0; j<GRID; ++j)
	{
		for (int i=0; i<GRID; ++i)
		{
			double m = static_cast<double>(cells[4*(j*GRID+i)]);
			if (!(m >= 0.) || !std::isfinite(m))
				m = 0.;
			double peak = static_cast<double>(cells[4*(j*GRID+i)+1]);
			if (!(peak >= 0.) || !std::isfinite(peak))
				peak = 0.;
			const double dx = (i+0.5)*cw - 0.5*w;
			const double dy = (j+0.5)*ch - 0.5*h;
			const double d = std::sqrt(dx*dx+dy*dy);
			const double inCenter = std::clamp((centerRadius-d)/softness + 0.5, 0., 1.);
			const double inSpot = std::clamp((spotRadius-d)/softness + 0.5, 0., 1.);
			sumAll += m;
			segments.push_back(m);
			sumCenter += inCenter*m;
			weightCenter += inCenter;
			sumSpot += inSpot*m;
			weightSpot += inSpot;
			maxCell = std::max(maxCell, peak);
			// Grey world for the white balance: like a camera, leave out what saturates the sensor
			const double red = sane(static_cast<double>(cells[4*(j*GRID+i)+2]));
			const double blue = sane(static_cast<double>(cells[4*(j*GRID+i)+3]));
			const double green = std::max(0., (m - 0.2126*red - 0.0722*blue)/0.7152);
			if (red < clipLevel && green < clipLevel && blue < clipLevel)
			{
				grey[0] += red;
				grey[1] += green;
				grey[2] += blue;
			}
		}
	}
	const double meanAll = sumAll/(GRID*GRID);
	r.centerWeighted = weightCenter > 0. ? 0.25*meanAll + 0.75*sumCenter/weightCenter : meanAll;
	r.spot = weightSpot > 0. ? sumSpot/weightSpot : meanAll;
	r.highlight = maxCell;
	// The log-average without the brightest 2% of the segments. A black segment counts as a thousandth of the mean.
	{
		std::sort(segments.begin(), segments.end());
		const size_t n = segments.size() - segments.size()/50;
		double trimmedSum = 0.;
		for (size_t k=0; k<n; ++k)
			trimmedSum += segments[k];
		const double floor = 1e-3*trimmedSum/static_cast<double>(std::max<size_t>(1, n));
		double logSum = 0.;
		for (size_t k=0; k<n; ++k)
			logSum += std::log(std::max(segments[k], floor));
		r.logMean = (n > 0 && floor > 0.) ? std::exp(logSum/static_cast<double>(n)) : 0.;
	}
	r.greyRed = grey[0];
	r.greyGreen = grey[1];
	r.greyBlue = grey[2];
	return r;
}

// ---- StelPhotoProbe ----

StelPhotoProbe::~StelPhotoProbe()
{
	QOpenGLContext* ctx = QOpenGLContext::currentContext();
	if (!ctx)
		return;
	if (fence)
		ctx->extraFunctions()->glDeleteSync(fence);
	if (pbo)
		ctx->extraFunctions()->glDeleteBuffers(1, &pbo);
}

void StelPhotoProbe::measure(GLuint fbo, int x, int y, int w, int h, double sceneScale)
{
	QOpenGLContext* ctx = QOpenGLContext::currentContext();
	if (!ctx)
		return;
	QOpenGLExtraFunctions& gl = *ctx->extraFunctions();
	if (!initTried)
	{
		initTried = true;
		gl.glGenBuffers(1, &pbo);
		gl.glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo);
		gl.glBufferData(GL_PIXEL_PACK_BUFFER, SIZE*SIZE*4*static_cast<int>(sizeof(float)), nullptr, GL_STREAM_READ);
		gl.glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
		ok = pbo != 0;
	}
	if (!ok || pending || !(sceneScale > 0.) || w < SIZE || h < SIZE)
		return;
	// the square around the point, inside the image
	const int x0 = qBound(0, x - SIZE/2, w - SIZE);
	const int y0 = qBound(0, y - SIZE/2, h - SIZE);

	GLint savedReadFbo = 0;
	gl.glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &savedReadFbo);
	gl.glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
	gl.glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo);
	gl.glReadPixels(x0, y0, SIZE, SIZE, GL_RGBA, GL_FLOAT, nullptr);
	gl.glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
	gl.glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(savedReadFbo));
	if (fence)
		gl.glDeleteSync(fence);
	fence = gl.glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
	pending = true;
	pendingScale = sceneScale;
	pendingCount = SIZE*SIZE;
}

bool StelPhotoProbe::takeReading(double& luminance)
{
	if (!ok || !pending)
		return false;
	QOpenGLExtraFunctions& gl = *QOpenGLContext::currentContext()->extraFunctions();
	if (fence)
	{
		// not done yet: try again in the next frame instead of waiting here
		if (gl.glClientWaitSync(fence, 0, 0) == GL_TIMEOUT_EXPIRED)
			return false;
		gl.glDeleteSync(fence);
		fence = nullptr;
	}
	gl.glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo);
	const void* data = gl.glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, SIZE*SIZE*4*static_cast<int>(sizeof(float)), GL_MAP_READ_BIT);
	bool result = false;
	if (data)
	{
		const float* px = static_cast<const float*>(data);
		double sum = 0.;
		for (int i=0; i<pendingCount; ++i)
		{
			const double Y = 0.2126*static_cast<double>(px[4*i]) + 0.7152*static_cast<double>(px[4*i+1]) + 0.0722*static_cast<double>(px[4*i+2]);
			if (Y > 0. && std::isfinite(Y))
				sum += Y;
		}
		luminance = sum / pendingCount / pendingScale;
		gl.glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
		result = true;
	}
	gl.glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
	pending = false;
	return result;
}
