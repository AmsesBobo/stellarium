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

// Photographic mode of StelSkyDrawer: the films. Their data (speed, reciprocity failure, grain, curve, spectral
// sensitivity) and what the camera does with them: the reciprocity failure, the color shift of long exposures,
// push processing, grain and halation.

#include "StelSkyDrawer.hpp"
#include "StelPhotoBloom.hpp"
#include "StelApp.hpp"
#include "StelCore.hpp"
#include "StelMovementMgr.hpp"
#include "StelProjector.hpp"

#include <QtGlobal>
#include <array>
#include <cmath>
#include <initializer_list>

// Reciprocity and color shifts: FUJIFILM data sheets (Provia 100F and Provia 400F AF3-066E: no
// correction up to 128 s and 32 s), Kodak E-28 (E200: none up to 10 s) and Kodak E-31 (T-Max 400 and Tri-X:
// the adjusted times for 1, 10 and 100 s). Where the data sheets stop, the Schwarzschild exponent is taken
// from the last points; for E200 ("unusually good reciprocity" in astrophotography) and Royal Gold 400 (not
// recommended beyond 10 s) it is an estimate (0.9 and 0.8).
// Granularity: diffuse RMS of the data sheets (Royal Gold 400: estimated, Kodak gives a print grain index).
// Curves: slide film has a gamma of about 1.7 and is viewed as it is, so the picture follows E^1.7; a negative
// (gamma about 0.65) printed on paper (about 1.8) gives a system gamma of about 1.15 and much more latitude.
// Black and white films are panchromatic: more red and blue than the eye, T-Max a little less blue.
// Spectral sensitivity of the color films: the curves of the data sheets (Fujifilm Provia 100F, also for 400F;
// Kodak Ektachrome 100 for E200; Kodak Ultramax 400 for Royal Gold 400), as digitized by the spektrafilm project
// (A. Volpato, github.com/andreavolpato/spektrafilm, profiles CC BY-SA 4.0). The red layer reaches far into
// the red (peak 645-655 nm, still 0.77-1.0 of it at H-alpha), the green one is narrower than the eye's: in the
// twilight, when ozone takes away the yellow and orange (Chappuis band), the film keeps its red where the eye
// loses it, and blue and red make the violet of film pictures. The layers are fitted, by least squares, to
// the scene in two ways, over blackbodies (1800-20000 K), daylight (D50-D75) and twilight spectra (daylight,
// ozone up to 60 air masses, Rayleigh scattering): from linear sRGB (rgbToFilm, for the smooth spectra of the
// stars, planets, Milky Way and landscape: 1-2% off for blackbodies) and from the X, Y, Z and scotopic W
// of ShowMySky (skyXYZW: the violet twilight within about 5%; from sRGB the film would lose half of the red;
// the lightweight atmosphere has no W, it is estimated from X, Y and Z, see xyYToRGB.glsl: about 7%).
// The sky from X, Y, Z, W is off by default ("Spectral sky" in the dialog,
// astro/flag_photographic_film_spectral_sky): its large coefficients cancel each other, so that a few percent in W
// make 25% in red, and the violet it gives to the twilight is not in Till Credner's twilight slides (Ektachrome,
// Sensia: a deep blue sky over an orange horizon), which the colors of rgbToFilm match.
// The airglow (O I 557.7 and 630.0/636.4 nm, Na 589 nm, OH bands, with a continuum fitted to its color) is
// computed from its spectrum directly.
// Emission nebulae (H II regions) too: their light is in lines, most of it in H-alpha at 656 nm, which the V band
// of their catalog magnitude hardly sees (about 9%) but the red layer of a film does. A typical H II region
// (relative to H-beta: H-alpha 2.86, [N II] 658.3/654.8 0.8/0.27, [O III] 500.7/495.9 2.0/0.67 as in the bright
// ones, M 42, M 8, M 20, the Rosette, H-gamma and H-delta, [S II] 0.4, He I, a weak continuum; reddened by
// E(B-V) = 0.5) exposes the red layer about 4.6-4.9 times as much as white light of the same V, green and blue
// 0.7-0.85 times: on film they are bright and red (North America, Orion, Lagoon). The scene colors that give
// these layers through rgbToFilm.

