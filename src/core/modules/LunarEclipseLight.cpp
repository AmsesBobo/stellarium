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

#include "LunarEclipseLight.hpp"

#include <QtGlobal>
#include <algorithm>
#include <cmath>

namespace
{
constexpr double ECL_EARTH_RADIUS_KM = 6371.;   // km
constexpr double ECL_SCALE_HEIGHT = 8.;      // km, density of the air (and its refractivity)
constexpr double ECL_TOP = 40.;              // km: above, the atmosphere neither bends nor dims the light noticeably
constexpr int ECL_HEIGHTS = 400;             // tangent heights from 0 to ECL_TOP
constexpr int ECL_WAVELENGTHS = 12;          // from 400 to 708 nm
constexpr int ECL_AZIMUTHS = 90;             // around the half of the limb (the other half by symmetry)

// Densities of the species, normalized to a vertical column of 1 (z in km)
double airDensity(double z) { return std::exp(-z/ECL_SCALE_HEIGHT)/ECL_SCALE_HEIGHT; }
double gaussianLayer(double z, double center, double width) { return std::exp(-0.5*(z-center)*(z-center)/(width*width))/(width*std::sqrt(2.*M_PI)); }
double ozoneDensity(double z) { return gaussianLayer(z, 22., 6.); }
double tropoAerosolDensity(double z) { return std::exp(-z/1.2)/1.2; }
double stratoAerosolDensity(double z) { return gaussianLayer(z, 20., 4.); }

// The column along a straight grazing path at the tangent height h, relative to the vertical column
template <typename F> double tangentColumn(double h, F density)
{
	constexpr double step = 1.; // km
	double sum = 0.;
	for (double s = -1200.; s <= 1200.; s += step)
	{
		const double z = std::sqrt((ECL_EARTH_RADIUS_KM+h)*(ECL_EARTH_RADIUS_KM+h) + s*s) - ECL_EARTH_RADIUS_KM;
		sum += density(z)*step;
	}
	return sum;
}

double interpolate(double x, const std::vector<double>& xs, const std::vector<double>& ys)
{
	if (x <= xs.front()) return ys.front();
	if (x >= xs.back()) return ys.back();
	const auto it = std::upper_bound(xs.begin(), xs.end(), x);
	const size_t i = static_cast<size_t>(it - xs.begin());
	const double t = (x - xs[i-1])/(xs[i] - xs[i-1]);
	return ys[i-1] + t*(ys[i] - ys[i-1]);
}

// Piecewise Gaussian fit of the CIE 1931 color matching functions (Wyman, Sloan and Shirley 2013)
double lobe(double l, double mu, double s1, double s2) { const double s = l < mu ? s1 : s2; return std::exp(-0.5*(l-mu)*(l-mu)/(s*s)); }
double cieX(double l) { return 1.056*lobe(l, 599.8, 37.9, 31.0) + 0.362*lobe(l, 442.0, 16.0, 26.7) - 0.065*lobe(l, 501.1, 20.4, 26.2); }
double cieY(double l) { return 0.821*lobe(l, 568.8, 46.9, 40.5) + 0.286*lobe(l, 530.9, 16.3, 31.1); }
double cieZ(double l) { return 1.217*lobe(l, 437.0, 11.8, 36.0) + 0.681*lobe(l, 459.0, 26.0, 13.8); }

}

double LunarEclipseLight::sunVisible(double sigma, double r, double d, double u, int rings)
{
	// Rings of the solar disk with radii from x0 sigma to sigma: those inside x0 = (r-d)/sigma are hidden entirely, so
	// all the rings fall on the visible part, also close to the edge of the umbra where only the limb shows.
	const double x0 = std::max(0., (r - d)/sigma);
	if (x0 >= 1.)
		return 0.;
	double visible = 0.;
	for (int i=0; i<rings; ++i)
	{
		const double x = x0 + (1. - x0)*(i + 0.5)/rings;
		const double rho = x*sigma;
		const double weight = x*(1. - u*(1. - std::sqrt(std::max(0., 1. - x*x))));
		double covered;
		if (d <= r - rho) covered = 1.;
		else if (d >= r + rho || rho >= d + r) covered = 0.;
		else covered = std::acos(std::clamp((rho*rho + d*d - r*r)/(2.*rho*d), -1., 1.))/M_PI;
		visible += weight*(1. - covered);
	}
	// relative to the whole disk: the integral of x (1 - u (1 - sqrt(1-x^2))) from 0 to 1 is 1/2 - u/6
	return visible*(1. - x0)/rings/(0.5 - u/6.);
}

