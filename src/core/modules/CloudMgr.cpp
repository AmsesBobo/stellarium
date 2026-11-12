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
#include <QStringList>
#include <QOpenGLBuffer>
#include <QOpenGLExtraFunctions>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFramebufferObjectFormat>
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
constexpr int MaxExposureSamples = 5;

// The tile of the clouds and the weather fronts in it, for the shaders: see CloudShape::frontCover()
QByteArray frontShader()
{
	const auto n = [](double v) {return QByteArray::number(v, 'g', 8);};
	return "uniform highp vec2 frontPhase[2];\n"
	       "uniform sampler2D cloudTile;        // the height map (see CloudShape)\n"
	       "uniform highp vec2 cloudFrontDrift; // where the fronts lie on the tile\n"
	       "uniform highp vec4 cloudFrontTurn;  // where the wind has carried them: the turn of the map, as cosine and sine; its offset (km)\n"
	       "// The place on the map of the fronts, from a place on the map before the wind\n"
	       "highp vec2 frontMap(highp vec2 map)\n"
	       "{\n"
	       "\treturn vec2(cloudFrontTurn.x*map.x - cloudFrontTurn.y*map.y, cloudFrontTurn.y*map.x + cloudFrontTurn.x*map.y) + cloudFrontTurn.zw;\n"
	       "}\n"
	       "// The cover of the fronts at a place of the map without the shift of a layer. front: on, the shift across the\n"
	       "// fronts (km), the bias of the cover, its scale\n"
	       "highp float frontCover(highp vec2 map, highp vec4 front)\n"
	       "{\n"
	       "\thighp float bend = " + n(CloudShape::FrontBend) + "*(textureLod(cloudTile, map/" + n(CloudShape::FrontBendScale) + " + frontPhase[0] + vec2(0.1, 0.6), 1.).z - 0.5);\n"
	       "\thighp vec2 uv = vec2((" + n(CloudShape::FrontAxisCos) + "*map.x - (" + n(CloudShape::FrontAxisSin) + ")*map.y)/" + n(CloudShape::FrontAlong)
	       + ", ((" + n(CloudShape::FrontAxisSin) + ")*map.x + " + n(CloudShape::FrontAxisCos) + "*map.y + bend + front.y)/" + n(CloudShape::FrontAcross) + ") + cloudFrontDrift + frontPhase[1];\n"
	       "\treturn front.w*clamp(" + n(CloudShape::FrontGain) + "*(textureLod(cloudTile, uv, 1.).z - " + n(CloudShape::FrontMid) + ") + front.z, 0., "
	       + n(CloudShape::FrontMax) + ");\n"
	       "}\n";
}

// The density of the cirrus, for both shaders: see CloudShape::cirrusDensity()
QByteArray cirrusShader()
{
	return R"(uniform highp vec2 cirrusTexturePhase[5];
highp float cirrusNoise(highp vec2 uv, highp float lod)
{
	return textureLod(cloudTile, uv, lod).z;
}

// The density from 0 to 1 at a place of the map, with the size of a pixel there (km) and the cover: see
// CloudShape::cirrusDensity(), which is the same, step for step.
// Cirrus are trails of falling ice crystals. Each one begins in a small dense tuft, where the crystals form, and trails
// away from it to one side as the crystals fall into slower air: a streak that widens, thins out and bends a little,
// combed into fine fibres along its length (cirrus uncinus, the hooks, and with many streaks side by side fibratus).
// The streaks lie in the cells of a grid, each with a tuft at a random place and a tail of a random length, width and bend
highp float cirrusDensity(highp vec2 map, highp float foot, highp float cover)
{
	if (cover <= 0.) return 0.;
	highp vec2 m = vec2(0.819*map.x - 0.574*map.y, 0.574*map.x + 0.819*map.y);
	// A weak shared bend gives a prevailing direction without combing every
	// filament into the same large wave.
	m += 2.4*(vec2(cirrusNoise(map/60. + cirrusTexturePhase[0] + 0.5, max(log2(foot*1024./60.), 0.)),
	               cirrusNoise(map/60. + cirrusTexturePhase[0] + 0.25, max(log2(foot*1024./60.), 0.))) - 0.5);
	highp float patch = cirrusNoise(map/190. + cirrusTexturePhase[1], max(log2(foot*1024./190.), 0.));
	highp float area = smoothstep(0., 1., clamp((patch - 0.85 + 0.9*cover)/0.2, 0., 1.));
	if (area <= 0.) return 0.;
	// (cells of 14 km along the streaks and 5 km across; the grid repeats after 64 cells, and the wind carries it on by
	// whole grids: cirrusTexturePhase[2])
	highp vec2 c = vec2(m.x/14., m.y/5.) + 64.*cirrusTexturePhase[2];
	highp vec2 first = floor(c);
	highp float sum = 0.;
	for (int i = -2; i <= 0; ++i)
		for (int j = -1; j <= 1; ++j)
		{
			highp vec2 cell = first + vec2(float(i), float(j));
			// (random numbers of the cell: texels of the noise of the tile, far apart)
			highp vec2 id = mod(cell, 64.);
			highp vec2 r = vec2(textureLod(cloudTile, (vec2(mod(37.*id.x + 17.*id.y, 1024.), mod(53.*id.y + 29.*id.x, 1024.)) + 0.5)/1024., 0.).z,
			                    textureLod(cloudTile, (vec2(mod(13.*id.x + 41.*id.y + 300., 1024.), mod(7.*id.y + 59.*id.x + 700., 1024.)) + 0.5)/1024., 0.).z);
			r = clamp((r - 0.25)/0.5, 0., 1.);
			highp vec2 q = fract(vec2(13.7, 29.3)*r + vec2(0.3, 0.6));
			// (fewer streaks at a low cover)
			if (q.x > 0.25 + 0.7*cover)
				continue;
			highp vec2 head = cell + 0.25 + 0.6*vec2(q.y, r.x);
			highp vec2 d = (c - head)*vec2(14., 5.);   // km from the tuft: along the streak, across it
			// (each streak a little to its own side of the common direction, up to 12 degrees)
			highp float turn = 0.42*(fract(11.3*q.x + 5.1*r.y) - 0.5);
			d = vec2(d.x + turn*d.y, d.y - turn*d.x);
			highp float tailLength = 6. + 16.*r.y;
			if (d.x < -3. || d.x > tailLength)
				continue;
			highp float along = max(d.x, 0.)/tailLength;
			// (the tail bends to one side, wanders a little on its way and widens)
			highp float wander = cirrusNoise(vec2(d.x/40., 0.37) + vec2(5.3, 2.9)*r, max(log2(foot*1024./40.), 0.));
			highp float width = (0.45 + 0.5*r.x)*(0.9 + 1.4*along);
			highp float across = d.y - 0.3*(fract(7.1*q.y) - 0.5)*tailLength*along*along - 2.2*width*(wander - 0.5)*(0.25 + along);
			highp float v = across/width;
			// (the fibres are not straight: they wave a little, each part of the tail on its own)
			v += 0.3*(cirrusNoise(vec2(d.x/7., v/5.) + vec2(4.1, 0.9)*r, max(log2(foot*1024./7.), 0.)) - 0.5);
			// (the fibres: a noise drawn out along the tail, in the coordinates of the tail, so that they spread and bend
			// with it; in three sizes: strands, the fibres in them, and the finest hairs)
			highp float strands = cirrusNoise(vec2(d.x/60., v/2.5) + vec2(3.1, 5.7)*r, max(log2(foot*1024./(2.5*width)), 0.));
			highp float fibres = 0.45*strands
			                   + 0.33*cirrusNoise(vec2(d.x/25., v/0.8) + vec2(7.3, 1.9)*r, max(log2(foot*1024./(0.8*width)), 0.))
			                   + 0.22*cirrusNoise(vec2(d.x/11., v/0.27) + vec2(1.7, 8.3)*r, max(log2(foot*1024./(0.27*width)), 0.));
			highp float combed = smoothstep(0.4, 0.6, fibres);
			// (no smooth outline: the strands end at different widths, and single fibres stand out at the sides)
			highp float side = 1. - smoothstep(0.3, 1.7, abs(v) + 1.6*(strands - 0.5));
			highp float tail = side*pow(1. - along, 1.1)*smoothstep(0., 0.04, along)*(0.3 + 0.7*(1. - along))*(0.05 + 1.25*combed);
			// (the tuft at the head, where the crystals form: the dense, woolly beginning of the streak, a little longer
			// than wide and no wider than the tail that leaves it; a ball there makes a jellyfish of the streak)
			highp float lumps = 0.6*cirrusNoise(d/5. + vec2(2.3, 6.1)*r, max(log2(foot*1024./5.), 0.))
			                  + 0.4*cirrusNoise(d/1.9 + vec2(6.7, 3.3)*r, max(log2(foot*1024./1.9), 0.));
			// (short towards the front: the streak begins there)
			highp vec2 h = vec2(d.x/(d.x < 0. ? 1. : 2.2), across/(1.15*width));
			highp float tuft = 1.5*exp(-dot(h, h))*(0.3 + 1.1*lumps);
			sum += (0.55 + 0.45*q.y)*(tuft + tail);
		}
	// Increasing cover adds patches and streaks, rather than a uniform veil. Density stays in [0,1].
	return clamp(1.5*area*sum, 0., 1.);
}
)";
}

// The actual directional atmosphere radiance, in the same linear camera units
// as the scene. Its capture contains no Sun, Moon, stars or landscape.
QByteArray atmosphereShader()
{
	return R"(
uniform sampler2D cloudSkyTex;
uniform bool cloudHasSky;
uniform highp vec4 cloudSkyUv;
highp vec3 foregroundSky(highp vec2 ndc, highp vec3 fallback)
{
	if (!cloudHasSky)
		return fallback;
	highp vec2 uv = cloudSkyUv.xy + (0.5*ndc + 0.5)*cloudSkyUv.zw;
	return max(texture2D(cloudSkyTex, uv).rgb, vec3(0.));
}
)";
}

// Keep potentially very large wind translations out of float world coordinates.
// Each repeating texture gets its own double-precision fractional phase. Resetting a
// shared map origin would not preserve textures with different scales or rotations.
QVector2D texturePhase(double x, double y)
{
	return QVector2D(static_cast<float>(std::remainder(x, 1.)), static_cast<float>(std::remainder(y, 1.)));
}

void setWindTurn(QOpenGLShaderProgram& p, const char* name, const CloudShape::Motion& motion)
{
	p.setUniformValue(name, QVector4D(static_cast<float>(motion.cosine), static_cast<float>(motion.sine), 0.f, 0.f));
}

void setLayerMotion(QOpenGLShaderProgram& p, const char* prefix, const CloudShape::Layer& layer, const CloudShape::Motion& motion)
{
	const QByteArray name(prefix);
	setWindTurn(p, (name + "Turn").constData(), motion);
	const double x = motion.offsetX, y = motion.offsetY;
	const double fine = 1./(layer.size*CloudShape::FineScale);
	const auto formation = CloudShape::formationParameters(layer);
	p.setUniformValue((name + "Formation").constData(), QVector4D(static_cast<float>(formation[0]), static_cast<float>(formation[1]),
	                                                            static_cast<float>(formation[2]), static_cast<float>(formation[3])));
	const QVector2D phases[] = {
		texturePhase(x*layer.stretch()/(layer.size*layer.bodyScale()*CloudShape::TileSize), y/(layer.size*layer.bodyScale()*CloudShape::TileSize)),
		texturePhase(x/CloudShape::CoverScaleLarge, y/CloudShape::CoverScaleLarge),
		texturePhase(x/CloudShape::CoverScaleSmall, y/CloudShape::CoverScaleSmall),
		texturePhase(x/CloudShape::GrowScale, y/CloudShape::GrowScale),
		texturePhase(x*formation[0], y*formation[1]),
		texturePhase(x*fine, y*fine), texturePhase(x*fine*2.7, y*fine*2.7),
		texturePhase(x*fine*0.37, y*fine*0.37),
		texturePhase(x/CloudShape::DeformScale, y/CloudShape::DeformScale),
		texturePhase(x*formation[0]*2.3, y*formation[1]*2.3)
	};
	p.setUniformValueArray((name + "Phase").constData(), phases, 10);
	// (the 3D billows move with the clouds, or the clouds would slide through them)
	if (name == "cloud")
		p.setUniformValue("cloudBillowPhase", texturePhase(x/layer.billowTile(), y/layer.billowTile()));
}

void setFrontMotion(QOpenGLShaderProgram& p, const CloudShape::Motion& motion)
{
	setWindTurn(p, "cloudFrontTurn", motion);
	const double x = motion.offsetX, y = motion.offsetY;
	const QVector2D phases[] = {
		texturePhase(x/CloudShape::FrontBendScale, y/CloudShape::FrontBendScale),
		texturePhase((CloudShape::FrontAxisCos*x - CloudShape::FrontAxisSin*y)/CloudShape::FrontAlong,
		             (CloudShape::FrontAxisSin*x + CloudShape::FrontAxisCos*y)/CloudShape::FrontAcross)
	};
	p.setUniformValueArray("frontPhase", phases, 2);
}

void setCirrusMotion(QOpenGLShaderProgram& p, const char* turnName, const CloudShape::Motion& motion)
{
	setWindTurn(p, turnName, motion);
	const double x = motion.offsetX, y = motion.offsetY;
	const double u = CloudShape::CirrusAxisCos*x - CloudShape::CirrusAxisSin*y;
	const double v = CloudShape::CirrusAxisSin*x + CloudShape::CirrusAxisCos*y;
	const QVector2D phases[] = {
		texturePhase(x/CloudShape::CirrusBendScale, y/CloudShape::CirrusBendScale),
		texturePhase(x/CloudShape::CirrusPatchScale, y/CloudShape::CirrusPatchScale),
		// (the grid of the streaks, in whole grids: see cirrusDensity())
		texturePhase(u/(CloudShape::CirrusCellX*CloudShape::CirrusCells), v/(CloudShape::CirrusCellY*CloudShape::CirrusCells)),
		QVector2D(0.f, 0.f),   // (two free places)
		QVector2D(0.f, 0.f)
	};
	p.setUniformValueArray("cirrusTexturePhase", phases, 5);
}

