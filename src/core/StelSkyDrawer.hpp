/*
 * Stellarium
 * Copyright (C) 2007 Fabien Chereau
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

#ifndef STELSKYDRAWER_HPP
#define STELSKYDRAWER_HPP

#include "StelApp.hpp"
#include "RefractionExtinction.hpp"
#include "StelTextureTypes.hpp"
#include "VecMath.hpp"
#include "StelPhotoMeter.hpp"
#include "StelAtmosphereTransmission.hpp"
#include "StelProjectorType.hpp"

#include <array>
#include <memory>
#include <QObject>
#include <vector>
#include <QImage>
#include <QSettings>
#include <QOpenGLFunctions>

class StelProjector;

class QOpenGLVertexArrayObject;
class StelToneReproducer;
class QOpenGLBuffer;
class StelCore;
class StelPainter;

//! Contains the 2 parameters necessary to draw a star on screen.
//! the radius and luminance of the star halo texture.
struct RCMag
{
	float radius;
	float luminance;
};

//! @class StelSkyDrawer
//! Provide a set of methods used to draw sky objects taking into account
//! eyes adaptation, zoom level, instrument model and artificially set magnitude limits
//!
//! The photographic mode (a camera instead of the eye) is implemented in its own files:
//! StelSkyDrawerPhotoCamera.cpp (camera settings, tone curve, lens, sensor), StelSkyDrawerPhotoExposure.cpp
//! (automatic and program exposure, metering, information line), StelSkyDrawerPhotoFilm.cpp (the films),
//! StelSkyDrawerPhotoStars.cpp (stars and planets: point spread function, trails), StelSkyDrawerPhotoSky.cpp
//! (white balance, extinction colors, airglow) and StelSkyDrawerPhotoImages.cpp (images of planets and landscape).
class StelSkyDrawer : public QObject, protected QOpenGLFunctions
{
	Q_OBJECT

	//! Sets how much brighter stars will be bigger than fainter stars
	Q_PROPERTY(double relativeStarScale READ getRelativeStarScale WRITE setRelativeStarScale NOTIFY relativeStarScaleChanged)
	//! The absolute star brightness scale
	Q_PROPERTY(double absoluteStarScale READ getAbsoluteStarScale WRITE setAbsoluteStarScale NOTIFY absoluteStarScaleChanged)
	Q_PROPERTY(double twinkleAmount READ getTwinkleAmount WRITE setTwinkleAmount NOTIFY twinkleAmountChanged)
	Q_PROPERTY(bool flagStarTwinkle READ getFlagTwinkle WRITE setFlagTwinkle NOTIFY flagTwinkleChanged)
	Q_PROPERTY(bool flagForcedTwinkle READ getFlagForcedTwinkle WRITE setFlagForcedTwinkle NOTIFY flagForcedTwinkleChanged)
	Q_PROPERTY(double lightPollutionLuminance READ getLightPollutionLuminance WRITE setLightPollutionLuminance NOTIFY lightPollutionLuminanceChanged)
	Q_PROPERTY(bool flagDrawBigStarHalo READ getFlagDrawBigStarHalo WRITE setFlagDrawBigStarHalo NOTIFY flagDrawBigStarHaloChanged)
	Q_PROPERTY(bool flagStarSpiky READ getFlagStarSpiky WRITE setFlagStarSpiky NOTIFY flagStarSpikyChanged)

	Q_PROPERTY(bool flagStarMagnitudeLimit READ getFlagStarMagnitudeLimit WRITE setFlagStarMagnitudeLimit NOTIFY flagStarMagnitudeLimitChanged)
	Q_PROPERTY(bool flagNebulaMagnitudeLimit READ getFlagNebulaMagnitudeLimit WRITE setFlagNebulaMagnitudeLimit NOTIFY flagNebulaMagnitudeLimitChanged)
	Q_PROPERTY(bool flagPlanetMagnitudeLimit READ getFlagPlanetMagnitudeLimit WRITE setFlagPlanetMagnitudeLimit NOTIFY flagPlanetMagnitudeLimitChanged)

	Q_PROPERTY(double customStarMagLimit READ getCustomStarMagnitudeLimit WRITE setCustomStarMagnitudeLimit NOTIFY customStarMagLimitChanged)
	Q_PROPERTY(double customNebulaMagLimit READ getCustomNebulaMagnitudeLimit WRITE setCustomNebulaMagnitudeLimit NOTIFY customNebulaMagLimitChanged)
	Q_PROPERTY(double customPlanetMagLimit READ getCustomPlanetMagnitudeLimit WRITE setCustomPlanetMagnitudeLimit NOTIFY customPlanetMagLimitChanged)

	Q_PROPERTY(bool flagLuminanceAdaptation READ getFlagLuminanceAdaptation WRITE setFlagLuminanceAdaptation NOTIFY flagLuminanceAdaptationChanged)
	Q_PROPERTY(double daylightLabelThreshold READ getDaylightLabelThreshold WRITE setDaylightLabelThreshold NOTIFY daylightLabelThresholdChanged)

	//! Photographic (camera-like) rendering mode. Only active in high graphics mode (OpenGL 3+).
	Q_PROPERTY(bool flagPhotographicMode READ getFlagPhotographicMode WRITE setFlagPhotographicMode NOTIFY flagPhotographicModeChanged)
	//! Exposure correction of the photographic mode, in photographic stops (EV). 0 is the neutral exposure.
	Q_PROPERTY(double photoExposureEV READ getPhotoExposureEV WRITE setPhotoExposureEV NOTIFY photoExposureEVChanged)
	//! Exposure time of the virtual camera in the photographic mode, in seconds.
	Q_PROPERTY(double photoExposureTime READ getPhotoExposureTime WRITE setPhotoExposureTime NOTIFY photoExposureTimeChanged)
	//! Aperture (f-number) of the virtual camera in the photographic mode, e.g. 2.8 for f/2.8.
	Q_PROPERTY(double photoAperture READ getPhotoAperture WRITE setPhotoAperture NOTIFY photoApertureChanged)
	Q_PROPERTY(double photoLensMaxAperture READ getPhotoLensMaxAperture WRITE setPhotoLensMaxAperture NOTIFY photoLensMaxApertureChanged)
	//! ISO sensitivity of the virtual camera in the photographic mode.
	Q_PROPERTY(double photoISO READ getPhotoISO WRITE setPhotoISO NOTIFY photoISOChanged)
	//! Smallest width (FWHM) of a star image in pixels, set by sensor and optics.
	Q_PROPERTY(double photoPsfMinFwhm READ getPhotoPsfMinFwhm WRITE setPhotoPsfMinFwhm NOTIFY photoPsfMinFwhmChanged)
	//! Atmospheric seeing (FWHM) in arc seconds. It matters only at small fields of view.
	Q_PROPERTY(double photoSeeing READ getPhotoSeeing WRITE setPhotoSeeing NOTIFY photoSeeingChanged)
	//! Fraction of a star's light that the lens scatters into a wide, faint halo (0 ... 0.3).
	Q_PROPERTY(double photoHaloStrength READ getPhotoHaloStrength WRITE setPhotoHaloStrength NOTIFY photoHaloStrengthChanged)
	//! Number of blades of the lens diaphragm (0 = round aperture, 4 ... 9). The straight edges of the stopped
	//! down aperture make diffraction spikes: as many as blades for an even number, twice as many for an odd one.
	Q_PROPERTY(int photoApertureBlades READ getPhotoApertureBlades WRITE setPhotoApertureBlades NOTIFY photoApertureBladesChanged)
	//! Fraction of a star's light that goes into the diffraction spikes (0 ... 0.1).
	Q_PROPERTY(double photoSpikeStrength READ getPhotoSpikeStrength WRITE setPhotoSpikeStrength NOTIFY photoSpikeStrengthChanged)
	//! Photographic mode: apply the lens halo to the whole image ("bloom") instead of drawing it with each star.
	//! This gives the right shape for extended bright objects (lunar crescent, Sun behind branches, lights).
	Q_PROPERTY(bool flagPhotoBloom READ getFlagPhotoBloom WRITE setFlagPhotoBloom NOTIFY flagPhotoBloomChanged)
	//! Photographic mode: automatic exposure with aperture priority. The camera meters the image and
	//! chooses the exposure time (1/8000 s ... 30 s) for the set aperture and ISO, like the "Av"/"A"
	//! mode of a camera. The exposure compensation (photoExposureEV) then changes the exposure time.
	Q_PROPERTY(bool flagPhotoAutoExposure READ getFlagPhotoAutoExposure WRITE setFlagPhotoAutoExposure NOTIFY flagPhotoAutoExposureChanged)
	//! With the automatic exposure: program mode (P) with automatic ISO instead of aperture priority. The camera
	//! chooses the exposure time, the aperture and the ISO along a program line (see applyPhotoMeterReading()).
	Q_PROPERTY(bool flagPhotoProgramExposure READ getFlagPhotoProgramExposure WRITE setFlagPhotoProgramExposure NOTIFY flagPhotoProgramExposureChanged)
	Q_PROPERTY(bool flagPhotoProgramApertureLock READ getFlagPhotoProgramApertureLock WRITE setFlagPhotoProgramApertureLock NOTIFY flagPhotoProgramApertureLockChanged)
	//! Program mode: the highest ISO the automatic ISO may use (like the "maximum sensitivity" of a camera's auto-ISO menu)
	Q_PROPERTY(double photoProgramMaxISO READ getPhotoProgramMaxISO WRITE setPhotoProgramMaxISO NOTIFY photoProgramMaxISOChanged)
	//! Program mode: the longest exposure time the program may use, in seconds (on a fixed tripod the "500 rule" can make it shorter)
	Q_PROPERTY(double photoProgramMaxTime READ getPhotoProgramMaxTime WRITE setPhotoProgramMaxTime NOTIFY photoProgramMaxTimeChanged)
	//! Metering mode of the automatic exposure: 0 = center-weighted average, 1 = spot, 2 = highlight-weighted
	Q_PROPERTY(int photoMeteringMode READ getPhotoMeteringMode WRITE setPhotoMeteringMode NOTIFY photoMeteringModeChanged)
	//! Photographic mode: show a line with the camera settings in use at the top of the screen
	Q_PROPERTY(bool flagPhotoInfoLine READ getFlagPhotoInfoLine WRITE setFlagPhotoInfoLine NOTIFY flagPhotoInfoLineChanged)
	//! Tone curve of the camera: 0 = linear, 1 = standard (camera JPEG), 2 = film
	Q_PROPERTY(int photoToneCurve READ getPhotoToneCurve WRITE setPhotoToneCurve NOTIFY photoToneCurveChanged)
	//! Photographic mode: color the extinction of stars, planets, Sun, Moon, Milky Way and zodiacal light with the
	//! spectral transmission of the atmosphere model (atmosphere/lightweight-extinction.dat). The amount of the
	//! extinction still follows the extinction coefficient.
	Q_PROPERTY(bool flagPhotoExtinctionColor READ getFlagPhotoExtinctionColor WRITE setFlagPhotoExtinctionColor NOTIFY flagPhotoExtinctionColorChanged)
	//! Photographic mode: the airglow, the faint light of the upper atmosphere (about 90 km) that is the natural night
	//! sky at a dark site (with the lightweight atmosphere model, which holds the scattered light of the Sun and the Moon)
	Q_PROPERTY(bool flagPhotoAirglow READ getFlagPhotoAirglow WRITE setFlagPhotoAirglow NOTIFY flagPhotoAirglowChanged)
	//! Photographic mode: the brightness of the airglow relative to its mean (1.0); it changes from night to night
	//! by about a factor of two
	Q_PROPERTY(double photoAirglowStrength READ getPhotoAirglowStrength WRITE setPhotoAirglowStrength NOTIFY photoAirglowStrengthChanged)
	//! Photographic mode: the relative humidity of the air (percent, 20 to 98). The aerosols take up water and grow:
	//! the haze scatters more light, whiter and more forward. The extinction coefficient is the one at a normal
	//! humidity (60%); see getPhotoAerosol().
	Q_PROPERTY(double photoHumidity READ getPhotoHumidity WRITE setPhotoHumidity NOTIFY photoHumidityChanged)
	//! Photographic mode: the aureole of the haze, the glow around the Moon, planets and bright stars from the light
	//! that the aerosols scatter forwards (stronger near the horizon and in humid air)
	Q_PROPERTY(bool flagPhotoAureole READ getFlagPhotoAureole WRITE setFlagPhotoAureole NOTIFY flagPhotoAureoleChanged)
	//! White balance of the camera (see PhotoWhiteBalance): 0 = automatic, 1 = daylight, 2 = cloudy, 3 = shade,
	//! 4 = tungsten, 5 = fluorescent, 6 = flash, 7 = custom color temperature, 8 = neutral (D65, the rendering as it is)
	Q_PROPERTY(int photoWhiteBalance READ getPhotoWhiteBalance WRITE setPhotoWhiteBalance NOTIFY photoWhiteBalanceChanged)
	//! Color temperature (K) of the custom white balance
	Q_PROPERTY(double photoWhiteBalanceKelvin READ getPhotoWhiteBalanceKelvin WRITE setPhotoWhiteBalanceKelvin NOTIFY photoWhiteBalanceKelvinChanged)
	//! Lens: vignetting (natural cos^3 falloff and optical vignetting that shrinks when stopping down)
	Q_PROPERTY(bool flagPhotoVignetting READ getFlagPhotoVignetting WRITE setFlagPhotoVignetting NOTIFY flagPhotoVignettingChanged)
	//! Digital sensor modified for H-alpha (the IR-cut filter replaced): emission nebulae get more red
	Q_PROPERTY(bool flagPhotoHalphaModified READ getFlagPhotoHalphaModified WRITE setFlagPhotoHalphaModified NOTIFY flagPhotoHalphaModifiedChanged)
	//! Lens: off-axis aberrations of star images (coma and astigmatism), growing towards the corners and with the aperture
	Q_PROPERTY(bool flagPhotoAberrations READ getFlagPhotoAberrations WRITE setFlagPhotoAberrations NOTIFY flagPhotoAberrationsChanged)
	//! Lens: strength of the off-axis aberrations (1 = a typical fast lens; 0 ... 3)
	Q_PROPERTY(double photoAberrationStrength READ getPhotoAberrationStrength WRITE setPhotoAberrationStrength NOTIFY photoAberrationStrengthChanged)
	Q_PROPERTY(double photoDetectionSnr READ getPhotoDetectionSnr WRITE setPhotoDetectionSnr NOTIFY photoDetectionSnrChanged)
	//! Lens: reflections inside the lens ("ghosts") of bright sources such as the Sun and the Moon
	Q_PROPERTY(bool flagPhotoLensFlare READ getFlagPhotoLensFlare WRITE setFlagPhotoLensFlare NOTIFY flagPhotoLensFlareChanged)
	//! Lens: strength of the reflections (1 = single-layer coated lens, less for multi-coating; 0 ... 3)
	Q_PROPERTY(double photoLensFlareStrength READ getPhotoLensFlareStrength WRITE setPhotoLensFlareStrength NOTIFY photoLensFlareStrengthChanged)
	//! Lens: the distance the lens is focused at (meters). 0 = infinity (stars sharp). A negative value means
	//! the focus ring turned beyond the infinity mark, by the same amount (dioptres) as focusing at that distance.
	Q_PROPERTY(double photoFocusDistance READ getPhotoFocusDistance WRITE setPhotoFocusDistance NOTIFY photoFocusDistanceChanged)
	//! Star trails: the camera stands on a fixed tripod, so during the exposure the stars and planets move
	//! with the diurnal motion and draw arcs around the celestial pole
	Q_PROPERTY(bool flagPhotoStarTrails READ getFlagPhotoStarTrails WRITE setFlagPhotoStarTrails NOTIFY flagPhotoStarTrailsChanged)
	//! What the camera stands on while the sky turns during the exposure (with flagPhotoStarTrails, see PhotoMount):
	//! a fixed tripod (star trails), a star tracker that follows the sky (the landscape blurs), or as the mount of
	//! the view (alt-azimuthal: tripod, equatorial: tracker)
	Q_PROPERTY(int photoMount READ getPhotoMount WRITE setPhotoMount NOTIFY photoMountChanged)
	//! Sensor noise (see PhotoNoise): off, as in the whole picture on the screen, or as in a 100% view of the sensor
	Q_PROPERTY(int photoNoise READ getPhotoNoise WRITE setPhotoNoise NOTIFY photoNoiseChanged)
	//! Sensor format (see PhotoSensorFormat): from medium format to compact cameras. It sets the focal length for the field of view
	//! (and so the depth of field), and the light the sensor collects for the picture (and so the noise).
	Q_PROPERTY(int photoSensorFormat READ getPhotoSensorFormat WRITE setPhotoSensorFormat NOTIFY photoSensorFormatChanged)
	//! Photographic mode: film in the camera instead of the digital sensor (see PhotoFilm; 0 = the digital sensor).
	//! The film sets the speed, the color balance, the characteristic curve and the grain; it is 35 mm (36 x 24 mm).
	Q_PROPERTY(int photoFilm READ getPhotoFilm WRITE setPhotoFilm NOTIFY photoFilmChanged)
	//! Push processing of the film, in stops (0: normal): more speed, contrast and grain
	Q_PROPERTY(int photoFilmPush READ getPhotoFilmPush WRITE setPhotoFilmPush NOTIFY photoFilmPushChanged)
	Q_PROPERTY(bool flagPhotoFilmSpectralSky READ getFlagPhotoFilmSpectralSky WRITE setFlagPhotoFilmSpectralSky NOTIFY flagPhotoFilmSpectralSkyChanged)
	//! Zooming with the mouse wheel in steps of the focal length (see PhotoZoomSteps): free, whole millimeters as
	//! marked on a zoom lens, or the focal lengths of common lenses
	Q_PROPERTY(int photoZoomSteps READ getPhotoZoomSteps WRITE setPhotoZoomSteps NOTIFY photoZoomStepsChanged)
	//! The camera is turned for a portrait (upright) picture: the long side of the sensor is vertical
	Q_PROPERTY(bool flagPhotoPortrait READ getFlagPhotoPortrait WRITE setFlagPhotoPortrait NOTIFY flagPhotoPortraitChanged)
	//! Dim the view outside the frame of the sensor (what the picture holds), like a viewfinder
	Q_PROPERTY(bool flagPhotoFrameMask READ getFlagPhotoFrameMask WRITE setFlagPhotoFrameMask NOTIFY flagPhotoFrameMaskChanged)
	//! Lunar eclipses: the state of the Earth's atmosphere that refracts sunlight into its shadow (see
	//! LunarEclipseLight::Atmosphere): from a bright eclipse (clean air) to a very dark one (after a major eruption)
	Q_PROPERTY(int photoEclipseAtmosphere READ getPhotoEclipseAtmosphere WRITE setPhotoEclipseAtmosphere NOTIFY photoEclipseAtmosphereChanged)
	//! The exposure time in use (seconds): the set one, or the one chosen by the automatic exposure.
	//! The change signal is only emitted when the value shown by a camera (1/3-stop steps) changes.
	Q_PROPERTY(double photoEffectiveExposureTime READ getPhotoEffectiveExposureTime NOTIFY photoEffectiveExposureTimeChanged)

	Q_PROPERTY(double extinctionCoefficient READ getExtinctionCoefficient WRITE setExtinctionCoefficient NOTIFY extinctionCoefficientChanged)
	Q_PROPERTY(double atmosphereTemperature READ getAtmosphereTemperature WRITE setAtmosphereTemperature NOTIFY atmosphereTemperatureChanged)
	Q_PROPERTY(double atmospherePressure READ getAtmospherePressure WRITE setAtmospherePressure NOTIFY atmospherePressureChanged)

	Q_PROPERTY(bool flagDrawSunAfterAtmosphere READ getFlagDrawSunAfterAtmosphere WRITE setFlagDrawSunAfterAtmosphere NOTIFY flagDrawSunAfterAtmosphereChanged)
	Q_PROPERTY(bool flagEarlySunHalo READ getFlagEarlySunHalo WRITE setFlagEarlySunHalo NOTIFY flagEarlySunHaloChanged)
	Q_PROPERTY(bool flagTfromK READ getFlagTfromK WRITE setFlagTfromK NOTIFY flagTfromKChanged)
	Q_PROPERTY(double turbidity READ getT WRITE setT NOTIFY turbidityChanged)

public:
	//! The camera pass of the photographic mode is linear up to this value, then rolls off smoothly towards 1.0.
	static constexpr float PHOTO_SHOULDER_KNEE = 0.8f;
	//! Most lines of diffraction spikes through a star (9 blades: 18 spikes)
	static constexpr int PHOTO_MAX_SPIKE_LINES = 9;
	//! Middle grey of the reflected-light meter calibration (scene units)
	static constexpr double PHOTO_MIDDLE_GREY = 0.18;
	//! Range of exposure times of the automatic exposure (seconds), as in a camera
	static constexpr double PHOTO_AUTO_MIN_TIME = 1./8000.;
	static constexpr double PHOTO_AUTO_MAX_TIME = 30.;
	//! Time constant of the smooth adaptation of the automatic exposure (seconds)
	static constexpr double PHOTO_AUTO_TIME_CONSTANT = 0.3;
	//! Program mode: the program line of a camera with automatic ISO, from bright to dark. In daylight the base
	//! ISO and a medium aperture (stopped down further when 1/8000 s is not short enough); then the aperture opens
	//! at a time that is safe by hand; then the time grows up to where the stars still stay points on a fixed
	//! tripod (the "500 rule": 500 s / (focal length in mm x crop factor), at most 30 s; with a star tracker 30 s);
	//! then the ISO rises up to its ceiling; then the time grows to the longest of the camera.
	static constexpr double PHOTO_PROGRAM_BASE_ISO = 100.;
	static constexpr double PHOTO_PROGRAM_MAX_ISO = 6400.;      //!< default highest ISO of the program mode
	static constexpr double PHOTO_PROGRAM_ISO_CEILING = 204800.; //!< highest ISO the user can allow
	static constexpr double PHOTO_PROGRAM_MIN_MAX_TIME = 1./30.; //!< shortest "longest time" the user can set
	static constexpr double PHOTO_PROGRAM_DAY_APERTURE = 8.;
	static constexpr double PHOTO_PROGRAM_MIN_APERTURE = 22.;  //!< smallest opening (largest f-number)
	static constexpr double PHOTO_PROGRAM_HAND_TIME = 1./125.;
	static constexpr double PHOTO_PROGRAM_RULE = 500.;          //!< the "500 rule" (seconds x mm)
	//! Font size (pixels) of the line with the camera settings
	static constexpr int PHOTO_INFO_FONT_SIZE = 18;
	//! White balance settings, with the color temperatures that cameras use for them
	enum PhotoWhiteBalance {
		PhotoWBAuto = 0,        //!< automatic (grey world from the exposure meter, 3000 ... 8000 K)
		PhotoWBDaylight = 1,    //!< 5200 K
		PhotoWBCloudy = 2,      //!< 6000 K
		PhotoWBShade = 3,       //!< 7000 K
		PhotoWBTungsten = 4,    //!< 3200 K
		PhotoWBFluorescent = 5, //!< 4000 K
		PhotoWBFlash = 6,       //!< 5500 K
		PhotoWBCustom = 7,      //!< photoWhiteBalanceKelvin
		PhotoWBNeutral = 8      //!< D65: no correction, the colors as rendered (display white)
	};
	//! Sensor noise: how the noise is shown
	enum PhotoNoise {
		PhotoNoiseOff = 0,    //!< no noise
		PhotoNoiseImage = 1,  //!< the whole picture on the screen: a screen pixel collects the light of all photosites in it
		PhotoNoisePixel = 2   //!< a 100% view ("pixel peeping"): one photosite per screen pixel
	};
	//! Sensor model (a modern full frame sensor of 24 megapixels): pitch of a photosite (m), its full well
	//! (electrons) at the base ISO, the read noise before and after the ISO amplifier (electrons, the latter
	//! at the base ISO), dark current (electrons per second at 20 degrees C, doubling every 6 degrees),
	//! hot pixels (fraction of the photosites and their mean dark current), warming of the sensor above the air.
	//! For another pitch (other sensors) the full well and the dark current scale with the area of the photosite,
	//! the read noise stays the same per photosite.
	static constexpr double PHOTO_SENSOR_PITCH = 6.0e-6;
	static constexpr float PHOTO_SENSOR_FULL_WELL = 50000.f;
	static constexpr float PHOTO_SENSOR_BASE_ISO = 100.f;
	static constexpr float PHOTO_READ_NOISE_UPSTREAM = 1.5f;
	static constexpr float PHOTO_READ_NOISE_DOWNSTREAM = 15.f;
	static constexpr float PHOTO_DARK_CURRENT = 0.05f;
	static constexpr float PHOTO_DARK_DOUBLING = 6.f;
	//! Where the sensor saturates, in scene units (middle grey at the metered exposure is 0.18): 3.65 stops above
	//! middle grey, as the camera makers set the ISO speed (standard output sensitivity, ISO 12232), measured on a
	//! Canon EOS R6 (see updatePhotoToneCurve()). The noise model and the standard tone curve both use it.
	static constexpr float PHOTO_SENSOR_SATURATION = 2.26f;
	static constexpr float PHOTO_HOT_PIXEL_FRACTION = 2e-5f;
	static constexpr float PHOTO_HOT_PIXEL_CURRENT = 20.f;
	static constexpr float PHOTO_SENSOR_WARMING = 4.f;
	//! What the camera stage needs for the noise (see StelApp)
	struct PhotoNoiseParams
	{
		bool enabled = false;
		float white = 1.f;       //!< electrons of one photosite per scene unit at the current ISO (it saturates at PHOTO_SENSOR_SATURATION)
		float read2 = 0.f;       //!< variance of the read noise of one photosite (electrons^2)
		float dark = 0.f;        //!< mean dark signal of one photosite over the exposure (electrons)
		float sites = 1.f;       //!< photosites per screen pixel
		float hotProbability = 0.f; //!< probability that a screen pixel holds a hot photosite
		float hot = 0.f;         //!< mean dark signal of a hot photosite over the exposure (electrons)
		float seed = 0.f;        //!< changes every frame (the noise of a new exposure)
	};
	PhotoNoiseParams getPhotoNoiseParams(const StelProjector& prj) const;
	//! The grain of a film (see getPhotoGrainParams()): it lies in the density of the developed film
	struct PhotoGrainParams
	{
		int mode = 0;            //!< 0: none, 1: slide film (the density of the picture itself), 2: negative printed on paper
		bool mono = false;       //!< black and white: the same grain in red, green and blue
		float sigma = 0.f;       //!< RMS density at density 1 in a pixel (Selwyn: the 48 micrometer value x sqrt of the areas)
		float negativeGamma = 1.f; //!< negative: the slope of its curve (density per log10 exposure)
		float printGamma = 1.f;  //!< negative: the slope of the paper, which multiplies the density fluctuation of the negative
		float toe = 0.01f;       //!< negative: the exposure at the toe of its curve (camera units of the tone curve)
		float seed = 0.f;        //!< changes every frame, as the sensor noise
	};
	PhotoGrainParams getPhotoGrainParams(const StelProjector& prj) const;
	//! The reciprocity failure of a film, for each point of the picture after its brightness (see the .cpp)
	struct PhotoReciprocityParams
	{
		bool enabled = false;
		float time = 1.f;        //!< the exposure time given, seconds
		int points = 0;          //!< the pairs of the data sheet (time, effective time) and its green filters
		float times[4] = {};
		float effective[4] = {};
		float green[4] = {};
		float exponent = 1.f;    //!< Schwarzschild exponent beyond the last pair
	};
	PhotoReciprocityParams getPhotoReciprocityParams() const;
	//! The exposure a film records at the camera exposure x, after its reciprocity failure at that brightness, and
	//! the inverse (found from x0)
	float photoFilmRecordedExposure(float x) const;
	float photoFilmExposureForRecorded(float xr, float x0) const;
	//! The exposure of middle grey in the camera stage (before the tone curve)
	static constexpr float PHOTO_MIDDLE_GREY_EXPOSURE = 0.18f;
	//! The halation of a film (see StelPhotoBloom::computeHalationWeights()): a ring of light reflected at the back
	//! of the film base around bright stars, reddish in a color film
	struct PhotoHalationParams
	{
		float radius = 0.f;      //!< the critical radius 2 t tan(theta_c) in pixels (0: no halation)
		float density = 0.f;     //!< optical density of the antihalation layer
		Vec3f gain = Vec3f(0.f); //!< the share of the light in red, green and blue that comes back as halation
	};
	PhotoHalationParams getPhotoHalationParams(const StelProjector& prj) const;
	static constexpr double PHOTO_FILM_BASE_THICKNESS = 125e-6; //!< 35 mm film base (cellulose triacetate), meters
	//! The glow of the lens alone, without the filter stack of a sensor: set after Till Credner's pictures of
	//! Jupiter on film (50 mm, a white core of about 0.07 mm at 15 minutes)
	static constexpr float PHOTO_FILM_HALO_DEFAULT = 0.003f;
	//! Base plus fog of a slide film, and its maximum density (density of the darkest black of the picture)
	static constexpr float PHOTO_SLIDE_BASE_DENSITY = 0.2f;
	static constexpr float PHOTO_SLIDE_MAX_DENSITY = 3.5f;
	//! Base plus fog of a negative (with the orange mask of a color negative it is higher, but that is filtered
	//! away in the print and adds no grain of its own)
	static constexpr float PHOTO_NEGATIVE_FOG_DENSITY = 0.25f;
	//! A pixel of a "100% view" of a film: a scan at 4000 dpi, as by the film scanners of the time (6.35 micrometers)
	static constexpr double PHOTO_FILM_SCAN_PIXEL = 25.4e-3/4000.;
	//! The mean luminance <Y> (linear, unit = white) of a texture and the mean of its square <Y^2>, measured on
	//! the GPU from its mipmaps. <Y^2>/<Y> is the typical luminance of the lit parts, whatever their share of the
	//! image. Used to calibrate images of nebulae in the photographic mode. Returns false on failure.
	//! It must be called with the GL context current; it restores the bound framebuffer and viewport.
	//! Photographic mode: blur and trail of a body with a visible disk (Moon, Sun, planets). The disk is drawn
	//! into an offscreen image (call this after creating the painter, before drawing), which endPhotoBodyImage()
	//! then puts into the scene, spread along the star trail and over the blur disk of a defocused lens.
	//! Labels, lines and markers are drawn as usual and stay sharp. Returns false (draw directly) when the
	//! body needs neither, or the offscreen image cannot be made.
	//! @param posJ2000 position of the body, @param radius its radius on the screen with rings (pixels)
	//! @param glow false: the disk of a body, trailed and blurred, covering what is behind it. true: light that
	//! arises in the lens (the halo and the spikes of a bright body), only trailed and added to the scene.
	bool beginPhotoBodyImage(const Vec3d& posJ2000, float radius, bool glow = false);
	//! Photographic mode: the apparent motion of the body drawn next against the stars (the change of its unit
	//! direction per second, J2000 frame), e.g. the Moon, about 0.5 degrees per hour. During the exposure it adds to
	//! the turn of the sky (a fixed tripod), or makes the only trail (a star tracker follows the stars, not the
	//! body). Set it before drawing a body (drawPhotoBody(), beginPhotoBodyImage()), and back to zero after.
	void setPhotoBodyMotion(const Vec3d& motionJ2000) {photoBodyMotion = motionJ2000;}
	void endPhotoBodyImage();
	//! Photographic mode: one copy of an extended image (a picture of a nebula) among the copies whose sum is
	//! what the camera records of it during the exposure (the star trail) and over the blur disk of the lens.
	struct PhotoImageSample
	{
		Mat4d viewRotation; //!< turns the sky in view (eye) coordinates to where this copy lies
		Vec3f factor;       //!< share of the light of the image, with the change of the extinction along the trail
	};
	//! The copies for an image around @a center (in the frame of @a prj). Empty: the image needs no copies.
	std::vector<PhotoImageSample> getPhotoImageSamples(const StelProjector& prj, const Vec3d& center) const;
	//! @a prj with its view turned by the rotation of a sample (see getPhotoImageSamples())
	StelProjectorP getPhotoImageSampleProjector(const StelProjectorP& prj, const Mat4d& viewRotation) const;
	//! @param power the texture values are raised to this power first (see PHOTO_DSO_DESTRETCH)
	//! @param meanColor if given, the mean of the red, green and blue values (to the power)
	bool photoTextureStatistics(class StelTexture& texture, float power, float& mean, float& meanSquare, Vec3f* meanColor = nullptr);
	//! Images of nebulae are processed pictures: their contrast is compressed ("stretched") to show the faint
	//! parts. In the photographic mode their values are raised to this power, which gives back a part of the
	//! real contrast (the core of M42 is some 4 magnitudes brighter than its outer parts, not 2).
	static constexpr float PHOTO_DSO_DESTRETCH = 1.5f;
	//! The frame of the sensor in the view (window coordinates: x, y of the lower left corner, width, height):
	//! the largest rectangle with the proportions of the sensor (turned for a portrait) that fits in the viewport,
	//! centered on the optical axis (the viewport center). The picture is what lies inside. The focal length, the
	//! lens model (its corners are the corners of this frame), the photosites per pixel and the metering refer to it.
	Vec4f getPhotoFrame(const StelProjector& prj) const;
	//! Diagonal of the frame of the sensor in the view (pixels)
	double getPhotoFrameDiagonal(const StelProjector& prj) const;
	//! The sensor in use: diagonal and pitch of a photosite (m)
	double getPhotoSensorDiagonal() const {return getPhotoSensorSpec(getPhotoEffectiveSensorFormat()).diagonal;}
	double getPhotoSensorPitch() const {return getPhotoSensorSpec(getPhotoEffectiveSensorFormat()).pitch;}
	//! The focal length (m) of a lens that gives the field of view fov (degrees, as StelMovementMgr) on the sensor in use
	double getPhotoFocalLengthForFov(double fov) const;
	//! The field of view (degrees, as StelMovementMgr) of a lens of the focal length f (m) on the sensor in use
	double getPhotoFovForFocalLength(double f) const;
	//! The field of view a zoom from @a currentFov towards @a targetFov ends at when the mouse wheel zoom steps
	//! (photoZoomSteps, in photographic mode): the nearest step of the focal length, at least the next one in the
	//! direction of the zoom. Otherwise @a targetFov.
	double getPhotoWheelZoomFov(double currentFov, double targetFov) const;
	//! Write all the settings of the photographic mode to @a conf ("Save settings" of the configuration dialog;
	//! with immediate saving on, each setter writes its own). While the program mode runs, the aperture and the
	//! ISO it chose are not saved: the user's are, which it gives back when it stops.
	void savePhotoSettings(QSettings* conf) const;
	//! Lens model: the default widest aperture of the lens (see setPhotoLensMaxAperture()); it is wide open there
	//! (optical vignetting is strongest there). Allowed range of that setting:
	static constexpr float PHOTO_LENS_MAX_APERTURE = 1.4f;
	static constexpr double PHOTO_LENS_MAX_APERTURE_MIN = 0.95, PHOTO_LENS_MAX_APERTURE_MAX = 8.;
	//! The aerosols of the photographic mode (haze), at the observer: what the humidity makes of the extinction coefficient
	struct PhotoAerosol
	{
		double depth = 0.1;      //!< vertical optical depth above the observer at 550 nm
		double baseDepth = 0.1;  //!< the same at the normal humidity (60%)
		double angstrom = 1.3;   //!< Angstrom exponent: the optical depth goes as wavelength^-angstrom
		double asymmetry = 0.7;  //!< Henyey-Greenstein asymmetry parameter g of the scattering
		double albedo = 0.9;     //!< single scattering albedo
	};
	//! See PhotoAerosol (the normal humidity outside the photographic mode)
	const PhotoAerosol& getPhotoAerosol() const {return photoAerosol;}
	//! How much brighter the light pollution of the sky is with this haze than at the normal humidity (the light of
	//! lamps nearby, scattered towards the zenith; 1 at the normal humidity)
	double getPhotoHazeLightFactor() const {return photoHazeLightFactor;}
	//! The normal humidity: the extinction coefficient and the light pollution setting are for it
	static constexpr double PHOTO_NORMAL_HUMIDITY = 60.;
	//! Whether the aureole of the haze is drawn now (on, photographic mode, on the Earth with its atmosphere, some haze)
	bool getFlagPhotoAureoleActive() const;
	//! The aureole of the haze on the screen: the radius of its core and its extent (pixels, at the centre)
	void getPhotoAureoleSize(const StelProjector& prj, float& core, float& extent) const;
	//! Set the uniforms of photoHazeLight() (xyYToRGB.glsl) of a sky shader: the haze beyond the aerosols of the
	//! ShowMySky models. meanSkyLuminance: of the sky that lights it (cd/m^2); addLight: whether this pass adds the
	//! light of the haze (one pass of the Sun and the Moon)
	void setPhotoHazeUniforms(QOpenGLShaderProgram& program, double meanSkyLuminance, bool addLight) const;
	//! The share of the light that the haze scatters into the aureole, against the light that comes straight through,
	//! on a grid of gw x gh cells over the scene image (w x h pixels, from the lower left): it grows with the airmass
	std::vector<float> getPhotoAureoleMap(int gw, int gh, int w, int h) const;
	//! Optical vignetting: offset of the pupil image against the lens barrel in the corner of the frame,
	//! in units of the barrel radius. 0.81 gives half the light in the corner when wide open.
	static constexpr float PHOTO_VIGNETTING_SHIFT = 0.81f;
	//! The digital sensor (a stock camera, Nikon D5100 as measured by NPL), and the same with its IR-cut filter replaced
	//! by a clear one (red as at 600 nm up to 700 nm), both with the color matrix of the stock camera, white balanced
	//! on daylight: an H II region's line spectrum (H-alpha, [N II], [O III], H-beta, ... as for the films, reddened by
	//! E(B-V) = 0.5). The stock camera sees H-alpha with about 30% of its red, and the [O III] and H-beta lines
	//! in blue and green: emission nebulae come out pink. Per unit V, so R/V = 2.1 instead of the 4-5 of red pictures.
	static constexpr float PHOTO_EMISSION_DIGITAL[3] = {2.085f, 0.951f, 2.246f};
	static constexpr float PHOTO_EMISSION_DIGITAL_HALPHA[3] = {3.531f, 0.794f, 2.364f};
	//! Coma: length of the comet in the corner, wide open, as a fraction of the image diagonal (scales with 1/N^2).
	//! 0.002 is about 90 micrometers on a full frame sensor (15 pixels of 6 micrometers): a good fast lens wide
	//! open, the comet clear in the corners but small; at f/2.8 a quarter of it.
	static constexpr float PHOTO_COMA_LENGTH = 0.002f;
	//! Astigmatism: length of the line image in the corner, wide open, as a fraction of the diagonal (scales with 1/N).
	//! 0.0015 is about 65 micrometers on a full frame sensor.
	static constexpr float PHOTO_ASTIGMATISM_LENGTH = 0.0015f;
	//! Mounts of the camera (see photoMount)
	enum PhotoMount {
		PhotoMountTripod = 0, //!< fixed on the ground: the stars draw trails
		PhotoMountTracker = 1, //!< turns with the sky (around the pole): the stars stay sharp, the landscape blurs
		PhotoMountView = 2     //!< as the mount of the view (StelMovementMgr): a tripod on the alt-azimuthal mount,
		                       //!< a tracker on the equatorial one (and the galactic ones, which turn with the sky too)
	};
	//! Sensor formats (the values are stored in the settings: keep them)
	enum PhotoSensorFormat {
		PhotoSensorFullFrame = 0,      //!< 36 x 24 mm
		PhotoSensorAPSC = 1,           //!< 23.5 x 15.6 mm (crop factor 1.53, as Nikon, Sony, Fujifilm, Pentax)
		PhotoSensorAPSCCanon = 2,      //!< 22.3 x 14.9 mm (crop factor 1.61, Canon)
		PhotoSensorMicroFourThirds = 3,//!< 17.3 x 13.0 mm (crop factor 2.0, Olympus/OM System, Panasonic)
		PhotoSensorOneInch = 4,        //!< 13.2 x 8.8 mm (crop factor 2.7, compact cameras)
		PhotoSensorMediumFormat = 5,   //!< 43.8 x 32.9 mm (crop factor 0.79, Fujifilm GFX, Hasselblad X)
		PhotoSensorTwoThirds = 6,      //!< 2/3": 8.8 x 6.6 mm (crop factor 3.9, Fujifilm X10/X20, bridge cameras)
		PhotoSensor1_1_7 = 7,          //!< 1/1.7": 7.6 x 5.7 mm (crop factor 4.6, Canon PowerShot G and S series)
		PhotoSensor1_2_3 = 8           //!< 1/2.3": 6.17 x 4.55 mm (crop factor 5.6, most compact and superzoom cameras)
	};
	static constexpr int PHOTO_SENSOR_FORMAT_COUNT = 9;
	//! Size of a sensor: its diagonal (m) and the pitch of a photosite (m), and a short name for the info line
	//! (empty for full frame). The diagonal of the view is the diagonal of the sensor, which gives the focal length
	//! of the lens: f = (sensor diagonal) x (focal length in pixels) / (diagonal of the view in pixels).
	struct PhotoSensorSpec
	{
		double diagonal;
		double pitch;
		const char* shortName;
		double aspect; //!< width / height (long side over short side)
	};
	static const PhotoSensorSpec& getPhotoSensorSpec(int format);
	//! Films (the values are stored in the settings: keep them)
	enum PhotoFilm {
		PhotoFilmNone = 0,           //!< the digital sensor
		PhotoFilmProvia100F = 1,     //!< Fujichrome Provia 100F (RDP III), color slide film
		PhotoFilmProvia400F = 2,     //!< Fujichrome Provia 400F (RHP III), color slide film
		PhotoFilmEktachromeE200 = 3, //!< Kodak Ektachrome E200 (EPD, as Elite Chrome 200), color slide film
		PhotoFilmRoyalGold400 = 4,   //!< Kodak Royal Gold 400, color negative film (printed)
		PhotoFilmTMax400 = 5,        //!< Kodak T-Max 400, black and white negative film (printed)
		PhotoFilmTriX400 = 6         //!< Kodak Tri-X 400, black and white negative film (printed)
	};
	static constexpr int PHOTO_FILM_COUNT = 7;
	enum PhotoFilmType { PhotoFilmSlide = 0, PhotoFilmNegative = 1, PhotoFilmMonochrome = 2 };
	//! The data of a film, from the manufacturers' data sheets where they give them. The films themselves are listed
	//! in StelSkyDrawerPhotoFilm.cpp; the default values are those of the digital sensor (no film).
	struct PhotoFilmSpec
	{
		const char* name = "Digital sensor";
		const char* shortName = "";
		double iso = 0.;
		int type = PhotoFilmSlide;  //!< PhotoFilmType
		//! Reciprocity failure: pairs (exposure time, effective time) in seconds, increasing. Up to the first time
		//! the film follows the reciprocity law; between the points log-log interpolation; after the last point
		//! the Schwarzschild exponent (the effective time grows as t^p).
		int points = 0;
		std::array<double, 4> time = {};
		std::array<double, 4> effective = {};
		double exponent = 1.;
		//! The color shift of long exposures: the green filter the data sheet asks for at the times of time[]
		//! (CC density; the film loses green, the picture turns magenta)
		std::array<double, 4> green = {};
		double granularity = 0.;  //!< diffuse RMS granularity x 1000 (48 micrometer aperture, density 1 above the base)
		double gamma = 1.;        //!< slope of the characteristic curve of the film (density per log exposure)
		float toneA = 1.f;        //!< tone curve of the picture: slope (system gamma), clipping point and middle grey
		float toneClip = 1.f;
		float toneMid = 0.18f;
		//! Black and white films: their sensitivity to linear red, green and blue (sum 1)
		std::array<float, 3> mono = {};
		//! Color films with spectral data: their own sensitivity to the sky, the stars, the airglow and the nebulae
		//! (the four tables below)
		bool spectral = false;
		//! Color films: the exposures of the red, green and blue layers from the linear sRGB of the scene (row by row;
		//! daylight D55 gives 1, 1, 1), from the spectral sensitivities of the data sheet (see getPhotoFilmSpec())
		std::array<float, 9> rgbToFilm = {};
		//! Color films: the sky from its spectrum, as ShowMySky gives it (luminance X, Y, Z and the scotopic W, cd/m^2):
		//! the linear sRGB of the scene that the layers (rgbToFilm) record as the film does (row by row, 3 x 4)
		std::array<float, 12> skyXYZW = {};
		//! Color films: the airglow, computed from its line spectrum (linear sRGB of the scene per unit luminance)
		std::array<float, 3> airglow = {};
		//! Color films: an emission nebula (H II region), from its line spectrum: the linear sRGB of the scene for
		//! a nebula as bright in V as white light of luminance 1 (see StelSkyImageTile)
		std::array<float, 3> emission = {};
	};
	//! Whether a film has spectral data (a color film): its own sensitivity to the sky, the stars and the airglow
	static bool getPhotoFilmSpectral(const PhotoFilmSpec& f) {return f.spectral;}
	//! The sky in the spectral model of the color film in use (rows of getPhotoFilmSpec().skyXYZW), or nullptr
	const float* getPhotoFilmSkyMatrix() const;
	//! The data of a film (PhotoFilm; out of range: the nearest one), see StelSkyDrawerPhotoFilm.cpp
	static const PhotoFilmSpec& getPhotoFilmSpec(int film);
	//! The film in use with its push processing (speed, contrast and grain changed, see setPhotoFilmPush())
	const PhotoFilmSpec& getPhotoActiveFilmSpec() const;
	static constexpr int PHOTO_FILM_MAX_PUSH = 2;
	//! Whether a film is in use now (photographic mode on)
	bool getFlagPhotoFilmActive() const {return photoFilm != PhotoFilmNone && getFlagPhotographicModeActive();}
	//! The exposure time that counts for the film (reciprocity failure), for the exposure time t (seconds)
	double getPhotoFilmEffectiveTime(double t) const;
	//! The time a film f counts an exposure of t seconds as (reciprocity failure, see PhotoFilmSpec), for the whole
	//! picture (getPhotoFilmEffectiveTime()) and for each point after its brightness (photoFilmRecordedExposure())
	static double photoFilmEffectiveTime(const PhotoFilmSpec& f, double t);
	//! The relative sensitivity of the red, green and blue layers of the film after the exposure time t (seconds)
	Vec3f getPhotoFilmTint(double t) const;
	//! Black and white film: the weights of linear red, green and blue (0 otherwise)
	Vec3f getPhotoFilmMonoWeights() const;
	//! The tone curve in use: the film's own (PhotoToneFilm with the film's parameters), or photoToneCurve
	int getPhotoActiveToneCurve() const {return photoFilm != PhotoFilmNone ? static_cast<int>(PhotoToneFilm) : photoToneCurve;}
	//! The sensor format in use: a film is 35 mm (full frame)
	int getPhotoEffectiveSensorFormat() const {return photoFilm != PhotoFilmNone ? static_cast<int>(PhotoSensorFullFrame) : photoSensorFormat;}
	//! Full frame: the reference of the noise model (see PHOTO_SENSOR_PITCH)
	static constexpr double PHOTO_SENSOR_DIAGONAL = 0.043267;
	//! Focus: the closest focus distance of the lens (meters)
	static constexpr double PHOTO_FOCUS_MIN_DISTANCE = 0.3;
	//! Focus: how far the focus ring turns beyond the infinity mark, as a distance (meters, see photoFocusDistance)
	static constexpr double PHOTO_FOCUS_BEYOND_INFINITY = 30.;
	//! Focus: brightness change from the center to the edge of a defocused star image from the spherical
	//! aberration of the lens, wide open (it scales with 1/N^2 and the aberration strength)
	static constexpr float PHOTO_SPHERICAL_ABERRATION = 0.6f;
	//! Lateral chromatic aberration: the red image is a little larger than the green one, the blue one a little
	//! smaller. Shift of each in the corner, as a fraction of the image diagonal (red and blue then lie 0.08% of
	//! the diagonal apart there, an average wide-angle lens). It does not change with the aperture.
	static constexpr float PHOTO_LATERAL_CA = 0.0004f;
	//! Axial chromatic aberration (secondary spectrum): red and blue focus a little behind green, as a fraction
	//! of the focal length. Their blur disks have the radius (shift)/(2N): colored (purple) fringes around bright
	//! stars with the lens wide open, gone when stopped down.
	static constexpr float PHOTO_AXIAL_CA_RED = 1.f/1600.f;
	static constexpr float PHOTO_AXIAL_CA_BLUE = 1.f/800.f;
	//! The lens model for the given projection: center of the image (optical axis), half diagonal and focal length
	//! (all in window pixels), relative pupil size (1 = wide open) and the aberration lengths in the corner (pixels)
	struct PhotoLens
	{
		Vec2f center;
		float corner = 1.f;
		float focal = 1.f;
		float pupil = 1.f;
		float coma = 0.f;
		float astigmatism = 0.f;
		bool vignetting = false;
		//! Defocus: radius of the blur disk of a star (the image of the aperture), pixels. Positive when the
		//! lens is focused nearer than infinity (the image of the star lies in front of the sensor), negative
		//! beyond infinity (behind the sensor). 0 = in focus.
		float defocus = 0.f;
		//! Defocus: relative brightness change from the center to the edge of the blur disk (spherical aberration)
		float sphericalAberration = 0.f;
		//! Chromatic aberration: radial shift of the red, green and blue images in the corner (pixels, outwards
		//! positive), and the radius of their blur disks (pixels). Zero: none.
		Vec3f chromaShift = Vec3f(0.f);
		Vec3f chromaBlur = Vec3f(0.f);
	};
	PhotoLens getPhotoLens(const StelProjector& prj) const;
	//! Time constant of the smooth adaptation of the automatic white balance (seconds)
	static constexpr double PHOTO_AWB_TIME_CONSTANT = 0.7;
	//! The color temperature (K) whose white light has the given blue/red ratio (linear sRGB), within the AWB range
	static double photoKelvinForBlueRedRatio(double ratio);
	//! Range of the automatic white balance (K), like in cameras
	static constexpr double PHOTO_AWB_MIN_KELVIN = 3000.;
	static constexpr double PHOTO_AWB_MAX_KELVIN = 8000.;
	//! Steps of the mouse wheel zoom in the photographic mode
	enum PhotoZoomSteps {
		PhotoZoomFree = 0,               //!< any focal length
		PhotoZoomWholeMillimeters = 1,   //!< whole millimeters, as on the scale of a zoom lens
		PhotoZoomCommonFocalLengths = 2  //!< the focal lengths of common lenses (getPhotoCommonFocalLengths())
	};
	//! The focal lengths of common lenses and telescopes, in millimeters, increasing (the steps of PhotoZoomCommonFocalLengths)
	static const std::vector<double>& getPhotoCommonFocalLengths();
	//! Metering modes of the automatic exposure
	enum PhotoMeteringMode {
		PhotoMeteringCenterWeighted = 0, //!< 75% in a central circle, 25% over the whole image
		PhotoMeteringSpot = 1,           //!< a central circle of 2.5% of the image area
		PhotoMeteringHighlight = 2       //!< the brightest part must not saturate
	};
	//! Tone curves of the camera pass (scene value after the exposure -> linear display value)
	enum PhotoToneCurve {
		PhotoToneLinear = 0,   //!< linear, with a soft shoulder above PHOTO_SHOULDER_KNEE (never clips)
		PhotoToneStandard = 1, //!< like the JPEG of a camera: more contrast, clipping 3 stops above middle grey
		PhotoToneFilm = 2      //!< like negative film: soft contrast, 6 stops of room above middle grey
	};
	//! Parameters of a tone curve y = x^a / (x^(a*d)*b + c) for x below the clipping point, 1 above
	//! (T. Lottes, "Advanced Techniques and Optimization of HDR Color Pipelines", GDC 2016). It maps
	//! middle grey (0.18) to middle grey, and the clipping point to white.
	struct PhotoToneParams
	{
		float a = 1.f;   //!< contrast (slope in log-log space around middle grey)
		float ad = 1.f;  //!< a*d, d shapes the shoulder
		float b = 0.f;
		float c = 1.f;
		float clip = 1.f; //!< scene value that becomes white (saturation of the sensor)
	};
	//! The inverse of the highlight shoulder of the linear tone curve (for values below 1.0).
	static float invertPhotoShoulder(float y);
	//! The tone curve in use: scene value (after the exposure) -> linear display value 0..1
	float photoToneCurveValue(float x) const;
	//! The inverse of the tone curve in use (exact, by bisection): display value -> scene value
	float photoToneCurveInverse(float y) const;
	//! Parameters of the tone curve in use (not meaningful for PhotoToneLinear)
	const PhotoToneParams& getPhotoToneParams() const {return photoToneParams;}
	//! For annotations (lines, labels, markers) in the photographic mode: the linear color that has
	//! to be written into the scene buffer so that the camera pass reproduces the given linear color
	//! exactly, independently of the exposure.
	Vec3f photoAnnotationCompensation(const Vec3f& linearColor) const;

	//! Constructor
	StelSkyDrawer(StelCore* core);
	//! Destructor
	~StelSkyDrawer() override;

	//! Init parameters from config file
	void init();

	//! Update with respect to the time and StelProjector/StelToneReproducer state
	//! @param deltaTime the time increment in second since last call.
	void update(double deltaTime);

	//! Set the proper openGL state before making calls to drawPointSource
	//! @param p a pointer to a valid instance of a Painter. The instance must be valid until postDrawPointSource() is called
	void preDrawPointSource(StelPainter* p);

	//! Finalize the drawing of point sources
	//! Temporary: test if calling this in corona makes any sense.
	void postDrawPointSource(StelPainter* sPainter, bool drawInCorona=false);

	//! Draw a point source halo.
	//! @param sPainter the StelPainter to use for drawing.
	//! @param v the 3d position of the source in J2000 reference frame
	//! @param rcMag the radius and luminance of the source as computed by computeRCMag()
	//! @param bVindex the source B-V index (into the private colorTable. This is not the astronomical B-V value.)
	//! @param checkInScreen whether source in screen should be checked to avoid unnecessary drawing.
	//! @param twinkleFactor allows height-dependent twinkling. Recommended value: min(1,1-0.9*sin(altitude)). Allowed values [0..1]
	//! @param photoTint photographic mode: the color of the extinction (see getPhotoExtinctionTint()), or nullptr
	//! @return true if the source was actually visible and drawn
	bool drawPointSource(StelPainter* sPainter, const Vec3d& v, const RCMag &rcMag, int bVindex, bool checkInScreen=false, float twinkleFactor=1.0f, const Vec3f* photoTint=nullptr)
	{
		if (getFlagPhotographicModeActive())
		{
			if (photoTint)
			{
				// Tinted color, again at unit luminance (the brightness comes from the magnitude)
				const Vec3f& c = photoColorTable[bVindex];
				const Vec3f tinted(c[0]*(*photoTint)[0], c[1]*(*photoTint)[1], c[2]*(*photoTint)[2]);
				const float Y = 0.2126f*tinted[0] + 0.7152f*tinted[1] + 0.0722f*tinted[2];
				if (Y > 0.f)
					return drawPhotoPointSource(sPainter, v, rcMag, tinted/Y, 1.f, checkInScreen);
			}
			return drawPhotoPointSource(sPainter, v, rcMag, photoColorTable[bVindex], 1.f, checkInScreen);
		}
		return drawPointSource(sPainter, v, rcMag, colorTable[bVindex], checkInScreen, twinkleFactor);
	}

	//! Draw a point source halo.
	//! @param sPainter the StelPainter to use for drawing.
	//! @param v the 3d position of the source in J2000 reference frame
	//! @param rcMag the radius and luminance of the source as computed by computeRCMag()
	//! @param color the RGB color value (0...1, 0...1, 0...1)
	//! @param checkInScreen whether source in screen should be checked to avoid unnecessary drawing.
	//! @param twinkleFactor allows height-dependent twinkling. Recommended value: min(1,1-0.9*sin(altitude)). Allowed values [0..1]
	//! @return true if the source was actually visible and drawn
	bool drawPointSource(StelPainter* sPainter, const Vec3d& v, const RCMag &rcMag, const Vec3f& bcolor, bool checkInScreen=false, float twinkleFactor=1.0f);

	//! Draw an image of the solar corona onto the screen at position v.
	//! @param radius of current disk size of the sun [radians].
	//! @param color solar color (after atmospheric extinction).
	//! @param alpha opacity value. Set 1 for full visibility, but usually keep close to 0 except during solar eclipses.
	void drawSunCorona(StelPainter* painter, const Vec3d& posJ2000, double radius, const Vec3f& color, const float alpha);

	//! Terminate drawing of a 3D model, draw the halo
	//! @param p the StelPainter instance to use for this drawing operation
	//! @param v the 3d position of the source in J2000 reference frame
	//! @param illuminatedArea the illuminated area in arcmin^2
	//! @param mag the source integrated magnitude
	//! @param color the object halo RGB color
	//! @param isSun the object is the sun (will be drawn with different texture)
	void postDrawSky3dModel(StelPainter* p, const Vec3d& v, float illuminatedArea, float mag, const Vec3f& color = Vec3f(1.f,1.f,1.f), const bool isSun=false);

	//! Compute RMag and CMag from magnitude.
	//! @param mag the object integrated V magnitude
	//! @param rcMag array of 2 floats containing the radius and luminance
	//! @return false if the object is too faint to be displayed
	bool computeRCMag(float mag, RCMag*) const;

	//! Report that an object of luminance lum with an on-screen area of area pixels is currently displayed
	//! This information is used to determine the world adaptation luminance
	//! This method should be called during the update operations of the main loop
	//! @param lum luminance in cd/m^2
	//! @param fastAdaptation adapt the eye quickly if true, other wise use a smooth adaptation
	void reportLuminanceInFov(float lum, bool fastAdaptation=false);

	//! To be called before the drawing stage starts
	void preDraw();

	//! Compute the luminance for an extended source with the given surface brightness
	//! @param sb surface brightness in V magnitude/arcmin^2
	//! @return the luminance in cd/m^2
	static float surfaceBrightnessToLuminance(float sb);
	//! Compute the surface brightness from the luminance of an extended source
	//! @param lum luminance in cd/m^2
	//! @return surface brightness in V magnitude/arcmin^2
	static float luminanceToSurfacebrightness(float lum);

	//! Convert quantized B-V index to float B-V
	static inline float indexToBV(unsigned char bV)
	{
		return static_cast<float>(bV)*(4.f/127.f)-0.5f;
	}

	//! Convert quantized B-V index to RGB colors
	static inline const Vec3f& indexToColor(int bV)
	{
		return colorTable[bV];
	}

public slots:
	//! Set the way brighter stars will look bigger as the fainter ones
	void setRelativeStarScale(double b=1.0);
	//! Get the way brighter stars will look bigger as the fainter ones
	double getRelativeStarScale() const {return starRelativeScale;}

	//! Set the absolute star brightness scale
	void setAbsoluteStarScale(double b=1.0);
	//! Get the absolute star brightness scale
	double getAbsoluteStarScale() const {return starAbsoluteScaleF;}

	//! Set source twinkle amount.
	void setTwinkleAmount(double b);
	//! Get source twinkle amount.
	double getTwinkleAmount() const {return twinkleAmount;}

	//! Set flag for source twinkling.
	void setFlagTwinkle(bool b);
	//! Get flag for source twinkling.
	bool getFlagTwinkle() const {return flagStarTwinkle;}

	//! Set flag for enable twinkling of stars without atmosphere.
	//! @note option for planetariums
	void setFlagForcedTwinkle(bool b);
	//! Get flag for enable twinkling of stars without atmosphere.
	//! @note option for planetariums
	bool getFlagForcedTwinkle() const {return flagForcedTwinkle;}

	//! Set the parameters so that the stars disappear at about the naked-eye limiting magnitude corresponding
	//! to the given zenith luminance at moonless night.
	//! The limit is valid only at a given zoom level (around 60 deg)
	void setLightPollutionLuminance(double luminance);
	//! Get the current zenith luminance at moonless night.
	double getLightPollutionLuminance() const {return lightPollutionLuminance;}

	Q_DECL_DEPRECATED int getBortleScaleIndex() const;

	//! Set flag for drawing a halo around bright stars.
	void setFlagDrawBigStarHalo(bool b);
	//! Get flag for drawing a halo around bright stars.
	bool getFlagDrawBigStarHalo() const {return flagDrawBigStarHalo;}

	//! Set flag to draw stars with rays
	void setFlagStarSpiky(bool b);
	//! Get whether to draw stars with rays
	bool getFlagStarSpiky() const {return flagStarSpiky;}

	//! Get the magnitude of the currently faintest visible point source
	//! It depends on the zoom level, on the eye adaptation and on the point source rendering parameters
	//! @return the limit V mag at which a point source will be displayed
	float getLimitMagnitude() const {return limitMagnitude;}

	//! Toggle the application of user-defined star magnitude limit.
	//! If enabled, stars fainter than the magnitude set with
	//! setCustomStarMagnitudeLimit() will not be displayed.
	void setFlagStarMagnitudeLimit(bool b);
	//! @return true if the user-defined star magnitude limit is in force.
	bool getFlagStarMagnitudeLimit() const {return flagStarMagnitudeLimit;}
	//! Toggle the application of user-defined deep-sky object magnitude limit.
	//! If enabled, deep-sky objects fainter than the magnitude set with
	//! setCustomNebulaMagnitudeLimit() will not be displayed.
	void setFlagNebulaMagnitudeLimit(bool b);
	//! @return true if the user-defined nebula magnitude limit is in force.
	bool getFlagNebulaMagnitudeLimit() const {return flagNebulaMagnitudeLimit;}
	//! Toggle the application of user-defined solar system object magnitude limit.
	//! If enabled, planets, planetary moons, asteroids (KBO, ...) and comets fainter than the magnitude set with
	//! setCustomPlanetMagnitudeLimit() will not be displayed.
	void setFlagPlanetMagnitudeLimit(bool b);
	//! @return true if the user-defined nebula magnitude limit is in force.
	bool getFlagPlanetMagnitudeLimit() const {return flagPlanetMagnitudeLimit;}

	//! Get the value used for forced star magnitude limiting.
	double getCustomStarMagnitudeLimit() const {return customStarMagLimit;}
	//! Sets a lower limit for star magnitudes (anything fainter is ignored).
	//! In force only if flagStarMagnitudeLimit is set.
	void setCustomStarMagnitudeLimit(double limit);
	//! Get the value used for forced nebula magnitude limiting.
	double getCustomNebulaMagnitudeLimit() const {return customNebulaMagLimit;}
	//! Sets a lower limit for nebula magnitudes (anything fainter is ignored).
	//! In force only if flagNebulaMagnitudeLimit is set.
	void setCustomNebulaMagnitudeLimit(double limit);
	//! Get the value used for forced solar system object magnitude limiting.
	double getCustomPlanetMagnitudeLimit() const {return customPlanetMagLimit;}
	//! Sets a lower limit for solar system object magnitudes (anything fainter is ignored).
	//! In force only if flagPlanetMagnitudeLimit is set.
	void setCustomPlanetMagnitudeLimit(double limit);

	//! Get the luminance of the faintest visible object (e.g. RGB<0.05)
	//! It depends on the zoom level, on the eye adaptation and on the point source rendering parameters
	//! @return the limit V luminance at which an object will be visible
	float getLimitLuminance() const {return limitLuminance;}

	//! Set the value of the eye adaptation flag
	void setFlagLuminanceAdaptation(bool b);
	//! Get the current value of eye adaptation flag
	bool getFlagLuminanceAdaptation() const {return flagLuminanceAdaptation;}

	//! Set the label brightness threshold
	void setDaylightLabelThreshold(double t);
	//! Get the current label brightness threshold
	double getDaylightLabelThreshold() const {return daylightLabelThreshold;}
	//! Return a brightness value based on objects in view (sky, sun, moon, ...)
	double getWorldAdaptationLuminance() const;

	//! Switch the photographic (camera-like) rendering mode on or off.
	//! In this mode the scene is rendered into a floating-point buffer and a final
	//! "camera" pass applies the exposure and a soft saturation of the highlights.
	//! The mode needs high graphics mode (OpenGL 3+); otherwise it has no effect.
	void setFlagPhotographicMode(bool b);
	//! Get whether the photographic rendering mode is switched on.
	bool getFlagPhotographicMode() const {return flagPhotographicMode;}
	//! Toggle the photographic rendering mode.
	void toggleFlagPhotographicMode() {setFlagPhotographicMode(!flagPhotographicMode);}
	//! Set the exposure correction of the photographic mode, in stops (EV).
	//! +1 EV doubles the recorded light, -1 EV halves it. The value is limited to [-12, +12].
	void setPhotoExposureEV(double ev);
	//! Get the exposure correction of the photographic mode, in stops (EV).
	double getPhotoExposureEV() const {return photoExposureEV;}
	//! Increase the exposure by 1/3 stop, like one click on a camera dial.
	void increasePhotoExposure();
	//! Decrease the exposure by 1/3 stop, like one click on a camera dial.
	void decreasePhotoExposure();
	//! Reset the exposure correction to 0 EV.
	void resetPhotoExposure();
	//! Get the linear factor of the exposure compensation, i.e. 2^EV. It is applied in the final camera pass.
	float getPhotoExposureFactor() const;
	//! Whether the photographic mode is switched on AND can work (high graphics mode).
	//! Modules should use this one to decide how to render.
	bool getFlagPhotographicModeActive() const;

	//! Set the exposure time of the virtual camera, in seconds (limited to 1/8000 s ... 3600 s).
	void setPhotoExposureTime(double seconds);
	//! Get the exposure time of the virtual camera, in seconds.
	double getPhotoExposureTime() const {return photoExposureTime;}
	//! Set the aperture (f-number) of the virtual camera (limited to f/0.7 ... f/64).
	void setPhotoAperture(double fNumber);
	//! The widest aperture of the lens (its smallest f-number): neither the user nor the program mode can open
	//! wider. The lens model (vignetting, aberrations, the shape of the aperture) counts from it: a slow lens wide
	//! open behaves as a fast one wide open (config: viewing/photographic_lens_max_aperture)
	void setPhotoLensMaxAperture(double fNumber);
	double getPhotoLensMaxAperture() const {return photoLensMaxAperture;}
	//! Get the aperture (f-number) of the virtual camera.
	double getPhotoAperture() const {return photoAperture;}
	//! Set the ISO sensitivity of the virtual camera (limited to 25 ... 409600).
	void setPhotoISO(double iso);
	//! Get the ISO sensitivity of the virtual camera.
	double getPhotoISO() const {return photoISO;}
	//! Set the smallest star image width (FWHM) in pixels (0.8 ... 10).
	void setPhotoPsfMinFwhm(double pixels);
	double getPhotoPsfMinFwhm() const {return static_cast<double>(photoMinFwhmPixels);}
	//! Set the atmospheric seeing (FWHM) in arc seconds (0 ... 60).
	void setPhotoSeeing(double arcsec);
	double getPhotoSeeing() const {return static_cast<double>(photoSeeingArcsec);}
	//! Set the fraction of a star's light in the lens halo (0 ... 0.3).
	void setPhotoHaloStrength(double fraction);
	double getPhotoHaloStrength() const {return static_cast<double>(photoHaloFraction);}
	//! Set the number of diaphragm blades (0 = round aperture, 4 ... 9).
	void setPhotoApertureBlades(int blades);
	int getPhotoApertureBlades() const {return photoApertureBlades;}
	//! Number of diffraction spikes the diaphragm makes (0, or the blades, or twice the blades if odd)
	int getPhotoSpikeCount() const {return 2*getPhotoSpikeLines();}
	//! Set the fraction of a star's light in the diffraction spikes (0 ... 0.1).
	void setPhotoSpikeStrength(double fraction);
	double getPhotoSpikeStrength() const {return static_cast<double>(photoSpikeFraction);}
	//! Switch the image-based lens halo ("bloom") of the photographic mode on or off.
	void setFlagPhotoBloom(bool b);
	bool getFlagPhotoBloom() const {return flagPhotoBloom;}
	//! Switch the automatic exposure (aperture priority) of the photographic mode on or off.
	void setFlagPhotoAutoExposure(bool b);
	bool getFlagPhotoAutoExposure() const {return flagPhotoAutoExposure;}
	void setFlagPhotoProgramExposure(bool b);
	//! Program mode: keep the aperture the user set and let the camera choose only the time and the ISO (as
	//! aperture priority with automatic ISO does on a camera) (config: viewing/flag_photographic_program_aperture_lock)
	void setFlagPhotoProgramApertureLock(bool b);
	bool getFlagPhotoProgramApertureLock() const {return flagPhotoProgramApertureLock;}
	bool getFlagPhotoProgramExposure() const {return flagPhotoProgramExposure;}
	//! Set the highest ISO of the program mode (100 to 204800; a film keeps its own speed)
	void setPhotoProgramMaxISO(double iso);
	double getPhotoProgramMaxISO() const {return photoProgramMaxISO;}
	//! Set the longest exposure time of the program mode, in seconds (1/30 to 30)
	void setPhotoProgramMaxTime(double seconds);
	double getPhotoProgramMaxTime() const {return photoProgramMaxTime;}
	//! The program mode is in use (automatic exposure in program mode, photographic mode on)
	bool getFlagPhotoProgramExposureActive() const {return flagPhotoProgramExposure && getFlagPhotoAutoExposureActive();}
	//! Whether the automatic exposure is switched on AND the photographic mode is active.
	bool getFlagPhotoAutoExposureActive() const;
	//! Set the metering mode of the automatic exposure (see PhotoMeteringMode).
	void setPhotoMeteringMode(int mode);
	int getPhotoMeteringMode() const {return photoMeteringMode;}
	//! Set the tone curve of the camera (see PhotoToneCurve).
	void setPhotoToneCurve(int curve);
	int getPhotoToneCurve() const {return photoToneCurve;}
	void setFlagPhotoVignetting(bool b);
	bool getFlagPhotoVignetting() const {return flagPhotoVignetting;}
	void setFlagPhotoHalphaModified(bool b);
	bool getFlagPhotoHalphaModified() const {return flagPhotoHalphaModified;}
	//! An emission nebula (H II region) as the camera in use records it: the linear sRGB of the scene for a nebula as
	//! bright in V as white light of luminance 1 (see StelSkyImageTile). Negative: the camera has no spectral model
	//! (black and white film), the colors of the picture are used.
	Vec3f getPhotoEmissionColor() const;
	void setFlagPhotoLensFlare(bool b);
	bool getFlagPhotoLensFlare() const {return flagPhotoLensFlare;}
	void setPhotoLensFlareStrength(double s);
	double getPhotoLensFlareStrength() const {return static_cast<double>(photoLensFlareStrength);}
	//! Set the distance the lens is focused at (meters; 0 = infinity, negative = beyond infinity, see photoFocusDistance)
	void setPhotoFocusDistance(double meters);
	double getPhotoFocusDistance() const {return photoFocusDistance;}
	void setFlagPhotoStarTrails(bool b);
	bool getFlagPhotoStarTrails() const {return flagPhotoStarTrails;}
	//! Set the mount of the camera (see PhotoMount)
	void setPhotoMount(int mount);
	int getPhotoMount() const {return photoMount;}
	//! The mount in effect: PhotoMountTripod or PhotoMountTracker (PhotoMountView resolved with the mount of the view)
	int getPhotoEffectiveMount() const;
	//! Photographic mode, star tracker: the landscape turns past the camera during the exposure. The landscape is
	//! drawn into an offscreen image between these calls (its horizon line and labels after, see Landscape), which
	//! endPhotoGroundImage() then puts into the scene averaged along that motion. Returns false (draw directly)
	//! when the camera does not follow the sky, the motion is too small to show, or the image cannot be made.
	bool beginPhotoGroundImage();
	void endPhotoGroundImage();
	//! Set how the sensor noise is shown (see PhotoNoise)
	void setPhotoNoise(int mode);
	int getPhotoNoise() const {return photoNoise;}
	//! The focal length (meters) of the camera lens with the field of the given projection (on the sensor in use)
	double getPhotoFocalLength(const StelProjector& prj) const;
	//! Set the sensor format (see PhotoSensorFormat). The lens stays: the field of view changes as when the lens
	//! is put on the other camera.
	void setPhotoSensorFormat(int format);
	int getPhotoSensorFormat() const {return photoSensorFormat;}
	//! Put a film in the camera (see PhotoFilm), or the digital sensor back (0). The lens stays.
	void setPhotoFilm(int film);
	int getPhotoFilm() const {return photoFilm;}
	//! Push processing of the film in stops (0 to PHOTO_FILM_MAX_PUSH)
	void setPhotoFilmPush(int stops);
	int getPhotoFilmPush() const {return photoFilmPush;}
	//! Color film: the sky from its spectrum (ShowMySky's X, Y, Z and scotopic W; the lightweight atmosphere
	//! estimates W), else from its color like the rest of the scene (config: astro/flag_photographic_film_spectral_sky)
	void setFlagPhotoFilmSpectralSky(bool b);
	bool getFlagPhotoFilmSpectralSky() const {return flagPhotoFilmSpectralSky;}
	//! Zoom the view to the field of a lens of the given focal length (millimeters) on the sensor in use
	void setPhotoFocalLength(double millimeters);
	//! The steps of the mouse wheel zoom (PhotoZoomSteps)
	void setPhotoZoomSteps(int steps);
	int getPhotoZoomSteps() const {return photoZoomSteps;}
	//! Turn the camera for a portrait picture. The lens stays: the field of view changes with the frame.
	void setFlagPhotoPortrait(bool b);
	bool getFlagPhotoPortrait() const {return flagPhotoPortrait;}
	void setFlagPhotoFrameMask(bool b);
	bool getFlagPhotoFrameMask() const {return flagPhotoFrameMask;}
	void setPhotoEclipseAtmosphere(int state);
	int getPhotoEclipseAtmosphere() const {return photoEclipseAtmosphere;}
	//! Photographic mode: a bright body whose light can be reflected inside the lens (see StelPhotoLensFlare).
	//! It may lie outside the frame. Collected while drawing, the ghosts are drawn by drawDeferredPhotoBodies().
	//! @param v position (J2000 frame), @param mag magnitude (with extinction and landscape), @param color display color
	void addPhotoGhostSource(const Vec3d& v, float mag, const Vec3f& color);
	void setFlagPhotoAberrations(bool b);
	bool getFlagPhotoAberrations() const {return flagPhotoAberrations;}
	void setPhotoAberrationStrength(double s);
	double getPhotoAberrationStrength() const {return static_cast<double>(photoAberrationStrength);}
	//! Photographic mode: the detection limit, as a signal to noise ratio. A star is drawn only when the light of
	//! its core stands out of the noise of the sky and the sensor by this factor (3: barely seen in the grain;
	//! 5: the usual limit of detection). Higher: fewer faint stars, faster. 0: all stars are drawn.
	void setPhotoDetectionSnr(double snr);
	double getPhotoDetectionSnr() const {return static_cast<double>(photoDetectionSnr);}
	//! Set the white balance (see PhotoWhiteBalance).
	void setPhotoWhiteBalance(int wb);
	int getPhotoWhiteBalance() const {return photoWhiteBalance;}
	//! Set the color temperature (K, 2000 ... 12000) of the custom white balance.
	void setPhotoWhiteBalanceKelvin(double kelvin);
	double getPhotoWhiteBalanceKelvin() const {return photoWhiteBalanceKelvin;}
	//! The color temperature (K) the camera neutralizes now (for the automatic white balance: its current choice)
	double getPhotoWhiteBalanceTemperature() const;
	//! Color temperature (K) of a white balance preset (0 for automatic)
	static double photoWhiteBalancePresetKelvin(int wb);
	//! Whether the automatic white balance is in use (photographic mode active)
	bool getFlagPhotoAutoWhiteBalanceActive() const;
	//! The white balance as a matrix on linear sRGB (light of the chosen color temperature -> display white)
	//! (with a film: its fixed daylight balance and the color shift of long exposures)
	const float* getPhotoWhiteBalanceMatrix() const;
	//! Scene value where the sensor saturates (for the exposure meter): the clipping point of the tone curve
	//! before the exposure compensation
	double getPhotoMeterClipLevel() const;
	//! Switch the extinction colors of the photographic mode on or off.
	void setFlagPhotoExtinctionColor(bool b);
	bool getFlagPhotoExtinctionColor() const {return flagPhotoExtinctionColor;}
	//! Whether the extinction colors come from the transmission table now: switched on, photographic mode,
	//! atmosphere with extinction, table loaded, observer on the Earth.
	bool getFlagPhotoExtinctionColorActive() const {return photoExtinctionActive;}
	//! Switch the airglow of the photographic mode on or off.
	void setFlagPhotoAirglow(bool b);
	bool getFlagPhotoAirglow() const {return flagPhotoAirglow;}
	//! Set the brightness of the airglow relative to its mean (0.25 to 2)
	void setPhotoAirglowStrength(double strength);
	double getPhotoAirglowStrength() const {return static_cast<double>(photoAirglowStrength);}
	//! The airglow at the zenith above the atmosphere, in linear RGB of the scene buffer (photographic mode, on the
	//! Earth, with the airglow on; otherwise 0). The brightness follows the solar cycle as in Skybright (Schaefer).
	Vec3f getPhotoAirglow() const;
	//! The luminance of the airglow at the zenith as observed (cd/m^2; 0 when it is not drawn). The light pollution
	//! setting of Stellarium is the whole brightness of the night sky (a Bortle 1 sky is natural): the atmosphere
	//! models draw only the part above this as artificial light, so that the natural sky is not counted twice.
	double getPhotoAirglowZenithLuminance() const;
	//! The extinction of the airglow per airmass in red, green and blue (magnitudes; the green one is the
	//! extinction coefficient)
	Vec3f getPhotoAirglowExtinction() const;
	//! The color of the extinction towards the given direction (normalized geometric alt-azimuthal vector):
	//! linear RGB at unit luminance, so that it changes the color but not the brightness, which comes from
	//! the magnitude with extinction. (1,1,1) when the extinction colors are not active.
	Vec3f getPhotoExtinctionTint(const Vec3d& altAzGeometric) const;
	//! A display (sRGB) color with a linear tint applied, keeping its largest channel (which callers use for dimming).
	static Vec3f photoTintedColor(const Vec3f& srgbColor, const Vec3f& linearTint);
	//! GLSL code: vec3 photoExtinctionTint(float sinAltitude), the tint of getPhotoExtinctionTint() for shaders.
	static QByteArray getPhotoExtinctionShader();
	//! Set the uniforms of getPhotoExtinctionShader(), with the table bound to the given texture unit.
	//! Set the uniforms of the color film's sky (photoFilmSky, photoFilmSkyR/G/B of xyYToRGB.glsl)
	void setPhotoFilmSkyUniforms(class QOpenGLShaderProgram& program) const;
	void setPhotoExtinctionUniforms(class QOpenGLShaderProgram& program, int textureUnit);
	//! GLSL code for the trails and the blur of extended sky textures (Milky Way, zodiacal light): uniform int
	//! photoSkyTrailSamples (1 = neither), highp vec4 photoSkySample(int i) (the window offset of the point the lens
	//! spreads onto the pixel, its weight, and the turn of the sky at the time of the sample), and
	//! highp vec3 photoSkyTurn(highp vec3 dir, highp float angle). The camera stands still during the exposure, so a
	//! pixel keeps its altitude (and extinction) while the sky turns past it. Uniform photoSkyLodBias: texture bias.
	static QByteArray getPhotoSkyTrailShader();
	//! Set the uniforms of getPhotoSkyTrailShader() for a texture drawn in the frame of @a modelView, with
	//! @a texturePixelsPerRad texels per radian (it limits the number of samples needed).
	void setPhotoSkyTrailUniforms(class QOpenGLShaderProgram& program, const StelProjector::ModelViewTranformP& modelView, double texturePixelsPerRad) const;
	//! Show the line with the camera settings at the top of the screen.
	void setFlagPhotoInfoLine(bool b);
	bool getFlagPhotoInfoLine() const {return flagPhotoInfoLine;}
	//! The exposure time in use (seconds): the set one, or the one chosen by the automatic exposure.
	double getPhotoEffectiveExposureTime() const;
	//! The exposure time as a camera shows it: the nearest standard value (1/3-stop steps).
	static double photoNominalExposureTime(double seconds);
	//! Text of an exposure time, e.g. "1/125 s", "2.5 s" or "4 min".
	static QString photoExposureTimeText(double seconds);
	//! A focus distance as on the scale of a lens (see photoFocusDistance)
	static QString photoFocusDistanceText(double meters);
	//! The focal length as a lens is marked, in millimeters: below 10 mm with a tenth (the 4.5 mm of a compact camera)
	static double photoNominalFocalLength(double millimeters);
	//! Automatic exposure: use a reading of the exposure meter. Called by StelApp once per frame.
	//! @param deltaTime time since the last call, seconds (for the smooth adaptation)
	void applyPhotoMeterReading(const StelPhotoMeter::Reading& reading, double deltaTime);
	//! Sky quality meter: use a reading of the area means of the scene (StelPhotoMeter::AreaMeans of the whole
	//! scene buffer), or forget the last one (an empty reading). Called by StelApp a few times per second.
	void applyPhotoSqmReading(const StelPhotoMeter::Reading& reading);
	//! What a sky quality meter (SQM-L, about 20 degrees wide) pointed at the zenith would measure: the luminance
	//! (cd/m^2) of the rendered sky, stars and Milky Way included, weighted by its field; -1 when the zenith is not
	//! in view enough to tell
	double getPhotoSqmLuminance() const {return photoSqmLuminance;}
	//! The darkest the zenith gets at the place: the airglow and the light pollution only (no Moon, twilight,
	//! zodiacal light, Milky Way or stars), cd/m^2; 0 off the Earth or without the atmosphere
	double getPhotoZenithBackgroundLuminance() const;
	//! Draw the line with the camera settings (if switched on). Called by StelApp after the camera
	//! stage, so that the text is neither exposed nor blurred like the scene.
	void drawPhotoInfoLine(StelCore* core) const;
	//! Restore the default camera and star image settings of the photographic mode.
	void resetPhotographicSettings();
	//! Get the factor that converts a scene luminance in cd/m^2 into the linear value of the
	//! scene buffer in the photographic mode, before the exposure compensation.
	//! It follows the standard exposure equation for a reflected-light meter (calibration K = 12.5):
	//! a luminance L that the meter would expose "correctly" for t, N and ISO ends up at
	//! middle grey (0.18), i.e. value = 0.18 * L * t * ISO / (12.5 * N^2). 1.0 means saturation.
	float getPhotoSceneScale() const;
	//! Whether the lens halo is applied to the whole image (photographic mode active, bloom on, halo > 0).
	//! The star images then leave out their halo.
	bool getFlagPhotoBloomActive() const;
	//! Moffat alpha (pixels) and beta of the lens halo
	float getPhotoHaloAlpha() const {return photoCoreAlpha*PHOTO_HALO_WIDTH_FACTOR;}
	static float getPhotoHaloBeta() {return PHOTO_HALO_BETA;}
	//! Fraction of the light that stays in the PSF core (the rest goes into the halo and the spikes)
	float getPhotoCoreFraction() const;
	//! Fraction of the light in the diffraction spikes (0 without spikes). It depends on the aperture:
	//! wide open the diaphragm is round and makes no spikes; its edges become straight when stopping down.
	float getPhotoSpikeFractionEffective() const;
	//! How much the diaphragm is a polygon with straight edges: 0 wide open, 1 two stops down and more
	float getPhotoApertureStraightness() const;
	//! Diffraction spikes: number of lines through a star (spikes/2), direction of the first line on the
	//! screen (radians), length scale (pixels; it grows with the f-number like the diffraction pattern)
	int getPhotoSpikeLines() const {return photoApertureBlades <= 0 ? 0 : ((photoApertureBlades % 2) ? photoApertureBlades : photoApertureBlades/2);}
	static float getPhotoSpikeAngle() {return PHOTO_SPIKE_ANGLE;}
	float getPhotoSpikeLength() const;
	//! Photographic mode: weight of the PSF core for a body whose disk has the given radius in pixels.
	//! 1 for an unresolved body (drawn like a star), falling to 0 as the disk becomes resolved.
	float getPhotoCoreWeight(float diskPixRadius) const;
	//! Photographic mode: the fraction of a body's light that its (resolved) disk keeps: the rest
	//! is spread by the lens into the halo and the diffraction spikes, or drawn as PSF core.
	float getPhotoDiskFraction(float diskPixRadius) const;
	//! Photographic mode: draw the image of a bright body seen through the camera: the PSF core
	//! (weighted with getPhotoCoreWeight()), the lens halo and the diffraction spikes, all spread
	//! around a disk of the given radius. The disk itself is drawn by the caller.
	//! @param mag magnitude of the body including extinction
	//! @param color display color (sRGB) of the body's light
	//! @param illuminatedFraction for a body in phases: its light comes from the lit part, so the image is
	//!        spread around a smaller disk shifted towards the Sun (no glow around the dark limb)
	//! @param sunDirection direction towards the Sun in the sky (J2000, perpendicular to v), used with illuminatedFraction
	//! Draw the halo and the spikes of the bright bodies of this frame (see drawPhotoBody()). StelApp calls
	//! this after all modules, so that the lens effects also cover the landscape.
	void drawDeferredPhotoBodies(StelCore* core);
	bool drawPhotoBody(StelPainter* sPainter, const Vec3d& v, float diskPixRadius, float mag, const Vec3f& color,
	                   float illuminatedFraction = 1.f, const Vec3d& sunDirection = Vec3d(0.));

	//! Informing the drawer whether atmosphere is displayed.
	//! This is used to avoid twinkling/simulate extinction/refraction.
	//! Not required as property!
	void setFlagHasAtmosphere(bool b) {flagHasAtmosphere=b;}
	//! This is used to decide whether to apply refraction/extinction before rendering point sources et al.
	bool getFlagHasAtmosphere() const {return flagHasAtmosphere;}

	//! Set extinction coefficient, mag/airmass (for extinction).
	//! In the photographic mode this is the coefficient at a normal humidity (see photoHumidity): the extinction in
	//! use (getExtinction()) follows the humidity.
	void setExtinctionCoefficient(double extCoeff);
	//! Get extinction coefficient, mag/airmass (for extinction), as set (at a normal humidity).
	double getExtinctionCoefficient() const {return static_cast<double>(extinctionBase);}
	//! Set atmospheric (ground) temperature in deg celsius (for refraction).
	void setAtmosphereTemperature(double celsius);
	//! Get atmospheric (ground) temperature in deg celsius (for refraction).
	double getAtmosphereTemperature() const {return static_cast<double>(refraction.getTemperature());}
	//! Set atmospheric (ground) pressure in mbar (for refraction).
	void setAtmospherePressure(double mbar);
	//! Get atmospheric (ground) pressure in mbar (for refraction).
	double getAtmospherePressure() const {return static_cast<double>(refraction.getPressure());}

	void setPhotoHumidity(double percent);
	double getPhotoHumidity() const {return photoHumidity;}
	void setFlagPhotoAureole(bool b);
	bool getFlagPhotoAureole() const {return flagPhotoAureole;}

	//! Get the current valid extinction computation object.
	const Extinction& getExtinction() const {return extinction;}
	//! Get the current valid refraction computation object.
	const Refraction& getRefraction() const {return refraction;}

	//! Get the radius of the big halo texture used when a 3d model is very bright.
	float getBig3dModelHaloRadius() const {return big3dModelHaloRadius;}
	//! Set the radius of the big halo texture used when a 3d model is very bright.
	void setBig3dModelHaloRadius(float r) {big3dModelHaloRadius=r;}

	// These are to find out the best sky parameters. Program feature for debugging/expert mode.
	// These will be connected from AtmosphereDialog and forward the settings to SkyLight class.

	void setFlagDrawSunAfterAtmosphere(bool val);
	bool getFlagDrawSunAfterAtmosphere() const {return flagDrawSunAfterAtmosphere;}
	void setFlagEarlySunHalo(bool val);
	bool getFlagEarlySunHalo() const {return flagEarlySunHalo;}

	bool getFlagTfromK(void) const {return flagTfromK;}
	void setFlagTfromK(bool val);

	double getT(void) const {return static_cast<double>(turbidity);}
	void setT(double newT);

signals:
	//! Emitted whenever the relative star scale changed
	void relativeStarScaleChanged(double b);
	//! Emitted whenever the absolute star scale changed
	void absoluteStarScaleChanged(double b);
	//! Emitted whenever the twinkle amount changed
	void twinkleAmountChanged(double b);
	//! Emitted whenever the twinkle flag is toggled
	void flagTwinkleChanged(bool b);
	//! Emitted whenever the force twinkle flag is toggled
	void flagForcedTwinkleChanged(bool b);
	//! Emitted whenever light pollution luminance changed
	void lightPollutionLuminanceChanged(double luminance);
	//! Emitted when flag to draw big halo around stars changed
	void flagDrawBigStarHaloChanged(bool b);
	//! Emitted on change of star texture
	void flagStarSpikyChanged(bool b);

	//! Emitted whenever the star magnitude limit flag is toggled
	void flagStarMagnitudeLimitChanged(bool b);
	//! Emitted whenever the nebula magnitude limit flag is toggled
	void flagNebulaMagnitudeLimitChanged(bool b);
	//! Emitted whenever the planet magnitude limit flag is toggled
	void flagPlanetMagnitudeLimitChanged(bool b);

	//! Emitted whenever the star magnitude limit changed
	void customStarMagLimitChanged(double limit);
	//! Emitted whenever the nebula magnitude limit changed
	void customNebulaMagLimitChanged(double limit);
	//! Emitted whenever the planet magnitude limit changed
	void customPlanetMagLimitChanged(double limit);

	//! Emitted whenever the luminance adaptation flag is toggled
	void flagLuminanceAdaptationChanged(bool b);
	//! Emitted when threshold value to draw info text in black is changed
	void daylightLabelThresholdChanged(double t);
	//! Emitted when the photographic rendering mode is switched on or off
	void flagPhotographicModeChanged(bool b);
	//! Emitted when the exposure correction of the photographic mode is changed
	void photoExposureEVChanged(double ev);
	//! Emitted when the exposure time of the virtual camera is changed
	void photoExposureTimeChanged(double seconds);
	//! Emitted when the aperture of the virtual camera is changed
	void photoApertureChanged(double fNumber);
	void photoLensMaxApertureChanged(double fNumber);
	//! Emitted when the ISO sensitivity of the virtual camera is changed
	void photoISOChanged(double iso);
	void photoPsfMinFwhmChanged(double pixels);
	void photoSeeingChanged(double arcsec);
	void photoHaloStrengthChanged(double fraction);
	void photoApertureBladesChanged(int blades);
	void photoSpikeStrengthChanged(double fraction);
	void flagPhotoBloomChanged(bool b);
	void flagPhotoAutoExposureChanged(bool b);
	void flagPhotoProgramExposureChanged(bool b);
	void flagPhotoProgramApertureLockChanged(bool b);
	void photoProgramMaxISOChanged(double iso);
	void photoProgramMaxTimeChanged(double seconds);
	void photoMeteringModeChanged(int mode);
	void flagPhotoInfoLineChanged(bool b);
	void photoToneCurveChanged(int curve);
	void flagPhotoExtinctionColorChanged(bool b);
	void flagPhotoAirglowChanged(bool b);
	void photoAirglowStrengthChanged(double strength);
	void photoHumidityChanged(double percent);
	void flagPhotoAureoleChanged(bool b);
	void photoWhiteBalanceChanged(int wb);
	void photoWhiteBalanceKelvinChanged(double kelvin);
	void flagPhotoVignettingChanged(bool b);
	void flagPhotoHalphaModifiedChanged(bool b);
	void flagPhotoAberrationsChanged(bool b);
	void flagPhotoLensFlareChanged(bool b);
	void photoLensFlareStrengthChanged(double s);
	void photoFocusDistanceChanged(double meters);
	void flagPhotoStarTrailsChanged(bool b);
	void photoMountChanged(int mount);
	void photoNoiseChanged(int mode);
	void photoSensorFormatChanged(int format);
	void photoFilmChanged(int film);
	void photoFilmPushChanged(int stops);
	void flagPhotoFilmSpectralSkyChanged(bool b);
	void photoZoomStepsChanged(int steps);
	void flagPhotoPortraitChanged(bool b);
	void flagPhotoFrameMaskChanged(bool b);
	void photoEclipseAtmosphereChanged(int state);
	void photoAberrationStrengthChanged(double s);
	void photoDetectionSnrChanged(double snr);
	void photoEffectiveExposureTimeChanged(double seconds);

	void extinctionCoefficientChanged(double coeff);
	void atmosphereTemperatureChanged(double celsius);
	void atmospherePressureChanged(double mbar);

	void flagTfromKChanged(bool b);
	void flagDrawSunAfterAtmosphereChanged(bool b);
	void flagEarlySunHaloChanged(bool b);
	void turbidityChanged(double t);

private:
	// Debug
	float reverseComputeRCMag(float rmag) const;

	//! Compute the current limit magnitude by dichotomy
	float computeLimitMagnitude() const;

	//! Compute the current limit luminance by dichotomy
	float computeLimitLuminance() const;

	//! Get StelSkyDrawer maximum FOV.
	float getMaxAdaptFov(void) const {return maxAdaptFov;}
	//! Set StelSkyDrawer maximum FOV.
	//! Usually stars/planet halos are drawn fainter when FOV gets larger,
	//! but when FOV gets larger than this value, the stars do not become
	//! fainter any more. Must be >= 60.0.
	void setMaxAdaptFov(float fov) {maxAdaptFov = (fov < 60.f) ? 60.f : fov;}

	//! Get StelSkyDrawer minimum FOV.
	float getMinAdaptFov(void) const {return minAdaptFov;}
	//! Set StelSkyDrawer minimum FOV.
	//! Usually stars/planet halos are drawn brighter when FOV gets smaller.
	//! But when FOV gets smaller than this value, the stars do not become
	//! brighter any more. Must be <= 60.0.
	void setMinAdaptFov(float fov) {minAdaptFov = (fov > 60.f) ? 60.f : fov;}

	//! Set the scaling applied to input luminance before they are converted by the StelToneReproducer
	void setInputScale(float in) {inScale = in;}
	//! Get the scaling applied to input luminance before they are converted by the StelToneReproducer
	float getInputScale() const {return inScale;}

	//! Compute the luminance for a point source with the given mag for the current FOV
	//! @param mag V magnitude of the point source
	//! @return the luminance in log(cd/m^2)
	inline float pointSourceMagToLuminance(float mag) const {return std::exp(pointSourceMagToLnLuminance(mag));}

	//! Compute the V magnitude for a point source with the given luminance for the current FOV
	//! @param lum the luminance in cd/m^2
	//! @return V magnitude of the point source
	float pointSourceLuminanceToMag(float lum) const;

	//! Compute the log of the luminance for a point source with the given mag for the current FOV
	//! @param mag V magnitude of the point source
	//! @return the luminance in cd/m^2
	float pointSourceMagToLnLuminance(float mag) const;

	//! Find the world adaptation luminance to use so that a point source of magnitude mag
	//! is displayed with a halo of size targetRadius
	float findWorldLumForMag(float mag, float targetRadius) const;

	//! Binds actual VAO if it's supported, sets up the relevant state manually otherwise.
	void bindVAO();
	//! Sets the vertex attribute states for the currently bound VAO so that glDraw* commands can work.
	void setupCurrentVAO();
	//! Binds zero VAO if VAO is supported, manually disables the relevant vertex attributes otherwise.
	void releaseVAO();

	StelCore* core;
	StelToneReproducer* eye;

	std::unique_ptr<QOpenGLVertexArrayObject> vao;
	std::unique_ptr<QOpenGLBuffer> vbo;

	Extinction extinction;
	Refraction refraction;

	float maxAdaptFov, minAdaptFov, lnfovFactor;
	bool flagStarTwinkle;      //! flickering stars due to atmospheric turbulence
	bool flagForcedTwinkle;    //! force flickering even if atmosphere is off
	double twinkleAmount;      //! magnitude range of atmospheric flicker at horizon
	bool flagDrawBigStarHalo;
	bool flagStarSpiky;

	//! Informing the drawer whether atmosphere is displayed.
	//! This is used to avoid twinkling/simulate extinction/refraction.
	bool flagHasAtmosphere;

	//! Controls the application of the user-defined star magnitude limit.
	//! @see customStarMagnitudeLimit
	bool flagStarMagnitudeLimit;
	//! Controls the application of the user-defined nebula magnitude limit.
	//! @see customNebulaMagnitudeLimit
	bool flagNebulaMagnitudeLimit;
	//! Controls the application of the user-defined planet magnitude limit.
	//! @see customPlanetMagnitudeLimit
	bool flagPlanetMagnitudeLimit;

	double starRelativeScale;
	double starAbsoluteScaleF;

	float starLinearScale;	// optimization variable

	//! Current magnitude limit for point sources
	float limitMagnitude;

	//! Current magnitude luminance
	float limitLuminance;

	//! User-defined magnitude limit for stars.
	//! Interpreted as a lower limit - stars fainter than this value will not be displayed.
	//! Used if flagStarMagnitudeLimit is true.
	double customStarMagLimit;
	//! User-defined magnitude limit for deep-sky objects.
	//! Interpreted as a lower limit - nebulae fainter than this value will not be displayed.
	//! Used if flagNebulaMagnitudeLimit is true.
	double customNebulaMagLimit;
	//! User-defined magnitude limit for solar system objects.
	//! Interpreted as a lower limit - planets fainter than this value will not be displayed.
	//! Used if flagPlanetMagnitudeLimit is true.
	double customPlanetMagLimit;

	//! Little halo texture
	QImage texImgHalo;
	QImage texImgHaloSpiky;
	StelTextureSP texHalo;
	StelTextureSP texHaloRayed;

	//! Load B-V conversion parameters from config file
	void initColorTableFromConfigFile(class QSettings* conf);

	//! Contains the list of colors matching a given B-V index
	static Vec3f colorTable[128];

	//! The current light pollution luminance
	double lightPollutionLuminance;

	//! The scaling applied to input luminance before they are converted by the StelToneReproducer
	float inScale;

	// Variables used for GL optimization when displaying point sources
	//! Vertex format for a point source.
	//! Texture pos is stored in another separately.
	struct StarVertex {
		Vec2f pos;
		unsigned char color[4];
	};
	static_assert(sizeof(StarVertex) == 12, "Size of StarVertex must be 12 bytes");
	
	//! Buffer for storing the vertex array data
	StarVertex* vertexArray;

	//! Buffer for storing the texture coordinate array data.
	unsigned char* textureCoordArray;
	
	class QOpenGLShaderProgram* starShaderProgram;
	struct StarShaderVars {
		int projectionMatrix;
		int texCoord;
		int pos;
		int color;
		int texture;
	};
	StarShaderVars starShaderVars;
	
	//! Current number of sources stored in the buffers (still to display)
	unsigned int nbPointSources;
	//! Maximum number of sources which can be stored in the buffers
	constexpr static unsigned int maxPointSources=1000;

	// ---- Point sources in the photographic mode ----
	// A star is drawn as the point spread function (PSF) of a camera: a Moffat profile with a
	// fixed width (set by the sensor and the seeing), plus a faint, wide lens halo. Its integral
	// is the total light the sensor records from the star, so brightness changes intensity,
	// not size. Bright stars only look bigger because their wings and halo rise above the noise
	// floor and their cores saturate in the final camera pass.

	//! One point source in the photographic mode (drawn with instancing: one record per star,
	//! the sprite corners come from a small shared buffer). All in floats, linear light.
	struct PhotoStarInstance {
		Vec2f center;   //!< window position of the star
		Vec3f color;    //!< linear color times total recorded light (sum over all pixels)
		float radius;   //!< sprite radius in pixels (the profile fades out smoothly towards it)
		Vec4f shape;    //!< x: disk radius (0 for a star), y: weight of the PSF core, z: radius of a disk with the area of the lit part, w: cos(phase angle)
		Vec2f sunDir;   //!< direction towards the Sun on the screen (bodies in phases)
		Vec4f trail;    //!< star trail: offsets (pixels) of the image at the start (xy) and the end (zw) of the exposure from its position at mid-exposure
		Vec3f trailStart; //!< star trail: light at the start of the exposure relative to mid-exposure (extinction: brightness and color), per channel
		Vec3f trailEnd;   //!< the same at the end of the exposure
	};
	static_assert(sizeof(PhotoStarInstance) == 88, "PhotoStarInstance must be tightly packed");
	//! Point sources per batch in the photographic mode. Far more stars are visible on a photo
	//! than to the eye, so batches are large to keep the number of draw calls low.
	constexpr static unsigned int maxPhotoPointSources=16384;
	std::vector<PhotoStarInstance> photoInstances;
	//! The B-V color table in linear light, normalized to unit luminance
	static Vec3f photoColorTable[128];
	//! Draw a point source in the photographic mode.
	//! @param linearUnitColor linear color with unit luminance
	//! @param dim extra linear factor on the light (e.g. a fading object)
	bool drawPhotoPointSource(StelPainter* sPainter, const Vec3d& v, const RCMag& rcMag, const Vec3f& linearUnitColor, float dim, bool checkInScreen);
	unsigned int nbPhotoPointSources = 0;
	std::unique_ptr<class QOpenGLShaderProgram> photoStarShaderProgram;
	std::unique_ptr<QOpenGLVertexArrayObject> photoVao;
	std::unique_ptr<QOpenGLBuffer> photoVbo;       //!< per-star records, refilled for every batch
	std::unique_ptr<QOpenGLBuffer> photoCornerVbo; //!< the 6 corners of a sprite (two triangles)
	struct PhotoStarShaderVars {
		int projectionMatrix;
		int corner;
		int center;
		int color;
		int radius;
		int shape;
		int sunDir;
		int trail;
		int trailStart;
		int trailEnd;
		int coreAlpha;
		int coreBeta;
		int haloAlpha;
		int haloBeta;
		int haloFraction;
		int coreFraction;
		int coreDiskTerm;
		int haloDiskTerm;
		int spikeLines;
		int spikeFraction;
		int spikeSigma;
		int spikeLength;
		int spikeDirs;
		int visibleThreshold;
		int spikeStart;
		int lensCenter;
		int lensCorner;
		int comaLength;
		int astigLength;
		int defocusRadius;
		int defocusArea;
		int defocusSign;
		int defocusSA;
		int defocusEdge;
		int coreFwhm;
		int apertureBlades;
		int apertureStraightness;
		int apertureRotation;
		int lensPupil;
		int lensShift;
		int lensVignetting;
	} photoStarShaderVars = {};
	void initPhotoStarRendering();
	//! What a batch of the photographic point sources draws: everything, only the PSF core
	//! (bright bodies, in their place among the other objects) or only the halo and the spikes
	//! (bright bodies, after the landscape: they arise in the lens and lie over everything)
	enum PhotoBatchMode { PhotoBatchAll, PhotoBatchCoreOnly, PhotoBatchWingsOnly };
	void flushPhotoPointSources(StelPainter* sPainter, PhotoBatchMode mode = PhotoBatchAll);
	//! Halo and spikes of bright bodies, drawn by drawDeferredPhotoBodies(), with the position (J2000) of the body
	struct PhotoDeferredBody
	{
		PhotoStarInstance instance;
		Vec3d posJ2000;
		Vec3d motion; //!< apparent motion against the stars (see setPhotoBodyMotion())
	};
	//! Apparent motion of the body being drawn (see setPhotoBodyMotion())
	Vec3d photoBodyMotion = Vec3d(0.);
	std::vector<PhotoDeferredBody> photoDeferredBodies;

	//! Precomputed each frame in update(): total recorded light (linear units) of a magnitude-0 star
	float photoFluxMag0 = 0.f;
	//! Precomputed each frame in update(): Moffat parameter alpha of the core, in pixels
	float photoCoreAlpha = 1.f;
	//! Precomputed each frame in update(): the faintest pixel value that can still show on screen
	float photoVisibleThreshold = 1e-4f;
	//! Estimated linear value of the sky background in the scene buffer (used for the visibility threshold)
	float photoSkyBackground = 0.f;
	//! Precomputed each frame in update(): the noise of the sky background in a pixel of the picture (scene units,
	//! like the light of a star): photon noise of the sky, read noise and dark signal of its photosites. A star
	//! counts as recorded (and is drawn) only above it, see computePhotoRCMag().
	float photoNoiseSigma = 0.f;
	//! Signal-to-noise ratio (over the image of the star, a matched filter) from which a star shows in the noise
	//! (config: viewing/photographic_detection_snr; 0 = no limit by the noise)
	float photoDetectionSnr = 3.f;
	//! Smallest PSF width (FWHM) in pixels, given by the sensor and the optics
	float photoMinFwhmPixels = 2.f;
	//! Seeing (FWHM) in arc seconds; it matters only when zoomed in
	float photoSeeingArcsec = 3.f;
	//! Moffat beta of the core (larger = closer to a Gaussian)
	static constexpr float PHOTO_CORE_BETA = 3.f;
	//! The lens halo: fraction of the light, width relative to the core, and Moffat beta
	float photoHaloFraction = 0.05f;
	//! The halo of the lens with a digital sensor and with a film (see setPhotoHaloStrength())
	float photoDigitalHaloFraction = 0.05f;
	float photoFilmHaloFraction = 0.003f;
	bool flagPhotoBloom = true;
	static constexpr float PHOTO_HALO_WIDTH_FACTOR = 4.f;
	//! Diffraction spikes: blades of the diaphragm (0 = round), fraction of the light in the spikes (stopped down)
	int photoApertureBlades = 6;
	float photoSpikeFraction = 0.02f;
	//! Spike profile: width (sigma) and core length of the 1/r^2 falloff, relative to the core alpha.
	//! The core length is for f/8 (PHOTO_SPIKE_REFERENCE_APERTURE) and scales with the f-number.
	static constexpr float PHOTO_SPIKE_WIDTH_FACTOR = 0.5f;
	static constexpr float PHOTO_SPIKE_LENGTH_FACTOR = 2.f;
	static constexpr float PHOTO_SPIKE_REFERENCE_APERTURE = 8.f;
	//! Orientation of the spikes on the sensor (one spike points straight up)
	static constexpr float PHOTO_SPIKE_ANGLE = static_cast<float>(M_PI/2.);
	static constexpr float PHOTO_HALO_BETA = 1.5f;
	//! Largest sprite radius in pixels (only the Moon or Venus at very long exposures get near it)
	static constexpr float PHOTO_MAX_SPRITE_RADIUS = 600.f;
	//! Compute flux and sprite size of a point source in the photographic mode
	bool computePhotoRCMag(float mag, RCMag* rcMag) const;
	//! Compute radius and luminance of a point source with the eye model (the classic rendering)
	bool computeEyeRCMag(float mag, RCMag* rcMag) const;

	//! The maximum transformed luminance to apply at the next update
	float maxLum;
	//! The previously used world luminance
	float oldLum;

	//! Big halo texture
	StelTextureSP texBigHalo;
	StelTextureSP texSunHalo;
	StelTextureSP texSunCorona;
	StelVertexArray coronaMesh; // area textured with texSunCorona
	const int coronaMeshDim;
	QVector<Vec2f> coronaTextureCoords;

	//! Simulate the eye's luminance adaptation?
	bool flagLuminanceAdaptation;
	//! a customizable value in cd/m^2 which is used to decide when to render the InfoText in black (when sky is brighter than this)
	double daylightLabelThreshold;

	float big3dModelHaloRadius;
	bool flagDrawSunAfterAtmosphere; // Overdraw to avoid color mixing. Useful to allow deep-red suns, but may also look ugly.
	bool flagEarlySunHalo; // Used to select if solar halo is plotted before (true) or after (default, false) the 3D sphere.
	bool flagTfromK; // true to compute T from extinction k
	float turbidity; // atmospheric turbidity: Preetham sky usable for 2<T<6, best at T=5.

	//! Render the sky like a camera (linear sensor, fixed exposure) instead of like the eye?
	bool flagPhotographicMode = false;
	//! Exposure correction of the photographic mode, in stops (EV)
	double photoExposureEV = 0.;
	//! Camera settings of the photographic mode: exposure time [s], f-number, ISO
	double photoExposureTime = 20.;
	double photoAperture = 2.8;
	double photoLensMaxAperture = PHOTO_LENS_MAX_APERTURE;
	double photoISO = 1600.;
	//! Automatic exposure (aperture priority)
	bool flagPhotoAutoExposure = false;
	bool flagPhotoProgramExposure = false;
	bool flagPhotoProgramApertureLock = false;
	//! Film halation: the density of the antihalation layer (negative: the film's own, see getPhotoHalationParams())
	float photoAntihalationDensity = -1.f;
	//! Push processing of the film (stops), and the film's data with it
	int photoFilmPush = 0;
	//! The data of the film in use, with the push processing applied (see updatePhotoActiveFilmSpec())
	PhotoFilmSpec photoActiveFilmSpec = {};
	//! Recompute photoActiveFilmSpec: whenever the film or its push processing changes
	void updatePhotoActiveFilmSpec();
	//! Film: the reciprocity failure per point after its brightness (else the same for the whole picture)
	bool flagPhotoReciprocityPerPoint = true;
	//! Film: the sky from its spectrum (X, Y, Z, W), else from its color (rgbToFilm)
	bool flagPhotoFilmSpectralSky = false;
	//! Program mode: the highest ISO and the longest exposure time it may use
	double photoProgramMaxISO = PHOTO_PROGRAM_MAX_ISO;
	double photoProgramMaxTime = PHOTO_AUTO_MAX_TIME;
	//! The aperture and the ISO set by the user: the program mode changes the ones in use, and gives these back
	double photoUserAperture = 4.;
	double photoUserISO = 400.;
	bool photoProgramRunning = false;
	//! Program mode: exposure (time x ISO / N^2) wanted by the meter, smoothed; and the settings it leads to
	double photoProgramExposure = 0.;
	void applyPhotoProgram(double exposure);
	void startPhotoProgram();
	void stopPhotoProgram();
	int photoMeteringMode = 0;
	bool flagPhotoInfoLine = true;
	//! Tone curve of the camera
	int photoToneCurve = PhotoToneStandard;
	PhotoToneParams photoToneParams;
	//! The inverse of the tone curve, tabulated for the annotations: scene values at display values i/(size-1)
	std::vector<float> photoToneInverseTable;
	void updatePhotoToneCurve();
	//! Extinction colors: the transmission table, and the tint as a function of the sine of the altitude
	//! (photoExtinctionTints[i] at sin(alt) = sinMin + (1-sinMin) (i/(N-1))^2, dense near the horizon)
	bool flagPhotoExtinctionColor = true;
	bool flagPhotoAirglow = true;
	float photoAirglowStrength = 1.f;
	//! See photoHumidity
	double photoHumidity = PHOTO_NORMAL_HUMIDITY;
	bool flagPhotoAureole = true;
	//! The extinction coefficient as set (the one in use, in extinction, follows the humidity)
	float extinctionBase = 0.13f;
	PhotoAerosol photoAerosol;
	double photoHazeLightFactor = 1.;
	//! The extinction in use and the aerosols from the coefficient as set and the humidity (each frame)
	void updatePhotoHumidity();
	double photoHumidityInputs[4] = {-1., -1., -1., -1.};  //!< what the last update was for
	bool photoExtinctionActive = false;
	StelAtmosphereTransmission atmosphereTransmission;
	std::vector<Vec3f> photoExtinctionTints;
	float photoExtinctionSinMin = 0.f;
	double photoExtinctionAltitude = -1e9;
	double photoExtinctionCoefficient = -1.;
	double photoExtinctionInUse = -1.;
	unsigned int photoExtinctionTexture = 0;
	bool photoExtinctionTextureDirty = true;
	void updatePhotoExtinction();
	//! White balance
	int photoWhiteBalance = PhotoWBDaylight;
	double photoWhiteBalanceKelvin = 5200.;
	double photoAutoWhiteBalanceKelvin = 5200.; //!< current choice of the automatic white balance
	bool photoAutoWhiteBalanceFresh = true;
	double photoWBMatrixKelvin = -1.;           //!< the temperature the matrices were made for
	float photoWBMatrix[9] = {1,0,0, 0,1,0, 0,0,1};    //!< row-major
	float photoWBInverse[9] = {1,0,0, 0,1,0, 0,0,1};   //!< row-major
	void updatePhotoWhiteBalanceMatrix();
	bool flagPhotoHalphaModified = false;
	//! Lens model
	bool flagPhotoVignetting = true;
	bool flagPhotoAberrations = true;
	float photoAberrationStrength = 1.f;
	//! Lens reflections
	bool flagPhotoLensFlare = true;
	float photoLensFlareStrength = 1.f;
	//! Focus distance (meters; 0 = infinity, negative = beyond infinity)
	double photoFocusDistance = 0.;
	//! Star trails
	bool flagPhotoStarTrails = true;
	//! Mount of the camera
	int photoMount = PhotoMountTripod;
	//! Precomputed each frame in update(): the camera follows the sky and the ground moves far enough to show
	bool photoGroundTrailActive = false;
	//! Offscreen image of the landscape (see beginPhotoGroundImage())
	std::unique_ptr<class QOpenGLFramebufferObject> photoGroundFbo;
	std::unique_ptr<class QOpenGLShaderProgram> photoGroundProgram;
	StelProjectorP photoGroundProjector; //!< the projector the composite program was made for
	struct PhotoGroundImage
	{
		bool active = false;
		int previousFbo = 0;
		int viewport[4] = {0, 0, 0, 0};
		float clearColor[4] = {0.f, 0.f, 0.f, 0.f};
	} photoGroundImage;
	//! Sensor noise
	int photoNoise = PhotoNoiseImage;
	//! Sensor format
	int photoSensorFormat = PhotoSensorFullFrame;
	int photoFilm = PhotoFilmNone;
	double photoDigitalISO = 1600.;            //!< the ISO of the digital sensor, kept while a film is in use
	mutable float photoWBMatrixFilm[9] = {1,0,0, 0,1,0, 0,0,1};
	//! Steps of the mouse wheel zoom (PhotoZoomSteps)
	int photoZoomSteps = PhotoZoomFree;
	//! Frame of the sensor: portrait orientation, and the dimming outside it
	bool flagPhotoPortrait = false;
	bool flagPhotoFrameMask = true;
	//! State of the Earth's atmosphere for lunar eclipses (LunarEclipseLight::Atmosphere)
	int photoEclipseAtmosphere = 1;
	//! Measuring the mean of textures (see photoTextureMeanLuminance())
	std::unique_ptr<class QOpenGLShaderProgram> photoTextureMeanProgram;
	std::unique_ptr<class QOpenGLFramebufferObject> photoTextureMeanFbo;
	std::unique_ptr<QOpenGLVertexArrayObject> photoTextureMeanVao;
	bool photoTextureMeanTried = false;
	//! Offscreen image of a body (see beginPhotoBodyImage())
	std::unique_ptr<class QOpenGLFramebufferObject> photoBodyFbo;
	std::unique_ptr<class QOpenGLShaderProgram> photoBodyProgram;
	std::unique_ptr<QOpenGLVertexArrayObject> photoBodyVao;
	bool photoBodyTried = false;
	struct PhotoBodyImage
	{
		bool active = false;
		int x0 = 0, y0 = 0, w = 0, h = 0;  //!< the image: window pixels
		float margin = 0.f;                 //!< how far the blur and the trail reach beyond it (pixels)
		Vec4f trail;                        //!< offsets at the start and the end of the exposure (pixels)
		Vec3f logStart, logEnd;             //!< log of the extinction along the trail
		float defocus = 0.f;                //!< signed radius of the blur disk (pixels)
		bool glow = false;                  //!< light of the lens: added, not covering (see beginPhotoBodyImage())
		int previousFbo = 0;
		int viewport[4] = {0, 0, 0, 0};
		bool scissor = false;
		float clearColor[4] = {0.f, 0.f, 0.f, 0.f};
	} photoBodyImage;
	unsigned int photoNoiseFrame = 0;
	//! Precomputed each frame in update(): star trails are drawn (long enough to show)
	bool photoTrailsActive = false;
	//! Precomputed each frame in update(): angle the sky turns during the exposure (radians), the axis of the
	//! diurnal rotation in view (eye) coordinates, and the rotations from mid-exposure to its start and end
	double photoTrailAngle = 0.;
	Vec3d photoTrailAxisView;
	Mat4d photoTrailToStart;
	Mat4d photoTrailToEnd;
	//! Precomputed each frame in update(): view (eye) coordinates -> geometric altitude/azimuth, for the extinction along the trails
	StelProjector::ModelViewTranformP photoTrailAltAzTransform;
	bool photoTrailExtinction = false;
	//! The light of a source in the given direction (view coordinates) after the extinction, relative to its light
	//! in the direction viewRef: brightness and color (photographic extinction color), per channel
	Vec3f photoTrailExtinctionFactor(const Vec3d& view, const Vec3d& viewRef) const;
	//! Star trail of a point source at v (frame of the projector) whose image lies at win: the offsets of the image
	//! at the start and the end of the exposure (see PhotoStarInstance::trail). Returns its length (pixels), 0 = none.
	//! A source whose trailed image cannot show (its light is spread too thinly) gives -1.
	//! @param flux total recorded light of the source, @param diskRadius radius of its disk (pixels; a trail
	//! shorter than the disk is not drawn)
	float computePhotoTrail(const StelProjector& prj, const Vec3d& v, const Vec3d& win, float flux, float diskRadius, PhotoStarInstance& instance) const;
	//! Area of the image of the aperture (disk or polygon of the blades) in units of its radius squared
	float getPhotoApertureShapeArea() const;
	//! Visibility and size of the core of a defocused point source (or of a small body) of the given core flux:
	//! the radius of its blur disk is R (pixels). Returns the sprite radius needed, 0 if it cannot show.
	float photoDefocusCoreRadius(float coreFlux, float R) const;
	//! FWHM of the PSF core (pixels)
	float photoCoreFwhm() const {return 2.f*photoCoreAlpha*std::sqrt(std::pow(2.f, 1.f/PHOTO_CORE_BETA) - 1.f);}
	//! Half width of the blurred edge of a defocused star image (pixels)
	float photoDefocusEdge() const {return qMax(0.5f, 0.6f*photoCoreFwhm());}
	struct PhotoGhostSource { Vec3d v; float mag; Vec3f color; };
	std::vector<PhotoGhostSource> photoGhostSources;
	std::unique_ptr<class StelPhotoLensFlare> photoLensFlare;
	//! The lens of the current frame (window coordinates), for the sprite sizes
	PhotoLens photoLensCache;
	bool photoLensAberrations = false;
	//! Exposure time chosen by the automatic exposure (seconds)
	double photoAutoExposureTime = 1./60.;
	//! The automatic exposure jumps to the metered value at its first reading
	bool photoAutoExposureFresh = true;
	//! The metered exposure is outside the range of the camera: -1 too dark (longest time), +1 too bright
	int photoAutoExposureLimit = 0;
	//! See getPhotoSqmLuminance()
	double photoSqmLuminance = -1.;
	//! Last value announced with photoEffectiveExposureTimeChanged (nominal value)
	double photoAnnouncedExposureTime = 0.;
	void announcePhotoExposureTime();
	//! ID of the on-screen message that shows the current exposure (0 = none)
	int photoExposureMessageID = 0;
	//! Show the current exposure briefly on screen
	void showPhotoExposureMessage();
};

#endif // STELSKYDRAWER_HPP
