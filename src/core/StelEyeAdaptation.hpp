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
//! The cones see the photopic luminance of the light, the rods its scotopic luminance (K'm = 1700 lm/W). The
//! thresholds are compared for a test light of the spectrum of the view, in photopic units: a threshold of the
//! rods, in scotopic units, is divided by the scotopic/photopic ratio of the view.
//!
//! From the levels of the two systems comes the mesopic adaptation of CIE 191:2010: the share m of the cones in
//! the vision (1 above about 5 cd/m^2, 0 below about 0.005 cd/m^2), which sets the spectral sensitivity of the
//! eye (the Purkinje shift) and how much color it sees. While the rods are still far less sensitive than the
//! cones (in the first minutes in the dark after bright light), the cones keep the vision.
class StelEyeAdaptation
{
public:
	enum System {Cones, Rods};

	//! The eye fully adapted to the photopic and the scotopic luminance (cd/m^2, scotopic cd/m^2)
	void reset(double photopic, double scotopic);
	//! The eye after looking at the luminances (photopic, scotopic) for the time dt (seconds; any length)
	void update(double photopic, double scotopic, double dt);

	//! The mesopic adaptation (CIE 191:2010) now: the share of the cones (0 to 1)
	double getMesopicM() const {return mesopicM;}
	//! The ratio of the scotopic to the photopic luminance of the view (its spectrum, as the rods see it)
	double getScotopicRatio() const {return scotopicRatio;}
	//! The mesopic luminance (CIE 191:2010) of light with the photopic and scotopic luminances, for the share m
	//! of the cones
	static double mesopicLuminance(double photopic, double scotopic, double m);
	//! The share m of the cones for the adapting photopic and scotopic luminances (the iteration of CIE 191:2010)
	static double mesopicAdaptation(double photopic, double scotopic);
	//! The scotopic luminance (scotopic cd/m^2) of a linear sRGB color whose luminance is in cd/m^2: a fit to the
	//! scotopic/photopic ratios of black bodies from 2000 to 20000 K and the CIE daylights (within 1% from 3000 K
	//! up, 2.44 for white); saturated colors are only approximate, red ones come out at zero
	static constexpr double SCOTOPIC_RED = -0.0555853;
	static constexpr double SCOTOPIC_GREEN = 1.6003170;
	static constexpr double SCOTOPIC_BLUE = 0.8948308;
	static double scotopicLuminance(double red, double green, double blue)
	{
		const double s = SCOTOPIC_RED*red + SCOTOPIC_GREEN*green + SCOTOPIC_BLUE*blue;
		return s > 0. ? s : 0.;
	}
	//! CIE 191:2010: V'(555 nm) = 683/1699, and the constants of the share of the cones m = a + b log10(L_mes)
	static constexpr double MESOPIC_V0 = 683./1699.;
	static constexpr double MESOPIC_A = 0.7670;
	static constexpr double MESOPIC_B = 0.3334;

	//! The effective adaptation luminance (cd/m^2), see the class description
	double getAdaptationLuminance() const {return adaptationLuminance;}
	//! The increment threshold of the eye now (cd/m^2)
	double getThreshold() const {return threshold;}
	//! How much higher the threshold of the eye is than when fully adapted to the current light (log10 units,
	//! 0 when adapted; times 2.5 it is in magnitudes)
	double getThresholdElevation() const {return thresholdElevation;}
	//! The system that sets the threshold now
	System getLimitingSystem() const {return limitingSystem;}
	//! Whether the eye is (nearly) adapted to the current light
	bool isAdapted() const {return thresholdElevation < 0.02;}

	//! The increment threshold of the steady state at the adaptation luminance: of the cones (photopic cd/m^2),
	//! of the rods (scotopic cd/m^2; Ferwerda et al. 1996, fitted to the data of Hood & Finkelstein 1986)
	static double conesThreshold(double luminance);
	static double rodsThreshold(double scotopic);
	//! The threshold of the eye (the lower one, photopic cd/m^2) in the steady state at the photopic luminance,
	//! for light of the scotopic/photopic ratio (2.44: white)
	static double steadyThreshold(double luminance, double scotopicRatio = WHITE_SCOTOPIC_RATIO);
	static constexpr double WHITE_SCOTOPIC_RATIO = SCOTOPIC_RED + SCOTOPIC_GREEN + SCOTOPIC_BLUE;
	//! The adaptation luminance (photopic cd/m^2) of the steady state with the threshold (photopic cd/m^2): the
	//! inverse of steadyThreshold(). The threshold cannot get lower than in darkness: the eye's own "dark light"
	//! (the luminance where the threshold starts to rise) is the lowest result.
	static double luminanceForThreshold(double threshold, double scotopicRatio = WHITE_SCOTOPIC_RATIO);

	// The parameters of the model (times in seconds, levels in log10 cd/m^2, rates in log10 units per second)
	//! Light adaptation: time constants of the neural levels when the light gets brighter
	static constexpr double CONES_LIGHT_TIME = 0.3;
	static constexpr double RODS_LIGHT_TIME = 0.5;
	//! Neural dark adaptation: the cones exponentially, the rods at a limited rate (1 log unit per minute)
	static constexpr double CONES_NEURAL_DARK_TIME = 3.;
	static constexpr double RODS_NEURAL_DARK_RATE = 1./60.;
	//! Bleaching: light above these levels sets the photochemical level, within about BLEACH_TIME. A cone
	//! bleaches noticeably above about 100 cd/m^2 (some 1000 trolands; half of the pigment at 20000 trolands,
	//! Rushton & Henry 1968), a rod above about 2.5 scotopic cd/m^2 (1 cd/m^2 of white light). The rods
	//! saturate (Aguilar & Stiles 1954): above about 80 scotopic cd/m^2 their level rises only slowly (brighter
	//! light still bleaches more, and the rods take over later in the dark: after 8 to 10 minutes, as in the
	//! curves of Hecht, Haig & Chase 1937).
	static constexpr double CONES_BLEACH_LEVEL = 2.;
	static constexpr double RODS_BLEACH_LEVEL = 0.4;
	static constexpr double RODS_SATURATION_LEVEL = 1.9;
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
	void updateDerived(double logPhotopic, double logScotopic);
	//! The photochemical level of the rods that light of the level l leads to
	static double rodsPhotoTarget(double l)
	{
		return l <= RODS_SATURATION_LEVEL ? l : RODS_SATURATION_LEVEL + RODS_SATURATED_SLOPE*(l - RODS_SATURATION_LEVEL);
	}

	Levels cones, rods;
	double adaptationLuminance = 1e-3;
	double threshold = 1e-3;
	double thresholdElevation = 0.;
	System limitingSystem = Rods;
	double mesopicM = 0.;
	double scotopicRatio = WHITE_SCOTOPIC_RATIO;
};

#endif // STELEYEADAPTATION_HPP