QVector4D frontUniform(const CloudShape::Front& front)
{
	return QVector4D(front.on ? 1.f : 0.f, static_cast<float>(front.shift), static_cast<float>(front.bias), static_cast<float>(front.scale));
}
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
	setFlagFollowDate(conf->value("clouds/flag_follow_date", false).toBool());
	// The first layer: cumulus by default; the second one: altocumulus, off
	layers[1].shift = 2357.;
	struct Defaults {double cover, size, base, density, flatness; bool on;};
	const Defaults defaults[LayerCount] = {{0.2, 1., 1.5, 1., 0., true}, {0.4, 0.4, 3.5, 0.6, 0.6, false}};
	for (edited = LayerCount - 1; edited >= 0; --edited)
	{
		const Defaults& d = defaults[edited];
		setFlagLayerOn(conf->value(settingKey("flag_on"), d.on).toBool());
		setCover(conf->value(settingKey("cover"), d.cover).toDouble());
		setSize(conf->value(settingKey("size"), d.size).toDouble());
		setBaseHeight(conf->value(settingKey("base_height"), d.base).toDouble());
		setDensity(conf->value(settingKey("density"), d.density).toDouble());
		setFlatness(conf->value(settingKey("flatness"), d.flatness).toDouble());
		setBankAzimuth(conf->value(settingKey("bank_azimuth"), 270.0).toDouble());
		setBankWidth(conf->value(settingKey("bank_width"), 90.0).toDouble());
		setBankDistance(conf->value(settingKey("bank_distance"), 60.0).toDouble());
		setBankDepth(conf->value(settingKey("bank_depth"), 30.0).toDouble());
		setFlagBank(conf->value(settingKey("flag_bank"), false).toBool());
		setWindSpeed(conf->value(settingKey("wind_speed"), edited == 0 ? 8. : 12.).toDouble());
		setWindDirection(conf->value(settingKey("wind_direction"), edited == 0 ? 250. : 260.).toDouble());
	}
	edited = 0;
	setFlagHalfResolution(conf->value("clouds/flag_half_resolution", true).toBool());
	// (no setting in the dialog yet: 2, 3 or 4 in the configuration file)
	lowResDivisor = std::clamp(conf->value("clouds/low_resolution_divisor", 2).toInt(), 2, 4);
	setFlagCirrus(conf->value("clouds/flag_cirrus", false).toBool());
	setFlagManualWind(conf->value("clouds/flag_manual_wind", false).toBool());
	setCirrusCover(conf->value("clouds/cirrus_cover", 0.4).toDouble());
	setCirrusHeight(conf->value("clouds/cirrus_height", 8.).toDouble());
	setCirrusOpticalDepth(conf->value("clouds/cirrus_optical_depth", 1.0).toDouble());
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
	if (!qFuzzyCompare(value, layers[edited].cover))
	{
		layers[edited].cover = value;
		StelApp::immediateSave(settingKey("cover"), value);
		emit coverChanged(value);
		emit typeChanged(getType());
	}
}

void CloudMgr::setFlagManualWind(bool b)
{
	if (b != flagManualWind)
	{
		flagManualWind = b;
		StelApp::immediateSave("clouds/flag_manual_wind", b);
		emit flagManualWindChanged(b);
	}
}

void CloudMgr::setWindSpeed(double value)
{
	value = std::clamp(value, 0., 60.);
	if (!qFuzzyCompare(value, windSpeeds[edited]))
	{
		windSpeeds[edited] = value;
		StelApp::immediateSave(settingKey("wind_speed"), value);
		emit windSpeedChanged(value);
	}
}

void CloudMgr::setWindDirection(double value)
{
	value = std::fmod(std::fmod(value, 360.) + 360., 360.);
	if (!qFuzzyCompare(value + 1., windDirections[edited] + 1.))
	{
		windDirections[edited] = value;
		StelApp::immediateSave(settingKey("wind_direction"), value);
		emit windDirectionChanged(value);
	}
}

void CloudMgr::setFlagCirrus(bool b)
{
	if (b != flagCirrus)
	{
		flagCirrus = b;
		StelApp::immediateSave("clouds/flag_cirrus", b);
		emit flagCirrusChanged(b);
	}
}

void CloudMgr::setCirrusCover(double value)
{
	value = std::clamp(value, 0.0, 1.0);
	if (!qFuzzyCompare(value, cirrusCover))
	{
		cirrusCover = value;
		StelApp::immediateSave("clouds/cirrus_cover", value);
		emit cirrusCoverChanged(value);
	}
}

void CloudMgr::setCirrusHeight(double value)
{
	value = std::clamp(value, 5.0, 13.0);
	if (!qFuzzyCompare(value, cirrusHeight))
	{
		cirrusHeight = value;
		StelApp::immediateSave("clouds/cirrus_height", value);
		emit cirrusHeightChanged(value);
	}
}

void CloudMgr::setCirrusOpticalDepth(double value)
{
	value = std::clamp(value, 0.02, 3.0);
	if (!qFuzzyCompare(value, cirrusOpticalDepth))
	{
		cirrusOpticalDepth = value;
		StelApp::immediateSave("clouds/cirrus_optical_depth", value);
		emit cirrusOpticalDepthChanged(value);
	}
}

void CloudMgr::setFlagHalfResolution(bool b)
{
	if (b != flagHalfResolution)
	{
		flagHalfResolution = b;
		StelApp::immediateSave("clouds/flag_half_resolution", b);
		emit flagHalfResolutionChanged(b);
	}
}

void CloudMgr::setBaseHeight(double value)
{
	value = std::clamp(value, 0.3, 9.0);
	if (!qFuzzyCompare(value, layers[edited].base))
	{
		layers[edited].base = value;
		StelApp::immediateSave(settingKey("base_height"), value);
		emit baseHeightChanged(value);
		emit typeChanged(getType());
	}
}

void CloudMgr::setDensity(double value)
{
	value = std::clamp(value, 0.05, 1.0);
	if (!qFuzzyCompare(value, layers[edited].density))
	{
		layers[edited].density = value;
		StelApp::immediateSave(settingKey("density"), value);
		emit densityChanged(value);
		emit typeChanged(getType());
	}
}

void CloudMgr::setFlatness(double value)
{
	value = std::clamp(value, 0.0, 1.0);
	if (!qFuzzyCompare(value, layers[edited].flatness))
	{
		layers[edited].flatness = value;
		StelApp::immediateSave(settingKey("flatness"), value);
		emit flatnessChanged(value);
		emit typeChanged(getType());
	}
}

void CloudMgr::setBankAzimuth(double value)
{
	value = std::clamp(value, 0.0, 360.0);
	if (!qFuzzyCompare(value, layers[edited].bankAzimuth))
	{
		layers[edited].bankAzimuth = value;
		StelApp::immediateSave(settingKey("bank_azimuth"), value);
		emit bankAzimuthChanged(value);
	}
}

void CloudMgr::setBankWidth(double value)
{
	value = std::clamp(value, 5.0, 360.0);
	if (!qFuzzyCompare(value, layers[edited].bankWidth))
	{
		layers[edited].bankWidth = value;
		StelApp::immediateSave(settingKey("bank_width"), value);
		emit bankWidthChanged(value);
	}
}

void CloudMgr::setBankDistance(double value)
{
	value = std::clamp(value, 0.0, 300.0);
	if (!qFuzzyCompare(value, layers[edited].bankDistance))
	{
		layers[edited].bankDistance = value;
		StelApp::immediateSave(settingKey("bank_distance"), value);
		emit bankDistanceChanged(value);
	}
}

void CloudMgr::setBankDepth(double value)
{
	value = std::clamp(value, 2.0, 300.0);
	if (!qFuzzyCompare(value, layers[edited].bankDepth))
	{
		layers[edited].bankDepth = value;
		StelApp::immediateSave(settingKey("bank_depth"), value);
		emit bankDepthChanged(value);
	}
}

void CloudMgr::setFlagBank(bool b)
{
	if (b != layers[edited].bank)
	{
		layers[edited].bank = b;
		StelApp::immediateSave(settingKey("flag_bank"), b);
		emit flagBankChanged(b);
	}
}

namespace
{
// The types of clouds: base (km), size, density, flatness, cover
struct CloudType
{
	double base, size, density, flatness, cover;
};
constexpr CloudType CloudTypes[] = {
	{1.2, 0.6, 1., 0., 0.3},     // cumulus humilis: fair weather heaps, white in the sunlight, most under 1 km across
	{1.0, 1.2, 0.9, 0.4, 0.6},   // stratocumulus: large flat masses close together, grey below
	{3.5, 0.4, 0.3, 0.6, 0.4},   // altocumulus: sheets of small, thin cloudlets higher up, with open sky between the sheets
	{3.5, 0.7, 0.35, 0.1, 0.15}, // altocumulus floccus: a few ragged, fuzzy tufts higher up
};
}

int CloudMgr::getType() const
{
	const auto near = [](double a, double b) {return std::fabs(a - b) < 0.005;};
	for (int i = 0; i < 4; ++i)
	{
		const CloudType& t = CloudTypes[i];
		if (near(layers[edited].base, t.base) && near(layers[edited].size, t.size) && near(layers[edited].density, t.density) && near(layers[edited].flatness, t.flatness))
			return i + 1;
	}
	return TypeCustom;
}

void CloudMgr::setType(int type)
{
	if (type < TypeCumulus || type > TypeFloccus || type == getType())
		return;
	const CloudType& t = CloudTypes[type - 1];
	setBaseHeight(t.base);
	setSize(t.size);
	setDensity(t.density);
	setFlatness(t.flatness);
	setCover(t.cover);
}

CloudShape::Layer CloudMgr::currentLayer(int i) const
{
	CloudShape::Layer l = layers[i];
	l.cover = currentCovers[i];
	l.front = fronts[i];
	return l;
}

QString CloudMgr::settingKey(const char* name) const
{
	return (edited == 0 ? QStringLiteral("clouds/") : QStringLiteral("clouds/layer2_")) + QLatin1String(name);
}

void CloudMgr::setLayer(int layer)
{
	const int i = std::clamp(layer, 1, LayerCount) - 1;
	if (i != edited)
	{
		edited = i;
		emit layerChanged(i + 1);
		emitLayerSettings();
	}
}

void CloudMgr::emitLayerSettings()
{
	const CloudShape::Layer& l = layers[edited];
	emit flagLayerOnChanged(layerOn[edited]);
	emit coverChanged(l.cover);
	emit sizeChanged(l.size);
	emit baseHeightChanged(l.base);
	emit densityChanged(l.density);
	emit flatnessChanged(l.flatness);
	emit bankAzimuthChanged(l.bankAzimuth);
	emit bankWidthChanged(l.bankWidth);
	emit bankDistanceChanged(l.bankDistance);
	emit bankDepthChanged(l.bankDepth);
	emit flagBankChanged(l.bank);
	emit windSpeedChanged(windSpeeds[edited]);
	emit windDirectionChanged(windDirections[edited]);
	emit typeChanged(getType());
}

void CloudMgr::setFlagLayerOn(bool b)
{
	if (b != layerOn[edited])
	{
		layerOn[edited] = b;
		StelApp::immediateSave(settingKey("flag_on"), b);
		emit flagLayerOnChanged(b);
	}
}

void CloudMgr::setFlagFollowDate(bool b)
{
	if (b != flagFollowDate)
	{
		flagFollowDate = b;
		StelApp::immediateSave("clouds/flag_follow_date", b);
		emit flagFollowDateChanged(b);
	}
}

double CloudMgr::windHeight(int wind) const
{
	return wind == CirrusWind ? cirrusHeight : layers[wind].base;
}

CloudShape::Motion CloudMgr::motionAt(const StelCore* core, double jd, int wind) const
{
	// (against the wind of the first layer)
	const double faster = CloudShape::windAtHeight(windHeight(wind))/CloudShape::windAtHeight(layers[0].base);
	CloudShape::Motion m = CloudShape::motion(jd);
	if (!flagManualWind)
	{
		if (qFuzzyCompare(faster, 1.))
			return m;
		// Reference the bounded automatic motion to a fixed epoch, not a resetting ten-day
		// window. The height correction then remains continuous when time is scrubbed.
		const StelLocation& location = core->getCurrentLocation();
		const double phi = static_cast<double>(location.getLatitude())*M_PI/180.;
		const double lambda = static_cast<double>(location.getLongitude())*M_PI/180.;
		const double radius = 2.*(EarthRadius + windHeight(wind))*std::cos(phi)/(1. + std::fabs(std::sin(phi)));
		const double x = radius*std::cos(lambda), y = radius*std::sin(lambda);
		const double turn = -CloudShape::WindTurn*(windHeight(wind) - layers[0].base);
		return CloudShape::heightMotion(jd, x, y, faster, turn);
	}
	// Constant manual wind from a fixed epoch. Texture phases are reduced in double
	// precision before upload, so long journeys do not lose the small cloud details.
	const StelLocation& location = core->getCurrentLocation();
	const double phi = static_cast<double>(location.getLatitude())*M_PI/180.;
	// (the cirrus go with the wind of the first layer, faster and turned by their height)
	const double windSpeed = wind == CirrusWind ? windSpeeds[0]*faster : windSpeeds[wind];
	const double windDirection = wind == CirrusWind ? windDirections[0] + (phi >= 0. ? 1. : -1.)*CloudShape::WindTurn*180./M_PI*(cirrusHeight - layers[0].base)
	                                                : windDirections[wind];
	const double lambda = static_cast<double>(location.getLongitude())*M_PI/180.;
	const double towards = (windDirection + 180.)*M_PI/180.;
	const double east = 0.001*windSpeed*std::sin(towards), north = 0.001*windSpeed*std::cos(towards);  // km/s
	// on the map: km there are larger by 2/(1 + |sin(latitude)|), and the north points to its center (or away from it in the
	// south)
	const double k = 2./(1. + std::fabs(std::sin(phi)));
	const double pole = phi >= 0. ? -1. : 1.;
	const double vx = k*(-std::sin(lambda)*east + pole*std::cos(lambda)*north);
	const double vy = k*(std::cos(lambda)*east + pole*std::sin(lambda)*north);
	return CloudShape::manualMotion(jd, vx, vy);
}

void CloudMgr::computeWind(const StelCore* core, int wind, double* mapX, double* mapY) const
{
	const StelLocation& location = core->getCurrentLocation();
	const double phi = static_cast<double>(location.getLatitude())*M_PI/180.;
	const double lambda = static_cast<double>(location.getLongitude())*M_PI/180.;
	const double radius = 2.*(EarthRadius + windHeight(wind))*std::cos(phi)/(1. + std::fabs(std::sin(phi)));
	const double x = radius*std::cos(lambda), y = radius*std::sin(lambda);
	const double jd = core->getJD(), step = 60.;
	const CloudShape::Motion a = motionAt(core, jd, wind), b = motionAt(core, jd + step/86400., wind);
	*mapX = ((b.cosine - a.cosine)*x - (b.sine - a.sine)*y + b.offsetX - a.offsetX)/step;
	*mapY = ((b.sine - a.sine)*x + (b.cosine - a.cosine)*y + b.offsetY - a.offsetY)/step;
}

