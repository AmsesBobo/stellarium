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

// Photographic mode of StelSkyDrawer: the images of planets and of the landscape as the camera records them: their
// statistics, their exposure and their star trails.

#include "StelSkyDrawer.hpp"
#include "StelProjector.hpp"
#include "StelTextureMgr.hpp"
#include "StelCore.hpp"
#include "StelPainter.hpp"

#include <QOpenGLContext>
#include <QOpenGLVertexArrayObject>
#include <QOpenGLShaderProgram>
#include <QOpenGLFramebufferObject>
#include <QOpenGLExtraFunctions>
#include <QVector2D>
#include <QDebug>
#include <QtGlobal>

bool StelSkyDrawer::photoTextureStatistics(StelTexture& texture, float power, float& mean, float& meanSquare, Vec3f* meanColor)
{
	mean = meanSquare = -1.f;
	if (!photoTextureMeanTried)
	{
		photoTextureMeanTried = true;
		// One fragment that averages 32 x 32 samples of a coarse mipmap level (each the mean of many texels)
		const char* vsrc = R"(
void main(void)
{
	// a triangle that covers the 1 x 1 viewport
	gl_Position = vec4(float((gl_VertexID & 1) * 4 - 1), float((gl_VertexID >> 1) * 4 - 1), 0., 1.);
}
)";
		const char* fsrc = R"(
uniform sampler2D tex;
uniform highp float lod;
uniform highp float power;
void main(void)
{
	highp vec3 sum = vec3(0.);
	highp float sum2 = 0.;
	for (int j = 0; j < 32; ++j)
		for (int i = 0; i < 32; ++i)
		{
			highp vec3 c = pow(max(textureLod(tex, (vec2(float(i), float(j)) + 0.5) / 32., lod).rgb, vec3(0.)), vec3(power));
			highp float y = dot(c, vec3(0.2126, 0.7152, 0.0722));
			sum += c;
			sum2 += y*y;
		}
	FRAG_COLOR = vec4(sum / 1024., sum2 / 1024.);
}
)";
		auto program = std::make_unique<QOpenGLShaderProgram>();
		if (program->addShaderFromSourceCode(QOpenGLShader::Vertex, StelOpenGL::globalShaderPrefix(StelOpenGL::VERTEX_SHADER) + vsrc) &&
		    program->addShaderFromSourceCode(QOpenGLShader::Fragment, StelOpenGL::globalShaderPrefix(StelOpenGL::FRAGMENT_SHADER) + fsrc) &&
		    StelPainter::linkProg(program.get(), "photographic texture mean"))
		{
			photoTextureMeanProgram = std::move(program);
			photoTextureMeanFbo.reset(new QOpenGLFramebufferObject(1, 1, QOpenGLFramebufferObject::NoAttachment, GL_TEXTURE_2D, GL_RGBA32F));
			photoTextureMeanVao.reset(new QOpenGLVertexArrayObject);
			photoTextureMeanVao->create();
			if (!photoTextureMeanFbo->isValid())
				photoTextureMeanProgram.reset();
		}
	}
	if (!photoTextureMeanProgram)
		return false;
	int w = 0, h = 0;
	if (!texture.getDimensions(w, h) || w <= 0 || h <= 0)
		return false;

	auto& gl = *QOpenGLContext::currentContext()->extraFunctions();
	GLint previousFbo = 0;
	GLint viewport[4] = {0, 0, 0, 0};
	gl.glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFbo);
	gl.glGetIntegerv(GL_VIEWPORT, viewport);
	GLint previousVao = 0;
	gl.glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &previousVao);
	const GLboolean blend = gl.glIsEnabled(GL_BLEND);
	const GLboolean scissor = gl.glIsEnabled(GL_SCISSOR_TEST);
	const GLboolean depth = gl.glIsEnabled(GL_DEPTH_TEST);
	gl.glDisable(GL_BLEND);
	gl.glDisable(GL_SCISSOR_TEST);
	gl.glDisable(GL_DEPTH_TEST);

	photoTextureMeanFbo->bind();
	gl.glViewport(0, 0, 1, 1);
	texture.bind(0);
	photoTextureMeanProgram->bind();
	photoTextureMeanProgram->setUniformValue("tex", 0);
	photoTextureMeanProgram->setUniformValue("power", power);
	// The mipmap level where 32 samples cover the image. The squares are of averaged texels, so <Y^2> comes out
	// somewhat smaller than for the full resolution: good enough for a calibration.
	photoTextureMeanProgram->setUniformValue("lod", qMax(0.f, std::log2(static_cast<float>(qMax(w, h))/32.f)));
	photoTextureMeanVao->bind();
	gl.glDrawArrays(GL_TRIANGLES, 0, 3);
	gl.glBindVertexArray(static_cast<GLuint>(previousVao));
	float pixel[4] = {0.f, 0.f, 0.f, 0.f};
	gl.glReadPixels(0, 0, 1, 1, GL_RGBA, GL_FLOAT, pixel);
	photoTextureMeanProgram->release();

	gl.glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previousFbo));
	gl.glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
	if (blend) gl.glEnable(GL_BLEND);
	if (scissor) gl.glEnable(GL_SCISSOR_TEST);
	if (depth) gl.glEnable(GL_DEPTH_TEST);

	mean = 0.2126f*pixel[0] + 0.7152f*pixel[1] + 0.0722f*pixel[2];
	meanSquare = pixel[3];
	if (meanColor)
		meanColor->set(pixel[0], pixel[1], pixel[2]);
	return std::isfinite(mean) && std::isfinite(meanSquare);
}

