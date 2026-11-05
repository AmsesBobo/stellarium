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

#include "NoctilucentCloudLight.hpp"
#include "SpectralData.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>

namespace
{
constexpr double EarthRadius = 6371.;     // km
constexpr double CloudHeight = 82.;       // km
constexpr double SolarIlluminance = 133000.;  // lux, above the atmosphere
constexpr double SunRadius = 0.00465;     // radians
constexpr double ReferenceWavelength = 532.;  // nm: the optical depth of the layer is the one of the lidars
constexpr double FirstWavelength = 380.;  // nm
constexpr double WavelengthStep = 10.;    // nm
constexpr int WavelengthCount = 41;

// The particles: a log-normal distribution of the radius, by the model of Lange et al. (2022, Ann. Geophys. 40, 407)
constexpr double MedianRadius = 50.;      // nm
constexpr double DistributionWidth = 1.4;
constexpr double IceRefractiveIndex = 1.31;
constexpr int RadiusCount = 48;

// Density profiles from 0 to 100 km, every 10 km, to interpolate in the logarithm: the air against sea level (U.S.
// Standard Atmosphere 1976) and the ozone (10^12 molecules per cm^3, scaled to a column below): a peak near 22 km and
// above 38 km a scale height of 4.6 km (Krueger and Minzner 1976). What matters here is this upper tail, along which
// the sunlight reaches the clouds.
constexpr std::array<double, 11> AirProfile = {1., 0.3376, 0.0726, 0.01503, 0.00326, 0.00084, 0.00025, 6.8e-5, 1.5e-5, 2.8e-6, 4.5e-7};
constexpr std::array<double, 11> OzoneProfile = {0.54, 0.86, 4.9, 2.6, 0.5, 0.057, 0.0065, 0.00074, 8.4e-5, 9.6e-6, 1.1e-6};

// A number in [0, 1) from the integer n and a salt (splitmix64)
double unitHash(int64_t n, uint64_t salt)
{
	uint64_t z = static_cast<uint64_t>(n)*0x9E3779B97F4A7C15ull + salt*0xBF58476D1CE4E5B9ull + 0x94D049BB133111EBull;
	z = (z ^ (z >> 30))*0xBF58476D1CE4E5B9ull;
	z = (z ^ (z >> 27))*0x94D049BB133111EBull;
	z ^= z >> 31;
	return static_cast<double>(z >> 11)*(1./9007199254740992.);
}

double smooth(double e0, double e1, double x)
{
	const double t = std::clamp((x - e0)/(e1 - e0), 0., 1.);
	return t*t*(3. - 2.*t);
}

double profile(const std::array<double, 11>& values, double z)
{
	const double x = std::clamp(z/10., 0., 9.999);
	const std::size_t i = static_cast<std::size_t>(x);
	return std::exp(std::log(values[i]) + (x - static_cast<double>(i))*(std::log(values[i+1]) - std::log(values[i])));
}
double airDensity(double z) { return profile(AirProfile, z); }
double ozoneDensity(double z) { return profile(OzoneProfile, z); }
double gaussianLayer(double z, double center, double width)
{
	return std::exp(-0.5*(z-center)*(z-center)/(width*width))/(width*std::sqrt(2.*M_PI));
}
double stratosphericAerosol(double z) { return gaussianLayer(z, 20., 4.); }
double troposphericAerosol(double z) { return std::exp(-z/1.2)/1.2; }

// The column of a density along a straight grazing line at the tangent height h: a sum over the distance s from the
// tangent point, dense near it, on both sides
template <typename F> double grazingColumn(double h, F density)
{
	constexpr int Steps = 240;
	constexpr double Reach = 2400.;  // km
	double sum = 0.;
	for (int i = 0; i < Steps; ++i)
	{
		const double u = (i + 0.5)/Steps;
		const double s = Reach*u*u;
		const double r = EarthRadius + h;
		sum += density(std::sqrt(r*r + s*s) - EarthRadius)*2.*Reach*u/Steps;
	}
	return 2.*sum;
}

// The same along the vertical
template <typename F> double verticalColumn(F density)
{
	double sum = 0.;
	for (double z = 0.05; z < 100.; z += 0.1)
		sum += density(z)*0.1;
	return sum;
}

double bendingAngle(double h)
{
	// The total bending (radians) of the ray with the tangent height h (Bouguer), from the refractivity and the
	// scale height of the air there
	constexpr double SeaLevelRefractivity = 2.77e-4;  // at 550 nm, 15 degrees Celsius
	const double scale = 1./std::max(1e-3, std::log(airDensity(h)/airDensity(h + 1.)));
	return SeaLevelRefractivity*airDensity(h)*std::sqrt(2.*M_PI*(EarthRadius + h)/scale);
}

// The way of the sunlight to the layer: the transmission against the height of the straight ray, for one wavelength.
// The rays between two real tangent heights arrive between two heights of the straight rays, which is where their
// light is put; several rays can arrive at one place. Then the disk of the Sun. The heights are 0.25 km apart.
std::vector<double> transmission(double wavelength)
{
	constexpr double Step = 0.25;  // km
	constexpr int Rays = 480;      // up to 120 km
	const int cells = static_cast<int>(NoctilucentCloudLight::MaxHeight/Step) + 1;
	static const auto columns = [] {
		std::array<std::array<double, 4>, Rays + 1> c;
		const double air = verticalColumn(airDensity), ozone = verticalColumn(ozoneDensity);
		for (int i = 0; i <= Rays; ++i)
		{
			const double h = i*Step;
			c[static_cast<std::size_t>(i)] = {grazingColumn(h, airDensity)/air, grazingColumn(h, ozoneDensity)/ozone,
			                                  grazingColumn(h, troposphericAerosol), grazingColumn(h, stratosphericAerosol)};
		}
		return c;
	}();
	const double um = 0.001*wavelength;
	const double tauRayleigh = 0.0088*std::pow(um, -4.05);
	const double tauOzone = 300.*2.687e16*SpectralData::ozoneCrossSection(wavelength)*1e-21;
	const double tauTropo = 0.10*std::pow(um/0.55, -1.3);
	const double tauStrato = 0.003*std::pow(um/0.55, -1.0);

	const auto arrival = [](double h) {
		const double r = EarthRadius + CloudHeight, rt = EarthRadius + h;
		return h + std::sqrt(std::max(0., r*r - rt*rt))*bendingAngle(h);
	};
	const auto light = [&](int i) {
		const auto& c = columns[static_cast<std::size_t>(i)];
		const double h = i*Step;
		// clouds: as in the eclipses (LunarEclipseLight), their tops spread from 10 to 12 km
		const double clouds = 0.6*std::clamp((12. - h)/2., 0., 1.);
		return std::exp(-(c[0]*tauRayleigh + c[1]*tauOzone + c[2]*tauTropo + c[3]*tauStrato))*(1. - clouds);
	};

	std::vector<double> sharp(static_cast<std::size_t>(cells), 0.);
	for (int i = 0; i < Rays; ++i)
	{
		const double a = arrival(i*Step), b = arrival((i + 1)*Step);
		const double low = std::min(a, b), high = std::max(a, b);
		const double energy = 0.5*(light(i) + light(i + 1))*Step;
		const int first = std::max(0, static_cast<int>(low/Step));
		const int last = std::min(cells - 1, static_cast<int>(high/Step));
		for (int k = first; k <= last; ++k)
		{
			const double overlap = std::min(high, (k + 1)*Step) - std::max(low, k*Step);
			if (overlap > 0.)
				sharp[static_cast<std::size_t>(k)] += energy*overlap/std::max(high - low, 1e-6)/Step;
		}
	}

	// The disk of the Sun, seen from the point of the layer over the distance to the tangent point
	std::vector<double> soft(static_cast<std::size_t>(cells), 0.);
	for (int k = 0; k < cells; ++k)
	{
		const double h = k*Step;
		const double r = EarthRadius + CloudHeight, rt = EarthRadius + h;
		const double radius = std::max(Step, SunRadius*std::sqrt(std::max(0., r*r - rt*rt)));
		double sum = 0., weightSum = 0.;
		for (int j = std::max(0, k - static_cast<int>(radius/Step)); j <= std::min(cells - 1, k + static_cast<int>(radius/Step)); ++j)
		{
			const double x = (j - k)*Step/radius;
			const double weight = std::sqrt(std::max(0., 1. - x*x));
			sum += weight*sharp[static_cast<std::size_t>(j)];
			weightSum += weight;
		}
		soft[static_cast<std::size_t>(k)] = weightSum > 0. ? sum/weightSum : sharp[static_cast<std::size_t>(k)];
	}
	return soft;
}
}