namespace
{
using FilmSpec = StelSkyDrawer::PhotoFilmSpec;

//! One point of the reciprocity table of a data sheet: at the exposure time (s) the film records as much as at the
//! effective time with a short exposure, and asks for a green filter of this CC density
struct ReciprocityPoint
{
	double time;
	double effective;
	double green;
};

//! The reciprocity table (increasing times, at most four points) and the Schwarzschild exponent after its last point
void setReciprocity(FilmSpec& f, std::initializer_list<ReciprocityPoint> points, double exponent)
{
	Q_ASSERT(points.size() <= f.time.size());
	f.points = 0;
	for (const ReciprocityPoint& p : points)
	{
		f.time[f.points] = p.time;
		f.effective[f.points] = p.effective;
		f.green[f.points] = p.green;
		++f.points;
	}
	f.exponent = exponent;
}

//! The spectral data of a color film (see PhotoFilmSpec), fitted to the spectral sensitivities of its data sheet
struct SpectralData
{
	std::array<float, 9> rgbToFilm;
	std::array<float, 12> skyXYZW;
	std::array<float, 3> airglow;
	std::array<float, 3> emission;
};

void setSpectral(FilmSpec& f, const SpectralData& d)
{
	f.spectral = true;
	f.rgbToFilm = d.rgbToFilm;
	f.skyXYZW = d.skyXYZW;
	f.airglow = d.airglow;
	f.emission = d.emission;
}

// The curves of Fujifilm Provia 100F (used for Provia 400F too)
const SpectralData proviaSpectral = {
	{0.8823f, -0.0092f, 0.0425f, 0.1133f, 0.8423f, 0.0529f, 0.0625f, -0.0729f, 1.2184f},
	{7.7363f, -8.6066f, -2.8156f, 2.1554f, -1.9413f, 3.4614f, 0.5767f, -0.5048f, -0.7867f, 1.2239f, 1.4820f, -0.4456f},
	{1.3715f, 0.9966f, 0.6528f},
	{5.183f, 0.129f, 0.291f}
};

// The curves of Kodak Ektachrome 100 (for E200)
const SpectralData ektachromeSpectral = {
	{0.8792f, -0.0011f, 0.0371f, 0.0948f, 0.8654f, 0.0501f, 0.1603f, -0.4778f, 1.5726f},
	{7.5488f, -8.3193f, -2.7195f, 2.0691f, -1.6322f, 2.9534f, 0.4129f, -0.3452f, -1.9772f, 3.2321f, 2.0329f, -1.0508f},
	{1.3868f, 1.0087f, 0.7239f},
	{5.185f, 0.272f, 0.f}
};

// The curves of Kodak Ultramax 400 (for Royal Gold 400)
const SpectralData ultramaxSpectral = {
	{0.9412f, -0.0826f, 0.0515f, 0.0703f, 0.8433f, 0.1094f, 0.1719f, -0.4943f, 1.5769f},
	{8.9881f, -10.5313f, -3.4227f, 2.7209f, -1.3177f, 2.4159f, 0.2430f, -0.1723f, -2.2375f, 3.6457f, 2.1490f, -1.1702f},
	{1.2693f, 1.0570f, 0.7534f},
	{5.226f, 0.567f, 0.053f}
};

FilmSpec provia100F()
{
	FilmSpec f;
	f.name = "Fujichrome Provia 100F";
	f.shortName = "Provia 100F";
	f.iso = 100.;
	f.type = StelSkyDrawer::PhotoFilmSlide;
	setReciprocity(f, {{128., 128., 0.}, {240., 190.5, 0.025}}, 0.63);
	f.granularity = 8.;
	f.gamma = 1.7;
	f.toneA = 1.7f;
	f.toneClip = 1.0f;
	f.toneMid = 0.15f;
	setSpectral(f, proviaSpectral);
	return f;
}

FilmSpec provia400F()
{
	FilmSpec f;
	f.name = "Fujichrome Provia 400F";
	f.shortName = "Provia 400F";
	f.iso = 400.;
	f.type = StelSkyDrawer::PhotoFilmSlide;
	setReciprocity(f, {{32., 32., 0.}, {60., 37.8, 0.05}, {120., 60., 0.075}, {240., 120., 0.075}}, 0.66);
	f.granularity = 13.;
	f.gamma = 1.7;
	f.toneA = 1.7f;
	f.toneClip = 1.0f;
	f.toneMid = 0.15f;
	setSpectral(f, proviaSpectral);
	return f;
}

FilmSpec ektachromeE200()
{
	FilmSpec f;
	f.name = "Kodak Ektachrome E200";
	f.shortName = "E200";
	f.iso = 200.;
	f.type = StelSkyDrawer::PhotoFilmSlide;
	setReciprocity(f, {{10., 10., 0.}}, 0.9);
	f.granularity = 12.;
	f.gamma = 1.7;
	f.toneA = 1.7f;
	f.toneClip = 1.0f;
	f.toneMid = 0.15f;
	setSpectral(f, ektachromeSpectral);
	return f;
}

FilmSpec royalGold400()
{
	FilmSpec f;
	f.name = "Kodak Royal Gold 400";
	f.shortName = "Royal Gold 400";
	f.iso = 400.;
	f.type = StelSkyDrawer::PhotoFilmNegative;
	setReciprocity(f, {{10., 10., 0.}}, 0.8);
	f.granularity = 5.;
	f.gamma = 0.65;
	f.toneA = 1.15f;
	f.toneClip = 11.5f;
	f.toneMid = 0.18f;
	setSpectral(f, ultramaxSpectral);
	return f;
}

FilmSpec tMax400()
{
	FilmSpec f;
	f.name = "Kodak T-Max 400";
	f.shortName = "T-Max 400";
	f.iso = 400.;
	f.type = StelSkyDrawer::PhotoFilmMonochrome;
	setReciprocity(f, {{0.4, 0.4, 0.}, {1.26, 1., 0.}, {15., 10., 0.}, {300., 100., 0.}}, 0.77);
	f.granularity = 10.;
	f.gamma = 0.65;
	f.toneA = 1.15f;
	f.toneClip = 5.8f;
	f.toneMid = 0.18f;
	f.mono = {0.30f, 0.52f, 0.18f};
	return f;
}

FilmSpec triX400()
{
	FilmSpec f;
	f.name = "Kodak Tri-X 400";
	f.shortName = "Tri-X 400";
	f.iso = 400.;
	f.type = StelSkyDrawer::PhotoFilmMonochrome;
	setReciprocity(f, {{0.17, 0.17, 0.}, {2., 1., 0.}, {50., 10., 0.}, {1200., 100., 0.}}, 0.725);
	f.granularity = 17.;
	f.gamma = 0.65;
	f.toneA = 1.15f;
	f.toneClip = 5.8f;
	f.toneMid = 0.18f;
	f.mono = {0.27f, 0.50f, 0.23f};
	return f;
}
}

