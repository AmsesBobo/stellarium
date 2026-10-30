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

#include "LightDomes.hpp"
#include "StelApp.hpp"
#include "StelCore.hpp"
#include "StelSkyDrawer.hpp"
#include "StelLocation.hpp"
#include "StelLocationMgr.hpp"
#include "StelModuleMgr.hpp"
#include "LandscapeMgr.hpp"
#include "StelFileMgr.hpp"
#include "StelPainter.hpp"
#include "StelProjector.hpp"
#include "StelUtils.hpp"
#include "StelRefractionModel.hpp"

#include <QSettings>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringList>
#include <QtConcurrent>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QGuiApplication>
#include <QFontMetrics>
#include <cmath>
#include <array>
#include <numeric>
#include <algorithm>

#ifndef GL_RGB32F
#define GL_RGB32F 0x8815
#endif

namespace
{
constexpr double EARTH_RADIUS_KM = 6371.;   // km
// The table of the scattered light of a town of 1 lumen: distances (log spaced), the altitudes of the map rows,
// azimuths from the town in 5 degree steps, three colors
constexpr int KERNEL_DISTANCES = 56;
constexpr double KERNEL_MIN_DISTANCE = 0.25, KERNEL_MAX_DISTANCE = 400.;  // km
constexpr int KERNEL_AZIMUTHS = 37;
constexpr double KERNEL_AZIMUTH_STEP = 5.;                                // degrees
// The atmosphere: Rayleigh optical depth tau = 0.0088 lambda^-4.05 (lambda in micrometers) over a scale height of
// 8 km; aerosols over 1.2 km, with the Angstrom exponent, single scattering albedo and Henyey-Greenstein g of the
// haze of the photographic mode (StelSkyDrawer::getPhotoAerosol(), from the humidity: 1.3, 0.9 and 0.7 normally)
constexpr double WAVELENGTHS[3] = {0.610, 0.550, 0.465};
constexpr double RAYLEIGH_SCALE_HEIGHT = 8., AEROSOL_SCALE_HEIGHT = 1.2;
// Garstang's emission function: G of the light that falls on the ground is reflected, F goes directly upwards
constexpr double GARSTANG_G = 0.15, GARSTANG_F = 0.15;

// The altitude of a row of the map: from the horizon (limb: the sea-level horizon of an elevated observer, below
// 0 degrees) to the zenith, denser near the horizon
double rowAltitude(int row, double limb = 0.)
{
	const double v = static_cast<double>(row)/(LightDomes::MAP_ALTITUDES - 1);
	return limb + (0.5*M_PI - limb)*v*v;
}

// The geometric depression of the sea-level horizon for an observer at the altitude h (km)
double horizonDip(double h)
{
	return h > 0. ? -std::acos(EARTH_RADIUS_KM/(EARTH_RADIUS_KM + h)) : 0.;
}

// The vertical profile of the aerosols: the boundary layer of the ray-traced model (StelRefractionModel, with the
// growth of its aerosols by the humidity), or an exponential with a scale height of 1.2 km (top 0)
struct AerosolProfile
{
	double top = 0.;      // km; 0: the exponential
	double growth = 1.;
	static constexpr double STEP = 0.01, MAX_HEIGHT = 50.;  // km
	std::vector<double> cumulative;  // column of the relative density from sea level (km)
	double density0 = 1.;
	explicit AerosolProfile(double topKm = 0., double g = 1.) : top(topKm), growth(g)
	{
		if (top <= 0.)
			return;
		density0 = absolute(0.);
		const int n = static_cast<int>(MAX_HEIGHT/STEP) + 1;
		cumulative.resize(static_cast<size_t>(n));
		cumulative[0] = 0.;
		for (int i = 1; i < n; ++i)
			cumulative[static_cast<size_t>(i)] = cumulative[static_cast<size_t>(i-1)] + relative((i - 0.5)*STEP)*STEP;
	}
	double absolute(double h) const
	{
		const double m = 1000.*h, t = 1000.*top;
		return StelRefractionModel::aerosolShape(m, t)*(1. + (growth - 1.)*StelRefractionModel::boundaryLayerFraction(m, t));
	}
	// density against the one at sea level
	double relative(double h) const
	{
		if (top <= 0.)
			return std::exp(-qMax(0., h)/AEROSOL_SCALE_HEIGHT);
		return absolute(qMax(0., h))/density0;
	}
	double cumulativeAt(double h) const
	{
		const double f = qBound(0., h/STEP, static_cast<double>(cumulative.size() - 1));
		const size_t i = qMin(static_cast<size_t>(f), cumulative.size() - 2);
		const double t = f - static_cast<double>(i);
		return cumulative[i] + t*(cumulative[i+1] - cumulative[i]);
	}
	// the column of the relative density between the heights h1 and h2 (km), signed
	double column(double h1, double h2) const
	{
		h1 = qMax(0., h1);
		h2 = qMax(0., h2);
		if (top <= 0.)
			return AEROSOL_SCALE_HEIGHT*(std::exp(-h1/AEROSOL_SCALE_HEIGHT) - std::exp(-h2/AEROSOL_SCALE_HEIGHT));
		return cumulativeAt(h2) - cumulativeAt(h1);
	}
	// the vertical column above sea level (km): the thickness of a uniform layer of the density at sea level
	double thickness() const
	{
		return top <= 0. ? AEROSOL_SCALE_HEIGHT : cumulative.back();
	}
};

double kernelDistance(int i)
{
	return KERNEL_MIN_DISTANCE*std::pow(KERNEL_MAX_DISTANCE/KERNEL_MIN_DISTANCE, static_cast<double>(i)/(KERNEL_DISTANCES - 1));
}

// The optical depths of the two kinds of scatterers between the heights h1 and h2 (km) over a straight path of the
// length L (km), the height changing linearly along it: per unit of their scattering coefficient at sea level
inline void columns(const AerosolProfile& profile, double h1, double h2, double L, double& rayleigh, double& aerosol)
{
	h1 = qMax(0., h1);
	h2 = qMax(0., h2);
	const double dh = h2 - h1;
	const auto column = [&](double H) {
		if (std::fabs(dh) < 1e-4*H)
			return std::exp(-0.5*(h1 + h2)/H)*L;
		return H*L/dh*(std::exp(-h1/H) - std::exp(-h2/H));
	};
	rayleigh = column(RAYLEIGH_SCALE_HEIGHT);
	aerosol = std::fabs(dh) < 1e-3 ? profile.relative(0.5*(h1 + h2))*L : profile.column(h1, h2)*L/dh;
}

struct Air
{
	Vec3d rayleigh;  // scattering coefficients at sea level (1/km), red, green, blue
	Vec3d aerosol;   // extinction coefficients of the aerosols at sea level (1/km), delta-M scaled (see makeAir())
	Vec3d aerosolScattering;  // their scattering coefficients (delta-M scaled)
	double g = 0.7;  // asymmetry of the aerosol scattering (delta-M scaled)
	const AerosolProfile* profile = nullptr;  // the vertical profile of the aerosols
	Vec3d depth(double rayleighColumn, double aerosolColumn) const
	{
		return rayleigh*rayleighColumn + aerosol*aerosolColumn;
	}
};

inline double height(const Vec3d& p) { return p.norm() - EARTH_RADIUS_KM; }

// The air of a vertical aerosol optical depth at sea level (550 nm), with its Angstrom exponent, g and albedo.
// The aerosols scatter strongly forwards: a single scattering model would lose all that light from the beam, though
// most of it goes on in nearly the same direction (in a haze the light of distant towns is dimmed far less than its
// optical depth says). The delta-M approximation (Wiscombe 1977) treats the forward peak (the fraction g^2 of the
// scattered light) as not scattered: the extinction becomes beta (1 - albedo g^2), the scattering beta albedo (1 - g^2)
// with the asymmetry g/(1 + g).
Air makeAir(double aerosolDepth, double angstrom, double g, double albedo, const AerosolProfile& profile)
{
	Air air;
	air.profile = &profile;
	const double f = g*g;
	for (int c=0; c<3; ++c)
	{
		const double lambda = WAVELENGTHS[c];
		air.rayleigh[c] = 0.0088*std::pow(lambda, -4.05)/RAYLEIGH_SCALE_HEIGHT;
		const double extinction = aerosolDepth*std::pow(lambda/0.55, -angstrom)/profile.thickness();
		air.aerosol[c] = extinction*(1. - albedo*f);
		air.aerosolScattering[c] = extinction*albedo*(1. - f);
	}
	air.g = g/(1. + g);
	return air;
}

// Luminance (cd/m^2) at the observer (height h0, km), red, green and blue, towards zenith angle z and azimuth a from
// a town of 1 lumen at the great circle distance d (km): single scattering along the line of sight
Vec3d scatteredLight(const Air& air, double h0, double d, double z, double a)
{
	const Vec3d O(0., 0., EARTH_RADIUS_KM + h0);
	const double phi = d/EARTH_RADIUS_KM;
	const Vec3d S(EARTH_RADIUS_KM*std::sin(phi), 0., EARTH_RADIUS_KM*std::cos(phi));
	const Vec3d upS = S/S.norm();
	const Vec3d u(std::sin(z)*std::cos(a), std::sin(z)*std::sin(a), std::cos(z));
	const double sMax = qMin(2.5*d + 60., 1200.);
	constexpr int STEPS = 120;
	const double s0 = 0.02;
	const double ratio = std::pow(sMax/s0, 1./(STEPS - 1));
	const double g = air.g;
	Vec3d total(0.), toObserver(0.);
	double sEdge = 0., sNext = s0, hPrev = h0, sPrev = 0.;
	for (int i=0; i<STEPS; ++i, sNext *= ratio)
	{
		const double s = 0.5*(sEdge + sNext);
		const double ds = sNext - sEdge;
		sEdge = sNext;
		const Vec3d P = O + u*s;
		const double h = height(P);
		if (h < 0. || h > 50.)
			break;
		double cr, ca;
		columns(*air.profile, hPrev, h, s - sPrev, cr, ca);
		toObserver += air.depth(cr, ca);
		hPrev = h;
		sPrev = s;
		const Vec3d SP = P - S;
		const double r = SP.norm();
		const double cosZ = (SP*upS)/r;
		if (cosZ <= 0.)
			continue;  // below the horizon of the town
		const double zp = std::acos(cosZ);
		const double emission = (2.*GARSTANG_G*(1. - GARSTANG_F)*cosZ + 0.554*GARSTANG_F*zp*zp*zp*zp)/(2.*M_PI);  // cd per lm
		// from the town up to P: two straight pieces (the path curves over the Earth)
		const double hMid = height(0.5*(S + P));
		double r1, a1, r2, a2;
		columns(*air.profile, 0., hMid, 0.5*r, r1, a1);
		columns(*air.profile, hMid, h, 0.5*r, r2, a2);
		const Vec3d fromTown = air.depth(r1 + r2, a1 + a2);
		const double c = -(SP*u)/r;  // cosine of the scattering angle (from S->P towards P->O)
		const double rayleighPhase = 3./(16.*M_PI)*(1. + c*c);
		const double q = 1. + g*g - 2.*g*c;
		const double hgPhase = (1. - g*g)/(4.*M_PI*q*std::sqrt(q));
		const double wR = std::exp(-h/RAYLEIGH_SCALE_HEIGHT)*rayleighPhase;
		const double wA = air.profile->relative(h)*hgPhase;
		const double base = emission/(r*r*1e6)*ds;  // lux per scattering coefficient
		for (int k=0; k<3; ++k)
			total[k] += base*(air.rayleigh[k]*wR + air.aerosolScattering[k]*wA)*std::exp(-fromTown[k] - toObserver[k]);
	}
	return total;
}
}