bool StelSkyDrawer::beginPhotoBodyImage(const Vec3d& posJ2000, float radius, bool glow)
{
	PhotoBodyImage& im = photoBodyImage;
	im.active = false;
	if (!getFlagPhotographicModeActive() || !(radius > 0.f))
		return false;
	const StelProjectorP prj = core->getProjection(StelCore::FrameJ2000);
	Vec3d win;
	if (!prj->project(posJ2000, win))
		return false;
	// the star trail of the body (no visibility test: flux "infinite") and the blur of the lens
	PhotoStarInstance trailInfo;
	const float length = computePhotoTrail(*prj, posJ2000, win, 1e30f, 0.f, trailInfo);
	// The halo and the spikes are already images of the blurred lens
	const float defocus = glow ? 0.f : photoLensCache.defocus;
	if (length < 0.75f && std::fabs(defocus) < 0.5f)
		return false;
	const Vec2f A(trailInfo.trail[0], trailInfo.trail[1]), B(trailInfo.trail[2], trailInfo.trail[3]);
	const Vec2f C = -0.5f*(A + B);
	const float reach = qMax(qMax(A.norm(), B.norm()), C.norm()) + std::fabs(defocus) + 2.f;
	// The sharp image: the disk with a border, but only what can reach the viewport
	const int vx = prj->getViewportPosX(), vy = prj->getViewportPosY();
	const int vw = prj->getViewportWidth(), vh = prj->getViewportHeight();
	const float r = radius + 2.f;
	const int x0 = qMax(static_cast<int>(std::floor(win[0] - r)), static_cast<int>(std::floor(vx - reach)));
	const int y0 = qMax(static_cast<int>(std::floor(win[1] - r)), static_cast<int>(std::floor(vy - reach)));
	const int x1 = qMin(static_cast<int>(std::ceil(win[0] + r)), static_cast<int>(std::ceil(vx + vw + reach)));
	const int y1 = qMin(static_cast<int>(std::ceil(win[1] + r)), static_cast<int>(std::ceil(vy + vh + reach)));
	const int w = x1 - x0, h = y1 - y0;
	if (w <= 0 || h <= 0 || w > 8192 || h > 8192)
		return false;

	if (!photoBodyTried)
	{
		photoBodyTried = true;
		const char* vsrc = R"(
ATTRIBUTE highp vec2 position; // window coordinates
uniform highp vec4 viewport;   // x, y, width, height of the viewport
VARYING highp vec2 vWin;
void main(void)
{
	vWin = position;
	gl_Position = vec4((position - viewport.xy) / viewport.zw * 2. - 1., 0., 1.);
}
)";
		const char* fsrc = R"(
VARYING highp vec2 vWin;
uniform sampler2D tex;
uniform highp vec2 origin;      // window position of texel (0,0)
uniform highp vec2 texSize;     // size of the texture (texels)
uniform highp vec2 imageSize;   // the part of it that holds the image
uniform highp vec4 trail;       // positions at the start (xy) and the end (zw) relative to mid-exposure
uniform highp vec3 logStart;    // extinction along the trail
uniform highp vec3 logEnd;
uniform highp float defocus;    // radius of the blur disk
uniform highp float defocusSign;
uniform highp float defocusSA;
uniform int blades;
uniform highp float straightness;
uniform highp float rotation;
uniform int samples;
uniform highp float lod;
const highp float PI = 3.14159265358979;
void main(void)
{
	highp vec2 A = trail.xy;
	highp vec2 B = trail.zw;
	highp vec2 C = -0.5*(A + B);
	highp vec4 sum = vec4(0.);
	highp float weights = 0.;
	highp float n = float(samples);
	for (int i = 0; i < 1024; ++i)
	{
		if (i >= samples) break;
		highp float fi = float(i) + 0.5;
		// Time along the exposure: in order for a pure trail, else well spread (golden ratio)
		highp float t = defocus != 0. ? fract(fi*0.6180339887) : fi/n;
		highp vec2 offset = (1.-t)*(1.-t)*A + 2.*t*(1.-t)*C + t*t*B;
		highp float w = 1.;
		if (defocus != 0.)
		{
			// A point of the image of the aperture (golden-angle spiral, even density)
			highp float rr = sqrt(fi/n);
			highp float a = float(i)*2.39996323;
			highp float scale = 1.;
			if (blades >= 3 && straightness > 0.)
			{
				highp float nb = float(blades);
				highp float sector = 2.*PI/nb;
				highp float d = mod(a - rotation, sector) - 0.5*sector;
				scale = 1. / mix(1., cos(d)/cos(PI/nb), straightness);
			}
			offset += defocusSign * abs(defocus) * rr * scale * vec2(cos(a), sin(a));
			w = max(0., 1. + defocusSA*(rr*rr - 0.5));
		}
		// the light here comes from the point of the sharp image that was offset onto it
		highp vec2 p = vWin - offset - origin;
		highp vec4 c = vec4(0.);
		if (p.x >= 0. && p.y >= 0. && p.x <= imageSize.x && p.y <= imageSize.y)
			c = textureLod(tex, p / texSize, lod);
		highp vec3 tint = exp(logStart*((1.-t)*(1.-2.*t)) + logEnd*(t*(2.*t-1.)));
		sum += w * vec4(c.rgb*tint, c.a);
		weights += w;
	}
	FRAG_COLOR = weights > 0. ? sum / weights : vec4(0.); // premultiplied: the body covers what is behind it
}
)";
		auto program = std::make_unique<QOpenGLShaderProgram>();
		if (program->addShaderFromSourceCode(QOpenGLShader::Vertex, StelOpenGL::globalShaderPrefix(StelOpenGL::VERTEX_SHADER) + vsrc) &&
		    program->addShaderFromSourceCode(QOpenGLShader::Fragment, StelOpenGL::globalShaderPrefix(StelOpenGL::FRAGMENT_SHADER) + fsrc))
		{
			program->bindAttributeLocation("position", 0);
			if (StelPainter::linkProg(program.get(), "photographic body image"))
			{
				photoBodyProgram = std::move(program);
				photoBodyVao.reset(new QOpenGLVertexArrayObject);
				photoBodyVao->create();
			}
		}
	}
	if (!photoBodyProgram)
		return false;
	// One image buffer, grown when needed (with depth for the rings)
	if (!photoBodyFbo || photoBodyFbo->width() < w || photoBodyFbo->height() < h)
	{
		const int fw = qMax(w, photoBodyFbo ? photoBodyFbo->width() : 0);
		const int fh = qMax(h, photoBodyFbo ? photoBodyFbo->height() : 0);
		QOpenGLFramebufferObjectFormat format;
		format.setAttachment(QOpenGLFramebufferObject::Depth);
		format.setInternalTextureFormat(GL_RGBA16F);
		format.setMipmap(true);
		photoBodyFbo.reset(new QOpenGLFramebufferObject((fw + 63)/64*64, (fh + 63)/64*64, format));
		if (!photoBodyFbo->isValid())
		{
			photoBodyFbo.reset();
			return false;
		}
	}

	auto& gl = *QOpenGLContext::currentContext()->extraFunctions();
	gl.glGetIntegerv(GL_FRAMEBUFFER_BINDING, &im.previousFbo);
	gl.glGetIntegerv(GL_VIEWPORT, im.viewport);
	gl.glGetFloatv(GL_COLOR_CLEAR_VALUE, im.clearColor);
	im.scissor = gl.glIsEnabled(GL_SCISSOR_TEST);
	gl.glDisable(GL_SCISSOR_TEST);
	photoBodyFbo->bind();
	// The projection stays: the viewport is shifted so that window pixel (x0, y0) lands on texel (0, 0)
	gl.glViewport(im.viewport[0] - x0, im.viewport[1] - y0, im.viewport[2], im.viewport[3]);
	gl.glClearColor(0.f, 0.f, 0.f, 0.f);
	gl.glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	im.x0 = x0; im.y0 = y0; im.w = w; im.h = h;
	im.margin = reach;
	im.trail = length > 0.f ? trailInfo.trail : Vec4f(0.f);
	for (int c=0; c<3; ++c)
	{
		im.logStart[c] = std::log(qMax(1e-9f, trailInfo.trailStart[c]));
		im.logEnd[c] = std::log(qMax(1e-9f, trailInfo.trailEnd[c]));
	}
	im.defocus = std::fabs(defocus) >= 0.5f ? defocus : 0.f;
	im.glow = glow;
	im.active = true;
	return true;
}