const StelSkyDrawer::PhotoFilmSpec& StelSkyDrawer::getPhotoFilmSpec(int film)
{
	static const std::array<PhotoFilmSpec, PHOTO_FILM_COUNT> films = [] {
		std::array<PhotoFilmSpec, PHOTO_FILM_COUNT> a;  // PhotoFilmNone: the digital sensor (the default values)
		a[PhotoFilmProvia100F] = provia100F();
		a[PhotoFilmProvia400F] = provia400F();
		a[PhotoFilmEktachromeE200] = ektachromeE200();
		a[PhotoFilmRoyalGold400] = royalGold400();
		a[PhotoFilmTMax400] = tMax400();
		a[PhotoFilmTriX400] = triX400();
		return a;
	}();
	return films[qBound(0, film, PHOTO_FILM_COUNT - 1)];
}

// Reciprocity failure of a film, point by point. A film does not add up light at a low intensity as well as at a
// normal one: the latent image of a few photons fades before the next ones arrive. So the failure depends on the
// intensity, not on the time as such (Schwarzschild). The data sheets give it for a normal exposure: at the time t
// a middle grey is recorded as at t_eff(t). A point that gets the intensity I during the time t is recorded as a
// middle grey would be with the time t_n that it needs at that intensity, t_n = t x_mid/x (x its exposure, x_mid
// that of middle grey): its exposure counts with t_eff(t_n)/t_n. Middle grey goes as before; a faint sky loses much
// more, a bright star core less: the long exposures of film have their dark skies and high contrast. The green
// filter of the data sheet, per point the same way: the faint parts of a long exposure turn magenta.
// The exposure a film records at the camera exposure x (middle grey 0.18): x times the share that counts after
// the reciprocity failure at the brightness of the point (as the shader does it, without the color shift)
float StelSkyDrawer::photoFilmRecordedExposure(float x) const
{
	if (!getFlagPhotoFilmActive() || !flagPhotoReciprocityPerPoint || !(x > 0.f))
		return x;
	const PhotoFilmSpec& f = photoActiveFilmSpec;
	if (f.points <= 0)
		return x;
	const double tn = qMin(1e20, getPhotoEffectiveExposureTime()*PHOTO_MIDDLE_GREY_EXPOSURE/static_cast<double>(x));
	return static_cast<float>(static_cast<double>(x)*photoFilmEffectiveTime(f, tn)/tn);
}