struct LightDomes::Kernel
{
	AtmosphereKey key;
	std::vector<float> data;  // [color][distance][row][azimuth]
	static size_t index(int c, int d, int row, int a)
	{
		return ((static_cast<size_t>(c)*KERNEL_DISTANCES + d)*MAP_ALTITUDES + row)*KERNEL_AZIMUTHS + a;
	}
	// interpolated in the logarithm of the distance and in the azimuth
	float at(int c, double dist, int row, double azimuthDeg) const
	{
		if (dist > KERNEL_MAX_DISTANCE)
			return 0.f;
		const double fd = qBound(0., std::log(dist/KERNEL_MIN_DISTANCE)/std::log(KERNEL_MAX_DISTANCE/KERNEL_MIN_DISTANCE)*(KERNEL_DISTANCES - 1), KERNEL_DISTANCES - 1.001);
		const double fa = qBound(0., azimuthDeg/KERNEL_AZIMUTH_STEP, KERNEL_AZIMUTHS - 1.001);
		const int d0 = static_cast<int>(fd), a0 = static_cast<int>(fa);
		const float td = static_cast<float>(fd - d0), ta = static_cast<float>(fa - a0);
		const float v00 = data[index(c, d0, row, a0)], v01 = data[index(c, d0, row, a0 + 1)];
		const float v10 = data[index(c, d0 + 1, row, a0)], v11 = data[index(c, d0 + 1, row, a0 + 1)];
		// logarithmic in the distance: the light falls steeply with it
		const auto lerpLog = [](float x, float y, float t) {
			return (x > 0.f && y > 0.f) ? std::exp(std::log(x)*(1.f - t) + std::log(y)*t) : x*(1.f - t) + y*t;
		};
		return lerpLog(v00*(1.f - ta) + v01*ta, v10*(1.f - ta) + v11*ta, td);
	}
};

