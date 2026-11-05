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

#include "NoctilucentCloudMgr.hpp"
#include "NoctilucentCloudLight.hpp"
#include "StelApp.hpp"
#include "StelCore.hpp"
#include "StelLocation.hpp"
#include "StelModuleMgr.hpp"
#include "StelPainter.hpp"
#include "StelProjector.hpp"
#include "StelTranslator.hpp"
#include "SolarSystem.hpp"
#include "Planet.hpp"
#include "StelSkyDrawer.hpp"
#include "StelOpenGL.hpp"
#include "RefractionExtinction.hpp"

#include <QSettings>
#include <QOpenGLBuffer>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QOpenGLFramebufferObject>
#include <QVector2D>
#include <QVector3D>
#include <QVector4D>
#include <algorithm>
#include <cmath>
#include <vector>

#ifndef GL_RGB32F
#define GL_RGB32F 0x8815
#endif
#ifndef GL_RGBA16F
#define GL_RGBA16F 0x881A
#endif

namespace
{
constexpr double EarthRadius = 6371.;     // km
constexpr double CloudHeight = 82.;       // km
constexpr double CloudThickness = 1.;     // km: the fine forms blur along a slanted line of sight through it
// The wind that carries the layer, km/s, for each night between these (the case of Dalin et al. 2004: 40 to 60 m/s from
// the north-east): the summer mesosphere blows to the west and to the equator, in both hemispheres. The field turns
// around the pole as a whole, at the westward speed at the latitude ReferenceLatitude.
constexpr double WestwardWind[2] = {0.030, 0.045};
constexpr double EquatorwardWind[2] = {0.015, 0.035};
constexpr double ReferenceLatitude = 60.*M_PI/180.;
constexpr double SampleSpacing = 2.;      // km: what the wind may move the clouds between two samples of an exposure
constexpr int MaxSamples = 8;
// With the Sun above this the sunlit clouds are lost in the bright sky (radians)
constexpr double HighestSunAltitude = -3.*M_PI/180.;
constexpr int VertexAttribIndex = 0;
}

NoctilucentCloudMgr::NoctilucentCloudMgr()
{
	setObjectName("NoctilucentCloudMgr");
}

NoctilucentCloudMgr::~NoctilucentCloudMgr()
{
}

