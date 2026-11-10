/* Stellarium cloud animation regression tests. GPL-2.0-or-later. */
#include "../core/modules/CloudShape.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace
{
void require(bool condition, const char* message)
{
	if (!condition) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
double distance(const CloudShape::Motion& a, const CloudShape::Motion& b)
{
	return std::hypot(a.offsetX - b.offsetX, a.offsetY - b.offsetY);
}
}

int main()
{
	const double second = 1./86400.;
	// Both sides of old resets, including dates before the epoch and before JD zero.
	for (double boundary : {-10., 2451540., 2451550., 2461360.})
	{
		const auto a = CloudShape::manualMotion(boundary - second, 0.008, -0.004);
		const auto b = CloudShape::manualMotion(boundary + second, 0.008, -0.004);
		require(std::abs(distance(a, b) - 2.*std::hypot(0.008, 0.004)) < 1e-5, "Manual wind jumps at ten-day boundary");
		const auto c = CloudShape::heightMotion(boundary - second, 3700., 1800., 1.6, -0.2);
		const auto d = CloudShape::heightMotion(boundary + second, 3700., 1800., 1.6, -0.2);
		require(distance(c, d) < 0.2, "Upper layer jumps at ten-day boundary");
	}
	const auto tile = CloudShape::computeTile();
	CloudShape::Layer layer;
	for (double cycle : {-100., -1., 0., 1., 100.})
	{
		const double boundary = 2451545. + cycle*CloudShape::GrowScale/(0.0025*86400.);
		const auto a = CloudShape::motion(boundary - second), b = CloudShape::motion(boundary + second);
		require(std::abs(std::remainder(a.growU - b.growU, 1.)) < 0.001, "Growth U jumps");
		require(std::abs(std::remainder(a.growV - b.growV, 1.)) < 0.001, "Growth V jumps");
		const auto left = CloudShape::development(tile, layer, a, 3.7, -12.3);
		const auto right = CloudShape::development(tile, layer, b, 3.7, -12.3);
		require(std::abs(left[0] - right[0]) < 0.01, "Cloud lifecycle jumps at phase wrap");
		const double boilBoundary = 2451545. + cycle*2.*CloudShape::FineScale/(0.001*86400.);
		const auto c = CloudShape::motion(boilBoundary - second), d = CloudShape::motion(boilBoundary + second);
		require(std::hypot(c.boilX - d.boilX, c.boilY - d.boilY) < 0.003, "Fine detail jumps at old wrap");
	}
	const auto now = CloudShape::motion(2461360.25), later = CloudShape::motion(2461360.25 + 20./1440.);
	double nearby = 0., distant = 0., evolving = 0., flatEvolving = 0.;
	for (int i = 0; i < 256; ++i)
	{
		const double x = i*1.31 - 160., y = i*3.17 - 400.;
		layer.flatness = 0.;
		const auto a = CloudShape::development(tile, layer, now, x, y);
		const auto b = CloudShape::development(tile, layer, now, x + 0.1, y);
		const auto c = CloudShape::development(tile, layer, now, x + 20., y);
		const auto d = CloudShape::development(tile, layer, later, x, y);
		require(a[1] >= 0.65 && a[1] <= 1. && a[2] >= 0.45 && a[2] <= 1., "Lifecycle exceeds layer bounds");
		require(a == CloudShape::development(tile, layer, CloudShape::motion(2461360.25), x, y), "Date replay is not deterministic");
		nearby += std::abs(a[0] - b[0]); distant += std::abs(a[0] - c[0]); evolving += std::abs(a[2] - d[2]);
		layer.flatness = 0.6;
		flatEvolving += std::abs(CloudShape::development(tile, layer, now, x, y)[2] - CloudShape::development(tile, layer, later, x, y)[2]);
	}
	require(nearby < 0.15*distant, "Cloud development lacks spatial coherence");
	require(evolving > 1. && flatEvolving < 0.7*evolving, "Lifecycle is static or flat layers change too strongly");
	for (double flat : {0., 0.4, 0.6}) for (double cover : {0., 0.25, 0.6, 1.}) for (double elevation : {0.01, 0.2, 0.7, 1.5})
	{
		layer.flatness = flat; layer.cover = cover;
		const double t = CloudShape::transmission(tile, layer, now, now, 6371., {std::cos(elevation), 0., std::sin(elevation)}, {1., 0., 0.}, {0., 1., 0.}, {0., 0., 1.});
		require(std::isfinite(t) && t >= 0. && t <= 1., "Transmission out of range");
		if (cover == 0.) require(t == 1., "Clear sky develops clouds");
	}
	// Geometric Sun directions in the same south/east/up axes used by the renderer.
	const auto sun = [](double latitude, double declination, double hourAngle) {
		return std::array<double, 3>{std::sin(latitude)*std::cos(declination)*std::cos(hourAngle) - std::cos(latitude)*std::sin(declination),
		                            -std::cos(declination)*std::sin(hourAngle),
		                            std::sin(latitude)*std::sin(declination) + std::cos(latitude)*std::cos(declination)*std::cos(hourAngle)};
	};
	const double latitude = 60.*M_PI/180., summer = 23.44*M_PI/180.;
	const double morning = CloudShape::solarHeating(latitude, sun(latitude, summer, -M_PI/4.));
	const double afternoon = CloudShape::solarHeating(latitude, sun(latitude, summer, M_PI/4.));
	require(afternoon > morning, "Solar response has no afternoon lag");
	require(CloudShape::solarHeating(latitude, sun(latitude, summer, 0.)) > CloudShape::solarHeating(latitude, sun(latitude, -summer, 0.)), "Solar response ignores season");
	const double polar = 70.*M_PI/180.;
	require(CloudShape::solarHeating(polar, sun(polar, -summer, 0.)) == 0., "Polar night produces solar heating");
	require(CloudShape::solarHeating(polar, sun(polar, summer, M_PI)) > 0., "Midnight sun loses its heating");
	require(CloudShape::solarHeating(0., sun(0., 0., M_PI)) < 0.001, "Equatorial midnight retains daytime heating");
	require(CloudShape::solarHeating(0., sun(0., 0., M_PI/2.)) > 0.1, "Heating vanishes instantly at sunset");
	for (int i = -24; i <= 24; ++i)
	{
		const double angle = i*M_PI/12.;
		const double a = CloudShape::solarHeating(latitude, sun(latitude, summer, angle));
		const double b = CloudShape::solarHeating(latitude, sun(latitude, summer, angle + 2.*M_PI*second));
		require(std::isfinite(a) && a >= 0. && a <= 1. && std::abs(a - b) < 0.001, "Solar heating is discontinuous");
	}
	layer.flatness = 0.; layer.base = 1.2;
	const double nightCumulus = CloudShape::solarCoverScale(layer, 0.);
	layer.flatness = 0.6; layer.base = 3.5;
	require(nightCumulus >= 0.69 && CloudShape::solarCoverScale(layer, 0.) > nightCumulus, "Flat layers respond too strongly to solar heating");
	double deformationChange = 0.;
	for (int i = 0; i < 128; ++i)
	{
		const double x = 1.31*i, y = -2.17*i;
		const auto a = CloudShape::deformation(tile, layer, now, x, y);
		const auto b = CloudShape::deformation(tile, layer, now, x + 0.01, y);
		const auto c = CloudShape::deformation(tile, layer, later, x, y);
		require(a == CloudShape::deformation(tile, layer, CloudShape::motion(2461360.25), x, y), "Deformation replay is not deterministic");
		for (int axis = 0; axis < 2; ++axis)
		{
			require(std::abs(a[axis]) <= 0.15*layer.size, "Deformation is too large");
			require(std::abs(a[axis] - b[axis]) < 0.003, "Deformation introduces rough edges");
			deformationChange += std::abs(a[axis] - c[axis]);
		}
	}
	require(deformationChange > 0.01, "Deformation does not evolve");
	for (double cycle : {-1., 0., 1., 100.})
	{
		const double boundary = 2451545. + cycle*CloudShape::DeformScale/(0.001*86400.);
		const auto a = CloudShape::deformation(tile, layer, CloudShape::motion(boundary - second), 11., -17.);
		const auto b = CloudShape::deformation(tile, layer, CloudShape::motion(boundary + second), 11., -17.);
		require(std::hypot(a[0] - b[0], a[1] - b[1]) < 0.001, "Deformation jumps at phase wrap");
	}

	CloudShape::Layer cumulus, stratocumulus, altocumulus;
	cumulus.base = 1.2; cumulus.flatness = 0.;
	stratocumulus.base = 1.; stratocumulus.flatness = 0.4;
	altocumulus.base = 3.5; altocumulus.flatness = 0.6;
	const auto cu = CloudShape::formationParameters(cumulus);
	const auto sc = CloudShape::formationParameters(stratocumulus);
	const auto ac = CloudShape::formationParameters(altocumulus);
	require(sc[0] < cu[0] && sc[1] < cu[1], "Stratocumulus does not form broader banks");
	require(ac[0] > cu[0] && ac[1] > cu[1], "Altocumulus does not form smaller patches");
	require(cu[3] == 0. && sc[3] == 0. && ac[3] > 0., "Bands affect the wrong cloud types");
	double typeDifference = 0.;
	for (int i = 0; i < 128; ++i)
	{
		const double x = i*1.31 - 70., y = i*1.79 - 90.;
		const auto a = CloudShape::development(tile, cumulus, now, x, y);
		const auto b = CloudShape::development(tile, altocumulus, now, x, y);
		typeDifference += std::abs(a[0] - b[0]);
		for (double base : {2., 2.75, 3.5})
		{
			layer = altocumulus; layer.base = base - 1e-5;
			const auto before = CloudShape::development(tile, layer, now, x, y);
			layer.base = base + 1e-5;
			const auto after = CloudShape::development(tile, layer, now, x, y);
			require(std::abs(before[0] - after[0]) < 0.001, "Formation jumps when layer height changes");
		}
	}
	require(typeDifference > 1., "Cloud types have indistinguishable grouping");


	// Group evolution must be continuous when the stored deformation phase wraps,
	// even with local growth held fixed. It should reorganize low clouds over hours.
	for (const auto& kind : {cumulus, stratocumulus, altocumulus})
	{
		double groupChange = 0.;
		auto start = now, evolved = now;
		evolved.deformU += 0.19; evolved.deformV -= 0.11;
		for (int i = 0; i < 128; ++i)
		{
			const double x = i*1.31 - 70., y = i*1.79 - 90.;
			const auto a = CloudShape::development(tile, kind, start, x, y);
			const auto b = CloudShape::development(tile, kind, evolved, x, y);
			groupChange += std::abs(a[0] - b[0]);
			for (int axis = 0; axis < 2; ++axis)
			{
				auto left = now, right = now;
				if (axis == 0) {left.deformU = 0.5 - 1e-8; right.deformU = -0.5 + 1e-8;}
				else {left.deformV = 0.5 - 1e-8; right.deformV = -0.5 + 1e-8;}
				const auto c = CloudShape::development(tile, kind, left, x, y);
				const auto d = CloudShape::development(tile, kind, right, x, y);
				for (int channel = 0; channel < 3; ++channel)
					require(std::abs(c[channel] - d[channel]) < 1e-6, "Bank evolution jumps at phase wrap");
			}
		}
		require(kind.flatness >= 0.6 ? groupChange == 0. : groupChange > 0.1,
		        "Bank evolution affects wrong cloud types or remains static");
	}

	// Automatic cumulus coverage is reduced without changing the solar cycle or
	// the established sheet-cloud presets. This multiplier is shared with the GPU
	// through Front::scale, and is not used by manual coverage.
	CloudShape::Layer automatic;
	automatic.base = 1.2; automatic.flatness = 0.;
	for (double heat : {0., 0.5, 1.})
	{
		const double solar = CloudShape::solarCoverScale(automatic, heat);
		const double weather = CloudShape::weatherCoverScale(automatic, heat);
		require(std::abs(weather - 0.7*solar) < 1e-12, "Automatic cumulus reduction is inconsistent");
		require(CloudShape::FrontMax*weather <= 0.63 + 1e-12, "Automatic cumulus exceeds coverage ceiling");
		for (double flat : {0.4, 0.6, 1.})
		{
			auto sheet = automatic; sheet.flatness = flat;
			require(CloudShape::weatherCoverScale(sheet, heat) == CloudShape::solarCoverScale(sheet, heat),
			        "Automatic sheet-cloud coverage changed");
		}
	}
	for (double flat : {0.15, 0.4})
	{
		automatic.flatness = flat - 1e-6;
		const double left = CloudShape::weatherCoverScale(automatic, 0.5);
		automatic.flatness = flat + 1e-6;
		require(std::abs(left - CloudShape::weatherCoverScale(automatic, 0.5)) < 1e-6,
		        "Automatic coverage jumps between cloud types");
	}

	// Date weather selects occupied regions, leaving real clear gaps while keeping
	// full density within selected groups. Manual and sheet-cloud weather is unchanged.
	CloudShape::Layer populationLayer;
	populationLayer.base = 1.2; populationLayer.flatness = 0.;
	int emptyGroups = 0, fullGroups = 0;
	for (int i = 0; i < 1024; ++i)
	{
		const double x = i*1.17 - 500., y = i*3.19 - 1200.;
		populationLayer.front.on = false;
		const auto manual = CloudShape::development(tile, populationLayer, now, x, y);
		populationLayer.front.on = true;
		const auto dated = CloudShape::development(tile, populationLayer, now, x, y);
		require(dated[0] == manual[0] && dated[1] == manual[1], "Population mask changes body shape or height");
		require(dated[2] >= 0. && dated[2] <= manual[2], "Population mask amplifies density");
		emptyGroups += dated[2] == 0.; fullGroups += dated[2] == manual[2];
		for (double flat : {0.4, 0.6})
		{
			auto sheet = populationLayer; sheet.flatness = flat;
			const auto automatic = CloudShape::development(tile, sheet, now, x, y);
			sheet.front.on = false;
			require(automatic == CloudShape::development(tile, sheet, now, x, y), "Population mask changes sheet clouds");
		}
	}
	require(emptyGroups > 100 && fullGroups > 20, "Automatic cumulus lack clear gaps or intact groups");

	// Cumulus shrink at low coverage while retaining density. Only the last
	// remnants thin out; the retained core must never leak into clear skies.
	const auto mature = CloudShape::cohesiveWeather(-0.20, 0.8, 0.7, 0., 0.25);
	const auto fading = CloudShape::cohesiveWeather(-0.8, 0.8, 0.7, 0., 0.25);
	require(fading[0] < mature[0] && std::abs(fading[2] - mature[2]) < 1e-12,
	        "Small cumulus lose density before shrinking");
	const auto remnant = CloudShape::cohesiveWeather(-1., 0.8, 0.7, 0., 0.25);
	const auto dissolving = CloudShape::cohesiveWeather(-1.2, 0.8, 0.7, 0., 0.25);
	require(std::abs(remnant[0] - dissolving[0]) < 1e-12 && dissolving[2] < remnant[2],
	        "Last cumulus remnants do not dissolve smoothly");
	for (double offset : {-2., -1.25, -0.8, -0.35, -0.20, 0., 0.45})
	{
		const auto clear = CloudShape::cohesiveWeather(offset, 0.8, 0.7, 0., 0.);
		require(clear[2] == 0., "Coherent core leaks into clear skies");
		const auto a = CloudShape::cohesiveWeather(offset - 1e-6, 0.8, 0.7, 0., 0.25);
		const auto b = CloudShape::cohesiveWeather(offset + 1e-6, 0.8, 0.7, 0., 0.25);
		require(std::abs(a[0]-b[0]) < 3e-6 && std::abs(a[2]-b[2]) < 3e-6, "Coherent fading is discontinuous");
	}

	std::puts("Cloud behaviour: continuous motion, deterministic replay, distinct formations, coherent groups, solar response, smooth deformation and transmission PASS");
}
