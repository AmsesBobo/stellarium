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

#include "CloudShape.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace
{
constexpr double EarthRadius = 6371.;

// A random number from 0 to 1 for a cell of a layer of the tile, a slot for each use (the cell numbers repeat with the tile)
double cellRandom(int x, int y, std::uint32_t layer, std::uint32_t slot)
{
	std::uint32_t h = static_cast<std::uint32_t>(x)*73856093u ^ static_cast<std::uint32_t>(y)*19349663u ^ layer*83492791u ^ slot*2654435761u;
	h ^= h >> 13;
	h *= 1274126177u;
	h ^= h >> 16;
	h *= 2246822519u;
	h ^= h >> 13;
	return static_cast<double>(h)/4294967296.;
}

int wrap(int i, int n)
{
	return ((i % n) + n) % n;
}

// Rounded seeds, at most one per cell, with radii in cells. Large bodies may have
// elliptical footprints and attached shoulders; fine lobes remain spherical.
struct Sphere
{
	float x, y, radius, number;
	bool exists;
	float cosine = 1.f, sine = 0.f, aspect = 1.f, shoulder = 0.f;
};
class SphereLayer
{
public:
	SphereLayer(int cellCount, std::uint32_t layer, double radius0, double radius1, double chance, bool variedBodies = false)
		: cells(cellCount), spheres(static_cast<std::size_t>(cellCount)*cellCount)
	{
		for (int j = 0; j < cells; ++j)
			for (int i = 0; i < cells; ++i)
			{
				Sphere& sphere = spheres[index(i, j)];
				sphere = {static_cast<float>(cellRandom(i, j, layer, 1)), static_cast<float>(cellRandom(i, j, layer, 2)),
				                        static_cast<float>(radius0 + (radius1 - radius0)*cellRandom(i, j, layer, 3)),
				                        static_cast<float>(cellRandom(i, j, layer, 4)), cellRandom(i, j, layer, 0) <= chance};
				if (variedBodies)
				{
					const double angle = 2.*M_PI*cellRandom(i, j, layer, 6);
					sphere.cosine = static_cast<float>(std::cos(angle));
					sphere.sine = static_cast<float>(std::sin(angle));
					// Reciprocal axes preserve footprint area. Keep the support within
					// the two-cell neighbourhood, including the attached shoulder.
					sphere.aspect = static_cast<float>(0.86 + 0.34*cellRandom(i, j, layer, 7));
					sphere.shoulder = static_cast<float>(0.25 + 0.40*cellRandom(i, j, layer, 8));
				}
			}
	}
	//! At the position (in cells): the body field and a smoothly blended height seed from 0 to 1
	double at(double x, double y, double* number = nullptr) const
	{
		const int cx = static_cast<int>(std::floor(x)), cy = static_cast<int>(std::floor(y));
		double best = -1e9, weightSum = 0., heightSum = 0.;
		for (int j = -2; j <= 2; ++j)
			for (int i = -2; i <= 2; ++i)
			{
				const Sphere& sphere = spheres[index(wrap(cx + i, cells), wrap(cy + j, cells))];
				if (!sphere.exists)
					continue;
				const double dx = x - (cx + i + static_cast<double>(sphere.x)), dy = y - (cy + j + static_cast<double>(sphere.y));
				const double r = static_cast<double>(sphere.radius);
				const double along = (sphere.cosine*dx + sphere.sine*dy)/(r*sphere.aspect);
				const double across = (-sphere.sine*dx + sphere.cosine*dy)*sphere.aspect/r;
				double s = 1. - along*along - across*across;
				if (sphere.shoulder > 0.f)
				{
					// A lower attached billow, never a detached satellite. Blend its
					// join so the silhouette is asymmetric without a sharp crease.
					const double q = along - sphere.shoulder;
					const double shoulder = 0.72 - (q*q + across*across)/0.64;
					const double blend = std::max(0.12 - std::abs(s - shoulder), 0.)/0.12;
					s = std::max(s, shoulder) + 0.03*blend*blend;
				}
				best = std::max(best, s);
				if (number)
				{
					// Compact, smooth weights: switching the dominant body must not create a cliff
					// in the height map. The support fits inside the neighbourhood searched above.
					const double w = std::max(s + 0.6, 0.);
					const double weight = w*w*w;
					weightSum += weight;
					// Gently offset the tallest part from the footprint centre.
					// This is baked into the height channel, shared by CPU and GPU.
					const double lean = sphere.shoulder > 0.f ? 0.12*std::clamp(along, -1., 1.) : 0.;
					heightSum += weight*std::clamp(static_cast<double>(sphere.number) + lean, 0., 1.);
				}
			}
		if (number)
			*number = weightSum > 0. ? heightSum/weightSum : 0.5;
		return best;
	}
	//! The height of the lobes at a place in tiles, from 0 to 1
	double lobes(double u, double v) const
	{
		return std::sqrt(std::max(at(u*cells, v*cells), 0.));
	}
private:
	std::size_t index(int i, int j) const
	{
		return static_cast<std::size_t>(j)*cells + static_cast<std::size_t>(i);
	}
	int cells;
	std::vector<Sphere> spheres;
};

// Soft periodic value noise (cells per tile)
double softNoise(double x, double y, int cells, std::uint32_t layer)
{
	const double fx = std::floor(x), fy = std::floor(y);
	const int ix = static_cast<int>(fx), iy = static_cast<int>(fy);
	double u = x - fx, v = y - fy;
	u = u*u*(3. - 2.*u);
	v = v*v*(3. - 2.*v);
	const auto at = [cells, layer](int i, int j) {return cellRandom(wrap(i, cells), wrap(j, cells), layer, 5);};
	return (1. - v)*((1. - u)*at(ix, iy) + u*at(ix + 1, iy)) + v*((1. - u)*at(ix, iy + 1) + u*at(ix + 1, iy + 1));
}

// Bilinear reading of a channel at a place in tiles
double readTile(const std::vector<float>& tile, double u, double v, std::size_t channel)
{
	const int n = CloudShape::TileTexels;
	const double x = (u - std::floor(u))*n - 0.5, y = (v - std::floor(v))*n - 0.5;
	const double fx = std::floor(x), fy = std::floor(y);
	const int ix = static_cast<int>(fx), iy = static_cast<int>(fy);
	const double a = x - fx, b = y - fy;
	const auto at = [&tile, n, channel](int i, int j) {
		return static_cast<double>(tile[(static_cast<std::size_t>(wrap(j, n))*n + static_cast<std::size_t>(wrap(i, n)))*4 + channel]);
	};
	return (1. - b)*((1. - a)*at(ix, iy) + a*at(ix + 1, iy)) + b*((1. - a)*at(ix, iy + 1) + a*at(ix + 1, iy + 1));
}
}

