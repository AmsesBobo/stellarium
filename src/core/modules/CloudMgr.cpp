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
#include "CloudShape.hpp"
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
#include <QVector2D>
#include <QVector3D>
#include <QVector4D>
#include <algorithm>
#include <cmath>

namespace
{
constexpr double EarthRadius = 6371.;     // km
constexpr int VertexAttribIndex = 0;
}

#ifndef GL_RGBA32F
#define GL_RGBA32F 0x8814
#endif

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
	setCover(conf->value("clouds/cover", 0.2).toDouble());
	setSize(conf->value("clouds/size", 1.).toDouble());
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

void CloudMgr::setCover(double value)
{
	value = std::clamp(value, 0., 1.);
	if (!qFuzzyCompare(value, cover))
	{
		cover = value;
		StelApp::immediateSave("clouds/cover", value);
		emit coverChanged(value);
	}
}

void CloudMgr::setSize(double value)
{
	value = std::clamp(value, 0.4, 2.5);
	if (!qFuzzyCompare(value, size))
	{
		size = value;
		StelApp::immediateSave("clouds/size", value);
		emit sizeChanged(value);
	}
}

void CloudMgr::bindTileTexture(int unit)
{
	auto& gl = *QOpenGLContext::currentContext()->functions();
	gl.glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(unit));
	if (!tileTexture)
	{
		if (tile.empty())
			tile = CloudShape::computeTile();
		gl.glGenTextures(1, &tileTexture);
		gl.glBindTexture(GL_TEXTURE_2D, tileTexture);
		gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, CloudShape::TileTexels, CloudShape::TileTexels, 0, GL_RGBA, GL_FLOAT, tile.data());
		gl.glGenerateMipmap(GL_TEXTURE_2D);
		gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
		gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
		gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	}
	gl.glBindTexture(GL_TEXTURE_2D, tileTexture);
	gl.glActiveTexture(GL_TEXTURE0);
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
uniform highp vec4 cloudRadii;      // the observer's radius; the radius of the base; c of the base and of the top of the
                                    // layer: (r - observer)(r + observer) (km)
uniform highp float cloudPixel;     // the size of a pixel (radians)
uniform highp vec3 cloudSun;        // unit vectors towards the Sun and the Moon
uniform highp vec3 cloudMoon;
uniform highp vec3 cloudKey;        // the unit vector towards the light that casts shadows
uniform highp vec3 cloudBelow;      // the light of the clouds and the air (see CloudLight), for the scene
uniform highp vec3 cloudAbove;
uniform highp vec3 cloudAboveSun;
uniform highp vec3 cloudCity;
uniform highp vec3 sideSky;
uniform highp vec3 sideSunLog[5];   // the logarithm of the light of CloudLight::Light::sideSun
uniform highp vec2 sideSunOffset;   // the first offset of its altitude and its step (degrees)
uniform highp vec3 sideMoon;
uniform highp vec3 silverSun;
uniform highp vec3 silverMoon;
uniform highp vec3 airSky;
uniform highp float airSunlit;      // how much of the light of the sky comes from sunlit air below the clouds
uniform highp vec3 airMoon;
uniform highp vec3 airExtinction;   // per km
const int CloudSubsteps = 6;
uniform sampler2D cloudTile;        // the height map (see CloudShape)
uniform highp vec4 cloudTileScale;  // 1/km: of the tile, of its two cover sizes, of the fine lobes
uniform highp float cloudSize;      // the scale of the clouds against those of the tile
uniform highp vec4 cloudCover;      // cover offset: constant, per noise; the mean size of a texel of the tile (km); the
                                    // extinction in the clouds (per km)

// The place on the map: a polar stereographic projection, in km, of each hemisphere
highp vec2 cloudMap(highp vec3 p)
{
	highp vec3 earth = p.x*cloudSouth + p.y*cloudEast + p.z*cloudUp;
	return 2.*cloudRadii.y*earth.xy/(length(earth) + abs(earth.z));
}

// The heights over the base of the top and the underside of the cloud at a place (km, the top below 0 outside the clouds),
// from the shape at the level of detail lod. near: how much the fine lobes count at the height z
highp vec2 cloudHeights(highp vec2 map, highp float lod, highp float near, highp float z)
{
	highp vec4 f = textureLod(cloudTile, map*cloudTileScale.x, lod);
	highp float noise = 0.6*textureLod(cloudTile, map*cloudTileScale.y, 2.).z + 0.4*textureLod(cloudTile, map*cloudTileScale.z, 2.).z;
	highp float s = f.x + min(cloudCover.x + cloudCover.y*noise, 0.34); // (it never fills the clear parts of the map)
	// Fine lobes, stronger at the edge of a cloud, which they tear up. They change with the height, or the sides of the
	// clouds would show grooves that run straight up
	if (near > 0.)
	{
		highp vec2 fine = (map + z*vec2(1.2, -0.9))*cloudTileScale.w;
		s += near*(0.12 + 0.3*(1. - clamp(s/0.3, 0., 1.)))*(textureLod(cloudTile, fine, 0.).w - 0.62)
		   + near*0.1*(textureLod(cloudTile, fine*2.7, 0.).w - 0.62);
	}
	// a rounded top, and an underside that is flat but for its rim
	highp float height = cloudSize*f.y;
	highp float top = height*s/pow(abs(s) + 0.01, 0.3);
	return vec2(top, 0.3*height*pow(1. - clamp(s/0.35, 0., 1.), 2.));
}