void CloudMgr::update(double)
{
	if (!flagShow)
		return;
	const StelCore* core = StelApp::getInstance().getCore();
	const double longitude = static_cast<double>(core->getCurrentLocation().getLongitude());
	// Recent solar heating affects cumulus most. Only the automatic weather mode uses
	// this modulation; manually selected coverage remains under the user's control.
	double heating = 0.;
	if (flagFollowDate)
	{
		const SolarSystem* ssystem = GETSTELMODULE(SolarSystem);
		Vec3d sun = ssystem->getSun()->getAltAzPosGeometric(core);
		sun.normalize();
		heating = CloudShape::solarHeating(core->getCurrentLocation().getLatitude()*M_PI/180., {sun[0], sun[1], sun[2]});
	}
	// The higher clouds of a front precede the lower layers.
	for (int i = 0; i < LayerCount; ++i)
	{
		fronts[i].on = flagFollowDate;
		fronts[i].shift = layers[i].base > 2.5 ? 180. : 0.;
		fronts[i].bias = layers[i].base > 2.5 ? -0.1 : 0.;
		fronts[i].scale = CloudShape::weatherCoverScale(layers[i], heating);
	}
	cirrusFront.on = flagFollowDate;
	cirrusFront.shift = 420.;
	cirrusFront.bias = 0.1;
	cirrusFront.scale = 1.;
	// the cover over the observer
	if (flagFollowDate && tile.empty())
		tile = CloudShape::computeTile();
	const CloudShape::Motion here = motionAt(core, core->getJD(), 0);
	const double latitude = static_cast<double>(core->getCurrentLocation().getLatitude())*M_PI/180.;
	const double lambdaHere = longitude*M_PI/180.;
	const auto coverHere = [&](const CloudShape::Front& front, double height) {
		// the place of the map over the observer, at the height of a layer
		const double r = EarthRadius + height;
		const double ex = r*std::cos(latitude)*std::cos(lambdaHere), ey = r*std::cos(latitude)*std::sin(lambdaHere);
		const double scale = 2.*r/(r + std::fabs(r*std::sin(latitude)));
		return CloudShape::frontCover(tile, here.cosine*scale*ex - here.sine*scale*ey + here.offsetX, here.sine*scale*ex + here.cosine*scale*ey + here.offsetY,
		                              front, here);
	};
	for (int i = 0; i < LayerCount; ++i)
		currentCovers[i] = flagFollowDate ? coverHere(fronts[i], layers[i].base) : layers[i].cover;
	currentCirrusCover = flagFollowDate ? coverHere(cirrusFront, cirrusHeight) : cirrusCover;

	// The winds over the Earth from the winds on the map: turn them back, into the frame of the observer's meridian
	static const QStringList points = {q_("N"), q_("NE"), q_("E"), q_("SE"), q_("S"), q_("SW"), q_("W"), q_("NW")};
	QString winds[LayerCount];
	for (int i = 0; i < LayerCount; ++i)
	{
		double mapX = 0., mapY = 0.;
		computeWind(core, i, &mapX, &mapY);
		const CloudShape::Motion m = motionAt(core, core->getJD(), i);
		const double earthX = m.cosine*mapX + m.sine*mapY, earthY = -m.sine*mapX + m.cosine*mapY;
		const double lambda = longitude*M_PI/180.;
		const double north = static_cast<double>(core->getCurrentLocation().getLatitude()) >= 0. ? -1. : 1.;
		// (the map moves as the clouds do, so the wind is the opposite of the place on the map that passes over the observer)
		// (km on the map are larger than on the Earth away from the pole)
		const double scale = (1. + std::fabs(std::sin(static_cast<double>(core->getCurrentLocation().getLatitude())*M_PI/180.)))/2.;
		windEast[i] = -scale*(-std::sin(lambda)*earthX + std::cos(lambda)*earthY);
		windNorth[i] = -scale*north*(std::cos(lambda)*earthX + std::sin(lambda)*earthY);
		const double from = std::atan2(-windEast[i], -windNorth[i])*180./M_PI;
		const int point = static_cast<int>(std::lround((from < 0. ? from + 360. : from)/45.)) % 8;
		winds[i] = q_("%1 m/s from %2").arg(std::hypot(windEast[i], windNorth[i])*1000., 0, 'f', 0).arg(points[point]);
	}
	QString covers = QString::number(currentCovers[0], 'f', 2), wind = winds[0];
	if (layerOn[1])
	{
		covers = layerOn[0] ? q_("%1 and %2").arg(covers, QString::number(currentCovers[1], 'f', 2)) : QString::number(currentCovers[1], 'f', 2);
		wind = layerOn[0] ? q_("%1 and %2").arg(wind, winds[1]) : winds[1];
	}
	const QString info = q_("Cover %1, wind %2. Made up, not a forecast.").arg(covers, wind);
	if (info != weatherInfo)
	{
		weatherInfo = info;
		emit weatherInfoChanged(info);
	}
}

void CloudMgr::setSize(double value)
{
	value = std::clamp(value, 0.4, 2.5);
	if (!qFuzzyCompare(value, layers[edited].size))
	{
		layers[edited].size = value;
		StelApp::immediateSave(settingKey("size"), value);
		emit sizeChanged(value);
		emit typeChanged(getType());
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
		gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, CloudShape::TileTexels, CloudShape::TileTexels, 0, GL_RGBA, GL_FLOAT, tile.data());
		gl.glGenerateMipmap(GL_TEXTURE_2D);
		gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
		gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
		gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	}
	gl.glBindTexture(GL_TEXTURE_2D, tileTexture);
	gl.glActiveTexture(GL_TEXTURE0);
}

void CloudMgr::bindBillowTexture(int unit)
{
	auto& gl = *QOpenGLContext::currentContext()->extraFunctions();
	gl.glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(unit));
	if (!billowTexture)
	{
		const std::vector<unsigned char> billows = CloudShape::computeBillows();
		gl.glGenTextures(1, &billowTexture);
		gl.glBindTexture(GL_TEXTURE_3D, billowTexture);
		gl.glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA8, CloudShape::BillowTexels, CloudShape::BillowTexels, CloudShape::BillowTexels, 0,
		                GL_RGBA, GL_UNSIGNED_BYTE, billows.data());
		gl.glGenerateMipmap(GL_TEXTURE_3D);
		gl.glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
		gl.glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		gl.glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_REPEAT);
		gl.glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_REPEAT);
		gl.glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_REPEAT);
	}
	gl.glBindTexture(GL_TEXTURE_3D, billowTexture);
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
	                        + frontShader()
	                        + cirrusShader()
	                        + atmosphereShader()
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
uniform highp vec3 cloudCity;
uniform highp vec3 sideSunLog[5];   // the logarithm of the light of CloudLight::Light::sideSun
uniform highp vec2 sideSunOffset;   // the first offset of its altitude and its step (degrees)
uniform highp vec3 sideMoon;
uniform highp vec3 silverSun;
uniform highp vec3 silverMoon;
uniform highp vec3 airSky;
uniform highp vec3 airMoon;
uniform highp vec3 airExtinction;   // per km
const int CloudSubsteps = 12;
uniform highp vec4 cloudTileScale;  // 1/km: of the tile, of its two cover sizes, of the fine lobes
uniform highp float cloudSize;      // the scale of the clouds against those of the tile
uniform highp vec4 cloudTurn;       // the wind: the turn of the map, as cosine and sine; its offset (km)
uniform highp vec4 cloudFormation;
uniform highp vec4 otherFormation;
uniform highp vec2 cloudPhase[10];
uniform highp vec2 otherPhase[10];
uniform highp vec2 cloudDeform;
uniform highp float cloudDeformScale;
uniform highp vec2 cloudGrow;       // the place of the noise of growth (tiles)
uniform highp sampler3D cloudBillow; // the billows of the cauliflower tops (see CloudShape::computeBillows())
uniform highp vec3 cloudBillowScale; // 1/km of their tile, how deep they eat in (km), their change (in tiles)
uniform highp vec2 cloudBillowPhase; // where the wind has carried them (in tiles; see setLayerMotion())
uniform highp float cloudGrowScale; // 1/km
uniform highp float cloudGrowStrength;
uniform highp vec2 cloudSmear;      // what the wind moves the map during the exposure (km)
uniform int cloudSamples;           // moments of the exposure to average
uniform highp vec4 cloudShape;      // the stretch of the map, the flatness, the density, the scale of the heights
uniform highp vec4 cloudBank;       // the bank: its azimuth and half width (radians), its near and far edge (km; the far
                                    // edge below 0 without a bank)
uniform highp vec4 cloudCover;      // cover offset: constant, per noise; the mean size of a texel of the tile (km); the
                                    // extinction in the clouds (per km)
uniform highp vec4 cloudFront;      // its fronts (see frontCover())
uniform highp vec2 cloudShift;      // where the clouds of this layer lie on the map (km)
uniform highp vec3 cloudRadius;     // (the same as cloudRadii.y, cloudSize and cloudTileScale.x)
// The same for the layer above, which casts its shadows on this one (otherRadius.x = 0: none)
uniform highp vec4 otherShape;
uniform highp vec4 otherBank;
uniform highp vec4 otherCover;
uniform highp vec4 otherFront;
uniform highp vec2 otherShift;
uniform highp vec3 otherRadius;
uniform highp vec4 otherTurn;
// The cirrus, which also cast shadows on the clouds below
uniform highp vec4 cirrusShade;     // their optical depth at the densest (0: none), the radius of their layer, their cover
uniform highp vec4 cirrusTurn;      // where the wind has carried them (as cloudTurn)
uniform highp vec4 cirrusFront;     // their fronts (see frontCover())

// The place on the map: a polar stereographic projection, in km, of each hemisphere
highp vec2 cloudMoment = vec2(0., 0.);    // where the clouds are at this moment of the exposure, against its middle

// (before the wind)
highp vec2 stillMap(highp vec3 p, highp float radius)
{
	highp vec3 earth = p.x*cloudSouth + p.y*cloudEast + p.z*cloudUp;
	return 2.*radius*earth.xy/(length(earth) + abs(earth.z));
}

highp vec2 cloudMapAt(highp vec3 p, highp float radius, highp vec2 shift, highp vec4 turn)
{
	highp vec2 map = stillMap(p, radius);
	return vec2(turn.x*map.x - turn.y*map.y, turn.y*map.x + turn.x*map.y) + turn.zw + shift + cloudMoment;
}

highp vec2 cloudMap(highp vec3 p)
{
	return cloudMapAt(p, cloudRadii.y, cloudShift, cloudTurn);
}

// The light that the cirrus let through to a place, towards the light key (a soft shadow)
highp float cirrusShadow(highp vec3 p, highp vec3 key)
{
	highp float r = length(p);
	if (cirrusShade.x <= 0. || r > cirrusShade.y || key.z < -0.03)
		return 1.;
	highp vec3 q = p + key*(cirrusShade.y - r)/max(dot(p/r, key), 0.03);
	highp vec2 still = stillMap(q, cirrusShade.y);
	highp vec2 map = vec2(cirrusTurn.x*still.x - cirrusTurn.y*still.y, cirrusTurn.y*still.x + cirrusTurn.x*still.y) + cirrusTurn.zw + vec2(4711.);
	highp float cover = cirrusFront.x > 0. ? frontCover(frontMap(still), cirrusFront) : cirrusShade.z;
	highp float slant = 1./max(dot(q/length(q), key), 0.05);
	return exp(-cirrusShade.x*cirrusDensity(map, 0.4, cover)*slant);
}

// Development and broad deformation are sampled only at the coarse step endpoints.
// Fine ray samples interpolate them, keeping the extra texture work out of that loop.
struct CloudWeather
{
	highp vec3 growth; // coverage offset, height and density multipliers
	highp vec2 drift;  // smooth horizontal displacement in km
};
CloudWeather mixWeather(CloudWeather a, CloudWeather b, highp float t)
{
	return CloudWeather(mix(a.growth, b.growth, t), mix(a.drift, b.drift, t));
}

