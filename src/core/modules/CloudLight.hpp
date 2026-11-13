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
//! The base is a diffuse surface. It reflects the light of the ground and lets through the light on its top, more where
//! the cloud is thin: with the part D of that light that gets through (D = 1/(1 + 0.75 tau (1 - g)), g = 0.85),
//!   L = ((1 - D) a E + D E) / pi + L_city
//! with the albedo a of the ground. The shader applies this at each place, from the parts given here.
//!
//! Sunlight reaches the clouds through the air, along a ray that the refraction bends around the Earth, as a straight
//! ray over an Earth of 7/6 of its radius. Low Sun makes it red, and it stays on the clouds after sunset, first
//! on their undersides and then on their tops only, until the shadow of the Earth reaches them. Clouds towards the Sun stay
//! in its light the longest.
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
//! Low over the horizon the sides of the clouds show. They are a white surface in the light of the sky, the ground, the Sun
//! and the Moon. The thin edges of a cloud near the Sun or the Moon scatter it forwards, which makes the silver lining.
//!
//! The air between the observer and the cloud dims it and adds its own light. It only scatters what reaches it: direct
//! moonlight, the light of the sky, that of towns, and direct sunlight while it stands above the shadow of the Earth. With
//! the extinction along the way, the light of each source is its illuminance E, a phase function and
//!   E (1 - exp(-tau)) / (4 pi)
//! The air stands in the shadow of the cloud towards the Sun and the Moon, so there is no forward peak. Sunlit air is not
//! computed: the sky behind the cloud stands for it, which takes it to the color of the sky at each place.
class CloudLight
{
public:
	using Rgb = std::array<double, 3>;
	static constexpr double SunOffsetStart = -1.5, SunOffsetStep = 0.75;

	//! What lights the clouds at this moment, all linear sRGB of the scene
	struct Sources
	{
		double sunAltitude = 0.;      //!< radians
		double sunSine = 0.;          //!< the sine of the altitude of the Sun
		double base = 1.5;            //!< km: the height of the base of the clouds over the ground
		double top = 2.3;             //!< km: the height of their tops
		Rgb moon = {0., 0., 0.};      //!< lux on a surface facing the Moon, with the extinction
		double moonSine = 0.;
		Rgb groundExtra = {0., 0., 0.}; //!< lux on the ground from other sources (the light domes of towns)
		Rgb aurora = {0., 0., 0.};    //!< lux on a level surface from the aurorae, high above the clouds
		double artificial = 0.;       //!< the artificial luminance of the clear sky at the zenith (cd/m^2)
		Rgb lamp = {1., 1., 1.};      //!< the color of the street lamps, at unit luminance
		double extinction = 0.2;      //!< the extinction at the zenith (mag/airmass)
	};

	//! The light of the clouds and the air at this moment
	struct Light
	{
		Rgb below = {0., 0., 0.};       //!< the luminance of a surface that reflects all of the light of the ground
		Rgb above = {0., 0., 0.};       //!< the same, that lets through all of the light of the sky and the Moon on its top
		Rgb aboveSun = {0., 0., 0.};    //!< the same for the sunlight on its top
		Rgb city = {0., 0., 0.};        //!< the luminance of the base from the light of towns
		Rgb airSky = {0., 0., 0.};      //!< of the air, at optical depth without limit: from the sky and the towns
		double airSunlit = 0.;          //!< how much of the light of the sky comes from sunlit air below the clouds, from 0 to 1
		Rgb airMoon = {0., 0., 0.};     //!< the same from the Moon
		Rgb silverSun = {0., 0., 0.};   //!< the illuminance of the Sun on the tops, over 4 pi
		Rgb silverMoon = {0., 0., 0.};  //!< the same of the Moon
		Rgb extinction = {0., 0., 0.};  //!< the optical depth of the air below the clouds per km
		Rgb sideSky = {0., 0., 0.};     //!< the luminance of the sides of the clouds in the light of the sky and the ground
		//! The same in sunlight, on a side that faces the Sun, at the base over the observer. Clouds far away see the Sun at
		//! another altitude, and so do their tops: the values are for its altitude plus SunOffsetStart + i SunOffsetStep degrees
		std::array<Rgb, 5> sideSun = {};
		Rgb sideMoon = {0., 0., 0.};    //!< the same in moonlight
	};

	static Light compute(const Sources& sources);

	//! The transmission of the air for sunlight at a height (km) with the Sun at an altitude (radians) as seen from the ground
	static Rgb sunlightTransmission(double sunAltitude, double height, double extinction);
	//! The color of the light of the sky (at unit luminance) with the Sun at this altitude (radians)
	static Rgb skyColor(double sunAltitude);
	//! The horizontal illuminance (lux) from the clear sky alone with the Sun at this altitude (radians), down to the
	//! light of the stars and the airglow
	static double skyIlluminance(double sunAltitude);
	//! The factor from the artificial luminance of the clear sky to that of the clouds above it
	static double cloudAmplification(double artificial);
};

#endif // CLOUDLIGHT_HPP
