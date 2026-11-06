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
	return "uniform sampler2D cloudTile;        // the height map (see CloudShape)\n"
	       "uniform highp vec2 cloudFrontDrift; // where the fronts lie on the tile\n"
	       "// The cover of the fronts at a place of the map without the shift of a layer. front: on, the shift across the\n"
	       "// fronts (km), the bias of the cover, its scale\n"
	       "highp float frontCover(highp vec2 map, highp vec4 front)\n"
	       "{\n"
	       "\thighp float bend = " + n(CloudShape::FrontBend) + "*(textureLod(cloudTile, map/" + n(CloudShape::FrontBendScale) + " + vec2(0.1, 0.6), 1.).z - 0.5);\n"
	       "\thighp vec2 uv = vec2((" + n(CloudShape::FrontAxisCos) + "*map.x - (" + n(CloudShape::FrontAxisSin) + ")*map.y)/" + n(CloudShape::FrontAlong)
	       + ", ((" + n(CloudShape::FrontAxisSin) + ")*map.x + " + n(CloudShape::FrontAxisCos) + "*map.y + bend + front.y)/" + n(CloudShape::FrontAcross) + ") + cloudFrontDrift;\n"
	       "\treturn front.w*clamp(" + n(CloudShape::FrontGain) + "*(textureLod(cloudTile, uv, 1.).z - " + n(CloudShape::FrontMid) + ") + front.z, 0., "
	       + n(CloudShape::FrontMax) + ");\n"
	       "}\n";
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
	}
	edited = 0;
	setFlagHalfResolution(conf->value("clouds/flag_half_resolution", true).toBool());
	setFlagCirrus(conf->value("clouds/flag_cirrus", false).toBool());
	setFlagManualWind(conf->value("clouds/flag_manual_wind", false).toBool());
	setWindSpeed(conf->value("clouds/wind_speed", 8.).toDouble());
	setWindDirection(conf->value("clouds/wind_direction", 250.).toDouble());
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
	if (!qFuzzyCompare(value, windSpeed))
	{
		windSpeed = value;
		StelApp::immediateSave("clouds/wind_speed", value);
		emit windSpeedChanged(value);
	}
}