// The sunlight on a side of a cloud with the Sun higher by this much (degrees) than at the base over the observer
highp vec3 cloudSunlight(highp float offset)
{
	highp float x = clamp((offset - sideSunOffset.x)/sideSunOffset.y, 0., 3.999);
	highp int i = int(x);
	return exp(mix(sideSunLog[i], sideSunLog[i + 1], x - float(i)));
}

highp float shellDistance(highp float b, highp float c)
{
	// from inside the shell, in this form to keep the precision over short ways
	return c/(b + sqrt(b*b + c));
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
	// Below the horizon the line of sight meets the Earth. Near it the refraction carries it on along the Earth
	if (altAz.z < -0.015)
	{
		FRAG_COLOR = vec4(0., 0., 0., 1.);
		return;
	}
	// (a level line of sight still reaches the layer, however far away)
	if (altAz.z < 0.008)
		altAz.z = 0.008*exp(altAz.z/0.008 - 1.);
	highp float r0 = cloudRadii.x;
	highp float b = r0*altAz.z;
	highp float t0 = shellDistance(b, cloudRadii.z), t1 = shellDistance(b, cloudRadii.w);
	int steps = int(clamp(ceil((t1 - t0)/0.12), 1., 24.));
	highp float ds = (t1 - t0)/float(steps);

	// Through the layer, until the clouds are opaque; the first place in a cloud is the one we see. Long steps look for
	// the clouds, short ones find their surface
	highp float depth = 0., hitT = -1., hitZ = 0., hitTop = 0., hitUnder = 0., hitLod = 0., hitNear = 0.;
	for (int k = 0; k < steps && depth < 7.; ++k)
	{
		highp float t = t0 + float(k)*ds;
		highp vec3 p = vec3(altAz.xy*t, r0 + altAz.z*t);
		highp float z = length(p) - cloudRadii.y;
		highp float foot = max(t*cloudPixel, 0.1*ds);
		highp float lod = max(log2(foot/cloudCover.z), 0.);
		highp float near = 1. - smoothstep(0.015, 0.05, foot/cloudSize);
		// A blurred look at the map tells whether a cloud lies anywhere near this step (the clear parts are clamped)
		if (textureLod(cloudTile, cloudMap(p + 0.5*ds*altAz)*cloudTileScale.x, log2(2.*ds/cloudCover.z)).x > -0.345)
		{
			highp float dt = ds/float(CloudSubsteps);
			for (int j = 0; j < CloudSubsteps && depth < 7.; ++j)
			{
				highp float u = t + (float(j) + 0.5)*dt;
				highp vec3 q = vec3(altAz.xy*u, r0 + altAz.z*u);
				highp float height = length(q) - cloudRadii.y;
				highp vec2 h = cloudHeights(cloudMap(q), lod, near, height);
				highp float e = max(0.012, foot);
				// (the density grows over the first meters into the cloud)
				highp float inside = smoothstep(-e, e + 0.05*cloudSize, min(h.x - height, height - h.y));
				depth += cloudCover.w*dt*inside;
				if (hitT < 0. && depth > 0.03)
				{
					// the surface lies between this place and the last one
					highp float inner = u, outer = u - dt;
					for (int n = 0; n < 4; ++n)
					{
						highp float mid = 0.5*(inner + outer);
						q = vec3(altAz.xy*mid, r0 + altAz.z*mid);
						height = length(q) - cloudRadii.y;
						h = cloudHeights(cloudMap(q), lod, near, height);
						if (min(h.x - height, height - h.y) > -e)
							inner = mid;
						else
							outer = mid;
					}
					hitT = inner;
					hitZ = height;
					hitTop = h.x;
					hitUnder = h.y;
					hitLod = lod;
					hitNear = near;
				}
			}
		}
	}
	if (hitT < 0.)
	{
		FRAG_COLOR = vec4(0., 0., 0., 1.);
		return;
	}
	highp float T = exp(-depth);

	// The normal of the surface, from the slope of the top. The clouds stand on the local level
	highp vec3 p = vec3(altAz.xy*hitT, r0 + altAz.z*hitT);
	highp float delta = max(0.035*cloudSize, 2.*hitT*cloudPixel);
	// (the base goes over into the sides with the height)
	highp float fromBelow = 1. - smoothstep(0., 0.3*cloudSize, hitZ);
	highp vec2 map = cloudMap(p);
	highp float centerTop = cloudHeights(map, hitLod, 0.5*hitNear, hitZ).x;
	highp vec2 slope = vec2(cloudHeights(cloudMap(p + vec3(delta, 0., 0.)), hitLod, 0.5*hitNear, hitZ).x, cloudHeights(cloudMap(p + vec3(0., delta, 0.)), hitLod, 0.5*hitNear, hitZ).x) - centerTop;
	highp vec3 normal = normalize(vec3(-slope/delta, 1.));

	// The hollows between the lobes see less of the sky
	highp float blurred = cloudHeights(map, hitLod + 3.5, 0., 0.).x;
	highp float open = clamp(1. - 1.5*(blurred - hitZ)/(0.3*cloudSize), 0.35, 1.);

	// The shadow from the clouds themselves and each other towards the light, in longer steps further away: scattered
	// many times, light gets into the clouds much deeper than their extinction says
	highp float shadow = 1.;
	if (hitT*cloudPixel < 0.3 && cloudKey.z > -0.05)
	{
		highp float seen = 0., reach = 0.05*cloudSize;
		highp vec3 start = p + 0.03*cloudSize*mix(normal, vec3(0., 0., -1.), fromBelow);
		for (int k = 0; k < 6; ++k)
		{
			highp vec3 q = start + reach*cloudKey;
			highp float height = length(q) - cloudRadii.y;
			highp vec2 h = cloudHeights(cloudMap(q), hitLod + 0.7*float(k), 0., 0.);
			seen += 0.5*reach*smoothstep(0., 0.1*reach + 0.03, min(h.x - height, height - h.y));
			reach *= 1.9;
		}
		shadow = exp(-4.*seen/cloudSize);
	}

	highp float sunCos = dot(altAz, cloudSun), moonCos = dot(altAz, cloudMoon);
	highp float high = clamp(hitZ/max(hitTop, 0.05), 0., 1.);
	// Towards the Sun a cloud far away stands under a higher Sun, by the curve of the Earth, and so does a higher part of it
	highp vec2 sunWay = cloudSun.xy/max(length(cloudSun.xy), 0.01);
	highp vec3 sideSun = cloudSunlight(dot(p.xy, sunWay)*57.3/7433. + 0.35*hitZ);
	highp float keyIsSun = step(0.5, dot(cloudKey, cloudSun));
	// (soft on the side away from the light, which the cloud lets through)
	highp float lit = 0.2 + 0.8*shadow;
	highp vec3 side = sideSky*(0.45 + 0.35*normal.z + 0.2*high)*open
	                + sideSun*clamp((dot(normal, cloudSun) + 0.5)/1.5, 0., 1.)*mix(1., lit, keyIsSun)
	                + sideMoon*clamp((dot(normal, cloudMoon) + 0.5)/1.5, 0., 1.)*mix(lit, 1., keyIsSun);

	// From below the line of sight meets the underside. It reflects the ground and lets through what falls on its top, less
	// where the cloud is thick; towards the Sun that light goes on forwards
	highp float through = 1./(1. + 6.75*max(hitTop - hitZ, 0.)/cloudSize);
	highp float forwards = 1. + 3.*exp(-(1. - sunCos)/0.06);
	highp vec3 base = mix(cloudBelow, cloudAbove, through) + through*cloudAboveSun*forwards*shadow + cloudCity;
	// A low Sun lights the bumps of the underside from the side, and the light spreads in the cloud
	highp float lowSun = 1. - smoothstep(0.03, 0.3, abs(cloudSun.z));
	base += sideSun*lowSun*lit*(0.2 + 0.6*clamp(dot(normalize(vec3(normal.xy, 0.05)), cloudSun), 0., 1.))*(1. + 3.*exp(-(1. - sunCos)/0.15));
	highp vec3 light = mix(side, base, fromBelow);

	// The thin edges scatter the light of the Sun and the Moon forwards (Henyey-Greenstein with g = 0.85)
	highp vec3 silver = silverSun*(0.2775/pow(1.7225 - 1.7*sunCos, 1.5)) + silverMoon*(0.2775/pow(1.7225 - 1.7*moonCos, 1.5));
	light += silver*T*(1. - T);

	// The way through the air dims the cloud and adds the light of the air (Rayleigh phase function)
	highp vec3 way = exp(-airExtinction*hitT);
	highp vec3 air = (1. - way)*(airSky + airMoon*0.75*(1. + moonCos*moonCos));
	// In sunlit air the sky behind the cloud stands for the air in front of it, so that far clouds fade into the sky. It does
	// not do so near the Sun, where the air lies in the shadow of the cloud
	highp float kept = (1. - way.y)*airSunlit*(1. - smoothstep(0.6, 0.98, sunCos));
	highp float cover = (1. - T)*(1. - kept);
	// out = source + scene x alpha
	FRAG_COLOR = vec4((light*way + air)*cover, 1. - cover);
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
	       && core->getCurrentLocation().altitude*0.001 < CloudShape::BaseHeight;
}