void NoctilucentCloudMgr::init()
{
	QSettings* conf = StelApp::getInstance().getSettings();
	occurrenceMode = std::clamp(conf->value("noctilucent_clouds/occurrence", 1).toInt(), 1, 3);
	setFlagShow(conf->value("noctilucent_clouds/flag_show", false).toBool());
	setFlagHalfResolution(conf->value("noctilucent_clouds/flag_half_resolution", false).toBool());
	setOpticalDepth(conf->value("noctilucent_clouds/optical_depth", 1e-5).toDouble());
	addAction("actionShow_Noctilucent_Clouds", N_("Display Options"), N_("Noctilucent clouds"), "flagShow");

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

double NoctilucentCloudMgr::getCallOrder(StelModuleActionName actionName) const
{
	// Before the landscape, which hides what is below its horizon; the light is added, so the order against the sky
	// and the stars does not matter
	if (actionName == StelModule::ActionDraw)
		return StelApp::getInstance().getModuleMgr().getModule("LandscapeMgr")->getCallOrder(actionName) - 1.;
	return 0.;
}

void NoctilucentCloudMgr::setFlagShow(bool b)
{
	if (b != flagShow)
	{
		flagShow = b;
		StelApp::immediateSave("noctilucent_clouds/flag_show", b);
		emit flagShowChanged(b);
		emit occurrenceChanged(getOccurrence());
	}
}

void NoctilucentCloudMgr::setOccurrence(int mode)
{
	if (mode <= 0)
	{
		setFlagShow(false);
		return;
	}
	mode = std::min(mode, 3);
	if (mode != occurrenceMode)
	{
		occurrenceMode = mode;
		StelApp::immediateSave("noctilucent_clouds/occurrence", mode);
		if (flagShow)
			emit occurrenceChanged(mode);
	}
	setFlagShow(true);
}

void NoctilucentCloudMgr::setFlagHalfResolution(bool b)
{
	if (b != flagHalfResolution)
	{
		flagHalfResolution = b;
		StelApp::immediateSave("noctilucent_clouds/flag_half_resolution", b);
		emit flagHalfResolutionChanged(b);
	}
}

double NoctilucentCloudMgr::nightNumber(const StelCore* core)
{
	return std::floor(core->getJD() + static_cast<double>(core->getCurrentLocation().getLongitude())/360.);
}

void NoctilucentCloudMgr::update(double)
{
	// How far the clouds reach in this hemisphere tonight, for the user interface
	QString info;
	const StelCore* core = StelApp::getInstance().getCore();
	if (flagShow && core->getCurrentPlanet()->getEnglishName() == QStringLiteral("Earth"))
	{
		const bool south = core->getCurrentLocation().getLatitude() < 0.f;
		const NoctilucentCloudLight::Occurrence o = NoctilucentCloudLight::occurrence(core->getJD(), nightNumber(core), occurrenceMode, south);
		if (o.any && o.edge < 88.)
			info = q_("This night: clouds down to about %1 degrees of latitude").arg(qRound(o.edge));
		else
			info = q_("This night: no clouds in this hemisphere");
	}
	if (info != occurrenceInfo)
	{
		occurrenceInfo = info;
		emit occurrenceInfoChanged(info);
	}
}

void NoctilucentCloudMgr::setOpticalDepth(double value)
{
	value = std::clamp(value, 1e-7, 1e-3);
	if (!qFuzzyCompare(value, opticalDepth))
	{
		opticalDepth = value;
		StelApp::immediateSave("noctilucent_clouds/optical_depth", value);
		emit opticalDepthChanged(value);
	}
}

double NoctilucentCloudMgr::tangentHeight(const Vec3d& p, const Vec3d& sun)
{
	// On the sunlit side of the Earth the ray comes down to the point itself; behind it, the ray passes closest to the
	// center of the Earth before it arrives
	const double along = p*sun;
	return (along > 0. ? p.norm() : (p ^ sun).norm()) - EarthRadius;
}

double NoctilucentCloudMgr::lowestSunAltitude(double observerHeight)
{
	// The last point in the sunlight lies on the horizon of the observer towards the Sun. Seen from the center of the
	// Earth it lies this far from the observer (radians), and the Sun's rays reach it this far past their tangent point
	const double r = EarthRadius + CloudHeight;
	const double toHorizon = std::acos(EarthRadius/r) + std::acos(EarthRadius/(EarthRadius + std::max(0., observerHeight)));
	const double pastTangent = std::acos((EarthRadius + NoctilucentCloudLight::LowestHeight)/r);
	return -(toHorizon + pastTangent);
}

void NoctilucentCloudMgr::buildProgram(const StelProjector& projector)
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
		qWarning().noquote() << "NoctilucentCloudMgr: error in the vertex shader:\n" << newProgram->log();
		return;
	}

	// Everything is in the AltAz frame of the observer with its origin in the center of the Earth (km): the observer
	// stands on its z axis.
	const QByteArray frag = StelOpenGL::globalShaderPrefix(StelOpenGL::FRAGMENT_SHADER)
	                        + projector.getUnProjectShader()
	                        + extinction.getForwardTransformShader()
	                        + StelSkyDrawer::getPhotoExtinctionShader()
	                        + R"(
VARYING highp vec3 ndcPos;
uniform highp mat4 projectionMatrixInverse;
uniform bool extinctionEnabled;
uniform highp vec3 cloudSun;        // unit vector towards the Sun
uniform highp vec3 cloudSouth;      // the axes of the frame in the Earth-fixed one
uniform highp vec3 cloudEast;
uniform highp vec3 cloudUp;
uniform highp vec4 cloudFoot;       // the map position of the observer (km), the size of a pixel (radians), the
                                    // thickness of the layer (km)
uniform highp vec2 cloudNight;      // from the date: a number for the forms and the direction of the bands (radians)
uniform highp vec4 cloudReach;      // the latitude that the clouds reach (radians, north and south), the threshold of
                                    // their cover (north and south)