// The camera exposure that a film records as xr (the inverse of photoFilmRecordedExposure(), which grows
// monotonically), found from x0 upwards or downwards
float StelSkyDrawer::photoFilmExposureForRecorded(float xr, float x0) const
{
	if (!getPhotoReciprocityParams().enabled || !(xr > 0.f))
		return xr;
	double lo = qMax(1e-30, static_cast<double>(x0)), hi = lo;
	while (photoFilmRecordedExposure(static_cast<float>(lo)) > xr && lo > 1e-30)
		lo *= 0.5;
	while (photoFilmRecordedExposure(static_cast<float>(hi)) < xr && hi < 1e30)
		hi *= 2.;
	for (int i = 0; i < 40; ++i)
	{
		const double m = std::sqrt(lo*hi);
		if (photoFilmRecordedExposure(static_cast<float>(m)) < xr)
			lo = m;
		else
			hi = m;
	}
	return static_cast<float>(std::sqrt(lo*hi));
}

StelSkyDrawer::PhotoReciprocityParams StelSkyDrawer::getPhotoReciprocityParams() const
{
	PhotoReciprocityParams p;
	if (!getFlagPhotoFilmActive() || !flagPhotoReciprocityPerPoint)
		return p;
	const PhotoFilmSpec& f = getPhotoActiveFilmSpec();
	if (f.points <= 0)
		return p;
	p.enabled = true;
	p.time = static_cast<float>(getPhotoEffectiveExposureTime());
	p.points = f.points;
	for (int i = 0; i < 4; ++i)
	{
		p.times[i] = static_cast<float>(f.time[i]);
		p.effective[i] = static_cast<float>(f.effective[i]);
		p.green[i] = static_cast<float>(f.green[i]);
	}
	p.exponent = static_cast<float>(f.exponent);
	return p;
}

double StelSkyDrawer::photoFilmEffectiveTime(const PhotoFilmSpec& f, double t)
{
	if (f.points <= 0 || !(t > f.time[0]))
		return t;
	for (int i = 1; i < f.points; ++i)
	{
		if (t <= f.time[i])
		{
			// log-log interpolation between the points of the data sheet
			const double u = std::log(t/f.time[i-1]) / std::log(f.time[i]/f.time[i-1]);
			return f.effective[i-1] * std::pow(f.effective[i]/f.effective[i-1], u);
		}
	}
	// beyond the last point: the Schwarzschild exponent
	const int last = f.points - 1;
	return f.effective[last] * std::pow(t/f.time[last], f.exponent);
}

double StelSkyDrawer::getPhotoFilmEffectiveTime(double t) const
{
	if (!getFlagPhotoFilmActive() || !(t > 0.))
		return t;
	return photoFilmEffectiveTime(photoActiveFilmSpec, t);
}

Vec3f StelSkyDrawer::getPhotoFilmTint(double t) const
{
	if (!getFlagPhotoFilmActive() || !(t > 0.))
		return Vec3f(1.f);
	const PhotoFilmSpec& f = getPhotoActiveFilmSpec();
	double green = 0.;
	if (f.points > 0 && t > f.time[0])
	{
		green = f.green[f.points - 1];
		for (int i = 1; i < f.points; ++i)
			if (t <= f.time[i])
			{
				const double u = std::log(t/f.time[i-1]) / std::log(f.time[i]/f.time[i-1]);
				green = f.green[i-1] + u*(f.green[i] - f.green[i-1]);
				break;
			}
	}
	// A CC green filter of density g corrects a green layer that has lost that much: 10^-g
	return Vec3f(1.f, static_cast<float>(std::pow(10., -green)), 1.f);
}

Vec3f StelSkyDrawer::getPhotoFilmMonoWeights() const
{
	if (!getFlagPhotoFilmActive())
		return Vec3f(0.f);
	const PhotoFilmSpec& f = getPhotoActiveFilmSpec();
	if (f.type != PhotoFilmMonochrome)
		return Vec3f(0.f);
	return Vec3f(f.mono[0], f.mono[1], f.mono[2]);
}