void StelSkyDrawer::endPhotoBodyImage()
{
	PhotoBodyImage& im = photoBodyImage;
	if (!im.active)
		return;
	im.active = false;
	auto& gl = *QOpenGLContext::currentContext()->extraFunctions();
	gl.glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(im.previousFbo));
	gl.glViewport(im.viewport[0], im.viewport[1], im.viewport[2], im.viewport[3]);
	gl.glClearColor(im.clearColor[0], im.clearColor[1], im.clearColor[2], im.clearColor[3]);
	if (im.scissor)
		gl.glEnable(GL_SCISSOR_TEST);

	// Mipmaps: the samples of a long trail or a large blur disk read a correspondingly smoothed image
	gl.glActiveTexture(GL_TEXTURE0);
	gl.glBindTexture(GL_TEXTURE_2D, photoBodyFbo->texture());
	gl.glGenerateMipmap(GL_TEXTURE_2D);
	gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
	gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

	const Vec2f A(im.trail[0], im.trail[1]), B(im.trail[2], im.trail[3]);
	const float length = A.norm() + B.norm();
	const float R = std::fabs(im.defocus);
	// Samples: about one per pixel along the trail and per two square pixels of the blur disk
	const int samples = qBound(8, static_cast<int>(std::ceil(length + 0.5f*M_PIf*R*R)), 256);
	const float area = length*qMax(1.f, 2.f*R) + M_PIf*R*R;
	const float lod = 0.5f*std::log2(qMax(1.f, area/static_cast<float>(samples)));

	auto& prog = *photoBodyProgram;
	prog.bind();
	prog.setUniformValue("viewport", QVector4D(im.viewport[0], im.viewport[1], im.viewport[2], im.viewport[3]));
	prog.setUniformValue("tex", 0);
	prog.setUniformValue("origin", QVector2D(im.x0, im.y0));
	prog.setUniformValue("texSize", QVector2D(photoBodyFbo->width(), photoBodyFbo->height()));
	prog.setUniformValue("imageSize", QVector2D(im.w, im.h));
	prog.setUniformValue("trail", QVector4D(A[0], A[1], B[0], B[1]));
	prog.setUniformValue("logStart", QVector3D(im.logStart[0], im.logStart[1], im.logStart[2]));
	prog.setUniformValue("logEnd", QVector3D(im.logEnd[0], im.logEnd[1], im.logEnd[2]));
	prog.setUniformValue("defocus", im.defocus);
	prog.setUniformValue("defocusSign", im.defocus > 0.f ? -1.f : 1.f);
	prog.setUniformValue("defocusSA", im.defocus > 0.f ? -photoLensCache.sphericalAberration : photoLensCache.sphericalAberration);
	const int blades = photoApertureBlades >= 3 ? photoApertureBlades : 0;
	const float sector = blades > 0 ? 2.f*M_PIf/static_cast<float>(blades) : 0.f;
	prog.setUniformValue("blades", blades);
	prog.setUniformValue("straightness", blades > 0 ? getPhotoApertureStraightness() : 0.f);
	prog.setUniformValue("rotation", PHOTO_SPIKE_ANGLE - 0.5f*sector);
	prog.setUniformValue("samples", samples);
	prog.setUniformValue("lod", lod);

	// The quad: the image and how far the trail and the blur reach
	const float m = im.margin;
	const GLfloat quad[] = {
		im.x0 - m, im.y0 - m,   im.x0 + im.w + m, im.y0 - m,
		im.x0 - m, im.y0 + im.h + m,   im.x0 + im.w + m, im.y0 + im.h + m };
	GLint previousVao = 0;
	gl.glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &previousVao);
	GLint previousBuffer = 0;
	gl.glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &previousBuffer);
	photoBodyVao->bind();
	GLuint vbo = 0;
	gl.glGenBuffers(1, &vbo);
	gl.glBindBuffer(GL_ARRAY_BUFFER, vbo);
	gl.glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STREAM_DRAW);
	gl.glEnableVertexAttribArray(0);
	gl.glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
	const GLboolean blend = gl.glIsEnabled(GL_BLEND);
	const GLboolean depth = gl.glIsEnabled(GL_DEPTH_TEST);
	GLint blendSrcRGB, blendDstRGB, blendSrcA, blendDstA;
	gl.glGetIntegerv(GL_BLEND_SRC_RGB, &blendSrcRGB);
	gl.glGetIntegerv(GL_BLEND_DST_RGB, &blendDstRGB);
	gl.glGetIntegerv(GL_BLEND_SRC_ALPHA, &blendSrcA);
	gl.glGetIntegerv(GL_BLEND_DST_ALPHA, &blendDstA);
	gl.glEnable(GL_BLEND);
	gl.glDisable(GL_DEPTH_TEST);
	if (im.glow)
		gl.glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ZERO, GL_ONE); // light added to the scene
	else
		gl.glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	gl.glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	gl.glBlendFuncSeparate(static_cast<GLenum>(blendSrcRGB), static_cast<GLenum>(blendDstRGB), static_cast<GLenum>(blendSrcA), static_cast<GLenum>(blendDstA));
	if (!blend) gl.glDisable(GL_BLEND);
	if (depth) gl.glEnable(GL_DEPTH_TEST);
	gl.glDisableVertexAttribArray(0);
	gl.glDeleteBuffers(1, &vbo);
	gl.glBindVertexArray(static_cast<GLuint>(previousVao));
	gl.glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(previousBuffer));
	prog.release();
}