struct LightDomes::Result
{
	std::vector<float> map;   // [row][azimuth][rgb], cd/m^2
	double zenith = 0.;       // luminance at the zenith
	Vec3f illuminance = Vec3f(0.f);
	struct Label
	{
		QString text;
		double azimuth;           // degrees
		double glow;              // luminance of the dome 5 degrees above the horizon (cd/m^2)
	};
	std::vector<Label> labels;  // the brightest domes first
	bool satellite = false;     // the light of the satellite map is in it
};

LightDomes::LightDomes()
{
	setObjectName("LightDomes");
}

LightDomes::~LightDomes()
{
	if (jobRunning)
		job.waitForFinished();
	QOpenGLContext* ctx = QOpenGLContext::currentContext();
	if (texture && ctx)
		ctx->functions()->glDeleteTextures(1, &texture);
}

void LightDomes::init()
{
	QSettings* conf = StelApp::getInstance().getSettings();
	flagEnabled = conf->value("viewing/flag_photographic_light_domes", false).toBool();
	lumensPerInhabitant = qBound(10., conf->value("viewing/photographic_light_dome_lumens", 1000.).toDouble(), 100000.);
	setSources(conf->value("viewing/photographic_light_domes", "").toString());
	flagDatabaseTowns = conf->value("viewing/flag_photographic_light_domes_database", false).toBool();
	databaseTownLamp = qBound(0, conf->value("viewing/photographic_light_dome_database_lamp", 0).toInt(), static_cast<int>(LampLED4000));
	flagTownLabels = conf->value("viewing/flag_photographic_light_dome_labels", false).toBool();
	lightSource = qBound(0, conf->value("viewing/photographic_light_dome_source", 0).toInt(), static_cast<int>(SourceSatellite));
	nightLights = new NightLightsMap(this);
	connect(nightLights, &NightLightsMap::downloadFinished, this, [this](bool ok, const QString& message) {
		satelliteMessage = ok ? QString() : message;
		dirty = true;
		emit satelliteDataChanged();
	});
	connect(nightLights, &NightLightsMap::downloadProgress, this, &LightDomes::satelliteDataChanged);
	dirty = true;
}

QString LightDomes::getSources() const
{
	QStringList list;
	for (const auto& s : sources)
		list << QString("%1,%2,%3,%4").arg(s.azimuth, 0, 'f', 1).arg(s.distance, 0, 'f', 2).arg(s.population, 0, 'f', 0).arg(s.lamp);
	return list.join(';');
}

void LightDomes::setSources(const QString& str)
{
	std::vector<Source> list;
	for (const QString& item : str.split(';', Qt::SkipEmptyParts))
	{
		const QStringList f = item.split(',');
		if (f.size() < 3)
			continue;
		Source s;
		s.azimuth = std::fmod(std::fmod(f[0].toDouble(), 360.) + 360., 360.);
		s.distance = qBound(0.1, f[1].toDouble(), KERNEL_MAX_DISTANCE);
		s.population = qBound(0., f[2].toDouble(), 1e8);
		s.lamp = f.size() > 3 ? qBound(0, f[3].toInt(), static_cast<int>(LampLED4000)) : static_cast<int>(LampMixed);
		list.push_back(s);
	}
	sources = list;
	const QString normalized = getSources();
	StelApp::immediateSave("viewing/photographic_light_domes", normalized);
	dirty = true;
	emit sourcesChanged(normalized);
}

void LightDomes::setFlagEnabled(bool b)
{
	if (b == flagEnabled)
		return;
	flagEnabled = b;
	StelApp::immediateSave("viewing/flag_photographic_light_domes", b);
	emit flagEnabledChanged(b);
}

void LightDomes::setLumensPerInhabitant(double lm)
{
	lm = qBound(10., lm, 100000.);
	if (qFuzzyCompare(lm, lumensPerInhabitant))
		return;
	lumensPerInhabitant = lm;
	StelApp::immediateSave("viewing/photographic_light_dome_lumens", lm);
	dirty = true;
	emit lumensPerInhabitantChanged(lm);
}

void LightDomes::setFlagDatabaseTowns(bool b)
{
	if (b == flagDatabaseTowns)
		return;
	flagDatabaseTowns = b;
	StelApp::immediateSave("viewing/flag_photographic_light_domes_database", b);
	databasePlanet.clear();  // found again (or cleared) in update()
	if (!b)
	{
		databaseTowns.clear();
		databaseDistricts = 0;
		emit databaseTownsChanged();
	}
	dirty = true;
	emit flagDatabaseTownsChanged(b);
}

void LightDomes::setDatabaseTownLamp(int lamp)
{
	lamp = qBound(0, lamp, static_cast<int>(LampLED4000));
	if (lamp == databaseTownLamp)
		return;
	databaseTownLamp = lamp;
	StelApp::immediateSave("viewing/photographic_light_dome_database_lamp", lamp);
	dirty = true;
	emit databaseTownLampChanged(lamp);
}

void LightDomes::setFlagTownLabels(bool b)
{
	if (b == flagTownLabels)
		return;
	flagTownLabels = b;
	StelApp::immediateSave("viewing/flag_photographic_light_dome_labels", b);
	emit flagTownLabelsChanged(b);
}

void LightDomes::setLightSource(int source)
{
	source = qBound(0, source, static_cast<int>(SourceSatellite));
	if (source == lightSource)
		return;
	lightSource = source;
	StelApp::immediateSave("viewing/photographic_light_dome_source", source);
	dirty = true;
	emit lightSourceChanged(source);
}

bool LightDomes::hasSatelliteData() const
{
	const StelLocation& here = StelApp::getInstance().getCore()->getCurrentLocation();
	return nightLights && here.planetName == QStringLiteral("Earth")
	       && nightLights->hasRegion(static_cast<double>(here.getLatitude()), static_cast<double>(here.getLongitude()), KERNEL_MAX_DISTANCE);
}

bool LightDomes::isSatelliteInUse() const
{
	return result && result->satellite;
}

void LightDomes::downloadSatelliteData()
{
	const StelLocation& here = StelApp::getInstance().getCore()->getCurrentLocation();
	if (!nightLights || here.planetName != QStringLiteral("Earth"))
		return;
	satelliteMessage.clear();
	nightLights->download(static_cast<double>(here.getLatitude()), static_cast<double>(here.getLongitude()), KERNEL_MAX_DISTANCE);
	emit satelliteDataChanged();
}

void LightDomes::recompute()
{
	if (flagDatabaseTowns && flagEnabled)
		updateDatabaseTowns();
	else
		databasePlanet.clear();
	dirty = true;
}

