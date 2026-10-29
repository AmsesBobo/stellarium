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

// Photographic mode of StelSkyDrawer: automatic exposure (aperture priority and program mode), metering, the
// exposure times a camera shows and the information line.

#include "StelSkyDrawer.hpp"
#include "StelProjector.hpp"
#include "StelApp.hpp"
#include "StelCore.hpp"
#include "StelPainter.hpp"
#include "StelTranslator.hpp"
#include "StelModuleMgr.hpp"
#include "LabelMgr.hpp"
#include "StelPropertyMgr.hpp"
#include "LandscapeMgr.hpp"
#include "LightDomes.hpp"
#include "StelLocation.hpp"

#include <QStringList>
#include <QtGlobal>
#include <QGuiApplication>
#include <QFontMetrics>
#include <QLocale>

double StelSkyDrawer::getPhotoMeterClipLevel() const
{
	const double clip = (getPhotoActiveToneCurve() == PhotoToneLinear) ? 1. : static_cast<double>(photoToneParams.clip);
	return clip / static_cast<double>(getPhotoExposureFactor());
}

double StelSkyDrawer::eyeAdaptationDisplayValue(double luminance)
{
	const double L = qMax(0., luminance);
	return PHOTO_MIDDLE_GREY * std::pow(L/(L + EYE_TONE_HALF_LUMINANCE), EYE_TONE_POWER);
}

float StelSkyDrawer::getPhotoSceneScale() const
{
	// The eye: the adaptation luminance is shown at its display value (see eyeAdaptationDisplayValue())
	if (getFlagNakedEyeModeActive())
	{
		// The adaptation works with the light of an observer of the reference age (see getEyeAdaptationLuminance()):
		// the scene gets there through the light factor of the pupil
		const double L = qMax(1e-7, eyeAdaptation.getAdaptationLuminance());
		return static_cast<float>(eyeLightFactor*eyeAdaptationDisplayValue(L)/L);
	}
	// Reflected-light meter calibration constant K (ISO 2720), and middle grey
	constexpr double K = 12.5;
	constexpr double middleGrey = 0.18;
	// The exposure time as given. With a film the reciprocity failure acts on each point of the picture after
	// this, according to its own brightness (see getPhotoReciprocityParams()); the meter of the camera does not
	// know it.
	const double t = flagPhotoReciprocityPerPoint ? getPhotoEffectiveExposureTime() : getPhotoFilmEffectiveTime(getPhotoEffectiveExposureTime());
	return static_cast<float>(middleGrey * t * photoISO / (K * photoAperture * photoAperture));
}

// ---- Automatic exposure (aperture priority) ----

namespace
{
// Exposure times in 1/3-stop steps, as a camera shows them
const double nominalExposureTimes[] = {
	1./8000, 1./6400, 1./5000, 1./4000, 1./3200, 1./2500, 1./2000, 1./1600, 1./1250, 1./1000,
	1./800, 1./640, 1./500, 1./400, 1./320, 1./250, 1./200, 1./160, 1./125, 1./100,
	1./80, 1./60, 1./50, 1./40, 1./30, 1./25, 1./20, 1./15, 1./13, 1./10,
	1./8, 1./6, 1./5, 1./4, 0.3, 0.4, 0.5, 0.6, 0.8, 1., 1.3, 1.6, 2., 2.5, 3.2, 4., 5., 6., 8., 10.,
	13., 15., 20., 25., 30., 40., 50., 60., 90., 120., 180., 240., 300., 600., 900., 1200., 1800., 3600. };

float linearToSRGB(float c)
{
	return c <= 0.0031308f ? 12.92f*c : 1.055f*std::pow(c, 1.f/2.4f) - 0.055f;
}
}

double StelSkyDrawer::photoNominalExposureTime(double seconds)
{
	if (!(seconds > 0.))
		return seconds;
	double best = seconds;
	double bestDistance = 1e300;
	for (const double t : nominalExposureTimes)
	{
		const double distance = std::fabs(std::log(t/seconds));
		if (distance < bestDistance)
		{
			bestDistance = distance;
			best = t;
		}
	}
	// Outside the table (or between its sparse long values): keep the true value
	return bestDistance < 0.2 ? best : seconds;
}

double StelSkyDrawer::photoNominalFocalLength(double millimeters)
{
	return millimeters < 9.95 ? std::round(10.*millimeters)/10. : std::round(millimeters);
}

