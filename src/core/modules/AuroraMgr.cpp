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

#include <QSettings>
#include <cmath>
#include <array>
#include <algorithm>

namespace
{
constexpr double EarthRadius = 6371.;   // km
constexpr double GreenHeight = 115.;    // km: the green (557.7 nm) and blue (427.8 nm) emissions
constexpr double RedHeight = 300.;      // km: the red (630.0 nm) emission
constexpr int PointsPerLine = 180;      // one every 8 minutes of magnetic local time

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
	setFlagShowOval(conf->value("aurora/flag_show_oval", false).toBool());
	addAction("actionShow_Aurora_Oval", N_("Display Options"), N_("Auroral oval"), "flagShowOval");
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

std::vector<AuroraMgr::OvalLine> AuroraMgr::computeOvalLines(const StelCore* core) const
{
	std::vector<OvalLine> lines;

	// The observer, over a spherical Earth, with its local axes in the Earth-fixed frame
	const StelLocation& loc = core->getCurrentLocation();
	const double phi = static_cast<double>(loc.getLatitude())*M_PI/180.;
	const double lambda = static_cast<double>(loc.getLongitude())*M_PI/180.;
	const Vec3d up(std::cos(phi)*std::cos(lambda), std::cos(phi)*std::sin(lambda), std::sin(phi));
	const Vec3d east(-std::sin(lambda), std::cos(lambda), 0.);
	const Vec3d north(-std::sin(phi)*std::cos(lambda), -std::sin(phi)*std::sin(lambda), std::cos(phi));
	const Vec3d observer = up * (EarthRadius + std::max(0., loc.altitude*0.001));

	// The direction of the Sun in the Earth-fixed frame, from its direction in the sky of the observer
	// (AltAz: x to the south, y to the east, z up)
	SolarSystem* ssystem = GETSTELMODULE(SolarSystem);
	Vec3d sunAltAz = core->j2000ToAltAz(ssystem->getSun()->getJ2000EquatorialPos(core), StelCore::RefractionOff);
	sunAltAz.normalize();
	const Vec3d sun = north*(-sunAltAz[0]) + east*sunAltAz[1] + up*sunAltAz[2];

	// Magnetic local time is measured from the geomagnetic longitude of the Sun: noon below it
	const MagneticFrame frame = magneticFrame(core->getJD());
	const double sunMagLon = std::atan2(sun*frame.y, sun*frame.x);

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
				const double lon = sunMagLon + (mlt - 12.)*M_PI/12.;
				const Vec3d point = frame.toGeographic(lat, lon) * (EarthRadius + spec.height);

				// In sight unless the line of sight passes below the surface of the Earth
				const Vec3d d = point - observer;
				const double s = -(observer*d)/(d*d);
				line.visible.push_back(s <= 0. || (observer + d*std::min(s, 1.)).norm() >= EarthRadius);

				Vec3d altAz(-(north*d), east*d, up*d);
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
	if (!flagShowOval)
		return;
	if (core->getCurrentPlanet()->getEnglishName() != "Earth")
		return;

	const std::vector<OvalLine> lines = computeOvalLines(core);

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