NoctilucentCloudLight::Occurrence NoctilucentCloudLight::occurrence(double jd, double night, int mode, bool south)
{
	Occurrence o;
	if (mode == OccurrenceFaint)
	{
		o.any = true;
		o.edge = 60.;
		o.strength = 0.6;
		o.cover = 0.36;
		return o;
	}
	if (mode == OccurrenceGreat)
	{
		o.any = true;
		o.edge = 48.;
		o.strength = 2.5;
		o.cover = 0.2;
		return o;
	}
	if (mode != OccurrenceSeason)
		return o;

	// The days from the solstice of the summer of the hemisphere: the one of June 2000, in tropical years
	constexpr double TropicalYear = 365.2422;
	const double days = std::remainder(jd - (2451716.575 + (south ? 0.5*TropicalYear : 0.)), TropicalYear);
	const double season = smooth(-38., -12., days)*(1. - smooth(35., 80., days));
	if (season <= 0.)
		return o;

	// How far the clouds of this night reach at the height of the season: the shares of the nights given above
	const double u = unitHash(static_cast<int64_t>(night), south ? 2 : 1);
	static constexpr std::array<double, 6> shares = {0., 0.1, 0.45, 0.8, 0.95, 1.};
	static constexpr std::array<double, 6> edges = {75., 70., 65., 60., 55., 52.};
	std::size_t i = 1;
	while (i < 5 && u > shares[i])
		++i;
	const double reach = edges[i - 1] + (u - shares[i - 1])/(shares[i] - shares[i - 1])*(edges[i] - edges[i - 1]) + (south ? 3. : 0.);

	o.any = true;
	o.edge = 90. - (90. - reach)*season;
	o.strength = (0.4 + 1.4*std::pow(u, 1.5))*(0.8 + 0.4*unitHash(static_cast<int64_t>(night), 3));
	o.cover = 0.28 + 0.22*(1. - u);
	return o;
}

