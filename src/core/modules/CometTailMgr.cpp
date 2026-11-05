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

#include "CometTailMgr.hpp"
#include "Comet.hpp"
#include "SolarSystem.hpp"
#include "StelApp.hpp"
#include "StelCore.hpp"
#include "StelModuleMgr.hpp"
#include "StelObjectMgr.hpp"
#include "StelPainter.hpp"
#include "StelProjector.hpp"
#include "StelTranslator.hpp"
#include "StelSkyDrawer.hpp"
#include "StelOpenGL.hpp"
#include "RefractionExtinction.hpp"

#include <QSettings>
#include <QOpenGLBuffer>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QVector4D>
#include <cmath>
#include <array>
#include <algorithm>

namespace
{
// Stumpff functions C(z) and S(z)
void stumpff(double z, double& C, double& S)
{
	if (z > 1e-6)
	{
		const double s = std::sqrt(z);
		C = (1. - std::cos(s))/z;
		S = (s - std::sin(s))/(s*s*s);
	}
	else if (z < -1e-6)
	{
		const double s = std::sqrt(-z);
		C = (std::cosh(s) - 1.)/(-z);
		S = (std::sinh(s) - s)/(s*s*s);
	}
	else
	{
		C = 0.5 - z/24. + z*z/720.;
		S = 1./6. - z/120. + z*z/5040.;
	}
}

// The grains of the guide lines
constexpr std::array<double, 6> SyndyneBetas = {1., 0.3, 0.1, 0.03, 0.01, 0.003};
constexpr std::array<double, 6> SynchroneAges = {2., 5., 10., 20., 40., 80.}; // days before the observation
constexpr double MaxAge = 90.;          // days: the oldest grains of the syndynes
constexpr int SyndynePoints = 120;
constexpr int SynchronePoints = 40;
constexpr double IonTailLength = 0.5;   // AU
constexpr double GuideMagnitude = 8.;   // comets brighter than this get guide lines (and the selected one)
constexpr double MaxDistance = 5.;      // AU from the Sun: no tails beyond

// The light of the comets (photographic mode)
constexpr double LightMagnitude = 10.;  // comets brighter than this get their light from this module
constexpr int BetaCount = 40;           // grains: beta from 0.001 to 1 (logarithmic, see TailModel) ...
constexpr int AgeCount = 120;           // ... and ages up to MaxGrainAge (closer where the comet turns fast, see releaseAges())
constexpr double MaxGrainAge = 150.;    // days
constexpr double DefaultBetaMin = 0.0001, DefaultBetaMax = 1.;
// The large grains (beta below 0.01, sizes above some 50 micrometers): a flatter size distribution (n(s) ~ s^-3, as
// much cross section in each decade of size), and ages up to some three years
constexpr double SizeBreakBeta = 0.01;
constexpr double LargeSizeIndex = 3.;
constexpr double LargeGrainAge = 1000.; // days
constexpr double LargeProductionIndex = 2.;
// The size distribution of the grains, n(s) ~ s^-4: the light of the tails comes from grains of a few micrometers
// (beta 0.1-0.7 in the tail of NEOWISE, sizes 1.7-12 micrometers: Manzini et al. 2021); s^-3.5 gives the large,
// slow grains so much of the light of the coma that the tails become far too faint
constexpr double DefaultSizeIndex = 4.;
constexpr double KmPerAU = 1.495978707e8;
// Speed of ejection of the grains: v0 sqrt(beta) / sqrt(r) (r in AU), about 0.55 km/s for the smallest grains at
// 1 AU (Whipple 1951 scaled to the observed expansion of dust envelopes)
constexpr double EjectionSpeed = 0.55*86400./KmPerAU; // AU/day
constexpr double DustFraction = 0.5;    // share of the dust in the light of the coma (visual)
constexpr double DustAperture = 1e5/KmPerAU;   // the coma in which the dust gives its share of the magnitude (AU)
constexpr double GasScaleLength = 8e4/KmPerAU; // Haser scale length of C2 at 1 AU (AU, grows with r^2)
constexpr double IlluminanceMag0 = 2.54e-6;    // lux of a star of magnitude 0 (V)
// The Sun at 1 AU (V = -26.74, lux)
const double SunIlluminance = IlluminanceMag0*std::pow(10., 0.4*26.74);
constexpr double CmPerAU = KmPerAU*1e5;
constexpr double EarthRadius = 6371e3;   // m, for the dip of the horizon
constexpr int FloatsPerVertex = 11;     // position 2, center 2, inverse 3, color 3, profile 1
constexpr double SplatFloor = 2e-5;     // light in the scene (1 is white) below which a splat is not drawn

// The striae: dust released in bursts, which lie along the synchrones in the tail (West 1976, McNaught 2007)
constexpr double StriaePeriod = 2.;     // days: the bursts come and go at about this rate
constexpr double DefaultStriae = 0.3;   // their strength (0: an even release)

// The colors (linear sRGB, luminance 1): sunlight reddened a little by the dust; the Swan bands of C2 with CN and
// the continuum, as cameras record comae (cyan-green)
const Vec3f DustColor(1.07f, 0.99f, 0.89f);
const Vec3f GasColor(0.2f/0.8065f, 1.f/0.8065f, 0.6f/0.8065f);
// The comet tail bands of CO+ (about 380-480 nm, strongest at 401, 427 and 456 nm) brought into the gamut with
// their luminance (deep blue, a little violet), as cameras record them (their blue channel reaches into the green)
const Vec3f IonColor(0.53f, 0.43f, 8.0f);
// The D lines of sodium (589.0 and 589.6 nm): orange-yellow, from the color matching functions (blue clipped)
const Vec3f SodiumColor(2.743f, 0.583f, 0.f);
// The same emissions on the color films, as their layers record them: the linear sRGB of the scene (as for
// StelSkyDrawer::PhotoFilmSpec::aurora, through its rgbToFilm) per unit luminance of the emission, from the
// spectra (CO+ comet tail bands with a little H2O+; C2 Swan bands with C3, CN and NH2; the D lines) and the
// spectral sensitivities of the films (see StelSkyDrawerPhotoFilm.cpp). The negative parts are clipped and the
// colors scaled as the digital ones above (by the luminance of the clipped CIE color). Provia sees CO+ as a pure
// deep blue, the coma a little bluer than the digital sensor (Royal Gold: cyan), sodium yellow-green with its
// green layer (Ektachrome and Royal Gold dimmer, orange-red: 589 nm falls between their green and red layers).
struct FilmEmissions {std::array<float, 9> colors;}; // ion, gas, sodium
constexpr std::array<FilmEmissions, 3> FilmColors = {{
	{{0.000f, 0.000f, 10.123f, 0.116f, 1.044f, 0.511f, 0.334f, 1.347f, 0.064f}},  // Provia 100F (and 400F)
	{{0.000f, 0.000f, 10.542f, 0.090f, 1.142f, 0.678f, 0.405f, 0.223f, 0.027f}},  // Ektachrome (E200)
	{{0.000f, 0.000f, 8.539f, 0.175f, 1.160f, 1.014f, 0.602f, 0.132f, 0.000f}},   // Ultramax (Royal Gold 400)
}};

// The colors of the emissions as the camera in use records them
struct Emissions {Vec3f ion, gas, sodium;};
Emissions emissionColors(const StelSkyDrawer* drawer)
{
	Emissions e = {IonColor, GasColor, SodiumColor};
	if (!drawer->getFlagPhotoFilmActive() || !StelSkyDrawer::getPhotoFilmSpectral(drawer->getPhotoActiveFilmSpec()))
		return e;
	int k;
	switch (drawer->getPhotoFilm())
	{
		case StelSkyDrawer::PhotoFilmProvia100F:
		case StelSkyDrawer::PhotoFilmProvia400F:
			k = 0;
			break;
		case StelSkyDrawer::PhotoFilmEktachromeE200:
			k = 1;
			break;
		case StelSkyDrawer::PhotoFilmRoyalGold400:
			k = 2;
			break;
		default:
			return e;
	}
	const std::array<float, 9>& c = FilmColors[k].colors;
	e.ion.set(c[0], c[1], c[2]);
	e.gas.set(c[3], c[4], c[5]);
	e.sodium.set(c[6], c[7], c[8]);
	return e;
}

// The ion tail: ions of CO+ picked up by the solar wind, accelerated from about 10 km/s to some 200 km/s within a
// few tenths of a day, glowing for a day or two
constexpr double IonFraction = 0.05;    // share of the luminance of the comet in its ion tail
constexpr double IonStartSpeed = 10.*86400./KmPerAU;   // AU/day
constexpr double IonFinalSpeed = 200.*86400./KmPerAU;  // AU/day
constexpr double IonAcceleration = 0.3; // days to reach the final speed (e-folding)
constexpr double IonLifetime = 4.;      // days (the tail fades with this age)
constexpr double IonMaxAge = 12.;       // days
constexpr int IonAgeCount = 140;
constexpr double IonWidth = 5e4/KmPerAU;  // width of the tail at the head (AU) ...
constexpr double IonOpening = 0.06;       // ... growing with the distance from the head
// Streamers: the main tail and rays at angles to it that fold onto the axis
struct IonStream {double angle, azimuth, strength, width, period;};
constexpr std::array<IonStream, 7> IonStreams = {{
	{0., 0., 1., 1., 3.},
	{5., 0.3, 0.35, 0.4, 5.},
	{-6., 0.1, 0.3, 0.4, 7.},
	{11., -0.2, 0.25, 0.3, 4.},
	{-13., 0.4, 0.2, 0.3, 6.},
	{19., 0.1, 0.15, 0.25, 9.},
	{-22., -0.3, 0.12, 0.25, 8.},
}};
constexpr double IonFolding = 0.12;     // days for a ray to fold onto the axis (e-folding)

// The sodium tail: neutral atoms pushed away from the Sun by the pressure of the light they scatter in the D lines,
// up to some 0.5 m/s^2 at 1 AU (beta about 85; Cremonese et al. 1997 found 82 for Hale-Bopp), much less when they
// move slowly to or from the Sun, as the D lines of the sunlight they see are dark (Swings effect: a Doppler shift
// takes them out of the Fraunhofer lines); ionized by the sunlight after about two days at 1 AU (Cremonese et al.)
constexpr double SodiumFraction = 0.003;    // share of the light of the comet at 1 AU (grows as r^-2) ...
constexpr double SodiumMaxFraction = 0.3;   // ... up to this
constexpr double SodiumAcceleration = 0.5e-3*86400.*86400./KmPerAU; // AU/day^2 at 1 AU (r^-2)
constexpr double SodiumLifetime = 1.7e5/86400.; // days at 1 AU (r^2)
constexpr double SwingsDepth = 0.95;        // the D lines of the Sun: their depth ...
constexpr double SwingsWidth = 12.*86400./KmPerAU; // ... and half width (AU/day of radial speed)
constexpr double SodiumWidth = 1e4/KmPerAU; // width of the tail at the head (AU) ...
constexpr double SodiumSpeed = 1.*86400./KmPerAU; // ... growing with the speed of the atoms across it (AU/day)
constexpr int SodiumAgeCount = 120;
constexpr int SodiumSteps = 400;            // steps of the integration of the motion of the atoms

// Smooth noise in [0, 1] of a time (in units of its scale), for the changes of the solar wind
double noise(double x, int seed)
{
	const auto hash = [seed](double i) {
		const double h = std::sin(i*127.1 + seed*311.7)*43758.5453;
		return h - std::floor(h);
	};
	const double i = std::floor(x), f = x - i;
	const double u = f*f*(3. - 2.*f);
	return hash(i)*(1. - u) + hash(i + 1.)*u;
}

// Grains evaporate close to the Sun: refractory dust survives from about 4 solar radii, most of it from 10
constexpr double SublimationInner = 0.02, SublimationOuter = 0.05; // AU
double survival(double r)
{
	const double x = std::clamp((r - SublimationInner)/(SublimationOuter - SublimationInner), 0., 1.);
	return x*x*(3. - 2.*x);
}

double smoothstep(double edge0, double edge1, double x)
{
	const double u = std::clamp((x - edge0)/(edge1 - edge0), 0., 1.);
	return u*u*(3. - 2.*u);
}

// The production of dust (relative to 1 AU) with the distance from the Sun: by default r^-4.5, steeper than the
// Afrho of NEOWISE after perihelion (r^-3.1, a production of about r^-3.6) and less steep than its brightening
// before it (r^-5.4 to r^-6.8 in SOHO, Battams et al., ATel 13853): the tails come mostly from the dust released
// around perihelion. From 4 AU on it falls much faster; closer than a few solar radii the grains evaporate.
constexpr double DefaultProductionIndex = 4.5;
double dustProduction(double r, double index)
{
	return std::pow(std::max(r, 0.02), -index)*(r > 4. ? std::exp(-(r - 4.)) : 1.)*survival(r);
}

// The parameters of the model for a comet: its own (ssystem_minor.ini) or the defaults
struct TailModel
{
	double dustFraction, afrho, betaMin, betaMax, sizeIndex, productionIndex, striae, ionFraction, sodiumFraction;
	int seed;  // of the noise of the striae and the solar wind
};
TailModel tailModel(const Comet* comet)
{
	const Comet::TailParameters& p = comet->getTailParameters();
	const auto value = [](double v, double fallback) {return v >= 0. ? v : fallback;};
	TailModel m;
	m.dustFraction = std::clamp(value(p.dustFraction, DustFraction), 0., 1.);
	m.afrho = p.afrho > 0. ? p.afrho : -1.;
	// the grains on Kepler orbits: beta at most 1 (no force)
	// (beta above 1: small absorbing grains pushed away from the Sun, see propagate())
	m.betaMax = std::clamp(value(p.betaMax, DefaultBetaMax), 1e-5, 3.);
	m.betaMin = std::clamp(value(p.betaMin, DefaultBetaMin), 1e-6, 1.);
	if (!(m.betaMin < m.betaMax*0.99))
		m.betaMin = m.betaMax*0.1;
	m.sizeIndex = std::clamp(value(p.sizeIndex, DefaultSizeIndex), 2., 5.);
	m.productionIndex = std::clamp(value(p.productionIndex, DefaultProductionIndex), 0., 8.);
	m.striae = std::clamp(value(p.striae, DefaultStriae), 0., 2.);
	m.ionFraction = value(p.ionFraction, IonFraction);
	m.sodiumFraction = value(p.sodiumFraction, SodiumFraction);
	m.seed = static_cast<int>(qHash(comet->getEnglishName()) % 1000u);
	return m;
}

// The ages (days, with the span each stands for) at which grains are released: closer together where the comet
// turns fast around the Sun (near the perihelion, above all of a sungrazer), where grains released a little apart
// in time go very different ways
void releaseAges(const Comet* comet, double t, double maxAge, int count, std::vector<double>& ages, std::vector<double>& dAges)
{
	constexpr int Fine = 1024;
	constexpr double TurnWeight = 0.5;  // the share of a half turn around the Sun against the whole span
	std::array<double, Fine + 1> tau, measure;
	measure[0] = 0.;
	double previousRate = 0.;
	for (int f = 0; f <= Fine; ++f)
	{
		const double u = static_cast<double>(f)/Fine;
		tau[f] = maxAge*u*u;
		Vec3d pos, vel;
		comet->computePosition(t - tau[f], pos, vel);
		const double rate = (pos^vel).norm()/(pos*pos); // radians per day around the Sun
		if (f > 0)
			measure[f] = measure[f - 1] + 1./Fine + TurnWeight*0.5*(rate + previousRate)*(tau[f] - tau[f - 1])/M_PI;
		previousRate = rate;
	}
	const auto ageAt = [&](double m) {
		const auto it = std::lower_bound(measure.begin(), measure.end(), m);
		const int f = std::clamp(static_cast<int>(it - measure.begin()), 1, Fine);
		const double x = (m - measure[f - 1])/std::max(measure[f] - measure[f - 1], 1e-300);
		return tau[f - 1] + std::clamp(x, 0., 1.)*(tau[f] - tau[f - 1]);
	};
	ages.resize(count);
	dAges.resize(count);
	const double total = measure[Fine];
	for (int k = 0; k < count; ++k)
	{
		ages[k] = ageAt(total*(k + 0.5)/count);
		dAges[k] = ageAt(total*(k + 1.)/count) - ageAt(total*static_cast<double>(k)/count);
	}
}

// Distance (AU) an ion has travelled in the frame of the comet after the time age (days)
double ionDistance(double age)
{
	return IonStartSpeed*age + (IonFinalSpeed - IonStartSpeed)*(age - IonAcceleration*(1. - std::exp(-age/IonAcceleration)));
}

// The phase function of cometary dust (Marcus 2007): two Henyey-Greenstein terms, forward (g 0.9) and backward
// (g -0.6), as a function of the phase angle (radians)
double dustPhase(double cosPhase)
{
	const auto hg = [](double g, double cosScatter) {
		return (1. - g*g)/std::pow(1. + g*g - 2.*g*cosScatter, 1.5);
	};
	// the scattering angle is 180 degrees less the phase angle
	return 0.95*hg(0.9, -cosPhase) + 0.05*hg(-0.6, -cosPhase);
}
// The formulas for the magnitude of comets hold for the usual phase angles: the dust is normalized at 45 degrees
const double DustPhaseReference = dustPhase(std::cos(45.*M_PI/180.));
}

