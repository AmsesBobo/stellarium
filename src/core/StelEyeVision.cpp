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

#include "StelEyeVision.hpp"
#include "StelOpenGL.hpp"
#include "StelPainter.hpp"

#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLFramebufferObject>
#include <QOpenGLVertexArrayObject>
#include <QVector2D>
#include <cmath>
#include <algorithm>

namespace
{
const char* vertexSource = R"(
ATTRIBUTE vec3 vertex;
void main()
{
	gl_Position = vec4(vertex, 1.);
}
)";

// One direction of the Gaussian blur, weighted by the part of each pixel that is scene (not annotation):
// in rgb the weighted sum of the scene, in alpha the sum of the weights. The first pass reads the scene (the
// coverage of the annotations in its alpha), the second the result of the first.
const char* blurSource = R"(
uniform sampler2D src;
uniform ivec2 size;
uniform ivec2 direction;
uniform bool firstPass;
uniform highp float sigma;
uniform int taps;          // samples on each side
uniform highp float stride; // pixels between the samples
highp float sane(highp float x) { return (x >= 0.) ? min(x, 1e30) : 0.; }
void main()
{
	highp vec2 p = gl_FragCoord.xy;
	highp vec3 sum = vec3(0.);
	highp float weight = 0.;
	for (int k = -taps; k <= taps; ++k)
	{
		highp float d = float(k)*stride;
		highp float w = exp(-0.5*d*d/(sigma*sigma));
		ivec2 texel = clamp(ivec2(p + vec2(direction)*d), ivec2(0), size - ivec2(1));
		highp vec4 t = texelFetch(src, texel, 0);
		highp vec3 c = vec3(sane(t.r), sane(t.g), sane(t.b));
		if (firstPass)
		{
			highp float scene = 1. - clamp(t.a, 0., 1.);
			sum += w*scene*c;
			weight += w*scene;
		}
		else
		{
			sum += w*c;
			weight += w*sane(t.a);
		}
	}
	FRAG_COLOR = vec4(sum, weight);
}
)";

// The scene as the eye resolves it (or the scene itself), the noise of the rods, and the annotations over it
const char* finalSource = R"(
uniform sampler2D scene;
uniform sampler2D blurred;
uniform bool blur;
uniform highp float noiseScale;
uniform highp float noiseShare;
uniform highp float cellSize;
uniform highp float seed;      // integration times since some moment (with the fraction: the noise moves on smoothly)
uniform highp vec3 scotopicWeights;
highp float sane(highp float x) { return (x >= 0.) ? min(x, 1e30) : 0.; }
highp float hash(highp vec2 q)
{
	q = fract(q*vec2(0.1031, 0.1030));
	q += dot(q, q.yx + 33.33);
	return fract((q.x + q.y)*q.x);
}
// A random value of mean 0 and standard deviation 1 in each cell of the pattern k
highp float cellNoise(highp vec2 cell, highp float k)
{
	highp vec2 base = cell + vec2(mod(k, 512.)*17.13, mod(k, 512.)*-9.71);
	highp float s = hash(base) + hash(base + vec2(41.3, 7.9));
	return (s - 1.)*2.4494897; // the sum of 2 uniform numbers has the variance 1/6
}
// The cubic B-spline weights of the 4 cells around a point at the fraction f
highp vec4 bspline(highp float f)
{
	highp float f2 = f*f, f3 = f2*f;
	return vec4((1. - f)*(1. - f)*(1. - f), 3.*f3 - 6.*f2 + 4., -3.*f3 + 3.*f2 + 3.*f + 1., f3)/6.;
}
// The noise between the cells, interpolated with the cubic B-spline (smooth, without the pattern of the cells), its
// variance kept (the weights normalized in squares)
highp float smoothNoise(highp vec2 p, highp float k)
{
	highp vec2 g = p/cellSize - 0.5;
	highp vec2 i = floor(g);
	highp vec2 f = g - i;
	highp vec4 wx = bspline(f.x), wy = bspline(f.y);
	highp float n = 0.;
	for (int y = 0; y < 4; ++y)
		for (int x = 0; x < 4; ++x)
			n += wx[x]*wy[y]*cellNoise(i + vec2(float(x - 1), float(y - 1)), k);
	return n/(sqrt(dot(wx, wx))*sqrt(dot(wy, wy)));
}
// From one pattern to the next over the integration time (the variance kept)
highp float movingNoise(highp vec2 p)
{
	highp float k = floor(seed);
	highp float t = 1.5707963*(seed - k);
	return cos(t)*smoothNoise(p, k) + sin(t)*smoothNoise(p, k + 1.);
}
void main()
{
	ivec2 texel = ivec2(gl_FragCoord.xy);
	highp vec4 original = texelFetch(scene, texel, 0);
	highp float coverage = clamp(original.a, 0., 1.);
	highp vec3 c = vec3(sane(original.r), sane(original.g), sane(original.b));
	if (blur)
	{
		// The detail finer than the acuity of the eye is seen only where it has more contrast than the light
		// around it: the finest detail that is resolved at all needs the full contrast. The blurred scene keeps the
		// coarse light; of the fine detail only what stands out by more than the local light remains (the stars,
		// the bright planets), the rest (faint texture, dark lines) is lost.
		highp vec4 b = texelFetch(blurred, texel, 0);
		if (b.a > 1e-6)
		{
			highp vec3 coarse = b.rgb/b.a;
			c = coarse + max(vec3(0.), c - 2.*coarse);
		}
	}
	if (noiseScale > 0. && noiseShare > 0.)
	{
		// The photoisomerizations in a cell: their relative fluctuation
		highp float n = max(0., dot(c, scotopicWeights))*noiseScale;
		highp float relative = noiseShare/sqrt(max(n, 0.01));
		highp float f = clamp(1. + relative*movingNoise(gl_FragCoord.xy), 0., 4.);
		c *= f;
	}
	// The lines and labels as they were
	highp vec3 annotation = vec3(sane(original.r), sane(original.g), sane(original.b));
	FRAG_COLOR = vec4(mix(c, annotation, coverage), original.a);
}
)";

