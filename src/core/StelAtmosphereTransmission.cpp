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

#include "StelAtmosphereTransmission.hpp"

#include <QFile>
#include <QString>
#include <QDebug>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

bool StelAtmosphereTransmission::load(const QString& path)
{
	depths.clear();
	weights.clear();
	QFile file(path);
	if (!file.open(QFile::ReadOnly))
		return false;
	const QByteArray bytes = file.readAll();
	if (!parse(bytes.constData(), static_cast<std::size_t>(bytes.size())))
	{
		depths.clear();
		weights.clear();
		qWarning() << "Atmosphere transmission table is malformed:" << path;
		return false;
	}
	return true;
}

bool StelAtmosphereTransmission::parse(const char* bytes, std::size_t size)
{
	if (!bytes || size < 32 || std::memcmp(bytes, "AtmoExt\0", 8) != 0)
		return false;
	std::size_t pos = 8;
	const auto integer = [&]() {
		const auto p = reinterpret_cast<const unsigned char*>(bytes + pos);
		pos += 4;
		return uint32_t(p[0]) | uint32_t(p[1])<<8 | uint32_t(p[2])<<16 | uint32_t(p[3])<<24;
	};
	const auto real = [&]() { const uint32_t bits = integer(); float f; std::memcpy(&f, &bits, 4); return f; };
	if (integer() != 1)
		return false;
	width = integer();
	height = integer();
	count = integer();
	radius = real();
	thickness = real();
	if (width < 2 || width > 4096 || height < 2 || height > 4096 || count < 1 || count > 256 ||
	    !std::isfinite(radius) || !std::isfinite(thickness) || radius <= 0. || thickness <= 0.)
		return false;
	const std::size_t samples = std::size_t(width)*height*count;
	if (size != 32 + 4*(3*std::size_t(count) + samples))
		return false;
	weights.resize(count);
	for (auto& weight : weights)
		for (auto& component : weight)
		{
			component = real();
			if (!std::isfinite(component))
				return false;
		}
	depths.resize(samples);
	for (auto& depth : depths)
	{
		depth = real();
		if (!std::isfinite(depth) || depth < 0.f)
			return false;
	}
	return true;
}

double StelAtmosphereTransmission::horizonSine(double altitude) const
{
	if (!isLoaded())
		return 0.;
	const double h = std::clamp(altitude, 0., thickness);
	const double r = radius + h;
	const double rho = std::sqrt(h*(h + 2.*radius));
	return -rho/r;
}

Vec3d StelAtmosphereTransmission::transmission(double altitude, double elevation, double depthScale) const
{
	if (!isLoaded() || !std::isfinite(altitude) || !std::isfinite(elevation))
		return Vec3d(1.);
	const double h = std::clamp(altitude, 0., thickness);
	const double r = radius + h;
	const double rho = std::sqrt(h*(h + 2.*radius));
	const double horizon = std::sqrt(thickness*(thickness + 2.*radius));
	const double mu = std::sin(std::clamp(elevation, -M_PI_2, M_PI_2));
	if (mu < -rho/r)
		return Vec3d(0.);
	// Distance to the top of the atmosphere along the ray, and its range for this altitude
	const double dMin = thickness - h;
	const double d = std::sqrt(r*r*mu*mu + dMin*(2.*r + dMin)) - r*mu;
	const double x = std::clamp((d - dMin)/(horizon + rho - dMin), 0., 1.)*(width - 1);
	const double y = std::clamp(rho/horizon, 0., 1.)*(height - 1);
	const unsigned x0 = static_cast<unsigned>(x), y0 = static_cast<unsigned>(y);
	const unsigned x1 = std::min(x0 + 1, width - 1), y1 = std::min(y0 + 1, height - 1);
	const double fx = x - x0, fy = y - y0;
	double sum[3] = {0., 0., 0.};
	for (unsigned n = 0; n < count; ++n)
	{
		const auto at = [&](unsigned xx, unsigned yy) { return static_cast<double>(depths[(std::size_t(yy)*width + xx)*count + n]); };
		const double lo = at(x0, y0)*(1. - fx) + at(x1, y0)*fx;
		const double hi = at(x0, y1)*(1. - fx) + at(x1, y1)*fx;
		const double t = std::exp(-depthScale*std::max(0., lo*(1. - fy) + hi*fy));
		for (unsigned c = 0; c < 3; ++c)
			sum[c] += static_cast<double>(weights[n][c])*t;
	}
	return Vec3d(std::clamp(sum[0], 0., 1.), std::clamp(sum[1], 0., 1.), std::clamp(sum[2], 0., 1.));
}

double StelAtmosphereTransmission::zenithMagnitude(double altitude) const
{
	const Vec3d t = transmission(altitude, M_PI_2);
	const double Y = 0.2126*t[0] + 0.7152*t[1] + 0.0722*t[2];
	return Y > 0. ? -2.5*std::log10(Y) : 0.;
}