CloudShape::Motion CloudShape::motion(double jd)
{
	// One day in seconds and waves with periods in days that have no common multiple. A wave of a speed v (km/s) and an
	// angular frequency w (per s) moves things by v/w. The turn gives some 8 m/s at 60 degrees latitude, here about 3700 km
	// from the pole on the map, and the waves some 6 m/s more each way, so that the wind changes in strength and direction.
	const double days = jd - 2451545.;
	const auto wave = [days](double period, double phase) {return std::sin(2.*M_PI*days/period + phase);};
	const auto reach = [](double speed, double period) {return speed*period*86400./(2.*M_PI);};  // km
	const double turnRate = 0.008/3700.;  // per s
	// (the map turns against the wind, which then blows from the west)
	const double angle = -std::fmod(turnRate*86400.*days, 2.*M_PI)
	                     + (reach(0.0045, 3.1)*wave(3.1, 0.7) + reach(0.003, 7.7)*wave(7.7, 2.1))/3700.;
	Motion m;
	m.cosine = std::cos(angle);
	m.sine = std::sin(angle);
	m.offsetX = reach(0.004, 4.3)*wave(4.3, 1.3) + reach(0.0025, 2.3)*wave(2.3, 4.1);
	m.offsetY = reach(0.004, 3.7)*wave(3.7, 5.2) + reach(0.0025, 9.1)*wave(9.1, 0.4);
	// the noise of growth drifts at 2.5 m/s through the clouds: they grow and dissolve in about 10 to 20 minutes
	const double drift = 0.0025*86400.*days/GrowScale;
	// Wrap each texture axis independently; wrapping before scaling made V jump.
	m.growU = std::remainder(drift, 1.);
	m.growV = std::remainder(0.6*drift, 1.);
	// Broad deformation evolves more slowly than the life field, carried with each layer.
	m.deformU = std::remainder(0.001*86400.*days/DeformScale, 1.);
	m.deformV = std::remainder(-0.0006*86400.*days/DeformScale, 1.);
	// the fine lobes at 1 m/s
	// Bounded, continuous deformation. These coordinates feed several incommensurate
	// texture scales, so resetting them at one texture period cannot be seamless.
	m.boilX = 0.8*std::sin(0.001*86400.*days/0.8);
	m.boilY = 0.7*std::sin(0.0007*86400.*days/0.7 + 1.3);
	// the fronts change as if they drifted at 1.5 m/s across and 0.5 m/s along through the air: in a day or two
	m.frontU = std::fmod(0.0005*86400.*days/FrontAlong, 1.);
	m.frontV = std::fmod(0.0015*86400.*days/FrontAcross, 1.);
	return m;
}