double LightDomes::nearbyLightRatio(double depth, double angstrom, double g, double albedo,
                                    double normalDepth, double normalAngstrom, double normalG, double normalAlbedo, double altitudeKm)
{
	// The lamps around the observer as an even carpet of light from 0.25 to 20 km, seen at the zenith (green); the
	// optical depths at the observer taken back to sea level
	const double toSeaLevel = std::exp(altitudeKm/AEROSOL_SCALE_HEIGHT);
	const AerosolProfile exponential;
	const Air now = makeAir(depth*toSeaLevel, angstrom, g, albedo, exponential);
	const Air normal = makeAir(normalDepth*toSeaLevel, normalAngstrom, normalG, normalAlbedo, exponential);
	double a = 0., b = 0.;
	constexpr int N = 24;
	const double ratio = std::pow(20./0.25, 1./N);
	for (int i=0; i<N; ++i)
	{
		const double d0 = 0.25*std::pow(ratio, i), d1 = d0*ratio, d = std::sqrt(d0*d1);
		const double area = d*(d1 - d0);  // of the ring, up to 2 pi
		a += area*scatteredLight(now, altitudeKm, d, 0.002, 0.)[1];
		b += area*scatteredLight(normal, altitudeKm, d, 0.002, 0.)[1];
	}
	return b > 0. ? a/b : 1.;
}

double LightDomes::maxDistance()
{
	return KERNEL_MAX_DISTANCE;
}

namespace
{
// The radius of the disk of a town (km): about 0.5 km sqrt(P/1000), 5 km for 100000 inhabitants
inline double townRadius(double population)
{
	return qMax(0.2, 0.5*std::sqrt(population/1000.));
}
}

void LightDomes::updateDatabaseTowns()
{
	const StelLocation& here = StelApp::getInstance().getCore()->getCurrentLocation();
	databasePlanet = here.planetName;
	databaseLatitude = static_cast<double>(here.getLatitude());
	databaseLongitude = static_cast<double>(here.getLongitude());
	databaseTowns.clear();
	databaseDistricts = 0;
	if (here.planetName != QStringLiteral("Earth"))
	{
		emit databaseTownsChanged();
		dirty = true;
		return;
	}
	const auto unitVector = [](double latDeg, double lonDeg) {
		const double la = latDeg*M_PI/180., lo = lonDeg*M_PI/180.;
		return Vec3d(std::cos(la)*std::cos(lo), std::cos(la)*std::sin(lo), std::sin(la));
	};
	const Vec3d O = unitVector(databaseLatitude, databaseLongitude);
	const Vec3d north(-std::sin(databaseLatitude*M_PI/180.)*std::cos(databaseLongitude*M_PI/180.),
	                  -std::sin(databaseLatitude*M_PI/180.)*std::sin(databaseLongitude*M_PI/180.),
	                   std::cos(databaseLatitude*M_PI/180.));
	const Vec3d east(-std::sin(databaseLongitude*M_PI/180.), std::cos(databaseLongitude*M_PI/180.), 0.);
	// The towns of the base list (not the own locations of the user: they may be copies of them) on the Earth,
	// with inhabitants (not observatories, landing sites and the like), within the reach of the table
	struct Candidate { QString name; Vec3d p; double distance, population; };
	std::vector<Candidate> candidates;
	const double cosMax = std::cos(KERNEL_MAX_DISTANCE/EARTH_RADIUS_KM);
	const LocationMap all = StelApp::getInstance().getLocationMgr().getAllMap();
	for (auto it = all.cbegin(); it != all.cend(); ++it)
	{
		const StelLocation& loc = it.value();
		if (loc.isUserLocation || loc.population <= 0 || loc.planetName != QStringLiteral("Earth"))
			continue;
		const QChar role = loc.role.toUpper();
		if (role == 'O' || role == 'L' || role == 'A' || role == 'I' || role == 'H')
			continue;
		const Vec3d p = unitVector(static_cast<double>(loc.getLatitude(true)), static_cast<double>(loc.getLongitude(true)));
		const double c = p*O;
		if (c < cosMax)
			continue;
		candidates.push_back({loc.name, p, std::acos(qMin(1., c))*EARTH_RADIUS_KM, static_cast<double>(loc.population)});
	}
	// The largest first: a place inside the disk of a larger town is a district of it (or the same place twice)
	std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
		return a.population > b.population || (a.population == b.population && a.name < b.name);
	});
	std::vector<const Candidate*> kept;
	std::vector<double> keptCosRadius;
	for (const auto& c : candidates)
	{
		bool district = false;
		for (size_t i=0; i<kept.size() && !district; ++i)
		{
			if (c.p*kept[i]->p > keptCosRadius[i])  // closer than the radius of the larger town
			{
				district = true;
				++databaseTowns[i].districts;
			}
		}
		if (district)
		{
			++databaseDistricts;
			continue;
		}
		kept.push_back(&c);
		keptCosRadius.push_back(std::cos(townRadius(c.population)/EARTH_RADIUS_KM));
		DatabaseTown t;
		t.name = c.name;
		t.distance = c.distance;
		t.population = c.population;
		const Vec3d toward = c.p - O*(c.p*O);  // in the plane of the horizon
		t.azimuth = toward.normSquared() > 0. ? std::fmod(std::atan2(toward*east, toward*north)*180./M_PI + 360., 360.) : 0.;
		databaseTowns.push_back(t);
	}
	// for the list: the largest contribution first (Walker's law: the population over the distance^2.5)
	std::stable_sort(databaseTowns.begin(), databaseTowns.end(), [](const DatabaseTown& a, const DatabaseTown& b) {
		const auto w = [](const DatabaseTown& t) {return t.population*std::pow(qMax(t.distance, townRadius(t.population)), -2.5);};
		return w(a) > w(b);
	});
	dirty = true;
	emit databaseTownsChanged();
}

Vec3f LightDomes::lampColor(int lamp)
{
	// Linear sRGB of the scene per unit luminance against daylight (D55), from their spectra: sodium lamps as a
	// 2100 K blackbody, the sodium line at 589 nm, LEDs as blackbodies of their color temperature
	switch (lamp)
	{
		case LampHPS:     return Vec3f(2.185f, 0.686f, 0.051f);
		case LampLPS:     return Vec3f(2.519f, 0.592f, 0.f);
		case LampLED3000: return Vec3f(1.604f, 0.856f, 0.331f);
		case LampLED4000: return Vec3f(1.281f, 0.936f, 0.648f);
		default:          return Vec3f(1.733f, 0.811f, 0.350f);  // half HPS, half LED 4000 K (by luminance)
	}
}