CometTailMgr::CometTailMgr()
{
	setObjectName("CometTailMgr");
}

CometTailMgr::~CometTailMgr()
{
}

void CometTailMgr::init()
{
	QSettings* conf = StelApp::getInstance().getSettings();
	setFlagShowGuides(conf->value("comet_tails/flag_show_guides", false).toBool());
	setFlagShow(conf->value("comet_tails/flag_show", true).toBool());
	addAction("actionShow_Comet_Tails", N_("Display Options"), N_("Comet tails (photographic mode)"), "flagShow");
	addAction("actionShow_Comet_Tail_Guides", N_("Display Options"), N_("Comet tail guide lines"), "flagShowGuides");
}

double CometTailMgr::getCallOrder(StelModuleActionName actionName) const
{
	// After the comets themselves
	if (actionName == StelModule::ActionDraw)
		return StelApp::getInstance().getModuleMgr().getModule("SolarSystem")->getCallOrder(actionName) + 1.;
	return 0.;
}

void CometTailMgr::setFlagShowGuides(bool b)
{
	if (b != flagShowGuides)
	{
		flagShowGuides = b;
		StelApp::immediateSave("comet_tails/flag_show_guides", b);
		emit flagShowGuidesChanged(b);
	}
}

void CometTailMgr::setFlagShow(bool b)
{
	if (b != flagShow)
	{
		flagShow = b;
		StelApp::immediateSave("comet_tails/flag_show", b);
		emit flagShowChanged(b);
	}
}

