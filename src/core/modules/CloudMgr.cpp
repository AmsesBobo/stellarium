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

#include "CloudMgr.hpp"
#include "CloudLight.hpp"
#include "StelApp.hpp"
#include "StelCore.hpp"
#include "StelLocation.hpp"
#include "StelPainter.hpp"
#include "StelProjector.hpp"
#include "StelTranslator.hpp"
#include "Planet.hpp"
#include "SolarSystem.hpp"
#include "LightDomes.hpp"
#include "AuroraMgr.hpp"
#include "StelModuleMgr.hpp"
#include "StelSkyDrawer.hpp"
#include "StelOpenGL.hpp"
#include "RefractionExtinction.hpp"

#include <QSettings>
#include <QOpenGLBuffer>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QVector3D>
#include <QVector4D>
#include <algorithm>
#include <cmath>

namespace
{
constexpr double EarthRadius = 6371.;     // km
constexpr double CloudHeight = 1.5;       // km
constexpr int VertexAttribIndex = 0;

// The noise of the shader (see cloudHash(), cloudNoise() and cloudTransmission() there), in the same single precision
float fract(float x)
{
	return x - std::floor(x);
}
float cloudHash(float x, float y)
{
	float a = fract(x*0.1031f), b = fract(y*0.1031f), c = a;
	const float d = a*(b + 33.33f) + b*(c + 33.33f) + c*(a + 33.33f);
	a += d;
	b += d;
	c += d;
	return fract((a + b)*c);
}
float cloudNoise(float x, float y)
{
	const float ix = std::floor(x), iy = std::floor(y);
	const float fx = x - ix, fy = y - iy;
	const float u = fx*fx*(3.f - 2.f*fx), v = fy*fy*(3.f - 2.f*fy);
	const float bottom = cloudHash(ix, iy) + u*(cloudHash(ix + 1.f, iy) - cloudHash(ix, iy));
	const float top = cloudHash(ix, iy + 1.f) + u*(cloudHash(ix + 1.f, iy + 1.f) - cloudHash(ix, iy + 1.f));
	return bottom + v*(top - bottom);
}
}

CloudMgr::CloudMgr()
{
	setObjectName("CloudMgr");
}

CloudMgr::~CloudMgr()
{
}

void CloudMgr::init()
{
	QSettings* conf = StelApp::getInstance().getSettings();
	setFlagShow(conf->value("clouds/flag_show", false).toBool());
	addAction("actionShow_Clouds", N_("Display Options"), N_("Clouds"), "flagShow");

	// A full screen quad
	auto& gl = *QOpenGLContext::currentContext()->functions();
	vbo.reset(new QOpenGLBuffer(QOpenGLBuffer::VertexBuffer));
	vbo->create();
	vbo->bind();
	const GLfloat vertices[] = {-1, -1,  1, -1,  -1, 1,  1, 1};
	gl.glBufferData(GL_ARRAY_BUFFER, sizeof vertices, vertices, GL_STATIC_DRAW);
	vbo->release();
	vao.reset(new QOpenGLVertexArrayObject);
	vao->create();
	if (vao->isCreated())
	{
		vao->bind();
		vbo->bind();
		gl.glVertexAttribPointer(VertexAttribIndex, 2, GL_FLOAT, false, 0, nullptr);
		gl.glEnableVertexAttribArray(VertexAttribIndex);
		vbo->release();
		vao->release();
	}
}

void CloudMgr::setFlagShow(bool b)
{
	if (b != flagShow)
	{
		flagShow = b;
		StelApp::immediateSave("clouds/flag_show", b);
		emit flagShowChanged(b);
	}
}

void CloudMgr::buildProgram(const StelProjector& projector)
{
	program.reset();
	const Extinction& extinction = StelApp::getInstance().getCore()->getSkyDrawer()->getExtinction();
	std::unique_ptr<QOpenGLShaderProgram> newProgram(new QOpenGLShaderProgram);

	const QByteArray vert = StelOpenGL::globalShaderPrefix(StelOpenGL::VERTEX_SHADER) + R"(
ATTRIBUTE highp vec3 vertex;
VARYING highp vec3 ndcPos;
void main()
{
	gl_Position = vec4(vertex, 1.);
	ndcPos = vertex;
}
)";
	if (!newProgram->addShaderFromSourceCode(QOpenGLShader::Vertex, vert))
	{
		qWarning().noquote() << "CloudMgr: error in the vertex shader:\n" << newProgram->log();
		return;
	}

	// Everything is in the AltAz frame of the observer with its origin in the center of the Earth (km): the observer
	// stands on its z axis.
	const QByteArray frag = StelOpenGL::globalShaderPrefix(StelOpenGL::FRAGMENT_SHADER)
	                        + projector.getUnProjectShader()
	                        + extinction.getForwardTransformShader()
	                        + R"(
