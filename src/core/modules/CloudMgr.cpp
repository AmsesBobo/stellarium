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
// CloudShape::cirrusDensity()
highp float cirrusDensity(highp vec2 map, highp float foot, highp float cover)
{
	highp vec2 m = vec2(0.819*map.x - 0.574*map.y, 0.574*map.x + 0.819*map.y);
	// the fibres bend
	m += 9.*vec2(cirrusNoise(map/60. + cirrusTexturePhase[0] + 0.5, max(log2(foot*1024./60.), 0.)), cirrusNoise(map/60. + cirrusTexturePhase[0] + 0.25, max(log2(foot*1024./60.), 0.))) - 4.5;
	highp float patch = cirrusNoise(map/190. + cirrusTexturePhase[1], max(log2(foot*1024./190.), 0.));
	highp float area = clamp((patch - 0.85 + 0.9*cover)/0.2, 0., 1.);
	highp float fibres = 0.5*cirrusNoise(vec2(m.x/40., m.y/10.) + cirrusTexturePhase[2], max(log2(foot*1024./10.), 0.))
	                   + 0.3*cirrusNoise(vec2(m.x/12. + 0.37, m.y/3. + 0.71) + cirrusTexturePhase[3], max(log2(foot*1024./3.), 0.))
	                   + 0.2*cirrusNoise(vec2(m.x/4. + 0.13, m.y/0.9 + 0.29) + cirrusTexturePhase[4], max(log2(foot*1024./0.9), 0.));
	return area*smoothstep(0.4, 0.68, fibres + 0.3*(area - 0.5));
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
		texturePhase(x*layer.stretch()/(layer.size*CloudShape::TileSize), y/(layer.size*CloudShape::TileSize)),
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
		texturePhase(u/CloudShape::CirrusScaleX[0], v/CloudShape::CirrusScaleY[0]),
		texturePhase(u/CloudShape::CirrusScaleX[1], v/CloudShape::CirrusScaleY[1]),
		texturePhase(u/CloudShape::CirrusScaleX[2], v/CloudShape::CirrusScaleY[2])
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
	setFlagCirrus(conf->value("clouds/flag_cirrus", false).toBool());
	setFlagManualWind(conf->value("clouds/flag_manual_wind", false).toBool());
	setCirrusCover(conf->value("clouds/cirrus_cover", 0.4).toDouble());
	setCirrusHeight(conf->value("clouds/cirrus_height", 8.).toDouble());
	setCirrusOpticalDepth(conf->value("clouds/cirrus_optical_depth", 0.15).toDouble());
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
	{1.2, 0.8, 1., 0., 0.25},    // cumulus humilis: fair weather heaps, white in the sunlight
	{1.0, 1.2, 0.9, 0.4, 0.6},   // stratocumulus: large flat masses close together, grey below
	{3.5, 0.4, 0.6, 0.6, 0.55},  // altocumulus: small, thin flat elements higher up
};
}

int CloudMgr::getType() const
{
	const auto near = [](double a, double b) {return std::fabs(a - b) < 0.005;};
	for (int i = 0; i < 3; ++i)
	{
		const CloudType& t = CloudTypes[i];
		if (near(layers[edited].base, t.base) && near(layers[edited].size, t.size) && near(layers[edited].density, t.density) && near(layers[edited].flatness, t.flatness))
			return i + 1;
	}
	return TypeCustom;
}

void CloudMgr::setType(int type)
{
	if (type < TypeCumulus || type > TypeAltocumulus || type == getType())
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
		fronts[i].scale = CloudShape::solarCoverScale(layers[i], heating);
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
const int CloudSubsteps = 6;
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
uniform highp vec2 cloudBoil;       // of the fine lobes (km)
uniform highp sampler3D cloudBillow; // the billows of the cauliflower tops (see CloudShape::computeBillows())
uniform highp vec3 cloudBillowScale; // 1/km of their tile, how deep they eat in (km), their change (in tiles)
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
	highp float life = mix(smoothstep(0.25, 0.75, grow), 0.5, 0.65*shape.y);
	highp vec2 groupUV = map*formation.xy + phases[4];
	highp float broad = textureLod(cloudTile, groupUV, 2.).z;
	highp float detail = textureLod(cloudTile, map*formation.xy*2.3 + phases[9] + vec2(0.31, 0.57), 2.).z;
	highp float patches = 0.75*broad + 0.25*detail;
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
	highp vec2 drift = 0.3*size*(1. - 0.4*shape.y)*
		(vec2(textureLod(cloudTile, uv, 2.).z, textureLod(cloudTile, uv + vec2(0.37, 0.71), 2.).z) - 0.5);
	// At low coverage, fade the cloud body rather than eroding it down to detached
	// billow tops. Flat layers retain their more broken structure. See cohesiveWeather().
	highp float coherence = 1. - smoothstep(0.15, 0.5, shape.y);
	highp float fade = smoothstep(-1.25, -0.20, offset);
	highp float density = (0.45 + 0.55*life)*mix(1., fade, coherence)*presence;
	offset = mix(offset, max(offset, -0.20), coherence);
	return CloudWeather(vec3(offset, 0.65 + 0.35*life, density), drift);
}

