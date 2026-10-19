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

#include "StelPhotoBloom.hpp"
#include "StelOpenGL.hpp"
#include "StelPainter.hpp"

#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLFramebufferObject>
#include <QOpenGLVertexArrayObject>
#include <QDebug>
#include <QVector2D>
#include <cmath>

namespace
{
constexpr int MAX_LEVELS = 10;

const char* vertexSource = R"(
ATTRIBUTE vec3 vertex;
VARYING vec2 texcoord;
void main()
{
	gl_Position = vec4(vertex, 1.);
	texcoord = 0.5*vertex.xy+0.5;
}
)";

// Exact 2x2 box downsampling (each level is half the size of the previous one, rounded up; the
// missing texels at an odd edge repeat the last one). Guards against infinities and NaN from
// overflowing sources, which would spread over the whole halo.
const char* downSource = R"(
uniform sampler2D src;
uniform ivec2 srcSize;
highp float sane(highp float x) { return (x >= 0.) ? min(x, 1e30) : 0.; }
void main()
{
	ivec2 base = 2*ivec2(gl_FragCoord.xy);
	highp vec3 c = vec3(0);
	for (int dy = 0; dy < 2; ++dy)
		for (int dx = 0; dx < 2; ++dx)
		{
			highp vec3 t = texelFetch(src, min(base + ivec2(dx, dy), srcSize - ivec2(1)), 0).rgb;
			c += vec3(sane(t.r), sane(t.g), sane(t.b));
		}
	FRAG_COLOR = vec4(0.25*c, 1.);
}
)";

// Separable Gaussian blur with 9 taps
const char* blurSource = R"(
VARYING vec2 texcoord;
uniform sampler2D src;
uniform vec2 stepUV;
uniform highp float weights[5];
void main()
{
	highp vec3 sum = weights[0] * texture2D(src, texcoord).rgb;
	for (int i = 1; i < 5; ++i)
	{
		highp vec2 d = float(i) * stepUV;
		sum += weights[i] * (texture2D(src, texcoord + d).rgb + texture2D(src, texcoord - d).rgb);
	}
	FRAG_COLOR = vec4(sum, 1.);
}
)";

// Accumulation from the coarsest level up: weight x this level + the (bilinearly upsampled) sum of the coarser ones.
// A texel of the coarser level covers exactly 2x2 texels of this one (its size is rounded up).
const char* combineSource = R"(
VARYING vec2 texcoord;
uniform sampler2D blurTex;
uniform sampler2D coarserTex;
uniform vec2 coarserScale; // this level's texture coordinates -> the coarser level's
uniform bool hasCoarser;
uniform highp float weight;
void main()
{
	highp vec3 c = weight * texture2D(blurTex, texcoord).rgb;
	if (hasCoarser)
		c += texture2D(coarserTex, texcoord * coarserScale).rgb;
	FRAG_COLOR = vec4(c, 1.);
}
)";