CloudShape::Motion CloudShape::heightMotion(double jd, double x, double y, double speedRatio, double turn)
{
	Motion m = motion(jd);
	const Motion start = motion(2451545.);
	const double dx = (m.cosine - start.cosine)*x - (m.sine - start.sine)*y + m.offsetX - start.offsetX;
	const double dy = (m.sine - start.sine)*x + (m.cosine - start.cosine)*y + m.offsetY - start.offsetY;
	m.offsetX += speedRatio*(std::cos(turn)*dx - std::sin(turn)*dy) - dx;
	m.offsetY += speedRatio*(std::sin(turn)*dx + std::cos(turn)*dy) - dy;
	return m;
}

CloudShape::Motion CloudShape::manualMotion(double jd, double vx, double vy)
{
	Motion m = motion(jd);
	const double seconds = (jd - 2451545.)*86400.;
	m.cosine = 1.;
	m.sine = 0.;
	m.offsetX = -vx*seconds;
	m.offsetY = -vy*seconds;
	return m;
}

double CloudShape::solarHeating(double latitude, const std::array<double, 3>& sunAltAz)
{
	// Local axes: south, east, up. Hold declination fixed over this short interval and
	// rotate the hour angle backwards. This captures season and polar day/night using
	// Stellarium's current Sun, without maintaining history or changing the core date.
	const double sinLat = std::sin(latitude), cosLat = std::cos(latitude);
	const double sinDec = -cosLat*sunAltAz[0] + sinLat*sunAltAz[2];
	const double mean = sinLat*sinDec;
	double heat = 0., weights = 0.;
	for (int i = 0; i < 12; ++i)
	{
		const double hoursAgo = (i + 0.5)*0.5;
		const double angle = hoursAgo*M_PI/12.;
		const double sine = mean + (sunAltAz[2] - mean)*std::cos(angle) - cosLat*sunAltAz[1]*std::sin(angle);
		const double t = std::clamp((sine - 0.02)/0.63, 0., 1.);
		const double weight = std::exp(-hoursAgo/2.);
		heat += weight*t*t*(3. - 2.*t);
		weights += weight;
	}
	return heat/weights;
}

double CloudShape::solarCoverScale(const Layer& layer, double heating)
{
	const double response = 0.30*(1. - 0.85*layer.flatness)/(1. + 0.4*std::max(layer.base - 2.5, 0.));
	return 1. - response*(1. - std::clamp(heating, 0., 1.));
}

double CloudShape::weatherCoverScale(const Layer& layer, double heating)
{
	const auto smooth = [](double a, double b, double x) {
		const double t = std::clamp((x - a)/(b - a), 0., 1.);
		return t*t*(3. - 2.*t);
	};
	// Fair-weather heaps leave more open sky than sheet-like cloud banks.
	// Scale coverage, not density: individual clouds should remain substantial.
	// Blend continuously for custom layers, preserving stratocumulus/altocumulus.
	const double cumulus = (1. - smooth(0.15, 0.4, layer.flatness))
	                     * (1. - smooth(2.5, 3.5, layer.base));
	return solarCoverScale(layer, heating)*(1. - 0.3*cumulus);
}

double CloudShape::frontCover(const std::vector<float>& tile, double mapX, double mapY, const Front& front, const Motion& motion)
{
	// as in the shader of CloudMgr
	const double bend = FrontBend*(readTile(tile, mapX/FrontBendScale + 0.1, mapY/FrontBendScale + 0.6, 2) - 0.5);
	const double u = (FrontAxisCos*mapX - FrontAxisSin*mapY)/FrontAlong + motion.frontU;
	const double v = (FrontAxisSin*mapX + FrontAxisCos*mapY + bend + front.shift)/FrontAcross + motion.frontV;
	return front.scale*std::clamp(FrontGain*(readTile(tile, u, v, 2) - FrontMid) + front.bias, 0., FrontMax);
}