Vec3d CometTailMgr::propagate(const Vec3d& r0, const Vec3d& v0, double mu, double dt)
{
	// No force (beta = 1): a straight line
	if (std::fabs(mu) < 1e-12)
		return r0 + v0*dt;
	// A force away from the Sun (beta > 1, small absorbing grains): integrated (Runge-Kutta, steps growing
	// geometrically from the release, where the grain is closest to the Sun and the force strongest)
	if (mu < 0.)
	{
		constexpr int Steps = 64;
		const auto accel = [mu](const Vec3d& r) {const double rn = r.norm(); return r*(-mu/(rn*rn*rn));};
		const double sign = dt >= 0. ? 1. : -1.;
		const double span = std::fabs(dt);
		const double first = std::min(span, std::max(1e-4, 0.002*r0.norm()*r0.norm()/std::max(v0.norm(), 1e-6)));
		const double growth = span > first ? std::pow(span/first, 1./(Steps - 1)) : 1.;
		Vec3d r = r0, v = v0;
		double t = 0., h = first;
		for (int i = 0; i < Steps && t < span; ++i)
		{
			if (i == Steps - 1 || t + h > span)
				h = span - t;
			const double hs = h*sign;
			const Vec3d k1v = accel(r), k1r = v;
			const Vec3d k2v = accel(r + k1r*(0.5*hs)), k2r = v + k1v*(0.5*hs);
			const Vec3d k3v = accel(r + k2r*(0.5*hs)), k3r = v + k2v*(0.5*hs);
			const Vec3d k4v = accel(r + k3r*hs), k4r = v + k3v*hs;
			r += (k1r + k2r*2. + k3r*2. + k4r)*(hs/6.);
			v += (k1v + k2v*2. + k3v*2. + k4v)*(hs/6.);
			t += h;
			h *= growth;
		}
		return r;
	}
	const double rn = r0.norm();
	const double rv = r0*v0;
	const double vr = rv/rn;
	const double smu = std::sqrt(mu);
	const double alpha = 2./rn - (v0*v0)/mu;
	// Initial guess of the universal anomaly (Vallado 2013, algorithm 8)
	double x;
	if (alpha > 1e-9)
		x = smu*dt*alpha;
	else if (alpha < -1e-9)
	{
		const double a = 1./alpha;
		const double sgn = dt >= 0. ? 1. : -1.;
		const double num = -2.*mu*alpha*dt;
		const double den = rv + sgn*std::sqrt(-mu*a)*(1. - rn*alpha);
		x = sgn*std::sqrt(-a)*std::log(std::max(num/den, 1e-12));
	}
	else
		x = smu*dt/rn;
	// Newton's method on the universal Kepler equation
	for (int i = 0; i < 100; ++i)
	{
		double C, S;
		stumpff(alpha*x*x, C, S);
		const double F = rn*vr/smu*x*x*C + (1. - alpha*rn)*x*x*x*S + rn*x - smu*dt;
		const double dF = rn*vr/smu*x*(1. - alpha*x*x*S) + (1. - alpha*rn)*x*x*C + rn;
		const double step = F/dF;
		x -= step;
		if (std::fabs(step) < 1e-12*std::max(1., std::fabs(x)))
			break;
	}
	double C, S;
	stumpff(alpha*x*x, C, S);
	const double f = 1. - x*x/rn*C;
	const double g = dt - x*x*x/smu*S;
	return r0*f + v0*g;
}

