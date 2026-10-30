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

#include "StelRefractionModel.hpp"

#include <algorithm>
#include <cmath>

namespace
{
	constexpr double PI = 3.14159265358979323846;
	constexpr double G0 = 9.80665;          // m/s^2
	constexpr double GMR = G0*0.0289644/8.3144598;  // g0*M/R* (K/m), the hydrostatic constant of the 1976 standard
	constexpr double KELVIN = 273.15;
	// The free troposphere above the boundary layer: a rest of aerosols, thinning out
	constexpr double FREE_TROPOSPHERE_FRACTION = 0.07, FREE_TROPOSPHERE_SCALE_HEIGHT = 2000.; // -, m
	constexpr double OZONE_PEAK = 25000., OZONE_HALF_WIDTH = 15000.; // m: a tent from 10 to 40 km
	constexpr int SAMPLES = 256;

	// U.S. Standard Atmosphere 1976: base geopotential altitude (m'), base temperature (K), lapse rate (K/m'), base pressure (Pa)
	struct Layer {double h, T, L, P;};
	constexpr Layer LAYERS[] = {
		{    0., 288.15, -0.0065, 101325.     },
		{11000., 216.65,  0.0,     22632.06   },
		{20000., 216.65,  0.001,    5474.889  },
		{32000., 228.65,  0.0028,    868.0187 },
		{47000., 270.65,  0.0,       110.9063 },
		{51000., 270.65, -0.0028,     66.93887},
		{71000., 214.65, -0.002,       3.956420},
		{84852., 186.946, 0.0,         0.3734 },
	};
	constexpr int LAYER_COUNT = sizeof(LAYERS)/sizeof(LAYERS[0]);

	const Layer& layerOf(double hGeopotential)
	{
		int i = LAYER_COUNT-1;
		while (i > 0 && hGeopotential < LAYERS[i].h)
			--i;
		return LAYERS[i];
	}

	double geopotential(double h)
	{
		return StelRefractionModel::EarthRadius*h/(StelRefractionModel::EarthRadius + h);
	}

	// Vertical column of the air of the standard atmosphere above sea level, in m at the density at sea level
	double standardRayleighColumn()
	{
		static const double column = []() {
			const double rho0 = StelRefractionModel::standardPressure(0.)/(StelRefractionModel::standardTemperature(0.)+KELVIN);
			double sum = 0.;
			const double step = 10.;
			for (double h = 0.5*step; h < StelRefractionModel::AtmosphereTop; h += step)
				sum += StelRefractionModel::standardPressure(h)/(StelRefractionModel::standardTemperature(h)+KELVIN)/rho0*step;
			return sum;
		}();
		return column;
	}
}

double StelRefractionModel::standardTemperature(double altitude)
{
	const double h = geopotential(altitude);
	const Layer& l = layerOf(h);
	return l.T + l.L*(h - l.h) - KELVIN;
}

double StelRefractionModel::standardPressure(double altitude)
{
	const double h = geopotential(altitude);
	const Layer& l = layerOf(h);
	double P;
	if (l.L != 0.)
		P = l.P*std::pow(l.T/(l.T + l.L*(h - l.h)), GMR/l.L);
	else
		P = l.P*std::exp(-GMR*(h - l.h)/l.T);
	return P/100.;
}

double StelRefractionModel::refractivityCoefficient(double wavelength)
{
	// Edlén (1966), dry air at 15 degrees C and 1013.25 hPa, scaled with the density (P/T)
	const double s2 = 1./(wavelength*wavelength);
	const double ns = 1e-8*(8342.13 + 2406030./(130. - s2) + 15997./(38.9 - s2));
	return ns*(15. + KELVIN)/1013.25;
}

StelRefractionModel::StelRefractionModel(const Conditions& c)
	: conditions(c)
{
	conditions.altitude = std::clamp(conditions.altitude, -500., 80000.);
	conditions.pressure = std::max(conditions.pressure, 0.);
	conditions.temperature = std::clamp(conditions.temperature, -120., 80.);
	conditions.wavelength = std::clamp(conditions.wavelength, 0.3, 2.);
	conditions.aerosolLayerTop = std::clamp(conditions.aerosolLayerTop, 50., 10000.);
	conditions.aerosolLayerGrowth = std::clamp(conditions.aerosolLayerGrowth, 0.1, 20.);
	// the aerosol column above sea level at the normal humidity (the unit of the columns)
	{
		double sum = 0.;
		const double step = 5.;
		for (double h = 0.5*step; h < AtmosphereTop; h += step)
			sum += aerosolShape(h)*step;
		aerosolNorm = sum;
	}
	buildAtmosphere();
	traceRays();
}