double NoctilucentCloudLight::mieScattering(double x, const double* mu, int nAngles, double* intensity)
{
	const double m = IceRefractiveIndex;
	const int nStop = static_cast<int>(x + 4.*std::cbrt(x) + 2.);
	const int nStart = std::max(nStop, static_cast<int>(m*x)) + 15;
	std::vector<double> d(static_cast<std::size_t>(nStart) + 1, 0.);  // logarithmic derivative, downwards
	const double y = m*x;
	for (int n = nStart; n >= 2; --n)
		d[static_cast<std::size_t>(n) - 1] = n/y - 1./(d[static_cast<std::size_t>(n)] + n/y);

	using Complex = std::complex<double>;
	std::vector<Complex> s1(static_cast<std::size_t>(nAngles)), s2(static_cast<std::size_t>(nAngles));
	std::vector<double> pi0(static_cast<std::size_t>(nAngles), 0.), pi1(static_cast<std::size_t>(nAngles), 1.);
	double psi0 = std::cos(x), psi1 = std::sin(x), chi0 = -std::sin(x), chi1 = std::cos(x);
	double sum = 0.;
	for (int n = 1; n <= nStop; ++n)
	{
		const double psi = (2.*n - 1.)*psi1/x - psi0;
		const double chi = (2.*n - 1.)*chi1/x - chi0;
		const Complex xi(psi, -chi), xi1(psi1, -chi1);
		const double dn = d[static_cast<std::size_t>(n)];
		const double ta = dn/m + n/x, tb = dn*m + n/x;
		const Complex a = (ta*psi - psi1)/(ta*xi - xi1);
		const Complex b = (tb*psi - psi1)/(tb*xi - xi1);
		sum += (2.*n + 1.)*(std::norm(a) + std::norm(b));
		const double f = (2.*n + 1.)/(n*(n + 1.));
		for (std::size_t j = 0; j < static_cast<std::size_t>(nAngles); ++j)
		{
			const double tau = n*mu[j]*pi1[j] - (n + 1.)*pi0[j];
			s1[j] += f*(a*pi1[j] + b*tau);
			s2[j] += f*(a*tau + b*pi1[j]);
			const double next = ((2.*n + 1.)*mu[j]*pi1[j] - (n + 1.)*pi0[j])/n;
			pi0[j] = pi1[j];
			pi1[j] = next;
		}
		psi0 = psi1;
		psi1 = psi;
		chi0 = chi1;
		chi1 = chi;
	}
	for (std::size_t j = 0; j < static_cast<std::size_t>(nAngles); ++j)
		intensity[j] = std::norm(s1[j]) + std::norm(s2[j]);
	return 2.*sum/(x*x);
}

