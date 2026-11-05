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

#ifndef NOCTILUCENTCLOUDLIGHT_HPP
#define NOCTILUCENTCLOUDLIGHT_HPP

#include <vector>

//! @class NoctilucentCloudLight
//! The light of the noctilucent clouds, from the sunlight and its way to them: a table of the luminance of the clouds
//! against the tangent height of the ray of sunlight and the scattering angle.
//!
//! An optically thin layer scatters the sunlight once. With its vertical optical depth tau at 532 nm, seen along the
//! local vertical, its luminance at each wavelength is
//!   L = E T(h) tau s P(angle) / (4 pi)
//! with E the sunlight above the atmosphere, T the transmission along the ray that reaches the point, s the optical
//! depth against the one at 532 nm and P the phase function, normalized to a mean of 1 over the sphere. A line of sight
//! at a slant through the layer sees more of it.
//!
//! Transmission: the grazing ray crosses the air (Rayleigh), the ozone (Chappuis band) and the aerosols, down to the
//! tangent height, and refraction bends it (Bouguer): rays of a given real tangent height arrive as if they had passed
//! higher on a straight line, spreading out, and rays of different heights cross near a straight tangent height of
//! about 15 km, which is the effective height of the Earth's shadow. Clouds stop rays below about 11 km. The disk of
//! the Sun softens all of this over some 9 km.
//!
//! Scattering: spheres of ice (refractive index 1.31) in a log-normal size distribution, by Mie theory.
//! The sunlight is that of a black body of 5778 K (see SpectralData.hpp), with 133000 lux above the atmosphere.
class NoctilucentCloudLight
{
public:
	static constexpr int HeightCount = 128;     //!< tangent heights in the table, from 0 to MaxHeight
	static constexpr int AngleCount = 33;       //!< scattering angles in the table, from 0 to 180 degrees
	static constexpr double MaxHeight = 90.;    //!< km
	static constexpr double LowestHeight = 8.;  //!< km: no light reaches the clouds along lower straight rays

	//! The table: the luminance (cd/m^2, linear sRGB) for a vertical optical depth of 1 at 532 nm, seen along the
	//! local vertical, as three values for each pair, by rows of HeightCount per scattering angle
	static std::vector<float> computeTable();

	//! The scattering by a sphere of ice of size parameter x, at the angles with the cosines mu (nAngles of them):
	//! its scattering efficiency, and |S1|^2 + |S2|^2 for each angle (Bohren and Huffman 1983)
	static double mieScattering(double x, const double* mu, int nAngles, double* intensity);

	//! How the clouds occur in one hemisphere at one time
	struct Occurrence
	{
		bool any = false;       //!< whether there are any clouds at all
		double edge = 90.;      //!< the latitude (degrees, absolute) that they reach towards the equator
		double strength = 0.;   //!< the optical depth against the one of a typical display
		double cover = 0.28;    //!< the threshold of the noise of the areas that they cover: higher, more broken up
	};
	//! The occurrences of the setting NoctilucentCloudMgr::occurrence:
	//! 0 none, 1 by the season, the latitude and the night, 2 a faint display and 3 a great one, at all times
	static constexpr int OccurrenceOff = 0, OccurrenceSeason = 1, OccurrenceFaint = 2, OccurrenceGreat = 3;
	//! The clouds of a hemisphere at the Julian day jd of the night number night (an integer, see NoctilucentCloudMgr).
	//! By the season: from a month before the solstice to more than two months after it, as seen by the AIM satellite,
	//! and from the middle of May to early August by the observers on the ground. Each night has a
	//! number from its date, which sets how far towards the equator the clouds reach and how bright they are: at the
	//! height of the season beyond 70 degrees on nine nights of ten, beyond 65 on more than half of them, beyond 60 on one
	//! night of five and beyond 55 on one of twenty. Away from the height of the season they stay closer to the pole.
	//! The clouds are reported far less often in the south, in part for lack of observers: here they stay 3 degrees closer
	//! to the pole. The shares of the nights are a choice that fits the observers, not a measurement: the satellites
	//! only see the brighter clouds (AIM: over half of the time at 72.5 degrees in the middle of the season).
	static Occurrence occurrence(double jd, double night, int mode, bool south);
};

#endif // NOCTILUCENTCLOUDLIGHT_HPP