QString StelSkyDrawer::photoFocusDistanceText(double meters)
{
	const QString infinity = QString(QChar(0x221E));
	if (meters == 0.)
		return infinity;
	if (meters < 0.)
	{
		// Beyond the infinity mark: by how much, in dioptres (the unit of the focus ring travel)
		return QString("%1 %2 (%3 %4)").arg(qc_("beyond", "focus: beyond infinity"), infinity,
		                                    QLocale().toString(1./std::fabs(meters), 'f', 3), qc_("dpt", "unit: dioptre"));
	}
	if (meters >= 1000.)
		return QString("%1 %2").arg(QLocale().toString(meters/1000., 'g', 2), qc_("km", "distance"));
	return QString("%1 %2").arg(QLocale().toString(meters, 'g', meters < 10. ? 2 : 3), qc_("m", "distance"));
}

QString StelSkyDrawer::photoExposureTimeText(double seconds)
{
	const double t = photoNominalExposureTime(seconds);
	if (t < 0.25)
		return QString("1/%1 %2").arg(qRound(1./t)).arg(qc_("s", "unit of time: seconds"));
	if (t < 60.)
		return QString("%1 %2").arg(QLocale().toString(t, 'g', 3), qc_("s", "unit of time: seconds"));
	return QString("%1 %2").arg(QLocale().toString(t/60., 'g', 3), qc_("min", "unit of time: minutes"));
}

bool StelSkyDrawer::getFlagPhotoAutoExposureActive() const
{
	return flagPhotoAutoExposure && getFlagPhotoCameraActive();
}

double StelSkyDrawer::getPhotoEffectiveExposureTime() const
{
	if (getFlagNakedEyeModeActive())
		return EYE_INTEGRATION_TIME;
	return getFlagPhotoAutoExposureActive() ? photoAutoExposureTime : photoExposureTime;
}

void StelSkyDrawer::setFlagPhotoAutoExposure(bool b)
{
	if (b == flagPhotoAutoExposure)
		return;
	flagPhotoAutoExposure = b;
	if (b)
	{
		// Start from the set time; the first reading of the meter sets the right one at once
		photoAutoExposureTime = qBound(PHOTO_AUTO_MIN_TIME, photoExposureTime, PHOTO_AUTO_MAX_TIME);
		photoAutoExposureFresh = true;
	}
	photoAutoExposureLimit = 0;
	StelApp::immediateSave("viewing/flag_photographic_auto_exposure", b);
	emit flagPhotoAutoExposureChanged(b);
	announcePhotoExposureTime();
}

void StelSkyDrawer::setPhotoMeteringMode(int mode)
{
	mode = qBound(0, mode, 2);
	if (mode == photoMeteringMode)
		return;
	photoMeteringMode = mode;
	StelApp::immediateSave("viewing/photographic_metering_mode", mode);
	emit photoMeteringModeChanged(mode);
}

void StelSkyDrawer::setFlagPhotoInfoLine(bool b)
{
	if (b == flagPhotoInfoLine)
		return;
	flagPhotoInfoLine = b;
	StelApp::immediateSave("viewing/flag_photographic_info_line", b);
	emit flagPhotoInfoLineChanged(b);
}

void StelSkyDrawer::setFlagPhotoProgramExposure(bool b)
{
	if (b == flagPhotoProgramExposure)
		return;
	flagPhotoProgramExposure = b;
	photoAutoExposureFresh = true;
	if (!b)
		stopPhotoProgram();
	StelApp::immediateSave("viewing/flag_photographic_program_exposure", b);
	emit flagPhotoProgramExposureChanged(b);
	announcePhotoExposureTime();
}

void StelSkyDrawer::setFlagPhotoProgramApertureLock(bool b)
{
	if (b == flagPhotoProgramApertureLock)
		return;
	flagPhotoProgramApertureLock = b;
	photoAutoExposureFresh = true;
	// The program takes the user's aperture from the next frame on
	if (b && photoProgramRunning && !qFuzzyCompare(photoAperture, photoUserAperture))
	{
		photoAperture = photoUserAperture;
		emit photoApertureChanged(photoAperture);
	}
	StelApp::immediateSave("viewing/flag_photographic_program_aperture_lock", b);
	emit flagPhotoProgramApertureLockChanged(b);
}

void StelSkyDrawer::setPhotoProgramMaxISO(double iso)
{
	iso = qBound(PHOTO_PROGRAM_BASE_ISO, iso, PHOTO_PROGRAM_ISO_CEILING);
	if (qFuzzyCompare(iso, photoProgramMaxISO))
		return;
	photoProgramMaxISO = iso;
	StelApp::immediateSave("viewing/photographic_program_max_iso", iso);
	emit photoProgramMaxISOChanged(iso);
}