const float* StelSkyDrawer::getPhotoFilmSkyMatrix() const
{
	if (!getFlagPhotoFilmActive())
		return nullptr;
	const PhotoFilmSpec& f = getPhotoActiveFilmSpec();
	return getPhotoFilmSpectral(f) ? f.skyXYZW.data() : nullptr;
}

void StelSkyDrawer::setPhotoFilm(int film)
{
	const int f = qBound(0, film, PHOTO_FILM_COUNT - 1);
	if (f == photoFilm)
		return;
	// The same lens: keep the focal length (a film is 35 mm, the sensor may have been another size)
	const double focal = getPhotoFocalLengthForFov(core->getMovementMgr()->getAimFov());
	// The film has its own speed: keep the ISO of the digital sensor for later
	if (photoFilm == PhotoFilmNone)
		photoDigitalISO = photoProgramRunning ? photoUserISO : photoISO;
	// The lens glow of the film camera or of the digital camera (see setPhotoHaloStrength())
	const bool haloChanges = (photoFilm == PhotoFilmNone) != (f == PhotoFilmNone);
	photoFilm = f;
	updatePhotoActiveFilmSpec();
	if (haloChanges)
	{
		photoHaloFraction = (f != PhotoFilmNone) ? photoFilmHaloFraction : photoDigitalHaloFraction;
		emit photoHaloStrengthChanged(static_cast<double>(photoHaloFraction));
	}
	const double iso = (f != PhotoFilmNone) ? getPhotoActiveFilmSpec().iso : photoDigitalISO;
	if (photoProgramRunning)
		photoUserISO = iso;
	if (!qFuzzyCompare(iso, photoISO))
	{
		if (flagPhotoAutoExposure)
			photoAutoExposureTime = qBound(PHOTO_AUTO_MIN_TIME, photoAutoExposureTime*photoISO/iso, PHOTO_AUTO_MAX_TIME);
		photoISO = iso;
		emit photoISOChanged(iso);
	}
	if (getFlagPhotoCameraActive())
		core->getMovementMgr()->zoomTo(getPhotoFovForFocalLength(focal), 0.5f);
	photoWBMatrixKelvin = -1.;
	updatePhotoWhiteBalanceMatrix();
	updatePhotoToneCurve();
	StelApp::immediateSave("viewing/photographic_film", f);
	emit photoFilmChanged(f);
}

// Push processing: a longer first development (slide film) or development (negative) for a film exposed at a
// higher speed. Each stop doubles the speed; the curve gets steeper (about 12% more contrast per stop: the shadows
// fall deeper, the picture gains contrast) and the grain coarser (about 12% per stop in the data sheets of
// Kodak E200 and Fuji Provia 400F, 15% for black and white films, Tri-X at 1600). The reciprocity failure is not
// changed by it.
const StelSkyDrawer::PhotoFilmSpec& StelSkyDrawer::getPhotoActiveFilmSpec() const
{
	return photoActiveFilmSpec;
}

void StelSkyDrawer::updatePhotoActiveFilmSpec()
{
	photoActiveFilmSpec = getPhotoFilmSpec(photoFilm);
	if (photoFilm == PhotoFilmNone || photoFilmPush == 0)
		return;
	const double p = static_cast<double>(photoFilmPush);
	const bool mono = photoActiveFilmSpec.type == PhotoFilmMonochrome;
	photoActiveFilmSpec.iso *= std::exp2(p);
	const double contrast = 1. + 0.12*p;
	photoActiveFilmSpec.gamma *= contrast;
	photoActiveFilmSpec.toneA = static_cast<float>(static_cast<double>(photoActiveFilmSpec.toneA)*contrast);
	photoActiveFilmSpec.granularity *= std::pow(mono ? 1.15 : 1.12, p);
}

void StelSkyDrawer::setPhotoFilmPush(int stops)
{
	const int p = qBound(0, stops, PHOTO_FILM_MAX_PUSH);
	if (p == photoFilmPush)
		return;
	photoFilmPush = p;
	updatePhotoActiveFilmSpec();
	if (photoFilm != PhotoFilmNone)
	{
		const double iso = getPhotoActiveFilmSpec().iso;
		if (photoProgramRunning)
			photoUserISO = iso;
		if (!qFuzzyCompare(iso, photoISO))
		{
			if (flagPhotoAutoExposure)
				photoAutoExposureTime = qBound(PHOTO_AUTO_MIN_TIME, photoAutoExposureTime*photoISO/iso, PHOTO_AUTO_MAX_TIME);
			photoISO = iso;
			emit photoISOChanged(iso);
		}
		updatePhotoToneCurve();
	}
	StelApp::immediateSave("viewing/photographic_film_push", p);
	emit photoFilmPushChanged(p);
}