std::vector<StelSkyDrawer::PhotoImageSample> StelSkyDrawer::getPhotoImageSamples(const StelProjector& prj, const Vec3d& center) const
{
	std::vector<PhotoImageSample> samples;
	if (!getFlagPhotographicModeActive())
		return samples;
	Vec3d win;
	if (!prj.project(center, win))
		return samples;
	PhotoStarInstance trailInfo;
	const float length = qMax(0.f, computePhotoTrail(prj, center, win, 1e30f, 0.f, trailInfo));
	const float defocus = std::fabs(photoLensCache.defocus) >= 0.5f ? photoLensCache.defocus : 0.f;
	const float R = std::fabs(defocus);
	if (length < 0.75f && R == 0.f)
		return samples;
	// About one copy per pixel of the trail and per two square pixels of the blur disk (as for the bodies)
	const int n = qBound(2, static_cast<int>(std::ceil(length + 0.5f*M_PIf*R*R)), 96);
	const double pixelPerRad = static_cast<double>(prj.getPixelPerRadAtCenter());
	Vec3d view = center;
	prj.getModelViewTransform()->forward(view);
	view.normalize();
	const int blades = photoApertureBlades >= 3 ? photoApertureBlades : 0;
	const float straightness = blades > 0 ? getPhotoApertureStraightness() : 0.f;
	const float sector = blades > 0 ? 2.f*M_PIf/static_cast<float>(blades) : 0.f;
	const float rotation = PHOTO_SPIKE_ANGLE - 0.5f*sector;
	const float sign = defocus > 0.f ? -1.f : 1.f;
	const float sa = defocus > 0.f ? -photoLensCache.sphericalAberration : photoLensCache.sphericalAberration;
	float weights = 0.f;
	samples.reserve(static_cast<size_t>(n));
	for (int i=0; i<n; ++i)
	{
		const float fi = static_cast<float>(i) + 0.5f;
		// Time along the exposure: in order for a pure trail, else well spread over the disk (golden ratio)
		const float t = R > 0.f ? std::fmod(fi*0.6180339887f, 1.f) : fi/static_cast<float>(n);
		Mat4d rot = Mat4d::identity();
		Vec3f factor(1.f);
		if (length > 0.f)
		{
			// t = 0 at the start of the exposure (see photoTrailToStart)
			rot = Mat4d::rotation(photoTrailAxisView, photoTrailAngle*(0.5 - static_cast<double>(t)));
			if (photoTrailExtinction)
				factor = photoTrailExtinctionFactor(rot*view, view);
		}
		float w = 1.f;
		if (R > 0.f)
		{
			// A point of the image of the aperture (golden-angle spiral, even density), as a small turn of the view
			const float rr = std::sqrt(fi/static_cast<float>(n));
			const float a = static_cast<float>(i)*2.39996323f;
			float scale = 1.f;
			if (blades > 0 && straightness > 0.f)
			{
				const float d = std::fmod(std::fmod(a - rotation, sector) + sector, sector) - 0.5f*sector;
				scale = 1.f / (1.f + (std::cos(d)/std::cos(M_PIf/static_cast<float>(blades)) - 1.f)*straightness);
			}
			const double dx = static_cast<double>(sign*R*rr*scale*std::cos(a));
			const double dy = static_cast<double>(sign*R*rr*scale*std::sin(a));
			rot = Mat4d::yrotation(-dx/pixelPerRad) * Mat4d::xrotation(dy/pixelPerRad) * rot;
			w = qMax(0.f, 1.f + sa*(rr*rr - 0.5f));
		}
		weights += w;
		samples.push_back({rot, factor*w});
	}
	if (!(weights > 0.f))
	{
		samples.clear();
		return samples;
	}
	for (auto& s : samples)
		s.factor /= weights;
	return samples;
}

