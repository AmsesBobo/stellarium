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

#include "StelEyePupil.hpp"

#include <algorithm>
#include <cmath>

double StelEyePupil::stanleyDavies(double flux)
{
	const double x = std::pow(std::max(0., flux)/846., 0.41);
	return 7.75 - 5.75*x/(x + 2.);
}

double StelEyePupil::watsonYellott(double luminance, double fieldArea, double age, int eyes)
{
	// One eye alone gets a tenth of the effect of the light of both (the consensual response)
	const double monocular = (eyes < 2) ? 0.1 : 1.;
	const double flux = std::max(0., luminance)*std::max(0., fieldArea)*monocular;
	const double d = stanleyDavies(flux);
	const double y = std::clamp(age, MIN_FORMULA_AGE, MAX_FORMULA_AGE);
	return d + (y - REFERENCE_AGE)*(0.021323 - 0.0095623*d);
}

void StelEyePupil::reset(double luminance, double age)
{
	diameter = watsonYellott(luminance, ADAPTING_FIELD_AREA, age);
}

void StelEyePupil::update(double luminance, double age, double dt)
{
	const double target = watsonYellott(luminance, ADAPTING_FIELD_AREA, age);
	if (!(dt > 0.))
		return;
	const double tau = (target < diameter) ? CLOSING_TIME : OPENING_TIME;
	diameter = target + (diameter - target)*std::exp(-dt/tau);
}

double StelEyePupil::getLightFactor(double luminance) const
{
	const double reference = watsonYellott(luminance, ADAPTING_FIELD_AREA, REFERENCE_AGE);
	return reference > 0. ? (diameter*diameter)/(reference*reference) : 1.;
}