VARYING highp vec3 ndcPos;
uniform highp mat4 projectionMatrixInverse;
uniform highp vec3 cloudSouth;      // the axes of the frame in the Earth-fixed one
uniform highp vec3 cloudEast;
uniform highp vec3 cloudUp;
uniform highp vec4 cloudRadii;      // of the observer and the layer (km); the height of the layer above the observer
                                    // (km); the size of a pixel (radians)
uniform highp vec3 cloudSun;        // unit vectors towards the Sun and the Moon
uniform highp vec3 cloudMoon;
uniform highp vec3 cloudLight;      // the light of the clouds and the air (see CloudLight), for the scene
uniform highp vec3 airSky;
uniform highp vec3 airSun;
uniform highp vec3 airMoon;
uniform highp vec3 airExtinction;   // per km

// Value noise on a map of the layer in km: hash (Dave Hoskins, "Hash without Sine")
highp float cloudHash(highp vec2 p)
{
	highp vec3 p3 = fract(vec3(p.xyx)*0.1031);
	p3 += dot(p3, p3.yzx + 33.33);
	return fract((p3.x + p3.y)*p3.z);
}
highp float cloudNoise(highp vec2 p)
{
	highp vec2 i = floor(p), f = p - i;
	highp float a = cloudHash(i), b = cloudHash(i + vec2(1, 0)), c = cloudHash(i + vec2(0, 1)), d = cloudHash(i + vec2(1, 1));
	f = f*f*(3. - 2.*f);
	return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

// The test shapes: patches of about 1.5 km, opaque with sharp edges, which blend into a mean where a pixel covers
// more than a patch (foot: its size on the layer, km)
highp float cloudTransmission(highp vec2 map, highp float foot)
{
	highp float n = 0.7*cloudNoise(map/1.6) + 0.3*cloudNoise(map/0.6 + 37.);
	highp float edge = 0.015 + 0.3*foot/1.6;
	return 1. - smoothstep(0.56 - edge, 0.56 + edge, n);
}

void main(void)
{
	highp vec4 winPos = projectionMatrixInverse * vec4(ndcPos, 1);
	bool ok = false;
	highp vec3 altAz = unProject(winPos.x, winPos.y, ok).xyz;
	if (!ok)
	{
		FRAG_COLOR = vec4(0., 0., 0., 1.);
		return;
	}
	altAz = normalize(altAz);
	// Below the horizon the line of sight meets the Earth. Near it the refraction carries it on along the Earth: the
	// layer ends there, at about 140 km
	if (altAz.z < -0.015)
	{
		FRAG_COLOR = vec4(0., 0., 0., 1.);
		return;
	}
	highp float r0 = cloudRadii.x;
	highp float b = r0*max(altAz.z, 0.);
	// where the line of sight, from inside the shell, leaves it (in this form to keep the precision over short ways)
	highp float c = cloudRadii.z*(cloudRadii.x + cloudRadii.y);
	highp float t = c/(b + sqrt(b*b + c));
	highp vec3 p = vec3(0., 0., r0) + t*vec3(altAz.xy, max(altAz.z, 0.));
	// a slanted line of sight sees more of the layer in a pixel
	highp float slant = length(p)/dot(p, normalize(p - vec3(0., 0., r0)));

	// the place on the map: a polar stereographic projection, in km, of each hemisphere
	highp vec3 earth = p.x*cloudSouth + p.y*cloudEast + p.z*cloudUp;
	highp vec3 unit = earth/length(earth);
	highp vec2 map = 2.*cloudRadii.y*unit.xy/(1. + abs(unit.z));
	highp float T = cloudTransmission(map, t*cloudRadii.w*slant);

	// The way through the air dims the cloud and adds the light of the air (Rayleigh phase function)
	highp vec3 way = exp(-airExtinction*t);
	highp float sunCos = dot(altAz, cloudSun), moonCos = dot(altAz, cloudMoon);
	highp vec3 air = (1. - way)*(airSky + airSun*0.75*(1. + sunCos*sunCos) + airMoon*0.75*(1. + moonCos*moonCos));
	// out = source + scene x alpha
	FRAG_COLOR = vec4((cloudLight*way + air)*(1. - T), T);
}
)";
	if (!newProgram->addShaderFromSourceCode(QOpenGLShader::Fragment, frag))
	{
		qWarning().noquote() << "CloudMgr: error in the fragment shader:\n" << newProgram->log();
		return;
	}
	newProgram->bindAttributeLocation("vertex", VertexAttribIndex);
	if (!StelPainter::linkProg(newProgram.get(), "CloudMgr program"))
		return;
	program = std::move(newProgram);
}