std::vector<unsigned char> CloudShape::computeBillows()
{
	const int n = BillowTexels;
	std::vector<unsigned char> texels(static_cast<std::size_t>(n)*n*n*4, 255);
	// a random point in each cell of a lattice of cells^3, repeating over the tile
	const auto point = [](int i, int j, int k, int cells, int seed) {
		const auto wrap = [cells](int a) {return ((a % cells) + cells) % cells;};
		std::uint32_t h = static_cast<std::uint32_t>(wrap(i)*73856093) ^ static_cast<std::uint32_t>(wrap(j)*19349663)
		                ^ static_cast<std::uint32_t>(wrap(k)*83492791) ^ static_cast<std::uint32_t>(seed*2654435761u);
		std::array<double, 3> p;
		for (double& c : p)
		{
			h ^= h >> 15; h *= 0x2c1b3c6dU; h ^= h >> 12; h *= 0x297a2d39U; h ^= h >> 15;
			c = 0.1 + 0.8*static_cast<double>(h & 0xffffff)/16777216.;
		}
		return p;
	};
	const auto puffs = [&point](double x, double y, double z, int cells, int seed) {
		const double u = x*cells, v = y*cells, w = z*cells;
		const int i0 = static_cast<int>(std::floor(u)), j0 = static_cast<int>(std::floor(v)), k0 = static_cast<int>(std::floor(w));
		// (a smooth minimum of the distances: where two billows meet, the crease between them is rounded)
		double sum = 0.;
		for (int i = i0 - 1; i <= i0 + 1; ++i)
			for (int j = j0 - 1; j <= j0 + 1; ++j)
				for (int k = k0 - 1; k <= k0 + 1; ++k)
				{
					const auto p = point(i, j, k, cells, seed);
					const double dx = i + p[0] - u, dy = j + p[1] - v, dz = k + p[2] - w;
					sum += std::exp(-(dx*dx + dy*dy + dz*dz)/0.06);
				}
		const double nearest = -0.06*std::log(std::max(sum, 1e-30));
		// a ball of radius 0.85 cells around each point: its height over the surface
		return std::pow(std::clamp(1. - nearest/0.72, 0., 1.), 0.7);
	};
	for (int z = 0; z < n; ++z)
		for (int y = 0; y < n; ++y)
			for (int x = 0; x < n; ++x)
			{
				const double u = (x + 0.5)/n, v = (y + 0.5)/n, w = (z + 0.5)/n;
				unsigned char* t = &texels[((static_cast<std::size_t>(z)*n + y)*n + x)*4];
				t[0] = static_cast<unsigned char>(std::lround(255.*puffs(u, v, w, BillowCellsCoarse, 1)));
				t[1] = static_cast<unsigned char>(std::lround(255.*puffs(u, v, w, BillowCellsFine, 2)));
				t[2] = static_cast<unsigned char>(std::lround(255.*puffs(u, v, w, BillowCellsFiner, 3)));
			}
	return texels;
}

