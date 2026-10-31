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

#include "AuroraMgr.hpp"
#include "StelApp.hpp"
#include "StelCore.hpp"
#include "StelLocation.hpp"
#include "StelModuleMgr.hpp"
#include "StelPainter.hpp"
#include "StelProjector.hpp"
#include "StelUtils.hpp"
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
#include <QVector4D>
#include <cmath>
#include <array>
#include <algorithm>

namespace
{
constexpr double EarthRadius = 6371.;   // km
constexpr double GreenHeight = 115.;    // km: the green (557.7 nm) and blue (427.8 nm) emissions
constexpr double RedHeight = 300.;      // km: the red (630.0 nm) emission
constexpr int PointsPerLine = 180;      // one every 8 minutes of magnetic local time
constexpr int VertexAttribIndex = 0;

// The emissions in the linear sRGB of the scene (luminance in cd/m^2) per rayleigh, from the CIE 1931 color matching
// functions: 1 R is 10^10/(4 pi) photons per second, square meter and steradian. The green line of oxygen (557.7 nm,
// 1.93e-7 cd/m^2 per R) goes with the blue band of N2+ (427.8 nm), about 1/5 of it in rayleigh and from the same
// heights; the red line of oxygen (630.0 nm, 4.54e-8 cd/m^2 per R) with its companion at 636.4 nm (1/3 of it).
// Both are outside the sRGB gamut (the shader keeps their luminance and desaturates them into it).
const Vec3f GreenPerRayleigh(6.116e-8f, 2.493e-7f, 3.451e-8f);
const Vec3f RedPerRayleigh(3.604e-7f, -2.776e-8f, -3.84e-9f);
// The lower border of energetic aurorae: the first positive band of N2 (650-690 nm) with 0.6 of it of 427.8 nm
const Vec3f PurplePerRayleigh(7.789e-8f, -3.202e-8f, 2.007e-7f);

// A number in [0, 1) from the integer n and a salt (splitmix64)
double unitHash(int64_t n, uint64_t salt)
{
	uint64_t z = static_cast<uint64_t>(n)*0x9E3779B97F4A7C15ull + salt*0xBF58476D1CE4E5B9ull + 0x94D049BB133111EBull;
	z = (z ^ (z >> 30))*0xBF58476D1CE4E5B9ull;
	z = (z ^ (z >> 27))*0x94D049BB133111EBull;
	z ^= z >> 31;
	return static_cast<double>(z >> 11)*(1./9007199254740992.);
}

// The geomagnetic north pole of the IGRF dipole (World Data Center for Geomagnetism, Kyoto; IGRF-14,
// predicted from 2026): year, latitude, longitude (degrees, east positive)
struct PoleEntry {double year, latitude, longitude;};
const std::array<PoleEntry, 35> poleTable = {{
	{1900, 78.7, -68.8}, {1905, 78.7, -68.7}, {1910, 78.7, -68.7}, {1915, 78.6, -68.6}, {1920, 78.6, -68.4},
	{1925, 78.6, -68.3}, {1930, 78.6, -68.3}, {1935, 78.6, -68.4}, {1940, 78.5, -68.5}, {1945, 78.5, -68.5},
	{1950, 78.5, -68.8}, {1955, 78.5, -69.2}, {1960, 78.6, -69.5}, {1965, 78.6, -69.9}, {1970, 78.7, -70.2},
	{1975, 78.8, -70.5}, {1980, 78.9, -70.8}, {1985, 79.0, -70.9}, {1990, 79.2, -71.1}, {1995, 79.4, -71.4},
	{2000, 79.6, -71.6}, {2005, 79.8, -71.8}, {2010, 80.1, -72.2}, {2015, 80.4, -72.6}, {2020, 80.6, -72.7},
	{2021, 80.7, -72.7}, {2022, 80.7, -72.7}, {2023, 80.8, -72.7}, {2024, 80.8, -72.7}, {2025, 80.8, -72.8},
	{2026, 80.9, -72.8}, {2027, 80.9, -72.8}, {2028, 81.0, -72.9}, {2029, 81.0, -72.9}, {2030, 81.1, -73.0}
}};

// The model of Starkov (1994) of the auroral oval, with the coefficients as given by Sigernes et al. (2011,
// Space Weather 9, S05005; their table 1): each of A0..A3 and alpha1..alpha3 is b0 + b1 x + b2 x^2 + b3 x^3
// with x = log10|AL|, and the colatitude of the boundary is
// theta = A0 + A1 cos(15(t + alpha1)) + A2 cos(15(2t + alpha2)) + A3 cos(15(3t + alpha3))   (degrees, t: MLT in hours)
using Coefficients = std::array<std::array<double, 4>, 7>; // A0, A1, A2, A3, alpha1, alpha2, alpha3
const Coefficients starkovPoleward = {{
	{ -0.07, 24.54, -12.53, 2.15}, {-10.06, 19.83,  -9.33, 1.24}, { -4.44,  7.47, -3.01,  0.25},
	{ -3.77,  7.90,  -4.73, 0.91}, { -6.61, 10.17,  -5.80, 1.19}, {  6.37, -1.10,  0.34, -0.38},
	{ -4.48, 10.16,  -5.87, 0.98}
}};
const Coefficients starkovEquatorward = {{
	{  1.61, 23.21, -10.97, 2.03}, { -9.59, 17.78,  -7.20, 0.96}, {-12.07, 17.49, -7.96,  1.15},
	{ -6.56, 11.44,  -6.73, 1.31}, { -2.22,  1.50,  -0.58, 0.08}, {-23.98, 42.79, -26.96, 5.56},
	{-20.07, 36.67, -24.20, 5.11}
}};
const Coefficients starkovDiffuse = {{
	{  3.44, 29.77, -16.38, 3.35}, { -2.41,  7.89,  -4.32, 0.87}, { -0.74,  3.94, -3.09,  0.72},
	{ -2.12,  3.24,  -1.67, 0.31}, { -1.68, -2.48,   1.58, -0.28}, { 8.69, -20.73, 13.03, -2.14},
	{  8.61, -5.34,  -1.36, 0.76}
}};

double starkovLatitude(const Coefficients& c, double x, double mlt)
{
	std::array<double, 7> v;
	for (size_t i = 0; i < 7; ++i)
		v[i] = c[i][0] + x*(c[i][1] + x*(c[i][2] + x*c[i][3]));
	const double d = M_PI/180.;
	const double theta = v[0] + v[1]*std::cos(15.*(mlt + v[4])*d) + v[2]*std::cos(15.*(2.*mlt + v[5])*d)
	                     + v[3]*std::cos(15.*(3.*mlt + v[6])*d);
	return 90. - theta;
}

// The geomagnetic latitude (radians) at the height h (km) of the dipole field line that crosses the height of the
// green emission at the latitude lat: r = L cos^2(lat) along a field line
double alongFieldLine(double lat, double h)
{
	const double c2 = std::cos(lat)*std::cos(lat) * (EarthRadius + h)/(EarthRadius + GreenHeight);
	const double c = std::sqrt(std::min(1., c2));
	return std::copysign(std::acos(c), lat);
}
}

