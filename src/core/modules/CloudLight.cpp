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

#include "CloudLight.hpp"

#include <algorithm>
#include <cmath>

namespace
{
constexpr double SolarIlluminance = 133000.;  // lux, above the atmosphere
constexpr double BaseHeightForLight = 1.5;    // km
constexpr double GroundAlbedo = 0.2;
constexpr double SideReflectance = 0.85;
constexpr double DegreesToRadians = M_PI/180.;
// The air is below the layer, at sea level (Bucholtz 1995, at 612, 547 and 464 nm); the haze has about this scale height
constexpr double RayleighDepth[3] = {0.058, 0.098, 0.183};
constexpr double RayleighHeight = 8.;    // km
constexpr double HazeHeight = 1.5;       // km
constexpr double HazeColor[3] = {0.87, 1., 1.2};
constexpr double ExtinctionOfCleanAir = 0.12;  // mag/airmass, with the ozone
constexpr double OzoneDepth[3] = {0.041, 0.026, 0.004};  // 300 DU in the Chappuis band
constexpr double OzoneHeight = 22., OzoneWidth = 6.;     // km
// Much of what the haze scatters stays in the beam, by forward scattering
constexpr double HazeForwardShare = 0.6;
constexpr double RefractedEarthRadius = 6371.*7./6.;     // km

CloudLight::Rgb mix(const CloudLight::Rgb& a, const CloudLight::Rgb& b, double t)
{
	return {a[0] + t*(b[0] - a[0]), a[1] + t*(b[1] - a[1]), a[2] + t*(b[2] - a[2])};
}
double smooth(double edge0, double edge1, double x)
{
	const double t = std::clamp((x - edge0)/(edge1 - edge0), 0., 1.);
	return t*t*(3. - 2.*t);
}
}

CloudLight::Rgb CloudLight::skyColor(double sunAltitude)
{
	// By day as for the ground (see LandscapeMgr::groundLight()), in the twilight as the hemisphere of the sky of the
	// model, and neutral in the dark, from the stars and the airglow
	const Rgb day = {0.86, 1., 1.26}, twilight = {0.6, 1., 2.}, night = {1., 1., 1.};
	const double degrees = sunAltitude/DegreesToRadians;
	Rgb color = degrees > -4. ? mix(twilight, day, smooth(-4., 5., degrees)) : mix(night, twilight, smooth(-18., -14., degrees));
	const double luminance = 0.2126*color[0] + 0.7152*color[1] + 0.0722*color[2];
	return {color[0]/luminance, color[1]/luminance, color[2]/luminance};
}

double CloudLight::skyIlluminance(double sunAltitude)
{
	static constexpr std::array<double, 16> altitudes = {-18., -16., -14., -12., -10., -8., -6., -4., -2., 0., 5., 10., 20., 30., 60., 90.};
	static constexpr std::array<double, 16> logLux = {-3., -2.85, -2.6, -2., -1.1, -0.2, 0.55, 1.35, 2.2, 2.8, 3.4, 3.65, 3.95, 4.05, 4.25, 4.3};
	const double degrees = sunAltitude/DegreesToRadians;
	if (degrees <= altitudes.front())
		return std::pow(10., logLux.front());
	if (degrees >= altitudes.back())
		return std::pow(10., logLux.back());
	const std::size_t i = static_cast<std::size_t>(std::upper_bound(altitudes.begin(), altitudes.end(), degrees) - altitudes.begin());
	const double t = (degrees - altitudes[i-1])/(altitudes[i] - altitudes[i-1]);
	return std::pow(10., logLux[i-1] + t*(logLux[i] - logLux[i-1]));
}