std::vector<float> CloudShape::computeTile()
{
	const int n = TileTexels;
	constexpr int Bodies = 26;  // cells per tile: 1.6 km
	const SphereLayer bodies(Bodies, 3, 0.2, 0.55, 0.85, true), lobes(78, 4, 0.45, 0.8, 1.), small(190, 5, 0.45, 0.8, 1.), fine(64, 8, 0.45, 0.8, 1.);
	// Small cumulus of their own between the bodies, a third of their size or less: a field of cumulus comes in all
	// sizes, with many more small clouds than large ones
	constexpr int Cloudlets = 72;
	const SphereLayer cloudlets(Cloudlets, 11, 0.3, 0.5, 0.2);
	std::vector<float> tile(static_cast<std::size_t>(n)*n*4);
	// the shapes of the bodies alone, which set how much of the map a cover fills (see below)
	std::vector<float> bodiesOnly(static_cast<std::size_t>(n)*n), shapes(bodiesOnly.size());
	for (int j = 0; j < n; ++j)
		for (int i = 0; i < n; ++i)
		{
			const double u = (i + 0.5)/n, v = (j + 0.5)/n;
			// the outlines of the bodies are not round: the map is bent a little
			const double bendX = softNoise(u*40., v*40., 40, 1) - 0.5, bendY = softNoise(u*40., v*40., 40, 2) - 0.5;
			const double x = (u + 0.02*bendX)*Bodies, y = (v + 0.02*bendY)*Bodies;
			double which = 0.;
			const double body = bodies.at(x, y, &which);
			// Broad lobes dominate; smaller lobes only gently disturb their outline.
			// Their mean is taken off, so that they make the outline lumpy too
			// (only near the bodies: between them the shape falls off smoothly, so that a high cover grows the clouds rather
			// than making specks of the lobes)
			const double near = std::clamp((body + 0.6)/0.5, 0., 1.);
			const double detailed = body + near*near*(3. - 2.*near)*(0.48*(lobes.lobes(u, v) - 0.75) + 0.09*(small.lobes(u, v) - 0.75));
			// Keep a broad connected core beneath the billows. Negative detail must not
			// carve an otherwise solid body into isolated peaks at modest coverage.
			const double bodyShape = std::max(detailed, 0.85*body - 0.08);
			// (their outlines lumpy too; they fade out near a body, so that they do not stick to it as warts)
			double whichSmall = 0.;
			const double cloudlet = (0.9*cloudlets.at((u + 0.006*bendX)*Cloudlets, (v + 0.006*bendY)*Cloudlets, &whichSmall)
			                         + 0.12*(small.lobes(u, v) - 0.75) - 0.08)*(1. - near);
			const double shape = std::max(bodyShape, cloudlet);
			const std::size_t index = static_cast<std::size_t>(j)*n + static_cast<std::size_t>(i);
			float* texel = tile.data() + index*4;
			shapes[index] = static_cast<float>(std::clamp(shape, -0.6, 1.5));
			bodiesOnly[index] = static_cast<float>(std::clamp(bodyShape, -0.6, 1.5));
			// Broad billows within each body, between the large silhouette and the fine edge
			// lobes. Modulate height rather than adding specks to otherwise clear sky.
			const double billow = 0.65*softNoise(u*47. + 1.7*bendX, v*47. + 1.7*bendY, 47, 9)
			                    + 0.35*softNoise(u*103. - bendY, v*103. + bendX, 103, 10);
			// (a small cloud is lower than a body, about as high as it is wide; its height blends smoothly into that of a body, as a jump would stand as a
			// wall)
			const double t = std::clamp((cloudlet - bodyShape + 0.1)/0.2, 0., 1.), toSmall = t*t*(3. - 2.*t);
			texel[1] = static_cast<float>(((0.2 + 0.4*which) + toSmall*((0.06 + 0.1*whichSmall) - (0.2 + 0.4*which)))*(0.82 + 0.14*billow));
			texel[2] = static_cast<float>(0.65*softNoise(u*5., v*5., 5, 6) + 0.35*softNoise(u*11., v*11., 11, 7));
			texel[3] = static_cast<float>(fine.lobes(u, v));
		}
	// Close narrow clefts between neighbouring billows to give a cloud a shared
	// body. A circular closing preserves isolated silhouettes and large openings;
	// unlike inflating every sphere it does not grow every cloud indiscriminately.
	// All operations wrap, so the procedural tile remains seamless.
	constexpr int JoinRadius = 6;
	const auto pixel = [n](int x, int y) {
		return static_cast<std::size_t>(wrap(y, n))*n + wrap(x, n);
	};
	const auto close = [n, &pixel](std::vector<float>& shape) {
		std::vector<float> dilated(shape.size()), added(shape.size());
		for (int y = 0; y < n; ++y) for (int x = 0; x < n; ++x)
		{
			float value = -0.6f;
			for (int dy = -JoinRadius; dy <= JoinRadius; ++dy)
				for (int dx = -JoinRadius; dx <= JoinRadius; ++dx)
					if (dx*dx + dy*dy <= JoinRadius*JoinRadius)
						value = std::max(value, shape[pixel(x + dx, y + dy)]);
			dilated[pixel(x, y)] = value;
		}
		for (int y = 0; y < n; ++y) for (int x = 0; x < n; ++x)
		{
			float value = 1.5f;
			for (int dy = -JoinRadius; dy <= JoinRadius; ++dy)
				for (int dx = -JoinRadius; dx <= JoinRadius; ++dx)
					if (dx*dx + dy*dy <= JoinRadius*JoinRadius)
						value = std::min(value, dilated[pixel(x + dx, y + dy)]);
			added[pixel(x, y)] = std::max(value - shape[pixel(x, y)], 0.f);
		}
		// Smooth only the added connections, retaining the original wispy detail.
		// A separable binomial kernel avoids hard ridges from the closing operation.
		constexpr float weights[5] = {1.f, 4.f, 6.f, 4.f, 1.f};
		for (int y = 0; y < n; ++y) for (int x = 0; x < n; ++x)
		{
			float sum = 0.f;
			for (int dx = -2; dx <= 2; ++dx)
				sum += weights[dx + 2]*added[pixel(x + dx, y)];
			dilated[pixel(x, y)] = sum/16.f;
		}
		for (int y = 0; y < n; ++y) for (int x = 0; x < n; ++x)
		{
			float sum = 0.f;
			for (int dy = -2; dy <= 2; ++dy)
				sum += weights[dy + 2]*dilated[pixel(x, y + dy)];
			float& value = shape[pixel(x, y)];
			value = std::min(value + 0.95f*sum/16.f, 1.5f);
		}
	};
	close(shapes);
	close(bodiesOnly);
	// The small clouds must not add to the cover: a cover fills as much of the map as with the bodies alone, which then
	// give room to the small clouds. Each shape value is mapped to the one of the same rank among the bodies alone (a
	// monotonic mapping, which keeps every outline)
	std::vector<float> sortedShapes(shapes), sortedBodies(bodiesOnly);
	std::sort(sortedShapes.begin(), sortedShapes.end());
	std::sort(sortedBodies.begin(), sortedBodies.end());
	for (std::size_t k = 0; k < shapes.size(); ++k)
	{
		const auto rank = static_cast<std::size_t>(std::upper_bound(sortedShapes.begin(), sortedShapes.end(), shapes[k]) - sortedShapes.begin());
		const auto first = static_cast<std::size_t>(std::lower_bound(sortedShapes.begin(), sortedShapes.end(), shapes[k]) - sortedShapes.begin());
		// (ties, as at the clamp, take the middle of their ranks)
		tile[4*k] = sortedBodies[std::min((first + rank)/2, sortedBodies.size() - 1)];
	}
	return tile;
}

