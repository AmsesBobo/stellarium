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

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

//! @class CloudShape
//! The forms of the cumulus clouds: a flat base and a lumpy top over a height map. Heaps of spheres make the rounded
//! bodies and the cauliflower of a cumulus, with sharp edges, where noise alone would make smoke.
//! Large bodies have varied elliptical footprints, attached shoulders and gently offset tops, baked into the tile.
//! Narrow clefts between neighbouring billows are closed and blended to form a shared body.
//!
//! The map is a tile that repeats, computed once: large bodies some 0.6 to 2 km across, and on them smaller lobes that
//! shape both the outline and the top. Its channels:
//!   0 the shape S: above 0 inside a cloud, up to about 1 in its middle, -0.6 outside (the clamp keeps the mean of
//!     a large area, which the mipmaps give far away, near the edge of a cloud)
//!   1 the height of the cloud (km) at S = 1
//!   2 a soft noise of the tile, from 0 to 1, for the areas with and without clouds
//!   3 continuous warped-noise stratocumulus bank shape, independent of the ellipsoids
//! The top over the base is then about H (S + cover)^0.7, and the underside is flat but for its rim: with more cover the
//! clouds grow. The shader follows the line of
//! sight through the layer in this way, and transmission() does the same for what is drawn over the clouds.
class CloudShape
{
public:
	static constexpr int TileTexels = 1024;
	static constexpr double TileSize = 41.6;      //!< km
	static constexpr double BaseHeight = 1.5;     //!< km
	static constexpr double LayerThickness = 0.8; //!< km: the highest top above the base, at heightScale() 1
	//! The scales (km) of the tile for the areas of clouds, in two mixed sizes that do not repeat together
	static constexpr double CoverScaleLarge = 129.3, CoverScaleSmall = 53.7;
	static constexpr double FineScale = 5.9;      //!< km: the size of the tile for the fine lobes
	static constexpr double Extinction = 50.;     //!< per km in the clouds: they are opaque within some 100 m, with crisp edges