std::unique_ptr<QOpenGLFramebufferObject> makeFBO(int w, int h)
{
	QOpenGLFramebufferObjectFormat format;
	format.setAttachment(QOpenGLFramebufferObject::NoAttachment);
#if QT_CONFIG(opengles2)
	format.setInternalTextureFormat(GL_RGBA16F);
#else
	format.setInternalTextureFormat(GL_RGBA32F);
#endif
	std::unique_ptr<QOpenGLFramebufferObject> fbo(new QOpenGLFramebufferObject(w, h, format));
	auto& gl = *QOpenGLContext::currentContext()->functions();
	gl.glBindTexture(GL_TEXTURE_2D, fbo->texture());
	gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	gl.glBindTexture(GL_TEXTURE_2D, 0);
	return fbo;
}

std::unique_ptr<QOpenGLShaderProgram> makeProgram(const char* fragmentSource, const char* name)
{
	std::unique_ptr<QOpenGLShaderProgram> prog(new QOpenGLShaderProgram);
	prog->addShaderFromSourceCode(QOpenGLShader::Vertex, StelOpenGL::globalShaderPrefix(StelOpenGL::VERTEX_SHADER) + vertexSource);
	prog->addShaderFromSourceCode(QOpenGLShader::Fragment, StelOpenGL::globalShaderPrefix(StelOpenGL::FRAGMENT_SHADER) + fragmentSource);
	prog->bindAttributeLocation("vertex", 0);
	if (!StelPainter::linkProg(prog.get(), name))
		return nullptr;
	return prog;
}
}

StelEyeVision::StelEyeVision() = default;
StelEyeVision::~StelEyeVision() = default;

double StelEyeVision::acuityLimit(double luminance)
{
	const double l = std::log10(std::max(1e-9, luminance));
	return std::max(0.5, 17.25*std::atan(1.4*l + 0.35) + 25.72);
}

double StelEyeVision::blurSigma(double limit)
{
	// exp(-2 pi^2 sigma^2 R^2) = 1/2
	return std::sqrt(std::log(2.)/2.)/(M_PI*std::max(0.1, limit));
}