double CloudShape::coverOffset(double cover, double noise)
{
	const double fading = std::max(0.1 - cover, 0.);
	return 1.1*(cover - 0.45) + 2.*(noise - 0.5) - 150.*fading*fading;
}

std::array<double, 4> CloudShape::formationParameters(const Layer& layer)
{
	const auto smooth = [](double a, double b, double x) {
		const double t = std::clamp((x - a)/(b - a), 0., 1.);
		return t*t*(3. - 2.*t);
	};
	// Low flat layers organize into broad banks. Higher flat layers have smaller
	// patches with a weak band structure. Blend rather than switching at a preset.
	const double alto = smooth(2., 3.5, layer.base)*smooth(0.3, 0.6, layer.flatness);
	const double lowX = GroupScale*(1. + 2.*layer.flatness);
	const double lowY = GroupScale*(1. + 0.5*layer.flatness);
	return {1./(lowX + alto*(32. - lowX)), 1./(lowY + alto*(18. - lowY)),
	        // A little more contrast between low-cloud banks and their clear gaps.
	        // Keep the altocumulus formation parameters exactly as before.
	        0.95 - 0.45*std::clamp(layer.flatness/0.6, 0., 1.) + 0.25*alto
	        + 0.25*(1. - smooth(0.4, 0.6, layer.flatness)), 0.28*alto};
}

std::array<double, 3> CloudShape::development(const std::vector<float>& tile, const Layer& layer,
                                              const Motion& motion, double mapX, double mapY)
{
	// Groups travel with the cloud bodies; the life field evolves through them. Flattened
	// layers form longer, more persistent patches rather than pulsing like cumulus.
	const auto formation = formationParameters(layer);
	// Slowly migrating favourable regions let banks reorganize relative to their
	// advected bodies. Sines of wrapped phases are continuous across date scrubbing.
	// Keep the established high-cloud formations unchanged.
	const double flatT = std::clamp((layer.flatness - 0.4)/0.2, 0., 1.);
	const double low = 1. - flatT*flatT*(3. - 2.*flatT);
	const double amplitude = 0.045*low*(1. - 0.5*layer.flatness);
	const double u = mapX*formation[0] + amplitude*std::sin(2.*M_PI*motion.deformU);
	const double v = mapY*formation[1] + amplitude*std::sin(2.*M_PI*motion.deformV + 1.1);
	const double broad = readTile(tile, u, v, 2);
	const double detail = readTile(tile, u*2.3 + 0.31, v*2.3 + 0.57, 2);
	const double patches = 0.75*broad + 0.25*detail;
	// Warped, locally faded bands, not uniform stripes across the whole sky.
	const double bands = 0.5 + 0.5*std::cos(2.*M_PI*(5.*std::remainder(v, 1.) + 0.8*(broad - 0.5) + 0.35*(detail - 0.5)));
	const double bandT = std::clamp((broad - 0.25)/0.5, 0., 1.);
	const double bandWeight = formation[3]*bandT*bandT*(3. - 2.*bandT);
	const double group = patches + bandWeight*(bands - patches);
	const double grow = 0.7*readTile(tile, mapX/GrowScale + motion.growU, mapY/GrowScale + motion.growV, 2)
	                  + 0.3*readTile(tile, mapX/GrowScale - motion.growV + 0.37, mapY/GrowScale + motion.growU + 0.19, 2);
	// Shared moisture proxy coordinates growth within a group while retaining
	// local turnover. This is a visual model, not a thermodynamic calculation.
	const double coordinated = grow + 0.1*low*(patches - grow);
	const double t = std::clamp((coordinated - 0.25)/0.5, 0., 1.);
	const double life = (1. - 0.65*layer.flatness)*t*t*(3. - 2.*t) + 0.65*layer.flatness*0.5;
	const auto smooth = [](double a, double b, double x) {
		const double t = std::clamp((x - a)/(b - a), 0., 1.);
		return t*t*(3. - 2.*t);
	};
	// Keep CPU star/planet transmission aligned with the GPU population mask.
	const double population = layer.front.on ? (1. - smooth(0.15, 0.4, layer.flatness))
		* (1. - smooth(2.5, 3.5, layer.base)) : 0.;
	const double occupied = 1. + population*(smooth(0.45, 0.57, patches) - 1.);
	return {formation[2]*(group - 0.5) + GrowStrength*(life - 0.5), 0.65 + 0.35*life,
	        (0.45 + 0.55*life)*occupied};
}