double CloudMgr::getTransmission(const StelCore* core, Vec3d altAz) const
{
	if (!isShown(core))
		return 1.;
	if (tile.empty())
		tile = CloudShape::computeTile();
	altAz.normalize();
	const StelLocation& location = core->getCurrentLocation();
	const double phi = static_cast<double>(location.getLatitude())*M_PI/180.;
	const double lambda = static_cast<double>(location.getLongitude())*M_PI/180.;
	const std::array<double, 3> up = {std::cos(phi)*std::cos(lambda), std::cos(phi)*std::sin(lambda), std::sin(phi)};
	const std::array<double, 3> east = {-std::sin(lambda), std::cos(lambda), 0.};
	const std::array<double, 3> south = {std::sin(phi)*std::cos(lambda), std::sin(phi)*std::sin(lambda), -std::cos(phi)};
	return CloudShape::transmission(tile, cover, size, EarthRadius + std::max(0., location.altitude*0.001), {altAz[0], altAz[1], altAz[2]}, south, east, up);
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
	Vec3d sun = ssystem->getSun()->getAltAzPosGeometric(core);
	sun.normalize();
	s.sunSine = sun[2];
	s.sunAltitude = std::asin(std::clamp(s.sunSine, -1., 1.));
	s.top = CloudShape::BaseHeight + 0.6*size;
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
	// The shadows follow the Sun while it lights the clouds, and otherwise the Moon
	p.setUniformValue("cloudKey", direction(light.sideSun[3][1] > light.sideMoon[1] ? sun : moon));
	p.setUniformValue("cloudBelow", scene(light.below));
	p.setUniformValue("cloudAbove", scene(light.above));
	p.setUniformValue("cloudAboveSun", scene(light.aboveSun));
	p.setUniformValue("cloudCity", scene(light.city));
	p.setUniformValue("sideSky", scene(light.sideSky));
	QVector3D sideSunLog[5];
	for (std::size_t k = 0; k < 5; ++k)
	{
		const QVector3D v = scene(light.sideSun[k]);
		sideSunLog[k] = QVector3D(std::log(std::max(v.x(), 1e-30f)), std::log(std::max(v.y(), 1e-30f)), std::log(std::max(v.z(), 1e-30f)));
	}
	p.setUniformValueArray("sideSunLog", sideSunLog, 5);
	p.setUniformValue("sideSunOffset", QVector2D(static_cast<float>(CloudLight::SunOffsetStart), static_cast<float>(CloudLight::SunOffsetStep)));
	p.setUniformValue("silverSun", scene(light.silverSun));
	p.setUniformValue("silverMoon", scene(light.silverMoon));
	p.setUniformValue("sideMoon", scene(light.sideMoon));
	p.setUniformValue("airSky", scene(light.airSky));
	p.setUniformValue("airSunlit", static_cast<float>(light.airSunlit));
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
	const double r0 = EarthRadius + observerHeight;
	const double base = EarthRadius + CloudShape::BaseHeight, top = base + CloudShape::LayerThickness*size;
	p.setUniformValue("cloudRadii", QVector4D(static_cast<float>(r0), static_cast<float>(base), static_cast<float>((base - r0)*(base + r0)),
	                                          static_cast<float>((top - r0)*(top + r0))));
	p.setUniformValue("cloudPixel", static_cast<float>(1./static_cast<double>(prj->getPixelPerRadAtCenter())));
	bindTileTexture(2);
	p.setUniformValue("cloudTile", 2);
	p.setUniformValue("cloudTileScale", QVector4D(static_cast<float>(1./(size*CloudShape::TileSize)), static_cast<float>(1./CloudShape::CoverScaleLarge),
	                                              static_cast<float>(1./CloudShape::CoverScaleSmall), static_cast<float>(1./(size*CloudShape::FineScale))));
	p.setUniformValue("cloudSize", static_cast<float>(size));
	// the offset of the cover is linear in the noise: two points give it
	const float coverStart = static_cast<float>(CloudShape::coverOffset(cover, 0.));
	p.setUniformValue("cloudCover", QVector4D(coverStart, static_cast<float>(CloudShape::coverOffset(cover, 1.)) - coverStart,
	                                          static_cast<float>(size*CloudShape::TileSize/CloudShape::TileTexels), static_cast<float>(CloudShape::Extinction)));

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