bool CloudMgr::isShown(const StelCore* core) const
{
	if (!flagShow)
		return false;
	const StelSkyDrawer* drawer = core->getSkyDrawer();
	// (the blending needs the scene in floating point, which only the photographic mode has; the observer, over a
	// spherical Earth, is below the layer)
	return drawer->getFlagPhotographicModeActive() && drawer->getFlagHasAtmosphere()
	       && core->getCurrentPlanet()->getEnglishName() == QStringLiteral("Earth")
	       && core->getCurrentLocation().altitude*0.001 < CloudHeight;
}

double CloudMgr::getTransmission(const StelCore* core, Vec3d altAz) const
{
	if (!isShown(core))
		return 1.;
	altAz.normalize();
	if (altAz[2] < -0.015)
		return 1.;
	const StelLocation& location = core->getCurrentLocation();
	const double r0 = EarthRadius + std::max(0., location.altitude*0.001);
	const double rc = EarthRadius + CloudHeight;
	const double b = r0*std::max(altAz[2], 0.);
	const double c = (rc - r0)*(rc + r0);
	const double t = c/(b + std::sqrt(b*b + c));
	const Vec3d p(t*altAz[0], t*altAz[1], r0 + t*std::max(altAz[2], 0.));
	const double phi = static_cast<double>(location.getLatitude())*M_PI/180.;
	const double lambda = static_cast<double>(location.getLongitude())*M_PI/180.;
	const Vec3d up(std::cos(phi)*std::cos(lambda), std::cos(phi)*std::sin(lambda), std::sin(phi));
	const Vec3d east(-std::sin(lambda), std::cos(lambda), 0.);
	const Vec3d south(std::sin(phi)*std::cos(lambda), std::sin(phi)*std::sin(lambda), -std::cos(phi));
	Vec3d unit = south*p[0] + east*p[1] + up*p[2];
	unit.normalize();
	const double scale = 2.*rc/(1. + std::fabs(unit[2]));
	const float x = static_cast<float>(scale*unit[0]), y = static_cast<float>(scale*unit[1]);
	const float n = 0.7f*cloudNoise(x/1.6f, y/1.6f) + 0.3f*cloudNoise(x/0.6f + 37.f, y/0.6f + 37.f);
	const double e = std::clamp((static_cast<double>(n) - 0.545)/0.03, 0., 1.);
	return 1. - e*e*(3. - 2.*e);
}

void CloudMgr::setLightUniforms(const StelCore* core, QOpenGLShaderProgram& p) const
{
	const StelSkyDrawer* drawer = core->getSkyDrawer();
	const SolarSystem* ssystem = GETSTELMODULE(SolarSystem);
	CloudLight::Sources s;

	// The Sun and the Moon: lux on a surface facing them, from their magnitude with the extinction, and its color
	const auto body = [core, drawer](const PlanetP& planet, CloudLight::Rgb& light, double& sine) {
		Vec3d direction = planet->getAltAzPosGeometric(core);
		direction.normalize();
		sine = direction[2];
		const double lux = 2.54e-6*std::pow(10., -0.4*static_cast<double>(planet->getVMagnitudeWithExtinction(core)));
		const Vec3f tint = drawer->getPhotoExtinctionTint(direction);
		for (std::size_t i = 0; i < 3; ++i)
			light[i] = lux*static_cast<double>(tint[static_cast<int>(i)]);
		return direction;
	};
	const Vec3d sun = body(ssystem->getSun(), s.sun, s.sunSine);
	s.sunAltitude = std::asin(std::clamp(s.sunSine, -1., 1.));
	const Vec3d moon = body(ssystem->getMoon(), s.moon, s.moonSine);

	s.artificial = drawer->getLightPollutionLuminance();
	s.extinction = drawer->getEffectiveExtinctionCoefficient();
	const Vec3f lamp = LightDomes::lampColor(LightDomes::LampMixed);
	s.lamp = {static_cast<double>(lamp[0]), static_cast<double>(lamp[1]), static_cast<double>(lamp[2])};
	// the light domes of towns and the aurorae, on the ground below the clouds
	static const LightDomes* lightDomes = GETSTELMODULE(LightDomes);
	if (lightDomes)
	{
		const Vec3f E = lightDomes->getIlluminance();
		for (std::size_t i = 0; i < 3; ++i)
			s.groundExtra[i] += static_cast<double>(E[static_cast<int>(i)]);
		s.artificial += lightDomes->getZenithLuminance();
	}
	static const AuroraMgr* aurorae = GETSTELMODULE(AuroraMgr);
	if (aurorae)
	{
		const Vec3f E = aurorae->getIlluminance();
		for (std::size_t i = 0; i < 3; ++i)
			s.groundExtra[i] += static_cast<double>(E[static_cast<int>(i)]);
	}

	const CloudLight::Light light = CloudLight::compute(s);
	const double scale = static_cast<double>(drawer->getPhotoSceneScale());
	const auto scene = [scale](const CloudLight::Rgb& v) {
		return QVector3D(static_cast<float>(v[0]*scale), static_cast<float>(v[1]*scale), static_cast<float>(v[2]*scale));
	};
	const auto direction = [](const Vec3d& v) {return QVector3D(static_cast<float>(v[0]), static_cast<float>(v[1]), static_cast<float>(v[2]));};
	p.setUniformValue("cloudSun", direction(sun));
	p.setUniformValue("cloudMoon", direction(moon));
	p.setUniformValue("cloudLight", scene(light.cloud));
	p.setUniformValue("airSky", scene(light.airSky));
	p.setUniformValue("airSun", scene(light.airSun));
	p.setUniformValue("airMoon", scene(light.airMoon));
	p.setUniformValue("airExtinction", QVector3D(static_cast<float>(light.extinction[0]), static_cast<float>(light.extinction[1]), static_cast<float>(light.extinction[2])));
}