std::array<double, 3> CloudShape::cohesiveWeather(double offset, double height, double density, double flatness, double cover)
{
	// Cumulus get fewer and smaller at a low cover, down to a few percent of the sky, and keep their density (thin
	// clouds look fibrous); only the last few dissolve by thinning. As in the shader of CloudMgr
	const double t = std::clamp((flatness - 0.15)/0.35, 0., 1.);
	const double coherence = 1. - t*t*(3. - 2.*t);
	const double u = std::clamp((offset + 1.25)/0.4, 0., 1.);
	const double fade = u*u*(3. - 2.*u);
	// Preserving the core must not create faint clouds in a clear weather region.
	const double presence = std::clamp((coverOffset(cover, 0.) + 2.995)/1.61, 0., 1.);
	return {offset + coherence*(std::max(offset, -0.85) - offset), height,
	        density*(1. - coherence + coherence*fade)*presence*presence*(3. - 2.*presence)};
}

std::array<double, 2> CloudShape::deformation(const std::vector<float>& tile, const Layer& layer,
                                              const Motion& motion, double mapX, double mapY)
{
	const double u = mapX/DeformScale + motion.deformU, v = mapY/DeformScale + motion.deformV;
	const double strength = 0.3*layer.size*(1. - 0.4*layer.flatness);
	return {strength*(readTile(tile, u, v, 2) - 0.5),
	        strength*(readTile(tile, u + 0.37, v + 0.71, 2) - 0.5)};
}

double CloudShape::cirrusDensity(const std::vector<float>& tile, double mapX, double mapY, double cover)
{
	if (cover <= 0.) return 0.;
	// as in the shader of CloudMgr, at the full detail of the tile
	const auto smooth = [](double a, double b, double x) {
		const double t = std::clamp((x - a)/(b - a), 0., 1.);
		return t*t*(3. - 2.*t);
	};
	const double x = CirrusAxisCos*mapX - CirrusAxisSin*mapY + CirrusBend*(readTile(tile, mapX/CirrusBendScale + 0.5, mapY/CirrusBendScale + 0.5, 2) - 0.5);
	double y = CirrusAxisSin*mapX + CirrusAxisCos*mapY + CirrusBend*(readTile(tile, mapX/CirrusBendScale + 0.25, mapY/CirrusBendScale + 0.25, 2) - 0.5);
	const double patch = readTile(tile, mapX/CirrusPatchScale, mapY/CirrusPatchScale, 2);
	const double area = smooth(0., 1., (patch - 0.85 + 0.9*cover)/0.2);
	const double tuft = readTile(tile, x/CirrusScaleX[0], y/CirrusScaleY[0], 2);
	const double envelope = readTile(tile, x/CirrusScaleX[0] + 0.23, y/CirrusScaleY[0] + 0.61, 2);
	y += 1.4*(tuft - 0.5);
	const double strands = readTile(tile, x/CirrusScaleX[1] + 0.37, y/CirrusScaleY[1] + 0.71, 2);
	const double fine = readTile(tile, x/CirrusScaleX[2] + 0.13, y/CirrusScaleY[2] + 0.29, 2);
	const double wisps = smooth(0.38, 0.72, 0.7*strands + 0.3*fine);
	return std::clamp(1.6*area*smooth(0.3, 0.7, envelope)*(0.85*wisps + 0.15*smooth(0.52, 0.8, tuft)), 0., 1.);
}