void StelRefractionModel::buildAtmosphere()
{
	// The ground is the sea-level surface, or the observer if lower (a depression like the Dead Sea)
	gridBase = std::min(0., conditions.altitude);
	const int n = static_cast<int>(std::ceil((AtmosphereTop - gridBase)/gridStep)) + 1;
	gridTemperature.resize(static_cast<size_t>(n));
	gridPressure.resize(static_cast<size_t>(n));
	gridLnRefractivity.resize(static_cast<size_t>(n));
	gridDensity.resize(static_cast<size_t>(n));

	// The temperature of the observer: the standard profile shifted in the troposphere (up to the observer
	// at least), the shift fading out over the 9 km above
	const double hObs = conditions.altitude;
	const double shift = conditions.temperature - standardTemperature(hObs);
	const double hFull = std::max(11000., hObs);
	for (int i = 0; i < n; ++i)
	{
		const double h = gridBase + i*gridStep;
		const double w = h <= hFull ? 1. : std::max(0., 1. - (h - hFull)/9000.);
		gridTemperature[static_cast<size_t>(i)] = std::max(standardTemperature(h) + shift*w + KELVIN, 120.);
	}

	// Hydrostatic equilibrium, dlnP/dh = -g(h) M/(R T), g falling off with the square of the distance from
	// the center of the Earth; first relative to the ground, then scaled to the pressure at the observer
	std::vector<double> lnP(static_cast<size_t>(n));
	lnP[0] = 0.;
	for (int i = 1; i < n; ++i)
	{
		const double hm = gridBase + (i - 0.5)*gridStep;
		const double g = (EarthRadius/(EarthRadius + hm))*(EarthRadius/(EarthRadius + hm));
		const double invT = 0.5*(1./gridTemperature[static_cast<size_t>(i-1)] + 1./gridTemperature[static_cast<size_t>(i)]);
		lnP[static_cast<size_t>(i)] = lnP[static_cast<size_t>(i-1)] - GMR*g*invT*gridStep;
	}
	const double f = (hObs - gridBase)/gridStep;
	const int i0 = std::min(static_cast<int>(f), n-2);
	const double lnPObs = lnP[static_cast<size_t>(i0)] + (f - i0)*(lnP[static_cast<size_t>(i0+1)] - lnP[static_cast<size_t>(i0)]);
	const double offset = std::log(std::max(conditions.pressure, 1e-6)) - lnPObs;

	const double c = refractivityCoefficient(conditions.wavelength);
	const double rho0 = standardPressure(0.)/(standardTemperature(0.) + KELVIN);
	for (int i = 0; i < n; ++i)
	{
		const size_t k = static_cast<size_t>(i);
		gridPressure[k] = std::exp(lnP[k] + offset);
		const double T = gridTemperature[k];
		gridLnRefractivity[k] = std::log(std::max(c*gridPressure[k]/T, 1e-30));
		gridDensity[k] = gridPressure[k]/T/rho0;
	}
	refractivityObserver = c*conditions.pressure/(conditions.temperature + KELVIN);
}

double StelRefractionModel::pressureAt(double altitude) const
{
	const double f = std::clamp((altitude - gridBase)/gridStep, 0., static_cast<double>(gridPressure.size() - 1));
	const size_t i = std::min(static_cast<size_t>(f), gridPressure.size() - 2);
	const double t = f - static_cast<double>(i);
	return std::exp((1.-t)*std::log(gridPressure[i]) + t*std::log(gridPressure[i+1]));
}

double StelRefractionModel::temperatureAt(double altitude) const
{
	const double f = std::clamp((altitude - gridBase)/gridStep, 0., static_cast<double>(gridTemperature.size() - 1));
	const size_t i = std::min(static_cast<size_t>(f), gridTemperature.size() - 2);
	const double t = f - static_cast<double>(i);
	return (1.-t)*gridTemperature[i] + t*gridTemperature[i+1] - KELVIN;
}