AuroraMgr::AuroraMgr()
{
	setObjectName("AuroraMgr");
}

AuroraMgr::~AuroraMgr()
{
}

void AuroraMgr::init()
{
	QSettings* conf = StelApp::getInstance().getSettings();
	setKp(conf->value("aurora/kp", 3.).toDouble());
	setFlagShow(conf->value("aurora/flag_show", false).toBool());
	setFlagShowOval(conf->value("aurora/flag_show_oval", false).toBool());
	addAction("actionShow_Aurora", N_("Display Options"), N_("Aurorae"), "flagShow");
	addAction("actionShow_Aurora_Oval", N_("Display Options"), N_("Auroral oval"), "flagShowOval");

	// A full screen quad for the light
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

void AuroraMgr::update(double)
{
}

double AuroraMgr::getCallOrder(StelModuleActionName actionName) const
{
	// Before the landscape, which hides what is below its horizon
	if (actionName == StelModule::ActionDraw)
		return StelApp::getInstance().getModuleMgr().getModule("LandscapeMgr")->getCallOrder(actionName) - 1.;
	return 0.;
}

void AuroraMgr::setFlagShow(bool b)
{
	if (b != flagShow)
	{
		flagShow = b;
		emit flagShowChanged(b);
	}
}

void AuroraMgr::setFlagShowOval(bool b)
{
	if (b != flagShowOval)
	{
		flagShowOval = b;
		emit flagShowOvalChanged(b);
	}
}

void AuroraMgr::setKp(double value)
{
	value = std::clamp(value, 0., 9.);
	if (!qFuzzyCompare(value + 1., kp + 1.))
	{
		kp = value;
		emit kpChanged(value);
	}
}

AuroraMgr::Emission AuroraMgr::emission(double kp)
{
	kp = std::clamp(kp, 0., 9.);
	Emission e;
	// Calm arcs are a few kR (IBC I-II) on quiet nights, tens of kR in storms (IBC III); the diffuse aurora about
	// 1 kR. In storms the electrons are softer and the red emission stronger and higher (the red tops seen from
	// low latitudes).
	e.arcGreen = 3000.*std::pow(2., kp/2.);
	e.diffuseGreen = 500.*std::pow(2., kp/3.);
	e.redRatio = 0.15 + 0.05*kp;
	e.redHeight = 220. + 8.*kp;
	return e;
}

AuroraMgr::OvalBoundaries AuroraMgr::ovalBoundaries(double kp, double mlt)
{
	kp = std::clamp(kp, 0., 9.);
	const double al = 18. - 12.3*kp + 27.2*kp*kp - 2.*kp*kp*kp;
	const double x = std::log10(std::max(1., std::fabs(al)));
	OvalBoundaries b;
	b.poleward = starkovLatitude(starkovPoleward, x, mlt);
	b.equatorward = starkovLatitude(starkovEquatorward, x, mlt);
	b.diffuse = starkovLatitude(starkovDiffuse, x, mlt);
	return b;
}

void AuroraMgr::dipolePole(double jd, double& latitude, double& longitude)
{
	const double year = 2000. + (jd - 2451545.0)/365.25;
	if (year <= poleTable.front().year)
	{
		latitude = poleTable.front().latitude;
		longitude = poleTable.front().longitude;
		return;
	}
	if (year >= poleTable.back().year)
	{
		latitude = poleTable.back().latitude;
		longitude = poleTable.back().longitude;
		return;
	}
	size_t i = 1;
	while (poleTable[i].year < year)
		++i;
	const PoleEntry& a = poleTable[i-1];
	const PoleEntry& b = poleTable[i];
	const double f = (year - a.year)/(b.year - a.year);
	latitude = a.latitude + f*(b.latitude - a.latitude);
	longitude = a.longitude + f*(b.longitude - a.longitude);
}

AuroraMgr::MagneticFrame AuroraMgr::magneticFrame(double jd)
{
	double lat, lon;
	dipolePole(jd, lat, lon);
	MagneticFrame f;
	StelUtils::spheToRect(lon*M_PI/180., lat*M_PI/180., f.z);
	// The prime geomagnetic meridian goes through the geographic poles (any choice will do: only differences of
	// geomagnetic longitude, the magnetic local time, matter here)
	f.y = Vec3d(0., 0., 1.) ^ f.z;
	f.y.normalize();
	f.x = f.y ^ f.z;
	return f;
}


AuroraMgr::Substorm AuroraMgr::substorm(double jd, double kp)
{
	kp = std::clamp(kp, 0., 9.);
	Substorm s;
	const double cycle = 3.5 - 0.2*kp;  // hours
	const double hours = (jd - 2451545.0)*24.;
	const double n = std::floor(hours/cycle);
	const double x = hours/cycle - n;   // in the cycle, 0 to 1
	const int64_t slot = static_cast<int64_t>(n);
	const double growth = 0.35 + 0.25*unitHash(slot, 1);
	const double expansion = (0.2 + 0.2*unitHash(slot, 2))/cycle;
	const double strength = (0.35 + 0.65*unitHash(slot, 3))*std::min(1., 0.25 + kp/6.);
	const double maxBulge = 4.*strength*(0.6 + kp/15.);
	const double maxDrift = 0.5 + 0.15*kp;
	s.onsetMLT = 22.5 + 2.*(unitHash(slot, 4) - 0.5);
	if (x < growth)
	{
		const double t = x/growth;
		s.phase = 0;
		s.drift = maxDrift*t;
	}
	else if (x < growth + expansion)
	{
		const double t = (x - growth)/expansion;
		s.phase = 1;
		s.activity = strength*(1. - std::exp(-t/0.08));
		s.spread = 0.7 + 2.5*t;
		s.bulge = maxBulge*std::sqrt(t);
		s.drift = maxDrift;
	}
	else
	{
		const double t = (x - growth - expansion)/(1. - growth - expansion);
		s.phase = 2;
		s.activity = strength*(1. - t)*(1. - t);
		s.spread = 3.2 + 1.5*t;
		s.bulge = maxBulge*(1. - t);
		s.drift = maxDrift*(1. - t);
		s.pulsating = strength*std::sin(M_PI*t);
	}
	return s;
}

AuroraMgr::Geometry AuroraMgr::computeGeometry(const StelCore* core) const
{
	Geometry g;
	// The observer, over a spherical Earth, with its local axes in the Earth-fixed frame
	const StelLocation& loc = core->getCurrentLocation();
	const double phi = static_cast<double>(loc.getLatitude())*M_PI/180.;
	const double lambda = static_cast<double>(loc.getLongitude())*M_PI/180.;
	g.up.set(std::cos(phi)*std::cos(lambda), std::cos(phi)*std::sin(lambda), std::sin(phi));
	g.east.set(-std::sin(lambda), std::cos(lambda), 0.);
	g.north.set(-std::sin(phi)*std::cos(lambda), -std::sin(phi)*std::sin(lambda), std::cos(phi));
	g.observer = g.up * (EarthRadius + std::max(0., loc.altitude*0.001));

	// The direction of the Sun in the Earth-fixed frame, from its direction in the sky of the observer
	// (AltAz: x to the south, y to the east, z up)
	SolarSystem* ssystem = GETSTELMODULE(SolarSystem);
	Vec3d sunAltAz = core->j2000ToAltAz(ssystem->getSun()->getJ2000EquatorialPos(core), StelCore::RefractionOff);
	sunAltAz.normalize();
	g.sunAltitude = std::asin(std::clamp(sunAltAz[2], -1., 1.));
	const Vec3d sun = g.north*(-sunAltAz[0]) + g.east*sunAltAz[1] + g.up*sunAltAz[2];

	// Magnetic local time is measured from the geomagnetic longitude of the Sun: noon below it
	g.frame = magneticFrame(core->getJD());
	g.sunMagLon = std::atan2(sun*g.frame.y, sun*g.frame.x);
	return g;
}

std::vector<AuroraMgr::OvalLine> AuroraMgr::computeOvalLines(const Geometry& g) const
{
	std::vector<OvalLine> lines;
	struct LineSpec
	{
		int boundary; // 0 poleward, 1 equatorward, 2 diffuse
		double height;
		Vec3f color;
	};
	const std::array<LineSpec, 4> specs = {{
		{0, GreenHeight, Vec3f(0.35f, 1.0f, 0.45f)},
		{1, GreenHeight, Vec3f(0.35f, 1.0f, 0.45f)},
		{2, GreenHeight, Vec3f(0.15f, 0.45f, 0.20f)},
		{1, RedHeight,   Vec3f(1.0f, 0.30f, 0.30f)}
	}};

	std::array<OvalBoundaries, PointsPerLine+1> boundaries;
	for (int i = 0; i <= PointsPerLine; ++i)
		boundaries[static_cast<size_t>(i)] = ovalBoundaries(kp, 24.*i/PointsPerLine);

	for (const double hemisphere : {1., -1.})
	{
		for (const auto& spec : specs)
		{
			OvalLine line;
			line.color = spec.color;
			line.points.reserve(PointsPerLine+1);
			line.visible.reserve(PointsPerLine+1);
			for (int i = 0; i <= PointsPerLine; ++i)
			{
				const double mlt = 24.*i/PointsPerLine;
				const OvalBoundaries& b = boundaries[static_cast<size_t>(i)];
				const double latDeg = spec.boundary == 0 ? b.poleward : (spec.boundary == 1 ? b.equatorward : b.diffuse);
				const double lat = alongFieldLine(hemisphere*latDeg*M_PI/180., spec.height);
				const double lon = g.sunMagLon + (mlt - 12.)*M_PI/12.;
				const Vec3d point = g.frame.toGeographic(lat, lon) * (EarthRadius + spec.height);

				// In sight unless the line of sight passes below the surface of the Earth
				const Vec3d d = point - g.observer;
				const double s = -(g.observer*d)/(d*d);
				line.visible.push_back(s <= 0. || (g.observer + d*std::min(s, 1.)).norm() >= EarthRadius);

				Vec3d altAz(-(g.north*d), g.east*d, g.up*d);
				altAz.normalize();
				line.points.push_back(altAz);
			}
			lines.push_back(std::move(line));
		}
	}
	return lines;
}

void AuroraMgr::draw(StelCore* core)
{
	if (!flagShow && !flagShowOval)
		return;
	if (core->getCurrentPlanet()->getEnglishName() != QStringLiteral("Earth"))
		return;
	const Geometry g = computeGeometry(core);
	if (flagShow)
		drawLight(core, g);
	if (flagShowOval)
		drawOval(core, g);
}

void AuroraMgr::drawOval(StelCore* core, const Geometry& g) const
{
	const std::vector<OvalLine> lines = computeOvalLines(g);
	const StelProjectorP prj = core->getProjection(StelCore::FrameAltAz, StelCore::RefractionAuto);
	StelPainter painter(prj);
	painter.setBlending(true);
	painter.setLineSmooth(true);
	painter.setLineWidth(2.f * static_cast<float>(StelApp::getInstance().getDevicePixelsPerPixel()));
	for (const auto& line : lines)
	{
		painter.setColor(line.color, 0.9f);
		for (size_t i = 1; i < line.points.size(); ++i)
		{
			if (line.visible[i-1] && line.visible[i])
				painter.drawGreatCircleArc(line.points[i-1], line.points[i]);
		}
	}
	painter.setLineWidth(1.f);
	painter.setLineSmooth(false);
	painter.setBlending(false);
}

void AuroraMgr::buildLightProgram(const StelProjector& projector)
{
	lightProgram.reset();
	const Extinction& extinction = StelApp::getInstance().getCore()->getSkyDrawer()->getExtinction();
	std::unique_ptr<QOpenGLShaderProgram> program(new QOpenGLShaderProgram);

	const QByteArray vert = StelOpenGL::globalShaderPrefix(StelOpenGL::VERTEX_SHADER) + R"(
ATTRIBUTE highp vec3 vertex;
VARYING highp vec3 ndcPos;
void main()
{
	gl_Position = vec4(vertex, 1.);
	ndcPos = vertex;
}
)";
	if (!program->addShaderFromSourceCode(QOpenGLShader::Vertex, vert))
	{
		qWarning().noquote() << "AuroraMgr: error in the vertex shader:\n" << program->log();
		return;
	}

	// The light along the line of sight through the shell of the aurorae, 90 to 500 km. The line is cut at the
	// heights 90 + 410 (k/N)^2 km, dense where the green light is. In each piece the footprint latitude (at 115 km,
	// along the dipole field line) changes linearly: the thin arcs (Gaussian across) are integrated exactly over it,
	// with the vertical profile where the line crosses them; the broad diffuse aurora at the middle of the piece.
	// The vertical profiles are Chapman layers. Everything is in the geomagnetic frame (km).
	// The forms: the arcs fold (noise in magnetic longitude), and their light is broken into rays: a fine noise in
	// magnetic longitude, which stays the same along the dipole field lines (they lie in the magnetic meridians), so
	// the rays follow them and meet in the magnetic zenith (the corona) seen from below. A substorm (see substorm())
	// brightens and folds the arcs around its onset in magnetic local time and spreads out from there; afterwards
	// the diffuse aurora pulsates in patches. All noises come from the date and time: a moment always looks the same.
	const QByteArray frag = StelOpenGL::globalShaderPrefix(StelOpenGL::FRAGMENT_SHADER)
	                        + projector.getUnProjectShader()
	                        + extinction.getForwardTransformShader()
	                        + StelSkyDrawer::getPhotoExtinctionShader()
	                        + R"(
VARYING highp vec3 ndcPos;
uniform highp mat4 projectionMatrixInverse;
uniform bool extinctionEnabled;
uniform highp vec3 auroraSouth;      // the axes of the AltAz frame in the geomagnetic frame
uniform highp vec3 auroraEast;
uniform highp vec3 auroraUp;
uniform highp vec3 auroraObserver;   // km
uniform highp float auroraSunLon;    // geomagnetic longitude of the Sun (radians)
uniform highp vec4 auroraOval[49];   // poleward, equatorward, diffuse boundary at 115 km (radians), MLT 0, 0.5, ... 24 h
uniform highp vec4 auroraEmission;   // green (R) of the main arc, of the diffuse aurora; red against green; red peak (km)
uniform highp vec3 auroraGreenColor; // scene buffer per R of 557.7 nm (with 427.8 nm)
uniform highp vec3 auroraRedColor;   // per R of 630.0 nm (with 636.4 nm)
uniform highp vec3 auroraPurpleColor; // per R of the first positive band of N2 (650-690 nm, with 427.8 nm)
uniform highp float auroraTime;      // seconds since 0h UT
uniform highp float auroraSeed;      // from the date
uniform highp vec4 auroraSubstorm;   // onset (MLT, h), its spread in MLT (h), activity (0-1), poleward bulge (radians)
uniform highp vec4 auroraState;      // equatorward drift of the arcs (radians), pulsating aurora (0-1), purple lower
                                     // border against the green (energetic electrons), unused

const highp float PI = 3.14159265;
const highp float EARTH_RADIUS = 6371.;
const highp float FOOT_RADIUS = 6486.;  // 115 km
const highp float SHELL_BOTTOM = 90.;
const highp float SHELL_TOP = 500.;
const int STEPS = 48;

// Abramowitz and Stegun 7.1.26
highp float auroraErf(highp float x)
{
	highp float s = sign(x);
	x = abs(x);
	highp float t = 1./(1. + 0.3275911*x);
	highp float y = 1. - ((((1.061405429*t - 1.453152027)*t + 1.421413741)*t - 0.284496736)*t + 0.254829592)*t*exp(-x*x);
	return s*y;
}
// Chapman layer with the given peak and scale height (km), normalized to a column of 1
highp float chapman(highp float h, highp float peak, highp float scale)
{
	highp float z = (h - peak)/scale;
	return exp(1. - z - exp(-z))/(2.7182818*scale);
}
// Absolute geomagnetic latitude at 115 km of the dipole field line through p: r = L cos^2(lat)
highp float footLatitude(highp vec3 p)
{
	highp float r = length(p);
	highp float s = clamp(abs(p.z)/r, 0., 1.);
	return acos(sqrt(clamp((1. - s*s)*FOOT_RADIUS/r, 0., 1.)));
}
highp vec3 ovalAt(highp float mlt)
{
	highp float f = mod(mlt, 24.)*2.;
	int i = int(min(floor(f), 47.));
	return mix(auroraOval[i].xyz, auroraOval[i + 1].xyz, f - float(i));
}
// Length (km) of the piece of the line of sight from latitude lat0 to lat1, weighted by a Gaussian band at
// latitude a with the width w (standard deviation)
highp float bandLength(highp float lat0, highp float lat1, highp float a, highp float w, highp float len)
{
	highp float d = lat1 - lat0;
	if (abs(d) < 0.05*w)
	{
		highp float m = 0.5*(lat0 + lat1) - a;
		return len*exp(-0.5*m*m/(w*w));
	}
	highp float k = 0.70710678/w;
	return len*w*1.2533141/d*(auroraErf((lat1 - a)*k) - auroraErf((lat0 - a)*k));
}
// Hash (Dave Hoskins, "Hash without Sine") and value noise, periodic in x with n cells
highp float auroraHash(highp vec2 p)
{
	highp vec3 p3 = fract(vec3(p.xyx)*0.1031);
	p3 += dot(p3, p3.yzx + 33.33);
	return fract((p3.x + p3.y)*p3.z);
}
highp float auroraNoise(highp float x, highp float n, highp float y)
{
	highp float ix = floor(x), iy = floor(y);
	highp float fx = x - ix, fy = y - iy;
	highp float x0 = mod(ix, n), x1 = mod(ix + 1., n);
	highp vec2 s = vec2(auroraSeed, 0.37*auroraSeed);
	highp float a = auroraHash(vec2(x0, iy) + s), b = auroraHash(vec2(x1, iy) + s);
	highp float c = auroraHash(vec2(x0, iy + 1.) + s), d = auroraHash(vec2(x1, iy + 1.) + s);
	fx = fx*fx*(3. - 2.*fx);
	fy = fy*fy*(3. - 2.*fy);
	return mix(mix(a, b, fx), mix(c, d, fx), fy);
}
// Fold of the arcs: the shift of their latitude (radians) at the magnetic longitude lon (in turns), quiet and activity
highp float arcFold(highp float lon, highp float activity, int j)
{
	highp float o = float(j)*17.;
	highp float quiet = auroraNoise(lon*60. + o, 60., auroraTime/1500. + o) - 0.5;
	highp float act = 0.7*(auroraNoise(lon*240. + auroraTime/400. + o, 240., auroraTime/40. + o) - 0.5)
	                + 0.3*(auroraNoise(lon*900. + auroraTime/90. + o, 900., auroraTime/12. + o) - 0.5);
	return mix(0.06*quiet, 0.5*act, activity)*PI/180.;
}
// The rays: brightness against the mean (about 1) at the magnetic longitude lon (in turns), for the share of
// rays in the arc; the cells are 0.04 and 0.012 degrees of longitude (2 and 0.7 km in the oval), and they drift
// along the arc
highp float arcRays(highp float lon, highp float rays, highp float span, int j)
{
	highp float o = float(j)*31.;
	highp float r = 0.6*auroraNoise(lon*9000. + auroraTime/3. + o, 9000., auroraTime/2.5 + o)
	              + 0.4*auroraNoise(lon*30000. - auroraTime/1.5 + o, 30000., auroraTime/0.8 + o);
	// a piece of the line of sight that spans many rays sees their mean
	highp float mean = smoothstep(2., 8., span*9000.);
	return mix(1., mix(3.*r*r, 1., mean), rays);
}
// Patches of pulsating aurora: brightness against the mean, the patches flicker on and off in a few seconds
highp float pulsating(highp float lon, highp float lat, highp float amount)
{
	if (amount <= 0.)
		return 1.;
	highp float patchMask = smoothstep(0.5, 0.65, auroraNoise(lon*1200. + auroraTime/2000., 1200., lat*180./PI*4.));
	highp float phase = auroraNoise(lon*2400. + 7., 2400., lat*180./PI*8. + 11.);
	highp float period = 4. + 8.*auroraNoise(lon*600. + 3., 600., lat*180./PI*2. + 5.);
	highp float on = smoothstep(-0.3, 0.3, sin(2.*PI*(auroraTime/period + 5.*phase)));
	return 1. + amount*patchMask*(2.*on - 1.);
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
	highp vec3 dir = altAz.x*auroraSouth + altAz.y*auroraEast + altAz.z*auroraUp;
	highp vec3 p0 = auroraObserver;
	highp float r0 = length(p0);
	highp float b = dot(p0, dir);
	if (b < 0. && b*b - r0*r0 + EARTH_RADIUS*EARTH_RADIUS > 0.)
	{
		FRAG_COLOR = vec4(0); // the line of sight meets the Earth
		return;
	}

	highp float green = 0., red = 0., purple = 0.;
	highp float tPrev = 0., hPrev = 0., latPrev = 0., lonPrev = 0.;
	for (int k = 0; k <= STEPS; ++k)
	{
		highp float u = float(k)/float(STEPS);
		highp float h = SHELL_BOTTOM + (SHELL_TOP - SHELL_BOTTOM)*u*u;
		highp float rh = EARTH_RADIUS + h;
		highp float t = -b + sqrt(max(b*b - r0*r0 + rh*rh, 0.));
		highp vec3 p = p0 + t*dir;
		highp float lat = footLatitude(p);
		highp float lon = atan(p.y, p.x)/(2.*PI); // in turns
		if (k > 0)
		{
			highp float len = t - tPrev;
			highp float dlon = lon - lonPrev;
			dlon -= floor(dlon + 0.5);
			highp float lonm = lonPrev + 0.5*dlon;
			highp float mlt = mod(12. + (lonm*2.*PI - auroraSunLon)*12./PI, 24.);
			highp vec3 oval = ovalAt(mlt);
			// the substorm around its onset in magnetic local time
			highp float dm = mlt - auroraSubstorm.x;
			dm -= 24.*floor(dm/24. + 0.5);
			highp float activity = auroraSubstorm.z*exp(-dm*dm/(auroraSubstorm.y*auroraSubstorm.y));
			highp float bulge = auroraSubstorm.w*activity/max(auroraSubstorm.z, 1e-3);
			highp float width = max(oval.x - oval.y, 0.005) + bulge;
			// the arcs are brightest before magnetic midnight, faint on the day side; the diffuse aurora after it
			highp float night = 0.5 + 0.5*cos((mlt - 23.)*PI/12.);
			highp float arcs = auroraEmission.x*(0.1 + 0.9*night*sqrt(night))*(1. + 6.*activity);
			highp float morning = 0.5 + 0.5*cos((mlt - 3.)*PI/12.);
			highp float diffuse = auroraEmission.y*(0.3 + 0.7*morning)*(1. + 1.5*activity);
			// rays: a few in quiet arcs, most of the light in activity ones
			highp float rays = 0.15 + 0.85*smoothstep(0.03, 0.4, activity);

			// three arcs in the discrete oval, the brightest near its equatorward edge; they drift towards the
			// equator before a substorm and fill the bulge of the oval in it
			highp float dl = lat - latPrev;
			for (int j = 0; j < 3; ++j)
			{
				highp float frac = j == 0 ? 0.15 : (j == 1 ? 0.4 : 0.7);
				highp float w = (j == 0 ? 4. : (j == 1 ? 6. : 8.))/FOOT_RADIUS;
				highp float strength = j == 0 ? 1. : (j == 1 ? 0.45 : 0.25);
				highp float a0 = oval.y + frac*width - auroraState.x*(1. - activity);
				highp float a = a0 + arcFold(lonm, activity, j);
				highp float s = arcs*strength*bandLength(latPrev, lat, a, w, len);
				if (s <= 0.)
					continue;
				highp float c = abs(dl) > 1e-7 ? clamp((a - latPrev)/dl, 0., 1.) : 0.5;
				highp float hc = mix(hPrev, h, c);
				highp float ray = arcRays(lonPrev + c*dlon, rays, abs(dlon)*min(1., 3.*w/max(abs(dl), 1e-9)), j);
				// the rays reach higher than the arc
				highp float top = 12. + 30.*rays*min(ray, 2.);
				s *= ray;
				green += s*chapman(hc, 110., top);
				red += s*chapman(hc, auroraEmission.w, 60.);
				purple += s*chapman(hc, 97., 5.)*(0.3 + activity);
			}
			// the diffuse aurora, from its equatorward boundary into the discrete oval, pulsating after a substorm
			highp float latm = 0.5*(lat + latPrev);
			highp float hm = 0.5*(h + hPrev);
			highp float pulse = pulsating(lonm, latm, auroraState.y*smoothstep(0., 0.3, morning));
			highp float d = diffuse*len*pulse*smoothstep(oval.z, oval.y, latm)*(1. - smoothstep(oval.y + 0.5*width, oval.x + bulge, latm));
			green += d*chapman(hm, 115., 15.);
			red += d*chapman(hm, auroraEmission.w, 60.);
		}
		tPrev = t;
		hPrev = h;
		latPrev = lat;
		lonPrev = lon;
	}

	highp vec3 color = green*auroraGreenColor + auroraEmission.z*red*auroraRedColor + auroraState.z*purple*auroraPurpleColor;
	// into the gamut of the scene, with the same luminance
	highp float Y = dot(color, vec3(0.2126, 0.7152, 0.0722));
	highp float lowest = min(color.r, min(color.g, color.b));
	if (lowest < 0.)
		color = Y > 0. ? vec3(Y) + (color - vec3(Y))*(Y/(Y - lowest)) : vec3(0);
	if (extinctionEnabled)
		color *= pow(10., -0.4*extinctionMagnitude(altAz)) * photoExtinctionTint(altAz.z);
	FRAG_COLOR = vec4(color, 1.);
}
)";
	if (!program->addShaderFromSourceCode(QOpenGLShader::Fragment, frag))
	{
		qWarning().noquote() << "AuroraMgr: error in the fragment shader:\n" << program->log();
		return;
	}
	program->bindAttributeLocation("vertex", VertexAttribIndex);
	if (!StelPainter::linkProg(program.get(), "AuroraMgr light program"))
		return;
	lightProgram = std::move(program);
}