	//! Weather fronts, for the clouds that follow the date: long bands of clouds some hundred km wide with clear skies
	//! between them, from the soft noise of the tile stretched along an axis of the map and bent. The wind carries them
	//! over the observer, and they change slowly as they go. A front sets the cover at each place of the map, in place of
	//! the one cover of a layer. The constants are shared with the shader of CloudMgr.
	struct Front
	{
		bool on = false;
		double shift = 0.;          //!< km across the fronts: where a layer or the cirrus lie in them
		double bias = 0.;           //!< added to the cover
		double scale = 1.;          //!< multiplies the cover (the growth of the clouds in the afternoon)
	};
	static constexpr double FrontAxisCos = 0.940, FrontAxisSin = -0.342;
	static constexpr double FrontAlong = 5000., FrontAcross = 1400.; //!< km per tile along the fronts and across them
	static constexpr double FrontBendScale = 2300.;                  //!< km per tile of the noise that bends them
	static constexpr double FrontBend = 250.;                        //!< km
	static constexpr double FrontGain = 2.3, FrontMid = 0.38, FrontMax = 0.9;
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
		double shift = 0.;          //!< km: where the clouds of this layer lie on the map, so that two layers differ
		Front front;                //!< the fronts, if they set the cover in place of cover
		//! How much the flatness lowers the clouds, and stretches them along the map, as a factor to the size. Heaped cumulus
		//! grow about as high as they are wide; flat layers stay low. The height of a heaped cloud comes from how deep the
		//! air overturns more than from how wide the cloud is: small cumulus are not thin plates, and their height follows
		//! the size only weakly (as its 0.3rd power for cumulus; flat clouds as before)
		double heightScale() const
		{
			const double heaped = 1. - flatness;
			return (1. - 0.75*flatness)*(1. + 0.15*heaped*heaped)*std::pow(std::max(size, 0.05), -0.7*heaped*heaped)*(1. + 3.2*tower());
		}
		//! How much the layer towers, from 0 to 1: heaped clouds several km across are cumulus congestus,
		//! which grow to some 5 km above their base
		double tower() const
		{
			const double t = std::clamp((size - 3.5)/2.5, 0., 1.), heaped = 1. - flatness;
			return t*t*(3. - 2.*t)*heaped*heaped;
		}
		//! A bank shares its broad depth; individual seed heights leave only weak relief.
		double bodyHeight(double seedHeight, double environment) const
		{
			const double bank = 2.*(bodyScale() - 1.);
			return seedHeight + 0.95*bank*(0.44 + 0.12*environment - seedHeight);
		}
		//! Flatten the shared bank shoulders without changing other presets.
		double profileFlatness() const {return flatness + 2.*(bodyScale() - 1.)*(0.95 - flatness);}
		double profileWidth() const {return 0.0625*(1. - 0.84*2.*(bodyScale() - 1.));}
		double stretch() const {return 1./(1. + 2.*flatness);}
		//! The size of the tile of the billows (km). The billows of a cumulus are some tens to a few hundred meters across
		//! whatever the size of the cloud: they grow only weakly with it, or large clouds would be smooth lumps
		//! (the coarse billows are the cloudlets of altocumulus, which are broader: some 250 m)
		double billowTile() const
		{
			const double alto = std::clamp((base - 2.)/1.5, 0., 1.)*std::clamp((flatness - 0.3)/0.3, 0., 1.);
			return BillowScale*0.61*std::pow(std::max(size, 0.05), 0.4)*(1. + 0.6*alto);
		}
		//! Broader footprints for stratocumulus without increasing vertical depth.
		double bodyScale() const
		{
			const auto smooth = [](double a, double b, double x) {
				const double t = std::clamp((x - a)/(b - a), 0., 1.);
				return t*t*(3. - 2.*t);
			};
			return 1. + 0.5*smooth(0.15, 0.35, flatness)*(1. - smooth(0.45, 0.6, flatness));
		}
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
		double deformU = 0., deformV = 0.;
		double boilX = 0., boilY = 0.;
		double frontU = 0., frontV = 0.;  //!< where the fronts lie on the tile, as they change
	};
	static Motion motion(double jd);
	//! Continuous height-dependent motion at an observer's projected position (km).
	static Motion heightMotion(double jd, double x, double y, double speedRatio, double turn);
	//! Constant projected wind (km/s); translation is measured from J2000 in double precision.
	static Motion manualMotion(double jd, double vx, double vy);
	//! How much faster than at 1.5 km the wind blows at a height (km): it grows with the height up to the jet stream
	static double windAtHeight(double height) {return 0.75 + height/6.;}
	//! How far the wind turns with the height (radians per km): clockwise seen from above in the north, as when warm air
	//! comes in, and the other way in the south
	static constexpr double WindTurn = 3.*M_PI/180.;
	//! The cover (0 to FrontMax) of the fronts at a place of the map (km), without the shift of a layer
	static double frontCover(const std::vector<float>& tile, double mapX, double mapY, const Front& front, const Motion& motion);
	//! Smooth heating proxy (0..1) from recent geometric sunlight; latitude is in radians.
	//! Reconstructs the last six hours with fixed solar declination, without mutating simulation time.
	static double solarHeating(double latitude, const std::array<double, 3>& sunAltAz);
	//! Weaker solar response for flattened and elevated layers; does not clear the night sky.
	static double solarCoverScale(const Layer& layer, double heating);
	//! Automatic weather coverage, including sparser low cumulus. Manual cover is unaffected.
	static double weatherCoverScale(const Layer& layer, double heating);

	//! Cirrus: thin fibrous clouds of ice, high up. Their fibres lie along an axis of the map, from the soft noise of the
	//! tile in three sizes stretched along it, in patches that the cover sets. The constants are shared with the shader.
	static constexpr double CirrusAxisCos = 0.819, CirrusAxisSin = 0.574;
	static constexpr double CirrusPatchScale = 190.;          //!< km per tile of the patches
	//! The streaks of the cirrus lie in the cells of a grid: km per cell along the streaks and across them, and after
	//! how many cells the grid repeats
	static constexpr double CirrusCellX = 14., CirrusCellY = 5., CirrusCells = 64.;
	static constexpr double CirrusBendScale = 60.;            //!< km per tile of the noise that bends them
	static constexpr double CirrusBend = 2.4;                  //!< km
	static constexpr double CirrusShift = 4711.;              //!< km: where the cirrus lie on the map
	//! The density of cirrus (0 to 1) at a place of the map (km, with CirrusShift) with the cover (0 to 1)
	static double cirrusDensity(const std::vector<float>& tile, double mapX, double mapY, double cover);

	//! The tile, four values for each texel by rows
	static std::vector<float> computeTile();
	//! The billows of the cauliflower tops: a tileable 3D texture of BillowTexels^3 RGBA texels, each channel puffs
	//! (balls around random points, highest at their centers: an inverted Worley noise), red with BillowCellsCoarse
	//! cells across the tile, green with BillowCellsFine; blue holds wisps, thin threads of a ridged noise
	//! with BillowCellsWisps cells, for the shreds around a cloud; alpha holds broad, smooth lumps (a
	//! Perlin-Worley noise with BillowCellsLumps cells). The shader takes them away from the density of the clouds above
	//! their flat bases: what is left are the billows, each with its lit side and its shaded underside
	static constexpr int BillowTexels = 96;
	static constexpr int BillowCellsCoarse = 5, BillowCellsFine = 11;
	static constexpr int BillowCellsLumps = 3;   //!< the cells of the broad lumps of the alpha channel
	static constexpr int BillowCellsWisps = 6;   //!< the cells of the coarsest wisps of the blue channel
	static constexpr double BillowScale = 1.8;   //!< km per tile, for clouds of size 1 (see Layer::billowTile())
	static constexpr double BillowRise = 0.0012; //!< km/s: how fast the billows rise through a cloud
	static std::vector<unsigned char> computeBillows();

	//! What the shape adds with the setting cover (0 to 1) and the soft noise (from 0 to 1) at a place, before the limit of
	//! 0.45 that keeps the clear parts of the map (-0.6) clear, with a margin for the fine lobes; below a cover of 0.1 the last clouds go away
	static double coverOffset(double cover, double noise);
	static constexpr double GrowScale = 14.3;     //!< km: the size of the tile for the noise of growth
	static constexpr double GrowStrength = 0.75;
	static constexpr double DeformScale = 37.;   //!< km per tile: slow, broad deformation
	static constexpr double GroupScale = 61.;    //!< km per tile: groups several cloud bodies across
	//! How much of the outline of a stratocumulus comes from the continuous bank (channel 3 of the tile), the rest from
	//! the bodies (channel 0): with all of it the layer is one carpet, with part of it broad masses with gaps between them.
	//! The same number stands in cloudBodySample() of the shader of CloudMgr
	static constexpr double BankShare = 0.4;
	//! Numbers that stand in the shader of CloudMgr too (cloudBodySample(), cloudDepth(), cloudWeatherAt()) and must be
	//! changed there with them: the shape of an altocumulus sheet before it breaks up, how much the lanes between its
	//! cloudlets take away, by how much of the size of the clouds the slow deformation shifts them (km), and how much of
	//! a cloud the billows take away on average (the shader has the billows themselves)
	static constexpr double AltoSheet = 0.6, AltoGap = 0.5, DeformStrength = 1.6, MeanErosion = 0.56;
	static constexpr double CumulusGroupScale = 23.; //!< the same for heaped cumulus: groups some 2 to 5 km across
	//! Inverse group scales (1/km), coverage contrast and strength of broken bands.
	//! Continuous in height and flatness, including custom cloud settings.
	static std::array<double, 4> formationParameters(const Layer& layer);
	//! Coverage adjustment, height multiplier and density multiplier. Shared with cloudWeatherAt().
	static std::array<double, 3> development(const std::vector<float>& tile, const Layer& layer,
	                                      const Motion& motion, double mapX, double mapY);

	//! Preserve cumulus bodies at low coverage and dissolve them through density.
	//! Takes the combined coverage/development/bank offset; mirrors the shader.
	static std::array<double, 3> cohesiveWeather(double offset, double height, double density, double flatness, double cover);

	//! Smooth horizontal displacement (km), shared by geometry and transmission.
	static std::array<double, 2> deformation(const std::vector<float>& tile, const Layer& layer,
	                                      const Motion& motion, double mapX, double mapY);

	//! The transmission of the clouds along a line of sight, from a tile of computeTile().
	//! @param observerRadius km from the center of the Earth
	//! @param altAz unit vector, x to the south, y to the east, z up
	//! @param south, east, up the axes of that frame in the Earth-fixed one
	//! @param layer the settings of the layer
	//! @param motion where the wind has carried the clouds
	//! @param frontMotion where the wind has carried the fronts
	static double transmission(const std::vector<float>& tile, const Layer& layer, const Motion& motion, const Motion& frontMotion, double observerRadius, const std::array<double, 3>& altAz,
	                           const std::array<double, 3>& south, const std::array<double, 3>& east, const std::array<double, 3>& up);
};

#endif // CLOUDSHAPE_HPP