CloudWeather cloudWeather(highp vec3 p)
{
	return cloudWeatherAt(p, cloudMap(p), cloudCover, cloudFront, cloudRadii.y, cloudBank, cloudShape, cloudSize, cloudFormation, cloudPhase);
}

// The light that the layer above lets through to a place, towards the light key: one look at the middle of its clouds
highp float otherShadow(highp vec3 p, highp vec3 key)
{
	if (otherRadius.x <= 0. || key.z < 0.02)
		return 1.;
	highp float middle = 0.12*otherRadius.y*otherShape.w;
	highp float r = length(p);
	highp vec3 q = p + key*(otherRadius.x + middle - r)/max(dot(p/r, key), 0.02);
	highp vec2 map = cloudMapAt(q, otherRadius.x, otherShift, otherTurn);
	CloudWeather weather = cloudWeatherAt(q, map, otherCover, otherFront, otherRadius.x, otherBank, otherShape, otherRadius.y, otherFormation, otherPhase);
	highp vec4 f = textureLod(cloudTile, vec2((map.x + weather.drift.x)*otherShape.x, map.y + weather.drift.y)*otherRadius.z + otherPhase[0], 1.5);
	highp float s = f.x + weather.growth.x;
	highp float height = otherRadius.y*otherShape.w*f.y*weather.growth.y;
	highp float round = s/pow(abs(s) + 0.01, 0.55);
	highp float plateau = 0.8*s/sqrt(s*s + 0.0625);
	highp float top = height*mix(round, plateau, otherShape.y);
	highp float under = otherRadius.y*otherShape.w*(0.012 + 0.008*f.z);
	// Optical thickness, including the oblique path through the upper layer. Unlike
	// a linear density subtraction this stays nonnegative even for dense clouds.
	highp float slant = 1./max(dot(q/length(q), key), 0.05);
	highp float tau = otherCover.w*weather.growth.z*max(top - under, 0.)*slant;
	highp float transmission = 0.15 + 0.85*exp(-tau);
	return mix(1., transmission, smoothstep(0.02, 0.08, key.z));
}

// Where a place of the map lies on the tile of the shapes, which flat clouds stretch
highp vec2 cloudTileAt(highp vec2 map)
{
	return vec2(map.x*cloudShape.x, map.y)*cloudTileScale.x + cloudPhase[0];
}

// The integral of the ramp of the density at the edge of a cloud, from 0 below a to 1 above b, up to x
highp float edgeIntegral(highp float x, highp float a, highp float b)
{
	return x < a ? 0. : x < b ? 0.5*(x - a)*(x - a)/(b - a) : 0.5*(b - a) + x - b;
}

// The shape S of the last place that cloudHeights() looked at: about 0.4 S (km at size 1) is the way across to the edge
// of the cloud, where the heights tell only the way up or down (a steep side of a cloud is near though its top is high)
highp float cloudLastShape = 0.;