// What the cover and the growth add to the shape around a place p with its place on the map, with the cover offset of a
// layer, its fronts, its radius and its bank. It changes slowly: one value serves a whole step of the way. It never fills
// the clear parts of the map
CloudWeather cloudWeatherAt(highp vec3 p, highp vec2 map, highp vec4 cover, highp vec4 front, highp float radius, highp vec4 bank, highp vec4 shape, highp float size, highp vec4 formation, highp vec2 phases[10])
{
	highp float noise = 0.6*textureLod(cloudTile, map*cloudTileScale.y + phases[1], 2.).z + 0.4*textureLod(cloudTile, map*cloudTileScale.z + phases[2], 2.).z;
	highp vec2 growthMap = map*cloudGrowScale + phases[3];
	highp float grow = 0.7*textureLod(cloudTile, growthMap + cloudGrow, 2.).z
	                 + 0.3*textureLod(cloudTile, growthMap + vec2(-cloudGrow.y, cloudGrow.x) + vec2(0.37, 0.19), 2.).z;
	// Match CloudShape::development(): slow bank evolution in the moving air mass.
	highp float lowFormation = 1. - smoothstep(0.4, 0.6, shape.y);
	highp vec2 groupDrift = 0.045*lowFormation*(1. - 0.5*shape.y)*sin(6.28318530718*cloudDeform + vec2(0., 1.1));
	highp vec2 groupUV = map*formation.xy + phases[4] + groupDrift;
	highp float broad = textureLod(cloudTile, groupUV, 2.).z;
	highp float detail = textureLod(cloudTile, map*formation.xy*2.3 + phases[9] + 2.3*groupDrift + vec2(0.31, 0.57), 2.).z;
	highp float patches = 0.75*broad + 0.25*detail;
	highp float coordinated = mix(grow, patches, 0.1*lowFormation);
	highp float life = mix(smoothstep(0.25, 0.75, coordinated), 0.5, 0.65*shape.y);
	highp float bands = 0.5 + 0.5*cos(6.28318530718*(5.*fract(groupUV.y) + 0.8*(broad - 0.5) + 0.35*(detail - 0.5)));
	highp float group = mix(patches, bands, formation.w*smoothstep(0.25, 0.75, broad));
	highp float offset = cover.x + cover.y*noise;
	// (with fronts, the cover of the place: see CloudShape::coverOffset())
	if (front.x > 0.)
	{
		highp float c = frontCover(frontMap(stillMap(p, radius)), front);
		highp float fading = max(0.1 - c, 0.);
		offset = 1.1*(c - 0.45) + 2.*(noise - 0.5) - 150.*fading*fading;
	}
	highp float presence = smoothstep(-2.995, -1.385, offset - 2.*noise);
	// Automatic fair-weather cumulus occupy selected regions of the bank, not
	// every seed at a reduced radius. The stable group field travels with the
	// bodies; a soft boundary avoids popping as the weather moves.
	highp float population = front.x > 0. ? (1. - smoothstep(0.15, 0.4, shape.y))
		* (1. - smoothstep(6373.5, 6374.5, radius)) : 0.;
	presence *= mix(1., smoothstep(0.45, 0.57, patches), population);
	offset = min(offset + formation.z*(group - 0.5) + cloudGrowStrength*(life - 0.5), 0.45);
	// A bank: the clouds only stand in a sector of the sky at a distance from the observer (see CloudShape::bankMask())
	if (bank.w >= 0.)
	{
		highp float distance = length(p.xy);
		highp float soft = 1. + 0.15*(bank.w - bank.z);
		highp float m = smoothstep(bank.z - soft, bank.z + soft, distance)*(1. - smoothstep(bank.w - soft, bank.w + soft, distance));
		if (bank.y < 3.14159)
		{
			highp float away = abs(mod(atan(p.y, -p.x) - bank.x + 3.14159265, 6.2831853) - 3.14159265);
			highp float softAngle = 0.035 + 0.1*bank.y;
			m *= 1. - smoothstep(bank.y - softAngle, bank.y + softAngle, away);
		}
		offset += 2.5*(m - 1.);
	}
	highp vec2 uv = map*cloudDeformScale + phases[8] + cloudDeform;
	// (1.6: CloudShape::DeformStrength)
	highp vec2 drift = 1.6*size*(1. - 0.4*shape.y)*
		(vec2(textureLod(cloudTile, uv, 2.).z, textureLod(cloudTile, uv + vec2(0.37, 0.71), 2.).z) - 0.5);
	// At a low cover the cumulus get fewer and smaller, down to a few percent of the sky, and keep their density: thin,
	// half transparent clouds let the eye far in and look fibrous. Only the last few dissolve by thinning. Flat layers
	// retain their more broken structure. See cohesiveWeather().
	highp float coherence = 1. - smoothstep(0.15, 0.5, shape.y);
	highp float fade = smoothstep(-1.25, -0.85, offset);
	// Match CloudShape::development(): coherent variation from flatter elements
	// to deeper rolls, with substantial bodies rather than uniformly thin wisps.
	highp float stratiform = smoothstep(0.15, 0.35, shape.y)*(1. - smoothstep(0.45, 0.6, shape.y));
	highp float relief = smoothstep(0.2, 0.8, 0.65*detail + 0.35*life);
	highp float height = 0.65 + 0.35*mix(life, relief, stratiform);
	highp float density = (0.45 + 0.55*life + 0.2*stratiform*(1. - life))*mix(1., fade, coherence)*presence;
	offset = mix(offset, max(offset, -0.85), coherence);
	return CloudWeather(vec3(offset, height, density), drift);
}

CloudWeather cloudWeather(highp vec3 p)
{
	return cloudWeatherAt(p, cloudMap(p), cloudCover, cloudFront, cloudRadii.y, cloudBank, cloudShape, cloudSize, cloudFormation, cloudPhase);
}

// The bank skeleton shares the texture with the other shapes, but excludes
// independent cloudlets. Broad depth follows the bank, not each ellipsoidal seed.
// How much a layer is altocumulus, from the height of its base over the ground (km) and its flatness (as in
// CloudShape::formationParameters())
highp float cloudAlto(highp float base, highp float flatness)
{
	return smoothstep(2., 3.5, base)*smoothstep(0.3, 0.6, flatness);
}

highp vec4 cloudBodySample(highp vec4 f, highp float flatness, highp float base)
{
	// (altocumulus is a sheet some km across, which the weather alone lays out; the shader breaks it up in cloudlets)
	// (0.6: CloudShape::AltoSheet)
	f.x = mix(f.x, 0.6, cloudAlto(base, flatness));
	highp float bank = smoothstep(0.15, 0.35, flatness)*(1. - smoothstep(0.45, 0.6, flatness));
	// (only partly the continuous bank, CloudShape::BankShare: the masses of a stratocumulus keep outlines and gaps of their own)
	f.x = mix(f.x, f.w, 0.4*bank);
	f.y = mix(f.y, 0.44 + 0.12*f.z, 0.95*bank);
	return f;
}

// Use the same shared top profile for view rays and upper-layer shadows.
highp float cloudProfileFlatness(highp float flatness)
{
	highp float bank = smoothstep(0.15, 0.35, flatness)*(1. - smoothstep(0.45, 0.6, flatness));
	return mix(flatness, 0.95, bank);
}

// The bank shape is an outline distance, not a separate dome at each seed.
// Reach its shared top over a narrow shoulder, leaving 3D billows as relief.
highp float cloudPlateauWidth(highp float flatness)
{
	highp float bank = smoothstep(0.15, 0.35, flatness)*(1. - smoothstep(0.45, 0.6, flatness));
	return mix(0.0625, 0.01, bank);
}

// Filter the upper layer's transmission over the receiving footprint. Filtering
// shape alone and exponentiating afterwards makes dense edges almost binary.
highp float otherShadow(highp vec3 p, highp vec3 key, highp float foot)
{
	if (otherRadius.x <= 0. || key.z < 0.02)
		return 1.;
	highp float middle = 0.12*otherRadius.y*otherShape.w;
	highp float r = length(p);
	// For intersecting layers start at the receiver, never project backwards.
	highp float target = max(r, otherRadius.x + middle);
	highp float b = dot(p, key), c = (target - r)*(target + r);
	highp float distance = c/(b + sqrt(b*b + c));
	highp vec3 q = p + key*distance;
	highp vec2 map = cloudMapAt(q, otherRadius.x, otherShift, otherTurn);
	CloudWeather weather = cloudWeatherAt(q, map, otherCover, otherFront, otherRadius.x, otherBank, otherShape, otherRadius.y, otherFormation, otherPhase);
	highp float slant = 1./max(dot(q/length(q), key), 0.05);
	// Approximate the solar/lunar angular radius (0.27 degrees), the pixel
	// footprint, and unresolved soft cloud edges. Bound the grazing footprint.
	highp float radius = max(max(foot, 0.0047*distance), 0.06*otherRadius.y);
	highp float along = min(slant, 6.);
	highp vec2 axis = key.xy/max(length(key.xy), 1e-5);
	if (length(key.xy) < 1e-5) axis = vec2(1., 0.);
	highp vec2 across = vec2(-axis.y, axis.x);
	highp float lod = max(1.5, log2(max(radius*otherRadius.z*1024., 1.)));
	highp float transmission = 0.;
	for (int tap = 0; tap < 5; ++tap)
	{
		highp vec2 offset = tap == 0 ? vec2(0.) : tap == 1 ? axis*along : tap == 2 ? -axis*along : tap == 3 ? across : -across;
		highp vec2 uv = map + weather.drift + radius*offset;
		highp vec4 f = cloudBodySample(textureLod(cloudTile, vec2(uv.x*otherShape.x, uv.y)*otherRadius.z + otherPhase[0], lod), otherShape.y, otherRadius.x - cloudRadii.x);
		highp float s = f.x + weather.growth.x;
		highp float height = otherRadius.y*otherShape.w*f.y*weather.growth.y;
		highp float round = s/pow(abs(s) + 0.01, 0.55);
		highp float plateau = 0.8*s/sqrt(s*s + cloudPlateauWidth(otherShape.y));
		highp float top = height*mix(round, plateau, cloudProfileFlatness(otherShape.y));
		highp float under = max(otherRadius.y*otherShape.w*(0.012 + 0.008*f.z), r - otherRadius.x);
		highp float tau = otherCover.w*weather.growth.z*max(top - under, 0.)*slant;
		// Fractional edge coverage keeps an optically thick cloud's filtered
		// silhouette soft, rather than turning every partly covered tap opaque.
		highp float edge = max(0.05, radius/(0.4*otherRadius.y));
		highp float covered = smoothstep(-edge, edge, s);
		highp float through = 1. - covered*(1. - exp(-tau));
		transmission += (tap == 0 ? 1./3. : 1./6.)*through;
	}
	return mix(1., 0.15 + 0.85*transmission, smoothstep(0.02, 0.08, key.z));
}

// Where a place of the map lies on the tile of the shapes, which flat clouds stretch
highp vec2 cloudTileAt(highp vec2 map)
{
	return vec2(map.x*cloudShape.x, map.y)*cloudTileScale.x + cloudPhase[0];
}

// The clouds are a density in space, not a surface. The height map (see CloudShape) gives the outline and the height of
// a cloud; within them a 3D noise (see CloudShape::computeBillows()) decides where there is cloud. Where the map says that
// the middle of a cloud is, the noise takes nothing away; towards its top and its sides it takes away more and more, so
// that only the billows of the noise stand: the cauliflower of the tops, the ragged sides and the shreds around a cloud.
// The base stays flat, at the level where the rising air condenses. The density falls off over some tens of meters at the
// edges, where the cloud mixes with the dry air: an edge as crisp as a wall makes a cloud look like a solid.

// The height of the top over the base (km) at the last place that cloudDepth() looked at, for the light; how soft the
// edge of the cloud is there, and the density of the thin shreds around it
highp float cloudTopHere = 0., cloudSoftHere = 0.1, cloudVeilHere = 0.;

// The integral of the ramp of the density at the edge of a cloud, from 0 below a to 1 above b, up to x
highp float edgeIntegral(highp float x, highp float a, highp float b)
{
	return x < a ? 0. : x < b ? 0.5*(x - a)*(x - a)/(b - a) : 0.5*(b - a) + x - b;
}

// The mean density along a step of the way on which the depth goes evenly from d0 to d1, with the ramp from a to b.
// The marches take it between two of their places: an edge that falls between them is neither lost nor shows the steps
// as terraces on the cloud or as contours in its shade, and the steps need no grain to hide them
highp float edgeMean(highp float d0, highp float d1, highp float a, highp float b)
{
	return abs(d1 - d0) > 1e-4 ? (edgeIntegral(d1, a, b) - edgeIntegral(d0, a, b))/(d1 - d0) : clamp((d1 - a)/(b - a), 0., 1.);
}