void CloudMgr::drawOverSky(StelCore* core)
{
	if (!isShown(core))
		return;
	const StelSkyDrawer* drawer = core->getSkyDrawer();
	const StelLocation& location = core->getCurrentLocation();
	const double observerHeight = std::max(0., location.altitude*0.001);

	const StelProjectorP prj = core->getProjection(StelCore::FrameAltAz, StelCore::RefractionAuto);
	if (!programProjector || !prj->isSameProjection(*programProjector))
	{
		buildProgram(*prj);
		programProjector = prj;
	}
	if (!program)
		return;

	QOpenGLShaderProgram& p = *program;
	p.bind();
	p.setUniformValue("projectionMatrixInverse", prj->getProjectionMatrix().toQMatrix().inverted());
	prj->setUnProjectUniforms(p);
	drawer->getExtinction().setForwardTransformUniforms(p);
	setLightUniforms(core, p);

	// The frame of the observer in the Earth-fixed one, over a spherical Earth
	const double phi = static_cast<double>(location.getLatitude())*M_PI/180.;
	const double lambda = static_cast<double>(location.getLongitude())*M_PI/180.;
	const Vec3d up(std::cos(phi)*std::cos(lambda), std::cos(phi)*std::sin(lambda), std::sin(phi));
	const Vec3d east(-std::sin(lambda), std::cos(lambda), 0.);
	const Vec3d south(std::sin(phi)*std::cos(lambda), std::sin(phi)*std::sin(lambda), -std::cos(phi));
	const auto vec = [](const Vec3d& v) {return QVector3D(static_cast<float>(v[0]), static_cast<float>(v[1]), static_cast<float>(v[2]));};
	p.setUniformValue("cloudSouth", vec(south));
	p.setUniformValue("cloudEast", vec(east));
	p.setUniformValue("cloudUp", vec(up));
	p.setUniformValue("cloudRadii", QVector4D(static_cast<float>(EarthRadius + observerHeight), static_cast<float>(EarthRadius + CloudHeight),
	                                          static_cast<float>(CloudHeight - observerHeight),
	                                          static_cast<float>(1./static_cast<double>(prj->getPixelPerRadAtCenter()))));

	// out = source + scene x alpha; the alpha of the scene stays as it is. The state of the blending goes back to what it
	// was: StelPainter remembers it and would not set it again
	auto& gl = *QOpenGLContext::currentContext()->functions();
	const GLboolean blendWasOn = gl.glIsEnabled(GL_BLEND);
	GLint blend[4];
	gl.glGetIntegerv(GL_BLEND_SRC_RGB, &blend[0]);
	gl.glGetIntegerv(GL_BLEND_DST_RGB, &blend[1]);
	gl.glGetIntegerv(GL_BLEND_SRC_ALPHA, &blend[2]);
	gl.glGetIntegerv(GL_BLEND_DST_ALPHA, &blend[3]);
	gl.glEnable(GL_BLEND);
	gl.glBlendFuncSeparate(GL_ONE, GL_SRC_ALPHA, GL_ZERO, GL_ONE);
	drawQuad();
	gl.glBlendFuncSeparate(static_cast<GLenum>(blend[0]), static_cast<GLenum>(blend[1]), static_cast<GLenum>(blend[2]), static_cast<GLenum>(blend[3]));
	if (!blendWasOn)
		gl.glDisable(GL_BLEND);
	p.release();
}

void CloudMgr::drawQuad()
{
	auto& gl = *QOpenGLContext::currentContext()->functions();
	if (vao->isCreated())
		vao->bind();
	else
	{
		vbo->bind();
		gl.glVertexAttribPointer(VertexAttribIndex, 2, GL_FLOAT, false, 0, nullptr);
		gl.glEnableVertexAttribArray(VertexAttribIndex);
		vbo->release();
	}
	gl.glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	if (vao->isCreated())
		vao->release();
	else
		gl.glDisableVertexAttribArray(VertexAttribIndex);
}