std::vector<CometTailMgr::Guide> CometTailMgr::computeGuides(const Comet* comet, double jde, double lightTime) const
{
	std::vector<Guide> guides;
	const double t = jde - lightTime;  // the time the light left the comet
	const auto grain = [comet, t](double beta, double age) {
		Vec3d pos, vel;
		comet->computePosition(t - age, pos, vel);
		return propagate(pos, vel, SunMu*(1. - beta), age);
	};

	// Syndynes: one beta, all ages
	std::vector<double> syndyneAges, spans;
	releaseAges(comet, t, MaxAge, SyndynePoints, syndyneAges, spans);
	for (const double beta : SyndyneBetas)
	{
		Guide g;
		g.color = Vec3f(1.f, 0.75f, 0.3f);
		g.label = QString("β %1").arg(beta);
		g.points.push_back(grain(beta, 0.));
		for (const double age : syndyneAges)
			g.points.push_back(grain(beta, age));
		guides.push_back(std::move(g));
	}
	// Synchrones: one age, all beta of the comet (0.001 to 1 by default)
	const TailModel model = tailModel(comet);
	const double lo = std::log10(model.betaMin), hi = std::log10(model.betaMax);
	for (const double age : SynchroneAges)
	{
		Guide g;
		g.color = Vec3f(0.55f, 0.75f, 1.f);
		g.label = QString("%1 d").arg(age);
		for (int i = 0; i <= SynchronePoints; ++i)
		{
			const double beta = std::pow(10., lo + (hi - lo)*i/SynchronePoints);
			g.points.push_back(grain(beta, age));
		}
		guides.push_back(std::move(g));
	}
	// The ion tail: along the solar wind as the comet sees it (the wind away from the Sun less the comet's velocity)
	{
		Vec3d pos, vel;
		comet->computePosition(t, pos, vel);
		Vec3d wind = pos*(SolarWindSpeed/pos.norm()) - vel;
		wind.normalize();
		Guide g;
		g.color = Vec3f(0.35f, 0.6f, 1.f);
		g.label = q_("ion tail");
		for (int i = 0; i <= 20; ++i)
			g.points.push_back(pos + wind*(IonTailLength*i/20.));
		guides.push_back(std::move(g));
	}
	return guides;
}

void CometTailMgr::drawGuides(StelCore* core, const Comet* comet) const
{
	const double jde = core->getJDE();
	const Vec3d observer = core->getObserverHeliocentricEclipticPos();
	Vec3d pos, vel;
	comet->computePosition(jde, pos, vel);
	if (pos.norm() > MaxDistance)
		return;
	// The light time, iterated once
	double lightTime = (pos - observer).norm()*LightTimePerAU;
	comet->computePosition(jde - lightTime, pos, vel);
	lightTime = (pos - observer).norm()*LightTimePerAU;

	const std::vector<Guide> guides = computeGuides(comet, jde, lightTime);
	const StelProjectorP prj = core->getProjection(StelCore::FrameJ2000);
	StelPainter painter(prj);
	painter.setBlending(true);
	painter.setLineSmooth(true);
	painter.setLineWidth(1.5f*static_cast<float>(StelApp::getInstance().getDevicePixelsPerPixel()));
	const auto direction = [&observer](const Vec3d& p) {
		Vec3d d = StelCore::matVsop87ToJ2000.multiplyWithoutTranslation(p - observer);
		d.normalize();
		return d;
	};
	for (const Guide& g : guides)
	{
		painter.setColor(g.color, 0.85f);
		Vec3d previous = direction(g.points.front());
		for (size_t i = 1; i < g.points.size(); ++i)
		{
			const Vec3d d = direction(g.points[i]);
			painter.drawGreatCircleArc(previous, d);
			previous = d;
		}
		// a label only where the line is long enough on the screen to tell it apart from the others
		Vec3d a, b;
		if (prj->project(direction(g.points.front()), a) && prj->project(previous, b) && (a - b).norm() > 40.*prj->getDevicePixelsPerPixel())
			painter.drawText(previous, g.label, 0.f, 4.f, 4.f);
	}
	painter.setLineWidth(1.f);
	painter.setLineSmooth(false);
	painter.setBlending(false);
}

double CometTailMgr::lightTime(const Comet* comet, double jde, const Vec3d& observer)
{
	Vec3d pos, vel;
	comet->computePosition(jde, pos, vel);
	double lt = (pos - observer).norm()*LightTimePerAU;
	comet->computePosition(jde - lt, pos, vel);
	return (pos - observer).norm()*LightTimePerAU;
}

bool CometTailMgr::drawsComet(const StelCore* core, const Comet* comet) const
{
	if (!flagShow || !core->getSkyDrawer()->getFlagPhotographicModeActive() || (splatProgramTried && !splatProgram))
		return false;
	if (core->getCurrentPlanet().data() == comet || !(comet->getVMagnitude(core) < LightMagnitude))
		return false;
	// as Comet::draw(): not with orbital elements far from their epoch (orbit_good), e.g. of other apparitions
	if (!comet->hasValidPositionalData(core->getJDE(), Planet::PositionQuality::OrbitPlotting))
		return false;
	return comet->getHeliocentricEclipticPos().norm() < MaxDistance;
}