StelProjectorP StelSkyDrawer::getPhotoImageSampleProjector(const StelProjectorP& prj, const Mat4d& viewRotation) const
{
	// The turn in view coordinates, expressed in the frame of the model: M^T R M (M: the rotation of the model view)
	StelProjector::ModelViewTranformP modelView = prj->getModelViewTransform()->clone();
	Mat4d m = modelView->getApproximateLinearTransfo();
	m[12] = m[13] = m[14] = 0.;
	modelView->combine(m.transpose() * viewRotation * m);
	return core->getProjection(modelView, static_cast<StelCore::ProjectionType>(1000));
}

bool StelSkyDrawer::beginPhotoGroundImage()
{
	PhotoGroundImage& im = photoGroundImage;
	im.active = false;
	if (!photoGroundTrailActive || !getFlagPhotographicModeActive())
		return false;
	auto& gl = *QOpenGLContext::currentContext()->extraFunctions();
	GLint viewport[4];
	gl.glGetIntegerv(GL_VIEWPORT, viewport);
	// The image covers the window up to the viewport: texel = window pixel, nothing else changes
	const int w = viewport[0] + viewport[2], h = viewport[1] + viewport[3];
	if (w <= 0 || h <= 0 || w > 16384 || h > 16384)
		return false;

	// The composite program depends on the projection (its unprojection and projection functions)
	const StelProjector::ModelViewTranformP identity(new StelProjector::Mat4dTransform(Mat4d::identity(), Mat4d::identity()));
	const StelProjectorP prj = core->getProjection(identity);
	if (!photoGroundProgram || !photoGroundProjector || !prj->isSameProjection(*photoGroundProjector))
	{
		photoGroundProgram.reset();
		photoGroundProjector = prj;
		const char* vsrc = R"(
ATTRIBUTE highp vec2 position; // window coordinates
uniform highp vec4 viewport;
VARYING highp vec2 vWin;
void main(void)
{
	vWin = position;
	gl_Position = vec4((position - viewport.xy) / viewport.zw * 2. - 1., 0., 1.);
}
)";
		// The unprojection and the projection both declare some uniforms: keep the first declaration only
		const QByteArray unproject = prj->getUnProjectShader();
		QByteArray project;
		for (const QByteArray& line : prj->getProjectShader().split('\n'))
		{
			if (!(line.trimmed().startsWith("uniform ") && unproject.contains(line.trimmed())))
				project += line + '\n';
		}
		const QByteArray fsrc = unproject + project + R"(
VARYING highp vec2 vWin;
uniform sampler2D tex;
uniform highp vec2 texSize;
uniform highp vec3 axis;    // rotation axis of the sky (view coordinates)
uniform highp float angle;  // angle it turns during the exposure
uniform int samples;
void main(void)
{
	bool ok = false;
	highp vec3 dir = unProject(vWin.x, vWin.y, ok);
	highp vec2 here = clamp(vWin, vec2(0.5), texSize - 0.5) / texSize;
	if (!ok)
	{
		FRAG_COLOR = texture2D(tex, here);
		return;
	}
	dir = normalize(dir);
	// Nothing of the landscape within reach of the arc (the sky, zoomed in): skip the samples. A coarse level of
	// the mipmaps around this pixel, larger than the arc, holds the mean of everything the arc can reach.
	highp float reach = angle * PROJECTOR_pixelPerRad * length(cross(axis, dir));
	if (dot(textureLod(tex, here, log2(max(1., 2.*reach)) + 1.), vec4(1.)) <= 0.)
	{
		FRAG_COLOR = vec4(0.);
		return;
	}
	// The camera turns with the sky: this pixel sees the ground that turns past it. The samples read
	// a correspondingly smoothed image (mipmaps) where they lie far apart.
	highp float spacing = reach / float(samples);
	highp float lod = log2(max(1., spacing));
	highp vec4 sum = vec4(0.);
	for (int i = 0; i < 256; ++i)
	{
		if (i >= samples)
			break;
		highp float a = angle * ((float(i) + 0.5)/float(samples) - 0.5);
		highp float c = cos(a), s = sin(a);
		highp vec3 d = dir*c + cross(axis, dir)*s + axis*(dot(axis, dir)*(1. - c));
		highp vec3 win = project(d);
		// outside the image: the nearest edge stands for what lies beyond the frame
		highp vec2 p = clamp(win.xy, vec2(0.5), texSize - 0.5);
		sum += textureLod(tex, p / texSize, lod);
	}
	FRAG_COLOR = sum / float(samples); // premultiplied: the ground covers the sky a part of the time
}
)";
		auto program = std::make_unique<QOpenGLShaderProgram>();
		if (program->addShaderFromSourceCode(QOpenGLShader::Vertex, StelOpenGL::globalShaderPrefix(StelOpenGL::VERTEX_SHADER) + vsrc) &&
		    program->addShaderFromSourceCode(QOpenGLShader::Fragment, StelOpenGL::globalShaderPrefix(StelOpenGL::FRAGMENT_SHADER) + fsrc))
		{
			program->bindAttributeLocation("position", 0);
			if (StelPainter::linkProg(program.get(), "photographic ground image"))
				photoGroundProgram = std::move(program);
		}
		if (!photoGroundProgram)
			qWarning() << "Photographic mode: the landscape cannot be blurred for the star tracker";
	}
	if (!photoGroundProgram)
		return false;
	if (!photoGroundFbo || photoGroundFbo->width() != w || photoGroundFbo->height() != h)
	{
		QOpenGLFramebufferObjectFormat format;
		format.setAttachment(QOpenGLFramebufferObject::Depth);
		format.setInternalTextureFormat(GL_RGBA16F);
		format.setMipmap(true);
		photoGroundFbo.reset(new QOpenGLFramebufferObject(w, h, format));
		if (!photoGroundFbo->isValid())
		{
			photoGroundFbo.reset();
			return false;
		}
	}
	gl.glGetIntegerv(GL_FRAMEBUFFER_BINDING, &im.previousFbo);
	for (int i=0; i<4; ++i)
		im.viewport[i] = viewport[i];
	gl.glGetFloatv(GL_COLOR_CLEAR_VALUE, im.clearColor);
	photoGroundFbo->bind();
	gl.glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
	gl.glClearColor(0.f, 0.f, 0.f, 0.f);
	gl.glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	im.active = true;
	return true;
}