// How deep a place lies in the cloud: above 0 inside, below 0 outside, continuous everywhere; the density goes from 0
// to 1 as it goes from 0 to cloudSoftHere. At a place of the map and a height z over the base (km), with the weather
// there, the level of detail lod of the map and the footprint foot of a pixel (km). fine: with the finest billows, for
// the eye; the light does without them
highp float cloudDepth(highp vec2 map, highp float z, CloudWeather weather, highp float lod, highp float foot, bool fine)
{
	map += weather.drift;
	highp vec4 f = cloudBodySample(textureLod(cloudTile, cloudTileAt(map), lod), cloudShape.y, cloudRadii.y - cloudRadii.x);
	highp float s = f.x + weather.growth.x;
	cloudTopHere = 0.;
	cloudVeilHere = 0.;
	if (s < -0.3)
		return -10.;
	highp float flatness = cloudProfileFlatness(cloudShape.y);
	// Stratocumulus: broad masses, some km across, on a shared flat base, grey below and bright at their ragged rims,
	// with gaps of sky between them. Their thickness varies from place to place (lumps), which mottles the grey of the
	// base, and their rims are as torn as those of cumulus (heaped): a slab with a flat top and a smooth rim is a carpet,
	// not a cloud
	highp float rolls = smoothstep(0.15, 0.35, cloudShape.y)*(1. - smoothstep(0.45, 0.6, cloudShape.y));
	highp float heaped = max(1. - flatness, 0.9*rolls);
	// (the top of a stratocumulus lies against an inversion and is even: its relief is below, see the base)
	highp float lumps = max(1. - flatness, 0.1*rolls);
	highp float k = cloudBillowScale.x;
	// (the billows rise through the cloud as time goes on: cloudBillowScale.z)
	highp vec3 c = vec3(map*k + cloudBillowPhase, z*k - cloudBillowScale.z);
	highp float noiseLod = max(log2(foot*k*96.), 0.);
	// Broad lumps, the same all the way up: the towers of a cloud, of different heights on a shared base, and its
	// irregular outline
	// (the places in the tile of the billows are whole multiples of c only: the wind carries c on by whole tiles)
	highp float towers = textureLod(cloudBillow, vec3(c.xy + vec2(0.37, 0.11), 0.21), noiseLod).a;
	s += heaped*(0.16*(towers - 0.5) + 0.06);
	// Altocumulus: a sheet that has broken up into small rounded cloudlets, side by side like the cells of a honeycomb,
	// with lanes of sky between them (each cell is a small overturning of the air: it rises and condenses in the middle
	// and sinks, clear, around it). The cells are those of the coarse billows, on one level: where the sheet is thick
	// they touch and only thin lanes are left, towards its rim they stand apart and get smaller
	highp float alto = cloudAlto(cloudRadii.y - cloudRadii.x, cloudShape.y);
	// Floccus: heaped clouds at the level of altocumulus. No rising air from the ground feeds them through a flat base:
	// they are tufts, ragged below and fuzzy all around, which dissolve into shreds
	highp float floccus = smoothstep(2., 3.5, cloudRadii.y - cloudRadii.x)*(1. - smoothstep(0.1, 0.3, cloudShape.y));
	// How woolly the cloud is, against bubbly: floccus wholly, and heaped cumulus half. With the balls of the billows
	// alone a cumulus is a heap of lumps; a real one has soft, drawn-out shapes between its rounded tops, and sides that
	// dissolve into shreds
	highp float wool = max(floccus, 0.75*(1. - flatness)*(1. - alto));
	highp float cell = 1.;
	if (alto > 0.)
	{
		// (the cells are the bodies of the map, at a few times their scale: some 700 irregular ones before they repeat,
		// every few km. The cells of the billows repeat after 25, every km or so, which showed as straight rows of
		// cloudlets towards the horizon. Whole multiples of the place on the tile, so that the wind carries them on;
		// more across than along, against the stretch of the flat layers)
		highp float thick = clamp(s, 0., 1.);
		highp float bodies = textureLod(cloudTile, vec2(6., 3.)*cloudTileAt(map) + vec2(0.31, 0.67), lod + 2.).x;
		// (and small ones on and between them: cloudlets of many sizes and lumpy outlines)
		highp float small = textureLod(cloudBillow, vec3(c.xy + vec2(0.13, 0.57), 0.4), noiseLod).g;
		cell = smoothstep(-0.55 - 0.25*thick, 0.3 - 0.25*thick, bodies + 0.3*(small - 0.7) + 0.3*(towers - 0.5));
		s -= 0.5*alto*(1. - cell);   // (0.5: CloudShape::AltoGap)
		heaped = max(heaped, 0.7*alto);
	}
	// The rim of a cloud is torn: bays, tongues and shreds of some tens to a few hundred meters, different at each
	// height, where the dry air mixes in. The billows move the outline in and out, only near it: the middle of the cloud
	// keeps its thickness
	highp vec4 n = textureLod(cloudBillow, c, noiseLod);
	highp float rim = 1. - smoothstep(0.08, 0.5, s);
	s += heaped*rim*(0.2*(1. - wool)*(n.r - 0.7) + 0.22*(n.b - 0.58) + 0.32*(n.a - 0.5));
	// (between the rolls the layer thins out and breaks up)
	s -= 0.1*rolls*smoothstep(0.62, 0.3, towers);
	highp float height = cloudSize*cloudShape.w*f.y*weather.growth.y;
	highp float round = s/pow(abs(s) + 0.01, 0.55);
	highp float plateau = 0.8*s/sqrt(s*s + cloudPlateauWidth(cloudShape.y));
	highp float top = height*mix(round, plateau, flatness)*(1. + 0.9*lumps*(towers - 0.5))*mix(1., 0.12 + 1.2*cell*cell, alto);
	// (a stratocumulus is as thick in its rolls as before, though its base now lies higher between them)
	top *= 1. + 0.3*rolls;
	cloudTopHere = max(top, 0.);
	// How much of the noise stands: all of it in the middle of the cloud, less towards the top (over the upper part of
	// the cloud: a dome) and nothing at the level of the base, which is flat and sets in within a few meters
	// (below 0 under the base, above the top and outside the cloud, so that the depth is continuous)
	// The base of a stratocumulus is not level: the layer is some hundreds of meters thick in its rolls and thin between
	// them, with an even top, so the base hangs down in the rolls and lies higher between them. Seen from below, the
	// thick rolls are dark and what lies between them is bright
	// (broad rolls, from a blurred look at the lumps, with smaller ones on them)
	// (at whole multiples of c only, as above: at half of c the rolls jumped whenever the wind had carried c on by a tile)
	highp float broad = textureLod(cloudBillow, vec3(c.xy + vec2(0.61, 0.23), 0.7), noiseLod + 1.5).a;
	highp float lift = rolls*0.5*max(top, 0.)*smoothstep(0.8, 0.25, 0.65*broad + 0.35*towers);
	highp float lower = min((z - lift)/(0.008 + 0.022*cloudSize + 0.5*floccus*max(top, 0.) + 0.18*rolls*max(top, 0.)), 1.);
	highp float upper = min((top - z)/(mix(0.75, 0.4, rolls)*max(top, 0.) + 0.03*cloudSize), 1.);
	highp float cover = min(lower, upper);
	if (cover > 0.)
		cover = lower*upper*mix(1., clamp((s + 0.05)/0.35, 0., 1.), 0.85);
	// (the tufts of floccus are woolly, not bubbly: broad lumps and threads in place of the balls)
	highp float billow = smoothstep(0.4, 0.9, mix(0.27*n.a + 0.45*n.r + 0.28*n.g, 0.62*n.a + 0.3*n.b + 0.08*n.r + 0.07, wool));
	highp float erosion = mix(0.5, 0.85, heaped);
	highp float d = cover - erosion*(1. - billow);
	// (the finest billows, only where the edge is, and only where a pixel shows them)
	// Around the cloud, where it mixes with the dry air and dissolves, thin shreds: the fuzzy rim of a real cumulus
	// (threads of the wisps in the blue channel: with small balls there, the shreds were a halo of dots around the cloud)
	if (fine && d > -0.3 && d < 0.6 && noiseLod < 2.5)
	{
		highp vec4 m = textureLod(cloudBillow, 3.*c + vec3(0.43, 0.17, 0.71), noiseLod + 0.6);
		highp float shown = 1. - smoothstep(1.5, 2.5, noiseLod);
		cloudVeilHere = (0.1 + 0.2*wool)*heaped*shown*smoothstep(-0.3, 0., d)*smoothstep(0.62, 0.92, 0.6*n.b + 0.4*m.b)*smoothstep(0.35, 0.75, n.a);
		d -= mix(0.34, 0.5, rim)*(1. - 0.75*wool)*(1. - (0.6*m.r + 0.4*m.g))*shown;
	}
	// (soft edges; a little softer low on the sides of a heaped cloud than at its rising top)
	// (cloudlets of altocumulus and tufts of floccus are fuzzy all around)
	cloudSoftHere = mix(mix(0.24, mix(0.07, 0.14, wool), (1. - flatness)*smoothstep(0.15, 0.6, z/max(top, 0.01))), mix(0.34, 0.55, floccus), max(0.8*alto, floccus));
	return d;
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
	// (a long, low way through the layer, toward the horizon, gets up to 64 steps: with fewer, each one grows to several
	// km, longer than a cloud, and the far clouds fall apart in stripes; most ways stop early, in the first clouds)
	int steps = int(clamp(ceil((t1 - t0)/0.12), 1., 64.));
	highp float ds = (t1 - t0)/float(steps);

	// The exposure: the moments that the camera averages, as many as the movement needs on the screen here
	highp float spread = length(cloudSmear);
	int samples = int(clamp(ceil(spread/(max(t0*cloudPixel, 1e-4)*4.)), 1., float(cloudSamples)));
	highp float jitter = samples > 1 ? fract(52.9829189*fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715)))) : 0.5;
	// the steps between two moments blur the clouds, so that they do not show
	highp float smear = spread/float(samples);
	highp float look = log2((1.5*ds + smear)/cloudCover.z);

	highp float sunCos = dot(altAz, cloudSun), moonCos = dot(altAz, cloudMoon);
	highp float keyIsSun = step(0.5, dot(cloudKey, cloudSun));
	highp float towardKey = smoothstep(0.3, 0.9, dot(altAz, cloudKey));
	highp float keyUp = smoothstep(-0.05, 0.02, cloudKey.z);
	highp vec2 sunWay = cloudSun.xy/max(length(cloudSun.xy), 0.01);
	// A broad forward lobe for the light that many scatterings send on. The narrow silver lining comes below
	highp float sunPhase = 0.65 + 0.35*exp(-(1. - sunCos)/0.25);
	highp float moonPhase = 0.65 + 0.35*exp(-(1. - moonCos)/0.25);
	// The octaves of the many scatterings (see below): less of them looking towards the light, where they still go forwards,
	// and a thick cloud before the Sun stays dark
	highp float octaves = mix(0.35, 1., 1. - towardKey);

	// What the eye sees is the sum, along the line of sight, of the light that each bit of cloud sends towards it, dimmed
	// by the cloud in front of it. Each bit is lit through the cloud towards the light: the lit sides are bright, the far
	// sides and the bases grey, and a billow casts its shadow on the next one. There is no surface and no normal.
	// direct: the light of the Sun and the Moon; diffuse: that of the sky, the ground and the towns
	highp vec3 direct = vec3(0.), diffuse = vec3(0.);
	highp float T = 0., thin = 0., hitT = 0., hitWeight = 0., single = 0.;
	for (int moment = 0; moment < samples; ++moment)
	{
		// (the moments differ from pixel to pixel, so that the steps between them make grain, not stripes)
		highp float when = (float(moment) + jitter)/float(samples) - 0.5;
		cloudMoment = cloudSmear*when;
		highp float depth = 0.;
		CloudWeather weatherFrom = CloudWeather(vec3(0.), vec2(0.)), weatherTo = CloudWeather(vec3(0.), vec2(0.));
		int stride = int(clamp(floor(0.5/ds), 1., 4.)), phase = 0;
		highp float lastD = -10., lastU = -1e9;  // the depth in the cloud at the last short step, and where that was
		highp float lastSeen = -1.;              // the cloud on the way of the light at the last place that looked for it (km)
		// Through the layer, until the clouds are opaque. Long steps look for clouds, short ones go through them
		for (int k = 0; k < steps && depth < 7.; ++k)
		{
			highp float t = t0 + float(k)*ds;
			highp vec3 middle = vec3(altAz.xy*(t + 0.5*ds), r0 + altAz.z*(t + 0.5*ds));
			highp vec2 middleMap = cloudMap(middle);
			// A blurred look at the map tells whether a cloud may lie in this step, and how high it may reach
			// (the weather of the end of this step, against the one at its start: the clouds must not jump between steps)
			// (the weather changes over a km or more: it is looked up every half km or so of the way, and goes evenly in
			// between. Looked up at every step, it cost more than the clouds themselves towards the horizon)
			if (k == 0)
				weatherTo = cloudWeather(vec3(altAz.xy*t, r0 + altAz.z*t));
			if (phase == 0)
			{
				weatherFrom = weatherTo;
				highp float ahead = t + float(stride)*ds;
				weatherTo = cloudWeather(vec3(altAz.xy*ahead, r0 + altAz.z*ahead));
			}
			CloudWeather weatherStart = mixWeather(weatherFrom, weatherTo, float(phase)/float(stride));
			CloudWeather weatherEnd = mixWeather(weatherFrom, weatherTo, float(phase + 1)/float(stride));
			phase = phase + 1 == stride ? 0 : phase + 1;
			highp vec2 lookAt = cloudTileAt(middleMap + 0.5*(weatherStart.drift + weatherEnd.drift));
			highp vec4 blurred = textureLod(cloudTile, lookAt, look);
			// (and a wider look, for what lies a little beyond: a step that skips a cloud cuts it)
			blurred = cloudBodySample(max(blurred, textureLod(cloudTile, lookAt, look + 1.5)), cloudShape.y, cloudRadii.y - cloudRadii.x);
			highp float shape = blurred.x + 0.5*(weatherStart.growth.x + weatherEnd.growth.x);
			// (the towers reach up to 1.6 times the height of the map)
			highp float reach = 1.6*cloudSize*cloudShape.w*(blurred.y*pow(max(shape + 0.5, 0.), 0.7) + 0.1);
			if (max(weatherStart.growth.z, weatherEnd.growth.z) <= 1e-5
			    || !(t > 30. || (shape > -0.7 && length(middle) - cloudRadii.y - 0.5*ds*altAz.z < reach + 0.05 + smear)))
				continue;
			// (the map is no blurrier than a pixel, or a tenth of a step: the steps are short enough for far clouds)
			highp float foot = max(max(t*cloudPixel, 0.1*ds), smear);
			highp float lod = max(log2(foot/cloudCover.z), 0.);
			// (far away, where a pixel is wider than a billow, the light comes without a way of its own through the cloud)
			highp float marched = keyUp*(1. - smoothstep(0.15, 0.3, t*cloudPixel));
			// A second, closer look before the short steps: a few places along this step, with the map a little blurred.
			// Most of the steps that pass the first look hold no cloud after all, and the short steps through them cost
			// more than those through the clouds
			int probes = int(clamp(ceil(ds/0.04), 3., 6.));
			bool found = false;
			for (int i = 0; i < probes && !found; ++i)
			{
				highp float u = t + (float(i) + 0.5)*ds/float(probes);
				CloudWeather weather = mixWeather(weatherStart, weatherEnd, (float(i) + 0.5)/float(probes));
				highp vec4 f = cloudBodySample(textureLod(cloudTile, cloudTileAt(cloudMap(vec3(altAz.xy*u, r0 + altAz.z*u)) + weather.drift), lod + 2.),
				                               cloudShape.y, cloudRadii.y - cloudRadii.x);
				found = f.x + weather.growth.x > -0.45;
			}
			if (!found)
				continue;
			highp float dt = ds/float(CloudSubsteps);
			for (int j = 0; j < CloudSubsteps && depth < 7.; ++j)
			{
				highp float u = t + (float(j) + 0.5)*dt;
				highp vec3 q = vec3(altAz.xy*u, r0 + altAz.z*u);
				highp float height = length(q) - cloudRadii.y;
				CloudWeather weather = mixWeather(weatherStart, weatherEnd, (float(j) + 0.5)/float(CloudSubsteps));
				highp float d = cloudDepth(cloudMap(q), height, weather, lod, foot, true);
				// (the mean density since the last short step; after a step that was skipped, as clear air, that of this place)
				bool follows = u - lastU < 1.5*dt;
				// (after a step that was skipped, as clear air, only this place counts)
				highp float d0 = follows ? lastD : d;
				lastD = d; lastU = u;
				if (max(d0, d) <= -0.3)
					continue;
				highp float top = cloudTopHere, soft = cloudSoftHere, veil = cloudVeilHere;
				highp float extinction = cloudCover.w*weather.growth.z;
				// The stretch of the way from the last short step to this one, on which the depth in the cloud goes from one
				// to the other. Near the surface of a cloud, where it is bright and soon opaque, the stretch counts in two
				// parts, each with its own light: one part of some 20 m is as opaque as the eye sees deep, and with one light
				// for all of it the light jumped as the part moved through the surface from pixel to pixel, which drew fine
				// stripes across the cloud. Each part takes the mean density along it, and the light of the middle of what
				// lies in the cloud of it, which moves evenly with the edge. Then the steps need no grain to hide them
				// (in one part: the short steps are fine enough since there are 12 of them. More parts near the surface, each
				// with a light of its own, cost much and showed no difference)
				int parts = 1;
				for (int part = 0; part < parts && depth < 7.; ++part)
				{
					highp float da = mix(d0, d, float(part)/float(parts)), db = mix(d0, d, float(part + 1)/float(parts));
					highp float density = max(edgeMean(da, db, 0., soft), veil);
					if (density <= 0.)
						continue;
					highp float cross = da*db < 0. ? da/(da - db) : 0.;   // where the edge lies in the part
					highp float middle = 0.5*((da < 0. ? cross : 0.) + (db < 0. ? cross : 1.));
					highp float dHere = mix(da, db, middle);
					highp float uHere = u - dt*(1. - (float(part) + middle)/float(parts));
					highp vec3 here = vec3(altAz.xy*uHere, r0 + altAz.z*uHere);
					highp float heightHere = length(here) - cloudRadii.y;
					highp float opticalDepth = extinction*density*dt/float(parts);
					highp float weight = exp(-depth)*(1. - exp(-opticalDepth));
					depth += opticalDepth;

					// The way of the light to this place through the cloud: a few steps that grow, towards the Sun or the Moon
					// (the steps lie at the same places for every pixel: steps that differ from pixel to pixel make grain)
					highp float above = max(top - heightHere, 0.);
					highp float seen = 0.6*above;
					// (the way of the light costs most of the time of the clouds. It is left out where it shows least: for the
					// thin shreds around a cloud, which are lit all through, and deep in the cloud, behind what hides it
					// almost wholly, where the last light found goes on)
					bool deep = depth > 3.4 && lastSeen >= 0.;
					if (deep)
						seen = lastSeen;
					else if (opticalDepth < 0.02)
						seen = 0.;
					else if (marched > 0.)
					{
						highp float way = 0., stepLength = 0.035*cloudSize + 0.5*foot;
						highp float lastLight = dHere;
						seen = 0.;
						for (int l = 0; l < 4; ++l)
						{
							highp vec3 g = here + (way + stepLength)*cloudKey;
							highp float dLight = cloudDepth(cloudMap(g), length(g) - cloudRadii.y, weather, lod + 0.8*float(l), foot + 0.3*stepLength, false);
							// (the light sees a softer cloud, the softer the longer its step: with the mean density between its
							// places the shade then changes smoothly from place to place, where single looks at a crisp cloud drew
							// the outline of the cloud into its shade six times over, as contours)
							highp float width = max(0.3, stepLength/(0.75*max(cloudTopHere, 0.1*cloudSize)));
							seen += stepLength*edgeMean(lastLight, dLight, -0.25*width, 0.75*width);
							lastLight = dLight;
							way += stepLength;
							stepLength *= 2.3;
						}
						seen = mix(0.6*above, seen, marched);
					}
					if (!deep && opticalDepth >= 0.02)
						lastSeen = seen;
					// The light of the Sun or the Moon in the cloud, in two parts that add up to no more than what falls on it:
					// droplets of water take no light away, they only send it on, and mostly forwards.
					// First the beam and the first few scatterings, which still know where the light came from: in octaves
					// (Wrenninge's approximation, as in Hillaire's clouds), each further order of scattering sees the cloud
					// thinner. They light the side of a cloud that faces the light and soften the line between light and shade
					highp float tau = 0.55*extinction*seen;
					highp float scattered = mix(1., (exp(-tau) + octaves*0.55*exp(-0.35*tau))/(1. + 0.55*octaves), keyUp);
					// Then the light that has lost its direction after many scatterings and diffuses down through the cloud from
					// its top: of the light that falls on a layer of the optical depth tau, the part 1/(1 + 0.75 tau (1 - g))
					// comes out below (two-stream, g = 0.85; see CloudLight). This is the light of the grey bases, and it sets
					// how dark they are against the sunlit sides: about 0.4 of them under 250 m of cloud, 0.2 under 700 m
					highp float fromAbove = 1./(1. + 8.4*cloudShape.z*weather.growth.z*above);
					highp float fromBelow = 1./(1. + 8.4*cloudShape.z*weather.growth.z*heightHere);
					highp float diffused = keyUp*max(cloudKey.z, 0.)*fromAbove*(1. - exp(-0.35*tau))/0.85;
					// The light of the sky comes from above through the cloud in the same way, that of the ground from below
					diffuse += weight*(0.5*(cloudAbove*fromAbove + cloudBelow*fromBelow) + cloudCity*fromBelow);
					highp vec3 sunlight = cloudSunlight(dot(here.xy, sunWay)*57.3/7433. + 0.35*heightHere);
					direct += weight*(sunlight*(sunPhase*mix(1., scattered, keyIsSun) + diffused*keyIsSun)
					                  + sideMoon*(moonPhase*mix(scattered, 1., keyIsSun) + diffused*(1. - keyIsSun)));
					single += weight*exp(-extinction*seen);
					hitT += weight*uHere;
					hitWeight += weight;
				}
			}
		}
		// (a thick cloud is opaque: the Sun must not shine through where the way stopped)
		highp float through = exp(-depth)*(1. - smoothstep(5., 6.9, depth));
		T += through;
		thin += through*(1. - through);
	}
	if (hitWeight < 1e-6)
	{
		FRAG_COLOR = vec4(0., 0., 0., 1.);
		return;
	}
	highp float moments = float(samples);
	T /= moments;
	thin /= moments;
	hitT /= hitWeight;
	single /= hitWeight;
	// (the light was summed up to where the way stopped: the rest of an opaque cloud is as its last part)
	highp float opacity = hitWeight/moments;
	highp float fill = (1. - T)/max(opacity, 1e-4);
	direct *= fill/moments;
	diffuse *= fill/moments;
	cloudMoment = vec2(0.);

	// The layer above and the cirrus cast their shadows on the cloud, at the place where the line of sight met it
	highp vec3 p = vec3(altAz.xy*hitT, r0 + altAz.z*hitT);
	highp float upper = otherShadow(p, cloudKey, hitT*cloudPixel)*cirrusShadow(p, cloudKey);
	highp vec3 light = direct*upper + diffuse;

	// The thin edges scatter the light of the Sun and the Moon forwards (Henyey-Greenstein with g = 0.85)
	// (the edges light up only within some 20 degrees of them: further away the bodies of the clouds outshine them)
	// The light that is not the key has no way of its own through the cloud: it takes the transmission of the view
	highp vec3 silver = mix(T, single, keyIsSun)*upper*silverSun*(0.2775/pow(1.7225 - 1.7*sunCos, 1.5))*smoothstep(0.88, 0.985, sunCos)
	                  + mix(single, T, keyIsSun)*upper*silverMoon*(0.2775/pow(1.7225 - 1.7*moonCos, 1.5))*smoothstep(0.88, 0.985, moonCos);
	light += silver*thin;

	// The way through the air dims the cloud and adds the light of the air (Rayleigh phase function)
	highp vec3 way = exp(-airExtinction*hitT);
	highp vec3 sky = foregroundSky(ndcPos.xy, airSky + airMoon*0.75*(1. + moonCos*moonCos));
	highp vec3 air = (1. - way)*sky;
	// Haze is emitted in front of the cloud; it does not open a path through it: the transmission of the background stays T,
	// also at the horizon, or the Sun and the Moon would show behind opaque clouds
	// out = source + scene x alpha (light holds the opacity of the cloud already)
	FRAG_COLOR = vec4(light*way + air*(1. - T), T);
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
	       && core->getCurrentPlanet()->getEnglishName() == QStringLiteral("Earth");
}