uniform highp vec2 cloudStrength;   // their optical depth against a typical display (north and south)
uniform highp vec4 cloudDrift;      // the turn of the field around the pole since the start (cosine, sine); how far it has
                                    // moved towards the equator (km); the time since then (s)
uniform highp vec2 cloudWind;       // the wind here on the map (km/s)
uniform highp float cloudExposure;  // exposure time of the camera (s, 0: an instant)
uniform int cloudSamples;           // moments of the exposure to average
uniform highp vec3 cloudRadii;      // of the observer, the Earth and the layer (km)
uniform sampler2D cloudLight;       // the light against the tangent height of the sunlight and the scattering angle
uniform highp vec2 cloudLightSize;  // of the table
uniform highp vec4 cloudLightScale; // from a tangent height (km) to the columns of the table; from its values to the
                                    // scene buffer; unused; the tangent height below which no light gets through (km)

// Noise from the date, on a map of the layer in km: hash (Dave Hoskins, "Hash without Sine") and value noise
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
	f = f*f*f*(f*(f*6. - 15.) + 10.);
	return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}
// The noise with the scale (km) and an offset of its own for each use
highp float cloudField(highp vec2 q, highp float scale, highp float offset)
{
	return cloudNoise(q/scale + vec2(cloudNight.x + offset, cloudNight.x));
}
// The same, stretched: along and across the coordinates (km) u and v
highp float cloudStreak(highp vec2 uv, highp float along, highp float across, highp float offset)
{
	return cloudNoise(uv/vec2(along, across) + vec2(cloudNight.x + offset, cloudNight.x));
}
highp float cloudRidge(highp float n)
{
	highp float r = 1. - abs(2.*n - 1.);
	return r*r*r;
}
// A flow that bends the forms without gathering them: the curl of a noise, of about the size amount (km)
highp vec2 cloudCurl(highp vec2 q, highp float scale, highp float offset, highp float amount)
{
	const highp float e = 0.5;
	highp float dy = cloudField(q + vec2(0., e), scale, offset) - cloudField(q - vec2(0., e), scale, offset);
	highp float dx = cloudField(q + vec2(e, 0.), scale, offset) - cloudField(q - vec2(e, 0.), scale, offset);
	return vec2(dy, -dx)*amount*scale/(2.*e);
}
// Waves or billows of the wavelength lam (km) with crests of the length leng (km) and at the angle a (radians), moving
// across the crests over the time time at their own speed (km/s): their period is lam/speed. Their contrast is reduced
// by what covers them across their crests: the pixel and the slanted layer (foot: along and across the line of sight,
// km; view: the direction of the line of sight on the map) and the motion between two moments of the exposure (smear, km)
highp float cloudWaves(highp vec2 p, highp float a, highp float lam, highp float leng, highp float offset, highp float speed, highp float time, highp vec2 foot, highp vec2 view, highp vec2 smear)
{
	highp vec2 along = vec2(cos(a), sin(a)), across = vec2(-along.y, along.x);
	// (the cells of the noise are 0.31 lam: its power peaks at the wavelength lam, measured)
	highp float n = cloudNoise(vec2(dot(p, along)/leng, (dot(p, across) - speed*time)/(0.31*lam)) + vec2(cloudNight.x + offset, offset));
	highp vec2 w = foot*vec2(dot(across, view), dot(along, view));
	highp float width2 = dot(w, w) + dot(across, smear)*dot(across, smear);
	return (2.*n - 1.)*exp(-2.47*width2/(lam*lam));
}
// The density of the layer on the map, about 1 where a display is seen, after the forms of Dalin et al. (2004, Ann.
// Geophys. 22, 1875) and the classes veil, bands, waves, billows and whirls of the British observers:
// the map is bent by a large flow and by whirls of 50 to 170 km; broad soft areas, drawn out along the bands, carry
// thin bands 33 to 44 km apart in two directions; waves of 9.5 km with short crests at an angle to them form the
// herringbone where the two directions meet, billows of 4.6 km the finest grain.
// In time (s) the bands and waves run through the field with speeds of 6 to 11 m/s, which give the periods that Dalin
// et al. measured (bands 55 to 112 minutes, waves 13 to 18, billows 6 to 8); the whirls and the patches of waves
// wander more slowly, so that forms change and come and go.
highp float cloudDensity(highp vec2 q, highp float threshold, highp float time, highp vec2 foot, highp vec2 view, highp vec2 smear)
{
	highp vec2 p = q + 260.*vec2(cloudField(q, 700., 3.) - 0.5, cloudField(q + 97., 700., 11.) - 0.5);
	p += cloudCurl(q + vec2(0.004, -0.003)*time, 170., 20., 26.);
	p += cloudCurl(p - vec2(0.005, 0.006)*time, 48., 24., 5.);
	highp float angle = cloudNight.y + cloudField(q, 1000., 40.) - 0.5;
	highp vec2 along = vec2(cos(angle), sin(angle));
	highp vec2 uv = vec2(dot(p, along), dot(p, vec2(-along.y, along.x)));
	highp float angle2 = angle + 0.62;
	along = vec2(cos(angle2), sin(angle2));
	highp vec2 uv2 = vec2(dot(p, along), dot(p, vec2(-along.y, along.x)));

	highp float area = 0.45*cloudStreak(uv, 520., 150., 0.) + 0.3*cloudStreak(uv + vec2(0., 0.002)*time, 240., 70., 3.) + 0.25*cloudStreak(uv2, 170., 42., 6.);
	area = clamp((area - 0.3)*3., 0., 1.);
	highp float bands = 0.75*cloudRidge(cloudStreak(uv - vec2(0., 0.008)*time, 300., 44., 9.))*(0.2 + 0.8*cloudStreak(uv, 170., 90., 15.))
	                  + 0.75*cloudRidge(cloudStreak(uv - vec2(0., 0.010)*time, 180., 33., 12.))*(0.15 + 0.85*cloudStreak(uv, 120., 60., 14.))
	                  + 0.6*cloudRidge(cloudStreak(uv2 + vec2(0., 0.006)*time, 260., 38., 18.))*clamp((cloudStreak(uv2, 330., 160., 19.) - 0.42)*4., 0., 1.);
	// a fine grain over the whole layer
	highp float wide = max(foot.x, foot.y);
	highp float grain = (cloudField(p, 24., 41.) - 0.5 + 0.5*(cloudField(p, 11., 43.) - 0.5))*exp(-2.47*wide*wide/324.);
	// the herringbone: one direction of the waves on each side
	highp float side = clamp((cloudField(q + vec2(0.004, 0.002)*time, 65., 50.) - 0.5)*6. + 0.5, 0., 1.);
	highp float waves = mix(cloudWaves(p, angle + 0.8, 9.5, 34., 21., 0.0105, time, foot, view, smear),
	                        cloudWaves(p, angle - 0.8, 9.5, 34., 27., -0.0105, time, foot, view, smear), side);
	waves *= clamp((cloudField(q + vec2(-0.010, 0.007)*time, 110., 60.) + 0.4*cloudField(q + vec2(0.008, 0.009)*time, 37., 62.) - 0.46)*3., 0., 1.);
	highp float billows = cloudWaves(p, angle + 1.5, 4.6, 14., 33., 0.011, time, foot, view, smear)
	                      *clamp((cloudField(q + vec2(0.009, -0.011)*time, 50., 70.) - 0.5)*4., 0., 1.);
	highp float cover = clamp((cloudField(q, 520., 80.) + 0.5*cloudField(q + 53., 210., 82.) - threshold)*3.2, 0., 1.);
	return max(0., cover*(0.12 + 1.2*area)*(1. + bands + 0.9*grain + 1.35*waves + 0.7*billows));
}