LightDomes::AtmosphereKey LightDomes::currentAtmosphere() const
{
	const StelCore* core = StelApp::getInstance().getCore();
	AtmosphereKey key;
	const double h = qMax(0., static_cast<double>(core->getCurrentLocation().altitude)/1000.);
	key.altitude = std::round(h*20.)/20.;  // 50 m steps
	// The aerosols from the extinction coefficient (V magnitudes per airmass, at the observer): what the air
	// (Rayleigh above the observer) and ozone (0.016) do not explain, at least 0.01, taken back to sea level
	// The haze of the sky settings (the extinction coefficient and the humidity), at the observer, taken back to sea
	// level; quantized, so that small changes need no new table
	const StelSkyDrawer::PhotoAerosol& haze = core->getSkyDrawer()->getPhotoAerosol();
	const Extinction& extinction = core->getSkyDrawer()->getExtinction();
	const StelRefractionModel* model = extinction.getModel();
	if (model)
	{
		// The boundary layer of the ray-traced model: its top, the growth of its aerosols by the humidity, and the
		// vertical depth of all aerosols above sea level (with the growth)
		const auto& c = model->getConditions();
		key.layerTop = qMax(0.05, std::round(c.aerosolLayerTop/50.)*0.05);
		key.growth = std::round(c.aerosolLayerGrowth*20.)/20.;
		double column = 0.;
		for (double m = 5.; m < 20000.; m += 10.)
			column += model->aerosolDensity(m)*10.;
		key.aerosolDepth = std::round(qMin(3., extinction.getAerosolSeaLevelDepth()*column)*200.)/200.;
	}
	else
		key.aerosolDepth = std::round(qMin(3., haze.depth*std::exp(key.altitude/AEROSOL_SCALE_HEIGHT))*200.)/200.;
	key.angstrom = std::round(haze.angstrom*20.)/20.;
	key.asymmetry = std::round(haze.asymmetry*100.)/100.;
	key.albedo = std::round(haze.albedo*100.)/100.;
	return key;
}

namespace
{
// The table only depends on the atmosphere: it is kept in the cache directory (a few seconds to compute)
QString kernelCacheFile(double aerosolDepth, double altitude, double angstrom, double g, double albedo, double layerTop, double growth)
{
	return QString("%1/lightdomes/kernel-v4-%2-%3-%4-%5-%6-%7-%8.bin").arg(StelFileMgr::getCacheDir()).arg(qRound(aerosolDepth*1000.)).arg(qRound(altitude*1000.))
	        .arg(qRound(angstrom*100.)).arg(qRound(g*100.)).arg(qRound(albedo*100.)).arg(qRound(layerTop*1000.)).arg(qRound(growth*100.));
}
}

std::shared_ptr<LightDomes::Kernel> LightDomes::computeKernel(const AtmosphereKey& key)
{
	const size_t count = static_cast<size_t>(3)*KERNEL_DISTANCES*MAP_ALTITUDES*KERNEL_AZIMUTHS;
	const QString cacheFile = kernelCacheFile(key.aerosolDepth, key.altitude, key.angstrom, key.asymmetry, key.albedo, key.layerTop, key.growth);
	{
		QFile f(cacheFile);
		if (f.open(QIODevice::ReadOnly) && f.size() == static_cast<qint64>(count*sizeof(float)))
		{
			auto kern = std::make_shared<Kernel>();
			kern->key = key;
			kern->data.resize(count);
			if (f.read(reinterpret_cast<char*>(kern->data.data()), f.size()) == f.size())
				return kern;
		}
	}
	auto kern = std::make_shared<Kernel>();
	kern->key = key;
	kern->data.assign(static_cast<size_t>(3)*KERNEL_DISTANCES*MAP_ALTITUDES*KERNEL_AZIMUTHS, 0.f);
	std::vector<int> jobs(KERNEL_DISTANCES*MAP_ALTITUDES);
	std::iota(jobs.begin(), jobs.end(), 0);
	Kernel* k = kern.get();
	const AerosolProfile profile(key.layerTop, key.growth);
	const Air air = makeAir(key.aerosolDepth, key.angstrom, key.asymmetry, key.albedo, profile);
	// From altitude the rows reach down to the sea-level horizon (the rays below it hit the ground)
	const double limb = horizonDip(key.altitude);
	QtConcurrent::blockingMap(jobs, [k, key, air, limb](int job)
	{
		const int d = job / MAP_ALTITUDES, row = job % MAP_ALTITUDES;
		const double dist = kernelDistance(d);
		const double z = 0.5*M_PI - qMax(rowAltitude(row, limb), limb + 0.002);  // the horizon itself: just above it
		for (int a=0; a<KERNEL_AZIMUTHS; ++a)
		{
			const Vec3d L = scatteredLight(air, key.altitude, dist, z, a*KERNEL_AZIMUTH_STEP*M_PI/180.);
			for (int c=0; c<3; ++c)
				k->data[Kernel::index(c, d, row, a)] = static_cast<float>(L[c]);
		}
	});
	QDir().mkpath(QFileInfo(cacheFile).absolutePath());
	QFile f(cacheFile);
	if (f.open(QIODevice::WriteOnly))
		f.write(reinterpret_cast<const char*>(kern->data.data()), static_cast<qint64>(count*sizeof(float)));
	return kern;
}

namespace
{
// The row 5 degrees above the horizon, for the labels
int labelRow()
{
	return qRound((LightDomes::MAP_ALTITUDES - 1)*std::sqrt(5./90.));
}
// The VIIRS radiance of the Swedish towns of the location database (Stockholm to Motala and Naessjoe), summed within
// 1.5 times their radius (townRadius()): 0.013 to 0.022 W/sr per inhabitant upwards, in the band of the satellite
// (500-900 nm). The satellite map is calibrated with it to the lumens per inhabitant of the population model.
constexpr double VIIRS_WSR_PER_INHABITANT = 0.018;
}