void StelEyeVision::init()
{
	initTried = true;
	blurProgram = makeProgram(blurSource, "Naked eye: blur");
	finalProgram = makeProgram(finalSource, "Naked eye: night vision");
	ok = blurProgram && finalProgram;
}

GLuint StelEyeVision::outputTexture() const
{
	return outputFBO ? outputFBO->texture() : 0;
}

bool StelEyeVision::process(GLuint sceneTexture, int w, int h, const Params& params, QOpenGLVertexArrayObject* vao)
{
	if (!initTried)
		init();
	const bool blur = params.sigma >= 0.3f;
	const bool noise = params.noiseScale > 0.f && params.noiseShare > 0.005f;
	if (!ok || !vao || w < 2 || h < 2 || !(blur || noise))
		return false;
	if (w != width || h != height || !outputFBO)
	{
		width = w;
		height = h;
		horizontalFBO = makeFBO(w, h);
		verticalFBO = makeFBO(w, h);
		outputFBO = makeFBO(w, h);
	}
	auto& gl = *QOpenGLContext::currentContext()->functions();
	GLint savedViewport[4];
	gl.glGetIntegerv(GL_VIEWPORT, savedViewport);
	GLint savedFbo = 0;
	gl.glGetIntegerv(GL_FRAMEBUFFER_BINDING, &savedFbo);
	const GLboolean blendWasOn = gl.glIsEnabled(GL_BLEND);
	gl.glDisable(GL_BLEND);
	gl.glViewport(0, 0, w, h);
	vao->bind();

	if (blur)
	{
		// At most 32 samples on each side, spread over 3 sigma
		const float reach = 3.f*params.sigma;
		const int taps = std::clamp(static_cast<int>(std::ceil(reach)), 1, 32);
		const float stride = reach/static_cast<float>(taps);
		blurProgram->bind();
		blurProgram->setUniformValue("src", 0);
		blurProgram->setUniformValue("size", w, h);
		blurProgram->setUniformValue("sigma", params.sigma);
		blurProgram->setUniformValue("taps", taps);
		blurProgram->setUniformValue("stride", stride);
		gl.glActiveTexture(GL_TEXTURE0);
		// horizontal, from the scene
		horizontalFBO->bind();
		gl.glBindTexture(GL_TEXTURE_2D, sceneTexture);
		blurProgram->setUniformValue("direction", 1, 0);
		blurProgram->setUniformValue("firstPass", true);
		gl.glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		// vertical
		verticalFBO->bind();
		gl.glBindTexture(GL_TEXTURE_2D, horizontalFBO->texture());
		blurProgram->setUniformValue("direction", 0, 1);
		blurProgram->setUniformValue("firstPass", false);
		gl.glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		blurProgram->release();
	}

	outputFBO->bind();
	finalProgram->bind();
	gl.glActiveTexture(GL_TEXTURE0);
	gl.glBindTexture(GL_TEXTURE_2D, sceneTexture);
	finalProgram->setUniformValue("scene", 0);
	gl.glActiveTexture(GL_TEXTURE1);
	gl.glBindTexture(GL_TEXTURE_2D, blur ? verticalFBO->texture() : 0);
	finalProgram->setUniformValue("blurred", 1);
	finalProgram->setUniformValue("blur", blur);
	finalProgram->setUniformValue("noiseScale", noise ? params.noiseScale : 0.f);
	finalProgram->setUniformValue("noiseShare", params.noiseShare);
	finalProgram->setUniformValue("cellSize", std::max(1.f, params.cellSize));
	finalProgram->setUniformValue("seed", params.seed);
	finalProgram->setUniformValue("scotopicWeights", params.scotopic[0], params.scotopic[1], params.scotopic[2]);
	gl.glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	finalProgram->release();
	gl.glBindTexture(GL_TEXTURE_2D, 0);
	gl.glActiveTexture(GL_TEXTURE0);
	gl.glBindTexture(GL_TEXTURE_2D, 0);

	vao->release();
	gl.glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(savedFbo));
	gl.glViewport(savedViewport[0], savedViewport[1], savedViewport[2], savedViewport[3]);
	if (blendWasOn)
		gl.glEnable(GL_BLEND);
	return true;
}