const CometTailMgr::DustGrid& CometTailMgr::dustGrid(const Comet* comet, double jde, double lightTime) const
{
	DustGrid& grid = grids[comet];
	// The shape of the tail changes slowly: the grains are computed again after a quarter of an hour
	if (std::fabs(jde - grid.jde) < 0.01 && grid.offsets.size() == BetaCount*AgeCount)
		return grid;
	grid.jde = jde;
	grid.offsets.assign(BetaCount*AgeCount, Vec3d(0.));
	grid.weights.assign(BetaCount*AgeCount, 0.f);
	grid.spread.assign(BetaCount*AgeCount, 0.f);
	const double t = jde - lightTime;
	const TailModel model = tailModel(comet);
	const double lo = std::log10(model.betaMin), hi = std::log10(model.betaMax);
	Vec3d nucleus, vel;
	comet->computePosition(t, nucleus, vel);
	// The small grains leave the comet within months; the large ones (beta below SizeBreakBeta) linger along its
	// orbit for years, released also far from the Sun: the anti-tail when the Earth passes the plane of the orbit
	int firstSmall = 0;
	while (firstSmall < BetaCount && std::pow(10., lo + (hi - lo)*(firstSmall + 0.5)/BetaCount) < SizeBreakBeta)
		++firstSmall;
	for (int population = 0; population < 2; ++population)
	{
		const int j0 = population == 0 ? 0 : firstSmall, j1 = population == 0 ? firstSmall : BetaCount;
		if (j0 >= j1)
			continue;
		std::vector<double> ages, dAges;
		releaseAges(comet, t, population == 0 ? LargeGrainAge : MaxGrainAge, AgeCount, ages, dAges);
		for (int k = 0; k < AgeCount; ++k)
		{
			const double age = ages[k];
			Vec3d pos;
			comet->computePosition(t - age, pos, vel);
			const double r = pos.norm();
			// the large grains also from far out, where CO and CO2 drive the activity (a shallower law)
			double production = dustProduction(r, population == 0 ? std::min(model.productionIndex, LargeProductionIndex) : model.productionIndex);
			// the striae: bursts of dust at times (not in the inner coma, which gives the magnitude)
			if (model.striae > 0.)
			{
				const double x = noise((t - age)/StriaePeriod, model.seed + 17);
				production *= std::max(0., 1. + model.striae*smoothstep(2., 6., age)*(5.*x*x*x*x - 1.));
			}
			for (int j = j0; j < j1; ++j)
			{
				const double beta = std::pow(10., lo + (hi - lo)*(j + 0.5)/BetaCount);
				const int i = j*AgeCount + k;
				const double mu = SunMu*(1. - beta);
				const Vec3d grain = propagate(pos, vel, mu, age);
				grid.offsets[i] = grain - nucleus;
				// a grain that has passed close to the Sun since its release has evaporated
				double passage = 1.;
				if (pos*vel < 0. && mu > 0.)
				{
					const Vec3d h = pos^vel;
					const Vec3d ecc = (vel^h)/mu - pos/r;
					const double q = (h*h)/(mu*(1. + ecc.norm()));
					if (q < SublimationOuter && propagate(pos, vel, mu, age*0.999).norm() < grain.norm())
						passage = survival(q);
				}
				// grains of the size distribution n(s) ~ s^-alpha (alpha 4 by default, 3 for the large grains):
				// their cross section per interval of ln(beta) ~ beta^(alpha - 3)
				const double size = beta >= SizeBreakBeta ? std::pow(beta, model.sizeIndex - 3.)
				                  : std::pow(SizeBreakBeta, model.sizeIndex - 3.)*std::pow(beta/SizeBreakBeta, LargeSizeIndex - 3.);
				grid.weights[i] = static_cast<float>(size*production*passage*dAges[k]);
				grid.spread[i] = static_cast<float>(EjectionSpeed*std::sqrt(beta/std::max(r, 0.02))*age);
			}
		}
	}
	return grid;
}

bool CometTailMgr::initSplats()
{
	if (splatProgramTried)
		return splatProgram != nullptr;
	splatProgramTried = true;
	const QByteArray vert = StelOpenGL::globalShaderPrefix(StelOpenGL::VERTEX_SHADER) + R"(
ATTRIBUTE highp vec2 position; // window coordinates
ATTRIBUTE highp vec2 center;
ATTRIBUTE highp vec3 inverse;  // dust: the inverse covariance (xx, xy, yy, pixels); gas: scale length, softening
ATTRIBUTE highp vec3 color;    // light in the scene at the center
ATTRIBUTE highp float profile; // 0 Gaussian, 1 Haser
uniform highp vec4 viewport;
VARYING highp vec2 vOffset;
VARYING highp vec3 vInverse;
VARYING highp vec3 vColor;
VARYING highp float vProfile;
void main(void)
{
	vOffset = position - center;
	vInverse = inverse;
	vColor = color;
	vProfile = profile;
	gl_Position = vec4((position - viewport.xy)/viewport.zw*2. - 1., 0., 1.);
}
)";
	const QByteArray frag = StelOpenGL::globalShaderPrefix(StelOpenGL::FRAGMENT_SHADER) + R"(
VARYING highp vec2 vOffset;
VARYING highp vec3 vInverse;
VARYING highp vec3 vColor;
VARYING highp float vProfile;
void main(void)
{
	highp float f;
	if (vProfile < 0.5)
	{
		highp float q = vInverse.x*vOffset.x*vOffset.x + 2.*vInverse.y*vOffset.x*vOffset.y + vInverse.z*vOffset.y*vOffset.y;
		f = q < 18. ? exp(-0.5*q) : 0.;
	}
	else
	{
		// column density of the Haser model (one generation): 1/rho exp(-rho/L), softened in the central pixel
		highp float rho = length(vOffset);
		f = exp(-rho/vInverse.x)/sqrt(rho*rho + vInverse.y*vInverse.y);
	}
	FRAG_COLOR = vec4(vColor*f, 1.);
}
)";
	std::unique_ptr<QOpenGLShaderProgram> program(new QOpenGLShaderProgram);
	if (!program->addShaderFromSourceCode(QOpenGLShader::Vertex, vert)
	    || !program->addShaderFromSourceCode(QOpenGLShader::Fragment, frag))
	{
		qWarning() << "CometTailMgr: cannot compile the shaders:" << program->log();
		return false;
	}
	program->bindAttributeLocation("position", 0);
	program->bindAttributeLocation("center", 1);
	program->bindAttributeLocation("inverse", 2);
	program->bindAttributeLocation("color", 3);
	program->bindAttributeLocation("profile", 4);
	if (!program->link())
	{
		qWarning() << "CometTailMgr: cannot link the shaders:" << program->log();
		return false;
	}
	splatBuffer.reset(new QOpenGLBuffer(QOpenGLBuffer::VertexBuffer));
	splatBuffer->create();
	splatBuffer->setUsagePattern(QOpenGLBuffer::StreamDraw);
	splatVao.reset(new QOpenGLVertexArrayObject);
	splatVao->create();
	splatProgram = std::move(program);
	return true;
}