void LightDomes::addSatelliteLight(const Kernel& kern, const JobInput& in, Result& res)
{
	float pixelDeg = 0.f;
	const std::vector<NightLightsMap::Pixel> pixels = NightLightsMap::readRegion(in.snapshot, in.latitude, in.longitude, KERNEL_MAX_DISTANCE, &pixelDeg);
	res.satellite = true;
	if (pixels.empty())
		return;
	// The light of each pixel: its radiance times its area, as the lumens of the population model
	const double lumensPerWsr = in.lumensPerInhabitant/VIIRS_WSR_PER_INHABITANT;
	const double pixelRad = static_cast<double>(pixelDeg)*M_PI/180.;
	const double pixelArea = std::pow(EARTH_RADIUS_KM*1e5*pixelRad, 2);  // cm^2 at the equator
	const auto unit = [](double latDeg, double lonDeg) {
		const double la = latDeg*M_PI/180., lo = lonDeg*M_PI/180.;
		return Vec3d(std::cos(la)*std::cos(lo), std::cos(la)*std::sin(lo), std::sin(la));
	};
	const double la0 = in.latitude*M_PI/180., lo0 = in.longitude*M_PI/180.;
	const Vec3d O = unit(in.latitude, in.longitude);
	const Vec3d north(-std::sin(la0)*std::cos(lo0), -std::sin(la0)*std::sin(lo0), std::cos(la0));
	const Vec3d east(-std::sin(lo0), std::cos(lo0), 0.);
	// The light binned by the distance (the nodes of the table) and the azimuth (1 degree) around the observer
	std::vector<double> bins(static_cast<size_t>(KERNEL_DISTANCES)*MAP_AZIMUTHS, 0.);
	const double logRange = std::log(KERNEL_MAX_DISTANCE/KERNEL_MIN_DISTANCE);
	const auto addLight = [&](const Vec3d& p, double flux) {
		const double c = qBound(-1., p*O, 1.);
		const double d = std::acos(c)*EARTH_RADIUS_KM;
		if (d > KERNEL_MAX_DISTANCE)
			return;
		const Vec3d t = p - O*c;
		const double az = t.normSquared() > 0. ? std::atan2(t*east, t*north)*180./M_PI : 0.;
		const double fd = qBound(0., std::log(qMax(d, KERNEL_MIN_DISTANCE)/KERNEL_MIN_DISTANCE)/logRange*(KERNEL_DISTANCES - 1), KERNEL_DISTANCES - 1.0001);
		const int d0 = static_cast<int>(fd);
		const double td = fd - d0;
		double fa = az - 0.5;  // column a covers the azimuths a to a+1
		fa -= 360.*std::floor(fa/360.);
		const int a0 = static_cast<int>(fa) % MAP_AZIMUTHS, a1 = (a0 + 1) % MAP_AZIMUTHS;
		const double ta = fa - std::floor(fa);
		bins[static_cast<size_t>(d0)*MAP_AZIMUTHS + a0]       += flux*(1. - td)*(1. - ta);
		bins[static_cast<size_t>(d0)*MAP_AZIMUTHS + a1]       += flux*(1. - td)*ta;
		bins[static_cast<size_t>(d0 + 1)*MAP_AZIMUTHS + a0]   += flux*td*(1. - ta);
		bins[static_cast<size_t>(d0 + 1)*MAP_AZIMUTHS + a1]   += flux*td*ta;
	};
	// the towns of the database, for their labels: the light within 1.5 times their radius
	std::vector<Vec3d> townCentre;
	std::vector<double> townCos, townFlux(in.towns.size(), 0.);
	for (const auto& town : in.towns)
	{
		// back from the azimuth and distance to the place
		const double a = town.azimuth*M_PI/180., g = town.distance/EARTH_RADIUS_KM;
		townCentre.push_back(O*std::cos(g) + (north*std::cos(a) + east*std::sin(a))*std::sin(g));
		townCos.push_back(std::cos(1.5*townRadius(town.population)/EARTH_RADIUS_KM));
	}
	constexpr double NEAR = 3.;  // km: pixels closer than this are split into 4 x 4 points
	const double cosNear = std::cos(NEAR/EARTH_RADIUS_KM);
	for (const auto& px : pixels)
	{
		const double lat = static_cast<double>(px.lat), lon = static_cast<double>(px.lon);
		const double flux = static_cast<double>(px.radiance)*pixelArea*std::cos(lat*M_PI/180.)*1e-9*lumensPerWsr;  // nW -> W/sr -> lm
		const Vec3d p = unit(lat, lon);
		for (size_t i=0; i<townCentre.size(); ++i)
			if (p*townCentre[i] > townCos[i])
				townFlux[i] += flux;
		if (p*O < cosNear)
		{
			addLight(p, flux);
			continue;
		}
		for (int i=0; i<4; ++i)
			for (int j=0; j<4; ++j)
				addLight(unit(lat + (i - 1.5)*0.25*static_cast<double>(pixelDeg), lon + (j - 1.5)*0.25*static_cast<double>(pixelDeg)), flux/16.);
	}
	// For each distance the light by the azimuth (only the bins with light)
	std::vector<std::vector<std::pair<int, float>>> lit(KERNEL_DISTANCES);
	for (int d=0; d<KERNEL_DISTANCES; ++d)
		for (int a=0; a<MAP_AZIMUTHS; ++a)
		{
			const double f = bins[static_cast<size_t>(d)*MAP_AZIMUTHS + a];
			if (f > 0.)
				lit[static_cast<size_t>(d)].emplace_back(a, static_cast<float>(f));
		}
	const Vec3f color = lampColor(in.satelliteLamp);
	// The sky: for each row the sum over the distances of the light around times the table, by the difference of
	// the azimuths (a circular convolution)
	std::vector<int> rows(MAP_ALTITUDES);
	std::iota(rows.begin(), rows.end(), 0);
	float* map = res.map.data();
	QtConcurrent::blockingMap(rows, [&kern, &lit, color, map](int row)
	{
		std::vector<float> extended(3*2*MAP_AZIMUTHS);  // the table at the azimuth differences -360..359 degrees
		std::vector<float> sum(3*MAP_AZIMUTHS, 0.f);
		for (int d=0; d<KERNEL_DISTANCES; ++d)
		{
			const auto& list = lit[static_cast<size_t>(d)];
			if (list.empty())
				continue;
			for (int c=0; c<3; ++c)
			{
				float* ext = &extended[static_cast<size_t>(c)*2*MAP_AZIMUTHS];
				for (int k=0; k<2*MAP_AZIMUTHS; ++k)
				{
					int delta = std::abs(k - MAP_AZIMUTHS);
					if (delta > 180)
						delta = 360 - delta;
					const int a0 = qMin(delta/static_cast<int>(KERNEL_AZIMUTH_STEP), KERNEL_AZIMUTHS - 2);
					const float t = static_cast<float>(delta - a0*KERNEL_AZIMUTH_STEP)/static_cast<float>(KERNEL_AZIMUTH_STEP);
					ext[k] = color[c]*(kern.data[Kernel::index(c, d, row, a0)]*(1.f - t) + kern.data[Kernel::index(c, d, row, a0 + 1)]*t);
				}
			}
			for (const auto& [a, f] : list)
				for (int c=0; c<3; ++c)
				{
					// the column col gets the table at col - a
					const float* ext = &extended[static_cast<size_t>(c)*2*MAP_AZIMUTHS + MAP_AZIMUTHS - a];
					float* out = &sum[static_cast<size_t>(c)*MAP_AZIMUTHS];
					for (int col=0; col<MAP_AZIMUTHS; ++col)
						out[col] += f*ext[col];
				}
		}
		for (int col=0; col<MAP_AZIMUTHS; ++col)
			for (int c=0; c<3; ++c)
				map[(static_cast<size_t>(row)*MAP_AZIMUTHS + col)*3 + c] += sum[static_cast<size_t>(c)*MAP_AZIMUTHS + col];
	});
	// The labels of the towns, with the light measured around them
	const int row = labelRow();
	for (size_t i=0; i<in.towns.size(); ++i)
	{
		const DatabaseTown& town = in.towns[i];
		if (!(townFlux[i] > 0.) || town.distance <= townRadius(town.population))
			continue;
		const double glow = townFlux[i]*static_cast<double>(0.2126f*color[0]*kern.at(0, town.distance, row, 0.)
		                                                 + 0.7152f*color[1]*kern.at(1, town.distance, row, 0.)
		                                                 + 0.0722f*color[2]*kern.at(2, town.distance, row, 0.));
		res.labels.push_back({QString("%1 (%2 km)").arg(town.name).arg(qRound(town.distance)), town.azimuth, glow});
	}
}

