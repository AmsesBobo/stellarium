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

#ifndef STELATMOSPHERETRANSMISSION_HPP
#define STELATMOSPHERETRANSMISSION_HPP

#include "VecMath.hpp"

#include <array>
#include <vector>

class QString;

//! Spectral transmission of the Earth's atmosphere along a line of sight, from a precomputed table
//! of optical depths (CalcMySky, the model behind ShowMySky and the Lightweight atmosphere).
//! It gives the color of the extinction: the light of a star, planet or the Sun near the horizon
//! loses its blue first. It needs no rendering context and no ShowMySky library.
//!
//! File format of atmosphere/lightweight-extinction.dat (little endian):
//! @code
//! char[8]  magic "AtmoExt\0"
//! u32      version (1)
//! u32      width:  number of samples along the line of sight (distance to the top of the atmosphere)
//! u32      height: number of samples of the observer altitude
//! u32      count:  number of wavelengths
//! f32      planet radius (m)
//! f32      atmosphere thickness (m)
//! f32      weights[count][3]: linear sRGB weights of each wavelength; each column sums to 1, so a
//!                             transmission of 1 at all wavelengths gives white (1,1,1)
//! f32      depth[height][width][count]: optical depth (transmission = exp(-depth))
//! @endcode
//! The texture coordinates follow CalcMySky's texture-coordinates.frag: the altitude coordinate is
//! rho/horizon, where rho is the distance to the horizon of the observer and horizon the one from the
//! top of the atmosphere; the distance coordinate maps the distance d to the top of the atmosphere
//! along the ray from its minimum (straight up) to its maximum (towards the horizon).
//! (Format and sampling after Stellarium pull request #5116.)
class StelAtmosphereTransmission
{
public:
	//! Load a table. Returns false (and keeps nothing) if the file is missing or malformed.
	bool load(const QString& path);
	bool isLoaded() const {return !depths.empty();}

	//! Linear RGB transmission for an observer at the given altitude (m) looking at the given
	//! geometric elevation (radians). The optical depths are multiplied by depthScale first
	//! (to match an extinction coefficient set by the user). Channels are clamped to [0,1].
	//! Below the horizon of the observer (the line of sight hits the ground) the result is 0.
	Vec3d transmission(double altitude, double elevation, double depthScale = 1.) const;
	//! Extinction in magnitudes of white light (luminance Y) towards the zenith, for an observer at the given altitude (m)
	double zenithMagnitude(double altitude) const;
	//! Sine of the lowest elevation that the table covers for an observer at the given altitude (the dip of the horizon)
	double horizonSine(double altitude) const;

private:
	unsigned width = 0, height = 0, count = 0;
	double radius = 0., thickness = 0.;
	std::vector<std::array<float,3>> weights;
	std::vector<float> depths;

	bool parse(const char* bytes, std::size_t size);
};

#endif // STELATMOSPHERETRANSMISSION_HPP