void StelSkyDrawer::endPhotoGroundImage()
{
	PhotoGroundImage& im = photoGroundImage;
	if (!im.active)
		return;
	im.active = false;
	auto& gl = *QOpenGLContext::currentContext()->extraFunctions();
	gl.glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(im.previousFbo));
	gl.glViewport(im.viewport[0], im.viewport[1], im.viewport[2], im.viewport[3]);
	gl.glClearColor(im.clearColor[0], im.clearColor[1], im.clearColor[2], im.clearColor[3]);

	gl.glActiveTexture(GL_TEXTURE0);
	gl.glBindTexture(GL_TEXTURE_2D, photoGroundFbo->texture());
	gl.glGenerateMipmap(GL_TEXTURE_2D);
	gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
	gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	// About one sample per pixel of the longest arc (90 degrees from the pole)
	const double pixelPerRad = static_cast<double>(photoGroundProjector->getPixelPerRadAtCenter());
	const int samples = qBound(2, static_cast<int>(std::ceil(photoTrailAngle*pixelPerRad)), 256);

	auto& prog = *photoGroundProgram;
	prog.bind();
	photoGroundProjector->setUnProjectUniforms(prog);
	photoGroundProjector->setProjectUniforms(prog);
	prog.setUniformValue("viewport", QVector4D(im.viewport[0], im.viewport[1], im.viewport[2], im.viewport[3]));
	prog.setUniformValue("tex", 0);
	prog.setUniformValue("texSize", QVector2D(photoGroundFbo->width(), photoGroundFbo->height()));
	prog.setUniformValue("axis", QVector3D(static_cast<float>(photoTrailAxisView[0]), static_cast<float>(photoTrailAxisView[1]), static_cast<float>(photoTrailAxisView[2])));
	prog.setUniformValue("angle", static_cast<float>(photoTrailAngle));
	prog.setUniformValue("samples", samples);

	const float x0 = static_cast<float>(im.viewport[0]), y0 = static_cast<float>(im.viewport[1]);
	const float x1 = x0 + static_cast<float>(im.viewport[2]), y1 = y0 + static_cast<float>(im.viewport[3]);
	const GLfloat quad[] = { x0, y0,   x1, y0,   x0, y1,   x1, y1 };
	GLint previousVao = 0;
	gl.glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &previousVao);
	GLint previousBuffer = 0;
	gl.glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &previousBuffer);
	if (!photoBodyVao)
	{
		photoBodyVao.reset(new QOpenGLVertexArrayObject);
		photoBodyVao->create();
	}
	photoBodyVao->bind();
	GLuint vbo = 0;
	gl.glGenBuffers(1, &vbo);
	gl.glBindBuffer(GL_ARRAY_BUFFER, vbo);
	gl.glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STREAM_DRAW);
	gl.glEnableVertexAttribArray(0);
	gl.glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
	const GLboolean blend = gl.glIsEnabled(GL_BLEND);
	const GLboolean depth = gl.glIsEnabled(GL_DEPTH_TEST);
	GLint blendSrcRGB, blendDstRGB, blendSrcA, blendDstA;
	gl.glGetIntegerv(GL_BLEND_SRC_RGB, &blendSrcRGB);
	gl.glGetIntegerv(GL_BLEND_DST_RGB, &blendDstRGB);
	gl.glGetIntegerv(GL_BLEND_SRC_ALPHA, &blendSrcA);
	gl.glGetIntegerv(GL_BLEND_DST_ALPHA, &blendDstA);
	gl.glEnable(GL_BLEND);
	gl.glDisable(GL_DEPTH_TEST);
	gl.glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	gl.glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	gl.glBlendFuncSeparate(static_cast<GLenum>(blendSrcRGB), static_cast<GLenum>(blendDstRGB), static_cast<GLenum>(blendSrcA), static_cast<GLenum>(blendDstA));
	if (!blend) gl.glDisable(GL_BLEND);
	if (depth) gl.glEnable(GL_DEPTH_TEST);
	gl.glDisableVertexAttribArray(0);
	gl.glDeleteBuffers(1, &vbo);
	gl.glBindVertexArray(static_cast<GLuint>(previousVao));
	gl.glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(previousBuffer));
	prog.release();
}