double LunarEclipseLight::coordinate(double d, double e, double sigma)
{
	if (!(e > sigma) || !(sigma > 0.))
		return std::clamp(d/(e + sigma), 0., 1.);
	if (d <= e - sigma)
		return 0.5*d/(e - sigma);
	return std::min(1., 0.5 + 0.25*(d - e + sigma)/sigma);
}

double LunarEclipseLight::distance(double t, double e, double sigma)
{
	if (!(e > sigma) || !(sigma > 0.))
		return t*(e + sigma);
	if (t <= 0.5)
		return 2.*t*(e - sigma);
	return e - sigma + 4.*(t - 0.5)*sigma;
}

LunarEclipseLight& LunarEclipseLight::instance()
{
	static LunarEclipseLight model;
	return model;
}

LunarEclipseLight::LunarEclipseLight()
{
	values.assign(TABLE_SIZE, Vec4f(0.f));
	// Tangent heights: the grazing paths, and the columns of the species along them
	for (int i=0; i<ECL_HEIGHTS; ++i)
	{
		const double h = ECL_TOP*(i+0.5)/ECL_HEIGHTS;
		heights.push_back(h);
		columnAir.push_back(tangentColumn(h, airDensity));
		columnOzone.push_back(tangentColumn(h, ozoneDensity));
		columnTropoAerosol.push_back(tangentColumn(h, tropoAerosolDensity));
		columnStratoAerosol.push_back(tangentColumn(h, stratoAerosolDensity));
	}
	// Wavelengths, with what depends on them only
	const std::vector<double> ldWavelengths = {400., 450., 500., 550., 600., 650., 700.};
	const std::vector<double> ldCoefficients = {0.80, 0.75, 0.68, 0.62, 0.56, 0.52, 0.48}; // solar limb darkening
	for (int k=0; k<ECL_WAVELENGTHS; ++k)
	{
		const double l = 400. + 28.*k;
		const double um = 0.001*l;
		wavelengths.push_back(l);
		refractivity.push_back(2.87e-4*(1. + 5.67e-3/(um*um)));
		limbDarkening.push_back(interpolate(l, ldWavelengths, ldCoefficients));
		// Sunlight (Planck at 5778 K) weighted with the color matching functions
		const double planck = 1./(std::pow(l, 5.)*(std::exp(1.4388e7/(l*5778.)) - 1.));
		weightX.push_back(planck*cieX(l));
		weightY.push_back(planck*cieY(l));
		weightZ.push_back(planck*cieZ(l));
	}
}

