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

// Photographic mode of StelSkyDrawer: the camera settings: exposure, aperture, ISO, tone curve, lens, sensor,
// frame, focus, noise, mount and lens defects; saving and resetting the settings.

#include "StelSkyDrawer.hpp"
#include "StelProjector.hpp"
#include "StelApp.hpp"
#include "StelCore.hpp"
#include "StelMovementMgr.hpp"
#include "StelMainView.hpp"

#include <QSettings>
#include <QDebug>
#include <QtGlobal>
#include <algorithm>
#include <cmath>

void StelSkyDrawer::setFlagPhotographicMode(bool b)
{
	if (b==flagPhotographicMode)
		return;
	// The two modes exclude each other
	if (b && flagNakedEyeMode)
		setFlagNakedEyeMode(false);
	flagPhotographicMode=b;
	StelApp::immediateSave("viewing/flag_photographic_mode", b);
	emit flagPhotographicModeChanged(b);
	if (b && !StelMainView::getInstance().getGLInformation().isHighGraphicsMode)
		qWarning() << "Photographic mode needs high graphics mode (OpenGL 3 or later). It will have no effect.";
}

void StelSkyDrawer::setFlagEyeInstantAdaptation(bool b)
{
	if (b == flagEyeInstantAdaptation)
		return;
	flagEyeInstantAdaptation = b;
	StelApp::immediateSave("viewing/flag_naked_eye_instant_adaptation", b);
	emit flagEyeInstantAdaptationChanged(b);
}

void StelSkyDrawer::setEyeAge(double years)
{
	years = qBound(EYE_MIN_AGE, years, EYE_MAX_AGE);
	if (qFuzzyCompare(years, eyeAge))
		return;
	eyeAge = years;
	StelApp::immediateSave("viewing/naked_eye_age", years);
	emit eyeAgeChanged(years);
}

void StelSkyDrawer::setFlagNakedEyeMode(bool b)
{
	if (b==flagNakedEyeMode)
		return;
	if (b && flagPhotographicMode)
		setFlagPhotographicMode(false);
	flagNakedEyeMode=b;
	// The eye adapts to the view it finds
	eyeAdaptationFresh = true;
	StelApp::immediateSave("viewing/flag_naked_eye_mode", b);
	emit flagNakedEyeModeChanged(b);
	if (b && !StelMainView::getInstance().getGLInformation().isHighGraphicsMode)
		qWarning() << "Naked eye mode needs high graphics mode (OpenGL 3 or later). It will have no effect.";
}

void StelSkyDrawer::setPhotoExposureEV(double ev)
{
	ev = qBound(-12., ev, 12.);
	if (qFuzzyCompare(1.+ev, 1.+photoExposureEV))
		return;
	photoExposureEV=ev;
	StelApp::immediateSave("viewing/photographic_exposure_ev", ev);
	emit photoExposureEVChanged(ev);
}

void StelSkyDrawer::increasePhotoExposure()
{
	// Keep the value on the 1/3-stop grid, like the dial of a camera
	setPhotoExposureEV((std::round(photoExposureEV*3.)+1.)/3.);
	showPhotoExposureMessage();
}

void StelSkyDrawer::decreasePhotoExposure()
{
	setPhotoExposureEV((std::round(photoExposureEV*3.)-1.)/3.);
	showPhotoExposureMessage();
}

void StelSkyDrawer::resetPhotoExposure()
{
	setPhotoExposureEV(0.);
	showPhotoExposureMessage();
}

float StelSkyDrawer::getPhotoExposureFactor() const
{
	// With the automatic exposure, the compensation changes the exposure time instead (as in a camera)
	if (getFlagPhotoAutoExposureActive())
		return 1.f;
	return static_cast<float>(std::exp2(photoExposureEV));
}

bool StelSkyDrawer::getFlagPhotographicModeActive() const
{
	return (flagPhotographicMode || flagNakedEyeMode) && StelMainView::getInstance().getGLInformation().isHighGraphicsMode;
}

bool StelSkyDrawer::getFlagPhotoCameraActive() const
{
	return flagPhotographicMode && !flagNakedEyeMode && StelMainView::getInstance().getGLInformation().isHighGraphicsMode;
}

bool StelSkyDrawer::getFlagNakedEyeModeActive() const
{
	return flagNakedEyeMode && StelMainView::getInstance().getGLInformation().isHighGraphicsMode;
}

void StelSkyDrawer::setPhotoExposureTime(double seconds)
{
	seconds = qBound(1./8000., seconds, 3600.);
	if (qFuzzyCompare(seconds, photoExposureTime))
		return;
	photoExposureTime=seconds;
	StelApp::immediateSave("viewing/photographic_exposure_time", seconds);
	emit photoExposureTimeChanged(seconds);
	announcePhotoExposureTime();
}

void StelSkyDrawer::setPhotoLensMaxAperture(double fNumber)
{
	fNumber = qBound(PHOTO_LENS_MAX_APERTURE_MIN, fNumber, PHOTO_LENS_MAX_APERTURE_MAX);
	if (qFuzzyCompare(fNumber, photoLensMaxAperture))
		return;
	photoLensMaxAperture = fNumber;
	StelApp::immediateSave("viewing/photographic_lens_max_aperture", fNumber);
	emit photoLensMaxApertureChanged(fNumber);
	// The lens cannot open wider than it is: the aperture of the user (and of a running program) follows
	if (photoProgramRunning && photoUserAperture < fNumber)
		setPhotoAperture(fNumber);
	else if (!photoProgramRunning && photoAperture < fNumber)
		setPhotoAperture(fNumber);
	photoAutoExposureFresh = true;
}

