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

double StelEyeAdaptation::steadyThreshold(double luminance, double scotopicRatio)
{
	const double k = std::max(1e-3, scotopicRatio);
	return std::min(conesThreshold(luminance), rodsThreshold(k*luminance)/k);
}

double StelEyeAdaptation::luminanceForThreshold(double threshold, double scotopicRatio)
{
	// The threshold of the steady state rises with the luminance (from the plateau of the dark adapted rods):
	// the highest luminance whose threshold is not above the given one, by bisection in log units
	double lo = MIN_LOG_LUMINANCE, hi = MAX_LOG_LUMINANCE;
	if (steadyThreshold(std::pow(10., hi), scotopicRatio) <= threshold)
		return std::pow(10., hi);
	for (int i=0; i<60; ++i)
	{
		const double mid = 0.5*(lo + hi);
		if (steadyThreshold(std::pow(10., mid), scotopicRatio) <= threshold)
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

double StelEyeAdaptation::mesopicLuminance(double photopic, double scotopic, double m)
{
	return (m*photopic + (1. - m)*scotopic*MESOPIC_V0) / (m + (1. - m)*MESOPIC_V0);
}

double StelEyeAdaptation::mesopicAdaptation(double photopic, double scotopic)
{
	// The iteration of CIE 191:2010, from the middle
	double m = 0.5;
	for (int i=0; i<40; ++i)
	{
		const double lmes = std::max(1e-12, mesopicLuminance(photopic, scotopic, m));
		const double next = std::clamp(MESOPIC_A + MESOPIC_B*std::log10(lmes), 0., 1.);
		if (std::fabs(next - m) < 1e-6)
			return next;
		m = next;
	}
	return m;
}

void StelEyeAdaptation::reset(double photopic, double scotopic)
{
	const double lc = logOf(photopic), lr = logOf(scotopic);
	cones.neural = lc;
	rods.neural = lr;
	cones.photo = (lc > CONES_BLEACH_LEVEL) ? lc : MIN_LOG_LUMINANCE;
	rods.photo = (lr > RODS_BLEACH_LEVEL) ? rodsPhotoTarget(lr) : MIN_LOG_LUMINANCE;
	updateDerived(lc, lr);
}

void StelEyeAdaptation::update(double photopic, double scotopic, double dt)
{
	const double lc = logOf(photopic), lr = logOf(scotopic);
	if (!(dt > 0.))
	{
		updateDerived(lc, lr);
		return;
	}
	// After more than a few hours the eye is adapted, whatever it saw before
	if (dt > 6.*3600.)
	{
		reset(photopic, scotopic);
		return;
	}

	// The neural levels: light adaptation within a fraction of a second, the first part of the dark adaptation
	// within seconds (the cones) or a few minutes (the rods)
	if (lc >= cones.neural)
		cones.neural = approach(cones.neural, lc, dt, CONES_LIGHT_TIME);
	else
		cones.neural = approach(cones.neural, lc, dt, CONES_NEURAL_DARK_TIME);
	if (lr >= rods.neural)
		rods.neural = approach(rods.neural, lr, dt, RODS_LIGHT_TIME);
	else
		rods.neural = lr + fallRateLimited(rods.neural - lr, RODS_NEURAL_DARK_RATE, RODS_REGENERATION_KNEE, dt);

	// The photochemical levels: bleaching by bright light, regeneration of the pigment in the dark
	if (lc > CONES_BLEACH_LEVEL && lc > cones.photo)
		cones.photo = approach(std::max(cones.photo, CONES_BLEACH_LEVEL), lc, dt, BLEACH_TIME);
	else if (cones.photo > lc)
		cones.photo = approach(cones.photo, lc, dt, CONES_REGENERATION_TIME);
	const double rodsTarget = rodsPhotoTarget(lr);
	if (lr > RODS_BLEACH_LEVEL && rodsTarget > rods.photo)
		rods.photo = approach(std::max(rods.photo, RODS_BLEACH_LEVEL), rodsTarget, dt, BLEACH_TIME);
	else if (rods.photo > lr)
		rods.photo = lr + fallRateLimited(rods.photo - lr, RODS_REGENERATION_RATE, RODS_REGENERATION_KNEE, dt);
	updateDerived(lc, lr);
}

void StelEyeAdaptation::updateDerived(double logPhotopic, double logScotopic)
{
	// The thresholds for a test light of the spectrum of the view, in photopic units
	scotopicRatio = std::clamp(std::pow(10., logScotopic - logPhotopic), 1e-3, 1e3);
	const double tc = conesThreshold(std::pow(10., cones.level()));
	const double tr = rodsThreshold(std::pow(10., rods.level()))/scotopicRatio;
	threshold = std::min(tc, tr);
	limitingSystem = (tr < tc) ? Rods : Cones;
	adaptationLuminance = luminanceForThreshold(threshold, scotopicRatio);
	thresholdElevation = std::max(0., std::log10(threshold/steadyThreshold(std::pow(10., logPhotopic), scotopicRatio)));
	// The mesopic adaptation: CIE 191:2010 for the light the two systems follow (their neural levels, the steady
	// state), but at least the dominance of the cones while they are much more sensitive than the rods (after
	// bright light the rods need minutes: until then the eye sees with the cones). The dominance goes from 1 to 0
	// over two log units of the ratio of the thresholds (from rods 10 times less sensitive to 10 times more).
	const double steadyM = mesopicAdaptation(std::pow(10., cones.neural), std::pow(10., rods.neural));
	const double dominance = std::clamp(0.5 + 0.5*std::log10(tr/tc), 0., 1.);
	mesopicM = std::max(steadyM, dominance);
}