void StelSkyDrawer::setPhotoProgramMaxTime(double seconds)
{
	seconds = qBound(PHOTO_PROGRAM_MIN_MAX_TIME, seconds, PHOTO_AUTO_MAX_TIME);
	if (qFuzzyCompare(seconds, photoProgramMaxTime))
		return;
	photoProgramMaxTime = seconds;
	StelApp::immediateSave("viewing/photographic_program_max_time", seconds);
	emit photoProgramMaxTimeChanged(seconds);
}

void StelSkyDrawer::startPhotoProgram()
{
	if (photoProgramRunning)
		return;
	// Keep the user's settings: the program changes the ones in use, and gives these back when it stops
	photoUserAperture = photoAperture;
	photoUserISO = photoISO;
	photoProgramRunning = true;
}

void StelSkyDrawer::stopPhotoProgram()
{
	if (!photoProgramRunning)
		return;
	photoProgramRunning = false;
	const bool apertureChanged = !qFuzzyCompare(photoAperture, photoUserAperture);
	const bool isoChanged = !qFuzzyCompare(photoISO, photoUserISO);
	photoAperture = photoUserAperture;
	photoISO = photoUserISO;
	if (apertureChanged)
		emit photoApertureChanged(photoAperture);
	if (isoChanged)
		emit photoISOChanged(photoISO);
}

void StelSkyDrawer::applyPhotoProgram(double exposure)
{
	// The user's limits, like a camera's auto-ISO menu: the longest exposure time and the highest ISO. Past
	// both, the picture is underexposed (and the info line says so).
	const double tMin = PHOTO_AUTO_MIN_TIME, tHand = PHOTO_PROGRAM_HAND_TIME;
	const double tMax = qBound(qMax(tHand, PHOTO_PROGRAM_MIN_MAX_TIME), photoProgramMaxTime, PHOTO_AUTO_MAX_TIME);
	// The longest time on the program line before the ISO rises: on a fixed tripod the "500 rule" (the stars
	// stay points), with a star tracker (it follows them) the longest time allowed
	const double focalMm = 1000.*getPhotoFocalLength(*core->getProjection(StelCore::FrameJ2000));
	const double crop = PHOTO_SENSOR_DIAGONAL / getPhotoSensorDiagonal();
	const bool tracker = flagPhotoStarTrails && getPhotoEffectiveMount() == PhotoMountTracker;
	const double tLimit = tracker ? tMax : qBound(tHand, PHOTO_PROGRAM_RULE/qMax(1., focalMm*crop), tMax);
	// With a film the speed is fixed: the program changes only the time and the aperture
	const bool film = photoFilm != PhotoFilmNone;
	const double S0 = film ? getPhotoActiveFilmSpec().iso : PHOTO_PROGRAM_BASE_ISO;
	const double Smax = film ? S0 : qMax(S0, photoProgramMaxISO);
	// The widest aperture is that of the lens; a slow lens opens no wider than its daylight aperture either
	const double Nwide = photoLensMaxAperture;
	const double Nday = qMax(PHOTO_PROGRAM_DAY_APERTURE, Nwide), Nsmall = qMax(PHOTO_PROGRAM_MIN_APERTURE, Nday);
	// exposure = t S / N^2: walk along the line from bright to dark
	double t, N, S;
	bool timeAtLimit;
	if (flagPhotoProgramApertureLock)
	{
		// The aperture is the user's: the time up to its limit, then the ISO (too bright: the shortest time)
		N = qMax(photoUserAperture, Nwide);
		if (exposure <= tLimit*S0/(N*N))
		{
			S = S0; t = exposure*N*N/S;
		}
		else if (exposure <= tLimit*Smax/(N*N))
		{
			t = tLimit; S = exposure*N*N/t;
		}
		else
		{
			S = Smax; t = exposure*N*N/S;
		}
		timeAtLimit = exposure > tLimit*S0/(N*N) && exposure <= tLimit*Smax/(N*N);
	}
	else
	{
		if (exposure <= tMin*S0/(Nday*Nday))
		{
			// very bright: the shortest time, stopping down
			t = tMin; S = S0;
			N = qMin(Nsmall, std::sqrt(tMin*S0/qMax(1e-30, exposure)));
		}
		else if (exposure <= tHand*S0/(Nday*Nday))
		{
			N = Nday; S = S0; t = exposure*N*N/S;
		}
		else if (exposure <= tHand*S0/(Nwide*Nwide))
		{
			t = tHand; S = S0; N = std::sqrt(tHand*S0/exposure);
		}
		else if (exposure <= tLimit*S0/(Nwide*Nwide))
		{
			N = Nwide; S = S0; t = exposure*N*N/S;
		}
		else if (exposure <= tLimit*Smax/(Nwide*Nwide))
		{
			N = Nwide; t = tLimit; S = exposure*N*N/t;
		}
		else
		{
			N = Nwide; S = Smax; t = exposure*N*N/S;
		}
		// A camera sets the aperture and the ISO in third stops; the time makes up the rest. Where the time is at
		// its limit (the ISO rises), the ISO goes to the next step up, so that the time stays within the limit.
		N = std::exp2(std::round(6.*std::log2(N))/6.);
		timeAtLimit = exposure > tLimit*S0/(Nwide*Nwide) && exposure <= tLimit*Smax/(Nwide*Nwide);
	}
	const double isoSteps = 3.*std::log2(S/S0);
	S = qMin(Smax, S0*std::exp2((timeAtLimit ? std::ceil(isoSteps - 1e-6) : std::round(isoSteps))/3.));
	t = exposure*N*N/S;
	constexpr double tolerance = 1.12; // 1/6 stop
	photoAutoExposureLimit = (t > tMax*tolerance) ? -1 : ((t < tMin/tolerance) ? 1 : 0);
	t = qBound(tMin, t, tMax);
	photoAutoExposureTime = t;
	if (!qFuzzyCompare(N, photoAperture))
	{
		photoAperture = N;
		emit photoApertureChanged(N);
	}
	if (!qFuzzyCompare(S, photoISO))
	{
		photoISO = S;
		emit photoISOChanged(S);
	}
}