void StelSkyDrawer::setPhotoAperture(double fNumber)
{
	fNumber = qBound(photoLensMaxAperture, fNumber, 64.);
	// The program mode chooses the aperture: a setting now is the user's, used again when it stops
	if (photoProgramRunning)
	{
		photoUserAperture = fNumber;
		StelApp::immediateSave("viewing/photographic_aperture", fNumber);
		// A locked aperture is used by the program at once
		if (flagPhotoProgramApertureLock && !qFuzzyCompare(fNumber, photoAperture))
		{
			photoAperture = fNumber;
			emit photoApertureChanged(fNumber);
		}
		return;
	}
	if (qFuzzyCompare(fNumber, photoAperture))
		return;
	// Aperture priority: the camera keeps the exposure and changes the time at once
	if (flagPhotoAutoExposure)
		photoAutoExposureTime = qBound(PHOTO_AUTO_MIN_TIME, photoAutoExposureTime*(fNumber*fNumber)/(photoAperture*photoAperture), PHOTO_AUTO_MAX_TIME);
	photoAperture=fNumber;
	StelApp::immediateSave("viewing/photographic_aperture", fNumber);
	emit photoApertureChanged(fNumber);
}

void StelSkyDrawer::setPhotoISO(double iso)
{
	iso = qBound(25., iso, 409600.);
	// With a film the speed is the film's: a setting now is the digital sensor's, used again without the film
	if (photoFilm != PhotoFilmNone)
	{
		photoDigitalISO = iso;
		StelApp::immediateSave("viewing/photographic_iso", iso);
		return;
	}
	if (photoProgramRunning)
	{
		photoUserISO = iso;
		StelApp::immediateSave("viewing/photographic_iso", iso);
		return;
	}
	if (qFuzzyCompare(iso, photoISO))
		return;
	if (flagPhotoAutoExposure)
		photoAutoExposureTime = qBound(PHOTO_AUTO_MIN_TIME, photoAutoExposureTime*photoISO/iso, PHOTO_AUTO_MAX_TIME);
	photoISO=iso;
	StelApp::immediateSave("viewing/photographic_iso", iso);
	emit photoISOChanged(iso);
}

float StelSkyDrawer::invertPhotoShoulder(float y)
{
	constexpr float knee = PHOTO_SHOULDER_KNEE;
	if (y <= knee)
		return y;
	// The shoulder never quite reaches 1.0, so limit the target value a little below it
	y = qMin(y, 0.995f);
	return knee - (1.f-knee) * std::log(1.f - (y-knee)/(1.f-knee));
}

void StelSkyDrawer::updatePhotoToneCurve()
{
	// Contrast a, shoulder d and the clipping point (in scene units, middle grey = 0.18) of each curve.
	// Standard: the curve of a camera JPEG, measured on a Canon EOS R6 (ISO 6400, standard picture style): its
	// own JPEG against its raw data, pixel by pixel over a night sky picture (raw levels from 0.1% to 10% of
	// saturation, sRGB 2-135). Middle grey (sRGB 118) lies 3.65 stops below saturation, as the standard output
	// sensitivity (ISO 12232, used by the camera makers) puts the ISO speed, not 2.96 as the saturation-based
	// speed would: 0.18 * 2^3.65 = 2.26. The contrast is 1.65 (rms 1.8 sRGB levels; the camera lifts the
	// deepest shadows, below 1/2 of a percent of saturation, a little more). Compared with the earlier curve
	// (contrast 1.30, saturation at 1.4) a night sky gets darker and the Milky Way stands out more.
	// Film: like a negative film, slightly soft, with 6 stops of room above middle grey (0.18 * 64).
	float a = 1.f, d = 1.f, clip = 1.f;
	double midOut = 0.18;
	switch (photoToneCurve)
	{
		case PhotoToneStandard: a = 1.65f; d = 1.f; clip = PHOTO_SENSOR_SATURATION; break;
		case PhotoToneFilm:     a = 1.15f; d = 1.f; clip = 11.5f; break;
		default: break;
	}
	// A film has its own curve (see getPhotoFilmSpec()): slide film steep, with little room above middle grey and
	// a picture a little darker than middle grey at the metered exposure; negatives printed on paper soft
	if (photoFilm != PhotoFilmNone)
	{
		const PhotoFilmSpec& f = getPhotoActiveFilmSpec();
		a = f.toneA;
		clip = f.toneClip;
		midOut = static_cast<double>(f.toneMid);
	}
	constexpr double midIn = 0.18;
	const double ha = std::pow(static_cast<double>(clip), static_cast<double>(a));
	const double had = std::pow(static_cast<double>(clip), static_cast<double>(a*d));
	const double ma = std::pow(midIn, static_cast<double>(a));
	const double mad = std::pow(midIn, static_cast<double>(a*d));
	photoToneParams.a = a;
	photoToneParams.ad = a*d;
	photoToneParams.b = static_cast<float>((-ma + ha*midOut) / ((had - mad)*midOut));
	photoToneParams.c = static_cast<float>((had*ma - ha*mad*midOut) / ((had - mad)*midOut));
	photoToneParams.clip = clip;

	// Table of the inverse for the annotations (many lookups per frame)
	constexpr int tableSize = 1024;
	photoToneInverseTable.resize(tableSize);
	for (int i=0; i<tableSize; ++i)
		photoToneInverseTable[static_cast<size_t>(i)] = photoToneCurveInverse(static_cast<float>(i)/(tableSize-1));
}

float StelSkyDrawer::photoToneCurveValue(float x) const
{
	x = qMax(0.f, x);
	if (getPhotoActiveToneCurve() == PhotoToneLinear)
	{
		constexpr float knee = PHOTO_SHOULDER_KNEE;
		if (x <= knee)
			return x;
		return knee + (1.f-knee)*(1.f - std::exp(-(x-knee)/(1.f-knee)));
	}
	const PhotoToneParams& p = photoToneParams;
	if (x >= p.clip)
		return 1.f;
	return std::pow(x, p.a) / (std::pow(x, p.ad)*p.b + p.c);
}