void main(void)
{
	highp vec4 winPos = projectionMatrixInverse * vec4(ndcPos, 1);
	bool ok = false;
	highp vec3 altAz = unProject(winPos.x, winPos.y, ok).xyz;
	if (!ok)
	{
		FRAG_COLOR = vec4(0);
		return;
	}
	altAz = normalize(altAz);
	highp float r0 = cloudRadii.x;
	highp float b = r0*altAz.z;
	if (b < 0. && b*b - r0*r0 + cloudRadii.y*cloudRadii.y > 0.)
	{
		FRAG_COLOR = vec4(0); // the line of sight meets the Earth
		return;
	}
	// where the line of sight, from inside the shell, leaves it
	highp float t = -b + sqrt(b*b - r0*r0 + cloudRadii.z*cloudRadii.z);
	highp vec3 p = vec3(0., 0., r0) + t*altAz;
	// the ray of sunlight to that point: the lowest it passes over the Earth (see tangentHeight() in the C++ code)
	highp float tangent = (dot(p, cloudSun) > 0. ? length(p) : length(cross(p, cloudSun))) - cloudRadii.y;
	if (tangent < cloudLightScale.w)
	{
		FRAG_COLOR = vec4(0); // in the shadow of the Earth
		return;
	}
	highp float angle = acos(clamp(dot(altAz, cloudSun), -1., 1.))/3.14159265;
	highp vec2 lightCoord = (vec2(0.5) + vec2(tangent*cloudLightScale.x, angle*(cloudLightSize.y - 1.)))/cloudLightSize;
	// a slanted line of sight crosses more of the layer
	highp float slant = length(p)/dot(p, altAz);

	// the place on the map: a polar stereographic projection, in km (nearly true in the zones of the clouds), of each
	// hemisphere; the fine forms are blurred by the pixel and by the slanted thickness of the layer
	highp vec3 earth = p.x*cloudSouth + p.y*cloudEast + p.z*cloudUp;
	highp vec3 unit = earth/length(earth);
	bool southern = unit.z < 0.;
	// the clouds of this night reach this far, along a wavy edge, up to 5 degrees either way
	highp float latitude = asin(abs(unit.z));
	highp float reach = southern ? cloudReach.y : cloudReach.x;
	if (latitude < reach - 0.14)
	{
		FRAG_COLOR = vec4(0);
		return;
	}
	highp vec2 map = 2.*cloudRadii.z*unit.xy/(1. + abs(unit.z));
	reach += 0.175*(cloudField(map, 1100., 90.) - 0.5);
	highp float edge = smoothstep(reach - 0.026, reach + 0.07, latitude);
	highp vec2 view = map - cloudFoot.xy;
	view /= max(length(view), 1e-3);
	highp vec2 foot = (t*cloudFoot.z*vec2(slant, 1.) + vec2(cloudFoot.w*slant, 0.));
	// the clouds are carried by the wind; the camera averages the moments of its exposure, and the motion between two
	// of them blurs the finest forms
	highp vec2 smear = cloudWind*cloudExposure/float(cloudSamples);
	highp float density = 0.;
	for (int i = 0; i < cloudSamples; ++i)
	{
		highp float moment = cloudExposure*((float(i) + 0.5)/float(cloudSamples) - 0.5);
		highp vec2 place = map - cloudWind*moment;
		highp float radius = max(length(place), 1e-3);
		place *= abs(radius - cloudDrift.z)/radius;
		place = vec2(cloudDrift.x*place.x - cloudDrift.y*place.y, cloudDrift.y*place.x + cloudDrift.x*place.y);
		density += cloudDensity(southern ? place + 5000. : place, southern ? cloudReach.w : cloudReach.z, cloudDrift.w + moment, foot, view, smear);
	}
	density *= edge*(southern ? cloudStrength.y : cloudStrength.x)/float(cloudSamples);

	highp vec3 color = texture2D(cloudLight, lightCoord).rgb*cloudLightScale.y*slant*density;
	if (extinctionEnabled)
		color *= pow(10., -0.4*extinctionMagnitude(altAz)) * photoExtinctionTint(altAz.z);
	FRAG_COLOR = vec4(color, 1.);
}
)";
	if (!newProgram->addShaderFromSourceCode(QOpenGLShader::Fragment, frag))
	{
		qWarning().noquote() << "NoctilucentCloudMgr: error in the fragment shader:\n" << newProgram->log();
		return;
	}
	newProgram->bindAttributeLocation("vertex", VertexAttribIndex);
	if (!StelPainter::linkProg(newProgram.get(), "NoctilucentCloudMgr program"))
		return;
	program = std::move(newProgram);
}

