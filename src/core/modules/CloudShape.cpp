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

// Spheres in the cells of a layer of the tile, one at most in a cell, with a radius in cells
struct Sphere
{
	float x, y, radius, number;
	bool exists;
};
class SphereLayer
{
public:
	SphereLayer(int cellCount, std::uint32_t layer, double radius0, double radius1, double chance)
		: cells(cellCount), spheres(static_cast<std::size_t>(cellCount)*cellCount)
	{
		for (int j = 0; j < cells; ++j)
			for (int i = 0; i < cells; ++i)
				spheres[index(i, j)] = {static_cast<float>(cellRandom(i, j, layer, 1)), static_cast<float>(cellRandom(i, j, layer, 2)),
				                        static_cast<float>(radius0 + (radius1 - radius0)*cellRandom(i, j, layer, 3)),
				                        static_cast<float>(cellRandom(i, j, layer, 4)), cellRandom(i, j, layer, 0) <= chance};
	}
	//! At the position (in cells): the largest of 1 - (d/r)^2, and the number of its sphere from 0 to 1
	double at(double x, double y, double* number = nullptr) const
	{
		const int cx = static_cast<int>(std::floor(x)), cy = static_cast<int>(std::floor(y));
		double best = -1e9;
		for (int j = -2; j <= 2; ++j)
			for (int i = -2; i <= 2; ++i)
			{
				const Sphere& sphere = spheres[index(wrap(cx + i, cells), wrap(cy + j, cells))];
				if (!sphere.exists)
					continue;
				const double dx = x - (cx + i + static_cast<double>(sphere.x)), dy = y - (cy + j + static_cast<double>(sphere.y));
				const double r = static_cast<double>(sphere.radius);
				const double s = 1. - (dx*dx + dy*dy)/(r*r);
				if (s > best)
				{
					best = s;
					if (number)
						*number = static_cast<double>(sphere.number);
				}
			}
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
	const double drift = std::fmod(0.0025*86400.*days/GrowScale, 1.);
	m.growU = drift;
	m.growV = 0.6*drift;
	// the fine lobes at 1 m/s
	m.boilX = std::fmod(0.001*86400.*days, 2.*FineScale);
	m.boilY = -0.7*m.boilX;
	return m;
}

double CloudShape::coverOfDate(double jd, double longitude)
{
	const double days = jd - 2451545.;
	const auto wave = [days](double period, double phase) {return std::sin(2.*M_PI*days/period + phase);};
	const double weather = 0.5 + 0.28*wave(3.37, 1.1) + 0.16*wave(7.9, 2.3) + 0.1*wave(1.71, 3.4);
	const double hour = jd + 0.5 + longitude/360.;
	const double afternoon = 0.7 + 0.3*std::cos(2.*M_PI*(hour - std::floor(hour) - 0.625));
	return std::clamp(1.1*(weather - 0.25)*afternoon, 0., 0.75);
}

std::vector<float> CloudShape::computeTile()
{
	const int n = TileTexels;
	constexpr int Bodies = 26;  // cells per tile: 1.6 km
	const SphereLayer bodies(Bodies, 3, 0.2, 0.55, 0.85), lobes(78, 4, 0.45, 0.8, 1.), small(190, 5, 0.45, 0.8, 1.), fine(64, 8, 0.45, 0.8, 1.);
	std::vector<float> tile(static_cast<std::size_t>(n)*n*4);
	for (int j = 0; j < n; ++j)
		for (int i = 0; i < n; ++i)
		{
			const double u = (i + 0.5)/n, v = (j + 0.5)/n;
			// the outlines of the bodies are not round: the map is bent a little
			const double bendX = softNoise(u*40., v*40., 40, 1) - 0.5, bendY = softNoise(u*40., v*40., 40, 2) - 0.5;
			const double x = (u + 0.02*bendX)*Bodies, y = (v + 0.02*bendY)*Bodies;
			double which = 0.;
			const double body = bodies.at(x, y, &which);
			// lobes on the bodies; their mean is taken off, so that they make the outline lumpy too
			const double shape = body + 0.6*(lobes.lobes(u, v) - 0.75) + 0.25*(small.lobes(u, v) - 0.75);
			float* texel = tile.data() + (static_cast<std::size_t>(j)*n + static_cast<std::size_t>(i))*4;
			texel[0] = static_cast<float>(std::clamp(shape, -0.35, 1.5));
			texel[1] = static_cast<float>(0.2 + 0.4*which);
			texel[2] = static_cast<float>(0.65*softNoise(u*5., v*5., 5, 6) + 0.35*softNoise(u*11., v*11., 11, 7));
			texel[3] = static_cast<float>(fine.lobes(u, v));
		}
	return tile;
}

double CloudShape::coverOffset(double cover, double noise)
{
	const double fading = std::max(0.1 - cover, 0.);
	return 1.1*(cover - 0.45) + 2.*(noise - 0.5) - 150.*fading*fading;
}

double CloudShape::transmission(const std::vector<float>& tile, double cover, double size, const Motion& motion, double observerRadius, const std::array<double, 3>& altAz,
                                const std::array<double, 3>& south, const std::array<double, 3>& east, const std::array<double, 3>& up)
{
	if (altAz[2] < -0.015)
		return 1.;
	// as in the shader of CloudMgr, without the fine lobes
	const double z = altAz[2] < 0.008 ? 0.008*std::exp(altAz[2]/0.008 - 1.) : altAz[2];
	const double base = EarthRadius + BaseHeight, top = base + LayerThickness*size;
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
		const double mapX = motion.cosine*scale*ex - motion.sine*scale*ey + motion.offsetX;
		const double mapY = motion.sine*scale*ex + motion.cosine*scale*ey + motion.offsetY;
		const double noise = 0.6*readTile(tile, mapX/CoverScaleLarge, mapY/CoverScaleLarge, 2) + 0.4*readTile(tile, mapX/CoverScaleSmall, mapY/CoverScaleSmall, 2);
		const double tileX = mapX/(TileSize*size), tileY = mapY/(TileSize*size);
		const double grow = readTile(tile, mapX/GrowScale + motion.growU, mapY/GrowScale + motion.growV, 2);
		const double shape = readTile(tile, tileX, tileY, 0) + std::min(coverOffset(cover, noise) + GrowStrength*(grow - 0.5), 0.34);
		const double height = size*readTile(tile, tileX, tileY, 1);
		const double topHeight = height*shape/std::pow(std::fabs(shape) + 0.01, 0.3);
		const double rim = 1. - std::clamp(shape/0.35, 0., 1.);
		const double under = 0.3*height*rim*rim;
		const double level = r - base;
		const double e = std::clamp((std::min(topHeight - level, level - under) + 0.012)/(0.024 + 0.05*size), 0., 1.);
		depth += Extinction*ds*e*e*(3. - 2.*e);
		if (depth > 7.)
			return 0.;
	}
	return std::exp(-depth);
}