float StelSkyDrawer::photoToneCurveInverse(float y) const
{
	if (getPhotoActiveToneCurve() == PhotoToneLinear)
		return invertPhotoShoulder(qMax(0.f, y));
	if (!(y > 0.f))
		return 0.f;
	if (y >= 1.f)
		return photoToneParams.clip;
	// The curve rises monotonically from 0 to 1 between 0 and the clipping point: bisection
	float lo = 0.f, hi = photoToneParams.clip;
	for (int i=0; i<40; ++i)
	{
		const float mid = 0.5f*(lo+hi);
		if (photoToneCurveValue(mid) < y)
			lo = mid;
		else
			hi = mid;
	}
	return 0.5f*(lo+hi);
}

// ---- Lens model ----

StelSkyDrawer::PhotoLens StelSkyDrawer::getPhotoLens(const StelProjector& prj) const
{
	PhotoLens lens;
	const auto c = prj.getViewportCenter();
	lens.center.set(static_cast<float>(c[0]) - static_cast<float>(prj.getViewportPosX()),
	                static_cast<float>(c[1]) - static_cast<float>(prj.getViewportPosY()));
	// the corners of the picture: the corners of the frame of the sensor
	lens.corner = qMax(1.f, 0.5f*static_cast<float>(getPhotoFrameDiagonal(prj)));
	// Rectilinear lens: tan(field angle) = distance from the center / focal length (pixels)
	lens.focal = qMax(1.f, prj.getPixelPerRadAtCenter());
	const float N = static_cast<float>(photoAperture);
	const float wideOpen = static_cast<float>(photoLensMaxAperture);
	lens.pupil = qMin(1.f, wideOpen/N);
	lens.vignetting = flagPhotoVignetting && getFlagPhotoCameraActive();
	if (flagPhotoAberrations && getFlagPhotoCameraActive())
	{
		// Third-order aberrations: the coma grows with the field and the square of the aperture,
		// the astigmatism with the square of the field and the aperture (the field factors are in the shader)
		const float diagonal = 2.f*lens.corner;
		const float open = wideOpen/N;
		lens.coma = photoAberrationStrength * PHOTO_COMA_LENGTH * diagonal * open*open;
		lens.astigmatism = photoAberrationStrength * PHOTO_ASTIGMATISM_LENGTH * diagonal * open;
		// Spherical aberration: like the coma it grows with the square of the aperture (relative to the defocus)
		lens.sphericalAberration = qMin(0.9f, photoAberrationStrength * PHOTO_SPHERICAL_ABERRATION * open*open);
		// Chromatic aberration: the lateral part is a difference of magnification (a shift that grows with the
		// field), the axial part a difference of focus (a blur disk of radius shift/(2N), here in pixels: the
		// shift as a fraction of f times the focal length in pixels)
		const float lateral = photoAberrationStrength * PHOTO_LATERAL_CA * diagonal;
		lens.chromaShift.set(lateral, 0.f, -lateral);
		const float blur = photoAberrationStrength * 0.5f * lens.focal / N;
		lens.chromaBlur.set(blur*PHOTO_AXIAL_CA_RED, 0.f, blur*PHOTO_AXIAL_CA_BLUE);
	}
	if (photoFocusDistance != 0. && getFlagPhotoCameraActive())
	{
		// Focused at the distance s, the image of a star (at infinity) lies f^2/(s-f) in front of the sensor.
		// The cone of light from the aperture (diameter f/N) cuts the sensor in a disk of diameter
		// c = f^2/(N s) (exact for a thin lens). In pixels of the sensor (focal length f in pixels):
		// c = (1/s) f [m] f [px] / N. Beyond infinity the sign changes (the image lies behind the sensor).
		const double f = getPhotoFocalLength(prj);
		const double dioptres = 1./photoFocusDistance;
		lens.defocus = static_cast<float>(0.5 * dioptres * f * static_cast<double>(lens.focal) / photoAperture);
		// Very small blur: as good as in focus
		if (std::fabs(lens.defocus) < 0.05f)
			lens.defocus = 0.f;
	}
	return lens;
}

double StelSkyDrawer::getPhotoFocalLength(const StelProjector& prj) const
{
	// The diagonal of the frame in the view is the diagonal of the sensor
	const double diagonal = getPhotoFrameDiagonal(prj);
	return diagonal > 0. ? getPhotoSensorDiagonal() * prj.getPixelPerRadAtCenter() / diagonal : 0.05;
}

// The projection maps the field radius to pixels: pixelPerRad = (fov disk diameter)/2 / scaling(fov/2).
// The current projector gives the diameter, then the focal length in pixels follows for any field.
double StelSkyDrawer::getPhotoFocalLengthForFov(double fov) const
{
	const StelProjectorP prj = core->getProjection(StelCore::FrameJ2000);
	const double diagonal = getPhotoFrameDiagonal(*prj);
	const double halfDisk = static_cast<double>(prj->getPixelPerRadAtCenter()) * static_cast<double>(prj->fovToViewScalingFactor(static_cast<float>(M_PI/360.*static_cast<double>(prj->getFov()))));
	const double scaling = static_cast<double>(prj->fovToViewScalingFactor(static_cast<float>(M_PI/360.*fov)));
	if (!(diagonal > 0.) || !(scaling > 0.))
		return 0.05;
	return getPhotoSensorDiagonal() * (halfDisk/scaling) / diagonal;
}