// The heights over the base of the top and the underside of the cloud at a place (km, the top below 0 outside the clouds),
// from the shape at the level of detail lod with the local development parameters. near: how much the fine lobes count at the height z
highp vec2 cloudHeights(highp vec2 map, CloudWeather weather, highp float lod, highp float near, highp float z)
{
	map += weather.drift;
	highp vec4 f = textureLod(cloudTile, cloudTileAt(map), lod);
	highp float s = f.x + weather.growth.x;
	// Fine lobes, stronger at the edge of a cloud, which they tear up. They change with the height, or the sides of the
	// clouds would show grooves that run straight up
	if (near > 0.)
	{
		highp vec2 fine = (map + cloudBoil + z*vec2(1.2, -0.9))*cloudTileScale.w;
		// (they shape the clouds, but make none in the clear sky)
		highp float lobes = near*smoothstep(-0.15, 0.02, s);
		s += lobes*(0.04 + 0.10*(1. - clamp(s/0.3, 0., 1.)))*(textureLod(cloudTile, fine + cloudPhase[5], 0.).w - 0.62)
		   + lobes*0.025*(textureLod(cloudTile, fine*2.7 + cloudPhase[6], 0.).w - 0.62);
	}
	cloudLastShape = s;
	// a rounded top (a dome with steep sides, not a cone), or a flat one, and an underside that is flat but for its rim
	highp float height = cloudSize*cloudShape.w*f.y*weather.growth.y;
	highp float round = s/pow(abs(s) + 0.01, 0.55);
	// Smooth shoulders retain relief in stratocumulus and altocumulus instead of
	// clipping their tops into plates. Keep the CPU transmission model in sync.
	highp float plateau = 0.8*s/sqrt(s*s + 0.0625);
	highp float top = height*mix(round, plateau, cloudShape.y);
	// A common condensation level with only broad, shallow variation. Do not
	// imprint each rounded top or its rim on the underside. Mirror on the CPU.
	return vec2(top, cloudSize*cloudShape.w*(0.012 + 0.008*f.z));
}