// Diffraction spikes: per line direction, taps on both sides along the line. Level L of the pyramid
// (texels of 2^L pixels) serves the taps from 8(2^L-1) to 8(2^{L+1}-1) pixels away, 8 taps per level.
constexpr int SPIKE_LEVELS = 7;
constexpr int SPIKE_TAPS = 8;
constexpr int NEAR_SPIKE_LEVELS = 2; // levels 0 and 1: up to 24.5 pixels, see StelPhotoBloom::NEAR_SPIKE_LENGTH
QByteArray makeSpikeSource()
{
	QByteArray src = R"(
VARYING vec2 texcoord;
uniform sampler2D level0;
uniform sampler2D level1;
uniform sampler2D level2;
uniform sampler2D level3;
uniform sampler2D level4;
uniform sampler2D level5;
uniform sampler2D level6;
uniform int numLevels;
uniform int numLines;
uniform vec2 lineSteps[4]; // one pixel along each line, in texture coordinates
uniform vec2 levelScale[7]; // scene texture coordinates -> level texture coordinates
uniform highp float offsets[56];
uniform highp float weights[56];
void main()
{
	highp vec3 sum = vec3(0);
	for (int d = 0; d < 4; ++d)
	{
		if (d >= numLines) break;
		highp vec2 st = lineSteps[d];
)";
	for (int L=0; L<SPIKE_LEVELS; ++L)
	{
		const QByteArray l = QByteArray::number(L);
		src += "\t\tif (numLevels > " + l + ")\n\t\t{\n";
		src += "\t\t\tfor (int i = 0; i < 8; ++i)\n\t\t\t{\n";
		src += "\t\t\t\tint j = " + QByteArray::number(L*SPIKE_TAPS) + " + i;\n";
		src += "\t\t\t\tsum += weights[j] * (texture2D(level" + l + ", (texcoord + offsets[j]*st)*levelScale[" + l + "]).rgb"
		       " + texture2D(level" + l + ", (texcoord - offsets[j]*st)*levelScale[" + l + "]).rgb);\n";
		src += "\t\t\t}\n\t\t}\n";
	}
	src += "\t}\n\tFRAG_COLOR = vec4(sum, 1.);\n}\n";
	return src;
}

void setTextureParams(QOpenGLFunctions& gl, GLuint tex, GLint filter)
{
	gl.glBindTexture(GL_TEXTURE_2D, tex);
	gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
	gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
	gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

std::unique_ptr<QOpenGLFramebufferObject> makeFBO(QOpenGLFunctions& gl, int w, int h)
{
	QOpenGLFramebufferObjectFormat format;
	format.setAttachment(QOpenGLFramebufferObject::NoAttachment);
#if QT_CONFIG(opengles2)
	format.setInternalTextureFormat(GL_RGBA16F);
#else
	format.setInternalTextureFormat(GL_RGBA32F);
#endif
	std::unique_ptr<QOpenGLFramebufferObject> fbo(new QOpenGLFramebufferObject(w, h, format));
	setTextureParams(gl, fbo->texture(), GL_LINEAR);
	return fbo;
}

std::unique_ptr<QOpenGLShaderProgram> makeProgram(const QByteArray& fragmentSource, const char* name)
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

StelPhotoBloom::StelPhotoBloom() = default;
StelPhotoBloom::~StelPhotoBloom() = default;

void StelPhotoBloom::initShaders()
{
	downProgram = makeProgram(downSource, "photographic bloom: downsampling");
	blurProgram = makeProgram(blurSource, "photographic bloom: blur");
	combineProgram = makeProgram(combineSource, "photographic bloom: combination");
	spikeProgram = makeProgram(makeSpikeSource(), "photographic bloom: diffraction spikes");
	shadersOk = downProgram && blurProgram && combineProgram && spikeProgram;
	if (!shadersOk)
		qWarning() << "Photographic mode: the bloom shaders could not be built, bloom is off";
}

void StelPhotoBloom::resize(int w, int h)
{
	auto& gl = *QOpenGLContext::currentContext()->functions();
	levels.clear();
	sceneW = w;
	sceneH = h;
	// Levels down to a few pixels: the widest Gaussian is then a good fraction of the screen
	for (int k=0; k<MAX_LEVELS; ++k)
	{
		// Half the previous level, rounded up: a texel of level k covers exactly 2^k x 2^k scene pixels
		const int lw = (k == 0) ? w : (levels.back().w + 1)/2;
		const int lh = (k == 0) ? h : (levels.back().h + 1)/2;
		if (k > 0 && std::min(lw, lh) < 4)
			break;
		Level level;
		level.w = lw;
		level.h = lh;
		if (k > 0)
			level.down = makeFBO(gl, lw, lh);
		level.temp = makeFBO(gl, lw, lh);
		level.blur = makeFBO(gl, lw, lh);
		levels.push_back(std::move(level));
	}
	spikeFBO = makeFBO(gl, w, h);
	weightsAlpha = -1.f; // recompute the weights for the new number of levels
}

// Width (sigma, scene pixels) of the effective Gaussian of level k: the 2x2 box downsamplings
// (variance 4^(j-1)/4 each), the blur at that level, and the bilinear upsamplings back to the
// scene resolution (a tent of half width 2^j adds the variance 4^j/6).
double StelPhotoBloom::levelSigma(int k)
{
	double var = static_cast<double>(BLUR_SIGMA)*BLUR_SIGMA*std::pow(4., k);
	for (int j=1; j<=k; ++j)
		var += std::pow(4., j-1)/4. + std::pow(4., j)/6.;
	return std::sqrt(var);
}

// A Moffat profile normalized to unit integral is a mixture of Gaussians:
//   M(r) = integral f(t) G_t(r) dt,  f(t) = (beta-1)/Gamma(beta) t^(beta-2) e^-t,
// where G_t is the normalized Gaussian with sigma^2 = alpha^2/(2t).
// Each Gaussian of the mixture goes to the level with the nearest width (in log scale);
// those narrower than half the finest level do not blur noticeably and stay unblurred.
void StelPhotoBloom::computeWeights(float alpha, float beta)
{
	const int n = static_cast<int>(levels.size());
	weights.assign(static_cast<size_t>(n), 0.f);
	weightIdentity = 0.f;
	std::vector<double> lnSigma(static_cast<size_t>(n));
	for (int k=0; k<n; ++k)
		lnSigma[static_cast<size_t>(k)] = std::log(levelSigma(k));
	const double identityLimit = 0.5*levelSigma(0);
	const double b = static_cast<double>(beta);
	const double norm = (b-1.)/std::tgamma(b);
	std::vector<double> w(static_cast<size_t>(n), 0.);
	double wId = 0., total = 0.;
	constexpr double du = 0.002;
	for (double u=-40.; u<15.; u+=du)
	{
		const double t = std::exp(u);
		const double mass = norm*std::pow(t, b-1.)*std::exp(-t)*du; // f(t) dt with dt = t du
		total += mass;
		const double sigma = static_cast<double>(alpha)/std::sqrt(2.*t);
		if (sigma < identityLimit)
		{
			wId += mass;
			continue;
		}
		const double ls = std::log(sigma);
		int best = 0;
		for (int k=1; k<n; ++k)
			if (std::fabs(ls - lnSigma[static_cast<size_t>(k)]) < std::fabs(ls - lnSigma[static_cast<size_t>(best)]))
				best = k;
		w[static_cast<size_t>(best)] += mass;
	}
	if (total <= 0.)
		total = 1.;
	for (int k=0; k<n; ++k)
		weights[static_cast<size_t>(k)] = static_cast<float>(w[static_cast<size_t>(k)]/total);
	weightIdentity = static_cast<float>(wId/total);
	weightsAlpha = alpha;
	weightsBeta = beta;
	weightsLevels = n;
}

bool StelPhotoBloom::process(GLuint sceneTexture, int w, int h, float haloAlpha, float haloBeta,
                             int spikeLines, float spikeAngle, float spikeLength, QOpenGLVertexArrayObject* vao)
{
	spikesDone = false;
	if (!shadersTried)
	{
		shadersTried = true;
		initShaders();
	}
	if (!shadersOk || !vao || w < 8 || h < 8 || !(haloAlpha > 0.f) || !(haloBeta > 1.f))
		return false;
	if (w != sceneW || h != sceneH || levels.empty())
		resize(w, h);
	const int n = static_cast<int>(levels.size());
	if (std::fabs(haloAlpha-weightsAlpha) > 1e-3f*haloAlpha || haloBeta != weightsBeta || n != weightsLevels)
		computeWeights(haloAlpha, haloBeta);

	auto& gl = *QOpenGLContext::currentContext()->functions();
	GLint savedViewport[4];
	gl.glGetIntegerv(GL_VIEWPORT, savedViewport);
	GLint savedFbo = 0;
	gl.glGetIntegerv(GL_FRAMEBUFFER_BINDING, &savedFbo);
	const GLboolean blendWasOn = gl.glIsEnabled(GL_BLEND);
	gl.glDisable(GL_BLEND);
	gl.glActiveTexture(GL_TEXTURE0);
	// The scene is read with bilinear filtering for the first downsampling
	setTextureParams(gl, sceneTexture, GL_LINEAR);

	vao->bind();
	auto drawQuad = [&gl](QOpenGLFramebufferObject* target, int tw, int th)
	{
		target->bind();
		gl.glViewport(0, 0, tw, th);
		gl.glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	};
	auto sourceTexture = [this, sceneTexture](int k) -> GLuint
	{
		return k==0 ? sceneTexture : levels[static_cast<size_t>(k)].down->texture();
	};

	// 1. Downsampling chain
	downProgram->bind();
	downProgram->setUniformValue("src", 0);
	for (int k=1; k<n; ++k)
	{
		gl.glBindTexture(GL_TEXTURE_2D, sourceTexture(k-1));
		// an ivec2: QOpenGLShaderProgram has no integer vector setter
		gl.glUniform2i(downProgram->uniformLocation("srcSize"), levels[static_cast<size_t>(k-1)].w, levels[static_cast<size_t>(k-1)].h);
		drawQuad(levels[static_cast<size_t>(k)].down.get(), levels[static_cast<size_t>(k)].w, levels[static_cast<size_t>(k)].h);
	}

	// 2. Diffraction spikes, from the full image and the downsampled levels (before step 4 reuses them)
	if (spikeLines > 0 && spikeLength > 0.f)
	{
		// Only the inner part of the spikes (up to NEAR_SPIKE_LENGTH pixels): farther out, the downsampled
		// levels would widen the spikes. The far part is drawn with each star (StelSkyDrawer).
		const int numLevels = std::min(n, NEAR_SPIKE_LEVELS);
		GLfloat offsets[SPIKE_LEVELS*SPIKE_TAPS] = {};
		GLfloat spikeWeights[SPIKE_LEVELS*SPIKE_TAPS] = {};
		// Normalized Lorentzian along a line: K(x) = 1/(pi l (1 + x^2/l^2)); the integral of one side
		// over [a, b] is (atan(b/l) - atan(a/l))/pi. The central pixel is the star itself.
		const double l = static_cast<double>(spikeLength);
		for (int L=0; L<numLevels; ++L)
		{
			const double spacing = std::pow(2., L);
			const double start = 8.*(spacing-1.) + 0.5;
			for (int j=0; j<SPIKE_TAPS; ++j)
			{
				const double a = start + spacing*j;
				const double b = a + spacing;
				const double wgt = (std::atan(b/l) - std::atan(a/l))/M_PI;
				offsets[L*SPIKE_TAPS+j] = static_cast<GLfloat>(0.5*(a+b));
				spikeWeights[L*SPIKE_TAPS+j] = static_cast<GLfloat>(wgt);
			}
		}
		// Each line gets 1/spikeLines of the spike light
		for (int i=0; i<numLevels*SPIKE_TAPS; ++i)
			spikeWeights[i] = static_cast<GLfloat>(static_cast<double>(spikeWeights[i])/spikeLines);
		QVector2D steps[4];
		for (int k=0; k<4; ++k)
		{
			const float a = spikeAngle + static_cast<float>(k)*static_cast<float>(M_PI)/static_cast<float>(spikeLines);
			steps[k] = QVector2D(std::cos(a)/static_cast<float>(w), std::sin(a)/static_cast<float>(h));
		}
		spikeProgram->bind();
		for (int L=0; L<SPIKE_LEVELS; ++L)
		{
			gl.glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(L));
			gl.glBindTexture(GL_TEXTURE_2D, L < numLevels ? sourceTexture(L) : 0);
			spikeProgram->setUniformValue(QByteArray("level"+QByteArray::number(L)).constData(), L);
		}
		spikeProgram->setUniformValue("numLevels", numLevels);
		spikeProgram->setUniformValue("numLines", std::min(spikeLines, 4));
		spikeProgram->setUniformValueArray("lineSteps", steps, 4);
		QVector2D levelScales[SPIKE_LEVELS];
		for (int L=0; L<SPIKE_LEVELS; ++L)
		{
			if (L < numLevels)
			{
				const Level& lv = levels[static_cast<size_t>(L)];
				const float f = static_cast<float>(1 << L);
				levelScales[L] = QVector2D(static_cast<float>(w)/(f*static_cast<float>(lv.w)), static_cast<float>(h)/(f*static_cast<float>(lv.h)));
			}
			else
				levelScales[L] = QVector2D(1.f, 1.f);
		}
		spikeProgram->setUniformValueArray("levelScale", levelScales, SPIKE_LEVELS);
		spikeProgram->setUniformValueArray("offsets", offsets, SPIKE_LEVELS*SPIKE_TAPS, 1);
		spikeProgram->setUniformValueArray("weights", spikeWeights, SPIKE_LEVELS*SPIKE_TAPS, 1);
		drawQuad(spikeFBO.get(), w, h);
		for (int L=SPIKE_LEVELS-1; L>=0; --L)
		{
			gl.glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(L));
			gl.glBindTexture(GL_TEXTURE_2D, 0);
		}
		spikesDone = true;
	}

	// 3. Gaussian blur of every level
	GLfloat taps[5];
	{
		double sum = 0.;
		double t[5];
		for (int i=0; i<5; ++i)
		{
			t[i] = std::exp(-0.5*i*i/(static_cast<double>(BLUR_SIGMA)*BLUR_SIGMA));
			sum += (i==0 ? 1. : 2.)*t[i];
		}
		for (int i=0; i<5; ++i)
			taps[i] = static_cast<GLfloat>(t[i]/sum);
	}
	blurProgram->bind();
	blurProgram->setUniformValue("src", 0);
	blurProgram->setUniformValueArray("weights", taps, 5, 1);
	for (int k=0; k<n; ++k)
	{
		Level& level = levels[static_cast<size_t>(k)];
		gl.glBindTexture(GL_TEXTURE_2D, sourceTexture(k));
		blurProgram->setUniformValue("stepUV", 1.f/static_cast<float>(level.w), 0.f);
		drawQuad(level.temp.get(), level.w, level.h);
		gl.glBindTexture(GL_TEXTURE_2D, level.temp->texture());
		blurProgram->setUniformValue("stepUV", 0.f, 1.f/static_cast<float>(level.h));
		drawQuad(level.blur.get(), level.w, level.h);
	}

	// 4. Weighted sum, from the coarsest level up. The sum of levels >= k goes to level k's
	//    "down" buffer (no longer needed); the final sum at full resolution to level 0's "temp".
	combineProgram->bind();
	combineProgram->setUniformValue("blurTex", 0);
	combineProgram->setUniformValue("coarserTex", 1);
	for (int k=n-1; k>=0; --k)
	{
		Level& level = levels[static_cast<size_t>(k)];
		const bool hasCoarser = (k < n-1);
		gl.glActiveTexture(GL_TEXTURE1);
		gl.glBindTexture(GL_TEXTURE_2D, hasCoarser ? levels[static_cast<size_t>(k+1)].down->texture() : 0);
		gl.glActiveTexture(GL_TEXTURE0);
		gl.glBindTexture(GL_TEXTURE_2D, level.blur->texture());
		combineProgram->setUniformValue("hasCoarser", hasCoarser);
		if (hasCoarser)
		{
			const Level& coarser = levels[static_cast<size_t>(k+1)];
			combineProgram->setUniformValue("coarserScale", static_cast<float>(level.w)/(2.f*static_cast<float>(coarser.w)),
			                                                  static_cast<float>(level.h)/(2.f*static_cast<float>(coarser.h)));
		}
		combineProgram->setUniformValue("weight", weights[static_cast<size_t>(k)]);
		drawQuad(k==0 ? level.temp.get() : level.down.get(), level.w, level.h);
	}
	combineProgram->release();
	vao->release();

	// Restore the state for the final camera pass
	gl.glActiveTexture(GL_TEXTURE1);
	gl.glBindTexture(GL_TEXTURE_2D, 0);
	gl.glActiveTexture(GL_TEXTURE0);
	setTextureParams(gl, sceneTexture, GL_NEAREST);
	gl.glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(savedFbo));
	gl.glViewport(savedViewport[0], savedViewport[1], savedViewport[2], savedViewport[3]);
	if (blendWasOn)
		gl.glEnable(GL_BLEND);
	return true;
}

GLuint StelPhotoBloom::spikeTexture() const
{
	return (spikesDone && spikeFBO) ? spikeFBO->texture() : 0;
}

GLuint StelPhotoBloom::haloTexture() const
{
	return levels.empty() ? 0 : levels.front().temp->texture();
}