double StelSkyDrawer::getPhotoFovForFocalLength(double f) const
{
	const StelProjectorP prj = core->getProjection(StelCore::FrameJ2000);
	const double diagonal = getPhotoFrameDiagonal(*prj);
	const double halfDisk = static_cast<double>(prj->getPixelPerRadAtCenter()) * static_cast<double>(prj->fovToViewScalingFactor(static_cast<float>(M_PI/360.*static_cast<double>(prj->getFov()))));
	const double focalPixels = f * diagonal / getPhotoSensorDiagonal();
	if (!(focalPixels > 0.))
		return static_cast<double>(prj->getFov());
	return 360./M_PI * static_cast<double>(prj->viewScalingFactorToFov(static_cast<float>(halfDisk/focalPixels)));
}

void StelSkyDrawer::setPhotoFocalLength(double millimeters)
{
	if (!(millimeters > 0.))
		return;
	core->getMovementMgr()->zoomTo(getPhotoFovForFocalLength(millimeters/1000.), 0.5f);
}

// The focal lengths of lenses that are common in astrophotography and landscape photography: the wide end of the
// zoom lenses of compact cameras (small sensors), fisheyes and wide angles for the Milky Way and star trails,
// standard and short telephoto lenses for constellations, long telephoto lenses and small telescopes for
// nebulae, and the telescopes of amateurs (an 8" SCT is about 2000 mm).
const std::vector<double>& StelSkyDrawer::getPhotoCommonFocalLengths()
{
	static const std::vector<double> focalLengths = {
		4., 4.5, 5., 6., 8., 10., 12., 14., 15., 16., 17., 18., 20., 24., 28., 35., 40., 50., 70., 75., 85., 100.,
		105., 135., 180., 200., 250., 300., 400., 500., 600., 800., 1000., 1200., 1500., 2000., 2500., 3000.};
	return focalLengths;
}

namespace
{
//! The common focal length nearest to @a mm, in their ratio (as the zoom goes)
double nearestCommonFocalLength(double mm)
{
	const std::vector<double>& steps = StelSkyDrawer::getPhotoCommonFocalLengths();
	double nearest = steps.front();
	for (const double f : steps)
		if (std::fabs(std::log(f/mm)) < std::fabs(std::log(nearest/mm)))
			nearest = f;
	return nearest;
}
}

void StelSkyDrawer::setPhotoZoomSteps(int steps)
{
	const int s = qBound(static_cast<int>(PhotoZoomFree), steps, static_cast<int>(PhotoZoomCommonFocalLengths));
	if (s == photoZoomSteps)
		return;
	photoZoomSteps = s;
	// Turning it on sets the lens to the nearest step right away
	if (s != PhotoZoomFree && getFlagPhotoCameraActive())
	{
		const double mm = 1000.*getPhotoFocalLengthForFov(core->getMovementMgr()->getAimFov());
		if (mm > 0. && std::isfinite(mm))
			setPhotoFocalLength(s == PhotoZoomCommonFocalLengths ? nearestCommonFocalLength(mm) : qMax(1., std::round(mm)));
	}
	StelApp::immediateSave("viewing/photographic_zoom_steps", s);
	emit photoZoomStepsChanged(s);
}

double StelSkyDrawer::getPhotoWheelZoomFov(double currentFov, double targetFov) const
{
	if (photoZoomSteps == PhotoZoomFree || !getFlagPhotoCameraActive())
		return targetFov;
	const double current = 1000.*getPhotoFocalLengthForFov(currentFov);
	const double target = 1000.*getPhotoFocalLengthForFov(targetFov);
	if (!(current > 0.) || !(target > 0.) || !std::isfinite(target))
		return targetFov;
	const bool zoomIn = targetFov < currentFov;   // longer focal length
	const bool zoomOut = targetFov > currentFov;  // shorter focal length
	double mm = target;
	if (photoZoomSteps == PhotoZoomWholeMillimeters)
	{
		// The nearest whole millimeter, but at least the next one in the direction of the zoom, so that every
		// step of the wheel (also the small steps of a touchpad) changes the lens. The tolerance keeps a lens
		// already at a whole focal length (up to rounding) from counting as between two.
		constexpr double tolerance = 0.01;
		mm = std::round(target);
		if (zoomIn)
			mm = qMax(mm, std::floor(current + tolerance) + 1.);
		else if (zoomOut)
			mm = qMin(mm, std::ceil(current - tolerance) - 1.);
		mm = qMax(mm, 1.);
	}
	else
	{
		// Each step of the wheel changes the lens to the next common focal length in the direction of the zoom, as
		// one would take the next lens out of the bag; beyond the first and the last one the lens stays there
		const std::vector<double>& steps = getPhotoCommonFocalLengths();
		constexpr double tolerance = 1.001; // a lens already at a step (up to rounding) is not between two
		if (zoomIn)
		{
			const auto next = std::upper_bound(steps.cbegin(), steps.cend(), current*tolerance);
			if (next == steps.cend())
				return currentFov;
			mm = *next;
		}
		else if (zoomOut)
		{
			const auto next = std::lower_bound(steps.cbegin(), steps.cend(), current/tolerance);
			if (next == steps.cbegin())
				return currentFov;
			mm = *(next - 1);
		}
		else
			mm = nearestCommonFocalLength(current);
	}
	return getPhotoFovForFocalLength(mm/1000.);
}