CloudLight::Rgb CloudLight::sunlightTransmission(double sunAltitude, double height, double extinction)
{
	// the ray from the point, over the Earth with the larger radius: x along the surface, y up from its center
	const double radius = RefractedEarthRadius + height;
	const double dirX = std::cos(sunAltitude), dirY = std::sin(sunAltitude);
	// its tangent height: where a ray that goes down is lowest (with the disk of the Sun that softens it)
	double visible = 1.;
	if (sunAltitude < 0.)
		visible = smooth(-0.5, 0.5, radius*dirX - RefractedEarthRadius);
	if (visible <= 0.)
		return {0., 0., 0.};
	double rayleigh = 0., haze = 0., ozone = 0.;
	double s = 0., ds = 0.25;
	while (s < 1500.)
	{
		const double x = (s + 0.5*ds)*dirX, y = radius + (s + 0.5*ds)*dirY;
		const double h = std::sqrt(x*x + y*y) - RefractedEarthRadius;
		if (h > 80.)
			break;
		const double level = std::max(h, 0.);
		rayleigh += ds*std::exp(-level/RayleighHeight)/RayleighHeight;
		haze += ds*std::exp(-level/HazeHeight)/HazeHeight;
		const double o = (level - OzoneHeight)/OzoneWidth;
		ozone += ds*std::exp(-0.5*o*o)/(OzoneWidth*std::sqrt(2.*M_PI));
		s += ds;
		ds = std::min(ds*1.1, 8.);
	}
	const double hazeDepth = std::max(0., extinction - ExtinctionOfCleanAir)/1.086;
	Rgb t;
	for (std::size_t i = 0; i < 3; ++i)
		t[i] = visible*std::exp(-RayleighDepth[i]*rayleigh - (1. - HazeForwardShare)*hazeDepth*HazeColor[i]*haze - OzoneDepth[i]*ozone);
	return t;
}

double CloudLight::cloudAmplification(double artificial)
{
	// the power law through (0.00025 cd/m^2, 4.7) and (0.0025 cd/m^2, 10.7)
	return 4.7*std::pow(std::max(artificial, 1e-6)/0.00025, 0.357);
}

CloudLight::Light CloudLight::compute(const Sources& s)
{
	Light light;
	const Rgb sky = skyColor(s.sunAltitude);
	for (std::size_t k = 0; k < light.sideSun.size(); ++k)
	{
		const double offset = SunOffsetStart + SunOffsetStep*static_cast<double>(k);
		const Rgb t = sunlightTransmission(s.sunAltitude + offset*DegreesToRadians, BaseHeightForLight, s.extinction);
		for (std::size_t i = 0; i < 3; ++i)
			light.sideSun[k][i] = SideReflectance*SolarIlluminance*t[i]/M_PI;
	}
	const Rgb sunTop = sunlightTransmission(s.sunAltitude, s.top, s.extinction);
	light.airSunlit = smooth(-0.5*DegreesToRadians, 3.*DegreesToRadians, s.sunAltitude);
	const double hazeDepth = std::max(0., s.extinction - ExtinctionOfCleanAir)/1.086;
	const double cloudLight = cloudAmplification(s.artificial)*s.artificial;
	const double skyLight = skyIlluminance(s.sunAltitude);
	for (std::size_t i = 0; i < 3; ++i)
	{
		const double aboveSky = skyLight*sky[i] + s.moon[i]*std::max(0., s.moonSine);
		const double aboveSun = SolarIlluminance*sunTop[i]*std::max(0., s.sunSine);
		const double ground = aboveSky + aboveSun + s.groundExtra[i];
		light.below[i] = GroundAlbedo*ground/M_PI;
		light.above[i] = aboveSky/M_PI;
		light.aboveSun[i] = aboveSun/M_PI;
		light.city[i] = cloudLight*s.lamp[i];
		light.airSky[i] = skyLight*sky[i]/(4.*M_PI) + s.artificial*s.lamp[i];
		light.airMoon[i] = (s.moonSine > 0. ? 1. : 0.)*s.moon[i]/(4.*M_PI);
		light.sideSky[i] = SideReflectance*(skyLight*sky[i] + 0.5*GroundAlbedo*ground)/M_PI + 0.5*cloudLight*s.lamp[i];
		light.silverSun[i] = SolarIlluminance*sunTop[i]/(4.*M_PI);
		light.silverMoon[i] = (s.moonSine > 0. ? 1. : 0.)*s.moon[i]/(4.*M_PI);
		light.sideMoon[i] = (s.moonSine > 0. ? SideReflectance : 0.)*s.moon[i]/M_PI;
		light.extinction[i] = RayleighDepth[i]/RayleighHeight + hazeDepth*HazeColor[i]*(1. - std::exp(-1.))/HazeHeight;
	}
	return light;
}