void StelSkyDrawer::announcePhotoExposureTime()
{
	const double t = photoNominalExposureTime(getPhotoEffectiveExposureTime());
	if (qFuzzyCompare(t, photoAnnouncedExposureTime))
		return;
	photoAnnouncedExposureTime = t;
	emit photoEffectiveExposureTimeChanged(getPhotoEffectiveExposureTime());
}

void StelSkyDrawer::applyPhotoMeterReading(const StelPhotoMeter::Reading& reading, double deltaTime)
{
	// Automatic white balance: grey world. The mean color of the picture (without what saturates)
	// is taken as the color of the light, and the color temperature with the same blue/red ratio is
	// neutralized, within the range of a camera's automatic white balance.
	if (!getFlagPhotoAutoWhiteBalanceActive())
		photoAutoWhiteBalanceFresh = true;
	else if (reading.greyRed > 0. && reading.greyBlue > 0. && reading.greyGreen > 0.)
	{
		const double wanted = photoKelvinForBlueRedRatio(reading.greyBlue/reading.greyRed);
		if (photoAutoWhiteBalanceFresh)
		{
			photoAutoWhiteBalanceKelvin = wanted;
			photoAutoWhiteBalanceFresh = false;
		}
		else
		{
			const double f = 1. - std::exp(-qBound(0., deltaTime, 1.)/PHOTO_AWB_TIME_CONSTANT);
			photoAutoWhiteBalanceKelvin = std::exp(std::log(photoAutoWhiteBalanceKelvin) + f*(std::log(wanted) - std::log(photoAutoWhiteBalanceKelvin)));
		}
		updatePhotoWhiteBalanceMatrix();
	}

	// The eye: it adapts to the mean luminance of the view (center-weighted, like the meter of a camera)
	if (getFlagNakedEyeModeActive())
	{
		photoAutoExposureFresh = true;
		stopPhotoProgram();
		if (!(reading.sceneScale > 0.))
			return;
		const double luminance = qMax(1e-8, reading.centerWeighted / reading.sceneScale); // cd/m^2
		const double jd = core->getJD();
		// The pupil first: the adaptation gets the light it lets in, as the light of an observer of the reference
		// age (whose thresholds the model has)
		if (eyeAdaptationFresh || flagEyeInstantAdaptation)
		{
			eyePupil.reset(luminance, eyeAge);
			eyeLightFactor = eyePupil.getLightFactor(luminance);
			eyeAdaptation.reset(luminance*eyeLightFactor);
			eyeAdaptationFresh = false;
		}
		else
		{
			// The time that passed: on the clock, or in the program when it runs faster (or jumps)
			const double dt = qMax(qMax(0., deltaTime), std::fabs(jd - eyeAdaptationLastJD)*86400.);
			eyePupil.update(luminance, eyeAge, dt);
			eyeLightFactor = eyePupil.getLightFactor(luminance);
			eyeAdaptation.update(luminance*eyeLightFactor, dt);
		}
		eyeAdaptationLastJD = jd;
		return;
	}
	eyeAdaptationFresh = true;

	if (!getFlagPhotoAutoExposureActive())
	{
		photoAutoExposureFresh = true;
		stopPhotoProgram();
		return;
	}
	if (!(reading.sceneScale > 0.))
		return;

	// A reflected-light meter (ISO 2720) sets the exposure so that the metered luminance L
	// becomes middle grey: t = K N^2 / (S L). Here the meter reads the scene image, which holds
	// L times the scene scale of the frame it measured. The compensation changes the time.
	constexpr double K = 12.5;
	const double compensation = std::exp2(photoExposureEV);
	const double nn = photoAperture*photoAperture;
	const double metered = (photoMeteringMode == PhotoMeteringSpot) ? reading.spot : reading.centerWeighted;
	const double L = metered / reading.sceneScale; // cd/m^2
	double wanted = (L > 0.) ? compensation * K * nn / (photoISO * L) : PHOTO_AUTO_MAX_TIME * 4.;
	if (photoMeteringMode == PhotoMeteringHighlight)
	{
		// Highlight-weighted: the brightest segment must not saturate. It may come up to 90% of white
		// (display, linear), where the tone curve still keeps some contrast.
		const double Lh = reading.highlight / reading.sceneScale;
		const double highlightTarget = static_cast<double>(photoToneCurveInverse(0.9f));
		if (Lh > 0.)
			wanted = std::min(wanted, compensation * highlightTarget * K * nn / (StelSkyDrawer::PHOTO_MIDDLE_GREY * photoISO * Lh));
	}

	// Program mode: the meter gives the exposure (time x ISO / N^2), smoothed like the live view of a camera; the
	// program line turns it into the time, the aperture and the ISO
	if (flagPhotoProgramExposure)
	{
		startPhotoProgram();
		const double exposure = wanted * photoISO / nn;
		if (photoAutoExposureFresh || !(photoProgramExposure > 0.))
		{
			photoProgramExposure = exposure;
			photoAutoExposureFresh = false;
		}
		else
		{
			const double f = 1. - std::exp(-qBound(0., deltaTime, 1.)/PHOTO_AUTO_TIME_CONSTANT);
			photoProgramExposure = std::exp(std::log(photoProgramExposure) + f*(std::log(exposure) - std::log(photoProgramExposure)));
		}
		applyPhotoProgram(photoProgramExposure);
		announcePhotoExposureTime();
		return;
	}
	stopPhotoProgram();

	// The camera cannot go beyond its range of exposure times: the picture gets too dark or too bright
	constexpr double tolerance = 1.12; // 1/6 stop
	photoAutoExposureLimit = (wanted > PHOTO_AUTO_MAX_TIME*tolerance) ? -1 : ((wanted < PHOTO_AUTO_MIN_TIME/tolerance) ? 1 : 0);
	wanted = qBound(PHOTO_AUTO_MIN_TIME, wanted, PHOTO_AUTO_MAX_TIME);

	if (photoAutoExposureFresh || !(photoAutoExposureTime > 0.))
	{
		photoAutoExposureTime = wanted;
		photoAutoExposureFresh = false;
	}
	else
	{
		// Smooth adaptation in log space, like the live view of a camera
		const double dt = qBound(0., deltaTime, 1.);
		const double f = 1. - std::exp(-dt/PHOTO_AUTO_TIME_CONSTANT);
		photoAutoExposureTime = std::exp(std::log(photoAutoExposureTime) + f*(std::log(wanted) - std::log(photoAutoExposureTime)));
	}
	announcePhotoExposureTime();
}