bool CloudMgr::isCirrusShown(const StelCore* core) const
{
	return isShown(core) && flagCirrus && core->getCurrentLocation().altitude*0.001 < cirrusHeight;
}

double CloudMgr::cirrusTransmission(const StelCore* core, const Vec3d& altAz) const
{
	if (altAz[2] < -0.015)
		return 1.;
	// as in the shader: the place where the line of sight meets the layer, and the slant of the way through it
	const StelLocation& location = core->getCurrentLocation();
	const double r0 = EarthRadius + std::max(0., location.altitude*0.001), rc = EarthRadius + cirrusHeight;
	const double z = std::max(altAz[2], 0.);
	const double b = r0*z, c = (rc - r0)*(rc + r0);
	const double t = c/(b + std::sqrt(b*b + c));
	const Vec3d p(t*altAz[0], t*altAz[1], r0 + t*z);
	const double slant = 1./std::max((p[0]*altAz[0] + p[1]*altAz[1] + p[2]*z)/p.norm(), 0.05);
	const double phi = static_cast<double>(location.getLatitude())*M_PI/180.;
	const double lambda = static_cast<double>(location.getLongitude())*M_PI/180.;
	const Vec3d up(std::cos(phi)*std::cos(lambda), std::cos(phi)*std::sin(lambda), std::sin(phi));
	const Vec3d east(-std::sin(lambda), std::cos(lambda), 0.);
	const Vec3d south(std::sin(phi)*std::cos(lambda), std::sin(phi)*std::sin(lambda), -std::cos(phi));
	const Vec3d earth = south*p[0] + east*p[1] + up*p[2];
	const double scale = 2.*rc/(earth.norm() + std::fabs(earth[2]));
	const CloudShape::Motion m = motionAt(core, core->getJD(), CirrusWind);
	const double mapX = m.cosine*scale*earth[0] - m.sine*scale*earth[1] + m.offsetX + CloudShape::CirrusShift;
	const double mapY = m.sine*scale*earth[0] + m.cosine*scale*earth[1] + m.offsetY + CloudShape::CirrusShift;
	const CloudShape::Motion f = motionAt(core, core->getJD(), 0);
	const double cover = cirrusFront.on ? CloudShape::frontCover(tile, f.cosine*scale*earth[0] - f.sine*scale*earth[1] + f.offsetX,
	                                                             f.sine*scale*earth[0] + f.cosine*scale*earth[1] + f.offsetY, cirrusFront, f)
	                                    : currentCirrusCover;
	return std::exp(-cirrusOpticalDepth*CloudShape::cirrusDensity(tile, mapX, mapY, cover)*slant);
}

bool CloudMgr::isLayerShown(const StelCore* core, int i) const
{
	return isShown(core) && layerOn[i] && core->getCurrentLocation().altitude*0.001 < layers[i].base;
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
	const double observerRadius = EarthRadius + std::max(0., location.altitude*0.001);
	// The mean over the exposure, as for what the shader draws
	const StelSkyDrawer* drawer = core->getSkyDrawer();
	const double exposure = drawer->getFlagPhotoStarTrails() ? std::max(0., drawer->getPhotoEffectiveExposureTime()) : 0.;
	const CloudShape::Motion fronts = motionAt(core, core->getJD(), 0);
	double result = 1.;
	for (int l = 0; l < LayerCount; ++l)
	{
		if (!isLayerShown(core, l))
			continue;
		double windX = 0., windY = 0.;
		computeWind(core, l, &windX, &windY);
		const CloudShape::Motion motion = motionAt(core, core->getJD(), l);
		const int samples = exposure*std::hypot(windX, windY) > 0.01 ? 4 : 1;
		double sum = 0.;
		for (int i = 0; i < samples; ++i)
		{
			CloudShape::Motion moment = motion;
			const double when = (i + 0.5)/samples - 0.5;
			moment.offsetX += windX*exposure*when;
			moment.offsetY += windY*exposure*when;
			sum += CloudShape::transmission(tile, currentLayer(l), moment, fronts, observerRadius, {altAz[0], altAz[1], altAz[2]}, south, east, up);
		}
		result *= sum/samples;
	}
	if (isCirrusShown(core))
		result *= cirrusTransmission(core, altAz);
	return result;
}

void CloudMgr::setLightUniforms(const StelCore* core, QOpenGLShaderProgram& p, int i) const
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
	const CloudShape::Layer& layer = layers[i];
	s.base = layer.base;
	s.top = layer.base + 0.6*layer.size*layer.heightScale();
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
	// (the clouds shade the ground below them, which then sends less light back up to their bases)
	CloudLight::Rgb below = light.below;
	const double groundShade = 1. - 0.7*std::clamp(currentCovers[i], 0., 1.);
	for (double& v : below)
		v *= groundShade;
	p.setUniformValue("cloudBelow", scene(below));
	p.setUniformValue("cloudAbove", scene(light.above));
	p.setUniformValue("cloudCity", scene(light.city));
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
	p.setUniformValue("airMoon", scene(light.airMoon));
	p.setUniformValue("airExtinction", QVector3D(static_cast<float>(light.extinction[0]), static_cast<float>(light.extinction[1]), static_cast<float>(light.extinction[2])));
}

void CloudMgr::setUniformsOfLayer(QOpenGLShaderProgram& p, int i, const char* prefix) const
{
	// The shape of the layer i, for the uniforms named prefix + Cover, Shape, Bank, Shift and Radius
	const CloudShape::Layer& layer = layers[i];
	const QByteArray name(prefix);
	const float coverStart = static_cast<float>(CloudShape::coverOffset(currentCovers[i], 0.));
	p.setUniformValue((name + "Cover").constData(), QVector4D(coverStart, static_cast<float>(CloudShape::coverOffset(currentCovers[i], 1.)) - coverStart,
	                                                          static_cast<float>(layer.size*layer.bodyScale()*CloudShape::TileSize/CloudShape::TileTexels),
	                                                          static_cast<float>(CloudShape::Extinction*layer.density)));
	p.setUniformValue((name + "Shape").constData(), QVector4D(static_cast<float>(layer.stretch()), static_cast<float>(layer.flatness),
	                                                          static_cast<float>(layer.density), static_cast<float>(layer.heightScale())));
	// (a bank of a negative far edge: none)
	p.setUniformValue((name + "Bank").constData(), QVector4D(static_cast<float>(layer.bankAzimuth*M_PI/180.), static_cast<float>(0.5*layer.bankWidth*M_PI/180.),
	                                                         static_cast<float>(layer.bankDistance), static_cast<float>(layer.bank ? layer.bankDistance + layer.bankDepth : -1.)));
	p.setUniformValue((name + "Shift").constData(), QVector2D(static_cast<float>(layer.shift), static_cast<float>(layer.shift)));
	p.setUniformValue((name + "Front").constData(), frontUniform(fronts[i]));
	// the radius of the base (km), the size of the clouds, 1/km of the tile
	p.setUniformValue((name + "Radius").constData(), QVector3D(static_cast<float>(EarthRadius + layer.base), static_cast<float>(layer.size),
	                                                           static_cast<float>(1./(layer.size*layer.bodyScale()*CloudShape::TileSize))));
}