void StelRefractionModel::refractivity(double h, double& nm1, double& dnm1) const
{
	const double f = (h - gridBase)/gridStep;
	if (f >= static_cast<double>(gridLnRefractivity.size() - 1))
	{
		nm1 = dnm1 = 0.;
		return;
	}
	const size_t i = f <= 0. ? 0 : static_cast<size_t>(f);
	const double t = f - static_cast<double>(i);
	const double a = gridLnRefractivity[i], b = gridLnRefractivity[i+1];
	nm1 = std::exp(a + t*(b - a));
	dnm1 = nm1*(b - a)/gridStep;
}

double StelRefractionModel::boundaryLayerFraction(double h) const
{
	const double top = conditions.aerosolLayerTop;
	const double width = std::max(50., 0.1*top);
	const double x = (h - top)/width;
	return x > 40. ? 0. : 1./(1. + std::exp(x));
}

double StelRefractionModel::aerosolShape(double h) const
{
	const double s = boundaryLayerFraction(h);
	const double above = FREE_TROPOSPHERE_FRACTION*std::exp(-std::max(h - conditions.aerosolLayerTop, 0.)/FREE_TROPOSPHERE_SCALE_HEIGHT);
	return s + (1. - s)*above;
}

double StelRefractionModel::aerosolDensityNormal(double h) const
{
	return aerosolShape(h)/aerosolNorm;
}

double StelRefractionModel::aerosolDensity(double h) const
{
	// the swelling of the humidity in the boundary layer; the free troposphere above stays dry
	const double s = boundaryLayerFraction(h);
	return aerosolShape(h)*(1. + (conditions.aerosolLayerGrowth - 1.)*s)/aerosolNorm;
}

StelRefractionModel::Columns StelRefractionModel::density(double h) const
{
	Columns d;
	const double f = std::clamp((h - gridBase)/gridStep, 0., static_cast<double>(gridDensity.size() - 1));
	const size_t i = std::min(static_cast<size_t>(f), gridDensity.size() - 2);
	const double t = f - static_cast<double>(i);
	d.rayleigh = ((1.-t)*gridDensity[i] + t*gridDensity[i+1])/standardRayleighColumn();
	d.aerosol = aerosolDensity(h);
	d.ozone = std::max(0., 1. - std::fabs(h - OZONE_PEAK)/OZONE_HALF_WIDTH)/OZONE_HALF_WIDTH;
	return d;
}

void StelRefractionModel::trace(double apparentAltitude, double& geometricAltitude, Columns& cols, bool& hitGround) const
{
	// The ray from the observer outwards, in the plane through the center of the Earth: distance r from the
	// center, angle phi turned around the center, zenith angle z of the ray (against the local vertical).
	// dz/ds = -sin z (1/r + (dn/dr)/n) (the curvature of a ray in a stratified medium, Bouguer's invariant
	// n r sin z = const), integrated with the midpoint rule. The steps are short near the ground and in
	// the vertical, long in the thin air.
	const double rGround = EarthRadius + gridBase;
	const double rTop = EarthRadius + AtmosphereTop;
	double r = EarthRadius + conditions.altitude;
	double phi = 0.;
	double z = 0.5*PI - apparentAltitude;
	cols = Columns();
	hitGround = false;
	for (int it = 0; it < 1000000 && r < rTop; ++it)
	{
		const double h = r - EarthRadius;
		const double above = h - gridBase;
		const double dhMax = 4. + above/40.;
		const double dsMax = std::min(2000., 300. + above/8.);
		const double ds = std::min(dsMax, dhMax/std::max(std::fabs(std::cos(z)), 1e-9));

		double nm1, dnm1;
		refractivity(h, nm1, dnm1);
		const double k1 = -std::sin(z)*(1./r + dnm1/(1. + nm1));
		const double rm = r + 0.5*ds*std::cos(z);
		const double zm = z + 0.5*ds*k1;
		const double hm = rm - EarthRadius;
		refractivity(hm, nm1, dnm1);
		const double k2 = -std::sin(zm)*(1./rm + dnm1/(1. + nm1));

		r += ds*std::cos(zm);
		phi += ds*std::sin(zm)/rm;
		z += ds*k2;

		const Columns d = density(hm);
		cols.rayleigh += d.rayleigh*ds;
		cols.aerosol += d.aerosol*ds;
		cols.ozone += d.ozone*ds;

		if (r < rGround)
		{
			hitGround = true;
			break;
		}
	}
	// The direction of the ray beyond the atmosphere, against the vertical of the observer
	geometricAltitude = 0.5*PI - (z + phi);
}