std::shared_ptr<LightDomes::Result> LightDomes::computeMap(std::shared_ptr<Kernel> kern, const JobInput& in)
{
	auto res = std::make_shared<Result>();
	res->map.assign(static_cast<size_t>(MAP_ALTITUDES)*MAP_AZIMUTHS*3, 0.f);
	const std::vector<Source>& sources = in.sources;
	const std::vector<QString>& names = in.names;
	const double lumensPerInhabitant = in.lumensPerInhabitant;
	const int labelRow = ::labelRow();
	if (in.satellite)
		addSatelliteLight(*kern, in, *res);
	for (size_t i=0; i<sources.size(); ++i)
	{
		const Source& s = sources[i];
		const double flux = s.population*lumensPerInhabitant;
		if (!(flux > 0.))
			continue;
		const Vec3f color = lampColor(s.lamp);
		// A label for a town of the database that is not around the observer
		if (i < names.size() && !names[i].isEmpty() && s.distance > townRadius(s.population))
		{
			const double glow = flux*static_cast<double>(0.2126f*color[0]*kern->at(0, s.distance, labelRow, 0.)
			                                       + 0.7152f*color[1]*kern->at(1, s.distance, labelRow, 0.)
			                                       + 0.0722f*color[2]*kern->at(2, s.distance, labelRow, 0.));
			res->labels.push_back({QString("%1 (%2 km)").arg(names[i]).arg(qRound(s.distance)), s.azimuth, glow});
		}
		// A town is not a point: a disk of about 0.5 km sqrt(P/1000) (5 km for 100000 inhabitants), as seven points
		const double radius = townRadius(s.population);
		const double az = s.azimuth*M_PI/180.;
		const double cx = s.distance*std::sin(az), cy = s.distance*std::cos(az);  // east, north (km)
		for (int p=0; p<7; ++p)
		{
			double x = cx, y = cy, w = 0.25;
			if (p > 0)
			{
				const double t = (p - 1)*M_PI/3.;
				x += 0.7*radius*std::cos(t);
				y += 0.7*radius*std::sin(t);
				w = 0.125;
			}
			const double dist = qMax(KERNEL_MIN_DISTANCE, std::hypot(x, y));
			if (dist > KERNEL_MAX_DISTANCE)
				continue;
			const double azDeg = std::atan2(x, y)*180./M_PI;
			const float f = static_cast<float>(flux*w);
			for (int row=0; row<MAP_ALTITUDES; ++row)
			{
				// the light of this point by the azimuth from it (interpolated in the distance once)
				float profile[KERNEL_AZIMUTHS][3];
				for (int k=0; k<KERNEL_AZIMUTHS; ++k)
					for (int c=0; c<3; ++c)
						profile[k][c] = f*color[c]*kern->at(c, dist, row, k*KERNEL_AZIMUTH_STEP);
				for (int col=0; col<MAP_AZIMUTHS; ++col)
				{
					double da = std::fabs(std::fmod(col + 0.5 - azDeg + 720., 360.));
					if (da > 180.)
						da = 360. - da;
					const double fa = qMin(da/KERNEL_AZIMUTH_STEP, KERNEL_AZIMUTHS - 1.001);
					const int a0 = static_cast<int>(fa);
					const float t = static_cast<float>(fa - a0);
					float* px = &res->map[(static_cast<size_t>(row)*MAP_AZIMUTHS + col)*3];
					for (int c=0; c<3; ++c)
						px[c] += profile[a0][c]*(1.f - t) + profile[a0 + 1][c]*t;
				}
			}
		}
	}
	// The zenith, and the light on level ground: the integral of L sin(alt) over the sky
	double zenith = 0.;
	Vec3d E(0.);
	// (rows below 0 degrees, seen from altitude, do not light level ground)
	const double limb = horizonDip(kern->key.altitude);
	for (int row=0; row<MAP_ALTITUDES; ++row)
	{
		const double alt = rowAltitude(row, limb);
		const double lo = qMax(0., row > 0 ? 0.5*(rowAltitude(row - 1, limb) + alt) : limb);
		const double hi = qMax(0., row < MAP_ALTITUDES - 1 ? 0.5*(alt + rowAltitude(row + 1, limb)) : 0.5*M_PI);
		const double weight = (std::sin(hi)*std::sin(hi) - std::sin(lo)*std::sin(lo))*0.5*(2.*M_PI/MAP_AZIMUTHS);  // sin(alt) cos(alt) dalt daz
		for (int col=0; col<MAP_AZIMUTHS; ++col)
		{
			const float* px = &res->map[(static_cast<size_t>(row)*MAP_AZIMUTHS + col)*3];
			for (int c=0; c<3; ++c)
				E[c] += weight*static_cast<double>(px[c]);
			if (row == MAP_ALTITUDES - 1)
				zenith += (0.2126*px[0] + 0.7152*px[1] + 0.0722*px[2])/MAP_AZIMUTHS;
		}
	}
	res->zenith = zenith;
	std::stable_sort(res->labels.begin(), res->labels.end(), [](const Result::Label& a, const Result::Label& b) {return a.glow > b.glow;});
	res->illuminance.set(static_cast<float>(E[0]), static_cast<float>(E[1]), static_cast<float>(E[2]));
	return res;
}

void LightDomes::startComputation()
{
	const AtmosphereKey key = currentAtmosphere();
	const std::shared_ptr<Kernel> known = (kernel && kernel->key == key) ? kernel : nullptr;
	JobInput in;
	in.sources = sources;
	in.names.resize(in.sources.size());
	in.lumensPerInhabitant = lumensPerInhabitant;
	if (flagDatabaseTowns)
	{
		// the satellite map when it is there for the place, else the population
		if (lightSource == SourceSatellite && databasePlanet == QStringLiteral("Earth") && hasSatelliteData())
		{
			in.satellite = true;
			in.snapshot = nightLights->snapshot();
			in.latitude = databaseLatitude;
			in.longitude = databaseLongitude;
			in.satelliteLamp = databaseTownLamp;
			in.towns = databaseTowns;
		}
		else
		{
			for (const auto& t : databaseTowns)
			{
				in.names.push_back(t.name);
				Source s;
				s.azimuth = t.azimuth;
				s.distance = qMax(0.1, t.distance);
				s.population = t.population;
				s.lamp = databaseTownLamp;
				in.sources.push_back(s);
			}
		}
	}
	jobAtmosphere = key;
	job = QtConcurrent::run([known, key, in]()
	{
		const std::shared_ptr<Kernel> k = known ? known : computeKernel(key);
		return std::make_pair(k, computeMap(k, in));
	});
	jobRunning = true;
	dirty = false;
}