bool CloudMgr::beginAtmosphereCapture(StelCore* core, bool enabled)
{
	// Never use a previous frame when the atmosphere or the clouds are disabled.
	atmosphereReady = false;
	if (!enabled || !isShown(core) || !(isLayerShown(core, 0) || isLayerShown(core, 1) || isCirrusShown(core)))
		return false;
	auto& gl = *QOpenGLContext::currentContext()->functions();
	gl.glGetIntegerv(GL_FRAMEBUFFER_BINDING, &atmospherePreviousBuffer);
	gl.glGetIntegerv(GL_VIEWPORT, atmosphereViewport);
	if (!atmosphereComposite)
	{
		auto composite = std::make_unique<QOpenGLShaderProgram>();
		const QByteArray vert = StelOpenGL::globalShaderPrefix(StelOpenGL::VERTEX_SHADER) + R"(
ATTRIBUTE highp vec2 vertex;
VARYING highp vec2 texCoord;
void main()
{
	gl_Position = vec4(vertex, 0., 1.);
	texCoord = 0.5*vertex + 0.5;
}
)";
		const QByteArray frag = StelOpenGL::globalShaderPrefix(StelOpenGL::FRAGMENT_SHADER) + R"(
VARYING highp vec2 texCoord;
uniform sampler2D skyTex;
uniform highp vec4 skyUv;
void main()
{
	FRAG_COLOR = texture2D(skyTex, skyUv.xy + texCoord*skyUv.zw);
}
)";
		if (!composite->addShaderFromSourceCode(QOpenGLShader::Vertex, vert)
		    || !composite->addShaderFromSourceCode(QOpenGLShader::Fragment, frag))
			return false;
		composite->bindAttributeLocation("vertex", VertexAttribIndex);
		if (!StelPainter::linkProg(composite.get(), "cloud atmosphere capture"))
			return false;
		atmosphereComposite = std::move(composite);
	}
	// Keep the original viewport and pixel coordinates: the atmosphere renderers
	// also use projection matrices and meshes expressed in those coordinates.
	const QSize size(std::max(1, std::max(atmosphereViewport[2], atmosphereViewport[0] + atmosphereViewport[2])),
	                 std::max(1, std::max(atmosphereViewport[3], atmosphereViewport[1] + atmosphereViewport[3])));
	if (!atmosphereBuffer || atmosphereBuffer->size() != size)
	{
		QOpenGLFramebufferObjectFormat format;
		format.setInternalTextureFormat(GL_RGBA16F);
		atmosphereBuffer = std::make_unique<QOpenGLFramebufferObject>(size, format);
		gl.glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(atmospherePreviousBuffer));
		if (!atmosphereBuffer->isValid())
		{
			atmosphereBuffer.reset();
			return false;
		}
		gl.glBindTexture(GL_TEXTURE_2D, atmosphereBuffer->texture());
		gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		gl.glBindTexture(GL_TEXTURE_2D, 0);
	}
	gl.glBindFramebuffer(GL_FRAMEBUFFER, atmosphereBuffer->handle());
	GLfloat clear[4]; GLboolean mask[4];
	gl.glGetFloatv(GL_COLOR_CLEAR_VALUE, clear);
	gl.glGetBooleanv(GL_COLOR_WRITEMASK, mask);
	const bool scissor = gl.glIsEnabled(GL_SCISSOR_TEST);
	gl.glDisable(GL_SCISSOR_TEST);
	gl.glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	gl.glClearColor(0.f, 0.f, 0.f, 0.f);
	gl.glClear(GL_COLOR_BUFFER_BIT);
	gl.glClearColor(clear[0], clear[1], clear[2], clear[3]);
	gl.glColorMask(mask[0], mask[1], mask[2], mask[3]);
	if (scissor) gl.glEnable(GL_SCISSOR_TEST);
	return true;
}

void CloudMgr::endAtmosphereCapture()
{
	auto& gl = *QOpenGLContext::currentContext()->functions();
	gl.glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(atmospherePreviousBuffer));
	gl.glViewport(atmosphereViewport[0], atmosphereViewport[1], atmosphereViewport[2], atmosphereViewport[3]);
	GLint blend[4], activeTexture, texture0, currentProgram;
	gl.glGetIntegerv(GL_BLEND_SRC_RGB, &blend[0]); gl.glGetIntegerv(GL_BLEND_DST_RGB, &blend[1]);
	gl.glGetIntegerv(GL_BLEND_SRC_ALPHA, &blend[2]); gl.glGetIntegerv(GL_BLEND_DST_ALPHA, &blend[3]);
	gl.glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture);
	gl.glGetIntegerv(GL_CURRENT_PROGRAM, &currentProgram);
	const bool blending = gl.glIsEnabled(GL_BLEND), depth = gl.glIsEnabled(GL_DEPTH_TEST);
	gl.glActiveTexture(GL_TEXTURE0);
	gl.glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture0);
	atmosphereComposite->bind();
	atmosphereComposite->setUniformValue("skyTex", 0);
	atmosphereComposite->setUniformValue("skyUv", QVector4D(float(atmosphereViewport[0])/atmosphereBuffer->width(),
		float(atmosphereViewport[1])/atmosphereBuffer->height(), float(atmosphereViewport[2])/atmosphereBuffer->width(),
		float(atmosphereViewport[3])/atmosphereBuffer->height()));
	gl.glBindTexture(GL_TEXTURE_2D, atmosphereBuffer->texture());
	gl.glDisable(GL_DEPTH_TEST);
	gl.glEnable(GL_BLEND);
	// All supported atmospheres add their linear radiance in photographic mode.
	gl.glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ZERO, GL_ONE);
	drawQuad();
	atmosphereComposite->release();
	gl.glUseProgram(static_cast<GLuint>(currentProgram));
	gl.glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(texture0));
	gl.glActiveTexture(static_cast<GLenum>(activeTexture));
	gl.glBlendFuncSeparate(static_cast<GLenum>(blend[0]), static_cast<GLenum>(blend[1]), static_cast<GLenum>(blend[2]), static_cast<GLenum>(blend[3]));
	if (!blending) gl.glDisable(GL_BLEND);
	if (depth) gl.glEnable(GL_DEPTH_TEST);
	atmosphereReady = true;
}

void CloudMgr::bindAtmosphere(QOpenGLShaderProgram& p) const
{
	p.setUniformValue("cloudHasSky", atmosphereReady);
	p.setUniformValue("cloudSkyTex", 1);
	if (!atmosphereReady)
		return;
	auto& gl = *QOpenGLContext::currentContext()->functions();
	gl.glActiveTexture(GL_TEXTURE1);
	gl.glBindTexture(GL_TEXTURE_2D, atmosphereBuffer->texture());
	gl.glActiveTexture(GL_TEXTURE0);
	p.setUniformValue("cloudSkyUv", QVector4D(float(atmosphereViewport[0])/atmosphereBuffer->width(),
		float(atmosphereViewport[1])/atmosphereBuffer->height(), float(atmosphereViewport[2])/atmosphereBuffer->width(),
		float(atmosphereViewport[3])/atmosphereBuffer->height()));
}

void CloudMgr::drawOverSky(StelCore* core)
{
	if (!isShown(core) || !(isLayerShown(core, 0) || isLayerShown(core, 1) || isCirrusShown(core)))
		return;
	// The higher layer first, the lower one over it
	int order[LayerCount] = {0, 1};
	if (layers[1].base > layers[0].base)
		std::swap(order[0], order[1]);

	const StelProjectorP prj = core->getProjection(StelCore::FrameAltAz, StelCore::RefractionAuto);
	if (!programProjector || !prj->isSameProjection(*programProjector))
	{
		buildProgram(*prj);
		programProjector = prj;
	}
	if (!program)
		return;

	// out = source + scene x alpha; the alpha of the scene stays as it is. The state of the blending goes back to what it
	// was: StelPainter remembers it and would not set it again
	auto& gl = *QOpenGLContext::currentContext()->functions();
	const GLboolean blendWasOn = gl.glIsEnabled(GL_BLEND);
	GLint blend[4];
	gl.glGetIntegerv(GL_BLEND_SRC_RGB, &blend[0]);
	gl.glGetIntegerv(GL_BLEND_DST_RGB, &blend[1]);
	gl.glGetIntegerv(GL_BLEND_SRC_ALPHA, &blend[2]);
	gl.glGetIntegerv(GL_BLEND_DST_ALPHA, &blend[3]);
	// The cirrus lie above all the other clouds
	if (isCirrusShown(core))
		drawCirrus(core, *prj);
	for (int k = 0; k < LayerCount; ++k)
	{
		const int i = order[k];
		if (!isLayerShown(core, i))
			continue;
		// the layer above, if it shows, casts its shadows on this one
		const int above = order[0];
		drawLayer(core, *prj, i, above != i && isLayerShown(core, above) ? above : -1);
	}
	gl.glBlendFuncSeparate(static_cast<GLenum>(blend[0]), static_cast<GLenum>(blend[1]), static_cast<GLenum>(blend[2]), static_cast<GLenum>(blend[3]));
	if (!blendWasOn)
		gl.glDisable(GL_BLEND);
}

void CloudMgr::drawLayer(StelCore* core, const StelProjector& prj, int i, int other)
{
	const StelSkyDrawer* drawer = core->getSkyDrawer();
	const StelLocation& location = core->getCurrentLocation();
	const double observerHeight = std::max(0., location.altitude*0.001);
	const CloudShape::Layer& layer = layers[i];

	QOpenGLShaderProgram& p = *program;
	p.bind();
	bindAtmosphere(p);
	p.setUniformValue("projectionMatrixInverse", prj.getProjectionMatrix().toQMatrix().inverted());
	prj.setUnProjectUniforms(p);
	drawer->getExtinction().setForwardTransformUniforms(p);
	setLightUniforms(core, p, i);

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
	const double base = EarthRadius + layer.base, top = base + CloudShape::LayerThickness*layer.size*layer.heightScale();
	p.setUniformValue("cloudRadii", QVector4D(static_cast<float>(r0), static_cast<float>(base), static_cast<float>((base - r0)*(base + r0)),
	                                          static_cast<float>((top - r0)*(top + r0))));
	p.setUniformValue("cloudPixel", static_cast<float>((flagHalfResolution ? static_cast<double>(lowResDivisor) : 1.)/static_cast<double>(prj.getPixelPerRadAtCenter())));
	bindTileTexture(2);
	p.setUniformValue("cloudTile", 2);
	bindBillowTexture(3);
	p.setUniformValue("cloudBillow", 3);
	p.setUniformValue("cloudTileScale", QVector4D(static_cast<float>(1./(layer.size*layer.bodyScale()*CloudShape::TileSize)), static_cast<float>(1./CloudShape::CoverScaleLarge),
	                                              static_cast<float>(1./CloudShape::CoverScaleSmall), static_cast<float>(1./(layer.size*CloudShape::FineScale))));
	p.setUniformValue("cloudSize", static_cast<float>(layer.size));
	setUniformsOfLayer(p, i, "cloud");
	// The layer above casts its shadows (a radius of 0: none)
	if (other >= 0)
		setUniformsOfLayer(p, other, "other");
	else
		p.setUniformValue("otherRadius", QVector3D(0.f, 1.f, 1.f));

	const CloudShape::Motion motion = motionAt(core, core->getJD(), i);
	setLayerMotion(p, "cloud", layer, motion);
	// (the layer above in its own wind, the fronts in that of the first layer)
	if (other >= 0)
		setLayerMotion(p, "other", layers[other], motionAt(core, core->getJD(), other));
	setFrontMotion(p, motionAt(core, core->getJD(), 0));
	// (the cirrus cast their shadows if they show)
	p.setUniformValue("cirrusShade", QVector4D(isCirrusShown(core) ? static_cast<float>(cirrusOpticalDepth) : 0.f, static_cast<float>(EarthRadius + cirrusHeight),
	                                           static_cast<float>(currentCirrusCover), 0.f));
	setCirrusMotion(p, "cirrusTurn", motionAt(core, core->getJD(), CirrusWind));
	p.setUniformValue("cirrusFront", frontUniform(cirrusFront));
	p.setUniformValue("cloudGrow", QVector2D(static_cast<float>(motion.growU), static_cast<float>(motion.growV)));
	// The billows: 1/km of their tile, a free place, and how far
	// they have risen through the clouds (in tiles, reduced in double precision: the third coordinate of the tile moves on,
	// as the air rises in a cumulus and its billows with it)
	const double billowRise = CloudShape::BillowRise*(core->getJD() - 2451545.)*86400./layer.billowTile();
	p.setUniformValue("cloudBillowScale", QVector3D(static_cast<float>(1./layer.billowTile()),
	                                                0.f,
	                                                static_cast<float>(std::remainder(billowRise + 0.35*motion.boilX/layer.billowTile(), 1.))));
	p.setUniformValue("cloudFrontDrift", QVector2D(static_cast<float>(motion.frontU), static_cast<float>(motion.frontV)));
	p.setUniformValue("cloudDeform", QVector2D(static_cast<float>(motion.deformU), static_cast<float>(motion.deformV)));
	p.setUniformValue("cloudDeformScale", static_cast<float>(1./CloudShape::DeformScale));
	p.setUniformValue("cloudGrowScale", static_cast<float>(1./CloudShape::GrowScale));
	p.setUniformValue("cloudGrowStrength", static_cast<float>(CloudShape::GrowStrength));
	// The exposure, if the sky moves during it in the camera
	const double exposure = drawer->getFlagPhotoStarTrails() ? std::max(0., drawer->getPhotoEffectiveExposureTime()) : 0.;
	double windX = 0., windY = 0.;
	computeWind(core, i, &windX, &windY);
	p.setUniformValue("cloudSmear", QVector2D(static_cast<float>(windX*exposure), static_cast<float>(windY*exposure)));
	p.setUniformValue("cloudSamples", MaxExposureSamples);

	auto& gl = *QOpenGLContext::currentContext()->functions();
	const bool lowRes = flagHalfResolution && drawToLowResBuffer();
	if (lowRes)
	{
		p.release();
		compositeProgram->bind();
		gl.glActiveTexture(GL_TEXTURE0);
		gl.glBindTexture(GL_TEXTURE_2D, lowResBuffer->texture());
		compositeProgram->setUniformValue("cloudTex", 0);
	}
	gl.glEnable(GL_BLEND);
	gl.glBlendFuncSeparate(GL_ONE, GL_SRC_ALPHA, GL_ZERO, GL_ONE);
	drawQuad();
	if (lowRes)
	{
		gl.glBindTexture(GL_TEXTURE_2D, 0);
		compositeProgram->release();
	}
	else
		p.release();
}

