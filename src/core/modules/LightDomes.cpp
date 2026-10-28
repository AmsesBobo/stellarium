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
#include "StelModuleMgr.hpp"
#include "LandscapeMgr.hpp"
#include "StelFileMgr.hpp"

#include <QSettings>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringList>
#include <QtConcurrent>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <cmath>
#include <array>
#include <numeric>

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
// 8 km; aerosols with an Angstrom exponent 1.3 over 1.2 km, single scattering albedo 0.9, Henyey-Greenstein g = 0.7
constexpr double WAVELENGTHS[3] = {0.610, 0.550, 0.465};
constexpr double RAYLEIGH_SCALE_HEIGHT = 8., AEROSOL_SCALE_HEIGHT = 1.2;
constexpr double AEROSOL_ANGSTROM = 1.3, AEROSOL_ALBEDO = 0.9, AEROSOL_G = 0.7;
// Garstang's emission function: G of the light that falls on the ground is reflected, F goes directly upwards
constexpr double GARSTANG_G = 0.15, GARSTANG_F = 0.15;

double rowAltitude(int row)
{
	const double v = static_cast<double>(row)/(LightDomes::MAP_ALTITUDES - 1);
	return 0.5*M_PI*v*v;
}

double kernelDistance(int i)
{
	return KERNEL_MIN_DISTANCE*std::pow(KERNEL_MAX_DISTANCE/KERNEL_MIN_DISTANCE, static_cast<double>(i)/(KERNEL_DISTANCES - 1));
}

// The optical depths of the two kinds of scatterers between the heights h1 and h2 (km) over a straight path of the
// length L (km), the height changing linearly along it: per unit of their scattering coefficient at sea level
inline void columns(double h1, double h2, double L, double& rayleigh, double& aerosol)
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
	aerosol = column(AEROSOL_SCALE_HEIGHT);
}

struct Air
{
	Vec3d rayleigh;  // scattering coefficients at sea level (1/km), red, green, blue
	Vec3d aerosol;
	Vec3d depth(double rayleighColumn, double aerosolColumn) const
	{
		return rayleigh*rayleighColumn + aerosol*aerosolColumn;
	}
};

inline double height(const Vec3d& p) { return p.norm() - EARTH_RADIUS_KM; }

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
	const double g = AEROSOL_G;
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
		columns(hPrev, h, s - sPrev, cr, ca);
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
		columns(0., hMid, 0.5*r, r1, a1);
		columns(hMid, h, 0.5*r, r2, a2);
		const Vec3d fromTown = air.depth(r1 + r2, a1 + a2);
		const double c = -(SP*u)/r;  // cosine of the scattering angle (from S->P towards P->O)
		const double rayleighPhase = 3./(16.*M_PI)*(1. + c*c);
		const double q = 1. + g*g - 2.*g*c;
		const double hgPhase = (1. - g*g)/(4.*M_PI*q*std::sqrt(q));
		const double wR = std::exp(-h/RAYLEIGH_SCALE_HEIGHT)*rayleighPhase;
		const double wA = AEROSOL_ALBEDO*std::exp(-h/AEROSOL_SCALE_HEIGHT)*hgPhase;
		const double base = emission/(r*r*1e6)*ds;  // lux per scattering coefficient
		for (int k=0; k<3; ++k)
			total[k] += base*(air.rayleigh[k]*wR + air.aerosol[k]*wA)*std::exp(-fromTown[k] - toObserver[k]);
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
	const double k = static_cast<double>(core->getSkyDrawer()->getExtinction().getExtinctionCoefficient());
	const double rayleigh = 0.0088*std::pow(0.55, -4.05)*std::exp(-key.altitude/RAYLEIGH_SCALE_HEIGHT);
	const double aerosolHere = qMax(0.01, k/1.0857 - rayleigh - 0.016);
	key.aerosolDepth = std::round(qMin(3., aerosolHere*std::exp(key.altitude/AEROSOL_SCALE_HEIGHT))*200.)/200.;
	return key;
}

namespace
{
// The table only depends on the atmosphere: it is kept in the cache directory (a few seconds to compute)
QString kernelCacheFile(double aerosolDepth, double altitude)
{
	return QString("%1/lightdomes/kernel-v1-%2-%3.bin").arg(StelFileMgr::getCacheDir()).arg(qRound(aerosolDepth*1000.)).arg(qRound(altitude*1000.));
}
}

std::shared_ptr<LightDomes::Kernel> LightDomes::computeKernel(const AtmosphereKey& key)
{
	const size_t count = static_cast<size_t>(3)*KERNEL_DISTANCES*MAP_ALTITUDES*KERNEL_AZIMUTHS;
	const QString cacheFile = kernelCacheFile(key.aerosolDepth, key.altitude);
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
	Air air;
	for (int c=0; c<3; ++c)
	{
		const double lambda = WAVELENGTHS[c];
		air.rayleigh[c] = 0.0088*std::pow(lambda, -4.05)/RAYLEIGH_SCALE_HEIGHT;
		air.aerosol[c] = key.aerosolDepth*std::pow(lambda/0.55, -AEROSOL_ANGSTROM)/AEROSOL_SCALE_HEIGHT;
	}
	QtConcurrent::blockingMap(jobs, [k, key, air](int job)
	{
		const int d = job / MAP_ALTITUDES, row = job % MAP_ALTITUDES;
		const double dist = kernelDistance(d);
		const double z = 0.5*M_PI - qMax(rowAltitude(row), 0.002);  // the horizon itself: just above it
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

std::shared_ptr<LightDomes::Result> LightDomes::computeMap(std::shared_ptr<Kernel> kern, const std::vector<Source>& sources, double lumensPerInhabitant)
{
	auto res = std::make_shared<Result>();
	res->map.assign(static_cast<size_t>(MAP_ALTITUDES)*MAP_AZIMUTHS*3, 0.f);
	for (const auto& s : sources)
	{
		const double flux = s.population*lumensPerInhabitant;
		if (!(flux > 0.))
			continue;
		const Vec3f color = lampColor(s.lamp);
		// A town is not a point: a disk of about 0.5 km sqrt(P/1000) (5 km for 100000 inhabitants), as seven points
		const double radius = qMax(0.2, 0.5*std::sqrt(s.population/1000.));
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
	for (int row=0; row<MAP_ALTITUDES; ++row)
	{
		const double alt = rowAltitude(row);
		const double lo = row > 0 ? 0.5*(rowAltitude(row - 1) + alt) : 0.;
		const double hi = row < MAP_ALTITUDES - 1 ? 0.5*(alt + rowAltitude(row + 1)) : 0.5*M_PI;
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
	res->illuminance.set(static_cast<float>(E[0]), static_cast<float>(E[1]), static_cast<float>(E[2]));
	return res;
}

void LightDomes::startComputation()
{
	const AtmosphereKey key = currentAtmosphere();
	const std::shared_ptr<Kernel> known = (kernel && kernel->key == key) ? kernel : nullptr;
	const std::vector<Source> src = sources;
	const double lm = lumensPerInhabitant;
	jobAtmosphere = key;
	job = QtConcurrent::run([known, key, src, lm]()
	{
		const std::shared_ptr<Kernel> k = known ? known : computeKernel(key);
		return std::make_pair(k, computeMap(k, src, lm));
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
	if (!flagEnabled || jobRunning)
		return;
	// compute again when the towns, the lumens or the atmosphere (extinction, altitude) changed
	if (dirty || !kernel || !(currentAtmosphere() == kernel->key))
		startComputation();
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