const StelSkyDrawer::PhotoSensorSpec& StelSkyDrawer::getPhotoSensorSpec(int format)
{
	// Typical cameras of each format: pitch from the sensor width and the number of photosites across it
	static const PhotoSensorSpec specs[PHOTO_SENSOR_FORMAT_COUNT] = {
		{ PHOTO_SENSOR_DIAGONAL, PHOTO_SENSOR_PITCH, "", 36.0/24.0 },  // full frame: 36 mm / 6000 (24 Mpx)
		{ 0.028207, 3.92e-6, "APS-C", 23.5/15.6 },                     // 23.5 mm / 6000 (24 Mpx)
		{ 0.026820, 3.72e-6, "APS-C 1.6x", 22.3/14.9 },                // 22.3 mm / 6000 (24 Mpx)
		{ 0.021640, 3.34e-6, "4/3", 17.3/13.0 },                       // 17.3 mm / 5184 (20 Mpx)
		{ 0.015864, 2.41e-6, "1\"", 13.2/8.8 },                        // 13.2 mm / 5472 (20 Mpx)
		{ 0.054780, 5.30e-6, "MF", 43.8/32.9 },                        // 43.8 mm / 8256 (51 Mpx)
		{ 0.011000, 2.20e-6, "2/3\"", 8.8/6.6 },                      // 8.8 mm / 4000 (12 Mpx)
		{ 0.009500, 2.08e-6, "1/1.7\"", 7.6/5.7 },                    // 7.6 mm / 3648 (10 Mpx)
		{ 0.007666, 1.54e-6, "1/2.3\"", 6.17/4.55 }                   // 6.17 mm / 4000 (12 Mpx)
	};
	return specs[qBound(0, format, PHOTO_SENSOR_FORMAT_COUNT - 1)];
}

Vec4f StelSkyDrawer::getPhotoFrame(const StelProjector& prj) const
{
	const double vx = prj.getViewportPosX(), vy = prj.getViewportPosY();
	const double vw = prj.getViewportWidth(), vh = prj.getViewportHeight();
	// The eye: no sensor, the whole view
	if (getFlagNakedEyeModeActive())
		return Vec4f(static_cast<float>(vx), static_cast<float>(vy), static_cast<float>(vw), static_cast<float>(vh));
	const auto c = prj.getViewportCenter();
	// The largest frame around the optical axis that stays in the viewport
	const double halfW = qMax(1., qMin(c[0] - vx, vx + vw - c[0]));
	const double halfH = qMax(1., qMin(c[1] - vy, vy + vh - c[1]));
	double aspect = getPhotoSensorSpec(getPhotoEffectiveSensorFormat()).aspect;
	if (flagPhotoPortrait)
		aspect = 1./aspect;
	double w = 2.*halfW, h = 2.*halfH;
	if (w > aspect*h)
		w = aspect*h;
	else
		h = w/aspect;
	return Vec4f(static_cast<float>(c[0] - 0.5*w), static_cast<float>(c[1] - 0.5*h), static_cast<float>(w), static_cast<float>(h));
}

double StelSkyDrawer::getPhotoFrameDiagonal(const StelProjector& prj) const
{
	const Vec4f frame = getPhotoFrame(prj);
	return std::hypot(static_cast<double>(frame[2]), static_cast<double>(frame[3]));
}

void StelSkyDrawer::setFlagPhotoPortrait(bool b)
{
	if (b == flagPhotoPortrait)
		return;
	// The same lens on the turned camera: keep the focal length, the frame (and so the field of view) changes
	const double focal = getPhotoFocalLengthForFov(core->getMovementMgr()->getAimFov());
	flagPhotoPortrait = b;
	if (getFlagPhotoCameraActive())
		core->getMovementMgr()->zoomTo(getPhotoFovForFocalLength(focal), 0.5f);
	StelApp::immediateSave("viewing/flag_photographic_portrait", b);
	emit flagPhotoPortraitChanged(b);
}

void StelSkyDrawer::setPhotoEclipseAtmosphere(int state)
{
	const int s = qBound(0, state, 3);
	if (s == photoEclipseAtmosphere)
		return;
	photoEclipseAtmosphere = s;
	StelApp::immediateSave("viewing/photographic_eclipse_atmosphere", s);
	emit photoEclipseAtmosphereChanged(s);
}

void StelSkyDrawer::setFlagPhotoFrameMask(bool b)
{
	if (b == flagPhotoFrameMask)
		return;
	flagPhotoFrameMask = b;
	StelApp::immediateSave("viewing/flag_photographic_frame_mask", b);
	emit flagPhotoFrameMaskChanged(b);
}

void StelSkyDrawer::setPhotoSensorFormat(int format)
{
	const int f = qBound(0, format, PHOTO_SENSOR_FORMAT_COUNT - 1);
	if (f == photoSensorFormat)
		return;
	// The same lens on the other camera: keep the focal length, the field of view changes (the crop factor)
	const double focal = getPhotoFocalLengthForFov(core->getMovementMgr()->getAimFov());
	photoSensorFormat = f;
	if (getFlagPhotoCameraActive())
		core->getMovementMgr()->zoomTo(getPhotoFovForFocalLength(focal), 0.5f);
	StelApp::immediateSave("viewing/photographic_sensor_format", f);
	emit photoSensorFormatChanged(f);
}

void StelSkyDrawer::setPhotoFocusDistance(double meters)
{
	double s = std::isfinite(meters) ? meters : 0.;
	// 0 = infinity; beyond the closest focus and beyond the end stop past infinity the ring does not turn
	if (s > 0.)
		s = qMax(s, PHOTO_FOCUS_MIN_DISTANCE);
	else if (s < 0.)
		s = qMin(s, -PHOTO_FOCUS_BEYOND_INFINITY);
	if (s > 1e7 || s < -1e7)
		s = 0.;
	if (qFuzzyCompare(1. + s, 1. + photoFocusDistance) && (s == 0.) == (photoFocusDistance == 0.))
		return;
	photoFocusDistance = s;
	StelApp::immediateSave("viewing/photographic_focus_distance", s);
	emit photoFocusDistanceChanged(s);
}