void CloudMgr::setWindDirection(double value)
{
	value = std::fmod(std::fmod(value, 360.) + 360., 360.);
	if (!qFuzzyCompare(value + 1., windDirection + 1.))
	{
		windDirection = value;
		StelApp::immediateSave("clouds/wind_direction", value);
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

CloudShape::Motion CloudMgr::motionAt(const StelCore* core, double jd) const
{
	CloudShape::Motion m = CloudShape::motion(jd);
	if (!flagManualWind)
		return m;
	// The wind set by hand: the map does not turn, and moves at the speed of the wind over the observer. Its way counts
	// from the start of a span of 10 days, to keep it short for the precision of the shader: the clouds jump at its ends
	const StelLocation& location = core->getCurrentLocation();
	const double phi = static_cast<double>(location.getLatitude())*M_PI/180.;
	const double lambda = static_cast<double>(location.getLongitude())*M_PI/180.;
	const double towards = (windDirection + 180.)*M_PI/180.;
	const double east = 0.001*windSpeed*std::sin(towards), north = 0.001*windSpeed*std::cos(towards);  // km/s
	// on the map: km there are larger by 2/(1 + |sin(latitude)|), and the north points to its center (or away from it in the
	// south)
	const double k = 2./(1. + std::fabs(std::sin(phi)));
	const double pole = phi >= 0. ? -1. : 1.;
	const double vx = k*(-std::sin(lambda)*east + pole*std::cos(lambda)*north);
	const double vy = k*(std::cos(lambda)*east + pole*std::sin(lambda)*north);
	const double seconds = (jd - 10.*std::floor(jd/10.))*86400.;
	m.cosine = 1.;
	m.sine = 0.;
	m.offsetX = -vx*seconds;
	m.offsetY = -vy*seconds;
	return m;
}

void CloudMgr::computeWind(const StelCore* core, double* mapX, double* mapY) const
{
	const StelLocation& location = core->getCurrentLocation();
	const double phi = static_cast<double>(location.getLatitude())*M_PI/180.;
	const double lambda = static_cast<double>(location.getLongitude())*M_PI/180.;
	const double radius = 2.*(EarthRadius + layers[0].base)*std::cos(phi)/(1. + std::fabs(std::sin(phi)));
	const double x = radius*std::cos(lambda), y = radius*std::sin(lambda);
	const double jd = core->getJD(), step = 60.;
	const CloudShape::Motion a = motionAt(core, jd), b = motionAt(core, jd + step/86400.);
	*mapX = ((b.cosine - a.cosine)*x - (b.sine - a.sine)*y + b.offsetX - a.offsetX)/step;
	*mapY = ((b.sine - a.sine)*x + (b.cosine - a.cosine)*y + b.offsetY - a.offsetY)/step;
}

void CloudMgr::update(double)
{
	if (!flagShow)
		return;
	const StelCore* core = StelApp::getInstance().getCore();
	const double longitude = static_cast<double>(core->getCurrentLocation().getLongitude());
	// With the date, weather fronts set the cover. The higher clouds of a front lie a little across it, and the cirrus
	// further, so that they may come first; the clouds that the Sun makes grow in the afternoon
	const double afternoon = CloudShape::afternoon(core->getJD(), longitude);
	for (int i = 0; i < LayerCount; ++i)
	{
		fronts[i].on = flagFollowDate;
		fronts[i].shift = layers[i].base > 2.5 ? 180. : 0.;
		fronts[i].bias = layers[i].base > 2.5 ? -0.1 : 0.;
		fronts[i].scale = afternoon;
	}
	cirrusFront.on = flagFollowDate;
	cirrusFront.shift = 420.;
	cirrusFront.bias = 0.1;
	cirrusFront.scale = 1.;
	// the cover over the observer
	if (flagFollowDate && tile.empty())
		tile = CloudShape::computeTile();
	const CloudShape::Motion here = motionAt(core, core->getJD());
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

	// The wind over the Earth from the wind on the map: turn it back, into the frame of the observer's meridian
	double mapX = 0., mapY = 0.;
	computeWind(core, &mapX, &mapY);
	const CloudShape::Motion m = motionAt(core, core->getJD());
	const double earthX = m.cosine*mapX + m.sine*mapY, earthY = -m.sine*mapX + m.cosine*mapY;
	const double lambda = longitude*M_PI/180.;
	const double north = static_cast<double>(core->getCurrentLocation().getLatitude()) >= 0. ? -1. : 1.;
	// (the map moves as the clouds do, so the wind is the opposite of the place on the map that passes over the observer)
	// (km on the map are larger than on the Earth away from the pole)
	const double scale = (1. + std::fabs(std::sin(static_cast<double>(core->getCurrentLocation().getLatitude())*M_PI/180.)))/2.;
	windEast = -scale*(-std::sin(lambda)*earthX + std::cos(lambda)*earthY);
	windNorth = -scale*north*(std::cos(lambda)*earthX + std::sin(lambda)*earthY);

	static const QStringList points = {q_("N"), q_("NE"), q_("E"), q_("SE"), q_("S"), q_("SW"), q_("W"), q_("NW")};
	const double from = std::atan2(-windEast, -windNorth)*180./M_PI;
	const int point = static_cast<int>(std::lround((from < 0. ? from + 360. : from)/45.)) % 8;
	QString covers = QString::number(currentCovers[0], 'f', 2);
	if (layerOn[1])
		covers = layerOn[0] ? q_("%1 and %2").arg(covers, QString::number(currentCovers[1], 'f', 2)) : QString::number(currentCovers[1], 'f', 2);
	const QString info = q_("Cover %1, wind %2 m/s from %3. Made up, not a forecast.")
	                     .arg(covers).arg(std::hypot(windEast, windNorth)*1000., 0, 'f', 0).arg(points[point]);
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
uniform highp vec4 cloudTileScale;  // 1/km: of the tile, of its two cover sizes, of the fine lobes
uniform highp float cloudSize;      // the scale of the clouds against those of the tile
uniform highp vec4 cloudTurn;       // the wind: the turn of the map, as cosine and sine; its offset (km)
uniform highp vec2 cloudGrow;       // the place of the noise of growth (tiles)
uniform highp vec2 cloudBoil;       // of the fine lobes (km)
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

// The place on the map: a polar stereographic projection, in km, of each hemisphere
highp vec2 cloudMoment = vec2(0., 0.);    // where the clouds are at this moment of the exposure, against its middle

highp vec2 cloudMapAt(highp vec3 p, highp float radius, highp vec2 shift)
{
	highp vec3 earth = p.x*cloudSouth + p.y*cloudEast + p.z*cloudUp;
	highp vec2 map = 2.*radius*earth.xy/(length(earth) + abs(earth.z));
	return vec2(cloudTurn.x*map.x - cloudTurn.y*map.y, cloudTurn.y*map.x + cloudTurn.x*map.y) + cloudTurn.zw + shift + cloudMoment;
}

highp vec2 cloudMap(highp vec3 p)
{
	return cloudMapAt(p, cloudRadii.y, cloudShift);
}

// What the cover and the growth add to the shape around a place of the map, with the cover offset of a layer, its fronts,
// its shift and its bank. It changes slowly: one value serves a whole step of the way. It never fills the clear parts of
// the map
highp float cloudOffsetAt(highp vec3 p, highp vec2 map, highp vec4 cover, highp vec4 front, highp vec2 shift, highp vec4 bank)
{
	highp float noise = 0.6*textureLod(cloudTile, map*cloudTileScale.y, 2.).z + 0.4*textureLod(cloudTile, map*cloudTileScale.z, 2.).z;
	highp float grow = textureLod(cloudTile, map*cloudGrowScale + cloudGrow, 2.).z;
	highp float offset = cover.x + cover.y*noise;
	// (with fronts, the cover of the place: see CloudShape::coverOffset())
	if (front.x > 0.)
	{
		highp float c = frontCover(map - shift, front);
		highp float fading = max(0.1 - c, 0.);
		offset = 1.1*(c - 0.45) + 2.*(noise - 0.5) - 150.*fading*fading;
	}
	offset = min(offset + cloudGrowStrength*(grow - 0.5), 0.45);
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
	return offset;
}

highp float cloudOffset(highp vec3 p)
{
	return cloudOffsetAt(p, cloudMap(p), cloudCover, cloudFront, cloudShift, cloudBank);
}

// The light that the layer above lets through to a place, towards the light key: one look at the middle of its clouds
highp float otherShadow(highp vec3 p, highp vec3 key)
{
	if (otherRadius.x <= 0. || key.z < 0.02)
		return 1.;
	highp float middle = 0.12*otherRadius.y*otherShape.w;
	highp float r = length(p);
	highp vec3 q = p + key*(otherRadius.x + middle - r)/max(dot(p/r, key), 0.02);
	highp vec2 map = cloudMapAt(q, otherRadius.x, otherShift);
	highp float s = textureLod(cloudTile, vec2(map.x*otherShape.x, map.y)*otherRadius.z, 1.5).x + cloudOffsetAt(q, map, otherCover, otherFront, otherShift, otherBank);
	return 1. - 0.9*otherShape.z*smoothstep(-0.05, 0.1, s);
}

// Where a place of the map lies on the tile of the shapes, which flat clouds stretch
highp vec2 cloudTileAt(highp vec2 map)
{
	return vec2(map.x*cloudShape.x, map.y)*cloudTileScale.x;
}

// The heights over the base of the top and the underside of the cloud at a place (km, the top below 0 outside the clouds),
// from the shape at the level of detail lod with the offset of the place. near: how much the fine lobes count at the height z
highp vec2 cloudHeights(highp vec2 map, highp float offset, highp float lod, highp float near, highp float z)
{
	highp vec4 f = textureLod(cloudTile, cloudTileAt(map), lod);
	highp float s = f.x + offset;
	// Fine lobes, stronger at the edge of a cloud, which they tear up. They change with the height, or the sides of the
	// clouds would show grooves that run straight up
	if (near > 0.)
	{
		highp vec2 fine = (map + cloudBoil + z*vec2(1.2, -0.9))*cloudTileScale.w;
		// (they shape the clouds, but make none in the clear sky)
		highp float lobes = near*smoothstep(-0.15, 0.02, s);
		s += lobes*(0.12 + 0.2*(1. - clamp(s/0.3, 0., 1.)))*(textureLod(cloudTile, fine, 0.).w - 0.62)
		   + lobes*0.1*(textureLod(cloudTile, fine*2.7, 0.).w - 0.62);
	}
	// a rounded top, or a flat one, and an underside that is flat but for its rim
	highp float height = cloudSize*cloudShape.w*f.y;
	highp float round = s/pow(abs(s) + 0.01, 0.3);
	highp float top = height*mix(round, 0.8*clamp(4.*s, -1., 1.), cloudShape.y);
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

	// The exposure: the moments that the camera averages, as many as the movement needs on the screen here
	highp float spread = length(cloudSmear);
	int samples = int(clamp(ceil(spread/(max(t0*cloudPixel, 1e-4)*4.)), 1., float(cloudSamples)));
	highp float jitter = samples > 1 ? fract(52.9829189*fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715)))) : 0.5;
	highp vec2 hitMoment = vec2(0., 0.);
	highp float bestMoment = 2., T = 0., thin = 0.;
	highp float hitT = -1., hitZ = 0., hitTop = 0., hitUnder = 0., hitLod = 0., hitNear = 0., hitOffset = 0.;
	// the steps between two moments blur the surface, so that they do not show
	highp float smear = spread/float(samples);
	highp float look = log2((1.5*ds + smear)/cloudCover.z);
	for (int moment = 0; moment < samples; ++moment)
	{
		// (the moments differ from pixel to pixel, so that the steps between them make grain, not stripes)
		highp float when = (float(moment) + jitter)/float(samples) - 0.5;
		cloudMoment = cloudSmear*when;
		highp float firstT = -1., firstZ = 0., firstTop = 0., firstUnder = 0., firstLod = 0., firstNear = 0., firstOffset = 0.;
		highp float depth = 0., lastOffset = 0.;
		// Through the layer, until the clouds are opaque; the first place in a cloud is the one we see. Long steps look for
		// the clouds, short ones find their surface
		for (int k = 0; k < steps && depth < 7.; ++k)
		{
			highp float t = t0 + float(k)*ds;
			highp vec3 middle = vec3(altAz.xy*(t + 0.5*ds), r0 + altAz.z*(t + 0.5*ds));
			highp vec2 middleMap = cloudMap(middle);
			// A blurred look at the map tells whether a cloud may lie in this step, and how high it may reach
			highp vec4 blurred = textureLod(cloudTile, cloudTileAt(middleMap), look);
			// (the offset of the end of this step, against the one at its start: the clouds must not jump between steps)
			highp float offsetEnd = cloudOffset(vec3(altAz.xy*(t + ds), r0 + altAz.z*(t + ds)));
			highp float offsetStart = k == 0 ? cloudOffset(vec3(altAz.xy*t, r0 + altAz.z*t)) : lastOffset;
			lastOffset = offsetEnd;
			highp float shape = blurred.x + 0.5*(offsetStart + offsetEnd);
			if (t > 30. || shape > -0.13 && length(middle) - cloudRadii.y - 0.5*ds*altAz.z < cloudSize*cloudShape.w*blurred.y*pow(max(shape + 0.5, 0.), 0.7) + 0.05 + smear)
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
					highp float offset = mix(offsetStart, offsetEnd, (float(j) + 0.5)/float(CloudSubsteps));
					highp vec2 h = cloudHeights(cloudMap(q), offset, lod, near, height);
					// (the density grows over the first 100 m or so into the cloud: the edges are soft and let the sky through)
					highp float inside = smoothstep(-e, e + 0.1*cloudSize, min(h.x - height, height - h.y));
					depth += cloudCover.w*dt*inside;
					if (firstT < 0. && depth > 0.03)
					{
						// the surface lies between this place and the last one
						highp float inner = u, outer = u - dt;
						for (int n = 0; n < 4; ++n)
						{
							highp float mid = 0.5*(inner + outer);
							q = vec3(altAz.xy*mid, r0 + altAz.z*mid);
							height = length(q) - cloudRadii.y;
							h = cloudHeights(cloudMap(q), offset, lod, near, height);
							if (min(h.x - height, height - h.y) > -e)
								inner = mid;
							else
								outer = mid;
						}
						firstT = inner;
						firstZ = height;
						firstTop = h.x;
						firstUnder = h.y;
						firstLod = lod;
						firstNear = near;
						firstOffset = offset;
					}
				}
			}
		}
		// (a thick cloud is opaque: the Sun must not shine through where the way stopped)
		highp float through = exp(-depth)*(1. - smoothstep(5., 6.9, depth));
		T += through;
		thin += through*(1. - through);
		// the light of the clouds comes from the moment nearest to the middle of the exposure that met a cloud
		if (firstT >= 0. && abs(when) < bestMoment)
		{
			bestMoment = abs(when);
			hitMoment = cloudMoment;
			hitT = firstT;
			hitZ = firstZ;
			hitTop = firstTop;
			hitUnder = firstUnder;
			hitLod = firstLod;
			hitNear = firstNear;
			hitOffset = firstOffset;
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

	// The normal of the surface, from the slope of the top. The clouds stand on the local level
	highp vec3 p = vec3(altAz.xy*hitT, r0 + altAz.z*hitT);
	// (over some tens of meters, a step of the tile coarser: the light spreads in the cloud, and the facets of the tile
	// between its texels would show)
	highp float delta = max(0.06*cloudSize, 3.*hitT*cloudPixel);
	// (the base goes over into the sides with the height)
	highp float fromBelow = 1. - smoothstep(0., 0.3*cloudSize, hitZ);
	highp vec2 map = cloudMap(p);
	highp float normalLod = hitLod + 1.;
	highp float centerTop = cloudHeights(map, hitOffset, normalLod, 0.5*hitNear, hitZ).x;
	highp vec2 slope = vec2(cloudHeights(cloudMap(p + vec3(delta, 0., 0.)), hitOffset, normalLod, 0.5*hitNear, hitZ).x, cloudHeights(cloudMap(p + vec3(0., delta, 0.)), hitOffset, normalLod, 0.5*hitNear, hitZ).x) - centerTop;
	highp vec3 normal = normalize(vec3(-slope/delta, 1.));

	// The hollows between the lobes see less of the sky
	highp float blurred = cloudHeights(map, hitOffset, hitLod + 3.5, 0., 0.).x;
	highp float open = clamp(1. - 1.5*(blurred - hitZ)/(0.3*cloudSize), 0.35, 1.);

	// The shadow from the clouds themselves and each other towards the light, in longer steps further away: scattered
	// many times, light gets into the clouds much deeper than their extinction says
	highp float shadow = 1.;
	// (looking towards the light we see the side in shadow, from which less of the light that went around comes)
	highp float towardKey = smoothstep(0.3, 0.9, dot(altAz, cloudKey));
	if (hitT*cloudPixel < 0.3 && cloudKey.z > -0.05)
	{
		highp float seen = 0., reach = 0.05*cloudSize;
		highp vec3 start = p + 0.03*cloudSize*mix(normal, vec3(0., 0., -1.), fromBelow);
		for (int k = 0; k < 6; ++k)
		{
			highp vec3 q = start + reach*cloudKey;
			highp float height = length(q) - cloudRadii.y;
			highp vec2 h = cloudHeights(cloudMap(q), hitOffset, hitLod + 0.7*float(k), 0., 0.);
			// (softly at the edges, so that the shadows have no facets)
			seen += 0.5*reach*smoothstep(-0.1*reach - 0.02, 0.4*reach + 0.05, min(h.x - height, height - h.y));
			reach *= 1.9;
		}
		// (scattered many times, light gets around the shadow: a sum of ever weaker and wider passes, after Wrenninge)
		highp float tau = 4.*cloudShape.z*seen/cloudSize;
		highp float around = 1. - towardKey;
		shadow = (exp(-tau) + around*(0.5*exp(-0.4*tau) + 0.25*exp(-0.15*tau)))/(1. + 0.75*around);
	}
	// and the shadow of the layer above
	highp float upper = otherShadow(p, cloudKey);
	shadow *= upper;

	highp float sunCos = dot(altAz, cloudSun), moonCos = dot(altAz, cloudMoon);
	highp float high = clamp(hitZ/max(hitTop, 0.05), 0., 1.);
	// Towards the Sun a cloud far away stands under a higher Sun, by the curve of the Earth, and so does a higher part of it
	highp vec2 sunWay = cloudSun.xy/max(length(cloudSun.xy), 0.01);
	highp vec3 sideSun = cloudSunlight(dot(p.xy, sunWay)*57.3/7433. + 0.35*hitZ);
	highp float keyIsSun = step(0.5, dot(cloudKey, cloudSun));
	// (soft on the side away from the light, which the cloud lets through)
	highp float lit = mix(0.25, 0.2, towardKey) + mix(0.75, 0.8, towardKey)*shadow;
	// (the light spreads in the cloud: its sides away from the light are less dark than on a solid body, but towards the
	// light we look at the side in shadow, through which little of it comes)
	highp float sunFacing = mix(clamp((dot(normal, cloudSun) + 0.5)/1.5, 0., 1.), 0.55, 0.35 - 0.25*smoothstep(0.3, 0.9, sunCos));
	highp float moonFacing = mix(clamp((dot(normal, cloudMoon) + 0.5)/1.5, 0., 1.), 0.55, 0.35 - 0.25*smoothstep(0.3, 0.9, moonCos));
	highp vec3 side = sideSky*(0.45 + 0.35*normal.z + 0.2*high)*open
	                + sideSun*sunFacing*mix(1., lit, keyIsSun)
	                + sideMoon*moonFacing*mix(lit, 1., keyIsSun);

	// From below the line of sight meets the underside. It reflects the ground and lets through what falls on its top, less
	// where the cloud is thick; towards the Sun that light goes on forwards
	highp float through = 1./(1. + 6.75*cloudShape.z*max(hitTop - hitZ, 0.)/cloudSize);
	highp float forwards = 1. + 3.*exp(-(1. - sunCos)/0.06);
	highp vec3 base = mix(cloudBelow, cloudAbove, through) + through*cloudAboveSun*forwards*shadow + cloudCity;
	// A low Sun lights the bumps of the underside from the side, and the light spreads in the cloud
	highp float lowSun = 1. - smoothstep(0.03, 0.3, abs(cloudSun.z));
	highp float wide = 0.25*cloudSize;
	highp float wideLod = hitLod + 3.;
	highp vec2 rimOut = vec2(cloudHeights(cloudMap(p + vec3(wide, 0., 0.)), hitOffset, wideLod, 0., 0.).x, cloudHeights(cloudMap(p + vec3(0., wide, 0.)), hitOffset, wideLod, 0., 0.).x)
	                  - cloudHeights(map, hitOffset, wideLod, 0., 0.).x;
	highp float facing = clamp(dot(normalize(vec3(-rimOut, 0.1*cloudSize)), cloudSun), 0., 1.);
	base += sideSun*lowSun*mix(lit, 1., 0.7)*(0.2 + 0.6*facing)*(1. + 3.*exp(-(1. - sunCos)/0.15));
	highp vec3 light = mix(side, base, fromBelow);

	// The thin edges scatter the light of the Sun and the Moon forwards (Henyey-Greenstein with g = 0.85)
	// (the edges light up only within some 20 degrees of them: further away the bodies of the clouds outshine them)
	highp vec3 silver = silverSun*(0.2775/pow(1.7225 - 1.7*sunCos, 1.5))*smoothstep(0.88, 0.985, sunCos)
	                  + silverMoon*(0.2775/pow(1.7225 - 1.7*moonCos, 1.5))*smoothstep(0.88, 0.985, moonCos);
	// The thin parts send back less light than the body of a cloud, which has more orders of scattering: away from the
	// light the edges fade into the sky rather than outline the clouds
	highp float forwardLight = max(smoothstep(0.88, 0.985, sunCos), smoothstep(0.88, 0.985, moonCos));
	light *= mix(0.45 + 0.55*(1. - T*T), 1., forwardLight);
	light += silver*thin*upper;

	// The way through the air dims the cloud and adds the light of the air (Rayleigh phase function)
	highp vec3 way = exp(-airExtinction*hitT);
	highp vec3 air = (1. - way)*(airSky + airMoon*0.75*(1. + moonCos*moonCos));
	// In sunlit air the sky behind the cloud stands for the air in front of it, so that far clouds fade into the sky
	// (the Sun and the Moon behind the clouds are dimmed where they are drawn, so that their disks do not show through:
	// see CloudMgr::getTransmission())
	highp float kept = (1. - way.y)*airSunlit;
	highp float hidden = (1. - T)*(1. - kept);
	// out = source + scene x alpha
	FRAG_COLOR = vec4((light*way + air)*hidden, 1. - hidden);
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
	const CloudShape::Motion m = motionAt(core, core->getJD());
	const double mapX = m.cosine*scale*earth[0] - m.sine*scale*earth[1] + m.offsetX + CloudShape::CirrusShift;
	const double mapY = m.sine*scale*earth[0] + m.cosine*scale*earth[1] + m.offsetY + CloudShape::CirrusShift;
	const double cover = cirrusFront.on ? CloudShape::frontCover(tile, mapX - CloudShape::CirrusShift, mapY - CloudShape::CirrusShift, cirrusFront, m)
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
	double windX = 0., windY = 0.;
	computeWind(core, &windX, &windY);
	const CloudShape::Motion motion = motionAt(core, core->getJD());
	const int samples = exposure*std::hypot(windX, windY) > 0.01 ? 4 : 1;
	double result = 1.;
	for (int l = 0; l < LayerCount; ++l)
	{
		if (!isLayerShown(core, l))
			continue;
		double sum = 0.;
		for (int i = 0; i < samples; ++i)
		{
			CloudShape::Motion moment = motion;
			const double when = (i + 0.5)/samples - 0.5;
			moment.offsetX += windX*exposure*when;
			moment.offsetY += windY*exposure*when;
			sum += CloudShape::transmission(tile, currentLayer(l), moment, observerRadius, {altAz[0], altAz[1], altAz[2]}, south, east, up);
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
	p.setUniformValue("cloudTileScale", QVector4D(static_cast<float>(1./(layer.size*CloudShape::TileSize)), static_cast<float>(1./CloudShape::CoverScaleLarge),
	                                              static_cast<float>(1./CloudShape::CoverScaleSmall), static_cast<float>(1./(layer.size*CloudShape::FineScale))));
	p.setUniformValue("cloudSize", static_cast<float>(layer.size));
	setUniformsOfLayer(p, i, "cloud");
	// The layer above casts its shadows (a radius of 0: none)
	if (other >= 0)
		setUniformsOfLayer(p, other, "other");
	else
		p.setUniformValue("otherRadius", QVector3D(0.f, 1.f, 1.f));

	const CloudShape::Motion motion = motionAt(core, core->getJD());
	p.setUniformValue("cloudTurn", QVector4D(static_cast<float>(motion.cosine), static_cast<float>(motion.sine),
	                                         static_cast<float>(motion.offsetX), static_cast<float>(motion.offsetY)));
	p.setUniformValue("cloudGrow", QVector2D(static_cast<float>(motion.growU), static_cast<float>(motion.growV)));
	p.setUniformValue("cloudBoil", QVector2D(static_cast<float>(motion.boilX), static_cast<float>(motion.boilY)));
	p.setUniformValue("cloudFrontDrift", QVector2D(static_cast<float>(motion.frontU), static_cast<float>(motion.frontV)));
	p.setUniformValue("cloudGrowScale", static_cast<float>(1./CloudShape::GrowScale));
	p.setUniformValue("cloudGrowStrength", static_cast<float>(CloudShape::GrowStrength));
	// The exposure, if the sky moves during it in the camera
	const double exposure = drawer->getFlagPhotoStarTrails() ? std::max(0., drawer->getPhotoEffectiveExposureTime()) : 0.;
	double windX = 0., windY = 0.;
	computeWind(core, &windX, &windY);
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
uniform highp vec3 cirrusTau;       // the optical depth of the air at the zenith

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
	m += 9.*vec2(cirrusNoise(map/60. + 0.5, max(log2(foot*1024./60.), 0.)), cirrusNoise(map/60. + 0.25, max(log2(foot*1024./60.), 0.))) - 4.5;
	highp float patch = cirrusNoise(map/190., max(log2(foot*1024./190.), 0.));
	highp float area = clamp((patch - 0.85 + 0.9*cover)/0.2, 0., 1.);
	highp float fibres = 0.5*cirrusNoise(vec2(m.x/40., m.y/10.), max(log2(foot*1024./10.), 0.))
	                   + 0.3*cirrusNoise(vec2(m.x/12. + 0.37, m.y/3. + 0.71), max(log2(foot*1024./3.), 0.))
	                   + 0.2*cirrusNoise(vec2(m.x/4. + 0.13, m.y/0.9 + 0.29), max(log2(foot*1024./0.9), 0.));
	return area*smoothstep(0.4, 0.68, fibres + 0.3*(area - 0.5));
}

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
	highp vec2 map = 2.*cirrusRadii.y*earth.xy/(length(earth) + abs(earth.z));
	map = vec2(cloudTurn.x*map.x - cloudTurn.y*map.y, cloudTurn.y*map.x + cloudTurn.x*map.y) + cloudTurn.zw + vec2(4711.);
	highp float cover = cirrusFront.x > 0. ? frontCover(map - vec2(4711.), cirrusFront) : cirrusCover.x;
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
	// out = source + scene x alpha. The cirrus stand above nearly all the air: the light of the air in front of them stays,
	// in the share that does not reach the observer from them
	FRAG_COLOR = vec4(light*way, 1. - (1. - T)*way.y);
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
	p.setUniformValue("projectionMatrixInverse", prj.getProjectionMatrix().toQMatrix().inverted());
	prj.setUnProjectUniforms(p);

	const double phi = static_cast<double>(location.getLatitude())*M_PI/180.;
	const double lambda = static_cast<double>(location.getLongitude())*M_PI/180.;
	const auto vec = [](const Vec3d& v) {return QVector3D(static_cast<float>(v[0]), static_cast<float>(v[1]), static_cast<float>(v[2]));};
	p.setUniformValue("cloudSouth", vec(Vec3d(std::sin(phi)*std::cos(lambda), std::sin(phi)*std::sin(lambda), -std::cos(phi))));
	p.setUniformValue("cloudEast", vec(Vec3d(-std::sin(lambda), std::cos(lambda), 0.)));
	p.setUniformValue("cloudUp", vec(Vec3d(std::cos(phi)*std::cos(lambda), std::cos(phi)*std::sin(lambda), std::sin(phi))));
	const CloudShape::Motion motion = motionAt(core, core->getJD());
	p.setUniformValue("cloudTurn", QVector4D(static_cast<float>(motion.cosine), static_cast<float>(motion.sine),
	                                         static_cast<float>(motion.offsetX), static_cast<float>(motion.offsetY)));
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
	QVector3D sky;
	for (int c = 0; c < 3; ++c)
	{
		// half of the sky above, the ground of an albedo of 0.2 below, and the light of the towns
		const double ground = 0.2*(skyLux*skyColor[static_cast<std::size_t>(c)] + 133000.*sunGround[static_cast<std::size_t>(c)]*std::max(sun[2], 0.));
		const double value = (0.5*skyLux*skyColor[static_cast<std::size_t>(c)] + ground)/(4.*M_PI) + 2.*artificial*static_cast<double>(lamp[c]);
		sky[c] = static_cast<float>(value*scale);
	}
	p.setUniformValue("cirrusSky", sky);
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