void StelSkyDrawer::applyPhotoSqmReading(const StelPhotoMeter::Reading& reading)
{
	const int grid = StelPhotoMeter::GRID;
	if (reading.cells.size() != static_cast<size_t>(grid)*grid || reading.width <= 0 || reading.height <= 0 || !(reading.sceneScale > 0.))
	{
		photoSqmLuminance = -1.;
		return;
	}
	// The field of a SQM-L: about 20 degrees full width at half maximum, as a Gaussian of the angle from its axis
	constexpr double SIGMA = 20./2.3548*M_PI/180.;
	static const double fullWeight = []() {
		// the integral of the weight over the sphere
		double sum = 0.;
		constexpr int N = 2000;
		for (int k=0; k<N; ++k)
		{
			const double t = (k + 0.5)*M_PI/N;
			sum += std::exp(-0.5*t*t/(SIGMA*SIGMA))*std::sin(t)*(M_PI/N);
		}
		return 2.*M_PI*sum;
	}();
	// The direction of the corners of the segments (the projection of this frame; the reading is a frame or two
	// older, close enough)
	const StelProjectorP prj = core->getProjection(StelCore::FrameAltAz, StelCore::RefractionOff);
	std::vector<Vec3d> corners(static_cast<size_t>(grid + 1)*(grid + 1));
	std::vector<char> valid(corners.size());
	for (int j=0; j<=grid; ++j)
		for (int i=0; i<=grid; ++i)
		{
			Vec3d v;
			const double x = static_cast<double>(i*reading.width/grid), y = static_cast<double>(j*reading.height/grid);
			const size_t k = static_cast<size_t>(j)*(grid + 1) + i;
			valid[k] = prj->unProject(x, y, v) && v.normSquared() > 0.;
			v.normalize();
			corners[k] = v;
		}
	double weight = 0., weighted = 0.;
	for (int j=0; j<grid; ++j)
		for (int i=0; i<grid; ++i)
		{
			const size_t a = static_cast<size_t>(j)*(grid + 1) + i, b = a + 1, d = a + grid + 1, c = d + 1;
			if (!valid[a] || !valid[b] || !valid[c] || !valid[d])
				continue;  // not on the sky (outside the circle of a fisheye)
			const Vec3d& A = corners[a];
			const Vec3d& B = corners[b];
			const Vec3d& C = corners[c];
			const Vec3d& D = corners[d];
			// solid angle of the segment: two flat triangles (the segments are small)
			const double area = 0.5*((B - A)^(C - A)).norm() + 0.5*((C - A)^(D - A)).norm();
			Vec3d centre = A + B + C + D;
			centre.normalize();
			const double theta = std::acos(qBound(-1., centre[2], 1.));
			const double w = std::exp(-0.5*theta*theta/(SIGMA*SIGMA))*area;
			const double value = static_cast<double>(reading.cells[static_cast<size_t>(j)*grid + i]);
			if (!(w > 0.) || !std::isfinite(value) || value < 0.)
				continue;
			weight += w;
			weighted += w*value/reading.sceneScale;
		}
	// Only when most of the field is in view (the light around the zenith that is not seen is not known)
	photoSqmLuminance = (weight > 0.5*fullWeight) ? weighted/weight : -1.;
}