void NoctilucentCloudMgr::bindLightTexture(int unit)
{
	auto& gl = *QOpenGLContext::currentContext()->functions();
	gl.glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(unit));
	if (!lightTexture)
	{
		const std::vector<float> table = NoctilucentCloudLight::computeTable();
		gl.glGenTextures(1, &lightTexture);
		gl.glBindTexture(GL_TEXTURE_2D, lightTexture);
		gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB32F, NoctilucentCloudLight::HeightCount, NoctilucentCloudLight::AngleCount,
		                0, GL_RGB, GL_FLOAT, table.data());
		gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	}
	gl.glBindTexture(GL_TEXTURE_2D, lightTexture);
	gl.glActiveTexture(GL_TEXTURE0);
}

void NoctilucentCloudMgr::draw(StelCore* core)
{
	if (!flagShow)
		return;
	StelSkyDrawer* drawer = core->getSkyDrawer();
	if (!drawer->getFlagPhotographicModeActive() || core->getCurrentPlanet()->getEnglishName() != QStringLiteral("Earth"))
		return;

	// The direction of the Sun (AltAz: x to the south, y to the east, z up)
	SolarSystem* ssystem = GETSTELMODULE(SolarSystem);
	Vec3d sun = core->j2000ToAltAz(ssystem->getSun()->getJ2000EquatorialPos(core), StelCore::RefractionOff);
	sun.normalize();
	const double sunAltitude = std::asin(std::clamp(sun[2], -1., 1.));

	// The observer, over a spherical Earth, is below the layer
	const double observerHeight = std::max(0., core->getCurrentLocation().altitude*0.001);
	if (observerHeight >= CloudHeight - 1. || sunAltitude > HighestSunAltitude || sunAltitude < lowestSunAltitude(observerHeight))
		return;

	// The clouds of this night: are they in sight? The wavy edge comes up to 8 degrees closer, the horizon lies 10 degrees away
	const double night = nightNumber(core);
	const NoctilucentCloudLight::Occurrence north = NoctilucentCloudLight::occurrence(core->getJD(), night, occurrenceMode, false);
	const NoctilucentCloudLight::Occurrence southern = NoctilucentCloudLight::occurrence(core->getJD(), night, occurrenceMode, true);
	const double observerLatitude = static_cast<double>(core->getCurrentLocation().getLatitude());
	if (!(north.any && observerLatitude + 18. >= north.edge) && !(southern.any && 18. - observerLatitude >= southern.edge))
		return;

	const StelProjectorP prj = core->getProjection(StelCore::FrameAltAz, StelCore::RefractionAuto);
	if (!programProjector || !prj->isSameProjection(*programProjector))
	{
		buildProgram(*prj);
		programProjector = prj;
	}
	if (!program)
		return;

	const Extinction& extinction = drawer->getExtinction();
	bindLightTexture(2);

	QOpenGLShaderProgram& p = *program;
	p.bind();
	p.setUniformValue("projectionMatrixInverse", prj->getProjectionMatrix().toQMatrix().inverted());
	prj->setUnProjectUniforms(p);
	extinction.setForwardTransformUniforms(p);
	drawer->setPhotoExtinctionUniforms(p, 1);
	p.setUniformValue("extinctionEnabled", drawer->getFlagHasAtmosphere() && extinction.getExtinctionCoefficient() >= 0.01f);
	p.setUniformValue("cloudSun", QVector3D(static_cast<float>(sun[0]), static_cast<float>(sun[1]), static_cast<float>(sun[2])));
	p.setUniformValue("cloudRadii", QVector3D(static_cast<float>(EarthRadius + observerHeight), static_cast<float>(EarthRadius),
	                                          static_cast<float>(EarthRadius + CloudHeight)));
	p.setUniformValue("cloudLight", 2);
	p.setUniformValue("cloudLightSize", QVector2D(static_cast<float>(NoctilucentCloudLight::HeightCount),
	                                              static_cast<float>(NoctilucentCloudLight::AngleCount)));
	p.setUniformValue("cloudLightScale", QVector4D(static_cast<float>((NoctilucentCloudLight::HeightCount - 1)/NoctilucentCloudLight::MaxHeight),
	                                               static_cast<float>(opticalDepth)*drawer->getPhotoSceneScale(), 0.f,
	                                               static_cast<float>(NoctilucentCloudLight::LowestHeight)));

	// The frame of the observer in the Earth-fixed one, over a spherical Earth, and the place on the map
	const StelLocation& location = core->getCurrentLocation();
	const double phi = static_cast<double>(location.getLatitude())*M_PI/180.;
	const double lambda = static_cast<double>(location.getLongitude())*M_PI/180.;
	const Vec3d up(std::cos(phi)*std::cos(lambda), std::cos(phi)*std::sin(lambda), std::sin(phi));
	const Vec3d east(-std::sin(lambda), std::cos(lambda), 0.);
	const Vec3d south(std::sin(phi)*std::cos(lambda), std::sin(phi)*std::sin(lambda), -std::cos(phi));
	const auto vec = [](const Vec3d& v) {return QVector3D(static_cast<float>(v[0]), static_cast<float>(v[1]), static_cast<float>(v[2]));};
	p.setUniformValue("cloudSouth", vec(south));
	p.setUniformValue("cloudEast", vec(east));
	p.setUniformValue("cloudUp", vec(up));
	const double mapScale = 2.*(EarthRadius + CloudHeight)/(1. + std::fabs(up[2]));
	p.setUniformValue("cloudFoot", QVector4D(static_cast<float>(mapScale*up[0]), static_cast<float>(mapScale*up[1]),
	                                         static_cast<float>((flagHalfResolution ? 2. : 1.)/static_cast<double>(prj->getPixelPerRadAtCenter())),
	                                         static_cast<float>(CloudThickness)));

	// The forms of this night: the day changes at noon at the place, and nearby places share their night
	const double formNumber = std::fmod(night, 997.);
	p.setUniformValue("cloudNight", QVector2D(static_cast<float>(formNumber*0.61803), static_cast<float>(std::fmod(formNumber*2.39996, 2.*M_PI))));
	const auto reach = [](const NoctilucentCloudLight::Occurrence& o) {return static_cast<float>((o.any ? o.edge : 100.)*M_PI/180.);};
	p.setUniformValue("cloudReach", QVector4D(reach(north), reach(southern), static_cast<float>(north.cover), static_cast<float>(southern.cover)));
	p.setUniformValue("cloudStrength", QVector2D(static_cast<float>(north.strength), static_cast<float>(southern.strength)));

	// The wind of this night, from the time since the same moment for everybody
	const double since = (core->getJD() - night)*86400.;
	const double westward = WestwardWind[0] + (WestwardWind[1] - WestwardWind[0])*std::fmod(formNumber*0.7548777, 1.);
	const double equatorward = EquatorwardWind[0] + (EquatorwardWind[1] - EquatorwardWind[0])*std::fmod(formNumber*0.5698403, 1.);
	const double turnRate = westward/((EarthRadius + CloudHeight)*std::cos(ReferenceLatitude));
	p.setUniformValue("cloudDrift", QVector4D(static_cast<float>(std::cos(turnRate*since)), static_cast<float>(std::sin(turnRate*since)),
	                                          static_cast<float>(equatorward*since), static_cast<float>(since)));
	const Vec2d here(mapScale*up[0], mapScale*up[1]);
	const double hereRadius = std::max(here.norm(), 1e-3);
	const Vec2d wind(turnRate*here[1] + equatorward*here[0]/hereRadius, -turnRate*here[0] + equatorward*here[1]/hereRadius);
	p.setUniformValue("cloudWind", QVector2D(static_cast<float>(wind[0]), static_cast<float>(wind[1])));

	// The exposure, as for the aurorae: nothing with a camera that takes the picture of an instant
	const double exposure = drawer->getFlagPhotoStarTrails() ? std::max(0., drawer->getPhotoEffectiveExposureTime()) : 0.;
	p.setUniformValue("cloudExposure", static_cast<float>(exposure));
	p.setUniformValue("cloudSamples", std::clamp(static_cast<int>(std::ceil(exposure*wind.norm()/SampleSpacing)), 1, MaxSamples));

	auto& gl = *QOpenGLContext::currentContext()->functions();
	if (!flagHalfResolution)
	{
		lowResBuffer.reset();
		gl.glEnable(GL_BLEND);
		gl.glBlendFunc(GL_ONE, GL_ONE);
		drawQuad();
		gl.glDisable(GL_BLEND);
		p.release();
		return;
	}

	// At half resolution: into a buffer of its own (the full screen quad covers it all, so the shader maps it on the
	// same view), then scaled up and added to the scene
	GLint previousBuffer = 0;
	gl.glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousBuffer);
	GLint viewport[4];
	gl.glGetIntegerv(GL_VIEWPORT, viewport);
	const QSize size(std::max(1, (viewport[2] + 1)/2), std::max(1, (viewport[3] + 1)/2));
	if (!lowResBuffer || lowResBuffer->size() != size)
	{
		QOpenGLFramebufferObjectFormat format;
		format.setInternalTextureFormat(GL_RGBA16F);
		lowResBuffer.reset(new QOpenGLFramebufferObject(size, format));
		gl.glBindTexture(GL_TEXTURE_2D, lowResBuffer->texture());
		gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		gl.glBindTexture(GL_TEXTURE_2D, 0);
	}
	gl.glBindFramebuffer(GL_FRAMEBUFFER, lowResBuffer->handle());
	gl.glViewport(0, 0, size.width(), size.height());
	gl.glDisable(GL_BLEND);
	gl.glClearColor(0.f, 0.f, 0.f, 0.f);
	gl.glClear(GL_COLOR_BUFFER_BIT);
	drawQuad();
	p.release();
	gl.glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previousBuffer));
	gl.glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);

	if (!compositeProgram)
	{
		std::unique_ptr<QOpenGLShaderProgram> composite(new QOpenGLShaderProgram);
		const QByteArray vert = StelOpenGL::globalShaderPrefix(StelOpenGL::VERTEX_SHADER) + R"(
ATTRIBUTE highp vec3 vertex;
VARYING highp vec2 texCoord;
void main()
{
	gl_Position = vec4(vertex, 1.);
	texCoord = 0.5*vertex.xy + 0.5;
}
)";
		const QByteArray frag = StelOpenGL::globalShaderPrefix(StelOpenGL::FRAGMENT_SHADER) + R"(