void StelSkyDrawer::setPhotoNoise(int mode)
{
	const int m = qBound(static_cast<int>(PhotoNoiseOff), mode, static_cast<int>(PhotoNoisePixel));
	if (m == photoNoise)
		return;
	photoNoise = m;
	StelApp::immediateSave("viewing/photographic_noise", m);
	emit photoNoiseChanged(m);
}

StelSkyDrawer::PhotoNoiseParams StelSkyDrawer::getPhotoNoiseParams(const StelProjector& prj) const
{
	PhotoNoiseParams p;
	if (photoNoise == PhotoNoiseOff || !getFlagPhotoCameraActive() || getFlagPhotoFilmActive())
		return p;
	p.enabled = true;
	// The sensor collects the light in electrons. A photosite saturates at the scene value
	// PHOTO_SENSOR_SATURATION (the same point where the standard tone curve clips): at the base ISO that is the
	// full well, at a higher ISO the amplifier reaches white with proportionally fewer electrons. That is why a high ISO is noisy: for the same
	// brightness in the picture the sensor has caught fewer photons (for the same exposure time and aperture,
	// the photons are the same, but so is the noise relative to the signal).
	const float iso = qMax(static_cast<float>(photoISO), PHOTO_SENSOR_BASE_ISO);
	// A smaller photosite holds fewer electrons and collects less dark current, in proportion to its area
	const double pitch = getPhotoSensorPitch();
	const float area = static_cast<float>((pitch*pitch)/(PHOTO_SENSOR_PITCH*PHOTO_SENSOR_PITCH));
	p.white = PHOTO_SENSOR_FULL_WELL * area * PHOTO_SENSOR_BASE_ISO / iso / PHOTO_SENSOR_SATURATION;
	// Read noise: the part after the amplifier shrinks (in electrons) at higher ISO ("ISO invariance")
	const float downstream = PHOTO_READ_NOISE_DOWNSTREAM * PHOTO_SENSOR_BASE_ISO / iso;
	p.read2 = PHOTO_READ_NOISE_UPSTREAM*PHOTO_READ_NOISE_UPSTREAM + downstream*downstream;
	// Dark current grows with the temperature of the sensor: the air temperature (atmosphere settings) plus its own warming
	const float t = static_cast<float>(getPhotoEffectiveExposureTime());
	const float celsius = static_cast<float>(getAtmosphereTemperature()) + PHOTO_SENSOR_WARMING;
	const float thermal = std::exp2((celsius - 20.f)/PHOTO_DARK_DOUBLING);
	p.dark = PHOTO_DARK_CURRENT * area * thermal * t;
	p.hot = PHOTO_HOT_PIXEL_CURRENT * thermal * t;
	// Photosites per screen pixel: the frame in the view is the sensor (its diagonal), see getPhotoFocalLength()
	if (photoNoise == PhotoNoisePixel)
		p.sites = 1.f;
	else
	{
		const double diagonal = getPhotoFrameDiagonal(prj);
		const double pixel = diagonal > 0. ? getPhotoSensorDiagonal()/diagonal : pitch;
		p.sites = static_cast<float>(qMax(1., (pixel*pixel)/(pitch*pitch)));
	}
	p.hotProbability = qMin(1.f, 1.f - std::pow(1.f - PHOTO_HOT_PIXEL_FRACTION, p.sites));
	p.seed = static_cast<float>(photoNoiseFrame) + 0.5f;
	return p;
}

void StelSkyDrawer::setPhotoMount(int mount)
{
	const int m = qBound(static_cast<int>(PhotoMountTripod), mount, static_cast<int>(PhotoMountView));
	if (m == photoMount)
		return;
	photoMount = m;
	StelApp::immediateSave("viewing/photographic_mount", m);
	emit photoMountChanged(m);
}

int StelSkyDrawer::getPhotoEffectiveMount() const
{
	if (photoMount != PhotoMountView)
		return photoMount;
	// As the view: on the alt-azimuthal mount the camera stands still (the sky turns past it), on the others
	// (equatorial, galactic) the view turns with the sky, like a star tracker
	const StelMovementMgr* mvmgr = core ? core->getMovementMgr() : nullptr;
	if (!mvmgr || mvmgr->getMountMode() == StelMovementMgr::MountAltAzimuthal)
		return PhotoMountTripod;
	return PhotoMountTracker;
}

void StelSkyDrawer::setFlagPhotoVignetting(bool b)
{
	if (b == flagPhotoVignetting)
		return;
	flagPhotoVignetting = b;
	StelApp::immediateSave("viewing/flag_photographic_vignetting", b);
	emit flagPhotoVignettingChanged(b);
}

void StelSkyDrawer::setFlagPhotoHalphaModified(bool b)
{
	if (b == flagPhotoHalphaModified)
		return;
	flagPhotoHalphaModified = b;
	StelApp::immediateSave("viewing/flag_photographic_halpha_modified", b);
	emit flagPhotoHalphaModifiedChanged(b);
}

Vec3f StelSkyDrawer::getPhotoEmissionColor() const
{
	if (getFlagPhotoFilmActive())
	{
		const PhotoFilmSpec& f = getPhotoActiveFilmSpec();
		if (!getPhotoFilmSpectral(f))
			return Vec3f(-1.f);
		return Vec3f(f.emission[0], f.emission[1], f.emission[2]);
	}
	const float* e = flagPhotoHalphaModified ? PHOTO_EMISSION_DIGITAL_HALPHA : PHOTO_EMISSION_DIGITAL;
	return Vec3f(e[0], e[1], e[2]);
}

void StelSkyDrawer::setFlagPhotoLensFlare(bool b)
{
	if (b == flagPhotoLensFlare)
		return;
	flagPhotoLensFlare = b;
	StelApp::immediateSave("viewing/flag_photographic_lens_flare", b);
	emit flagPhotoLensFlareChanged(b);
}