void LightDomes::update(double deltaTime)
{
	Q_UNUSED(deltaTime)
	if (jobRunning && job.isFinished())
	{
		const auto r = job.result();
		kernel = r.first;
		result = r.second;
		jobRunning = false;
		textureStale = true;
		emit mapChanged();
	}
	if (!flagEnabled)
		return;
	// the towns of the location database: found again when the observer moved (by more than about 100 m)
	if (flagDatabaseTowns)
	{
		const StelLocation& here = StelApp::getInstance().getCore()->getCurrentLocation();
		if (here.planetName != databasePlanet
		    || std::fabs(static_cast<double>(here.getLatitude()) - databaseLatitude) > 1e-3
		    || std::fabs(static_cast<double>(here.getLongitude()) - databaseLongitude) > 1e-3)
			updateDatabaseTowns();
	}
	if (jobRunning)
		return;
	// compute again when the towns, the lumens or the atmosphere (extinction, altitude) changed
	if (dirty || !kernel || !(currentAtmosphere() == kernel->key))
		startComputation();
}

double LightDomes::getCallOrder(StelModuleActionName actionName) const
{
	// the labels over the landscape, like its gazetteer
	if (actionName == StelModule::ActionDraw)
		return StelApp::getInstance().getModuleMgr().getModule("LandscapeMgr")->getCallOrder(actionName) + 1.;
	return 0.;
}

void LightDomes::draw(StelCore* core)
{
	if (!flagTownLabels || !flagDatabaseTowns || !isActive() || result->labels.empty())
		return;
	// The domes that show: 5 degrees above the horizon at least 2 % of a dark natural sky there (about 4e-4
	// cd/m^2), the 25 brightest
	constexpr double MIN_GLOW = 8e-6;
	constexpr size_t MAX_LABELS = 25;
	static LandscapeMgr* lmgr = GETSTELMODULE(LandscapeMgr);
	const StelProjectorP prj = core->getProjection(StelCore::FrameAltAz, StelCore::RefractionOff);
	StelPainter painter(prj);
	const float ppx = static_cast<float>(core->getCurrentStelProjectorParams().devicePixelsPerPixel);
	const int fontSize = lmgr ? lmgr->getLabelFontSize() : 12;
	const int angle = lmgr ? lmgr->getLabelAngle() : 45;
	QFont font = QGuiApplication::font();
	font.setPixelSize(fontSize);
	painter.setFont(font);
	const QFontMetrics fm(font);
	painter.setColor(lmgr ? lmgr->getLabelColor() : Vec3f(0.f, 1.f, 0.f), 1.f);
	painter.setBlending(true);
	painter.setLineSmooth(true);
	// The brightest first; a fainter one that would overlap a name already written is left out (the tilted
	// names are parallel: they need about one line height between them)
	const bool tilted = angle > 0 && !prj->getFlagGravityLabels();
	const double lineHeight = 1.3*fontSize*static_cast<double>(ppx);
	std::vector<std::pair<double, double>> placed;  // screen x of the foot, width of the name
	size_t shown = 0;
	for (size_t i=0; i<result->labels.size() && shown<MAX_LABELS; ++i)
	{
		const Result::Label& label = result->labels[i];
		if (label.glow < MIN_GLOW)
			break;
		// as in the gazetteer of a landscape: a line from the horizon up to the name
		Vec3d foot, top, win;
		const double az = (180. - label.azimuth)*M_PI/180.;
		StelUtils::spheToRect(az, 0., foot);
		StelUtils::spheToRect(az, 1.5*M_PI/180., top);
		if (!prj->project(foot, win))
			continue;
		const int textWidth = qRound(ppx*fm.boundingRect(label.text).width());
		bool overlaps = false;
		for (const auto& p : placed)
		{
			const double need = tilted ? lineHeight/std::sin(angle*M_PI/180.) : 0.5*(p.second + textWidth) + 4.*ppx;
			if (std::fabs(win[0] - p.first) < need)
			{
				overlaps = true;
				break;
			}
		}
		if (overlaps)
			continue;
		placed.emplace_back(win[0], textWidth);
		++shown;
		if (prj->getFlagGravityLabels())
			painter.drawText(top, label.text, 0, 0, 0, false);
		else if (angle > 0)
			painter.drawText(top, label.text, angle, 0.5f*fontSize*std::sin(angle*M_PI_180f), -0.5f*fontSize*std::sin(angle*M_PI_180f), true);
		else
			painter.drawText(top, label.text, 0, -textWidth/2, 2, true);
		painter.drawGreatCircleArc(foot, top, nullptr);
	}
	painter.setLineSmooth(false);
	painter.setBlending(false);
}

bool LightDomes::isActive() const
{
	if (!flagEnabled || !result)
		return false;
	const StelCore* core = StelApp::getInstance().getCore();
	if (!core->getSkyDrawer()->getFlagPhotographicModeActive() || core->getCurrentLocation().planetName != QStringLiteral("Earth"))
		return false;
	static LandscapeMgr* lmgr = GETSTELMODULE(LandscapeMgr);
	return lmgr && lmgr->getFlagAtmosphere();
}

double LightDomes::getZenithLuminance() const
{
	return isActive() ? result->zenith : 0.;
}

Vec3f LightDomes::getIlluminance() const
{
	return isActive() ? result->illuminance : Vec3f(0.f);
}

bool LightDomes::bindMap(int unit)
{
	if (!isActive())
		return false;
	QOpenGLContext* ctx = QOpenGLContext::currentContext();
	if (!ctx)
		return false;
	QOpenGLFunctions& gl = *ctx->functions();
	gl.glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(unit));
	if (!texture)
	{
		gl.glGenTextures(1, &texture);
		textureStale = true;
	}
	gl.glBindTexture(GL_TEXTURE_2D, texture);
	if (textureStale)
	{
		gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB32F, MAP_AZIMUTHS, MAP_ALTITUDES, 0, GL_RGB, GL_FLOAT, result->map.data());
		gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
		gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		textureStale = false;
	}
	gl.glActiveTexture(GL_TEXTURE0);
	return true;
}