VARYING highp vec2 texCoord;
uniform sampler2D lightTex;
void main()
{
	FRAG_COLOR = vec4(texture2D(lightTex, texCoord).rgb, 1.);
}
)";
		if (composite->addShaderFromSourceCode(QOpenGLShader::Vertex, vert)
		    && composite->addShaderFromSourceCode(QOpenGLShader::Fragment, frag))
		{
			composite->bindAttributeLocation("vertex", VertexAttribIndex);
			if (StelPainter::linkProg(composite.get(), "NoctilucentCloudMgr composite program"))
				compositeProgram = std::move(composite);
		}
		if (!compositeProgram)
		{
			qWarning() << "NoctilucentCloudMgr: cannot build the program that adds the light at half resolution";
			flagHalfResolution = false;
			return;
		}
	}
	compositeProgram->bind();
	gl.glActiveTexture(GL_TEXTURE0);
	gl.glBindTexture(GL_TEXTURE_2D, lowResBuffer->texture());
	compositeProgram->setUniformValue("lightTex", 0);
	gl.glEnable(GL_BLEND);
	gl.glBlendFunc(GL_ONE, GL_ONE);
	drawQuad();
	gl.glDisable(GL_BLEND);
	gl.glBindTexture(GL_TEXTURE_2D, 0);
	compositeProgram->release();
}

void NoctilucentCloudMgr::drawQuad()
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