void StelSkyDrawer::setPhotoLensFlareStrength(double strength)
{
	const float v = qBound(0.f, static_cast<float>(strength), 3.f);
	if (qFuzzyCompare(1.f+v, 1.f+photoLensFlareStrength))
		return;
	photoLensFlareStrength = v;
	StelApp::immediateSave("viewing/photographic_lens_flare_strength", static_cast<double>(v));
	emit photoLensFlareStrengthChanged(static_cast<double>(v));
}

void StelSkyDrawer::setFlagPhotoAberrations(bool b)
{
	if (b == flagPhotoAberrations)
		return;
	flagPhotoAberrations = b;
	StelApp::immediateSave("viewing/flag_photographic_aberrations", b);
	emit flagPhotoAberrationsChanged(b);
}

void StelSkyDrawer::setPhotoAberrationStrength(double strength)
{
	const float v = qBound(0.f, static_cast<float>(strength), 3.f);
	if (qFuzzyCompare(1.f+v, 1.f+photoAberrationStrength))
		return;
	photoAberrationStrength = v;
	StelApp::immediateSave("viewing/photographic_aberration_strength", static_cast<double>(v));
	emit photoAberrationStrengthChanged(static_cast<double>(v));
}

void StelSkyDrawer::setPhotoDetectionSnr(double snr)
{
	const float v = qBound(0.f, static_cast<float>(snr), 20.f);
	if (qFuzzyCompare(1.f+v, 1.f+photoDetectionSnr))
		return;
	photoDetectionSnr = v;
	StelApp::immediateSave("viewing/photographic_detection_snr", static_cast<double>(v));
	emit photoDetectionSnrChanged(static_cast<double>(v));
}

void StelSkyDrawer::savePhotoSettings(QSettings* conf) const
{
	Q_ASSERT(conf);
	conf->setValue("viewing/flag_photographic_mode", getFlagPhotographicMode());
	conf->setValue("viewing/flag_naked_eye_mode", getFlagNakedEyeMode());
	conf->setValue("viewing/flag_naked_eye_instant_adaptation", getFlagEyeInstantAdaptation());
	conf->setValue("viewing/naked_eye_age", getEyeAge());
	conf->setValue("viewing/photographic_exposure_ev", getPhotoExposureEV());
	conf->setValue("viewing/photographic_exposure_time", getPhotoExposureTime());
	conf->setValue("viewing/photographic_aperture", photoProgramRunning ? photoUserAperture : photoAperture);
	conf->setValue("viewing/photographic_lens_max_aperture", photoLensMaxAperture);
	conf->setValue("viewing/photographic_iso", photoFilm != PhotoFilmNone ? photoDigitalISO : (photoProgramRunning ? photoUserISO : photoISO));
	conf->setValue("viewing/photographic_film", photoFilm);
	conf->setValue("viewing/photographic_film_push", photoFilmPush);
	conf->setValue("viewing/photographic_zoom_steps", getPhotoZoomSteps());
	conf->remove("viewing/flag_photographic_zoom_whole_millimeters"); // (replaced by the steps)
	conf->setValue("viewing/flag_photographic_portrait", getFlagPhotoPortrait());
	conf->setValue("viewing/photographic_eclipse_atmosphere", getPhotoEclipseAtmosphere());
	conf->setValue("viewing/flag_photographic_frame_mask", getFlagPhotoFrameMask());
	conf->setValue("viewing/photographic_sensor_format", getPhotoSensorFormat());
	conf->setValue("viewing/photographic_focus_distance", getPhotoFocusDistance());
	conf->setValue("viewing/photographic_noise", getPhotoNoise());
	conf->setValue("viewing/photographic_mount", getPhotoMount());
	conf->setValue("viewing/flag_photographic_star_trails", getFlagPhotoStarTrails());
	conf->setValue("viewing/flag_photographic_vignetting", getFlagPhotoVignetting());
	conf->setValue("viewing/flag_photographic_halpha_modified", getFlagPhotoHalphaModified());
	conf->setValue("viewing/flag_photographic_lens_flare", getFlagPhotoLensFlare());
	conf->setValue("viewing/photographic_lens_flare_strength", getPhotoLensFlareStrength());
	conf->setValue("viewing/flag_photographic_aberrations", getFlagPhotoAberrations());
	conf->setValue("viewing/photographic_aberration_strength", getPhotoAberrationStrength());
	conf->setValue("viewing/photographic_detection_snr", getPhotoDetectionSnr());
	conf->setValue("viewing/photographic_white_balance", getPhotoWhiteBalance());
	conf->setValue("viewing/photographic_white_balance_kelvin", getPhotoWhiteBalanceKelvin());
	conf->setValue("viewing/flag_photographic_extinction_color", getFlagPhotoExtinctionColor());
	conf->setValue("viewing/flag_photographic_airglow", getFlagPhotoAirglow());
	conf->setValue("astro/flag_photographic_film_spectral_sky", getFlagPhotoFilmSpectralSky());
	conf->setValue("viewing/photographic_airglow_strength", getPhotoAirglowStrength());
	conf->setValue("viewing/photographic_humidity", getPhotoHumidity());
	conf->setValue("viewing/flag_photographic_aureole", getFlagPhotoAureole());
	conf->setValue("viewing/flag_photographic_humidity", getFlagPhotoHumidity());
	conf->setValue("viewing/photographic_tone_curve", getPhotoToneCurve());
	conf->setValue("viewing/photographic_psf_min_fwhm", getPhotoPsfMinFwhm());
	conf->setValue("viewing/photographic_seeing", getPhotoSeeing());
	conf->setValue("viewing/photographic_halo_strength", static_cast<double>(photoDigitalHaloFraction));
	conf->setValue("viewing/photographic_film_halo_strength", static_cast<double>(photoFilmHaloFraction));
	conf->setValue("viewing/photographic_aperture_blades", getPhotoApertureBlades());
	conf->setValue("viewing/photographic_spike_strength", getPhotoSpikeStrength());
	conf->setValue("viewing/flag_photographic_bloom", getFlagPhotoBloom());
	conf->setValue("viewing/flag_photographic_auto_exposure", getFlagPhotoAutoExposure());
	conf->setValue("viewing/photographic_metering_mode", getPhotoMeteringMode());
	conf->setValue("viewing/flag_photographic_info_line", getFlagPhotoInfoLine());
	conf->setValue("viewing/flag_photographic_program_exposure", getFlagPhotoProgramExposure());
	conf->setValue("viewing/flag_photographic_program_aperture_lock", getFlagPhotoProgramApertureLock());
	conf->setValue("viewing/photographic_program_max_iso", getPhotoProgramMaxISO());
	conf->setValue("viewing/photographic_program_max_time", getPhotoProgramMaxTime());
}