void LunarEclipseLight::computeTransmission(int atmosphere)
{
	// The states of the atmosphere: stratospheric aerosol (vertical optical depth at 550 nm) and the share of the
	// grazing paths below the cloud tops that the clouds stop
	static const double stratoAerosol[ATMOSPHERE_COUNT] = {0.002, 0.01, 0.05, 0.15};
	static const double cloudShare[ATMOSPHERE_COUNT] = {0.4, 0.6, 0.6, 0.6};
	const int a = qBound(0, atmosphere, ATMOSPHERE_COUNT-1);
	// Ozone: Chappuis band (cm^2 per molecule), 300 Dobson units
	const std::vector<double> o3Wavelengths = {390., 420., 450., 480., 500., 520., 550., 575., 600., 625., 650., 675., 700., 710.};
	const std::vector<double> o3Section = {0.0, 0.03, 0.3, 0.8, 1.4, 2.0, 3.3, 4.6, 5.1, 4.2, 2.4, 1.6, 1.0, 0.8};
	transmission.assign(static_cast<size_t>(ECL_HEIGHTS*ECL_WAVELENGTHS), 0.);
	for (int k=0; k<ECL_WAVELENGTHS; ++k)
	{
		const double um = 0.001*wavelengths[static_cast<size_t>(k)];
		const double tauRayleigh = 0.0088*std::pow(um, -4.05);
		const double tauOzone = 300.*2.687e16*interpolate(wavelengths[static_cast<size_t>(k)], o3Wavelengths, o3Section)*1e-21;
		const double tauTropo = 0.10*std::pow(um/0.55, -1.3);
		const double tauStrato = stratoAerosol[a]*std::pow(um/0.55, -1.0);
		for (int i=0; i<ECL_HEIGHTS; ++i)
		{
			const size_t n = static_cast<size_t>(i);
			const double tau = columnAir[n]*tauRayleigh + columnOzone[n]*tauOzone + columnTropoAerosol[n]*tauTropo + columnStratoAerosol[n]*tauStrato;
			// clouds below about 11 km (the tops spread from 10 to 12 km)
			const double cloud = cloudShare[a]*std::clamp((12. - heights[n])/2., 0., 1.);
			transmission[n*ECL_WAVELENGTHS + static_cast<size_t>(k)] = std::exp(-tau)*(1. - cloud);
		}
	}
}

void LunarEclipseLight::update(double e, double sigma, int atmosphere)
{
	if (!(e > 0.) || !(sigma > 0.))
		return;
	const bool atmosphereChanged = atmosphere != currentAtmosphere;
	const bool geometryChanged = std::fabs(e/currentE - 1.) > 1e-3 || std::fabs(sigma/currentSigma - 1.) > 1e-3 || !(currentE > 0.);
	if (!atmosphereChanged && !geometryChanged)
		return;
	if (atmosphereChanged)
		computeTransmission(atmosphere);
	currentAtmosphere = atmosphere;
	currentE = e;
	currentSigma = sigma;
	computeTable();
}

