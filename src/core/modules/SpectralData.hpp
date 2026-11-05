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

#ifndef SPECTRALDATA_HPP
#define SPECTRALDATA_HPP

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

//! Spectra shared by the models of the sunlight through the atmosphere of the Earth (wavelengths in nm)
namespace SpectralData
{
inline double lobe(double l, double mu, double s1, double s2)
{
	const double s = l < mu ? s1 : s2;
	return std::exp(-0.5*(l-mu)*(l-mu)/(s*s));
}
//! Piecewise Gaussian fit of the CIE 1931 color matching functions (Wyman, Sloan and Shirley 2013)
inline double cieX(double l) { return 1.056*lobe(l, 599.8, 37.9, 31.0) + 0.362*lobe(l, 442.0, 16.0, 26.7) - 0.065*lobe(l, 501.1, 20.4, 26.2); }
inline double cieY(double l) { return 0.821*lobe(l, 568.8, 46.9, 40.5) + 0.286*lobe(l, 530.9, 16.3, 31.1); }
inline double cieZ(double l) { return 1.217*lobe(l, 437.0, 11.8, 36.0) + 0.681*lobe(l, 459.0, 26.0, 13.8); }

//! The spectrum of the sunlight (Planck at 5778 K), in arbitrary units
inline double sunlight(double l)
{
	return 1./(std::pow(l, 5.)*(std::exp(1.4388e7/(l*5778.)) - 1.));
}

//! The absorption cross section of ozone in the Chappuis band (10^-21 cm^2 per molecule)
inline double ozoneCrossSection(double l)
{
	static constexpr std::array<double, 14> wavelengths = {390., 420., 450., 480., 500., 520., 550., 575., 600., 625., 650., 675., 700., 710.};
	static constexpr std::array<double, 14> sections = {0.0, 0.03, 0.3, 0.8, 1.4, 2.0, 3.3, 4.6, 5.1, 4.2, 2.4, 1.6, 1.0, 0.8};
	if (l <= wavelengths.front())
		return sections.front();
	if (l >= wavelengths.back())
		return sections.back();
	const std::size_t i = static_cast<std::size_t>(std::upper_bound(wavelengths.begin(), wavelengths.end(), l) - wavelengths.begin());
	const double t = (l - wavelengths[i-1])/(wavelengths[i] - wavelengths[i-1]);
	return sections[i-1] + t*(sections[i] - sections[i-1]);
}
}

#endif // SPECTRALDATA_HPP
