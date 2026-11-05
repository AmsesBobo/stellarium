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

#ifndef CLOUDLIGHT_HPP
#define CLOUDLIGHT_HPP

#include <array>

//! @class CloudLight
//! The light of the ordinary clouds and of the air in front of them, from what lights them at this moment. Seen from the
//! ground a thick cloud shows its base, and it stays much the same over the sky, however much the sky behind it changes.
//!
//! The base is a diffuse surface, with the luminance
//!   L = (R a E + D E) / pi + L_city
//! It reflects the light of the ground below it, with its reflectance R and the albedo a of the ground, and it lets through
//! the part D of the light on its top. R and D belong to a layer of an optical depth of about 30 that does not absorb
//! (D = 1/(1 + 0.75 tau (1 - g)) with g = 0.85).
//!
//! The light of the sky comes from a table of the illuminance against the altitude of the Sun, not from the average
//! luminance of the atmosphere: that follows the bright part of the sky near the Sun and misses the cosine of a level
//! surface. The table fits the zenith at twilight (Patat et al. 2006, A&A 455, 385) with E = about 1.6 pi L_zenith, and
//! the usual tables of twilight illuminance.
//!
//! The light of towns: Kyba et al. (2011, PLoS ONE 6, e17307) measured the zenith at night outside Berlin. From their
//! medians, without the natural 22 mag/arcsec^2, the artificial luminance of the clear sky grows under an overcast sky by
//! 4.7 at the rural site (0.00025 cd/m^2) and 10.7 at the urban one (0.0025 cd/m^2): the power law between them gives
//! the luminance of the cloud.
//!
//! The air between the observer and the cloud dims it and adds its own light. It only scatters what reaches it: direct
//! sunlight while it stands above the shadow of the Earth, moonlight, the light of the sky, and that of towns. With
//! the extinction along the way, the light of each source is its illuminance E, a phase function and
//!   E (1 - exp(-tau)) / (4 pi)
//! The air stands in the shadow of the cloud towards the Sun and the Moon, so there is no forward peak.
class CloudLight
{
public:
	using Rgb = std::array<double, 3>;

	//! What lights the clouds at this moment, all linear sRGB of the scene
	struct Sources
	{
		double sunAltitude = 0.;      //!< radians
		Rgb sun = {0., 0., 0.};       //!< lux on a surface facing the Sun, with the extinction
		double sunSine = 0.;          //!< the sine of the altitude of the Sun
		Rgb moon = {0., 0., 0.};      //!< lux on a surface facing the Moon, with the extinction
		double moonSine = 0.;
		Rgb groundExtra = {0., 0., 0.}; //!< lux on the ground from other sources (the light domes, the aurorae)
		double artificial = 0.;       //!< the artificial luminance of the clear sky at the zenith (cd/m^2)
		Rgb lamp = {1., 1., 1.};      //!< the color of the street lamps, at unit luminance
		double extinction = 0.2;      //!< the extinction at the zenith (mag/airmass)
	};

	//! The light of the clouds and the air at this moment
	struct Light
	{
		Rgb cloud = {0., 0., 0.};       //!< the luminance of the base of the clouds (cd/m^2)
		Rgb airSky = {0., 0., 0.};      //!< of the air, at optical depth without limit: from the sky and the towns
		Rgb airSun = {0., 0., 0.};      //!< the same from the Sun, for a phase function of 1
		Rgb airMoon = {0., 0., 0.};     //!< the same from the Moon
		Rgb extinction = {0., 0., 0.};  //!< the optical depth of the air below the clouds per km
	};

	static Light compute(const Sources& sources);

	//! The color of the light of the sky (at unit luminance) with the Sun at this altitude (radians)
	static Rgb skyColor(double sunAltitude);
	//! The horizontal illuminance (lux) from the clear sky alone with the Sun at this altitude (radians), down to the
	//! light of the stars and the airglow
	static double skyIlluminance(double sunAltitude);
	//! The factor from the artificial luminance of the clear sky to that of the clouds above it
	static double cloudAmplification(double artificial);
};

#endif // CLOUDLIGHT_HPP