void LunarEclipseLight::computeTable()
{
	const double e = currentE, sigma = currentSigma;
	// The ring of the limb: apparent radius a(h), its width, and per wavelength the direction it comes from
	std::vector<double> a(ECL_HEIGHTS), da(ECL_HEIGHTS);
	for (int i=0; i<ECL_HEIGHTS; ++i)
	{
		a[static_cast<size_t>(i)] = e*(1. + heights[static_cast<size_t>(i)]/ECL_EARTH_RADIUS_KM);
		da[static_cast<size_t>(i)] = e*(ECL_TOP/ECL_HEIGHTS)/ECL_EARTH_RADIUS_KM;
	}
	std::vector<std::vector<double>> source(ECL_WAVELENGTHS, std::vector<double>(ECL_HEIGHTS));
	for (int k=0; k<ECL_WAVELENGTHS; ++k)
		for (int i=0; i<ECL_HEIGHTS; ++i)
		{
			const double h = heights[static_cast<size_t>(i)];
			const double bend = refractivity[static_cast<size_t>(k)]*std::exp(-h/ECL_SCALE_HEIGHT)*std::sqrt(2.*M_PI*(ECL_EARTH_RADIUS_KM + h)/ECL_SCALE_HEIGHT);
			source[static_cast<size_t>(k)][static_cast<size_t>(i)] = a[static_cast<size_t>(i)] - bend; // increases with h
		}
	double whiteX = 0., whiteY = 0., whiteZ = 0.;
	for (int k=0; k<ECL_WAVELENGTHS; ++k)
	{
		whiteX += weightX[static_cast<size_t>(k)];
		whiteY += weightY[static_cast<size_t>(k)];
		whiteZ += weightZ[static_cast<size_t>(k)];
	}
	const auto toRGB = [](double X, double Y, double Z) {
		return Vec3d(3.2406*X - 1.5372*Y - 0.4986*Z, -0.9689*X + 1.8758*Y + 0.0415*Z, 0.0557*X - 0.2040*Y + 1.0570*Z);
	};
	const Vec3d white = toRGB(whiteX, whiteY, whiteZ);
	const double dphi = M_PI/ECL_AZIMUTHS;
	const double eTop = e*(1. + ECL_TOP/ECL_EARTH_RADIUS_KM);

	for (int j=0; j<TABLE_SIZE; ++j)
	{
		const double d = distance(j/(TABLE_SIZE - 1.), e, sigma);
		double X = 0., Y = 0., Z = 0.;
		for (int k=0; k<ECL_WAVELENGTHS; ++k)
		{
			const size_t kk = static_cast<size_t>(k);
			const double u = limbDarkening[kk];
			const std::vector<double>& t = source[kk];
			double ring = 0.;
			for (int m=0; m<ECL_AZIMUTHS; ++m)
			{
				const double phi = (m + 0.5)*dphi;
				// the source direction t (along phi from the Earth's center) is on the solar disk (center at -d)
				// for t in [t1, t2]: t^2 + 2 t d cos(phi) + d^2 < sigma^2
				const double c = std::cos(phi), s = std::sin(phi);
				const double disc = sigma*sigma - d*d*s*s;
				if (disc <= 0.)
					continue;
				const double t1 = -d*c - std::sqrt(disc), t2 = -d*c + std::sqrt(disc);
				const auto begin = std::lower_bound(t.begin(), t.end(), t1);
				const auto end = std::upper_bound(t.begin(), t.end(), t2);
				for (auto it = begin; it < end; ++it)
				{
					const size_t i = static_cast<size_t>(it - t.begin());
					const double x = t[i]*c + d, y = t[i]*s;
					const double rho2 = (x*x + y*y)/(sigma*sigma);
					if (rho2 >= 1.)
						continue;
					const double radiance = 1. - u*(1. - std::sqrt(1. - rho2));
					ring += radiance*transmission[i*ECL_WAVELENGTHS + kk]*a[i]*da[i];
				}
			}
			ring *= 2.*dphi;                                   // both halves of the limb
			ring /= M_PI*sigma*sigma*(1. - u/3.);              // relative to the whole Sun
			// minus what passes the same ring without an atmosphere: the direct light between e and eTop
			const double geometric = sunVisible(sigma, e, d, u, 48) - sunVisible(sigma, eTop, d, u, 48);
			const double light = ring - geometric;
			X += light*weightX[kk];
			Y += light*weightY[kk];
			Z += light*weightZ[kk];
		}
		const Vec3d rgb = toRGB(X, Y, Z);
		values[static_cast<size_t>(j)] = Vec4f(static_cast<float>(rgb[0]/white[0]), static_cast<float>(rgb[1]/white[1]),
		                                      static_cast<float>(rgb[2]/white[2]), static_cast<float>(Y/whiteY));
	}
	++tableVersion;
}

Vec3f LunarEclipseLight::light(double x) const
{
	const double f = std::clamp(x, 0., 1.)*(TABLE_SIZE - 1);
	const int i = std::min(static_cast<int>(f), TABLE_SIZE - 2);
	const float t = static_cast<float>(f - i);
	const Vec4f& v0 = values[static_cast<size_t>(i)];
	const Vec4f& v1 = values[static_cast<size_t>(i+1)];
	return Vec3f(v0[0] + t*(v1[0]-v0[0]), v0[1] + t*(v1[1]-v0[1]), v0[2] + t*(v1[2]-v0[2]));
}

double LunarEclipseLight::luminance(double x) const
{
	const double f = std::clamp(x, 0., 1.)*(TABLE_SIZE - 1);
	const int i = std::min(static_cast<int>(f), TABLE_SIZE - 2);
	const double t = f - i;
	return static_cast<double>(values[static_cast<size_t>(i)][3])*(1.-t) + static_cast<double>(values[static_cast<size_t>(i+1)][3])*t;
}
