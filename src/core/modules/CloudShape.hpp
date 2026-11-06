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

#ifndef CLOUDSHAPE_HPP
#define CLOUDSHAPE_HPP

#include <array>
#include <vector>

//! @class CloudShape
//! The forms of the cumulus clouds: a flat base and a lumpy top over a height map. Heaps of spheres make the rounded
//! bodies and the cauliflower of a cumulus, with sharp edges, where noise alone would make smoke.
//!
//! The map is a tile that repeats, computed once: large bodies some 0.6 to 2 km across, and on them smaller lobes that
//! shape both the outline and the top. Its channels:
//!   0 the shape S: above 0 inside a cloud, up to about 1 in its middle, -0.35 outside (the clamp keeps the mean of
//!     a large area, which the mipmaps give far away, near the edge of a cloud)
//!   1 the height of the cloud (km) at S = 1
//!   2 a soft noise of the tile, from 0 to 1, for the areas with and without clouds
//!   3 fine lobes, from 0 to 1, for near clouds
//! The top over the base is then about H (S + cover)^0.7, and the underside is flat but for its rim: with more cover the
//! clouds grow. The shader follows the line of
//! sight through the layer in this way, and transmission() does the same for what is drawn over the clouds.
class CloudShape
{
public:
	static constexpr int TileTexels = 1024;
	static constexpr double TileSize = 41.6;      //!< km
	static constexpr double BaseHeight = 1.5;     //!< km
	static constexpr double LayerThickness = 0.8; //!< km: the highest top above the base
	//! The scales (km) of the tile for the areas of clouds, in two mixed sizes that do not repeat together
	static constexpr double CoverScaleLarge = 129.3, CoverScaleSmall = 53.7;
	static constexpr double FineScale = 5.9;      //!< km: the size of the tile for the fine lobes
	static constexpr double Extinction = 20.;     //!< per km in the clouds: they are opaque within a few hundred meters

	//! The settings of the layer of clouds
	struct Layer
	{
		double cover = 0.2;
		double size = 1.;
		double base = BaseHeight;   //!< km: the height of the base over the ground
		double density = 1.;        //!< against the usual extinction in the clouds
		double flatness = 0.;       //!< 0: heaped cumulus, 1: flat, drawn out layers
		//! A bank of clouds: the clouds only stand within it, in a sector of the sky around the observer
		bool bank = false;
		double bankAzimuth = 0.;    //!< degrees, from the north through the east
		double bankWidth = 90.;     //!< degrees
		double bankDistance = 60.;  //!< km to the near edge, over the ground
		double bankDepth = 30.;     //!< km
		//! How much the flatness lowers the clouds, and stretches them along the map
		double heightScale() const {return 1. - 0.75*flatness;}
		double stretch() const {return 1./(1. + 2.*flatness);}
	};
	//! What the bank of clouds lets stand at a place (km from the observer: x to the south, y to the east), from 0 to 1
	static double bankMask(const Layer& layer, double x, double y);

	//! Where the wind has carried the clouds at a moment: the map of the clouds, at the place (x, y) of the Earth, is at
	//! (cosine x - sine y + offsetX, sine x + cosine y + offsetY). It follows from the date alone, so that a moment always looks
	//! the same: westerly winds as a slow turn around the pole, with changes over the days from sums of waves, whose
	//! integrals are known. growU, growV and boilU, boilV are the places of the noise that makes clouds grow and dissolve,
	//! and of the fine lobes, which drift through the clouds. All this is made up: it is no weather.
	struct Motion
	{
		double cosine = 1., sine = 0., offsetX = 0., offsetY = 0.;
		double growU = 0., growV = 0.;
		double boilX = 0., boilY = 0.;
	};
	static Motion motion(double jd);
	//! The share of the sky under clouds at a moment and a longitude (degrees), for the setting that follows the date: it
	//! changes over the days, with some days clear, and the clouds grow in the afternoon
	static double coverOfDate(double jd, double longitude);

	//! The tile, four values for each texel by rows
	static std::vector<float> computeTile();

	//! What the shape adds with the setting cover (0 to 1) and the soft noise (from 0 to 1) at a place, before the limit of
	//! 0.34 that keeps the clear parts of the map (-0.35) clear; below a cover of 0.1 the last clouds go away
	static double coverOffset(double cover, double noise);
	static constexpr double GrowScale = 14.3;     //!< km: the size of the tile for the noise of growth
	static constexpr double GrowStrength = 0.55;

	//! The transmission of the clouds along a line of sight, from a tile of computeTile().
	//! @param observerRadius km from the center of the Earth
	//! @param altAz unit vector, x to the south, y to the east, z up
	//! @param south, east, up the axes of that frame in the Earth-fixed one
	//! @param layer the settings of the layer
	//! @param motion where the wind has carried the clouds
	static double transmission(const std::vector<float>& tile, const Layer& layer, const Motion& motion, double observerRadius, const std::array<double, 3>& altAz,
	                           const std::array<double, 3>& south, const std::array<double, 3>& east, const std::array<double, 3>& up);
};

#endif // CLOUDSHAPE_HPP
