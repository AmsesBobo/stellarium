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

#ifndef STELEYEPUPIL_HPP
#define STELEYEPUPIL_HPP

//! The pupil of the eye, for the naked eye mode.
//!
//! The diameter of the steady state is the unified formula of Watson & Yellott (2012, Journal of Vision 12(10):12),
//! from the luminance of the adapting field, its size and the age of the observer: the formula of Stanley &
//! Davies (1995) for the corneal flux density (luminance times field area), with the effect of age found by
//! Winn et al. (1994) around the mean age of their observers (28.58 years). Both eyes look at the field.
//!
//! The pupil does not change at once: it closes within a fraction of a second in brighter light and opens
//! within several seconds in the dark (Loewenfeld 1993).
class StelEyePupil
{
public:
	//! The steady pupil diameter (mm) for the luminance (cd/m^2) of a field of the area (square degrees), at
	//! the age (years; the formula holds from 20 to 83 years, beyond that the age is taken at these limits),
	//! seen with eyes (1 or 2) eyes
	static double watsonYellott(double luminance, double fieldArea, double age, int eyes = 2);
	//! The formula of Stanley & Davies (1995) for the corneal flux density F (cd/m^2 deg^2): diameter (mm)
	static double stanleyDavies(double flux);
	//! The reference age of the formula: the age of the observer in whom age has no effect (years)
	static constexpr double REFERENCE_AGE = 28.58;
	static constexpr double MIN_FORMULA_AGE = 20.;
	static constexpr double MAX_FORMULA_AGE = 83.;
	//! The area of the field that the pupil adapts to: the field of the eyes (a circle of 120 degrees), not the
	//! field of view on the screen (zooming in is looking through an instrument, it does not open the pupil)
	static constexpr double ADAPTING_FIELD_AREA = 3.14159265358979*60.*60.;
	//! The time constants of the pupil: closing, opening (seconds)
	static constexpr double CLOSING_TIME = 0.3;
	static constexpr double OPENING_TIME = 5.;

	//! The pupil fully adapted to the luminance (cd/m^2) at the age (years)
	void reset(double luminance, double age);
	//! The pupil after the time dt (seconds) in the luminance (cd/m^2)
	void update(double luminance, double age, double dt);
	//! The diameter now (mm)
	double getDiameter() const {return diameter;}
	//! How much light the pupil lets in now, relative to the steady pupil of an observer of the reference age at
	//! the luminance (cd/m^2): the ratio of the areas. The thresholds of the eye were measured by such observers.
	double getLightFactor(double luminance) const;

private:
	double diameter = 7.;
};

#endif // STELEYEPUPIL_HPP
