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

#ifndef STELEYEADAPTATION_HPP
#define STELEYEADAPTATION_HPP

//! The adaptation of the eye to the light of the view, over time, for the naked eye mode.
//!
//! The cones and the rods adapt separately. Each has two levels, as log10 of a luminance (cd/m^2):
//! - a neural level that follows the light within seconds (light adaptation, and the first part of the
//!   dark adaptation);
//! - a photochemical level, set by light bright enough to bleach the visual pigment, that falls only as
//!   fast as the pigment regenerates: the cones in a few minutes (exponentially, time constant about 2 min,
//!   Hollins & Alpern 1973), the rods at a nearly constant rate of about 0.24 log units per minute that
//!   slows down at the end (the "S2" and "S3" parts of the rod curve; Pugh 1975, Lamb & Pugh 2004).
//! Each system behaves as if adapted to the higher of its two levels. Its increment threshold is the one of
//! the steady state at that level (the threshold-versus-intensity functions of Ferwerda et al. 1996), and the
//! eye sees with the more sensitive system. After a bright day this gives the classic dark adaptation curve:
//! the cones reach their plateau after 2 to 3 minutes, the rods take over after about 10 minutes (the
//! rod-cone break), and the eye is fully dark adapted after 25 to 30 minutes.
//!
//! The effective adaptation luminance is the luminance to which a fully adapted eye would have the same
//! threshold: the one that sets the tone mapping (and later the visibility of faint objects).
//!
//! The rods should see the scotopic luminance of the light; until the scene has it, both systems get the
//! (photopic) luminance.
class StelEyeAdaptation
{
public:
	enum System {Cones, Rods};

	//! The eye fully adapted to the luminance (cd/m^2)
	void reset(double luminance);
	//! The eye after looking at the luminance (cd/m^2) for the time dt (seconds; any length)
	void update(double luminance, double dt);

	//! The effective adaptation luminance (cd/m^2), see the class description
	double getAdaptationLuminance() const {return adaptationLuminance;}
	//! How much higher the threshold of the eye is than when fully adapted to the current light (log10 units,
	//! 0 when adapted; times 2.5 it is in magnitudes)
	double getThresholdElevation() const {return thresholdElevation;}
	//! The system that sets the threshold now
	System getLimitingSystem() const {return limitingSystem;}
	//! Whether the eye is (nearly) adapted to the current light
	bool isAdapted() const {return thresholdElevation < 0.02;}

	//! The increment threshold of the steady state (cd/m^2) at the adaptation luminance (cd/m^2): of the
	//! cones, of the rods (Ferwerda et al. 1996, fitted to the data of Hood & Finkelstein 1986), and of the
	//! eye (the lower one)
	static double conesThreshold(double luminance);
	static double rodsThreshold(double luminance);
	static double steadyThreshold(double luminance);
	//! The adaptation luminance (cd/m^2) of the steady state with the threshold (cd/m^2): the inverse of
	//! steadyThreshold(). The threshold cannot get lower than in darkness: the eye's own "dark light"
	//! (the luminance where the threshold starts to rise) is the lowest result.
	static double luminanceForThreshold(double threshold);

	// The parameters of the model (times in seconds, levels in log10 cd/m^2, rates in log10 units per second)
	//! Light adaptation: time constants of the neural levels when the light gets brighter
	static constexpr double CONES_LIGHT_TIME = 0.3;
	static constexpr double RODS_LIGHT_TIME = 0.5;
	//! Neural dark adaptation: the cones exponentially, the rods at a limited rate (1 log unit per minute)
	static constexpr double CONES_NEURAL_DARK_TIME = 3.;
	static constexpr double RODS_NEURAL_DARK_RATE = 1./60.;
	//! Bleaching: light above these levels sets the photochemical level, within about BLEACH_TIME. A cone
	//! bleaches noticeably above about 100 cd/m^2 (some 1000 trolands; half of the pigment at 20000 trolands,
	//! Rushton & Henry 1968), a rod above about 1 cd/m^2. The rods saturate (Aguilar & Stiles 1954): above
	//! about 30 cd/m^2 their level rises only slowly (brighter light still bleaches more, and the rods take
	//! over later in the dark: after 8 to 10 minutes, as in the curves of Hecht, Haig & Chase 1937).
	static constexpr double CONES_BLEACH_LEVEL = 2.;
	static constexpr double RODS_BLEACH_LEVEL = 0.;
	static constexpr double RODS_SATURATION_LEVEL = 1.5;
	static constexpr double RODS_SATURATED_SLOPE = 0.25;
	static constexpr double BLEACH_TIME = 10.;
	//! Regeneration of the pigment: the cones exponentially, the rods at a limited rate that slows down within
	//! the last RODS_REGENERATION_KNEE log units
	static constexpr double CONES_REGENERATION_TIME = 120.;
	static constexpr double RODS_REGENERATION_RATE = 0.24/60.;
	static constexpr double RODS_REGENERATION_KNEE = 0.3;

private:
	struct Levels
	{
		double neural = 0.;
		double photo = -10.; //!< none (no bleaching)
		double level() const {return neural > photo ? neural : photo;}
	};
	//! A level above the target falling at the rate r E/(k + E), E its height above the target, for the time t:
	//! the new height (exact solution of k ln(E/E0) + E - E0 = -r t)
	static double fallRateLimited(double height, double rate, double knee, double t);
	void updateDerived(double logLuminance);
	//! The photochemical level of the rods that light of the level l leads to
	static double rodsPhotoTarget(double l)
	{
		return l <= RODS_SATURATION_LEVEL ? l : RODS_SATURATION_LEVEL + RODS_SATURATED_SLOPE*(l - RODS_SATURATION_LEVEL);
	}

	Levels cones, rods;
	double adaptationLuminance = 1e-3;
	double thresholdElevation = 0.;
	System limitingSystem = Rods;
};

#endif // STELEYEADAPTATION_HPP