void CometTailMgr::drawLight(StelCore* core, const Comet* comet)
{
	StelSkyDrawer* drawer = core->getSkyDrawer();
	const double jde = core->getJDE();
	const Vec3d observer = core->getObserverHeliocentricEclipticPos();
	const double lt = lightTime(comet, jde, observer);
	Vec3d nucleus, vel;
	comet->computePosition(jde - lt, nucleus, vel);
	const DustGrid& grid = dustGrid(comet, jde, lt);
	const TailModel model = tailModel(comet);
	const Emissions emissions = emissionColors(drawer);

	const StelProjectorP prj = core->getProjection(StelCore::FrameJ2000);
	const double pixPerRad = static_cast<double>(prj->getPixelPerRadAtCenter());
	const float sceneScale = drawer->getPhotoSceneScale();
	const Extinction& extinction = drawer->getExtinction();
	const bool withExtinction = drawer->getFlagHasAtmosphere() && extinction.getExtinctionCoefficient() >= 0.01f;
	const bool withTint = withExtinction && drawer->getFlagPhotoExtinctionColorActive();
	// The ground blocks the light from below the horizon (lowered for an observer high above it); without it the
	// wide splats of far, faint grains near the Sun, bright in forward scattering, would shine above the horizon
	const double horizonDip = std::acos(EarthRadius/(EarthRadius + std::max(0., static_cast<double>(core->getCurrentLocation().altitude))));
	const double hidden = -std::sin(horizonDip + 1.*M_PI/180.);
	const auto transmission = [&](const Vec3d& j2000) {
		if (!withExtinction)
			return Vec3f(1.f);
		Vec3d altAz = core->j2000ToAltAz(j2000, StelCore::RefractionOff);
		altAz.normalize();
		if (altAz[2] < hidden)
			return Vec3f(0.f);
		float mag = 0.f;
		extinction.forward(altAz, &mag);
		const float f = std::pow(10.f, -0.4f*mag);
		return withTint ? drawer->getPhotoExtinctionTint(altAz)*f : Vec3f(f);
	};
	const double viewX = prj->getViewportPosX(), viewY = prj->getViewportPosY();
	const double viewW = prj->getViewportWidth(), viewH = prj->getViewportHeight();

	// The whole light of the comet, from its magnitude (illuminance in lux)
	const double delta = (nucleus - observer).norm();
	const double total = IlluminanceMag0*std::pow(10., -0.4*static_cast<double>(comet->getVMagnitude(core)));
	Vec3d sight = nucleus - observer;
	sight.normalize();

	// The grains as seen now
	const int n = BetaCount*AgeCount;
	std::vector<Vec3d> win(n);
	std::vector<char> valid(n, 0);
	std::vector<double> light(n, 0.);
	std::vector<Vec3d> direction(n);
	double inside = 0.;
	for (int i = 0; i < n; ++i)
	{
		const Vec3d& off = grid.offsets[i];
		const Vec3d p = nucleus + off;
		const Vec3d d = p - observer;
		const double dist = d.norm();
		const double r = p.norm();
		if (!(dist > 1e-6) || !(r > 1e-3))
			continue;
		direction[i] = StelCore::matVsop87ToJ2000.multiplyWithoutTranslation(d/dist);
		valid[i] = prj->project(direction[i], win[i]);
		// the light of the grains relative to each other: their cross section, the distances
		light[i] = static_cast<double>(grid.weights[i])/(r*r*dist*dist);
		// the share in the coma (the aperture at the distance of the comet, across the line of sight)
		const double rho = (off - sight*(off*sight)).norm();
		// (the Gaussian of the splat in a disk of the aperture: exact in its center, about right off it)
		const double s2 = std::pow(0.6*static_cast<double>(grid.spread[i]), 2) + 1e-30;
		const double A2 = DustAperture*DustAperture;
		const double frac = (1. - std::exp(-0.5*A2/s2))*std::exp(-rho*rho/(s2 + 0.5*A2));
		inside += light[i]*frac;
	}
	if (!(inside > 0.))
		return;
	// The dust in the coma: its share of the magnitude, or from its Afrho (A'Hearn et al. 1984: the dust in an
	// aperture of radius rho gives Afrho rho/(2 delta r)^2 of the sunlight at 1 AU, delta and rho in cm, r in AU),
	// which grows towards the Sun as the production of dust over the speed of the grains (r^0.5); the gas gives
	// the rest of the magnitude
	const double r = nucleus.norm();
	double dust = model.dustFraction*total;
	double gas = (1. - model.dustFraction)*total;
	if (model.afrho > 0.)
	{
		const double afrho = model.afrho*dustProduction(r, model.productionIndex)*std::sqrt(std::max(r, 0.02));
		dust = SunIlluminance*afrho*DustAperture*CmPerAU/std::pow(2.*delta*CmPerAU*r, 2);
		gas = std::max(total - dust, 0.);
	}
	const double dustScale = dust/inside;


	vertexData.clear();
	const auto addQuad = [this](const Vec3d& c, double ex, double ey, const Vec3f& inv, const Vec3f& color, float profile) {
		const float corners[6][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, -1}, {1, 1}, {-1, 1}};
		for (const auto& k : corners)
		{
			const float v[FloatsPerVertex] = {static_cast<float>(c[0] + k[0]*ex), static_cast<float>(c[1] + k[1]*ey),
			                                  static_cast<float>(c[0]), static_cast<float>(c[1]),
			                                  inv[0], inv[1], inv[2], color[0], color[1], color[2], profile};
			vertexData.insert(vertexData.end(), v, v + FloatsPerVertex);
		}
	};
	// How many sigmas of a Gaussian splat with this light at its center are worth drawing: down to a light far
	// below what the exposure shows (at most 3; none for a splat that faint everywhere)
	const auto cutoff = [](const Vec3f& color) {
		const double brightest = static_cast<double>(std::max(color[0], std::max(color[1], color[2])));
		return brightest > SplatFloor ? std::sqrt(std::min(9., 2.*std::log(brightest/SplatFloor))) : 0.;
	};
	const auto outside = [&](const Vec3d& c, double ex, double ey) {
		return c[0] + ex < viewX || c[0] - ex > viewX + viewW || c[1] + ey < viewY || c[1] - ey > viewY + viewH;
	};

	// The dust: a Gaussian per grain, as wide as the cloud from the speed of ejection and the spacing to the
	// neighbors on the grid (along the syndyne and the synchrone), so that the splats join into a smooth tail
	double reach = 0.;
	Vec3d head;
	const bool headVisible = prj->project(StelCore::matVsop87ToJ2000.multiplyWithoutTranslation(sight), head);
	for (int j = 0; j < BetaCount; ++j)
	{
		for (int k = 0; k < AgeCount; ++k)
		{
			const int i = j*AgeCount + k;
			if (!valid[i])
				continue;
			const auto neighbor = [&](int i0, int i1) {
				if (i0 >= 0 && i1 >= 0 && valid[i0] && valid[i1])
					return Vec3d((win[i1] - win[i0])*0.5);
				if (i1 >= 0 && valid[i1])
					return Vec3d(win[i1] - win[i]);
				if (i0 >= 0 && valid[i0])
					return Vec3d(win[i] - win[i0]);
				return Vec3d(0.);
			};
			const Vec3d a = neighbor(k > 0 ? i - 1 : -1, k < AgeCount - 1 ? i + 1 : -1);
			const Vec3d b = neighbor(j > 0 ? i - AgeCount : -1, j < BetaCount - 1 ? i + AgeCount : -1);
			const Vec3d p = nucleus + grid.offsets[i];
			const double dist = (p - observer).norm();
			const double cloud = 0.6*static_cast<double>(grid.spread[i])/dist*pixPerRad;
			const double iso = cloud*cloud + 0.36;
			const double cxx = 0.36*(a[0]*a[0] + b[0]*b[0]) + iso;
			const double cxy = 0.36*(a[0]*a[1] + b[0]*b[1]);
			const double cyy = 0.36*(a[1]*a[1] + b[1]*b[1]) + iso;
			const double det = cxx*cyy - cxy*cxy;
			const double ex = 3.*std::sqrt(cxx), ey = 3.*std::sqrt(cyy);
			if (!(det > 0.) || outside(win[i], ex, ey))
				continue;
			// the phase angle at the grain: between the Sun and the observer
			const Vec3d toSun = -p/p.norm();
			const Vec3d toObserver = (observer - p)/dist;
			const double phase = dustPhase(toSun*toObserver)/DustPhaseReference;
			const double illuminance = dustScale*light[i]*phase;
			// peak luminance (cd/m^2): illuminance over the solid angle of the Gaussian
			const double peak = illuminance*pixPerRad*pixPerRad/(2.*M_PI*std::sqrt(det));
			const Vec3f t = transmission(direction[i]);
			const Vec3f color = Vec3f(DustColor[0]*t[0], DustColor[1]*t[1], DustColor[2]*t[2])*static_cast<float>(peak*static_cast<double>(sceneScale));
			// the quad only as far as its light is seen
			const double cut = cutoff(color);
			const double ex2 = cut*std::sqrt(cxx), ey2 = cut*std::sqrt(cyy);
			if (!(cut > 0.) || ex2 > 2e4 || ey2 > 2e4 || outside(win[i], ex2, ey2))
				continue;
			addQuad(win[i], ex2, ey2, Vec3f(static_cast<float>(cyy/det), static_cast<float>(-cxy/det), static_cast<float>(cxx/det)), color, 0.f);
			if (headVisible)
				reach = std::max(reach, (win[i] - head).norm() + std::max(ex2, ey2));
		}
	}

	// Streams of particles (ions, atoms) as Gaussians along them: positions, light (relative, at the distance 1 AU
	// from the observer) and widths (AU), stream by stream; scale brings the light to lux
	const auto addStreams = [&](const std::vector<Vec3d>& pos, const std::vector<double>& light, const std::vector<double>& width,
	                            size_t perStream, double scale, const Vec3f& emission) {
		std::vector<Vec3d> streamWin(pos.size());
		std::vector<char> streamValid(pos.size(), 0);
		std::vector<Vec3d> streamDir(pos.size());
		for (size_t i = 0; i < pos.size(); ++i)
		{
			Vec3d d = pos[i] - observer;
			d.normalize();
			streamDir[i] = StelCore::matVsop87ToJ2000.multiplyWithoutTranslation(d);
			streamValid[i] = prj->project(streamDir[i], streamWin[i]);
		}
		for (size_t i = 0; i < pos.size(); ++i)
		{
			if (!streamValid[i] || !(light[i] > 0.))
				continue;
			const size_t k = i % perStream;
			// along the stream: half the distance to the neighbors
			Vec3d a(0.);
			if (k > 0 && k < perStream - 1 && streamValid[i - 1] && streamValid[i + 1])
				a = (streamWin[i + 1] - streamWin[i - 1])*0.5;
			else if (k < perStream - 1 && streamValid[i + 1])
				a = streamWin[i + 1] - streamWin[i];
			else if (k > 0 && streamValid[i - 1])
				a = streamWin[i] - streamWin[i - 1];
			const double dist = (pos[i] - observer).norm();
			const double w = width[i]/dist*pixPerRad;
			const double iso = w*w + 0.36;
			const double cxx = 0.36*a[0]*a[0] + iso, cxy = 0.36*a[0]*a[1], cyy = 0.36*a[1]*a[1] + iso;
			const double det = cxx*cyy - cxy*cxy;
			const double ex = 3.*std::sqrt(cxx), ey = 3.*std::sqrt(cyy);
			if (!(det > 0.) || outside(streamWin[i], ex, ey))
				continue;
			const double illuminance = scale*light[i]/(dist*dist);
			const double peak = illuminance*pixPerRad*pixPerRad/(2.*M_PI*std::sqrt(det));
			const Vec3f tr = transmission(streamDir[i]);
			const Vec3f color = Vec3f(emission[0]*tr[0], emission[1]*tr[1], emission[2]*tr[2])*static_cast<float>(peak*static_cast<double>(sceneScale));
			// the quad only as far as its light is seen
			const double cut = cutoff(color);
			const double ex2 = cut*std::sqrt(cxx), ey2 = cut*std::sqrt(cyy);
			if (!(cut > 0.) || ex2 > 2e4 || ey2 > 2e4 || outside(streamWin[i], ex2, ey2))
				continue;
			addQuad(streamWin[i], ex2, ey2, Vec3f(static_cast<float>(cyy/det), static_cast<float>(-cxy/det), static_cast<float>(cxx/det)), color, 0.f);
			if (headVisible)
				reach = std::max(reach, (streamWin[i] - head).norm() + std::max(ex2, ey2));
		}
	};
	const Vec3d radial = nucleus/r;
	const double t = jde - lt;

	// The ion tail: streams of ions released at earlier times, each along the solar wind as the comet saw it then
	// (its direction and speed change with time, so knots and kinks move down the tail)
	const double ionActivity = r < 2. ? 1. : (r < 4. ? 0.5 + 0.5*std::cos((r - 2.)*M_PI/2.) : 0.);
	if (ionActivity > 0. && model.ionFraction > 0.)
	{
		Vec3d axis = radial*SolarWindSpeed - vel;
		axis.normalize();
		// two directions across the tail: in the plane of the orbit and out of it
		Vec3d e1 = vel - axis*(vel*axis);
		e1.normalize();
		const Vec3d e2 = axis^e1;
		const int seed = model.seed;
		// the light of the ions: released evenly in time, glowing while they live
		std::vector<Vec3d> ionPos(IonStreams.size()*IonAgeCount);
		std::vector<double> ionLight(ionPos.size(), 0.), ionWidth(ionPos.size(), 0.);
		double sum = 0.;
		for (size_t m = 0; m < IonStreams.size(); ++m)
		{
			const IonStream& st = IonStreams[m];
			for (int k = 0; k < IonAgeCount; ++k)
			{
				const double u = (k + 0.5)/IonAgeCount;
				const double age = IonMaxAge*u*u;
				const double dAge = 2.*IonMaxAge*u/IonAgeCount;
				const double release = (t - age)*24.; // hours
				// the wind as the ions met it: direction off by a few degrees, its speed 300-600 km/s
				const double speed = SolarWindSpeed*(0.75 + 0.75*noise(release/11., seed + 7));
				Vec3d dir = radial*speed - vel;
				dir.normalize();
				const double dx = (noise(release/13., seed + 1) - 0.5)*0.02, dy = (noise(release/17., seed + 2) - 0.5)*0.02;
				// a ray leaves the head at its angle and folds onto the axis with age
				const double angle = st.angle*M_PI/180.*std::exp(-age/IonFolding);
				const Vec3d side = e1*std::cos(st.azimuth*M_PI) + e2*std::sin(st.azimuth*M_PI);
				dir = dir*std::cos(angle) + side*std::sin(angle) + e1*dx + e2*dy;
				dir.normalize();
				const double s = ionDistance(age);
				// rays come and go: each is lit for a few hours now and then; the main tail varies less
				const double lit = m == 0 ? 0.6 + 0.8*noise(release/st.period, seed + 3)
				                          : std::max(0., noise(release/st.period, seed + 11*static_cast<int>(m)) - 0.4)*2.5;
				const size_t i = m*IonAgeCount + k;
				ionPos[i] = nucleus + dir*s;
				ionLight[i] = st.strength*lit*dAge*std::exp(-age/IonLifetime);
				ionWidth[i] = st.width*(IonWidth + IonOpening*s);
				sum += ionLight[i];
			}
		}
		if (sum > 0.)
			addStreams(ionPos, ionLight, ionWidth, IonAgeCount, model.ionFraction*ionActivity*total*delta*delta/sum, emissions.ion);
	}

	// The sodium tail: atoms released evenly in time, pushed away from the Sun by its light as they meet it with
	// their radial speed (the comet's and what they have gained), glowing in the D lines while they live
	const double sodiumFraction = std::min(model.sodiumFraction/std::pow(std::max(r, 0.02), 2), SodiumMaxFraction);
	if (sodiumFraction > 0.)
	{
		const double lifetime = SodiumLifetime*r*r;
		const double maxAge = 5.*lifetime;
		const double radialSpeed = vel*radial;
		const auto swings = [](double v) {return 1. - SwingsDepth*std::exp(-(v*v)/(SwingsWidth*SwingsWidth));};
		// the motion away from the nucleus (along the direction from the Sun): distance and speed by age
		std::vector<double> distance(SodiumSteps + 1, 0.), speed(SodiumSteps + 1, 0.);
		const double h = maxAge/SodiumSteps;
		const auto acceleration = [&](double s, double v) {return SodiumAcceleration/std::pow(r + s, 2)*swings(radialSpeed + v);};
		for (int n = 0; n < SodiumSteps; ++n)
		{
			const double s = distance[n], v = speed[n];
			const double a1 = acceleration(s, v);
			const double a2 = acceleration(s + 0.5*h*v, v + 0.5*h*a1);
			speed[n + 1] = v + h*a2;
			distance[n + 1] = s + h*(v + 0.5*h*a1);
		}
		const auto at = [&](const std::vector<double>& table, double age) {
			const double x = std::clamp(age/h, 0., static_cast<double>(SodiumSteps));
			const int n = std::min(static_cast<int>(x), SodiumSteps - 1);
			return table[n] + (x - n)*(table[n + 1] - table[n]);
		};
		std::vector<Vec3d> naPos(SodiumAgeCount);
		std::vector<double> naLight(SodiumAgeCount, 0.), naWidth(SodiumAgeCount, 0.);
		double reference = 0.;
		for (int k = 0; k < SodiumAgeCount; ++k)
		{
			const double u = (k + 0.5)/SodiumAgeCount;
			const double age = maxAge*u*u;
			const double dAge = 2.*maxAge*u/SodiumAgeCount;
			const double s = at(distance, age);
			// pushed along the direction from the Sun as it turned while the atom moved (its mean for an even push)
			Vec3d pos, v;
			comet->computePosition(t - age*2./3., pos, v);
			pos.normalize();
			const double alive = std::exp(-age/lifetime)*dAge;
			naPos[k] = nucleus + pos*s;
			// the light the atoms scatter: as the sunlight they see, in the lines or out of them
			naLight[k] = alive*swings(radialSpeed + at(speed, age))*std::pow(r/(r + s), 2);
			naWidth[k] = SodiumWidth + SodiumSpeed*age;
			reference += alive;
		}
		addStreams(naPos, naLight, naWidth, SodiumAgeCount, sodiumFraction*total*delta*delta/reference, emissions.sodium);
	}

	// The gas of the coma: the rest of the light, with the profile of Haser (scale length growing with r^2)
	if (headVisible && gas > 0.)
	{
		const double scale = std::max(GasScaleLength*r*r/delta*pixPerRad, 0.5);
		const double A = gas*pixPerRad*pixPerRad/(2.*M_PI*scale);
		const double ex = std::min(7.*scale, 2e4);
		if (!outside(head, ex, ex))
		{
			const Vec3f tr = transmission(StelCore::matVsop87ToJ2000.multiplyWithoutTranslation(sight));
			const Vec3f color = Vec3f(emissions.gas[0]*tr[0], emissions.gas[1]*tr[1], emissions.gas[2]*tr[2])*static_cast<float>(A*static_cast<double>(sceneScale));
			addQuad(head, ex, ex, Vec3f(static_cast<float>(scale), 0.5f, 0.f), color, 1.f);
			reach = std::max(reach, ex);
		}
	}
	if (vertexData.empty())
		return;

	// With a star trail or a lens out of focus, the light is drawn into an image of its own that is then trailed
	// (and added to the scene, see StelSkyDrawer::beginPhotoBodyImage())
	const bool image = headVisible && drawer->beginPhotoBodyImage(StelCore::matVsop87ToJ2000.multiplyWithoutTranslation(sight), static_cast<float>(reach), true);

	auto& gl = *QOpenGLContext::currentContext()->functions();
	splatProgram->bind();
	splatProgram->setUniformValue("viewport", QVector4D(static_cast<float>(viewX), static_cast<float>(viewY), static_cast<float>(viewW), static_cast<float>(viewH)));
	splatVao->bind();
	splatBuffer->bind();
	splatBuffer->allocate(vertexData.data(), static_cast<int>(vertexData.size()*sizeof(float)));
	const int stride = FloatsPerVertex*sizeof(float);
	const int sizes[5] = {2, 2, 3, 3, 1};
	size_t offset = 0;
	for (int a = 0; a < 5; ++a)
	{
		gl.glVertexAttribPointer(a, sizes[a], GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(offset*sizeof(float)));
		gl.glEnableVertexAttribArray(a);
		offset += sizes[a];
	}
	gl.glEnable(GL_BLEND);
	gl.glBlendFunc(GL_ONE, GL_ONE);
	gl.glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(vertexData.size()/FloatsPerVertex));
	gl.glDisable(GL_BLEND);
	for (int a = 0; a < 5; ++a)
		gl.glDisableVertexAttribArray(a);
	splatBuffer->release();
	splatVao->release();
	splatProgram->release();

	if (image)
		drawer->endPhotoBodyImage();
}

void CometTailMgr::draw(StelCore* core)
{
	const bool photo = flagShow && core->getSkyDrawer()->getFlagPhotographicModeActive();
	if (!flagShowGuides && !photo)
		return;
	static SolarSystem* ssystem = GETSTELMODULE(SolarSystem);
	if (!ssystem)
		return;
	if (photo)
		initSplats();
	const QList<StelObjectP>& selected = StelApp::getInstance().getStelObjectMgr().getSelectedObject();
	for (const PlanetP& p : ssystem->getAllMinorBodies())
	{
		if (p->getPlanetType() != Planet::isComet)
			continue;
		const Comet* comet = static_cast<const Comet*>(p.data());
		if (photo && drawsComet(core, comet))
			drawLight(core, comet);
		if (!flagShowGuides || !comet->hasValidPositionalData(core->getJDE(), Planet::PositionQuality::OrbitPlotting))
			continue;
		bool isSelected = false;
		for (const StelObjectP& s : selected)
			isSelected = isSelected || s.data() == p.data();
		if (!isSelected && !(comet->getVMagnitude(core) < GuideMagnitude))
			continue;
		drawGuides(core, comet);
	}
}