// How deep the billows eat into the cloud at a place of the map, at a height over the base under the top there (km).
// None at the flat base; where they are finer than the footprint foot (km), only their mean
highp float billows(highp vec2 map, highp float height, highp float top, highp float foot)
{
	highp vec3 c = vec3(map, height)*cloudBillowScale.x + vec3(0., 0., cloudBillowScale.z);
	highp float lod = max(log2(foot*cloudBillowScale.x*96.), 0.);
	highp vec4 b = textureLod(cloudBillow, c, lod);
	// (nor just above it at the thin rim of a cloud, where they would cut round dents into the flat base)
	highp float up = smoothstep(0.08, 0.45, height/max(top, 0.01))*smoothstep(0.03, 0.12, height/cloudSize);
	return cloudBillowScale.y*up*(1. - (0.55*b.r + 0.3*b.g + 0.15*b.b));
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

	// The exposure: the moments that the camera averages, as many as the movement needs on the screen here
	highp float spread = length(cloudSmear);
	int samples = int(clamp(ceil(spread/(max(t0*cloudPixel, 1e-4)*4.)), 1., float(cloudSamples)));
	highp float jitter = samples > 1 ? fract(52.9829189*fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715)))) : 0.5;
	highp vec2 hitMoment = vec2(0., 0.);
	highp float bestMoment = 2., T = 0., thin = 0.;
	highp float hitT = -1.;
	CloudWeather hitWeather = CloudWeather(vec3(0.), vec2(0.));
	highp vec4 hitVolume[2];
	highp vec2 hitVolumeWeight = vec2(0.);
	// the steps between two moments blur the surface, so that they do not show
	highp float smear = spread/float(samples);
	highp float look = log2((1.5*ds + smear)/cloudCover.z);
	for (int moment = 0; moment < samples; ++moment)
	{
		// (the moments differ from pixel to pixel, so that the steps between them make grain, not stripes)
		highp float when = (float(moment) + jitter)/float(samples) - 0.5;
		cloudMoment = cloudSmear*when;
		highp float firstWeight = 0., firstT = 0.;
		CloudWeather firstWeather = CloudWeather(vec3(0.), vec2(0.));
		highp float depth = 0.;
		CloudWeather lastWeather = CloudWeather(vec3(0.), vec2(0.));
		highp vec4 volume[2];
		volume[0] = vec4(0.); volume[1] = vec4(0.);
		highp vec2 volumeWeight = vec2(0.);
		// The near stratum takes its place from the separate piece of cloud that weighs most in it (a billow in front
		// of another one, or a wisp before the body): their mean would lie in the air between them, where the
		// sunlight comes through, and outline the billows with bright rims
		highp vec4 piece = vec4(0.), bestPiece = vec4(0.);
		highp float pieceWeight = 0., bestPieceWeight = 0.;
		highp float lastD = 0., lastU = -1e9; // the distance to the edge at the previous short step
		// Through the layer, until the clouds are opaque. Long steps look for clouds;
		// short ones integrate opacity and the representative lighting position
		for (int k = 0; k < steps && depth < 7.; ++k)
		{
			highp float t = t0 + float(k)*ds;
			highp vec3 middle = vec3(altAz.xy*(t + 0.5*ds), r0 + altAz.z*(t + 0.5*ds));
			highp vec2 middleMap = cloudMap(middle);
			// A blurred look at the map tells whether a cloud may lie in this step, and how high it may reach
			// (the offset of the end of this step, against the one at its start: the clouds must not jump between steps)
			CloudWeather weatherEnd = cloudWeather(vec3(altAz.xy*(t + ds), r0 + altAz.z*(t + ds)));
			CloudWeather weatherStart = k == 0 ? cloudWeather(vec3(altAz.xy*t, r0 + altAz.z*t)) : lastWeather;
			lastWeather = weatherEnd;
			highp vec4 blurred = textureLod(cloudTile, cloudTileAt(middleMap + 0.5*(weatherStart.drift + weatherEnd.drift)), look);
			highp float shape = blurred.x + 0.5*(weatherStart.growth.x + weatherEnd.growth.x);
			if (max(weatherStart.growth.z, weatherEnd.growth.z) > 1e-5 && (t > 30. || shape > -0.13 && length(middle) - cloudRadii.y - 0.5*ds*altAz.z < cloudSize*cloudShape.w*blurred.y*pow(max(shape + 0.5, 0.), 0.7) + 0.05 + smear))
			{
				// (far away a step passes over whole clouds: they blur together rather than glitter)
				highp float foot = max(max(t*cloudPixel, ds*mix(0.1, 0.6, smoothstep(20., 60., t))), smear);
				highp float lod = max(log2(foot/cloudCover.z), 0.);
				highp float near = 1. - smoothstep(0.015, 0.05, foot/cloudSize);
				highp float e = max(0.012, foot);
				highp float dt = ds/float(CloudSubsteps);
				for (int j = 0; j < CloudSubsteps && depth < 7.; ++j)
				{
					highp float u = t + (float(j) + 0.5)*dt;
					highp vec3 q = vec3(altAz.xy*u, r0 + altAz.z*u);
					highp float height = length(q) - cloudRadii.y;
					CloudWeather weather = mixWeather(weatherStart, weatherEnd, (float(j) + 0.5)/float(CloudSubsteps));
					highp vec2 h = cloudHeights(cloudMap(q), weather, lod, near, height);
					// (the nearest surface: the top, or a side; there the billows bite in, also into the side of a cloud. The underside
					// stays flat: holes in it would let the low Sun light up round patches)
					highp float dTop = min(h.x - height, 0.4*cloudSize*cloudLastShape), dBase = height - h.y;
					// Ragged edges: noise eats into the top and the sides and leaves wisps (where it is finer than a pixel its mean,
					// nothing). Not into the underside, which the light below must see as it is
					if (abs(dTop) < 0.2*cloudSize)
					{
						highp vec2 rag = (cloudMap(q) + weather.drift + 0.5*cloudBoil + height*vec2(-0.8, 1.1))*(0.37*cloudTileScale.w);
						dTop += 0.07*cloudSize*(textureLod(cloudTile, rag + cloudPhase[7], max(log2(foot*1024.*0.37*cloudTileScale.w), 0.)).w - 0.62);
					}
					// The billows of the cauliflower top (they only take away: deeper inside, nothing to do)
					if (dTop > -e && dTop < cloudBillowScale.y)
						dTop -= billows(cloudMap(q) + weather.drift, height, h.x, foot);
					highp float d = min(dTop, dBase);
					// The density grows over the first few tens of meters into the cloud: the edges of a cumulus are crisp (the ragged
					// noise above still leaves soft wisps). Take its mean between this short step and the last, where the distance to
					// the edge goes from one to the other: a crisp edge that falls between them is neither lost nor shows the steps
					highp float a = -e, b = e + 0.004*cloudSize;
					highp float inside = clamp((d - a)/(b - a), 0., 1.);
					if (u - lastU < 1.5*dt && abs(d - lastD) > 1e-4)
						inside = (edgeIntegral(d, a, b) - edgeIntegral(lastD, a, b))/(d - lastD);
					lastD = d; lastU = u;
					// Weight the lighting position by each sample's visible opacity. A single
					// threshold crossing jumped between steps and drew rings on the bases.
					highp float opticalDepth = cloudCover.w*weather.growth.z*dt*inside;
					highp float weight = exp(-depth)*(1. - exp(-opticalDepth));
					// Two overlapping optical-depth strata, not a single surface hit.
					// Smooth weights avoid a visible switch when a ray crosses a stratum.
					highp float deeper = smoothstep(0.2, 1.8, depth + 0.5*opticalDepth);
					highp vec2 vw = weight*vec2(1. - deeper, deeper);
					highp vec4 samplePoint = vec4(u, height, h.x, lod);
					volume[0] += vw.x*samplePoint;
					if (inside < 0.02)
					{
						if (pieceWeight > bestPieceWeight) {bestPiece = piece; bestPieceWeight = pieceWeight;}
						piece = vec4(0.); pieceWeight = 0.;
					}
					piece += vw.x*samplePoint;
					pieceWeight += vw.x;
					volume[1] += vw.y*samplePoint;
					volumeWeight += vw;
					depth += opticalDepth;
					firstWeight += weight;
					firstT += weight*u;
					firstWeather.growth += weight*weather.growth;
					firstWeather.drift += weight*weather.drift;
				}
			}
		}
		if (pieceWeight > bestPieceWeight) {bestPiece = piece; bestPieceWeight = pieceWeight;}
		if (bestPieceWeight > 1e-8)
			volume[0] = bestPiece*(volumeWeight.x/bestPieceWeight);
		// (a thick cloud is opaque: the Sun must not shine through where the way stopped)
		highp float through = exp(-depth)*(1. - smoothstep(5., 6.9, depth));
		T += through;
		thin += through*(1. - through);
		// the light of the clouds comes from the moment nearest to the middle of the exposure that met a cloud
		if (firstWeight > 1e-6 && abs(when) < bestMoment)
		{
			bestMoment = abs(when);
			hitMoment = cloudMoment;
			hitT = firstT/firstWeight;
			hitWeather = CloudWeather(firstWeather.growth/firstWeight, firstWeather.drift/firstWeight);
			hitVolume[0] = volume[0]/max(volumeWeight.x, 1e-8);
			hitVolume[1] = volume[1]/max(volumeWeight.y, 1e-8);
			hitVolumeWeight = volumeWeight/firstWeight;
		}
	}
	if (hitT < 0.)
	{
		FRAG_COLOR = vec4(0., 0., 0., 1.);
		return;
	}
	T /= float(samples);
	thin /= float(samples);
	cloudMoment = hitMoment;

	// Integrate scattering from two opacity-weighted parts of the visible volume.
	// There is no surface normal and no separate side/underside material.
	highp vec3 p = vec3(altAz.xy*hitT, r0 + altAz.z*hitT);
	highp float sunCos = dot(altAz, cloudSun), moonCos = dot(altAz, cloudMoon);
	highp float keyIsSun = step(0.5, dot(cloudKey, cloudSun));
	highp float towardKey = smoothstep(0.3, 0.9, dot(altAz, cloudKey));
	highp float shadowWeight = (1. - smoothstep(0.15, 0.3, hitT*cloudPixel))*smoothstep(-0.05, 0.02, cloudKey.z);
	highp float upper = otherShadow(p, cloudKey)*cirrusShadow(p, cloudKey);
	// Weather varies much more slowly than density. Share two weather anchors
	// between the light rays; each stratum still samples its own cloud geometry.
	highp float middleDistance = 0.32*cloudSize, endDistance = 1.28*cloudSize;
	CloudWeather middleWeather = hitWeather, endWeather = hitWeather;
	if (shadowWeight > 0.)
	{
		middleWeather = cloudWeather(p + middleDistance*cloudKey);
		endWeather = cloudWeather(p + endDistance*cloudKey);
	}
	highp vec2 sunWay = cloudSun.xy/max(length(cloudSun.xy), 0.01);
	highp vec3 light = vec3(0.);
	// Single scattering must include extinction on the incoming light path.
	// Accumulate it with the same visible-volume weights as the diffuse light.
	highp float singleTransmission = 0.;
	for (int v = 0; v < 2; ++v)
	{
		highp float contribution = hitVolumeWeight[v];
		if (contribution < 1e-6) continue;
		highp vec4 point = hitVolume[v];
		highp vec3 start = vec3(altAz.xy*point.x, r0 + altAz.z*point.x);
		highp float shadow = 1.;
		// When the light march is subpixel, use the view transmission as a
		// forward-direction approximation instead of unattenuated illumination.
		highp float directTransmission = T;
		if (shadowWeight > 0.)
		{
			highp float seen = 0., distance = 0., stepLength = 0.02*cloudSize;
			highp float lightNear = 1. - smoothstep(0.015, 0.05, max(hitT*cloudPixel, 0.012)/cloudSize);
			highp float lastLight = 0.03; // the distance to the edge at the last step (the start lies in the cloud)
			// (where the long steps lie along the way differs a little from pixel to pixel: where the way leaves a cloud between
			// two of them, the light then grows in a fine grain, not in bands that follow the edge of the cloud; the short
			// first steps, which draw the light and shade of the billows, stay put)
			highp float lightJitter = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233)))*43758.5453);
			for (int k = 0; k < 10; ++k)
			{
				highp float reach = distance + mix(0.5, lightJitter, smoothstep(2.5, 4.5, float(k)))*stepLength;
				CloudWeather weather = reach < middleDistance
					? mixWeather(hitWeather, middleWeather, reach/middleDistance)
					: mixWeather(middleWeather, endWeather, clamp((reach - middleDistance)/(endDistance - middleDistance), 0., 1.));
				highp vec3 q = start + reach*cloudKey;
				highp float height = length(q) - cloudRadii.y;
				// (the first steps see the fine lobes, so that each billow has a lit side and a shaded one)
				highp vec2 h = cloudHeights(cloudMap(q), weather, point.w + 0.45*max(float(k) - 1., 0.), k < 4 ? lightNear : 0., height);
				highp float dLight = min(h.x - height, 0.4*cloudSize*cloudLastShape);
				// (the billows shade each other)
				if (k < 4 && dLight < cloudBillowScale.y)
					dLight -= billows(cloudMap(q) + weather.drift, height, h.x, stepLength);
				// (the base a little lower than for the eye: the samples that make a base visible lie partly in its soft lower
				// edge, and a low Sun must not reach them by grazing just under the flat base)
				dLight = min(dLight, height - h.y + 0.02);
				// (the mean density from the last step to this one, where the distance to the edge goes from one to the other:
				// the share of the way that lies in the cloud grows smoothly as the edge moves)
				highp float la = -0.01, lb = 0.025 + 0.1*stepLength;
				highp float inside = abs(dLight - lastLight) > 1e-4 ? (edgeIntegral(dLight, la, lb) - edgeIntegral(lastLight, la, lb))/(dLight - lastLight)
				                                                    : clamp((dLight - la)/(lb - la), 0., 1.);
				lastLight = dLight;
				seen += stepLength*weather.growth.z*inside;
				distance += stepLength;
				stepLength *= 1.45;
			}
			directTransmission = mix(T, exp(-cloudCover.w*seen), shadowWeight);
			highp float tau = 16.*cloudShape.z*seen/cloudSize;
			highp float around = 1. - towardKey;
			highp float scattered = (exp(-tau) + around*(0.1*exp(-0.6*tau) + 0.03*exp(-0.12*tau)))/(1. + 0.5*around);
			shadow = mix(1., scattered, shadowWeight);
		}
		singleTransmission += contribution*directTransmission;
		shadow *= upper;
		// Broad diffuse transport retains depth without tracing each rounded top
		// onto the base. Direct light is determined by the density light march.
		highp vec4 field = textureLod(cloudTile, cloudTileAt(cloudMap(start) + hitWeather.drift), max(point.w + 3., 4.));
		highp float broadDepth = cloudSize*cloudShape.w*field.y*hitWeather.growth.y;
		highp float diffuseDepth = mix(max(point.z - point.y, 0.), broadDepth, 0.85);
		highp float through = 1./(1. + 6.75*cloudShape.z*hitWeather.growth.z*diffuseDepth/cloudSize);
		highp vec3 ambient = 0.5*mix(cloudBelow, cloudAbove, through) + cloudCity;
		highp vec3 sunlight = cloudSunlight(dot(start.xy, sunWay)*57.3/7433. + 0.35*point.y);
		// A broad forward lobe for multiply scattered light. The narrow silver
		// lining below uses the separately integrated incoming transmission.
		highp float sunPhase = 0.65 + 0.35*exp(-(1. - sunCos)/0.25);
		highp float moonPhase = 0.65 + 0.35*exp(-(1. - moonCos)/0.25);
		highp float scatteredLight = shadow;
		light += contribution*(ambient
			+ sunlight*sunPhase*mix(1., scatteredLight, keyIsSun)
			+ sideMoon*moonPhase*mix(scatteredLight, 1., keyIsSun));
	}

	// The thin edges scatter the light of the Sun and the Moon forwards (Henyey-Greenstein with g = 0.85)
	// (the edges light up only within some 20 degrees of them: further away the bodies of the clouds outshine them)
	// The non-key source has no separate light march. Its forward contribution
	// uses view transmission; the dominant source uses the sampled incoming path.
	highp vec3 silver = mix(T, singleTransmission, keyIsSun)*silverSun*(0.2775/pow(1.7225 - 1.7*sunCos, 1.5))*smoothstep(0.88, 0.985, sunCos)
	                  + mix(singleTransmission, T, keyIsSun)*silverMoon*(0.2775/pow(1.7225 - 1.7*moonCos, 1.5))*smoothstep(0.88, 0.985, moonCos);
	// The thin parts send back less light than the body of a cloud, which has more orders of scattering: away from the
	// light the edges fade into the sky rather than outline the clouds
	highp float forwardLight = max(smoothstep(0.88, 0.985, sunCos), smoothstep(0.88, 0.985, moonCos));
	light *= mix(0.45 + 0.55*(1. - T*T), 1., forwardLight);
	// Keep the partial-transmission edge envelope, but attenuate it by the
	// incoming path above. Previously shadowed billows could glow as bright
	// contours solely because their view transmission happened to be near 0.5.
	light += silver*thin*upper;

	// The way through the air dims the cloud and adds the light of the air (Rayleigh phase function)
	highp vec3 way = exp(-airExtinction*hitT);
	highp vec3 sky = foregroundSky(ndcPos.xy, airSky + airMoon*0.75*(1. + moonCos*moonCos));
	highp vec3 air = (1. - way)*sky;
	// Haze is emitted in front of the cloud; it does not open a path through it.
	// Keep background transmission equal to T, including at the horizon. Fading
	// opacity with atmospheric extinction also revealed the Sun/Moon behind opaque
	// clouds. The air term above supplies the foreground haze independently.
	highp float hidden = 1. - T;
	// out = source + scene x alpha
	FRAG_COLOR = vec4((light*way + air)*hidden, T);
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
	p.setUniformValue("cloudBelow", scene(light.below));
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
	                                                          static_cast<float>(layer.size*CloudShape::TileSize/CloudShape::TileTexels),
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
	                                                           static_cast<float>(1./(layer.size*CloudShape::TileSize))));
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
	p.setUniformValue("cloudPixel", static_cast<float>((flagHalfResolution ? 2. : 1.)/static_cast<double>(prj.getPixelPerRadAtCenter())));
	bindTileTexture(2);
	p.setUniformValue("cloudTile", 2);
	bindBillowTexture(3);
	p.setUniformValue("cloudBillow", 3);
	p.setUniformValue("cloudTileScale", QVector4D(static_cast<float>(1./(layer.size*CloudShape::TileSize)), static_cast<float>(1./CloudShape::CoverScaleLarge),
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
	p.setUniformValue("cloudBoil", QVector2D(static_cast<float>(motion.boilX), static_cast<float>(motion.boilY)));
	// The billows: 1/km of their tile, how deep they eat in (km, less in flat clouds), and their slow change (the third
	// coordinate of the tile moves on as the fine lobes boil)
	p.setUniformValue("cloudBillowScale", QVector3D(static_cast<float>(1./(layer.size*CloudShape::BillowScale)),
	                                                static_cast<float>(CloudShape::BillowDepth*layer.size*(1. - 0.7*layer.flatness)),
	                                                static_cast<float>(std::fmod(0.35*motion.boilX/(layer.size*CloudShape::BillowScale), 1.))));
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
	highp vec3 light = (1. - T)*(sunlight*cirrusPhase(dot(dir, cirrusSun)) + cirrusMoonLight*cirrusPhase(dot(dir, cirrusMoon)) + cirrusSky);
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