// The grain of a film. It is not a noise of the light but of the developed film: the silver grains (black and white)
// or the dye clouds (color) lie at random. The data sheets give the RMS fluctuation of the density sigma_D in a
// 48 micrometer aperture at density 1 (the granularity, x 1000). It shrinks as the square root of the area
// measured (Selwyn), and grows as the square root of the density (as many grains as blackening; Nutting and
// Siedentopf). A slide is the picture itself: densest in the shadows, so the grain shows most in a dark night sky,
// as a relative fluctuation of the light that passes, 10^(-sigma_D g). A negative is densest in the highlights;
// the paper of the print (its slope, the system gamma over the gamma of the negative) multiplies its fluctuation.
// The three layers of a color film each have their own grains. A screen pixel is a part of the film: the whole
// frame is the view (about 14 micrometers), or a scan at 4000 dpi for a "100% view".
StelSkyDrawer::PhotoGrainParams StelSkyDrawer::getPhotoGrainParams(const StelProjector& prj) const
{
	PhotoGrainParams p;
	if (photoNoise == PhotoNoiseOff || !getFlagPhotoFilmActive())
		return p;
	const PhotoFilmSpec& f = getPhotoActiveFilmSpec();
	double pixel = PHOTO_FILM_SCAN_PIXEL;
	if (photoNoise != PhotoNoisePixel)
	{
		const double diagonal = getPhotoFrameDiagonal(prj);
		pixel = diagonal > 0. ? getPhotoSensorDiagonal()/diagonal : PHOTO_FILM_SCAN_PIXEL;
	}
	const double aperture = M_PI*24e-6*24e-6;
	p.mode = (f.type == PhotoFilmSlide) ? 1 : 2;
	p.mono = (f.type == PhotoFilmMonochrome);
	p.sigma = static_cast<float>(f.granularity/1000. * std::sqrt(aperture/(pixel*pixel)));
	p.negativeGamma = static_cast<float>(f.gamma);
	p.printGamma = static_cast<float>(static_cast<double>(f.toneA)/f.gamma);
	p.toe = f.toneMid/16.f;
	p.seed = static_cast<float>(photoNoiseFrame) + 0.5f;
	return p;
}

// Halation. The light that passes the emulsion without being absorbed is scattered in it (the grains make it
// turbid) and reaches the film base as diffuse light. Red light gets there most: the silver halide absorbs it least
// and it passes the blue and green layers above; blue light is stopped by the yellow filter layer. The light that
// comes back from the base exposes the layer next to it, the red-sensitive one, first: halos of film are red to
// orange. The antihalation layer (dyes or colloidal silver behind the emulsion, removed in processing; a grey base
// in black and white films) absorbs most of it. The share of the light that reaches the base (red 50%, green 3%)
// and the antihalation density (0.45) are set after Till Credner's pictures on Ektachrome E200 (Jupiter, 50 mm,
// 15 min): a white core and a red ring from about 0.1 to 0.35 mm (astro/photographic_antihalation_density
// changes the density).
StelSkyDrawer::PhotoHalationParams StelSkyDrawer::getPhotoHalationParams(const StelProjector& prj) const
{
	PhotoHalationParams p;
	if (!getFlagPhotoFilmActive() || !getFlagPhotoBloomActive())
		return p;
	const PhotoFilmSpec& f = getPhotoActiveFilmSpec();
	const double diagonal = getPhotoFrameDiagonal(prj);
	if (!(diagonal > 0.))
		return p;
	const double pixel = getPhotoSensorDiagonal()/diagonal;  // meters on the film
	const double thetaC = std::asin(1./1.48);
	p.radius = static_cast<float>(2.*PHOTO_FILM_BASE_THICKNESS*std::tan(thetaC)/pixel);
	const bool mono = (f.type == PhotoFilmMonochrome);
	p.density = photoAntihalationDensity >= 0.f ? photoAntihalationDensity : (mono ? 0.5f : 0.45f);
	const Vec3f reach = mono ? Vec3f(0.3f, 0.3f, 0.3f) : Vec3f(0.5f, 0.03f, 0.f);
	p.gain = reach * static_cast<float>(StelPhotoBloom::halationReturned(p.density));
	return p;
}