void StelRefractionModel::traceRays()
{
	// The true horizon, from Bouguer's invariant: the grazing ray has n r sin z = n(ground) r(ground) at the ground
	const double rObs = EarthRadius + conditions.altitude;
	const double rGround = EarthRadius + gridBase;
	if (conditions.altitude - gridBase < 0.01)
		horizonApparent = 0.;
	else
	{
		double nm1Ground, nm1Obs, d;
		refractivity(gridBase, nm1Ground, d);
		refractivity(conditions.altitude, nm1Obs, d);
		const double sinZ = std::min(1., (1. + nm1Ground)*rGround/((1. + nm1Obs)*rObs));
		horizonApparent = std::asin(sinZ) - 0.5*PI;
	}

	apparent.resize(SAMPLES);
	geometric.resize(SAMPLES);
	lineColumns.resize(SAMPLES);
	const double span = 0.5*PI - horizonApparent;
	double nudge = 1e-7;
	for (int k = 0; k < SAMPLES; ++k)
	{
		const double u = static_cast<double>(k)/(SAMPLES - 1);
		double a = horizonApparent + span*std::pow(u, 2.5);
		if (k == 0)
			a += nudge;
		double g;
		Columns cols;
		bool hit;
		trace(a, g, cols, hit);
		// A ray just above the horizon may still touch the ground in the numerical integration: lift it a little
		while (hit && k < 4 && nudge < 1e-4)
		{
			nudge *= 3.;
			a = horizonApparent + std::max(span*std::pow(u, 2.5), nudge);
			trace(a, g, cols, hit);
		}
		if (k == SAMPLES - 1)
		{
			a = 0.5*PI;
			g = 0.5*PI;
		}
		apparent[static_cast<size_t>(k)] = a;
		geometric[static_cast<size_t>(k)] = g;
		lineColumns[static_cast<size_t>(k)] = cols;
	}
	// The samples must increase in both altitudes (the inverse lookups rely on it)
	for (size_t k = 1; k < geometric.size(); ++k)
		geometric[k] = std::max(geometric[k], geometric[k-1] + 1e-12);
	horizonGeometric = geometric.front();
	zenith = lineColumns.back();
	// the aerosols above the observer at the normal humidity
	zenithNormal = zenith;
	zenithNormal.aerosol = 0.;
	{
		const double step = 5.;
		for (double h = conditions.altitude + 0.5*step; h < AtmosphereTop; h += step)
			zenithNormal.aerosol += aerosolDensityNormal(h)*step;
	}
}

double StelRefractionModel::getGeometricDip() const
{
	const double rObs = EarthRadius + conditions.altitude;
	const double rGround = EarthRadius + gridBase;
	return rObs > rGround ? -std::acos(rGround/rObs) : 0.;
}

double StelRefractionModel::toGeometric(double a) const
{
	if (a <= apparent.front())
		return a - (apparent.front() - geometric.front());
	if (a >= apparent.back())
		return a;
	const size_t i = static_cast<size_t>(std::upper_bound(apparent.begin(), apparent.end(), a) - apparent.begin()) - 1;
	const double t = (a - apparent[i])/(apparent[i+1] - apparent[i]);
	return geometric[i] + t*(geometric[i+1] - geometric[i]);
}

double StelRefractionModel::toApparent(double g) const
{
	if (g <= geometric.front())
		return g + (apparent.front() - geometric.front());
	if (g >= geometric.back())
		return g;
	const size_t i = static_cast<size_t>(std::upper_bound(geometric.begin(), geometric.end(), g) - geometric.begin()) - 1;
	const double t = (g - geometric[i])/(geometric[i+1] - geometric[i]);
	return apparent[i] + t*(apparent[i+1] - apparent[i]);
}

StelRefractionModel::Columns StelRefractionModel::columns(double a) const
{
	if (a <= apparent.front())
		return lineColumns.front();
	if (a >= apparent.back())
		return lineColumns.back();
	const size_t i = static_cast<size_t>(std::upper_bound(apparent.begin(), apparent.end(), a) - apparent.begin()) - 1;
	const double t = (a - apparent[i])/(apparent[i+1] - apparent[i]);
	const Columns& c0 = lineColumns[i];
	const Columns& c1 = lineColumns[i+1];
	Columns c;
	c.rayleigh = c0.rayleigh + t*(c1.rayleigh - c0.rayleigh);
	c.aerosol = c0.aerosol + t*(c1.aerosol - c0.aerosol);
	c.ozone = c0.ozone + t*(c1.ozone - c0.ozone);
	return c;
}