void AuroraMgr::drawLight(StelCore* core, const Geometry& g)
{
	// The aurorae are as bright as the Milky Way to the full Moon: the photographic mode records them with their
	// luminance; by day they are lost in the sky
	StelSkyDrawer* drawer = core->getSkyDrawer();
	if (!drawer->getFlagPhotographicModeActive() || g.sunAltitude > 0.)
		return;

	const StelProjectorP prj = core->getProjection(StelCore::FrameAltAz, StelCore::RefractionAuto);
	if (!lightProjector || !prj->isSameProjection(*lightProjector))
	{
		buildLightProgram(*prj);
		lightProjector = prj;
	}
	if (!lightProgram)
		return;

	// The axes of the AltAz frame and the observer in the geomagnetic frame
	const MagneticFrame& f = g.frame;
	const auto toMag = [&f](const Vec3d& v) {return QVector3D(static_cast<float>(v*f.x), static_cast<float>(v*f.y), static_cast<float>(v*f.z));};
	// The oval at 115 km, MLT every half hour
	std::array<QVector4D, 49> oval;
	for (size_t i = 0; i < oval.size(); ++i)
	{
		const OvalBoundaries b = ovalBoundaries(kp, 0.5*static_cast<double>(i));
		oval[i] = QVector4D(static_cast<float>(b.poleward*M_PI/180.), static_cast<float>(b.equatorward*M_PI/180.),
		                    static_cast<float>(b.diffuse*M_PI/180.), 0.f);
	}
	const Emission e = emission(kp);
	const float scale = drawer->getPhotoSceneScale();
	const Vec3f green = GreenPerRayleigh*scale;
	const Vec3f red = RedPerRayleigh*scale;
	const Extinction& extinction = drawer->getExtinction();

	QOpenGLShaderProgram& p = *lightProgram;
	p.bind();
	p.setUniformValue("projectionMatrixInverse", prj->getProjectionMatrix().toQMatrix().inverted());
	prj->setUnProjectUniforms(p);
	extinction.setForwardTransformUniforms(p);
	drawer->setPhotoExtinctionUniforms(p, 1);
	p.setUniformValue("extinctionEnabled", drawer->getFlagHasAtmosphere() && extinction.getExtinctionCoefficient() >= 0.01f);
	p.setUniformValue("auroraSouth", toMag(-g.north));
	p.setUniformValue("auroraEast", toMag(g.east));
	p.setUniformValue("auroraUp", toMag(g.up));
	p.setUniformValue("auroraObserver", toMag(g.observer));
	p.setUniformValue("auroraSunLon", static_cast<float>(g.sunMagLon));
	p.setUniformValueArray("auroraOval", oval.data(), static_cast<int>(oval.size()));
	p.setUniformValue("auroraEmission", QVector4D(static_cast<float>(e.arcGreen), static_cast<float>(e.diffuseGreen),
	                                              static_cast<float>(e.redRatio), static_cast<float>(e.redHeight)));
	p.setUniformValue("auroraGreenColor", QVector3D(green[0], green[1], green[2]));
	p.setUniformValue("auroraRedColor", QVector3D(red[0], red[1], red[2]));
	const Vec3f purple = PurplePerRayleigh*scale;
	p.setUniformValue("auroraPurpleColor", QVector3D(purple[0], purple[1], purple[2]));
	// The forms and the substorm cycle from the time (UT)
	const double jd = core->getJD();
	const Substorm sub = substorm(jd, kp);
	p.setUniformValue("auroraTime", static_cast<float>(86400.*(jd + 0.5 - std::floor(jd + 0.5))));
	p.setUniformValue("auroraSeed", static_cast<float>(std::fmod(std::floor(jd + 0.5), 997.)));
	p.setUniformValue("auroraSubstorm", QVector4D(static_cast<float>(sub.onsetMLT), static_cast<float>(sub.spread),
	                                              static_cast<float>(sub.activity), static_cast<float>(sub.bulge*M_PI/180.)));
	const double energetic = 0.5*std::clamp((kp - 3.5)/4.5, 0., 1.);
	p.setUniformValue("auroraState", QVector4D(static_cast<float>(sub.drift*M_PI/180.), static_cast<float>(sub.pulsating),
	                                           static_cast<float>(energetic), 0.f));

	auto& gl = *QOpenGLContext::currentContext()->functions();
	gl.glEnable(GL_BLEND);
	gl.glBlendFunc(GL_ONE, GL_ONE);
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
	gl.glDisable(GL_BLEND);
	p.release();
}