void StelSkyDrawer::setPhotoToneCurve(int curve)
{
	curve = qBound(0, curve, 2);
	if (curve == photoToneCurve)
		return;
	photoToneCurve = curve;
	updatePhotoToneCurve();
	StelApp::immediateSave("viewing/photographic_tone_curve", curve);
	emit photoToneCurveChanged(curve);
}

Vec3f StelSkyDrawer::photoAnnotationCompensation(const Vec3f& linearColor) const
{
	const float invExposure = 1.f / getPhotoExposureFactor();
	// Undo the white balance as well (the camera stage applies it before the tone curve)
	const auto unbalance = [this](const Vec3f& v) {
		const float* m = photoWBInverse;
		return Vec3f(qMax(0.f, m[0]*v[0] + m[1]*v[1] + m[2]*v[2]),
		             qMax(0.f, m[3]*v[0] + m[4]*v[1] + m[5]*v[2]),
		             qMax(0.f, m[6]*v[0] + m[7]*v[1] + m[8]*v[2]));
	};
	if (photoToneCurve == PhotoToneLinear || photoToneInverseTable.size() < 2)
		return unbalance(Vec3f(invertPhotoShoulder(qMax(0.f, linearColor[0])) * invExposure,
		                       invertPhotoShoulder(qMax(0.f, linearColor[1])) * invExposure,
		                       invertPhotoShoulder(qMax(0.f, linearColor[2])) * invExposure));
	// Interpolate the table of the inverse curve. Keep white a little below the clipping point,
	// so that it does not depend on rounding at the edge.
	const auto lookup = [this](float y) {
		const float n = static_cast<float>(photoToneInverseTable.size() - 1);
		const float f = qBound(0.f, y, 0.998f) * n;
		const size_t i = static_cast<size_t>(f);
		const float t = f - static_cast<float>(i);
		return photoToneInverseTable[i] + t*(photoToneInverseTable[qMin(i+1, photoToneInverseTable.size()-1)] - photoToneInverseTable[i]);
	};
	return unbalance(Vec3f(lookup(linearColor[0]) * invExposure,
	                       lookup(linearColor[1]) * invExposure,
	                       lookup(linearColor[2]) * invExposure));
}

void StelSkyDrawer::resetPhotographicSettings()
{
	setPhotoExposureTime(20.);
	setPhotoLensMaxAperture(static_cast<double>(PHOTO_LENS_MAX_APERTURE));
	setPhotoAperture(2.8);
	setPhotoISO(1600.);
	setPhotoExposureEV(0.);
	setPhotoPsfMinFwhm(2.);
	setPhotoSeeing(3.);
	photoFilmHaloFraction = PHOTO_FILM_HALO_DEFAULT;
	photoDigitalHaloFraction = 0.05f;
	StelApp::immediateSave("viewing/photographic_film_halo_strength", static_cast<double>(PHOTO_FILM_HALO_DEFAULT));
	setPhotoHaloStrength(photoFilm != PhotoFilmNone ? PHOTO_FILM_HALO_DEFAULT : 0.05);
	setPhotoApertureBlades(6);
	setPhotoSpikeStrength(0.02);
	setFlagPhotoBloom(true);
	setFlagPhotoAutoExposure(false);
	setFlagPhotoProgramExposure(false);
	setFlagPhotoProgramApertureLock(false);
	setPhotoProgramMaxISO(PHOTO_PROGRAM_MAX_ISO);
	setPhotoProgramMaxTime(PHOTO_AUTO_MAX_TIME);
	setPhotoMeteringMode(0);
	setPhotoToneCurve(PhotoToneStandard);
	setFlagPhotoExtinctionColor(true);
	setFlagPhotoAirglow(true);
	setPhotoAirglowStrength(1.);
	setPhotoWhiteBalance(PhotoWBDaylight);
	setPhotoWhiteBalanceKelvin(5200.);
	setFlagPhotoVignetting(true);
	setFlagPhotoHalphaModified(false);
	setFlagPhotoAberrations(true);
	setPhotoAberrationStrength(1.);
	setPhotoDetectionSnr(3.);
	setFlagPhotoLensFlare(true);
	setPhotoLensFlareStrength(1.);
	setPhotoFocusDistance(0.);
	setFlagPhotoStarTrails(true);
	setPhotoMount(PhotoMountTripod);
	setPhotoNoise(PhotoNoiseImage);
	setPhotoFilmPush(0);
	setFlagPhotoFilmSpectralSky(false);
	setPhotoFilm(PhotoFilmNone);
	setPhotoSensorFormat(PhotoSensorFullFrame);
	setPhotoZoomSteps(PhotoZoomFree);
	setFlagPhotoPortrait(false);
	setFlagPhotoFrameMask(true);
	setPhotoEclipseAtmosphere(1);
}
