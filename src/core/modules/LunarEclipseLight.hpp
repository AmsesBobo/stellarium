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

#ifndef LUNARECLIPSELIGHT_HPP
#define LUNARECLIPSELIGHT_HPP

#include "VecMath.hpp"
#include <vector>

//! The light in the shadow of the Earth during a lunar eclipse: sunlight refracted by the Earth's atmosphere.
//!
//! Seen from a point on the Moon in the shadow, the Earth (angular radius e) covers the Sun (angular radius sigma,
//! its center at the distance d from the Earth's center). Around the Earth's limb the atmosphere bends sunlight
//! towards the point: a thin bright ring. A ray that passes at the tangent height h is bent by the angle
//! delta(h) = (n(h)-1) sqrt(2 pi (R+h)/H) (about 1.2 degrees at the ground, falling off with the density);
//! traced back, it comes from the direction at e(1+h/R) - delta(h) from the Earth's center. Where that is on the
//! solar disk, the ring shows the Sun's radiance (radiance is kept along a ray), times the transmission along the
//! grazing path: Rayleigh scattering (it reddens the light), the Chappuis band of ozone (the turquoise band at the
//! edge of the umbra), tropospheric and stratospheric aerosols (volcanic eruptions darken the eclipse), and the
//! clouds, which stop the rays below about 11 km. The light at the point is the ring summed over its solid angle,
//! for wavelengths across the visible spectrum, with the darkening towards the solar limb; the spectrum is turned
//! into linear RGB (CIE 1931 color matching functions, sRGB primaries) relative to full sunlight.
//!
//! The table holds that light minus what an Earth without atmosphere would let through in the same ring (that
//! part is the direct light of the penumbra, computed with the solid Earth). So the table is the whole effect of
//! the atmosphere: the light in the umbra, and in the penumbra the small loss near the limb (the enlargement of
//! the shadow follows from it). It depends on the distances (e and sigma) and on the state of the atmosphere.
class LunarEclipseLight
{
public:
	//! The state of the Earth's atmosphere, from a bright eclipse to a very dark one
	enum Atmosphere {
		AtmosphereClean = 0,         //!< little aerosol, fewer clouds: bright orange (Danjon L about 3-4)
		AtmosphereTypical = 1,       //!< the usual state: copper red (L about 2)
		AtmosphereVolcanic = 2,      //!< aerosols of a volcanic eruption in the stratosphere: dark red (L about 1)
		AtmosphereMajorEruption = 3  //!< after a major eruption (Pinatubo 1991): nearly invisible (L about 0)
	};
	static constexpr int ATMOSPHERE_COUNT = 4;
	//! Odd, so that the edge of the umbra (t = 0.5) falls on an entry
	static constexpr int TABLE_SIZE = 257;

	//! The table is shared (the Moon of the Earth)
	static LunarEclipseLight& instance();

	//! Make the table for the angular radii of the Earth e and of the Sun sigma seen from the Moon (radians),
	//! and the state of the atmosphere. Recomputed only when one of them changes.
	void update(double e, double sigma, int atmosphere);

	//! The coordinate of the table for the distance d from the center of the shadow (all angles seen from the Moon):
	//! t = 0.5 d/(e-sigma) in the umbra, then from 0.5 at its edge linearly to 1 at the outer edge of the penumbra.
	//! The edge of the umbra is always at 0.5: the light that is computed with the local radii of the Earth and the
	//! Sun (they change by +-0.45% over the face of the Moon) meets the table there exactly.
	static double coordinate(double d, double e, double sigma);
	//! The inverse of coordinate()
	static double distance(double t, double e, double sigma);
	//! The direct light: the part of the Sun (angular radius sigma, limb darkening u) not hidden by a dark disk of
	//! radius r whose center is at the distance d, relative to the whole Sun. The rings of the solar disk span only
	//! the radii that can be visible, so it is accurate also at the edge of the umbra, where only the limb shows.
	static double sunVisible(double sigma, double r, double d, double u, int rings = 24);
	//! The light of the atmosphere at the coordinate t (see coordinate()): linear RGB relative to full sunlight;
	//! it can be negative in the penumbra (the loss near the limb).
	Vec3f light(double t) const;
	//! Its luminance (Y)
	double luminance(double t) const;
	//! The table (TABLE_SIZE entries over t from 0 to 1; rgb, and the luminance in a)
	const std::vector<Vec4f>& table() const {return values;}
	//! Changes whenever the table is computed again
	int version() const {return tableVersion;}

private:
	LunarEclipseLight();
	void computeTransmission(int atmosphere);
	void computeTable();

	// tangent heights (km) and the columns of the species along the grazing path (relative to the vertical ones)
	std::vector<double> heights;
	std::vector<double> columnAir, columnOzone, columnTropoAerosol, columnStratoAerosol;
	// wavelengths (nm), and per wavelength: refraction at the ground, limb darkening, weights of X, Y, Z
	std::vector<double> wavelengths;
	std::vector<double> refractivity, limbDarkening, weightX, weightY, weightZ;
	// transmission along the grazing path [height][wavelength]
	std::vector<double> transmission;
	int currentAtmosphere = -1;
	double currentE = 0., currentSigma = 0.;
	std::vector<Vec4f> values;
	int tableVersion = 0;
};

#endif // LUNARECLIPSELIGHT_HPP
