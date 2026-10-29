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

#include "StelEyeAdaptation.hpp"

#include <algorithm>
#include <cmath>

namespace
{
//! The lowest luminance the model works with (cd/m^2)
constexpr double MIN_LOG_LUMINANCE = -8.;
constexpr double MAX_LOG_LUMINANCE = 9.;

double logOf(double luminance)
{
	return luminance > 0. ? std::clamp(std::log10(luminance), MIN_LOG_LUMINANCE, MAX_LOG_LUMINANCE) : MIN_LOG_LUMINANCE;
}

//! Exponential approach of a level to the target over the time t with the time constant tau
double approach(double level, double target, double t, double tau)
{
	return target + (level - target)*std::exp(-t/tau);
}
}

double StelEyeAdaptation::conesThreshold(double luminance)
{
	const double l = logOf(luminance);
	double t;
	if (l <= -2.6)
		t = -0.72;
	else if (l >= 1.9)
		t = l - 1.255;
	else
		t = std::pow(0.249*l + 0.65, 2.7) - 0.72;
	return std::pow(10., t);
}

double StelEyeAdaptation::rodsThreshold(double luminance)
{
	const double l = logOf(luminance);
	double t;
	if (l <= -3.94)
		t = -2.86;
	else if (l >= -1.44)
		t = l - 0.395;
	else
		t = std::pow(0.405*l + 1.6, 2.18) - 2.86;
	return std::pow(10., t);
}

double StelEyeAdaptation::steadyThreshold(double luminance)
{
	return std::min(conesThreshold(luminance), rodsThreshold(luminance));
}

double StelEyeAdaptation::luminanceForThreshold(double threshold)
{
	// The threshold of the steady state rises with the luminance (from the plateau of the dark adapted rods):
	// the highest luminance whose threshold is not above the given one, by bisection in log units
	double lo = MIN_LOG_LUMINANCE, hi = MAX_LOG_LUMINANCE;
	if (steadyThreshold(std::pow(10., hi)) <= threshold)
		return std::pow(10., hi);
	for (int i=0; i<60; ++i)
	{
		const double mid = 0.5*(lo + hi);
		if (steadyThreshold(std::pow(10., mid)) <= threshold)
			lo = mid;
		else
			hi = mid;
	}
	return std::pow(10., lo);
}

double StelEyeAdaptation::fallRateLimited(double height, double rate, double knee, double t)
{
	if (!(height > 0.) || !(t > 0.))
		return height;
	// k ln(E/E0) + E - E0 + r t = 0, decreasing in E: Newton's method in u = ln E (convex, from above)
	const double target = knee*std::log(height) + height - rate*t; // = k ln E + E
	double u = std::log(height);
	for (int i=0; i<60; ++i)
	{
		const double e = std::exp(u);
		const double f = knee*u + e - target;
		const double step = f/(knee + e);
		u -= step;
		if (std::fabs(step) < 1e-10)
			break;
	}
	return std::exp(u);
}

void StelEyeAdaptation::reset(double luminance)
{
	const double l = logOf(luminance);
	cones.neural = rods.neural = l;
	cones.photo = (l > CONES_BLEACH_LEVEL) ? l : MIN_LOG_LUMINANCE;
	rods.photo = (l > RODS_BLEACH_LEVEL) ? rodsPhotoTarget(l) : MIN_LOG_LUMINANCE;
	updateDerived(l);
}

void StelEyeAdaptation::update(double luminance, double dt)
{
	const double l = logOf(luminance);
	if (!(dt > 0.))
	{
		updateDerived(l);
		return;
	}
	// After more than a few hours the eye is adapted, whatever it saw before
	if (dt > 6.*3600.)
	{
		reset(luminance);
		return;
	}

	// The neural levels: light adaptation within a fraction of a second, the first part of the dark adaptation
	// within seconds (the cones) or a few minutes (the rods)
	if (l >= cones.neural)
		cones.neural = approach(cones.neural, l, dt, CONES_LIGHT_TIME);
	else
		cones.neural = approach(cones.neural, l, dt, CONES_NEURAL_DARK_TIME);
	if (l >= rods.neural)
		rods.neural = approach(rods.neural, l, dt, RODS_LIGHT_TIME);
	else
		rods.neural = l + fallRateLimited(rods.neural - l, RODS_NEURAL_DARK_RATE, RODS_REGENERATION_KNEE, dt);

	// The photochemical levels: bleaching by bright light, regeneration of the pigment in the dark
	const double conesTarget = l;
	if (l > CONES_BLEACH_LEVEL && conesTarget > cones.photo)
		cones.photo = approach(std::max(cones.photo, CONES_BLEACH_LEVEL), conesTarget, dt, BLEACH_TIME);
	else if (cones.photo > l)
		cones.photo = approach(cones.photo, l, dt, CONES_REGENERATION_TIME);
	const double rodsTarget = rodsPhotoTarget(l);
	if (l > RODS_BLEACH_LEVEL && rodsTarget > rods.photo)
		rods.photo = approach(std::max(rods.photo, RODS_BLEACH_LEVEL), rodsTarget, dt, BLEACH_TIME);
	else if (rods.photo > l)
		rods.photo = l + fallRateLimited(rods.photo - l, RODS_REGENERATION_RATE, RODS_REGENERATION_KNEE, dt);
	updateDerived(l);
}

void StelEyeAdaptation::updateDerived(double logLuminance)
{
	const double tc = conesThreshold(std::pow(10., cones.level()));
	const double tr = rodsThreshold(std::pow(10., rods.level()));
	const double threshold = std::min(tc, tr);
	limitingSystem = (tr < tc) ? Rods : Cones;
	adaptationLuminance = luminanceForThreshold(threshold);
	thresholdElevation = std::max(0., std::log10(threshold/steadyThreshold(std::pow(10., logLuminance))));
}