double CloudShape::bankMask(const Layer& layer, double x, double y)
{
	if (!layer.bank)
		return 1.;
	const auto smooth = [](double edge0, double edge1, double v) {
		const double t = std::clamp((v - edge0)/(edge1 - edge0), 0., 1.);
		return t*t*(3. - 2.*t);
	};
	const double distance = std::hypot(x, y);
	const double start = layer.bankDistance, end = layer.bankDistance + layer.bankDepth;
	const double soft = 1. + 0.15*layer.bankDepth;
	double m = smooth(start - soft, start + soft, distance)*(1. - smooth(end - soft, end + soft, distance));
	const double halfWidth = 0.5*layer.bankWidth*M_PI/180.;
	if (halfWidth < M_PI)
	{
		const double azimuth = std::atan2(y, -x) - layer.bankAzimuth*M_PI/180.;
		const double away = std::fabs(std::remainder(azimuth, 2.*M_PI));
		const double softAngle = 0.035 + 0.1*halfWidth;
		m *= 1. - smooth(halfWidth - softAngle, halfWidth + softAngle, away);
	}
	return m;
}

double CloudShape::transmission(const std::vector<float>& tile, const Layer& layer, const Motion& motion, const Motion& frontMotion, double observerRadius, const std::array<double, 3>& altAz,
                                const std::array<double, 3>& south, const std::array<double, 3>& east, const std::array<double, 3>& up)
{
	if (altAz[2] < -0.015)
		return 1.;
	// as in the shader of CloudMgr, without the fine lobes
	const double z = altAz[2] < 0.008 ? 0.008*std::exp(altAz[2]/0.008 - 1.) : altAz[2];
	const double size = layer.size, flat = layer.flatness;
	const double base = EarthRadius + layer.base, top = base + LayerThickness*size*layer.heightScale();
	const double b = observerRadius*z;
	const auto shell = [b, observerRadius](double r) {
		const double c = (r - observerRadius)*(r + observerRadius);
		return c/(b + std::sqrt(b*b + c));
	};
	const double t0 = shell(base), t1 = shell(top);
	const int steps = std::clamp(static_cast<int>(std::ceil((t1 - t0)/0.12)), 1, 24)*6;
	const double ds = (t1 - t0)/steps;
	double depth = 0.;
	for (int k = 0; k < steps; ++k)
	{
		const double t = t0 + (k + 0.5)*ds;
		const double px = t*altAz[0], py = t*altAz[1], pz = observerRadius + t*z;
		const double r = std::sqrt(px*px + py*py + pz*pz);
		const double ex = px*south[0] + py*east[0] + pz*up[0], ey = px*south[1] + py*east[1] + pz*up[1], ez = px*south[2] + py*east[2] + pz*up[2];
		const double scale = 2.*base/(r + std::fabs(ez));
		const double mapX = motion.cosine*scale*ex - motion.sine*scale*ey + motion.offsetX + layer.shift;
		const double mapY = motion.sine*scale*ex + motion.cosine*scale*ey + motion.offsetY + layer.shift;
		const double noise = 0.6*readTile(tile, mapX/CoverScaleLarge, mapY/CoverScaleLarge, 2) + 0.4*readTile(tile, mapX/CoverScaleSmall, mapY/CoverScaleSmall, 2);
		const auto drift = deformation(tile, layer, motion, mapX, mapY);
		const double tileX = (mapX + drift[0])*layer.stretch()/(TileSize*size), tileY = (mapY + drift[1])/(TileSize*size);
		const auto life = development(tile, layer, motion, mapX, mapY);
		const double cover = layer.front.on ? frontCover(tile, frontMotion.cosine*scale*ex - frontMotion.sine*scale*ey + frontMotion.offsetX,
		                                                 frontMotion.sine*scale*ex + frontMotion.cosine*scale*ey + frontMotion.offsetY, layer.front, frontMotion)
		                                    : layer.cover;
		const double offset = std::min(coverOffset(cover, noise) + life[0], 0.45) + 2.5*(bankMask(layer, px, py) - 1.);
		const auto weather = cohesiveWeather(offset, life[1], life[2], flat, cover);
		const double shape = readTile(tile, tileX, tileY, 0) + weather[0];
		const double height = size*layer.heightScale()*readTile(tile, tileX, tileY, 1)*weather[1];
		const double round = shape/std::pow(std::fabs(shape) + 0.01, 0.55), plateau = 0.8*shape/std::sqrt(shape*shape + 0.0625);
		const double topHeight = height*(round + flat*(plateau - round));
		const double under = size*layer.heightScale()*(0.012 + 0.008*readTile(tile, tileX, tileY, 2));
		const double level = r - base;
		const double e = std::clamp((std::min(topHeight - level, level - under) + 0.012)/(0.024 + 0.004*size), 0., 1.);
		depth += Extinction*layer.density*weather[2]*ds*e*e*(3. - 2.*e);
		if (depth > 7.)
			return 0.;
	}
	// as in the shader: a thick cloud is opaque
	const double fade = std::clamp((depth - 5.)/1.9, 0., 1.);
	return std::exp(-depth)*(1. - fade*fade*(3. - 2.*fade));
}