void CloudMgr::buildCirrusProgram(const StelProjector& projector)
{
	cirrusProgram.reset();
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
	// The cirrus: a thin layer that scatters the light once. Everything is in the AltAz frame of the observer with its origin
	// in the center of the Earth (km), as for the other clouds.
	const QByteArray frag = StelOpenGL::globalShaderPrefix(StelOpenGL::FRAGMENT_SHADER)
	                        + projector.getUnProjectShader()
	                        + frontShader()
	                        + cirrusShader()
	                        + atmosphereShader()
	                        + R"(
VARYING highp vec3 ndcPos;
uniform highp mat4 projectionMatrixInverse;
uniform highp vec3 cloudSouth;
uniform highp vec3 cloudEast;
uniform highp vec3 cloudUp;
uniform highp vec4 cloudTurn;       // the wind: the turn of the map, as cosine and sine; its offset (km)
uniform highp vec4 cirrusFront;     // the fronts (see frontCover())
uniform highp vec4 cirrusRadii;     // the observer's radius; the radius of the layer; (r - observer)(r + observer); the size
                                    // of a pixel (radians)
uniform highp vec2 cirrusCover;     // the cover; the vertical optical depth of the densest cirrus
uniform highp vec3 cirrusSun;       // unit vectors towards the Sun and the Moon
uniform highp vec3 cirrusMoon;
uniform highp vec3 cirrusSunLog[33];// the logarithm of the sunlight on the layer over 4 pi, against the altitude of the Sun
                                    // seen from the place: from -5 degrees in steps of 0.5 degree
uniform highp vec3 cirrusMoonLight; // the moonlight over 4 pi
uniform highp vec3 cirrusSky;       // the light of the sky, the ground and the towns over 4 pi
uniform highp vec3 cirrusAir;       // foreground air radiance at large optical depth
uniform highp vec3 cirrusTau;       // the optical depth of the air at the zenith

// Ice crystals: a strong forward peak, light to all sides, and the halo of 22 degrees, red inside and sharp there
highp vec3 cirrusPhase(highp float cosine)
{
	highp float g = 0.75;
	highp float forward = (1. - g*g)/pow(1. + g*g - 2.*g*cosine, 1.5);
	highp vec3 d = degrees(acos(clamp(cosine, -1., 1.))) - vec3(21.7, 22.0, 22.3);
	highp vec3 halo = mix(exp(-d*d/0.0625), exp(-max(d, 0.)/1.5), step(0., d));
	return vec3(0.3 + 0.7*forward) + 3.*halo;
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
	if (altAz.z < -0.015)
	{
		FRAG_COLOR = vec4(0., 0., 0., 1.);
		return;
	}
	highp vec3 dir = vec3(altAz.xy, max(altAz.z, 0.));
	highp float b = cirrusRadii.x*dir.z;
	highp float t = cirrusRadii.z/(b + sqrt(b*b + cirrusRadii.z));
	highp vec3 p = vec3(dir.xy*t, cirrusRadii.x + dir.z*t);
	highp vec3 up = p/length(p);
	highp float slant = 1./max(dot(up, dir), 0.05);

	highp vec3 earth = p.x*cloudSouth + p.y*cloudEast + p.z*cloudUp;
	highp vec2 still = 2.*cirrusRadii.y*earth.xy/(length(earth) + abs(earth.z));
	highp vec2 map = vec2(cloudTurn.x*still.x - cloudTurn.y*still.y, cloudTurn.y*still.x + cloudTurn.x*still.y) + cloudTurn.zw + vec2(4711.);
	highp float cover = cirrusFront.x > 0. ? frontCover(frontMap(still), cirrusFront) : cirrusCover.x;
	highp float density = cirrusDensity(map, t*cirrusRadii.w*slant, cover);
	highp float T = exp(-cirrusCover.y*density*slant);
	if (T > 0.999)
	{
		FRAG_COLOR = vec4(0., 0., 0., 1.);
		return;
	}

	// The sunlight on the place: the Sun stands higher there towards it, by the curve of the Earth
	highp float localSun = degrees(asin(clamp(dot(up, cirrusSun), -1., 1.)));
	highp float x = clamp((localSun + 5.)/0.5, 0., 31.999);
	highp int i = int(x);
	highp vec3 sunlight = exp(mix(cirrusSunLog[i], cirrusSunLog[i + 1], x - float(i)));
	// Scattered once, the light goes where the crystals send it: mostly forwards, around the Sun, and little to the sides.
	// That is all there is to a thin veil, which is faint away from the Sun. The denser tufts and streaks scatter the
	// light many times, until it has lost its direction and comes out below to all sides alike: this is what makes
	// cirrus white all over the sky. Of the light that falls on a layer of the optical depth tau, the part
	// 1/(1 + 0.75 tau (1 - g)) comes through (two-stream, as for the other clouds; g = 0.75 for ice), less the part
	// that was not scattered at all, exp(-tau/mu). (sunlight is E/(4 pi): times 4 mu for a white surface below the layer)
	highp float tauUp = cirrusCover.y*density;
	highp float muSun = max(dot(up, cirrusSun), 0.03), muMoon = max(dot(up, cirrusMoon), 0.03);
	highp float many = 1. - exp(-tauUp);
	highp float diffusedSun = 4.*muSun*max(1./(1. + 0.19*tauUp) - exp(-tauUp/muSun), 0.)*many;
	highp float diffusedMoon = 4.*muMoon*max(1./(1. + 0.19*tauUp) - exp(-tauUp/muMoon), 0.)*many;
	highp vec3 light = (1. - T)*(sunlight*cirrusPhase(dot(dir, cirrusSun)) + cirrusMoonLight*cirrusPhase(dot(dir, cirrusMoon)) + cirrusSky)
	                 + sunlight*diffusedSun + cirrusMoonLight*diffusedMoon;
	// The air below the cirrus dims them (Rozenberg's air mass)
	highp float sine = max(altAz.z, 0.);
	highp vec3 way = exp(-cirrusTau/(sine + 0.025*exp(-11.*sine)));
	// Foreground air adds light, but cannot make the solar/lunar disk behind the
	// cloud visible again. Use the same transmission as the cloud optical depth.
	highp float moonCos = dot(dir, cirrusMoon);
	highp vec3 sky = foregroundSky(ndcPos.xy, cirrusAir + cirrusMoonLight*0.75*(1. + moonCos*moonCos));
	highp vec3 air = (1. - way)*sky;
	// light already contains the factor (1 - T).
	FRAG_COLOR = vec4(light*way + air*(1. - T), T);
}
)";
	if (!newProgram->addShaderFromSourceCode(QOpenGLShader::Vertex, vert)
	    || !newProgram->addShaderFromSourceCode(QOpenGLShader::Fragment, frag))
	{
		qWarning().noquote() << "CloudMgr: error in the shaders of the cirrus:\n" << newProgram->log();
		return;
	}
	newProgram->bindAttributeLocation("vertex", VertexAttribIndex);
	if (!StelPainter::linkProg(newProgram.get(), "CloudMgr cirrus program"))
		return;
	cirrusProgram = std::move(newProgram);
}

void CloudMgr::drawCirrus(StelCore* core, const StelProjector& prj)
{
	if (!cirrusProjector || !prj.isSameProjection(*cirrusProjector))
	{
		buildCirrusProgram(prj);
		cirrusProjector = core->getProjection(StelCore::FrameAltAz, StelCore::RefractionAuto);
	}
	if (!cirrusProgram)
		return;
	const StelSkyDrawer* drawer = core->getSkyDrawer();
	const StelLocation& location = core->getCurrentLocation();
	const SolarSystem* ssystem = GETSTELMODULE(SolarSystem);
	QOpenGLShaderProgram& p = *cirrusProgram;
	p.bind();
	bindAtmosphere(p);
	p.setUniformValue("projectionMatrixInverse", prj.getProjectionMatrix().toQMatrix().inverted());
	prj.setUnProjectUniforms(p);

	const double phi = static_cast<double>(location.getLatitude())*M_PI/180.;
	const double lambda = static_cast<double>(location.getLongitude())*M_PI/180.;
	const auto vec = [](const Vec3d& v) {return QVector3D(static_cast<float>(v[0]), static_cast<float>(v[1]), static_cast<float>(v[2]));};
	p.setUniformValue("cloudSouth", vec(Vec3d(std::sin(phi)*std::cos(lambda), std::sin(phi)*std::sin(lambda), -std::cos(phi))));
	p.setUniformValue("cloudEast", vec(Vec3d(-std::sin(lambda), std::cos(lambda), 0.)));
	p.setUniformValue("cloudUp", vec(Vec3d(std::cos(phi)*std::cos(lambda), std::cos(phi)*std::sin(lambda), std::sin(phi))));
	const CloudShape::Motion motion = motionAt(core, core->getJD(), CirrusWind);
	setCirrusMotion(p, "cloudTurn", motion);
	setFrontMotion(p, motionAt(core, core->getJD(), 0));
	bindTileTexture(2);
	p.setUniformValue("cloudTile", 2);
	const double r0 = EarthRadius + std::max(0., location.altitude*0.001), rc = EarthRadius + cirrusHeight;
	p.setUniformValue("cirrusRadii", QVector4D(static_cast<float>(r0), static_cast<float>(rc), static_cast<float>((rc - r0)*(rc + r0)),
	                                           static_cast<float>(1./static_cast<double>(prj.getPixelPerRadAtCenter()))));
	p.setUniformValue("cirrusCover", QVector2D(static_cast<float>(currentCirrusCover), static_cast<float>(cirrusOpticalDepth)));
	p.setUniformValue("cirrusFront", frontUniform(cirrusFront));
	p.setUniformValue("cloudFrontDrift", QVector2D(static_cast<float>(motion.frontU), static_cast<float>(motion.frontV)));

	// The light: the Sun through the air down to the layer, the Moon, the sky, the ground and the towns
	const double scale = static_cast<double>(drawer->getPhotoSceneScale());
	const double extinction = drawer->getEffectiveExtinctionCoefficient();
	Vec3d sun = ssystem->getSun()->getAltAzPosGeometric(core);
	sun.normalize();
	Vec3d moon = ssystem->getMoon()->getAltAzPosGeometric(core);
	moon.normalize();
	p.setUniformValue("cirrusSun", vec(sun));
	p.setUniformValue("cirrusMoon", vec(moon));
	QVector3D sunLog[33];
	for (int k = 0; k < 33; ++k)
	{
		const CloudLight::Rgb t = CloudLight::sunlightTransmission((-5. + 0.5*k)*M_PI/180., cirrusHeight, extinction);
		const auto lg = [scale](double v) {return static_cast<float>(std::log(std::max(133000.*v*scale/(4.*M_PI), 1e-30)));};
		sunLog[k] = QVector3D(lg(t[0]), lg(t[1]), lg(t[2]));
	}
	p.setUniformValueArray("cirrusSunLog", sunLog, 33);
	const double moonLux = moon[2] > 0. ? 2.54e-6*std::pow(10., -0.4*static_cast<double>(ssystem->getMoon()->getVMagnitudeWithExtinction(core))) : 0.;
	const Vec3f moonTint = drawer->getPhotoExtinctionTint(moon);
	p.setUniformValue("cirrusMoonLight", QVector3D(static_cast<float>(moonLux*scale/(4.*M_PI)*static_cast<double>(moonTint[0])),
	                                               static_cast<float>(moonLux*scale/(4.*M_PI)*static_cast<double>(moonTint[1])),
	                                               static_cast<float>(moonLux*scale/(4.*M_PI)*static_cast<double>(moonTint[2]))));
	const double sunAltitude = std::asin(std::clamp(sun[2], -1., 1.));
	const double skyLux = CloudLight::skyIlluminance(sunAltitude);
	const CloudLight::Rgb skyColor = CloudLight::skyColor(sunAltitude);
	const CloudLight::Rgb sunGround = CloudLight::sunlightTransmission(sunAltitude, 0., extinction);
	const Vec3f lamp = LightDomes::lampColor(LightDomes::LampMixed);
	const double artificial = drawer->getLightPollutionLuminance();
	QVector3D sky, air;
	for (int c = 0; c < 3; ++c)
	{
		// half of the sky above, the ground of an albedo of 0.2 below, and the light of the towns
		const double ground = 0.2*(skyLux*skyColor[static_cast<std::size_t>(c)] + 133000.*sunGround[static_cast<std::size_t>(c)]*std::max(sun[2], 0.));
		const double value = (0.5*skyLux*skyColor[static_cast<std::size_t>(c)] + ground)/(4.*M_PI) + 2.*artificial*static_cast<double>(lamp[c]);
		sky[c] = static_cast<float>(value*scale);
		air[c] = static_cast<float>((skyLux*skyColor[static_cast<std::size_t>(c)]/(4.*M_PI)
		                             + artificial*static_cast<double>(lamp[c]))*scale);
	}
	p.setUniformValue("cirrusSky", sky);
	p.setUniformValue("cirrusAir", air);
	// the air at the zenith, redder the more it dims
	const float tau = static_cast<float>(extinction/1.086);
	p.setUniformValue("cirrusTau", QVector3D(0.62f*tau, tau, 1.75f*tau));

	auto& gl = *QOpenGLContext::currentContext()->functions();
	gl.glEnable(GL_BLEND);
	gl.glBlendFuncSeparate(GL_ONE, GL_SRC_ALPHA, GL_ZERO, GL_ONE);
	drawQuad();
	p.release();
}

bool CloudMgr::drawToLowResBuffer()
{
	auto& gl = *QOpenGLContext::currentContext()->functions();
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
uniform sampler2D cloudTex;
void main()
{
	FRAG_COLOR = texture2D(cloudTex, texCoord);
}
)";
		if (composite->addShaderFromSourceCode(QOpenGLShader::Vertex, vert)
		    && composite->addShaderFromSourceCode(QOpenGLShader::Fragment, frag))
		{
			composite->bindAttributeLocation("vertex", VertexAttribIndex);
			if (StelPainter::linkProg(composite.get(), "CloudMgr composite program"))
				compositeProgram = std::move(composite);
		}
		if (!compositeProgram)
		{
			qWarning() << "CloudMgr: cannot build the program that scales up the clouds";
			flagHalfResolution = false;
			return false;
		}
	}
	GLint previousBuffer = 0;
	gl.glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousBuffer);
	GLint viewport[4];
	gl.glGetIntegerv(GL_VIEWPORT, viewport);
	const QSize size(std::max(1, (viewport[2] + lowResDivisor - 1)/lowResDivisor), std::max(1, (viewport[3] + lowResDivisor - 1)/lowResDivisor));
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
	drawQuad();
	gl.glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previousBuffer));
	gl.glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
	return true;
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