double StelSkyDrawer::getPhotoZenithBackgroundLuminance() const
{
	if (!getFlagPhotographicModeActive() || core->getCurrentLocation().planetName != QStringLiteral("Earth"))
		return 0.;
	static LandscapeMgr* lmgr = GETSTELMODULE(LandscapeMgr);
	if (!lmgr || !lmgr->getFlagAtmosphere())
		return 0.;
	static LightDomes* domes = GETSTELMODULE(LightDomes);
	// As the atmosphere draws it (AtmosphereShowMySky): the light pollution setting is the whole night sky at the
	// zenith (at a normal humidity: the haze makes the artificial part brighter), of which the airglow and the light
	// domes of towns are parts; what they leave is spread evenly
	const double airglow = getPhotoAirglowZenithLuminance();
	const double dome = domes ? domes->getZenithLuminance() : 0.;
	const double setting = static_cast<double>(lightPollutionLuminance);
	return airglow + dome + qMax(0., photoHazeLightFactor*(setting - airglow) - dome);
}

void StelSkyDrawer::drawPhotoInfoLine(StelCore* core) const
{
	if (!flagPhotoInfoLine || !getFlagPhotographicModeActive())
		return;

	const QString sep = QStringLiteral("   ");
	QStringList parts;
	if (getFlagNakedEyeModeActive())
	{
		// The eye: the mode, the luminance it is adapted to and the system it sees with, and how far it still is
		// from being adapted in the dark
		parts << q_("Naked eye");
		const double L = getEyeAdaptationLuminance();
		const int decimals = qBound(0, 2 - static_cast<int>(std::floor(std::log10(qMax(1e-9, L)))), 9);
		QLocale locale;
		locale.setNumberOptions(QLocale::OmitGroupSeparator);
		const QString value = (L >= 1e-3) ? locale.toString(L, 'f', decimals) : locale.toString(L, 'e', 1);
		parts << QString("%1 %2 cd/m%3 (%4)").arg(qc_("adapted to", "eye: adaptation luminance"), value, QChar(0x00B2),
		                                          eyeAdaptation.getLimitingSystem() == StelEyeAdaptation::Rods ? qc_("rods", "eye: rod vision")
		                                                                                                    : qc_("cones", "eye: cone vision"));
		const double elevation = 2.5*eyeAdaptation.getThresholdElevation();
		if (elevation >= 0.05)
			parts << QString("%1 +%2 mag").arg(qc_("dark adaptation", "eye: how much brighter the faintest visible star is than when adapted"),
			                                    locale.toString(elevation, 'f', 1));
		parts << QString("%1 %2 mm").arg(qc_("pupil", "eye: diameter of the pupil"), locale.toString(eyePupil.getDiameter(), 'f', 1));
		if (std::fabs(photoExposureEV) > 0.01)
			parts << QString("%1 EV").arg(QString::asprintf("%+.1f", photoExposureEV));
	}
	else
	{
		const bool autoExposure = getFlagPhotoAutoExposureActive();
		const double t = getPhotoEffectiveExposureTime();
		// Exposure value at ISO 100. In manual mode the compensation brightens the picture like a lower EV;
		// with the automatic exposure it is already part of the exposure time.
		const double ev100 = std::log2(photoAperture*photoAperture/t) - std::log2(photoISO/100.) - (autoExposure ? 0. : photoExposureEV);
		parts << (!autoExposure ? qc_("M", "camera mode: manual")
		          : (flagPhotoProgramExposure ? qc_("P", "camera mode: program") : qc_("Av", "camera mode: aperture priority")));
		parts << photoExposureTimeText(t);
		// Film: what the long exposure counted for (reciprocity failure), when it is noticeably less
		if (getFlagPhotoFilmActive())
		{
			const double effective = getPhotoFilmEffectiveTime(t);
			if (effective < 0.94*t)
			{
				// not rounded to the steps of the camera: it is a computed value
				const QString text = effective < 60. ? QString("%1 %2").arg(QLocale().toString(effective, 'g', 2), qc_("s", "unit of time: seconds"))
				                                     : QString("%1 %2").arg(QLocale().toString(effective/60., 'g', 2), qc_("min", "unit of time: minutes"));
				parts << QString("(%1 %2)").arg(qc_("as", "film: effective exposure time after the reciprocity failure"), text);
			}
		}
		// The nominal values a camera shows (its third stops are rounded: f/1.4 for 1.41, ISO 1250 for 1270)
		auto nominal = [](double value, std::initializer_list<double> marks) {
			double best = value, bestDiff = 1e30;
			for (const double m : marks)
			{
				const double diff = std::fabs(std::log(value/m));
				if (diff < bestDiff) { bestDiff = diff; best = m; }
			}
			return bestDiff < 0.06 ? best : value; // within 1/12 stop of a mark
		};
		const double apertureShown = nominal(photoAperture, {0.95, 1.0, 1.1, 1.2, 1.4, 1.6, 1.8, 2.0, 2.2, 2.5, 2.8, 3.2, 3.5, 4.0, 4.5,
		                                                    5.0, 5.6, 6.3, 7.1, 8.0, 9.0, 10., 11., 13., 14., 16., 18., 20., 22., 32.});
		const double isoShown = nominal(photoISO, {50, 64, 80, 100, 125, 160, 200, 250, 320, 400, 500, 640, 800, 1000, 1250, 1600, 2000,
		                                           2500, 3200, 4000, 5000, 6400, 8000, 10000, 12800, 16000, 20000, 25600, 51200, 102400, 204800});
		parts << QString("f/%1").arg(QLocale().toString(apertureShown, 'g', 3));
		// Focal length of the lens for this field on the sensor in use, and the focus when it is not at infinity
		{
			QString lens = QString("%1 mm").arg(QLocale().toString(photoNominalFocalLength(1000.*getPhotoFocalLength(*core->getProjection(StelCore::FrameJ2000))), 'g', 5));
			const char* sensor = getFlagPhotoFilmActive() ? getPhotoActiveFilmSpec().shortName
			                                              : getPhotoSensorSpec(photoSensorFormat).shortName;
			if (sensor[0] != 0)
				lens += QStringLiteral(" ") + QString::fromUtf8(sensor);
			if (getFlagPhotoFilmActive() && photoFilmPush > 0)
				lens += QStringLiteral(" +%1").arg(photoFilmPush);
			parts << lens;
		}
		if (photoFocusDistance != 0.)
			parts << QString("%1 %2").arg(qc_("focus", "camera: focus distance"), photoFocusDistanceText(photoFocusDistance));
		parts << QString("ISO %1").arg(qRound(isoShown));
		parts << QString("EV100 %1").arg(QLocale().toString(ev100, 'f', 1));
		if (std::fabs(photoExposureEV) > 0.01)
			parts << QString("%1 EV").arg(QString::asprintf("%+.1f", photoExposureEV));
		if (!getFlagPhotoFilmActive())
		{
			const int kelvin = 50*qRound(getPhotoWhiteBalanceTemperature()/50.);
			parts << QString("%1 %2 K").arg(photoWhiteBalance == PhotoWBAuto ? qc_("AWB", "camera: automatic white balance") : qc_("WB", "camera: white balance")).arg(kelvin);
		}
		if (autoExposure)
		{
			static const char* modeNames[] = {
				QT_TRANSLATE_NOOP("StelSkyDrawer", "center-weighted"),
				QT_TRANSLATE_NOOP("StelSkyDrawer", "spot"),
				QT_TRANSLATE_NOOP("StelSkyDrawer", "highlight-weighted") };
			parts << q_(modeNames[qBound(0, photoMeteringMode, 2)]);
			if (photoAutoExposureLimit < 0)
				parts << q_("underexposed");
			else if (photoAutoExposureLimit > 0)
				parts << q_("overexposed");
		}
	}
	// The sky at the zenith: what a sky quality meter would read, and the darkest it gets at the place
	{
		const double background = getPhotoZenithBackgroundLuminance();
		const auto mpsas = [](double luminance) {return QLocale().toString(-2.5*std::log10(luminance/10.8e4), 'f', 2);};
		QString sky;
		if (photoSqmLuminance > 0.)
			sky = QString("%1 %2").arg(qc_("SQM", "sky quality meter: surface brightness of the sky at the zenith"), mpsas(photoSqmLuminance));
		else if (background > 0.)
			sky = QString("%1 %2").arg(qc_("SQM", "sky quality meter: surface brightness of the sky at the zenith"),
			                           qc_("-", "sky quality meter: the zenith is not in view"));
		if (background > 0.)
		{
			const int bortle = StelCore::luminanceToBortleScaleIndex(static_cast<float>(background));
			sky += QString(" (%1, %2)").arg(q_("best %1").arg(mpsas(background)), q_("Bortle %1").arg(bortle));
		}
		if (!sky.isEmpty())
			parts << sky;
	}
	const QString text = parts.join(sep);

	StelPainter sPainter(core->getProjection2d());
	sPainter.setAnnotation(false);
	const StelProjector::StelProjectorParams params = core->getCurrentStelProjectorParams();
	const float ppx = static_cast<float>(params.devicePixelsPerPixel);
	QFont font = QGuiApplication::font();
	font.setPixelSize(PHOTO_INFO_FONT_SIZE);
	sPainter.setFont(font);
	const QFontMetrics fm(font);
	const QSize fs = fm.size(Qt::TextSingleLine, text);

	// At the top of the screen, like the Equation of Time plugin (below its line if that is shown)
	float lines = 1.5f;
	StelPropertyMgr* propMgr = StelApp::getInstance().getStelPropertyManager();
	if (propMgr && propMgr->getProperty("EquationOfTime.showEOT", true) && propMgr->getStelPropertyValue("EquationOfTime.showEOT", true).toBool())
		lines += 1.5f;
	const float x = params.viewportXywh[2]*ppx/2.f - fs.width()*ppx/2.f;
	const float y = params.viewportXywh[3]*ppx - fs.height()*ppx*lines;

	// The text is drawn after the camera stage, straight into the display (sRGB) framebuffer, but the
	// painter converts colors to linear light: hand it the sRGB encoding of the wanted display values.
	auto encode = [](const Vec3f& display) {
		return Vec3f(linearToSRGB(display[0]), linearToSRGB(display[1]), linearToSRGB(display[2]));
	};
	// A soft shadow keeps the line readable on a bright sky
	sPainter.setColor(Vec3f(0.f), 0.6f);
	sPainter.drawText(x+ppx, y-ppx, text);
	// Light green, like the display in a camera's viewfinder
	sPainter.setColor(encode(Vec3f(0.62f, 1.f, 0.55f)), 1.f);
	sPainter.drawText(x, y, text);
}

void StelSkyDrawer::showPhotoExposureMessage()
{
	LabelMgr* labelMgr = GETSTELMODULE(LabelMgr);
	if (!labelMgr)
		return;
	if (photoExposureMessageID)
		labelMgr->deleteLabel(photoExposureMessageID);

	QString text = QString("%1 %2 EV").arg(q_("Exposure"), QString::asprintf("%+.1f", photoExposureEV));
	if (!flagPhotographicMode && !flagNakedEyeMode)
		text += QString(" (%1)").arg(q_("photographic mode is off"));

	const StelProjector::StelProjectorParams params = core->getCurrentStelProjectorParams();
	StelPainter painter(core->getProjection2d());
	const int x = qRound(params.viewportCenter[0] - painter.getFontMetrics().boundingRect(text).width()/2);
	const int y = qRound(params.viewportCenter[1] - params.viewportXywh[3]*params.viewportCenterOffset[1] - painter.getFontMetrics().height()/2);
	photoExposureMessageID = labelMgr->labelScreen(text, x, y, true, StelApp::getInstance().getScreenFontSize() + 3, "#99FF99", true, 2000);
}