std::vector<float> NoctilucentCloudLight::computeTable()
{
	std::array<double, AngleCount> mu;
	for (int j = 0; j < AngleCount; ++j)
		mu[static_cast<std::size_t>(j)] = std::cos(M_PI*j/(AngleCount - 1));

	// For one wavelength: the scattering cross section of the distribution (nm^2) and its phase function, from
	// the radii around the median; the largest ones weigh most, as the cross section grows with the sixth power
	const double logWidth = std::log(DistributionWidth);
	const auto scattering = [&](double wavelength, std::array<double, AngleCount>& phase) {
		std::array<double, AngleCount> intensity;
		phase.fill(0.);
		double section = 0.;
		for (int i = 0; i < RadiusCount; ++i)
		{
			const double z = -3. + 12.*(i + 0.5)/RadiusCount;  // in widths from the median
			const double radius = MedianRadius*std::exp(z*logWidth);
			const double x = 2.*M_PI*radius/wavelength;
			const double weight = std::exp(-0.5*z*z);
			const double efficiency = mieScattering(x, mu.data(), AngleCount, intensity.data());
			section += weight*efficiency*M_PI*radius*radius;
			for (std::size_t j = 0; j < AngleCount; ++j)
				phase[j] += weight*M_PI*radius*radius*2.*intensity[j]/(x*x);
		}
		for (double& p : phase)
			p /= section;
		return section;
	};

	std::array<double, AngleCount> phase;
	const double referenceSection = scattering(ReferenceWavelength, phase);

	// The whole sunlight: its luminance fixes the scale
	double white = 0.;
	for (int k = 0; k < WavelengthCount; ++k)
	{
		const double l = FirstWavelength + k*WavelengthStep;
		white += SpectralData::sunlight(l)*SpectralData::cieY(l);
	}

	std::vector<double> xyz(static_cast<std::size_t>(3*HeightCount*AngleCount), 0.);
	for (int k = 0; k < WavelengthCount; ++k)
	{
		const double l = FirstWavelength + k*WavelengthStep;
		const double section = scattering(l, phase)/referenceSection;
		const std::vector<double> t = transmission(l);
		const double sun = SolarIlluminance*SpectralData::sunlight(l)/white;
		const double weights[3] = {SpectralData::cieX(l), SpectralData::cieY(l), SpectralData::cieZ(l)};
		for (int j = 0; j < AngleCount; ++j)
			for (int i = 0; i < HeightCount; ++i)
			{
				const double h = MaxHeight*i/(HeightCount - 1);
				const double u = std::min(h/0.25, static_cast<double>(t.size()) - 1.001);
				const std::size_t c = static_cast<std::size_t>(u);
				const double tr = t[c] + (u - static_cast<double>(c))*(t[c + 1] - t[c]);
				const double value = sun*tr*section*phase[static_cast<std::size_t>(j)]/(4.*M_PI);
				for (int e = 0; e < 3; ++e)
					xyz[static_cast<std::size_t>(3*(j*HeightCount + i) + e)] += value*weights[e];
			}
	}

	// To the linear sRGB of the scene; what falls outside its gamut is clipped
	std::vector<float> table(xyz.size());
	for (std::size_t p = 0; p < xyz.size(); p += 3)
	{
		const double X = xyz[p], Y = xyz[p + 1], Z = xyz[p + 2];
		table[p]     = static_cast<float>(std::max(0.,  3.2406*X - 1.5372*Y - 0.4986*Z));
		table[p + 1] = static_cast<float>(std::max(0., -0.9689*X + 1.8758*Y + 0.0415*Z));
		table[p + 2] = static_cast<float>(std::max(0.,  0.0557*X - 0.2040*Y + 1.0570*Z));
	}
	return table;
}
