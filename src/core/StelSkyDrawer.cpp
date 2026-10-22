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


#include "StelSkyDrawer.hpp"
#include "StelPhotoBloom.hpp"
#include "StelPhotoLensFlare.hpp"
#include "StelProjector.hpp"
#include "StelFileMgr.hpp"

#include "StelToneReproducer.hpp"
#include "StelTextureMgr.hpp"
#include "StelApp.hpp"
#include "StelSRGB.hpp"
#include "StelCore.hpp"
#include "StelUtils.hpp"
#include "StelMovementMgr.hpp"
#include "StelPainter.hpp"
#include "StelMainView.hpp"
#include "StelTranslator.hpp"
#include "precession.h"

#include "StelModuleMgr.hpp"
#include "LandscapeMgr.hpp"
#include "LabelMgr.hpp"
#include "Landscape.hpp"
#include "StelPropertyMgr.hpp"
#include "StelObserver.hpp"
#include "Planet.hpp"

#include <QOpenGLContext>
#include <QOpenGLVertexArrayObject>
#include <QOpenGLShaderProgram>
#include <QOpenGLFramebufferObject>
#include <QOpenGLExtraFunctions>
#include <QVector2D>
#include <QOpenGLBuffer>
#include <QStringList>
#include <QSettings>
#include <QDebug>
#include <QtGlobal>
#include <QGuiApplication>
#include <QFontMetrics>
#include <QLocale>
#include <QDir>
#include <array>

#ifndef GL_RGBA16F
#define GL_RGBA16F 0x881A
#endif

// The 0.025 corresponds to the maximum eye resolution in degree
#define EYE_RESOLUTION (0.25f)
#define MAX_LINEAR_RADIUS 8.f

namespace
{
//! Encode a linear value (0..1) with the sRGB transfer function
float encodeSRGB(float c)
{
	return c <= 0.0031308f ? 12.92f*c : 1.055f*std::pow(c, 1.f/2.4f) - 0.055f;
}
}

StelSkyDrawer::StelSkyDrawer(StelCore* acore) :
	core(acore),
	eye(acore->getToneReproducer()),
	vao(new QOpenGLVertexArrayObject),
	vbo(new QOpenGLBuffer(QOpenGLBuffer::VertexBuffer)),
	maxAdaptFov(180.f),
	minAdaptFov(0.1f),
	lnfovFactor(0.f),
	flagStarTwinkle(false),
	flagForcedTwinkle(false),
	twinkleAmount(0.0),
	flagDrawBigStarHalo(true),
	flagStarSpiky(false),
	flagStarMagnitudeLimit(false),
	flagNebulaMagnitudeLimit(false),
	flagPlanetMagnitudeLimit(false),
	starRelativeScale(1.),
	starAbsoluteScaleF(1.),
	starLinearScale(19.569f),
	limitMagnitude(-100.f),
	limitLuminance(0.f),
	customStarMagLimit(0.0),
	customNebulaMagLimit(0.0),
	customPlanetMagLimit(0.0),
	inScale(1.f),
	starShaderProgram(Q_NULLPTR),
	starShaderVars(StarShaderVars()),
	nbPointSources(0),
	maxLum(0.f),
	oldLum(-1.f),
	coronaMeshDim(5), // low odd number: 3/5/7
	coronaTextureCoords(),
	flagLuminanceAdaptation(false),
	daylightLabelThreshold(250.0),
	big3dModelHaloRadius(150.f)
{
	setObjectName("StelSkyDrawer");
	QSettings* conf = StelApp::getInstance().getSettings();
	initColorTableFromConfigFile(conf);

	setFlagHasAtmosphere(conf->value("landscape/flag_atmosphere", true).toBool());
	setTwinkleAmount(conf->value("stars/star_twinkle_amount",0.3).toDouble());
	setFlagTwinkle(conf->value("stars/flag_star_twinkle",true).toBool());
	setFlagForcedTwinkle(conf->value("stars/flag_forced_twinkle",false).toBool());
	setFlagDrawBigStarHalo(conf->value("stars/flag_star_halo",true).toBool());
	flagStarSpiky=(conf->value("stars/flag_star_spiky", false).toBool()); // too early to use the set method here!
	setMaxAdaptFov(conf->value("stars/mag_converter_max_fov",70.0).toFloat());
	setMinAdaptFov(conf->value("stars/mag_converter_min_fov",0.1).toFloat());
	setFlagLuminanceAdaptation(conf->value("viewing/use_luminance_adaptation",true).toBool());
	setDaylightLabelThreshold(conf->value("viewing/sky_brightness_label_threshold", 250.0).toDouble());
	flagPhotographicMode = conf->value("viewing/flag_photographic_mode", false).toBool();
	photoExposureEV = qBound(-12., conf->value("viewing/photographic_exposure_ev", 0.0).toDouble(), 12.);
	photoExposureTime = qBound(1./8000., conf->value("viewing/photographic_exposure_time", 20.0).toDouble(), 3600.);
	photoAperture = qBound(0.7, conf->value("viewing/photographic_aperture", 2.8).toDouble(), 64.);
	photoISO = qBound(25., conf->value("viewing/photographic_iso", 1600.0).toDouble(), 409600.);
	photoMinFwhmPixels = qBound(0.8f, conf->value("viewing/photographic_psf_min_fwhm", 2.0).toFloat(), 10.f);
	photoSeeingArcsec = qBound(0.f, conf->value("viewing/photographic_seeing", 3.0).toFloat(), 60.f);
	photoDigitalHaloFraction = qBound(0.f, conf->value("viewing/photographic_halo_strength", 0.05).toFloat(), 0.3f);
	photoFilmHaloFraction = qBound(0.f, conf->value("viewing/photographic_film_halo_strength", static_cast<double>(PHOTO_FILM_HALO_DEFAULT)).toFloat(), 0.3f);
	photoHaloFraction = photoDigitalHaloFraction;
	// Diaphragm blades; older settings stored the number of spikes, which is the same for even numbers
	photoApertureBlades = conf->value("viewing/photographic_aperture_blades", conf->value("viewing/photographic_spike_count", 6)).toInt();
	photoApertureBlades = (photoApertureBlades<=0) ? 0 : qBound(4, photoApertureBlades, 9);
	photoSpikeFraction = qBound(0.f, conf->value("viewing/photographic_spike_strength", 0.02).toFloat(), 0.1f);
	flagPhotoBloom = conf->value("viewing/flag_photographic_bloom", true).toBool();
	photoAntihalationDensity = conf->value("astro/photographic_antihalation_density", -1.0).toFloat();
	flagPhotoReciprocityPerPoint = conf->value("astro/flag_photographic_reciprocity_per_point", true).toBool();
	flagPhotoFilmSpectralSky = conf->value("astro/flag_photographic_film_spectral_sky", false).toBool();
	flagPhotoAutoExposure = conf->value("viewing/flag_photographic_auto_exposure", false).toBool();
	photoMeteringMode = qBound(0, conf->value("viewing/photographic_metering_mode", 0).toInt(), 2);
	flagPhotoInfoLine = conf->value("viewing/flag_photographic_info_line", true).toBool();
	photoToneCurve = qBound(0, conf->value("viewing/photographic_tone_curve", static_cast<int>(PhotoToneStandard)).toInt(), 2);
	updatePhotoToneCurve();
	flagPhotoExtinctionColor = conf->value("viewing/flag_photographic_extinction_color", true).toBool();
	flagPhotoAirglow = conf->value("viewing/flag_photographic_airglow", true).toBool();
	photoAirglowStrength = qBound(0.25f, conf->value("viewing/photographic_airglow_strength", 1.0).toFloat(), 2.f);
	photoWhiteBalance = qBound(0, conf->value("viewing/photographic_white_balance", static_cast<int>(PhotoWBDaylight)).toInt(), 8);
	photoWhiteBalanceKelvin = qBound(2000., conf->value("viewing/photographic_white_balance_kelvin", 5200.).toDouble(), 12000.);
	flagPhotoVignetting = conf->value("viewing/flag_photographic_vignetting", true).toBool();
	flagPhotoAberrations = conf->value("viewing/flag_photographic_aberrations", true).toBool();
	photoAberrationStrength = qBound(0.f, conf->value("viewing/photographic_aberration_strength", 1.0).toFloat(), 3.f);
	flagPhotoLensFlare = conf->value("viewing/flag_photographic_lens_flare", true).toBool();
	photoLensFlareStrength = qBound(0.f, conf->value("viewing/photographic_lens_flare_strength", 1.0).toFloat(), 3.f);
	setPhotoFocusDistance(conf->value("viewing/photographic_focus_distance", 0.0).toDouble());
	flagPhotoStarTrails = conf->value("viewing/flag_photographic_star_trails", true).toBool();
	photoDetectionSnr = qBound(0.f, conf->value("viewing/photographic_detection_snr", 3.0).toFloat(), 20.f);
	flagPhotoProgramExposure = conf->value("viewing/flag_photographic_program_exposure", false).toBool();
	photoProgramMaxISO = qBound(PHOTO_PROGRAM_BASE_ISO, conf->value("viewing/photographic_program_max_iso", PHOTO_PROGRAM_MAX_ISO).toDouble(), PHOTO_PROGRAM_ISO_CEILING);
	photoProgramMaxTime = qBound(PHOTO_PROGRAM_MIN_MAX_TIME, conf->value("viewing/photographic_program_max_time", PHOTO_AUTO_MAX_TIME).toDouble(), PHOTO_AUTO_MAX_TIME);
	photoMount = qBound(static_cast<int>(PhotoMountTripod), conf->value("viewing/photographic_mount", static_cast<int>(PhotoMountTripod)).toInt(), static_cast<int>(PhotoMountView));
	photoSensorFormat = qBound(0, conf->value("viewing/photographic_sensor_format", static_cast<int>(PhotoSensorFullFrame)).toInt(), PHOTO_SENSOR_FORMAT_COUNT - 1);
	photoFilm = qBound(0, conf->value("viewing/photographic_film", 0).toInt(), PHOTO_FILM_COUNT - 1);
	photoFilmPush = qBound(0, conf->value("viewing/photographic_film_push", 0).toInt(), PHOTO_FILM_MAX_PUSH);
	photoDigitalISO = photoISO;
	if (photoFilm != PhotoFilmNone)
	{
		photoISO = getPhotoActiveFilmSpec().iso;
		photoHaloFraction = photoFilmHaloFraction;
	}
	updatePhotoToneCurve();
	flagPhotoZoomWholeMillimeters = conf->value("viewing/flag_photographic_zoom_whole_millimeters", false).toBool();
	flagPhotoPortrait = conf->value("viewing/flag_photographic_portrait", false).toBool();
	flagPhotoFrameMask = conf->value("viewing/flag_photographic_frame_mask", true).toBool();
	photoEclipseAtmosphere = qBound(0, conf->value("viewing/photographic_eclipse_atmosphere", 1).toInt(), 3);
	photoNoise = qBound(static_cast<int>(PhotoNoiseOff), conf->value("viewing/photographic_noise", static_cast<int>(PhotoNoiseImage)).toInt(), static_cast<int>(PhotoNoisePixel));
	updatePhotoWhiteBalanceMatrix();
	{
		const QString path = StelFileMgr::findFile("atmosphere/lightweight-extinction.dat");
		if (!path.isEmpty() && atmosphereTransmission.load(path))
			qInfo() << "Photographic mode: extinction colors from" << QDir::toNativeSeparators(path);
		else
			qWarning() << "Photographic mode: no atmosphere/lightweight-extinction.dat, extinction colors are approximated";
	}
	setFlagStarMagnitudeLimit(conf->value("astro/flag_star_magnitude_limit", false).toBool());
	setCustomStarMagnitudeLimit(conf->value("astro/star_magnitude_limit", 6.5).toDouble());
	setFlagPlanetMagnitudeLimit(conf->value("astro/flag_planet_magnitude_limit", false).toBool());
	setCustomPlanetMagnitudeLimit(conf->value("astro/planet_magnitude_limit", 6.5).toDouble());
	setFlagNebulaMagnitudeLimit(conf->value("astro/flag_nebula_magnitude_limit", false).toBool());
	setCustomNebulaMagnitudeLimit(conf->value("astro/nebula_magnitude_limit", 8.5).toDouble());

	setLightPollutionLuminance(conf->value("stars/init_light_pollution_luminance", StelCore::bortleScaleIndexToLuminance(3)).toFloat());
	setRelativeStarScale(conf->value("stars/relative_scale", 1.0).toDouble());
	setAbsoluteStarScale(conf->value("stars/absolute_scale", 1.0).toDouble());
	setExtinctionCoefficient(conf->value("landscape/atmospheric_extinction_coefficient", 0.13).toDouble());

	const QString extinctionMode = conf->value("astro/extinction_mode_below_horizon", "zero").toString();
	// zero by default
	if (extinctionMode=="mirror")
		extinction.setUndergroundExtinctionMode(Extinction::UndergroundExtinctionMirror);
	else if (extinctionMode=="max")
		extinction.setUndergroundExtinctionMode(Extinction::UndergroundExtinctionMax);

	setAtmosphereTemperature(conf->value("landscape/temperature_C", 15.0).toDouble());
	setAtmospherePressure(conf->value("landscape/pressure_mbar", 1013.0).toDouble());

	// four extras for finetuning
	setFlagDrawSunAfterAtmosphere(conf->value("landscape/draw_sun_after_atmosphere",false).toBool());
	setFlagEarlySunHalo(conf->value("landscape/early_solar_halo",false).toBool());
	setFlagTfromK(conf->value("landscape/use_T_from_k",false).toBool());
	setT(conf->value("landscape/turbidity",5.).toDouble());

	// Initialize buffers for use by gl vertex array
	vertexArray = new StarVertex[maxPointSources*6];
	
	textureCoordArray = new unsigned char[maxPointSources*6*2];
	for (unsigned int i=0;i<maxPointSources; ++i)
	{
		static const unsigned char texElems[] = {0, 0, 255, 0, 255, 255, 0, 0, 255, 255, 0, 255};
		unsigned char* elem = &textureCoordArray[i*6*2];
		std::memcpy(elem, texElems, 12);
	}
	texImgHalo=QImage(StelFileMgr::getInstallationDir()+"/textures/star16x16.png");
	texImgHaloSpiky=QImage(StelFileMgr::getInstallationDir()+"/textures/star16x16_rays.png");

	// Tessellate texture into an equispaced 5x5 field. Vertices have to be computed per frame.
	for (int j=0;j<coronaMeshDim-1;++j)
	{
		for (int i=0;i<coronaMeshDim-1;++i)
		{
			coronaTextureCoords << Vec2f((float(i))/(coronaMeshDim-1),     (float(j))/(coronaMeshDim-1));
			coronaTextureCoords << Vec2f((float(i)+1.f)/(coronaMeshDim-1), (float(j))/(coronaMeshDim-1));
			coronaTextureCoords << Vec2f((float(i))/(coronaMeshDim-1),     (float(j)+1.f)/(coronaMeshDim-1));
			coronaTextureCoords << Vec2f((float(i)+1.f)/(coronaMeshDim-1), (float(j))/(coronaMeshDim-1));
			coronaTextureCoords << Vec2f((float(i)+1.f)/(coronaMeshDim-1), (float(j)+1.f)/(coronaMeshDim-1));
			coronaTextureCoords << Vec2f((float(i))/(coronaMeshDim-1),     (float(j)+1.f)/(coronaMeshDim-1));
		}
	}
}

StelSkyDrawer::~StelSkyDrawer()
{
	delete[] vertexArray;
	vertexArray = Q_NULLPTR;
	delete[] textureCoordArray;
	textureCoordArray = Q_NULLPTR;
	
	delete starShaderProgram;
	starShaderProgram = Q_NULLPTR;
}

// Init parameters from config file
void StelSkyDrawer::init()
{
	initializeOpenGLFunctions();

	// Load star texture no mipmap:
	texHalo = StelApp::getInstance().getTextureManager().createTexture(texImgHalo);
	texHaloRayed = StelApp::getInstance().getTextureManager().createTexture(texImgHaloSpiky);
	texBigHalo = StelApp::getInstance().getTextureManager().createTexture(StelFileMgr::getInstallationDir()+"/textures/haloLune.png");
	texSunHalo = StelApp::getInstance().getTextureManager().createTexture(StelFileMgr::getInstallationDir()+"/textures/halo.png");	
	texSunCorona = StelApp::getInstance().getTextureManager().createTexture(StelFileMgr::getInstallationDir()+"/textures/corona.png");

	// Create shader program
	QOpenGLShader vshader(QOpenGLShader::Vertex);
	const char *vsrc =
		"ATTRIBUTE mediump vec2 pos;\n"
		"ATTRIBUTE mediump vec2 texCoord;\n"
		"ATTRIBUTE mediump vec3 color;\n"
		"uniform mediump mat4 projectionMatrix;\n"
		"VARYING mediump vec2 texc;\n"
		"VARYING mediump vec3 outColor;\n"
		"void main(void)\n"
		"{\n"
		"    gl_Position = projectionMatrix * vec4(pos.x, pos.y, 0, 1);\n"
		"    texc = texCoord;\n"
		"    outColor = color;\n"
		"}\n";
	vshader.compileSourceCode(StelOpenGL::globalShaderPrefix(StelOpenGL::VERTEX_SHADER) + vsrc);
	if (!vshader.log().isEmpty()) { qWarning() << "StelSkyDrawer::init(): Warnings while compiling vshader: " << vshader.log(); }

	QOpenGLShader fshader(QOpenGLShader::Fragment);
	const char *fsrc =
		"VARYING mediump vec2 texc;\n"
		"VARYING mediump vec3 outColor;\n"
		"uniform sampler2D tex;\n"
		"void main(void)\n"
		"{\n"
		"    FRAG_COLOR = texture2D(tex, texc)*vec4(outColor, 1.);\n"
		"}\n";
	fshader.compileSourceCode(StelOpenGL::globalShaderPrefix(StelOpenGL::FRAGMENT_SHADER) + fsrc);
	if (!fshader.log().isEmpty()) { qWarning() << "StelSkyDrawer::init(): Warnings while compiling fshader: " << fshader.log(); }

	starShaderProgram = new QOpenGLShaderProgram(QOpenGLContext::currentContext());
	starShaderProgram->addShader(&vshader);
	starShaderProgram->addShader(&fshader);
	StelPainter::linkProg(starShaderProgram, "starShader");
	starShaderVars.projectionMatrix = starShaderProgram->uniformLocation("projectionMatrix");
	starShaderVars.texCoord = starShaderProgram->attributeLocation("texCoord");
	starShaderVars.pos = starShaderProgram->attributeLocation("pos");
	starShaderVars.color = starShaderProgram->attributeLocation("color");
	starShaderVars.texture = starShaderProgram->uniformLocation("tex");

	vbo->create();
	vbo->bind();
	vbo->setUsagePattern(QOpenGLBuffer::StreamDraw);
	vbo->allocate(maxPointSources*6*sizeof(StarVertex) + maxPointSources*6*2);

	if(vao->create())
	{
		vao->bind();
		setupCurrentVAO();
		vao->release();
	}

	vbo->release();

	initPhotoStarRendering();

	update(0);
}

Vec3f StelSkyDrawer::photoColorTable[128];

// For a Moffat profile M(r) with the given beta, normalized to unit integral, evaluated at the distance
// max(0, r-R) from the edge of a disk of radius R: its integral is 1 + term*R/alpha + (beta-1)*R^2/alpha^2,
// with term = (beta-1) sqrt(pi) Gamma(beta-1/2) / Gamma(beta). This returns that term.
static float photoDiskTerm(float beta)
{
	return static_cast<float>((beta-1.) * std::sqrt(M_PI) * std::tgamma(beta-0.5) / std::tgamma(static_cast<double>(beta)));
}

void StelSkyDrawer::initPhotoStarRendering()
{
	// The photographic mode needs high graphics mode (OpenGL 3.3 core or GLES 3): it relies on
	// floating-point buffers and instanced drawing.
	if (!StelMainView::getInstance().getGLInformation().isHighGraphicsMode)
		return;

	photoInstances.resize(maxPhotoPointSources);

	// Linear version of the B-V color table, normalized to unit luminance: the magnitude gives
	// the V-band light, which is close to luminance. Computed once instead of per star and frame.
	for (int i=0; i<128; ++i)
	{
		const Vec3f linear = srgbToLinear(colorTable[i]);
		const float Y = 0.2126f*linear[0] + 0.7152f*linear[1] + 0.0722f*linear[2];
		photoColorTable[i] = (Y > 0.f) ? linear/Y : Vec3f(1.f);
	}

	// The profile is evaluated per pixel. Everything is in floats: a single star may record
	// hundreds of times the saturation level, and faint stars need far finer steps than 8 bits.
	// A bright body with a resolved disk (Moon, Sun, planets when zoomed in) uses the same profile,
	// evaluated at the distance from the lit part of its disk (the limb, or the terminator for a body
	// in phases), and renormalized so that its integral stays the same (see shape and vNorm).
	const char* vsrc = R"(
ATTRIBUTE highp vec2 corner;  // per vertex: corner of the sprite, in [-1, 1]
ATTRIBUTE highp vec2 center;  // per star: window position (for a body: center of its disk)
ATTRIBUTE highp vec3 color;   // per star
ATTRIBUTE highp float radius; // per star
ATTRIBUTE highp vec4 shape;   // per star: x = disk radius (0 for stars), y = weight of the PSF core,
                              //  z = radius of a disk with the area of the lit part, w = cos(phase angle)
ATTRIBUTE highp vec2 sunDir;  // per star: direction towards the Sun on the screen (bodies in phases)
ATTRIBUTE highp vec4 trail;   // per star: offsets of the image at the start (xy) and at the end (zw) of the exposure
                              //  from its position at mid-exposure (diurnal motion), pixels; 0 = no trail
ATTRIBUTE highp vec3 trailStart; // per star: light at the start of the exposure relative to mid-exposure (extinction)
ATTRIBUTE highp vec3 trailEnd;   // per star: the same at the end
uniform highp mat4 projectionMatrix;
uniform highp float coreAlpha;
uniform highp float coreBeta;
uniform highp float haloAlpha;
uniform highp float haloBeta;
uniform highp float coreDiskTerm; // (beta-1) sqrt(pi) Gamma(beta-1/2) / Gamma(beta) of the core
uniform highp float haloDiskTerm; // the same for the halo
uniform highp float spikeSigma;
uniform highp float spikeLength;
VARYING highp vec2 vOffset;
VARYING highp vec3 vColor;
VARYING highp float vRadius;
VARYING highp vec4 vNorm;  // x: core weight x norm, y: halo norm, z: spike norm, w: core weight
VARYING highp vec4 vShape;
VARYING highp vec2 vSunDir;
VARYING highp vec3 vLens;  // lens model: x = distance from the image center / half diagonal, yz = radial direction
uniform highp vec2 lensCenter;
uniform highp float lensCorner;
uniform highp float defocusRadius; // radius of the blur disk of a defocused star (pixels), 0 = in focus
uniform highp float defocusArea;   // area of the image of the aperture, in units of its radius squared
uniform highp float lensPupil;     // optical vignetting (see StelSkyDrawer::getPhotoLens())
uniform highp float lensShift;
uniform highp float lensVignetting; // 1 = on
VARYING highp vec3 vDefocus; // x: 1 = defocused image, y: radius of the blur disk, z: 1 / its area (pixels)
VARYING highp vec4 vTrail;     // the trail (see the attribute)
VARYING highp vec3 vTrailInfo; // x: number of straight segments, y: approximate length (pixels), z: 1 = trailed
VARYING highp vec3 vTrailLogStart; // log of the extinction factors at the start and the end of the exposure
VARYING highp vec3 vTrailLogEnd;
const highp float PI_V = 3.14159265358979;
// Area of the intersection of two circles with radii r1, r2 and the distance d of their centers
highp float circleOverlap(highp float r1, highp float r2, highp float d)
{
	if (d >= r1 + r2) return 0.;
	if (d <= abs(r2 - r1)) return PI_V * min(r1, r2) * min(r1, r2);
	highp float a1 = r1*r1 * acos(clamp((d*d + r1*r1 - r2*r2) / (2.*d*r1), -1., 1.));
	highp float a2 = r2*r2 * acos(clamp((d*d + r2*r2 - r1*r1) / (2.*d*r2), -1., 1.));
	highp float k = (-d + r1 + r2) * (d + r1 - r2) * (d - r1 + r2) * (d + r1 + r2);
	return a1 + a2 - 0.5 * sqrt(max(k, 0.));
}
void main(void)
{
	// A star trail is the quadratic curve through the positions at the start (A), the middle (0) and the end (B)
	// of the exposure. The sprite is a box around it (along the chord and across), with the radius of the
	// stationary image added on all sides.
	vTrail = trail;
	vTrailLogStart = log(max(trailStart, vec3(1e-9)));
	vTrailLogEnd = log(max(trailEnd, vec3(1e-9)));
	highp vec2 A = trail.xy;
	highp vec2 B = trail.zw;
	highp float trailLength = length(A) + length(B);
	vTrailInfo = vec3(1., trailLength, 0.);
	if (trailLength > 0.5)
	{
		highp vec2 C = -0.5 * (A + B); // control point: the curve passes through 0 at its middle
		// Enough straight segments that they stay within a quarter pixel of the curve (its sagitta / K^2)
		highp float sagitta = length(0.5 * (A + B));
		vTrailInfo = vec3(clamp(ceil(sqrt(sagitta / 0.25)), 1., 16.), trailLength, 1.);
		highp vec2 chord = B - A;
		highp float chordLength = length(chord);
		highp vec2 e = chordLength > 1e-3 ? chord / chordLength : vec2(1., 0.);
		highp vec2 n = vec2(-e.y, e.x);
		highp vec3 pe = vec3(dot(A, e), dot(B, e), dot(C, e));
		highp vec3 pn = vec3(dot(A, n), dot(B, n), dot(C, n));
		highp float e0 = min(min(pe.x, pe.y), min(pe.z, 0.));
		highp float e1 = max(max(pe.x, pe.y), max(pe.z, 0.));
		highp float n0 = min(min(pn.x, pn.y), min(pn.z, 0.));
		highp float n1 = max(max(pn.x, pn.y), max(pn.z, 0.));
		vOffset = e * (0.5*(e0 + e1) + corner.x * (0.5*(e1 - e0) + radius))
		        + n * (0.5*(n0 + n1) + corner.y * (0.5*(n1 - n0) + radius));
	}
	else
		vOffset = corner * radius;
	highp vec2 fromCenter = center - lensCenter;
	highp float dist = length(fromCenter);
	vLens = vec3(dist / lensCorner, dist > 0. ? fromCenter / dist : vec2(1., 0.));
	// Out of focus, a star (or a body smaller than the blur) becomes an image of the aperture. Off axis the lens
	// barrel cuts it (the "cat's eye" of the optical vignetting): its light is spread over the smaller area,
	// the light lost there is the vignetting of the camera stage.
	highp float b = abs(defocusRadius);
	highp float Reff = 0.;
	bool defocused = b > 0. && shape.x < b;
	vDefocus = vec3(0.);
	if (defocused)
	{
		Reff = sqrt(b*b + shape.z*shape.z);
		highp float cut = 1.;
		if (lensVignetting > 0.5)
			cut = circleOverlap(lensPupil, 1., lensShift * vLens.x) / (PI_V * lensPupil * lensPupil);
		vDefocus = vec3(1., Reff, 1. / (max(cut, 1e-3) * defocusArea * Reff * Reff));
	}
	gl_Position = projectionMatrix * vec4(center + vOffset, 0, 1);
	vColor = color;
	vRadius = radius;
	vShape = shape;
	vSunDir = sunDir;
	// Integral of a radial profile evaluated at max(0, r-R), relative to the plain profile:
	// 1 + (disk term) R/alpha + (beta-1) R^2/alpha^2. The last term is the part inside the disk.
	// With this normalization the profile at the edge of the lit part is the light spread from a
	// disk of the same surface brightness (within a factor 2 of the exact half-plane value).
	// Inside the lit part that light stays on the disk (it only blurs it), so it is not drawn
	// there (see the fragment shader): full for an unresolved body, none for a large disk.
	// A defocused image: the halo and the spikes spread around its blur disk.
	// A trailed image is a point source moving along the trail (its halo is integrated along it, unit integral).
	bool trailed = trailLength > 0.5;
	highp float R = defocused ? Reff : shape.z;
	highp float Rs = defocused ? Reff : (trailed ? 0. : shape.z);
	highp float Rh = trailed ? 0. : R;
	highp float w = shape.y;
	highp float cn = 1. / (1. + coreDiskTerm*R/coreAlpha + (coreBeta-1.)*R*R/(coreAlpha*coreAlpha));
	highp float hn = 1. / (1. + haloDiskTerm*Rh/haloAlpha + (haloBeta-1.)*Rh*Rh/(haloAlpha*haloAlpha));
	// A spike becomes a band as wide as the lit part
	highp float s1 = sqrt(2.*PI_V)*spikeSigma;
	highp float s2 = PI_V*spikeLength;
	highp float sn = (s1*s2) / ((s1 + 2.*Rs) * (s2 + 2.*Rs));
	vNorm = vec4(w*cn, hn, sn, w);
}
)";
	const char* fsrc = R"(
VARYING highp vec2 vOffset;
VARYING highp vec3 vColor;
VARYING highp float vRadius;
VARYING highp vec4 vNorm;
VARYING highp vec4 vShape;
VARYING highp vec2 vSunDir;
VARYING highp vec3 vLens;
uniform highp float comaLength;   // lens: length of the coma in the corner (pixels), 0 = none
uniform highp float astigLength;  // lens: length of the astigmatic line in the corner (pixels), 0 = none
uniform bool chromaEnabled;       // lens: chromatic aberration
uniform highp vec3 chromaShift;   // radial shift of the red, green and blue images in the corner (pixels)
uniform highp vec3 chromaBlur;    // radius of their blur disks (pixels): axial chromatic aberration
uniform highp float coreAlpha;
uniform highp float coreBeta;
uniform highp float haloAlpha;
uniform highp float haloBeta;
uniform highp float haloFraction; // 0 when the halo is applied to the whole image (bloom)
uniform highp float coreFraction; // 1 - halo fraction - spike fraction
uniform int spikeLines;           // number of lines through the star (spikes/2), 0 = no spikes
uniform highp float spikeFraction;
uniform highp float spikeSigma;   // half-width of a spike (Gaussian sigma), pixels
uniform highp float spikeLength;  // length scale of the 1/r^2 falloff, pixels
uniform highp vec2 spikeDirs[9];  // directions of the lines (up to PHOTO_MAX_SPIKE_LINES)
uniform highp float visibleThreshold; // fainter contributions cannot change a pixel
uniform highp float spikeStart;   // with bloom: the spikes start here (the inner part comes from the image)
VARYING highp vec3 vDefocus;
VARYING highp vec4 vTrail;
VARYING highp vec3 vTrailInfo;
VARYING highp vec3 vTrailLogStart;
VARYING highp vec3 vTrailLogEnd;
uniform highp float defocusSign;  // -1: the image of the aperture is inverted (the rays crossed before the sensor)
uniform highp float defocusSA;    // spherical aberration: > 0 bright rim, < 0 bright center
uniform highp float defocusEdge;  // half width of the blurred edge of the disk (pixels)
uniform highp float coreFwhm;     // FWHM of the PSF core (pixels)
uniform int apertureBlades;       // 0 = round
uniform highp float apertureStraightness;
uniform highp float apertureRotation;
uniform highp float lensPupil;
uniform highp float lensShift;
uniform highp float lensVignetting;
const highp float PI = 3.14159265358979;
// Moffat profile normalized to unit integral over the plane
highp float moffat(highp float r2, highp float alpha, highp float beta)
{
	return (beta - 1.) / (PI * alpha * alpha) * pow(1. + r2 / (alpha * alpha), -beta);
}
// Anisotropic Moffat profile, unit integral: widths au along the radial direction, av across
highp float moffat2(highp float u, highp float v, highp float au, highp float av, highp float beta)
{
	return (beta - 1.) / (PI * au * av) * pow(1. + u*u/(au*au) + v*v/(av*av), -beta);
}
// The PSF core of a star away from the image center, with the off-axis aberrations of the lens.
// Coma: the image of a point is a family of circles whose radius grows like t^2 and whose centers move
// outwards by twice that (a comet with its tail away from the center); here a few blurred blobs along it.
// Astigmatism: the image is drawn out into a short line, across the radial direction (tangential focus).
highp float aberratedCore(highp vec2 offset, highp float alpha)
{
	highp float rho = vLens.x;
	highp vec2 er = vLens.yz;
	highp float u = dot(offset, er);
	highp float v = dot(offset, vec2(-er.y, er.x));
	highp float coma = comaLength * rho;
	highp float astig = astigLength * rho * rho;
	highp float a2 = alpha * alpha;
	// a line of length L convolved with the core: about the width of a uniform segment, L^2/12, added in quadrature
	highp float av2 = a2 + astig * astig / 3.;
	const int K = 6;
	highp float sum = 0.;
	for (int k = 0; k < K; ++k)
	{
		highp float t = (float(k) + 0.5) / float(K);
		highp float ring = coma * t * t;       // radius of this comatic circle
		highp float au = sqrt(a2 + ring*ring);
		highp float av = sqrt(av2 + ring*ring);
		sum += moffat2(u - 2.*ring, v, au, av, coreBeta);
	}
	return sum / float(K);
}
// The Moffat profiles (beta 3: core, beta 1.5: halo) of a point source moving uniformly along a straight
// segment of length L (u: along the segment from its start, v: across), i.e. integrated along it. Exact; unit integral.
highp float segmentG3(highp float x, highp float A)
{
	highp float s = A + x*x;
	highp float rA = sqrt(A);
	return x / (4.*A*s*s) + 3.*x / (8.*A*A*s) + 3. / (8.*A*A*rA) * atan(x / rA);
}
highp float moffatSegment3(highp float u, highp float v, highp float L, highp float alpha)
{
	if (L < 1e-3 * alpha)
		return moffat(u*u + v*v, alpha, 3.);
	highp float a2 = alpha * alpha;
	highp float A = a2 + v*v;
	return 2.*a2*a2 / (PI*L) * (segmentG3(u, A) - segmentG3(u - L, A));
}
highp float moffatSegment15(highp float u, highp float v, highp float L, highp float alpha)
{
	if (L < 1e-3 * alpha)
		return moffat(u*u + v*v, alpha, 1.5);
	highp float A = alpha*alpha + v*v;
	highp float w = u - L;
	return 0.5*alpha / (PI*L) * (u / (A*sqrt(A + u*u)) - w / (A*sqrt(A + w*w)));
}
// A defocused point source: the image of the aperture (disk, or polygon of the blades) of radius R,
// cut by the lens barrel off axis (cat's eye), with the edge blurred by the PSF core and the aberrations.
// Unit integral (vDefocus.z is 1 / area).
highp float defocusDisk(highp vec2 p, highp float R)
{
	highp vec2 q = defocusSign * p / R;
	highp float r = length(q);
	highp float shape = r;
	if (apertureBlades >= 3 && apertureStraightness > 0.)
	{
		highp float n = float(apertureBlades);
		highp float sector = 2.*PI/n;
		highp float d = mod(atan(q.y, q.x) - apertureRotation, sector) - 0.5*sector; // angle from the normal of the nearest edge
		shape = mix(r, r * cos(d) / cos(PI/n), apertureStraightness);
	}
	highp float sd = shape - 1.;
	if (lensVignetting > 0.5)
	{
		// the lens barrel: radius 1/pupil, shifted with the distance from the image center
		highp vec2 barrel = -vLens.yz * (lensShift * vLens.x / lensPupil);
		sd = max(sd, length(q - barrel) - 1./lensPupil);
	}
	highp float rho = vLens.x;
	highp float coma = 0.5 * comaLength * rho;
	highp float astig = 0.5 * astigLength * rho * rho;
	highp float edge = sqrt(defocusEdge*defocusEdge + coma*coma + astig*astig);
	highp float coverage = 1. - smoothstep(-edge, edge, sd * R);
	// Spherical aberration: the zones of the lens focus at slightly different distances, so the light is
	// not spread evenly; on one side of the focus the rim is bright, on the other the center
	highp float sa = max(0., 1. + defocusSA * (r*r - 0.5));
	return coverage * sa * vDefocus.z;
}
// The core of one color with the chromatic aberration: its image shifted along the radial direction, and a small
// blur disk that widens it (as a small defocus widens the core, see main())
highp vec2 chromaOffset(highp float shift)
{
	return vOffset - vLens.yz * (shift * vLens.x);
}
highp float chromaAlpha(highp float blur)
{
	highp float k = 1.6 * blur / coreFwhm;
	return coreAlpha * sqrt(1. + k*k);
}
void main(void)
{
	highp float r = length(vOffset);
	highp float Rd = vShape.x;     // disk radius
	highp float Rl = vShape.z;     // radius of a disk with the area of the lit part
	highp float cosPhase = vShape.w;
	// Coordinates along the direction to the Sun (u) and across it (v)
	highp float u = dot(vOffset, vSunDir);
	highp float v = dot(vOffset, vec2(-vSunDir.y, vSunDir.x));
	// Distance from the lit part of the disk (stars: from the center). The terminator is the half
	// ellipse u = -cos(phase) sqrt(Rd^2 - v^2); the lit part is on the sunward side of it.
	highp float d;
	if (Rd <= 0.)
		d = r;
	else if (r <= Rd)
	{
		highp float ut = -cosPhase * sqrt(max(0., Rd*Rd - v*v));
		d = max(0., ut - u);
	}
	else if (u >= -cosPhase * r)
		d = r - Rd;  // the nearest point of the limb is lit
	else
	{
		// beyond the dark limb: distance to the nearer cusp
		highp vec2 cusp = vec2(-cosPhase*Rd, sqrt(max(0., 1. - cosPhase*cosPhase))*Rd);
		d = min(distance(vec2(u, v), cusp), distance(vec2(u, v), vec2(cusp.x, -cusp.y)));
	}
	highp float r2 = d * d;
	highp float brightness = max(vColor.r, max(vColor.g, vColor.b));
	highp float profile;
	highp vec3 chroma = vec3(0.); // change of the core in each color (chromatic aberration of the lens)
	// The colored fringes show only where the change of the core makes a visible color difference (a few
	// steps of the screen): bright stars. The others (most) skip it. The change is about (shift + blur)/alpha
	// of the peak of the core.
	bool showChroma = false;
	if (chromaEnabled)
	{
		highp float peak = brightness * coreFraction * vNorm.x * (coreBeta - 1.) / (PI * coreAlpha * coreAlpha);
		highp float change = min(1., (max(abs(chromaShift.r), abs(chromaShift.b)) * vLens.x + max(chromaBlur.r, chromaBlur.b)) / coreAlpha);
		showChroma = peak * change > 4. * visibleThreshold;
	}
	// Where the halo and the spikes start: the lit part, or the blur disk of a defocused image
	highp vec2 spikeP = vOffset - vSunDir * (Rd - Rl*Rl/max(Rd, 1e-6)); // from the photocenter of the lit part
	highp float spikeR = Rl;
	if (vDefocus.x > 0.5)
	{
		highp float Reff = vDefocus.y;
		highp float rp = length(spikeP);
		// A blur disk smaller than the PSF core only widens the core
		highp float t = smoothstep(0.5, 2.0, Reff / coreFwhm);
		highp float core = 0.;
		if (t < 1.)
		{
			highp float k = 1.6 * Reff / coreFwhm;
			highp float alphaWide = coreAlpha * sqrt(1. + k*k);
			core += (1. - t) * moffat(rp*rp, alphaWide, coreBeta);
		}
		if (t > 0.)
			core += t * defocusDisk(spikeP, Reff);
		profile = coreFraction * vNorm.w * core;
		highp float dh = max(0., rp - Reff);
		r2 = dh * dh;
		spikeR = Reff;
	}
	else if (Rd <= 0. && (comaLength > 0. || astigLength > 0.) && vLens.x > 0.05)
	{
		highp float base = aberratedCore(vOffset, coreAlpha);
		profile = coreFraction * vNorm.x * base;
		// Chromatic aberration of a star: the red and the blue core differ from the green one (green is the reference)
		if (showChroma)
			chroma = coreFraction * vNorm.x * vec3(aberratedCore(chromaOffset(chromaShift.r), chromaAlpha(chromaBlur.r)) - base, 0.,
			                                       aberratedCore(chromaOffset(chromaShift.b), chromaAlpha(chromaBlur.b)) - base);
	}
	else
	{
		highp float base = moffat(r2, coreAlpha, coreBeta);
		profile = coreFraction * vNorm.x * base;
		if (showChroma && Rd <= 0.)
		{
			highp vec2 oR = chromaOffset(chromaShift.r);
			highp vec2 oB = chromaOffset(chromaShift.b);
			chroma = coreFraction * vNorm.x * vec3(moffat(dot(oR, oR), chromaAlpha(chromaBlur.r), coreBeta) - base, 0.,
			                                       moffat(dot(oB, oB), chromaAlpha(chromaBlur.b), coreBeta) - base);
		}
	}
	// The halo and the spikes matter only for bright stars: skip them where even their
	// peak would stay below what can show on the screen (most stars)
	bool showHalo = brightness * haloFraction * vNorm.y * (haloBeta - 1.) / (PI * haloAlpha * haloAlpha) > visibleThreshold;
	highp float haloProfile = -1.; // < 0: the halo of the stationary image
	highp vec3 trailTint = vec3(1.); // the extinction along the trail
	highp float fadeDistance = r;
	highp float trailLength = 0.;
	highp vec2 trailNormal = vec2(0.);
	// Star trail: during the exposure the image moves along an arc around the celestial pole. The trailed image
	// is the stationary one integrated along the arc: straight segments of equal exposure time, each integrated exactly.
	if (vTrailInfo.z > 0.5)
	{
		highp vec2 A = vTrail.xy;
		highp vec2 B = vTrail.zw;
		highp vec2 C = -0.5 * (A + B);
		highp vec2 p = spikeP; // from the photocenter
		int K = int(vTrailInfo.x + 0.5);
		highp float invK = 1. / float(K);
		highp float Rdf = vDefocus.x > 0.5 ? vDefocus.y : 0.;
		// a small blur disk only widens the core, as for the stationary image
		highp float kd = 1.6 * Rdf / coreFwhm;
		highp float alphaCore = coreAlpha * sqrt(1. + kd*kd);
		highp float alphaHalo = sqrt(haloAlpha*haloAlpha + 0.5*Rdf*Rdf);
		highp float coreT = 0.;
		highp float haloT = 0.;
		highp float dmin = 1e30;
		highp vec2 nearest = vec2(0.);
		highp float tNearest = 0.5;
		highp vec2 P0 = A;
		for (int k = 1; k <= 16; ++k)
		{
			if (k > K) break;
			highp float t = float(k) * invK;
			highp vec2 P1 = (1.-t)*(1.-t)*A + 2.*t*(1.-t)*C + t*t*B;
			highp vec2 seg = P1 - P0;
			highp float L = length(seg);
			highp vec2 e = L > 1e-4 ? seg / L : vec2(1., 0.);
			highp vec2 q = p - P0;
			highp float u = dot(q, e);
			highp float v = dot(q, vec2(-e.y, e.x));
			coreT += moffatSegment3(u, v, L, alphaCore);
			if (showHalo)
				haloT += moffatSegment15(u, v, L, alphaHalo);
			highp float uc = clamp(u, 0., L);
			highp float dd = length(q - e*uc);
			if (dd < dmin)
			{
				dmin = dd;
				nearest = P0 + e*uc;
				tNearest = (float(k - 1) + (L > 0. ? uc / L : 0.)) * invK;
			}
			trailLength += L;
			P0 = P1;
		}
		coreT *= invK;
		haloT *= invK;
		if (Rdf > 0.)
		{
			// Out of focus: the blur disk swept along the trail, of even brightness
			highp float tD = smoothstep(0.5, 2.0, Rdf / coreFwhm);
			highp float coverage = 1. - smoothstep(-defocusEdge, defocusEdge, dmin - Rdf);
			coreT = mix(coreT, coverage / (PI*Rdf*Rdf + 2.*Rdf*trailLength), tD);
		}
		// A short trail keeps the details of the stationary image (aberrations, the shape of the blur disk)
		highp float tau = smoothstep(0.5, 2.0, trailLength / max(coreFwhm, 2.*Rdf));
		profile = mix(profile, coreFraction * vNorm.w * coreT, tau);
		chroma *= 1. - tau; // (a trail smears the colored fringes)
		// (the whole trailed image takes the tint of its nearest point, see trailTint below)
		haloProfile = haloT;
		// Extinction along the trail: the light at this point of the trail (mostly from the nearest part), from
		// the factors at the start (t = 0), the middle (1) and the end; interpolated in the log (magnitudes)
		highp float tn = tNearest;
		trailTint = exp(vTrailLogStart * ((1.-tn)*(1.-2.*tn)) + vTrailLogEnd * (tn*(2.*tn-1.)));
		// the spikes move with the image: they start from the trail (see below)
		spikeP = p - nearest;
		spikeR = Rdf;
		fadeDistance = dmin;
		highp vec2 chord = B - A;
		highp float chordLength = length(chord);
		trailNormal = chordLength > 1e-3 ? vec2(-chord.y, chord.x) / chordLength : vec2(0.);
	}
	if (showHalo)
		profile += haloFraction * vNorm.y * (haloProfile >= 0. ? haloProfile : moffat(r2, haloAlpha, haloBeta));
	// Diffraction spikes from straight edges in the aperture: thin lines whose intensity
	// falls off like 1/r^2 along them. Each line is normalized to unit integral.
	if (spikeLines > 0)
	{
		highp float lineNorm = vNorm.z * spikeFraction / (float(spikeLines) * sqrt(2.*PI) * spikeSigma * PI * spikeLength);
		if (brightness * lineNorm > visibleThreshold)
		{
			// Spikes start from the lit part (or the blur disk): around its photocenter, as wide as it
			highp vec2 p = spikeP;
			for (int k = 0; k < 9; ++k)
			{
				if (k >= spikeLines) break;
				highp vec2 dir = spikeDirs[k];
				highp float acrossC = abs(dot(p, vec2(-dir.y, dir.x)));
				highp float along = max(0., abs(dot(p, dir)) - sqrt(max(0., spikeR*spikeR - acrossC*acrossC)));
				highp float across = max(0., acrossC - spikeR);
				highp float line = lineNorm * exp(-0.5 * across * across / (spikeSigma * spikeSigma))
				                            / (1. + along * along / (spikeLength * spikeLength));
				if (spikeStart > 0.)
					line *= smoothstep(spikeStart - 0.5, spikeStart + 0.5, along);
				// Trailed: a spike across the trail is swept over it, its light spread over the trail length
				if (trailLength > 0.)
				{
					highp float width = sqrt(2.*PI) * spikeSigma + 2.*spikeR;
					line *= width / (width + trailLength * abs(dot(dir, trailNormal)));
				}
				profile += line;
			}
		}
	}
	// On the lit part (d = 0) the halo and spikes keep their peak value: the light the lens spreads
	// stays on the disk there (the disk itself is drawn with the remaining fraction), and it fills
	// small dark details such as crater shadows, as on a real overexposed photo.
	// Fade the outermost part of the sprite, so that its edge never shows
	highp float fade = 1. - smoothstep(Rd + 0.75 * (vRadius - Rd), vRadius, fadeDistance);
	FRAG_COLOR = vec4(min(max(vColor * trailTint * (vec3(profile) + chroma) * fade, vec3(0.)), vec3(1e30)), 1.); // guard against overflow
}
)";

	photoStarShaderProgram.reset(new QOpenGLShaderProgram);
	photoStarShaderProgram->addShaderFromSourceCode(QOpenGLShader::Vertex, StelOpenGL::globalShaderPrefix(StelOpenGL::VERTEX_SHADER) + vsrc);
	photoStarShaderProgram->addShaderFromSourceCode(QOpenGLShader::Fragment, StelOpenGL::globalShaderPrefix(StelOpenGL::FRAGMENT_SHADER) + fsrc);
	if (!StelPainter::linkProg(photoStarShaderProgram.get(), "photographic star shader"))
	{
		photoStarShaderProgram.reset();
		return;
	}
	auto& prog = *photoStarShaderProgram;
	photoStarShaderVars.projectionMatrix = prog.uniformLocation("projectionMatrix");
	photoStarShaderVars.corner = prog.attributeLocation("corner");
	photoStarShaderVars.center = prog.attributeLocation("center");
	photoStarShaderVars.color = prog.attributeLocation("color");
	photoStarShaderVars.radius = prog.attributeLocation("radius");
	photoStarShaderVars.shape = prog.attributeLocation("shape");
	photoStarShaderVars.sunDir = prog.attributeLocation("sunDir");
	photoStarShaderVars.trail = prog.attributeLocation("trail");
	photoStarShaderVars.trailStart = prog.attributeLocation("trailStart");
	photoStarShaderVars.trailEnd = prog.attributeLocation("trailEnd");
	photoStarShaderVars.coreAlpha = prog.uniformLocation("coreAlpha");
	photoStarShaderVars.coreBeta = prog.uniformLocation("coreBeta");
	photoStarShaderVars.haloAlpha = prog.uniformLocation("haloAlpha");
	photoStarShaderVars.haloBeta = prog.uniformLocation("haloBeta");
	photoStarShaderVars.haloFraction = prog.uniformLocation("haloFraction");
	photoStarShaderVars.coreFraction = prog.uniformLocation("coreFraction");
	photoStarShaderVars.coreDiskTerm = prog.uniformLocation("coreDiskTerm");
	photoStarShaderVars.haloDiskTerm = prog.uniformLocation("haloDiskTerm");
	photoStarShaderVars.spikeLines = prog.uniformLocation("spikeLines");
	photoStarShaderVars.spikeFraction = prog.uniformLocation("spikeFraction");
	photoStarShaderVars.spikeSigma = prog.uniformLocation("spikeSigma");
	photoStarShaderVars.spikeLength = prog.uniformLocation("spikeLength");
	photoStarShaderVars.spikeDirs = prog.uniformLocation("spikeDirs");
	photoStarShaderVars.visibleThreshold = prog.uniformLocation("visibleThreshold");
	photoStarShaderVars.spikeStart = prog.uniformLocation("spikeStart");
	photoStarShaderVars.lensCenter = prog.uniformLocation("lensCenter");
	photoStarShaderVars.lensCorner = prog.uniformLocation("lensCorner");
	photoStarShaderVars.comaLength = prog.uniformLocation("comaLength");
	photoStarShaderVars.astigLength = prog.uniformLocation("astigLength");
	photoStarShaderVars.defocusRadius = prog.uniformLocation("defocusRadius");
	photoStarShaderVars.defocusArea = prog.uniformLocation("defocusArea");
	photoStarShaderVars.defocusSign = prog.uniformLocation("defocusSign");
	photoStarShaderVars.defocusSA = prog.uniformLocation("defocusSA");
	photoStarShaderVars.defocusEdge = prog.uniformLocation("defocusEdge");
	photoStarShaderVars.coreFwhm = prog.uniformLocation("coreFwhm");
	photoStarShaderVars.apertureBlades = prog.uniformLocation("apertureBlades");
	photoStarShaderVars.apertureStraightness = prog.uniformLocation("apertureStraightness");
	photoStarShaderVars.apertureRotation = prog.uniformLocation("apertureRotation");
	photoStarShaderVars.lensPupil = prog.uniformLocation("lensPupil");
	photoStarShaderVars.lensShift = prog.uniformLocation("lensShift");
	photoStarShaderVars.lensVignetting = prog.uniformLocation("lensVignetting");

	const GLfloat corners[] = {-1,-1,  1,-1,  1,1,  -1,-1,  1,1,  -1,1};
	photoCornerVbo.reset(new QOpenGLBuffer(QOpenGLBuffer::VertexBuffer));
	photoCornerVbo->create();
	photoCornerVbo->bind();
	photoCornerVbo->allocate(corners, sizeof(corners));
	photoCornerVbo->release();

	photoVbo.reset(new QOpenGLBuffer(QOpenGLBuffer::VertexBuffer));
	photoVbo->create();
	photoVbo->setUsagePattern(QOpenGLBuffer::StreamDraw);
	photoVbo->bind();
	photoVbo->allocate(static_cast<int>(maxPhotoPointSources*sizeof(PhotoStarInstance)));
	photoVbo->release();

	photoVao.reset(new QOpenGLVertexArrayObject);
	photoVao->create();
	photoVao->bind();
	auto* hg = StelOpenGL::highGraphicsFunctions();
	photoCornerVbo->bind();
	prog.setAttributeBuffer(photoStarShaderVars.corner, GL_FLOAT, 0, 2, 0);
	prog.enableAttributeArray(photoStarShaderVars.corner);
	photoVbo->bind();
	prog.setAttributeBuffer(photoStarShaderVars.center, GL_FLOAT, offsetof(PhotoStarInstance, center), 2, sizeof(PhotoStarInstance));
	prog.setAttributeBuffer(photoStarShaderVars.color,  GL_FLOAT, offsetof(PhotoStarInstance, color),  3, sizeof(PhotoStarInstance));
	prog.setAttributeBuffer(photoStarShaderVars.radius, GL_FLOAT, offsetof(PhotoStarInstance, radius), 1, sizeof(PhotoStarInstance));
	prog.setAttributeBuffer(photoStarShaderVars.shape,  GL_FLOAT, offsetof(PhotoStarInstance, shape),  4, sizeof(PhotoStarInstance));
	prog.setAttributeBuffer(photoStarShaderVars.sunDir, GL_FLOAT, offsetof(PhotoStarInstance, sunDir), 2, sizeof(PhotoStarInstance));
	prog.setAttributeBuffer(photoStarShaderVars.trail,  GL_FLOAT, offsetof(PhotoStarInstance, trail),  4, sizeof(PhotoStarInstance));
	prog.setAttributeBuffer(photoStarShaderVars.trailStart, GL_FLOAT, offsetof(PhotoStarInstance, trailStart), 3, sizeof(PhotoStarInstance));
	prog.setAttributeBuffer(photoStarShaderVars.trailEnd,   GL_FLOAT, offsetof(PhotoStarInstance, trailEnd),   3, sizeof(PhotoStarInstance));
	prog.enableAttributeArray(photoStarShaderVars.center);
	prog.enableAttributeArray(photoStarShaderVars.color);
	prog.enableAttributeArray(photoStarShaderVars.radius);
	prog.enableAttributeArray(photoStarShaderVars.shape);
	prog.enableAttributeArray(photoStarShaderVars.sunDir);
	prog.enableAttributeArray(photoStarShaderVars.trail);
	prog.enableAttributeArray(photoStarShaderVars.trailStart);
	prog.enableAttributeArray(photoStarShaderVars.trailEnd);
	// One record per star (instance), not per vertex
	hg->glVertexAttribDivisor(static_cast<GLuint>(photoStarShaderVars.center), 1);
	hg->glVertexAttribDivisor(static_cast<GLuint>(photoStarShaderVars.color), 1);
	hg->glVertexAttribDivisor(static_cast<GLuint>(photoStarShaderVars.radius), 1);
	hg->glVertexAttribDivisor(static_cast<GLuint>(photoStarShaderVars.shape), 1);
	hg->glVertexAttribDivisor(static_cast<GLuint>(photoStarShaderVars.sunDir), 1);
	hg->glVertexAttribDivisor(static_cast<GLuint>(photoStarShaderVars.trail), 1);
	hg->glVertexAttribDivisor(static_cast<GLuint>(photoStarShaderVars.trailStart), 1);
	hg->glVertexAttribDivisor(static_cast<GLuint>(photoStarShaderVars.trailEnd), 1);
	photoVao->release();
	photoVbo->release();
}

void StelSkyDrawer::flushPhotoPointSources(StelPainter* sPainter, PhotoBatchMode mode)
{
	if (nbPhotoPointSources==0)
		return;
	if (!photoStarShaderProgram || !photoVao)
	{
		nbPhotoPointSources = 0;
		return;
	}

	sPainter->setBlending(true, GL_ONE, GL_ONE);
	const QMatrix4x4 qMat=sPainter->getProjector()->getProjectionMatrix().toQMatrix();

	photoVbo->bind();
	// Re-specify the whole buffer ("orphaning"): the driver gives us fresh memory instead of
	// waiting until the GPU has finished drawing the previous batch from it.
	photoVbo->allocate(photoInstances.data(), static_cast<int>(nbPhotoPointSources*sizeof(PhotoStarInstance)));
	photoVbo->release();

	auto& prog = *photoStarShaderProgram;
	prog.bind();
	prog.setUniformValue(photoStarShaderVars.projectionMatrix, qMat);
	prog.setUniformValue(photoStarShaderVars.coreAlpha, photoCoreAlpha);
	prog.setUniformValue(photoStarShaderVars.coreBeta, PHOTO_CORE_BETA);
	prog.setUniformValue(photoStarShaderVars.haloAlpha, photoCoreAlpha*PHOTO_HALO_WIDTH_FACTOR);
	prog.setUniformValue(photoStarShaderVars.haloBeta, PHOTO_HALO_BETA);
	prog.setUniformValue(photoStarShaderVars.haloFraction, (getFlagPhotoBloomActive() || mode == PhotoBatchCoreOnly) ? 0.f : photoHaloFraction);
	prog.setUniformValue(photoStarShaderVars.coreFraction, getPhotoCoreFraction());
	prog.setUniformValue(photoStarShaderVars.coreDiskTerm, photoDiskTerm(PHOTO_CORE_BETA));
	prog.setUniformValue(photoStarShaderVars.haloDiskTerm, photoDiskTerm(PHOTO_HALO_BETA));
	// With bloom, the halo and the inner part of the spikes come from the whole image (StelPhotoBloom)
	const int spikeLines = (getPhotoSpikeFractionEffective() > 0.f && mode != PhotoBatchCoreOnly) ? getPhotoSpikeLines() : 0;
	prog.setUniformValue(photoStarShaderVars.spikeLines, spikeLines);
	prog.setUniformValue(photoStarShaderVars.spikeFraction, spikeLines > 0 ? getPhotoSpikeFractionEffective() : 0.f);
	prog.setUniformValue(photoStarShaderVars.spikeSigma, photoCoreAlpha*PHOTO_SPIKE_WIDTH_FACTOR);
	prog.setUniformValue(photoStarShaderVars.spikeLength, getPhotoSpikeLength());
	QVector2D spikeDirs[PHOTO_MAX_SPIKE_LINES];
	for (int k=0; k<PHOTO_MAX_SPIKE_LINES; ++k)
	{
		const float a = PHOTO_SPIKE_ANGLE + (spikeLines > 0 ? static_cast<float>(k)*M_PIf/static_cast<float>(spikeLines) : 0.f);
		spikeDirs[k] = QVector2D(std::cos(a), std::sin(a));
	}
	prog.setUniformValueArray(photoStarShaderVars.spikeDirs, spikeDirs, PHOTO_MAX_SPIKE_LINES);
	prog.setUniformValue(photoStarShaderVars.visibleThreshold, photoVisibleThreshold);
	prog.setUniformValue(photoStarShaderVars.spikeStart, getFlagPhotoBloomActive() ? StelPhotoBloom::NEAR_SPIKE_LENGTH : 0.f);
	{
		const PhotoLens lens = getPhotoLens(*sPainter->getProjector());
		// The sprites are positioned in window coordinates (with the viewport offset)
		prog.setUniformValue(photoStarShaderVars.lensCenter, QVector2D(lens.center[0] + static_cast<float>(sPainter->getProjector()->getViewportPosX()),
		                                                                lens.center[1] + static_cast<float>(sPainter->getProjector()->getViewportPosY())));
		prog.setUniformValue(photoStarShaderVars.lensCorner, lens.corner);
		prog.setUniformValue(photoStarShaderVars.comaLength, lens.coma);
		prog.setUniformValue(photoStarShaderVars.astigLength, lens.astigmatism);
		prog.setUniformValue("chromaEnabled", lens.chromaShift[0] != 0.f || lens.chromaBlur[2] != 0.f);
		prog.setUniformValue("chromaShift", QVector3D(lens.chromaShift[0], lens.chromaShift[1], lens.chromaShift[2]));
		prog.setUniformValue("chromaBlur", QVector3D(lens.chromaBlur[0], lens.chromaBlur[1], lens.chromaBlur[2]));
		// Defocus: the image of the aperture. Focused nearer than infinity, the rays of a star cross in front
		// of the sensor: the image is inverted, and (spherical aberration of a typical, undercorrected lens)
		// soft-edged; beyond infinity it is upright with a bright rim.
		prog.setUniformValue(photoStarShaderVars.defocusRadius, lens.defocus);
		prog.setUniformValue(photoStarShaderVars.defocusArea, getPhotoApertureShapeArea());
		prog.setUniformValue(photoStarShaderVars.defocusSign, lens.defocus > 0.f ? -1.f : 1.f);
		prog.setUniformValue(photoStarShaderVars.defocusSA, lens.defocus > 0.f ? -lens.sphericalAberration : lens.sphericalAberration);
		prog.setUniformValue(photoStarShaderVars.defocusEdge, photoDefocusEdge());
		prog.setUniformValue(photoStarShaderVars.coreFwhm, photoCoreFwhm());
		const int blades = photoApertureBlades >= 3 ? photoApertureBlades : 0;
		const float sector = blades > 0 ? 2.f*M_PIf/static_cast<float>(blades) : 0.f;
		prog.setUniformValue(photoStarShaderVars.apertureBlades, blades);
		prog.setUniformValue(photoStarShaderVars.apertureStraightness, blades > 0 ? getPhotoApertureStraightness() : 0.f);
		// the same orientation as the spikes and the lens reflections: one blade edge normal at PHOTO_SPIKE_ANGLE
		prog.setUniformValue(photoStarShaderVars.apertureRotation, PHOTO_SPIKE_ANGLE - 0.5f*sector);
		prog.setUniformValue(photoStarShaderVars.lensPupil, lens.pupil);
		prog.setUniformValue(photoStarShaderVars.lensShift, PHOTO_VIGNETTING_SHIFT);
		prog.setUniformValue(photoStarShaderVars.lensVignetting, lens.vignetting ? 1.f : 0.f);
	}

	photoVao->bind();
	StelOpenGL::highGraphicsFunctions()->glDrawArraysInstanced(GL_TRIANGLES, 0, 6, static_cast<GLsizei>(nbPhotoPointSources));
	photoVao->release();
	prog.release();

	nbPhotoPointSources = 0;
}

void StelSkyDrawer::setupCurrentVAO()
{
	vbo->bind();
	starShaderProgram->setAttributeBuffer(starShaderVars.pos, GL_FLOAT, 0, 2, sizeof(StarVertex));
	starShaderProgram->setAttributeBuffer(starShaderVars.color, GL_UNSIGNED_BYTE, offsetof(StarVertex,color), 3, sizeof(StarVertex));
	starShaderProgram->setAttributeBuffer(starShaderVars.texCoord, GL_UNSIGNED_BYTE, maxPointSources*6*sizeof(StarVertex), 2, 0);
	vbo->release();
	starShaderProgram->enableAttributeArray(starShaderVars.pos);
	starShaderProgram->enableAttributeArray(starShaderVars.color);
	starShaderProgram->enableAttributeArray(starShaderVars.texCoord);
}

void StelSkyDrawer::bindVAO()
{
	if(vao->isCreated())
		vao->bind();
	else
		setupCurrentVAO();
}

void StelSkyDrawer::releaseVAO()
{
	if(vao->isCreated())
	{
		vao->release();
	}
	else
	{
		starShaderProgram->disableAttributeArray(starShaderVars.pos);
		starShaderProgram->disableAttributeArray(starShaderVars.color);
		starShaderProgram->disableAttributeArray(starShaderVars.texCoord);
	}
}

void StelSkyDrawer::update(double)
{
	float fov = static_cast<float>(core->getMovementMgr()->getCurrentFov());
	if (fov > maxAdaptFov)
	{
		fov = maxAdaptFov;
	}
	else
	{
		if (fov < minAdaptFov)
			fov = minAdaptFov;
	}

	if (getFlagHasAtmosphere() && core->getJD()>2387627.5) // JD given is J1825.0; ignore Bortle scale index before that.
	{
        // GZ: Light pollution must take global atmosphere setting into account!
        // moved parts from setBortleScale() here
        // This formula is a fit to a set of values calibrated by hand, looking at the faintest star in stellarium at around 40 deg FOV.
        // It should roughly match the scale described at http://en.wikipedia.org/wiki/Bortle_Dark-Sky_Scale
		const auto nelm = StelCore::luminanceToNELM(lightPollutionLuminance);
		setInputScale(3.3541f*std::exp(-0.404f*(16.5f-2*nelm)));
	}
	else
	    setInputScale(2.45f);

	// This factor is fully arbitrary. It corresponds to the collecting area x exposure time of the instrument
	// It is based on a power law, so that it varies progressively with the FOV to smoothly switch from human
	// vision to binocculares/telescope. Use a max of 0.7 because after that the atmosphere starts to glow too much!
	float powFactor = std::pow(60.f/qMax(0.7f,fov), 0.8f);
	eye->setInputScale(inScale*powFactor);

	// Set the fov factor for point source luminance computation
	// the division by powFactor should in principle not be here, but it doesn't look nice if removed
	lnfovFactor = std::log(1.f/50.f*2025000.f* 60.f*60.f / (fov*fov) / (EYE_RESOLUTION*EYE_RESOLUTION)/powFactor/1.4f);

	// Precompute
	starLinearScale = static_cast<float>(std::pow(35.*2.0*starAbsoluteScaleF, 1.40*0.5*starRelativeScale));

	// Photographic mode: everything a star needs that depends on the frame, not on the star.
	// With "screen pixel = sensor pixel", a star with illuminance E (lux) records as much light
	// in total as an extended source of luminance E/omega records in one pixel, where omega is
	// the solid angle of a pixel. Focal length and f-number cancel out.
	{
		const double pixPerRad = core->getProjection(StelCore::FrameJ2000)->getPixelPerRadAtCenter();
		const double omegaPixel = 1.0/(pixPerRad*pixPerRad); // sr
		constexpr double illuminanceMag0 = 2.54e-6; // lux, V band: 10^(-0.4*13.99)
		photoFluxMag0 = static_cast<float>(getPhotoSceneScale() * illuminanceMag0 / omegaPixel);
		// PSF width: the sensor/optics minimum combined with the seeing (which matters when zoomed in)
		const double seeingPixels = photoSeeingArcsec * (M_PI/180./3600.) * pixPerRad;
		const double fwhm = std::sqrt(static_cast<double>(photoMinFwhmPixels)*photoMinFwhmPixels + seeingPixels*seeingPixels);
		photoCoreAlpha = static_cast<float>(fwhm / (2.0*std::sqrt(std::pow(2.0, 1.0/PHOTO_CORE_BETA) - 1.0)));
		// The faintest visible light is half a step of an 8-bit display (less cannot change a pixel), measured at
		// the level of the sky background. In the darkest tones sRGB is linear, so a step is
		// 1/(255*12.92); above that it grows with the sky level (derivative of the sRGB curve).
		// Stars below this would not change a single pixel on the screen, so they are not drawn.
		// Use half the average sky luminance as a conservative estimate (zenith is darker).
		const auto* lmgr = qobject_cast<LandscapeMgr*>(StelApp::getInstance().getModuleMgr().getModule("LandscapeMgr", true));
		const float exposure = getPhotoExposureFactor();
		const float skyLuminance = (lmgr && lmgr->getFlagAtmosphere()) ? 0.5f*lmgr->getAtmosphereAverageLuminance() : 0.f;
		photoSkyBackground = skyLuminance * getPhotoSceneScale();
		// Half a step of the display above the sky, through the tone curve and the sRGB encoding. With a film, the
		// reciprocity failure of each point comes before the curve (see getPhotoReciprocityParams())
		const float skyOnScreen = photoSkyBackground * exposure;
		const float skyDisplay = photoToneCurveValue(photoFilmRecordedExposure(skyOnScreen));
		const float skyEncoded = skyDisplay <= 0.0031308f ? 12.92f*skyDisplay : 1.055f*std::pow(skyDisplay, 1.f/2.4f) - 0.055f;
		const float nextDisplay = srgbToLinear(qMin(skyEncoded + 0.5f/255.f, 1.f));
		const float nextOnScreen = photoFilmExposureForRecorded(photoToneCurveInverse(nextDisplay), skyOnScreen);
		// A saturated (white) sky hides everything
		photoVisibleThreshold = (nextOnScreen > skyOnScreen) ? (nextOnScreen - skyOnScreen) / exposure : 1e30f;
		// The noise of the sky in a pixel of the picture, whether the noise is shown or not: a real sensor
		// always has it, and a star that does not rise above it is not in the picture. A pixel of the screen
		// holds several photosites (the view is the whole sensor), which average their noise.
		if (getFlagPhotoFilmActive())
		{
			// Film: the grain. The data sheets give the RMS density fluctuation sigma_D in a 48 micrometer aperture
			// at density 1; it grows as the square root of the density (see getPhotoGrainParams()). Through the
			// curve of the film (dD = gamma dlog10 E) it is a relative fluctuation of the exposure
			// ln(10) sigma_D / gamma, smaller in a larger area (the square root of the areas). The level is the
			// sky, but at least the toe of the curve (4 stops below middle grey): fainter light hardly blackens
			// the film at all (its inertia), and a star must stand out of the grain there.
			const PhotoFilmSpec& f = getPhotoActiveFilmSpec();
			const StelProjectorP prj = core->getProjection(StelCore::FrameJ2000);
			const double diagonal = getPhotoFrameDiagonal(*prj);
			const double pixel = diagonal > 0. ? getPhotoSensorDiagonal()/diagonal : 6e-6;  // on the film, meters
			const double aperture = M_PI*24e-6*24e-6;
			const double relative = std::log(10.)*f.granularity/1000./f.gamma * std::sqrt(aperture/(pixel*pixel));
			const double level = qMax(static_cast<double>(photoSkyBackground), 0.18/16.);
			// The density of the film at the sky: a slide is densest in the dark sky, a negative there thin. With the
			// reciprocity failure per point the film records xr = f(x): the curve is steeper by k = dln f/dln x (1.3 for
			// Provia 400F in a faint sky), and the grain smaller by that factor, measured in exposure
			double density;
			const double x = level*static_cast<double>(exposure);
			const double xr = static_cast<double>(photoFilmRecordedExposure(static_cast<float>(x)));
			const double k = qMax(0.2, std::log(static_cast<double>(photoFilmRecordedExposure(static_cast<float>(1.05*x)))/qMax(1e-30, xr))/std::log(1.05));
			if (f.type == PhotoFilmSlide)
			{
				const double y = qMax(static_cast<double>(photoToneCurveValue(static_cast<float>(xr))),
				                      std::pow(10., static_cast<double>(PHOTO_SLIDE_BASE_DENSITY - PHOTO_SLIDE_MAX_DENSITY)));
				density = static_cast<double>(PHOTO_SLIDE_BASE_DENSITY) - std::log10(y);
			}
			else
			{
				const double toe = static_cast<double>(f.toneMid)/16.;
				density = static_cast<double>(PHOTO_NEGATIVE_FOG_DENSITY) + f.gamma*std::log10(qMax(1., xr/toe));
			}
			photoNoiseSigma = static_cast<float>(relative*level*std::sqrt(density)/k);
		}
		else
		{
			const StelProjectorP prj = core->getProjection(StelCore::FrameJ2000);
			PhotoNoiseParams noise = getPhotoNoiseParams(*prj);
			const double diagonal = getPhotoFrameDiagonal(*prj);
			const double pixel = diagonal > 0. ? getPhotoSensorDiagonal()/diagonal : getPhotoSensorPitch();
			const float sites = static_cast<float>(qMax(1., (pixel*pixel)/(getPhotoSensorPitch()*getPhotoSensorPitch())));
			const float skyElectrons = photoSkyBackground * noise.white * sites;
			const float variance = skyElectrons + sites*(noise.read2 + noise.dark);
			photoNoiseSigma = std::sqrt(qMax(0.f, variance)) / (noise.white * sites);
		}
	}

	updatePhotoExtinction();
	// A new realization of the sensor noise every frame, as in the live view of a camera
	photoNoiseFrame = (photoNoiseFrame + 1u) % 4093u;
	{
		const StelProjectorP prj = core->getProjection(StelCore::FrameJ2000);
		photoLensCache = getPhotoLens(*prj);
		photoLensCache.center += Vec2f(static_cast<float>(prj->getViewportPosX()), static_cast<float>(prj->getViewportPosY()));
		photoLensAberrations = photoLensCache.coma > 0.f || photoLensCache.astigmatism > 0.f;

		// Star trails: during the exposure the sky turns around the rotation axis of the planet we stand on
		// (one turn per sidereal day). The exposure is centered on the current time. On a star tracker the
		// camera turns with the sky: then the ground moves instead (see beginPhotoGroundImage()).
		photoTrailsActive = false;
		photoGroundTrailActive = false;
		const auto observer = core->getCurrentObserver();
		const auto home = observer ? observer->getHomePlanet() : QSharedPointer<Planet>();
		if (flagPhotoStarTrails && getFlagPhotographicModeActive() && !home.isNull())
		{
			const double siderealDay = home->getSiderealDay(); // days
			const double t = getPhotoEffectiveExposureTime();
			if (std::isfinite(siderealDay) && std::fabs(siderealDay) > 1e-6 && t > 0.)
			{
				photoTrailAngle = 2.*M_PI*t/(std::fabs(siderealDay)*86400.);
				// Shorter than half a pixel in the image center: no trails
				if (photoTrailAngle*prj->getPixelPerRadAtCenter() > 0.5)
				{
					Vec3d pole = core->equinoxEquToJ2000(Vec3d(0., 0., 1.), StelCore::RefractionOff);
					core->getJ2000ModelViewTransform(StelCore::RefractionOff)->forward(pole);
					pole.normalize();
					photoTrailAxisView = pole;
					// The direction of the rotation does not matter: the trail is symmetric about mid-exposure
					photoTrailToStart = Mat4d::rotation(pole, 0.5*photoTrailAngle);
					photoTrailToEnd = Mat4d::rotation(pole, -0.5*photoTrailAngle);
					const bool tracker = getPhotoEffectiveMount() == PhotoMountTracker;
					photoTrailsActive = !tracker;
					photoGroundTrailActive = tracker;
					// Extinction along the trails (the altitude changes during the exposure)
					photoTrailExtinction = getFlagHasAtmosphere() && extinction.getExtinctionCoefficient() >= 0.01f;
					photoTrailAltAzTransform = core->getAltAzModelViewTransform(StelCore::RefractionAuto);
				}
			}
		}
	}

	// update limit mag
	limitMagnitude = computeLimitMagnitude();

	// update limit luminance
	limitLuminance = computeLimitLuminance();
}

// Compute the current limit magnitude by dichotomy
float StelSkyDrawer::computeLimitMagnitude() const
{
	float a=-26.f;
	float b=30.f;
	RCMag rcmag;
	float lim = 0.f;
	int safety=0;
	while (std::fabs(lim-a)>0.05f)
	{
		// The limit magnitude decides which labels, hints and faint objects are shown. It keeps
		// following the eye model in the photographic mode too, so that labels stay readable.
		computeEyeRCMag(lim, &rcmag);
		float tmp = lim;
		if (rcmag.radius<=0.f)
		{
			lim=(a+lim)*0.5f;
			b=tmp;
		}
		else
		{
			lim=(b+lim)*0.5f;
			a=tmp;
		}
		++safety;
		if (safety>20)
		{
			lim=-99;
			break;
		}
	}
	return lim;
}

// Compute the current limit luminance by dichotomy
float StelSkyDrawer::computeLimitLuminance() const
{
	float a=0.f;
	float b=500000.f;
	float lim=40.f;
	int safety=0;
	while (std::fabs(lim-a)>0.05f)
	{
		float adaptL = eye->adaptLuminanceScaled(lim);
		if (adaptL<=0.05f) // Object considered not visible if its adapted scaled luminance<0.05
		{
			float tmp = lim;
			lim=(b+lim)*0.5f;
			a=tmp;
		}
		else
		{
			float tmp = lim;
			lim=(a+lim)*0.5f;
			b=tmp;
		}
		++safety;
		if (safety>30)
		{
			lim=500000;
			break;
		}
	}
	return lim;
}

// Compute the ln of the luminance for a point source with the given mag for the current FOV
float StelSkyDrawer::pointSourceMagToLnLuminance(float mag) const
{
	return -0.92103f*(mag + 12.12331f) + lnfovFactor;
}

float StelSkyDrawer::pointSourceLuminanceToMag(float lum) const
{
	return (std::log(lum) - lnfovFactor)/-0.92103f - 12.12331f;
}

// Compute the luminance for an extended source with the given surface brightness in Vmag/arcmin^2
float StelSkyDrawer::surfaceBrightnessToLuminance(float sb)
{
	return 2.f*2025000.f*std::exp(-0.92103f*(sb + 12.12331f))/(1.f/60.f*1.f/60.f);
}

// Compute the surface brightness from the luminance of an extended source
float StelSkyDrawer::luminanceToSurfacebrightness(float lum)
{
	return std::log(lum*(1.f/60.f*1.f/60.f)/(2.f*2025000.f))/-0.92103f - 12.12331f;
}

// Compute RMag and CMag from magnitude for a point source.
bool StelSkyDrawer::computePhotoRCMag(float mag, RCMag* rcMag) const
{
	// Total recorded light of the star (sum over all pixels, scene units: middle grey 0.18, the sensor saturates
	// at PHOTO_SENSOR_SATURATION)
	const float flux = photoFluxMag0 * std::pow(10.f, -0.4f*mag);
	// Radius where a Moffat component of the given total light drops to the visibility threshold
	auto radiusAtThreshold = [this](float componentFlux, float alpha, float beta) -> float
	{
		const float peak = componentFlux * (beta-1.f) / (M_PIf*alpha*alpha);
		if (!(peak > photoVisibleThreshold))
			return 0.f;
		return alpha * std::sqrt(std::pow(peak/photoVisibleThreshold, 1.f/beta) - 1.f);
	};
	const float spikeFraction = getPhotoSpikeFractionEffective();
	// Out of focus, the light of the core is spread over the blur disk (the image of the aperture):
	// faint stars disappear, as on a real photo
	const float defocusR = std::fabs(photoLensCache.defocus);
	const float coreFlux = flux*(1.f-photoHaloFraction-spikeFraction);
	const float rCore = defocusR > 0.f ? photoDefocusCoreRadius(coreFlux, defocusR)
	                                   : radiusAtThreshold(coreFlux, photoCoreAlpha, PHOTO_CORE_BETA);
	// It must also rise above the noise of the sky: signal-to-noise ratio over its image (matched filter: the
	// noise of the effective area of the PSF, pi alpha^2 (2 beta - 1)/(beta - 1)^2 for a Moffat profile, or of
	// the blur disk). The faint stars drowned in the noise are not in the picture, and the long lists of them
	// are not drawn. This limit grows with the focal length (a larger aperture at the same f-number collects
	// more light of a star, the sky per pixel stays the same) and the exposure, and drops in a bright sky.
	const float effectiveArea = M_PIf*photoCoreAlpha*photoCoreAlpha*(2.f*PHOTO_CORE_BETA - 1.f)/((PHOTO_CORE_BETA - 1.f)*(PHOTO_CORE_BETA - 1.f))
	                            + M_PIf*defocusR*defocusR;
	const bool detected = coreFlux > photoDetectionSnr * photoNoiseSigma * std::sqrt(effectiveArea);
	if (rCore <= 0.f || !detected)
	{
		rcMag->radius = 0.f;
		rcMag->luminance = 0.f;
		return false;
	}
	// The halo spreads around the blur disk (see the shader): the profile at max(0, r - R), renormalized
	float rHalo = 0.f;
	if (!getFlagPhotoBloomActive())
	{
		const float alpha = photoCoreAlpha*PHOTO_HALO_WIDTH_FACTOR;
		const float norm = 1.f/(1.f + photoDiskTerm(PHOTO_HALO_BETA)*defocusR/alpha + (PHOTO_HALO_BETA-1.f)*defocusR*defocusR/(alpha*alpha));
		const float r = radiusAtThreshold(flux*photoHaloFraction*norm, alpha, PHOTO_HALO_BETA);
		if (r > 0.f)
			rHalo = r + defocusR;
	}
	// Length of the diffraction spikes: where the intensity along a spike drops to the threshold
	float rSpike = 0.f;
	if (spikeFraction > 0.f)
	{
		const float lines = static_cast<float>(getPhotoSpikeLines());
		const float sigma = photoCoreAlpha*PHOTO_SPIKE_WIDTH_FACTOR;
		const float length = getPhotoSpikeLength();
		const float s1 = std::sqrt(2.f*M_PIf)*sigma;
		const float s2 = M_PIf*length;
		const float norm = (s1*s2)/((s1+2.f*defocusR)*(s2+2.f*defocusR)); // a band as wide as the blur disk
		const float peak = flux*spikeFraction*norm / (lines * s1 * s2);
		if (peak > photoVisibleThreshold)
			rSpike = defocusR + length * std::sqrt(peak/photoVisibleThreshold - 1.f);
		// With bloom, the inner part of the spikes comes from the image
		if (getFlagPhotoBloomActive() && rSpike <= StelPhotoBloom::NEAR_SPIKE_LENGTH)
			rSpike = 0.f;
	}
	// The sprite ends where the profile fades out; leave room for that fade (see the shader)
	rcMag->radius = qMin(PHOTO_MAX_SPRITE_RADIUS, qMax(qMax(qMax(rCore, rHalo), rSpike)/0.85f, 1.5f*photoCoreAlpha));
	rcMag->luminance = flux;
	return true;
}

bool StelSkyDrawer::computeRCMag(float mag, RCMag* rcMag) const
{
	if (getFlagPhotographicModeActive())
		return computePhotoRCMag(mag, rcMag);
	return computeEyeRCMag(mag, rcMag);
}

bool StelSkyDrawer::computeEyeRCMag(float mag, RCMag* rcMag) const
{
	rcMag->radius = eye->adaptLuminanceScaledLn(pointSourceMagToLnLuminance(mag), static_cast<float>(starRelativeScale)*1.40f*0.5f);
	rcMag->radius *=starLinearScale;
	// Use now statically min_rmag = 0.5, because higher and too small values look bad
	if (rcMag->radius < 0.3f)
	{
		rcMag->radius = 0.f;
		rcMag->luminance = 0.f;
		return false;
	}

	// if size of star is too small (blink) we put its size to 1.2 --> no more blink
	// And we compensate the difference of brighteness with cmag
	if (rcMag->radius<1.2f)
	{
		rcMag->luminance= rcMag->radius * rcMag->radius * rcMag->radius / 1.728f;
		if (rcMag->luminance < 0.05f)
		{
			rcMag->radius = rcMag->luminance = 0.f;
			return false;
		}
		rcMag->radius = 1.2f;
	}
	else
	{
		// cmag:
		rcMag->luminance = 1.0f;
		if (rcMag->radius>MAX_LINEAR_RADIUS)
		{
			rcMag->radius=MAX_LINEAR_RADIUS+std::sqrt(1.f+rcMag->radius-MAX_LINEAR_RADIUS)-1.f;
		}
	}
	rcMag->radius *= StelApp::getInstance().getScreenScale();
	return true;
}

void StelSkyDrawer::preDrawPointSource(StelPainter* p)
{
	Q_ASSERT(p);
	Q_ASSERT(nbPointSources==0);

	// Blending is really important. Otherwise faint stars in the vicinity of
	// bright star will cause tiny black squares on the bright star, e.g. see Procyon.
	p->setBlending(true, GL_ONE, GL_ONE);
}

// Finalize the drawing of point sources
void StelSkyDrawer::postDrawPointSource(StelPainter* sPainter, bool drawInCorona)
{
	Q_ASSERT(sPainter);

	flushPhotoPointSources(sPainter);
	if (nbPointSources==0)
		return;
	if (drawInCorona)
	{
		// Test whether this call is misplaced. TODO: Cleanup in summer of 2026.
		Q_ASSERT(0);
	}
	if (flagStarSpiky)
		texHaloRayed->bind();
	else
		texHalo->bind();
	sPainter->setBlending(true, GL_ONE, GL_ONE);

	const QMatrix4x4 qMat=sPainter->getProjector()->getProjectionMatrix().toQMatrix();

	vbo->bind();
	vbo->write(0, vertexArray, nbPointSources*6*sizeof(StarVertex));
	vbo->write(maxPointSources*6*sizeof(StarVertex), textureCoordArray, nbPointSources*6*2);
	vbo->release();

	starShaderProgram->bind();
	starShaderProgram->setUniformValue(starShaderVars.projectionMatrix, qMat);
	
	bindVAO();
	glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(nbPointSources)*6);
	releaseVAO();

	starShaderProgram->release();
	
	nbPointSources = 0;
}

// Draw a point source halo.
bool StelSkyDrawer::drawPhotoPointSource(StelPainter* sPainter, const Vec3d& v, const RCMag& rcMag, const Vec3f& linearUnitColor, float dim, bool checkInScreen)
{
	if (photoInstances.empty())
		return false;
	if (!(rcMag.luminance > 0.f) || !(rcMag.radius > 0.f) || !(dim > 0.f))
		return false;
	Vec3d win;
	if (!(checkInScreen ? sPainter->getProjector()->projectCheck(v, win) : sPainter->getProjector()->project(v, win)))
		return false;

	// No twinkling: a photo integrates over the exposure
	PhotoStarInstance& star = photoInstances[nbPhotoPointSources];
	star.center.set(static_cast<float>(win[0]), static_cast<float>(win[1]));
	star.color = linearUnitColor * (rcMag.luminance * dim);
	star.radius = rcMag.radius;
	if (photoLensAberrations)
	{
		// Room for the coma and the astigmatism of the lens (see aberratedCore() in the shader)
		const float rho = std::hypot(star.center[0]-photoLensCache.center[0], star.center[1]-photoLensCache.center[1]) / photoLensCache.corner;
		star.radius += 2.5f*photoLensCache.coma*rho + 0.7f*photoLensCache.astigmatism*rho*rho;
	}
	star.shape.set(0.f, 1.f, 0.f, 1.f);
	star.sunDir.set(1.f, 0.f);
	// Star trail (the sprite box around it is made in the shader)
	if (computePhotoTrail(*sPainter->getProjector(), v, win, rcMag.luminance*dim, 0.f, star) < 0.f)
		return false;
	++nbPhotoPointSources;
	if (nbPhotoPointSources>=maxPhotoPointSources)
		flushPhotoPointSources(sPainter);
	return true;
}

float StelSkyDrawer::getPhotoCoreWeight(float diskPixRadius) const
{
	// Same limits as the classic halo of the planets: below 3 pixels the body looks like a star
	const float scale = StelApp::getInstance().getScreenScale();
	const float tStart = 3.f*scale;
	const float tStop = 6.f*scale;
	return qBound(0.f, (tStop-diskPixRadius)/(tStop-tStart), 1.f);
}

float StelSkyDrawer::getPhotoDiskFraction(float diskPixRadius) const
{
	const float spikeFraction = getPhotoSpikeFractionEffective();
	return (1.f - photoHaloFraction - spikeFraction) * (1.f - getPhotoCoreWeight(diskPixRadius));
}

bool StelSkyDrawer::drawPhotoBody(StelPainter* sPainter, const Vec3d& v, float diskPixRadius, float mag, const Vec3f& color,
                                  float illuminatedFraction, const Vec3d& sunDirection)
{
	if (photoInstances.empty() || !photoStarShaderProgram)
		return false;
	const float dim = qMax(color[0], qMax(color[1], color[2]));
	if (!(dim > 0.f))
		return false;
	const Vec3f linear = srgbToLinear(color/dim);
	const float Y = 0.2126f*linear[0] + 0.7152f*linear[1] + 0.0722f*linear[2];
	if (!(Y > 0.f))
		return false;

	// Total recorded light of the body (sum over all pixels, linear units)
	const float flux = photoFluxMag0 * std::pow(10.f, -0.4f*mag);
	if (!(flux > 0.f))
		return false;
	const float diskR = qMax(0.f, diskPixRadius);
	const float coreWeight = getPhotoCoreWeight(diskR);
	// A body in phases: its light comes from the lit part. For the normalization, that part counts
	// as a disk of the same area; the shader measures the distance from the true lit shape.
	const float k = qBound(0.f, illuminatedFraction, 1.f);
	const float Rlit = diskR * std::sqrt(k);
	const float spikeFraction = getPhotoSpikeFractionEffective();
	// Out of focus, a body smaller than the blur disk becomes an image of the aperture, like a star
	// (the shader makes the same choice). Its halo and spikes then spread around the blur disk.
	const float defocusR = std::fabs(photoLensCache.defocus);
	const bool defocused = defocusR > 0.f && diskR < defocusR;
	const float R = defocused ? std::sqrt(defocusR*defocusR + Rlit*Rlit) : Rlit;

	// How far beyond the disk edge each component stays visible (same as for stars, with the
	// normalization of a profile spread around the disk)
	auto wingRadius = [this, R](float componentFlux, float alpha, float beta) -> float
	{
		const float norm = 1.f/(1.f + photoDiskTerm(beta)*R/alpha + (beta-1.f)*R*R/(alpha*alpha));
		const float peak = componentFlux * norm * (beta-1.f) / (M_PIf*alpha*alpha);
		if (!(peak > photoVisibleThreshold))
			return 0.f;
		return alpha * std::sqrt(std::pow(peak/photoVisibleThreshold, 1.f/beta) - 1.f);
	};
	const float rCore = defocused ? photoDefocusCoreRadius(flux*(1.f-photoHaloFraction-spikeFraction)*coreWeight, R)
	                              : wingRadius(flux*(1.f-photoHaloFraction-spikeFraction)*coreWeight, photoCoreAlpha, PHOTO_CORE_BETA);
	const float rHalo = getFlagPhotoBloomActive() ? 0.f : wingRadius(flux*photoHaloFraction, photoCoreAlpha*PHOTO_HALO_WIDTH_FACTOR, PHOTO_HALO_BETA);
	float rSpike = 0.f;
	if (spikeFraction > 0.f)
	{
		const float lines = static_cast<float>(getPhotoSpikeLines());
		const float sigma = photoCoreAlpha*PHOTO_SPIKE_WIDTH_FACTOR;
		const float length = getPhotoSpikeLength();
		const float s1 = std::sqrt(2.f*M_PIf)*sigma;
		const float s2 = M_PIf*length;
		const float norm = (s1*s2)/((s1+2.f*R)*(s2+2.f*R));
		const float peak = flux*spikeFraction*norm / (lines * s1 * s2);
		if (peak > photoVisibleThreshold)
			rSpike = length * std::sqrt(peak/photoVisibleThreshold - 1.f);
		// With bloom, the inner part of the spikes comes from the image
		if (getFlagPhotoBloomActive() && rSpike <= StelPhotoBloom::NEAR_SPIKE_LENGTH)
			rSpike = 0.f;
	}
	// Defocused: the halo and the spikes start at the blur disk (the core radius already includes it)
	const float wingOffset = defocused ? R : 0.f;
	const float wing = qMax(qMax(rCore, rHalo > 0.f ? rHalo + wingOffset : 0.f), rSpike > 0.f ? rSpike + wingOffset : 0.f);
	if (!(wing > 0.f))
		return false;
	// The spikes of the Sun can cross the whole screen
	const auto& prj = sPainter->getProjector();
	const float maxRadius = qMax(PHOTO_MAX_SPRITE_RADIUS, 1.5f*static_cast<float>(std::hypot(prj->getViewportWidth(), prj->getViewportHeight())));
	const float radius = qMin(diskR + maxRadius, diskR + qMax(wing/0.85f, 1.5f*photoCoreAlpha));

	// Only a body imaged on the sensor (its disk at least partly in the frame) produces this image
	Vec3d win;
	if (!prj->projectCheck(v, win))
	{
		if (!prj->project(v, win))
			return false;
		const double margin = static_cast<double>(diskR) + 2.;
		if (win[0] < prj->getViewportPosX() - margin || win[0] > prj->getViewportPosX() + prj->getViewportWidth() + margin ||
		    win[1] < prj->getViewportPosY() - margin || win[1] > prj->getViewportPosY() + prj->getViewportHeight() + margin)
			return false;
	}

	// Direction towards the Sun on the screen
	Vec2f sunDir(1.f, 0.f);
	if (k < 1.f && sunDirection.normSquared() > 0.)
	{
		const Vec3d vn = v/v.norm();
		Vec3d winSun;
		if (prj->project(vn + sunDirection/sunDirection.norm()*1e-4, winSun))
		{
			Vec2d dir(winSun[0]-win[0], winSun[1]-win[1]);
			const double len = dir.norm();
			if (len > 0.)
				sunDir.set(static_cast<float>(dir[0]/len), static_cast<float>(dir[1]/len));
		}
	}
	// Illuminated fraction k = (1 + cos(phase angle))/2
	const float cosPhase = 2.f*k - 1.f;

	flushPhotoPointSources(sPainter); // keep the order of drawing
	PhotoStarInstance& body = photoInstances[nbPhotoPointSources];
	body.center.set(static_cast<float>(win[0]), static_cast<float>(win[1]));
	body.color = (linear/Y) * (flux*dim);
	body.radius = radius;
	body.shape.set(diskR, coreWeight, Rlit, cosPhase);
	body.sunDir = sunDir;
	// A trail only for a body whose disk is smaller than the trail: it is drawn like a star then
	if (computePhotoTrail(*prj, v, win, flux*dim, diskR, body) < 0.f)
		return false;
	// The PSF core here, in the order of drawing: nearer bodies (the Moon) and the landscape can cover it.
	// The halo and the spikes arise in the lens and lie over everything: they are drawn at the end.
	PhotoStarInstance wings = body;
	wings.shape[1] = 0.f; // no core
	++nbPhotoPointSources;
	flushPhotoPointSources(sPainter, PhotoBatchCoreOnly);
	if (rHalo > 0.f || rSpike > 0.f)
		photoDeferredBodies.push_back({wings, v, photoBodyMotion});
	return true;
}

void StelSkyDrawer::drawDeferredPhotoBodies(StelCore* core)
{
	if (!getFlagPhotographicModeActive() || photoInstances.empty())
	{
		photoDeferredBodies.clear();
		photoGhostSources.clear();
		return;
	}
	const StelProjectorP prj = core->getProjection(StelCore::FrameJ2000);
	if (!photoDeferredBodies.empty())
	{
		StelPainter sPainter(prj);
		for (const auto& body : photoDeferredBodies)
		{
			const Vec4f& trail = body.instance.trail;
			// A body larger than its trail is drawn without one (the shader trails point-like images only):
			// its halo and spikes are trailed as an image of their own then
			const bool trailImage = (photoTrailsActive || photoGroundTrailActive) && trail[0] == 0.f && trail[1] == 0.f && trail[2] == 0.f && trail[3] == 0.f;
			if (trailImage)
			{
				flushPhotoPointSources(&sPainter, PhotoBatchWingsOnly);
				photoBodyMotion = body.motion;
				const bool image = beginPhotoBodyImage(body.posJ2000, body.instance.radius, true);
				photoBodyMotion = Vec3d(0.);
				if (image)
				{
					photoInstances[nbPhotoPointSources++] = body.instance;
					flushPhotoPointSources(&sPainter, PhotoBatchWingsOnly);
					endPhotoBodyImage();
					continue;
				}
			}
			photoInstances[nbPhotoPointSources++] = body.instance;
			if (nbPhotoPointSources >= maxPhotoPointSources)
				flushPhotoPointSources(&sPainter, PhotoBatchWingsOnly);
		}
		flushPhotoPointSources(&sPainter, PhotoBatchWingsOnly);
		photoDeferredBodies.clear();
	}

	// Lens reflections of the bright bodies, also of those outside the frame
	if (!photoGhostSources.empty())
	{
		std::vector<StelPhotoLensFlare::Source> sources;
		for (const auto& g : photoGhostSources)
		{
			const float dim = qMax(g.color[0], qMax(g.color[1], g.color[2]));
			if (!(dim > 0.f))
				continue;
			const Vec3f linear = srgbToLinear(g.color/dim);
			const float Y = 0.2126f*linear[0] + 0.7152f*linear[1] + 0.0722f*linear[2];
			const float flux = photoFluxMag0 * std::pow(10.f, -0.4f*g.mag) * dim;
			Vec3d win;
			if (!(Y > 0.f) || !(flux > 0.f) || !prj->project(g.v, win))
				continue;
			// in front of the camera only (a perspective projection maps the back half too)
			Vec3d view = g.v;
			prj->getModelViewTransform()->forward(view);
			if (view[2] >= 0.)
				continue;
			sources.push_back({Vec2f(static_cast<float>(win[0]), static_cast<float>(win[1])), (linear/Y)*flux});
		}
		photoGhostSources.clear();
		if (!sources.empty())
		{
			if (!photoLensFlare)
				photoLensFlare.reset(new StelPhotoLensFlare);
			StelPhotoLensFlare::Camera camera;
			camera.center = photoLensCache.center;
			camera.focal = qMax(1.f, prj->getPixelPerRadAtCenter());
			camera.corner = photoLensCache.corner;
			camera.fNumber = static_cast<float>(photoAperture);
			camera.blades = photoApertureBlades;
			camera.straightness = photoApertureBlades > 0 ? getPhotoApertureStraightness() : 0.f;
			camera.bladeAngle = PHOTO_SPIKE_ANGLE;
			camera.strength = photoLensFlareStrength;
			camera.visibleThreshold = photoVisibleThreshold;
			photoLensFlare->draw(sources, camera, prj->getProjectionMatrix());
		}
	}
}

bool StelSkyDrawer::drawPointSource(StelPainter* sPainter, const Vec3d& v, const RCMag& rcMag, const Vec3f& color, bool checkInScreen, float twinkleFactor)
{
	Q_ASSERT(sPainter);

	if (getFlagPhotographicModeActive())
	{
		// Colors arrive as display (sRGB) colors normalized to a maximum channel of 1.
		// A smaller maximum means the caller dimmed the source (e.g. a fading satellite).
		const float dim = qMax(color[0], qMax(color[1], color[2]));
		if (!(dim > 0.f))
			return false;
		const Vec3f linear = srgbToLinear(color/dim);
		const float Y = 0.2126f*linear[0] + 0.7152f*linear[1] + 0.0722f*linear[2];
		if (!(Y > 0.f))
			return false;
		return drawPhotoPointSource(sPainter, v, rcMag, linear/Y, dim, checkInScreen);
	}

	if (rcMag.radius<=0.f)
		return false;

	Vec3d win;
	if (!(checkInScreen ? sPainter->getProjector()->projectCheck(v, win) : sPainter->getProjector()->project(v, win)))
		return false;

	const float radius = rcMag.radius;
	const float frand=StelApp::getInstance().getRandF();

	// Random coef for star twinkling. twinkleFactor can introduce height-dependent twinkling.
	const float tw = ((flagStarTwinkle && (flagHasAtmosphere || flagForcedTwinkle))) ? (1.f-twinkleFactor*static_cast<float>(twinkleAmount)*frand)*rcMag.luminance : rcMag.luminance;

	const float scale = StelApp::getInstance().getScreenScale();
	// If the rmag is big, draw a big halo
	const float bigHaloThresholdRadius = (MAX_LINEAR_RADIUS+5)*scale;
	if (flagDrawBigStarHalo && radius > bigHaloThresholdRadius)
	{
		const float cmag = qMin(1.0f, qMin(rcMag.luminance, (radius-bigHaloThresholdRadius)/30.f/scale));
		const float rmag = 150.f * scale;

		texBigHalo->bind();
		sPainter->setBlending(true, GL_ONE, GL_ONE);
		sPainter->setColor(color*cmag);
		sPainter->drawSprite2dModeNoDeviceScale(win[0], win[1], rmag);
	}

	const auto starColorVec = colorToShader(color*tw+Vec3f(0.5/255))*255;
	const uint8_t starColor[3] = {
		static_cast<uint8_t>(std::min(starColorVec[0], 255.f)),
		static_cast<uint8_t>(std::min(starColorVec[1], 255.f)),
		static_cast<uint8_t>(std::min(starColorVec[2], 255.f)),
	};
	
	// Store the drawing instructions in the vertex arrays
	StarVertex* vx = &(vertexArray[nbPointSources*6]);
	vx->pos.set(win[0]-radius,win[1]-radius); std::memcpy(vx->color, starColor, 3); ++vx;
	vx->pos.set(win[0]+radius,win[1]-radius); std::memcpy(vx->color, starColor, 3); ++vx;
	vx->pos.set(win[0]+radius,win[1]+radius); std::memcpy(vx->color, starColor, 3); ++vx;
	vx->pos.set(win[0]-radius,win[1]-radius); std::memcpy(vx->color, starColor, 3); ++vx;
	vx->pos.set(win[0]+radius,win[1]+radius); std::memcpy(vx->color, starColor, 3); ++vx;
	vx->pos.set(win[0]-radius,win[1]+radius); std::memcpy(vx->color, starColor, 3); ++vx;

	++nbPointSources;
	if (nbPointSources>=maxPointSources)
	{
		// Flush the buffer (draw all buffered stars)
		postDrawPointSource(sPainter);
	}
	return true;
}

// Draw's the Sun's corona during a solar eclipse on Earth.
// painter: in FrameJ2000
// posJ2000: solar position, J2000
// radius: angular radius of the sun, radians
// color: attenuated sunlight color (extinction may have reddened the sun)
// alpha: transparency
void StelSkyDrawer::drawSunCorona(StelPainter* painter, const Vec3d& posJ2000, double radius, const Vec3f& color, const float alpha)
{
	radius *= (512.f/193.f); // Texture size is 1024, solar radius within is 192 or 193. Increase radius to the width/height of actual image, in radians.
	double ra, dec;
	StelUtils::rectToSphe(&ra, &dec, posJ2000);

	// Define a rotation matrix around the sun that adjusts image rotation in the sky. We must unrotate from the original image orientation and adjust for the new angle of J2000 vs. ecliptic.
	// Our corona image was made in 2008-08-01 near Khovd, Mongolia. It shows the correct parallactic angle for its location and time, we must add this, and subtract the ecliptic/equatorial angle from that date of 15.43 degrees.
	// https://en.wikipedia.org/wiki/Rotation_matrix
	const double eclAngle=getPrecessionAngleVondrakCurrentEpsilonA()*cos(ra);
	const double theta=(44.65-15.43)*M_PI_180 - eclAngle; // dynamical rotation angle!
	const double cTh=cos(theta);
	const double mcTh=1.-cTh;
	const double sTh=sin(theta);
	const Mat3d R3(posJ2000[0]*posJ2000[0]*mcTh+cTh,             posJ2000[0]*posJ2000[1]*mcTh-posJ2000[2]*sTh, posJ2000[0]*posJ2000[2]*mcTh+posJ2000[1]*sTh,
	               posJ2000[0]*posJ2000[1]*mcTh+posJ2000[2]*sTh, posJ2000[1]*posJ2000[1]*mcTh+cTh,             posJ2000[1]*posJ2000[2]*mcTh-posJ2000[0]*sTh,
	               posJ2000[0]*posJ2000[2]*mcTh-posJ2000[1]*sTh, posJ2000[1]*posJ2000[2]*mcTh+posJ2000[0]*sTh, posJ2000[2]*posJ2000[2]*mcTh+cTh);

	static QVector<Vec3d> contour(coronaTextureCoords.size());
	contour.clear();
	const int centerPoint=coronaMeshDim/2; // point 5:2=2 is the point index in sun's center
	for (int j=-centerPoint;j<coronaMeshDim-1-centerPoint;++j)
	{
		for (int i=-centerPoint;i<coronaMeshDim-1-centerPoint;++i)
		{
			const double decJ0=dec+j    *(2.0*radius/(coronaMeshDim-1));
			const double decJ1=dec+(j+1)*(2.0*radius/(coronaMeshDim-1));
			const double cDecJ0=1./cos(decJ0);
			const double cDecJ1=1./cos(decJ1);
			const double dRA0=i*(2.0*radius/(coronaMeshDim-1));
			const double dRA1=(i+1)*(2.0*radius/(coronaMeshDim-1));

			Vec3d vertex;
			StelUtils::spheToRect(ra-dRA0*cDecJ0, decJ0, vertex);
			contour << R3*vertex;
			StelUtils::spheToRect(ra-dRA1*cDecJ0, decJ0, vertex);
			contour << R3*vertex;
			StelUtils::spheToRect(ra-dRA0*cDecJ1, decJ1, vertex);
			contour << R3*vertex;
			StelUtils::spheToRect(ra-dRA1*cDecJ0, decJ0, vertex);
			contour << R3*vertex;
			StelUtils::spheToRect(ra-dRA1*cDecJ1, decJ1, vertex);
			contour << R3*vertex;
			StelUtils::spheToRect(ra-dRA0*cDecJ1, decJ1, vertex);
			contour << R3*vertex;
		}
	}
	Q_ASSERT(contour.length()==coronaTextureCoords.length());

	coronaMesh.vertex=contour;
	coronaMesh.texCoords=coronaTextureCoords;
	coronaMesh.primitiveType=StelVertexArray::Triangles;

	texSunCorona->bind();
	// For some reason we must mix color with the given alpha as well, else mixing does not work.
	painter->setColor(color*alpha, alpha);
	painter->setBlending(true, GL_ONE, GL_ONE);
	painter->drawStelVertexArray(coronaMesh, false);

	// GZ: WHY DO WE NEED THIS?
	postDrawPointSource(painter, true);
}

// Terminate drawing of a 3D model, draw the halo
void StelSkyDrawer::postDrawSky3dModel(StelPainter* painter, const Vec3d& v, float illuminatedArea, float mag, const Vec3f& color, const bool isSun)
{
	const float scale = StelApp::getInstance().getScreenScale();
	const float pixPerRad = painter->getProjector()->getPixelPerRadAtCenter();
	// Assume a disk shape
	float pixRadius = std::sqrt(illuminatedArea/(60.f*60.f)*M_PI_180f*M_PI_180f*(pixPerRad*pixPerRad))/M_PIf;
	float pxRd = pixRadius*3.f+100.f*scale;
	bool noStarHalo = false;

	if (isSun)
	{
		// Sun, halo size varies in function of the magnitude because sun as seen from pluto should look dimmer
		// as the sun as seen from earth
		texSunHalo->bind();
		painter->setBlending(true, GL_ONE, GL_ONE);

		const float rmag = big3dModelHaloRadius*scale*(mag+15.f)/-11.f;
		const float cmag = (rmag>=pxRd) ? 1.f : qMax(0.15f, 1.f-(pxRd-rmag)/100/scale); // was qMax(0, .), but this would remove the halo when sun is dim.
		Vec3f win;
		painter->getProjector()->project(v, win);
		painter->setColor(color*cmag);
		painter->drawSprite2dModeNoDeviceScale(win[0], win[1], rmag);
		noStarHalo = true;
	}

	// Photographic mode: a body whose disk is (nearly) unresolved records like a star, with the
	// camera PSF. As its disk grows, the PSF fades out and the disk takes over.
	if (getFlagPhotographicModeActive())
	{
		if (noStarHalo) // the Sun keeps its textured halo for now
			return;
		const float tStart = 3.f*scale;
		const float tStop = 6.f*scale;
		if (pixRadius >= tStop)
			return;
		RCMag rcm;
		if (!computePhotoRCMag(mag, &rcm))
			return;
		if (pixRadius > tStart)
			rcm.luminance *= (tStop-pixRadius)/(tStop-tStart);
		preDrawPointSource(painter);
		drawPointSource(painter, v, rcm, color);
		postDrawPointSource(painter);
		return;
	}

	// Now draw the halo according the object brightness
	const bool saveTwinkle = flagStarTwinkle;
	flagStarTwinkle = false;
	const bool saveForcedTwinkle = flagForcedTwinkle;
	flagForcedTwinkle = false;
	const bool saveSpiky = flagStarSpiky;
	if (mag<-5.f) // exclude very bright objects only
		flagStarSpiky = false;
	else
		flagStarSpiky = saveSpiky;

	RCMag rcm;
	computeRCMag(mag, &rcm);

	// We now have the radius and luminosity of the small halo
	// If the disk of the planet is big enough to be visible, we should adjust the eye adaptation luminance
	// so that the radius of the halo is small enough to be not visible (so that we see the disk)

	// TODO: Change drawing halo to more realistic view of stars and planets
	float tStart = 3.f*scale; // Was 2.f: planet's halo is too dim. Atque 2020-11-12: No need to change these anymore. It appears that this has to do with halo size vs FOV (?).
	float tStop = 6.f*scale;
	bool truncated=false;

	float maxHaloRadius = qMax(tStart*6.f, pixRadius*3.f); //Atque 2020-11-12: Careful, if tStart*6.f is too big (tStart*10.f or something), the Moon gets a ridiculously big halo.
	if (rcm.radius>maxHaloRadius)
	{
		truncated = true;
		// One more factor of scale here is needed to compensate for taking a square root of the already included factor
		rcm.radius=maxHaloRadius+std::sqrt((rcm.radius-maxHaloRadius)*scale);
	}

	// Fade the halo away when the disk is too big
	if (pixRadius>=tStop)
	{
		rcm.luminance=0.f;
	}
	if (pixRadius>tStart && pixRadius<tStop)
	{
		rcm.luminance=(tStop-pixRadius)/(tStop-tStart);
	}

	if (truncated && flagLuminanceAdaptation)
	{
		float wl = findWorldLumForMag(mag, rcm.radius);
		if (wl>0)
		{
			const float fov = static_cast<float>(core->getMovementMgr()->getCurrentFov());
			// Report to the SkyDrawer that a very bright object (most notably Sun, Moon, bright planet)
			// is in view. LP:1138533 correctly wants no such effect if object is hidden by landscape horizon.
			LandscapeMgr* lmgr=GETSTELMODULE(LandscapeMgr);
			Q_ASSERT(lmgr);
			Landscape *landscape=lmgr->getCurrentLandscape();
			Q_ASSERT(landscape);
			float opacity=(landscape->getFlagShow() ? landscape->getOpacity(core->j2000ToAltAz(v, StelCore::RefractionAuto)) : 0.0f);
			reportLuminanceInFov(qMin(700.f, qMin(wl/50, (60.f*60.f)/(fov*fov)*6.f))*(1.0f-opacity));
		}
	}

	if (!noStarHalo)
	{
		preDrawPointSource(painter);
		drawPointSource(painter, v, rcm, color);
		postDrawPointSource(painter);
	}
	flagStarTwinkle=saveTwinkle;
	flagForcedTwinkle=saveForcedTwinkle;
	flagStarSpiky=saveSpiky;
}

float StelSkyDrawer::findWorldLumForMag(float mag, float targetRadius) const
{
	const float saveLum = eye->getWorldAdaptationLuminance();	// save

	// Compute the luminance by dichotomy
	float a=0.001f;
	float b=500000.f;
	RCMag rcmag;
	rcmag.radius=-99;
	float curLum = 500.f;
	int safety=0;
	while (std::fabs(rcmag.radius-targetRadius)>0.1f)
	{
		eye->setWorldAdaptationLuminance(curLum);
		computeRCMag(mag, &rcmag);
		if (rcmag.radius<=targetRadius)
		{
			float tmp = curLum;
			curLum=(a+curLum)/2;
			b=tmp;
		}
		else
		{
			float tmp = curLum;
			curLum=(b+curLum)/2;
			a=tmp;
		}
		++safety;
		if (safety>20)
		{
			if (curLum>490000.f)
			{
				curLum = 500000.f;
			}
			else
			{
				curLum=-1;
			}
			break;
		}
	}

	eye->setWorldAdaptationLuminance(saveLum);	// restore

	return curLum;
}

// Report that an object of luminance lum is currently displayed
void StelSkyDrawer::reportLuminanceInFov(float lum, bool fastAdaptation)
{
	if (lum > maxLum)
	{
		if (oldLum<0)
			oldLum=lum;
		// Use a log law for smooth transitions
		if (fastAdaptation==true && lum>oldLum)
		{
			maxLum = lum;
		}
		else
		{
			float transitionSpeed = 0.2f;
			maxLum = std::exp(std::log(oldLum)+(std::log(lum)-std::log(oldLum))* qMin(1.f, 1.f/StelApp::getInstance().getFps()/transitionSpeed));
		}
	}
}

void StelSkyDrawer::preDraw()
{
	eye->setWorldAdaptationLuminance(maxLum);
	// Re-initialize for next stage
	oldLum = maxLum;
	maxLum = 0;
}


void StelSkyDrawer::setLightPollutionLuminance(const double luminance)
{
	if(lightPollutionLuminance==luminance)
		return;
	lightPollutionLuminance=luminance;
	StelApp::immediateSave("stars/init_light_pollution_luminance", luminance);
	emit lightPollutionLuminanceChanged(luminance);
}

int StelSkyDrawer::getBortleScaleIndex() const
{
	return StelCore::luminanceToBortleScaleIndex(lightPollutionLuminance);
}

void StelSkyDrawer::setFlagStarSpiky(bool b)
{
	if (b!=flagStarSpiky)
	{
		flagStarSpiky=b;
		StelApp::immediateSave("stars/flag_star_spiky", b);
		emit flagStarSpikyChanged(flagStarSpiky);
	}
}

// colors for B-V display
Vec3f StelSkyDrawer::colorTable[128] = {
	Vec3f(0.602745f,0.713725f,1.000000f),
	Vec3f(0.604902f,0.715294f,1.000000f),
	Vec3f(0.607059f,0.716863f,1.000000f),
	Vec3f(0.609215f,0.718431f,1.000000f),
	Vec3f(0.611372f,0.720000f,1.000000f),
	Vec3f(0.613529f,0.721569f,1.000000f),
	Vec3f(0.635490f,0.737255f,1.000000f),
	Vec3f(0.651059f,0.749673f,1.000000f),
	Vec3f(0.666627f,0.762092f,1.000000f),
	Vec3f(0.682196f,0.774510f,1.000000f),
	Vec3f(0.697764f,0.786929f,1.000000f),
	Vec3f(0.713333f,0.799347f,1.000000f),
	Vec3f(0.730306f,0.811242f,1.000000f),
	Vec3f(0.747278f,0.823138f,1.000000f),
	Vec3f(0.764251f,0.835033f,1.000000f),
	Vec3f(0.781223f,0.846929f,1.000000f),
	Vec3f(0.798196f,0.858824f,1.000000f),
	Vec3f(0.812282f,0.868236f,1.000000f),
	Vec3f(0.826368f,0.877647f,1.000000f),
	Vec3f(0.840455f,0.887059f,1.000000f),
	Vec3f(0.854541f,0.896470f,1.000000f),
	Vec3f(0.868627f,0.905882f,1.000000f),
	Vec3f(0.884627f,0.916862f,1.000000f),
	Vec3f(0.900627f,0.927843f,1.000000f),
	Vec3f(0.916627f,0.938823f,1.000000f),
	Vec3f(0.932627f,0.949804f,1.000000f),
	Vec3f(0.948627f,0.960784f,1.000000f),
	Vec3f(0.964444f,0.972549f,1.000000f),
	Vec3f(0.980261f,0.984313f,1.000000f),
	Vec3f(0.996078f,0.996078f,1.000000f),
	Vec3f(1.000000f,1.000000f,1.000000f),
	Vec3f(1.000000f,0.999643f,0.999287f),
	Vec3f(1.000000f,0.999287f,0.998574f),
	Vec3f(1.000000f,0.998930f,0.997861f),
	Vec3f(1.000000f,0.998574f,0.997148f),
	Vec3f(1.000000f,0.998217f,0.996435f),
	Vec3f(1.000000f,0.997861f,0.995722f),
	Vec3f(1.000000f,0.997504f,0.995009f),
	Vec3f(1.000000f,0.997148f,0.994296f),
	Vec3f(1.000000f,0.996791f,0.993583f),
	Vec3f(1.000000f,0.996435f,0.992870f),
	Vec3f(1.000000f,0.996078f,0.992157f),
	Vec3f(1.000000f,0.991140f,0.981554f),
	Vec3f(1.000000f,0.986201f,0.970951f),
	Vec3f(1.000000f,0.981263f,0.960349f),
	Vec3f(1.000000f,0.976325f,0.949746f),
	Vec3f(1.000000f,0.971387f,0.939143f),
	Vec3f(1.000000f,0.966448f,0.928540f),
	Vec3f(1.000000f,0.961510f,0.917938f),
	Vec3f(1.000000f,0.956572f,0.907335f),
	Vec3f(1.000000f,0.951634f,0.896732f),
	Vec3f(1.000000f,0.946695f,0.886129f),
	Vec3f(1.000000f,0.941757f,0.875526f),
	Vec3f(1.000000f,0.936819f,0.864924f),
	Vec3f(1.000000f,0.931881f,0.854321f),
	Vec3f(1.000000f,0.926942f,0.843718f),
	Vec3f(1.000000f,0.922004f,0.833115f),
	Vec3f(1.000000f,0.917066f,0.822513f),
	Vec3f(1.000000f,0.912128f,0.811910f),
	Vec3f(1.000000f,0.907189f,0.801307f),
	Vec3f(1.000000f,0.902251f,0.790704f),
	Vec3f(1.000000f,0.897313f,0.780101f),
	Vec3f(1.000000f,0.892375f,0.769499f),
	Vec3f(1.000000f,0.887436f,0.758896f),
	Vec3f(1.000000f,0.882498f,0.748293f),
	Vec3f(1.000000f,0.877560f,0.737690f),
	Vec3f(1.000000f,0.872622f,0.727088f),
	Vec3f(1.000000f,0.867683f,0.716485f),
	Vec3f(1.000000f,0.862745f,0.705882f),
	Vec3f(1.000000f,0.858617f,0.695975f),
	Vec3f(1.000000f,0.854490f,0.686068f),
	Vec3f(1.000000f,0.850362f,0.676161f),
	Vec3f(1.000000f,0.846234f,0.666254f),
	Vec3f(1.000000f,0.842107f,0.656346f),
	Vec3f(1.000000f,0.837979f,0.646439f),
	Vec3f(1.000000f,0.833851f,0.636532f),
	Vec3f(1.000000f,0.829724f,0.626625f),
	Vec3f(1.000000f,0.825596f,0.616718f),
	Vec3f(1.000000f,0.821468f,0.606811f),
	Vec3f(1.000000f,0.817340f,0.596904f),
	Vec3f(1.000000f,0.813213f,0.586997f),
	Vec3f(1.000000f,0.809085f,0.577090f),
	Vec3f(1.000000f,0.804957f,0.567183f),
	Vec3f(1.000000f,0.800830f,0.557275f),
	Vec3f(1.000000f,0.796702f,0.547368f),
	Vec3f(1.000000f,0.792574f,0.537461f),
	Vec3f(1.000000f,0.788447f,0.527554f),
	Vec3f(1.000000f,0.784319f,0.517647f),
	Vec3f(1.000000f,0.784025f,0.520882f),
	Vec3f(1.000000f,0.783731f,0.524118f),
	Vec3f(1.000000f,0.783436f,0.527353f),
	Vec3f(1.000000f,0.783142f,0.530588f),
	Vec3f(1.000000f,0.782848f,0.533824f),
	Vec3f(1.000000f,0.782554f,0.537059f),
	Vec3f(1.000000f,0.782259f,0.540294f),
	Vec3f(1.000000f,0.781965f,0.543529f),
	Vec3f(1.000000f,0.781671f,0.546765f),
	Vec3f(1.000000f,0.781377f,0.550000f),
	Vec3f(1.000000f,0.781082f,0.553235f),
	Vec3f(1.000000f,0.780788f,0.556471f),
	Vec3f(1.000000f,0.780494f,0.559706f),
	Vec3f(1.000000f,0.780200f,0.562941f),
	Vec3f(1.000000f,0.779905f,0.566177f),
	Vec3f(1.000000f,0.779611f,0.569412f),
	Vec3f(1.000000f,0.779317f,0.572647f),
	Vec3f(1.000000f,0.779023f,0.575882f),
	Vec3f(1.000000f,0.778728f,0.579118f),
	Vec3f(1.000000f,0.778434f,0.582353f),
	Vec3f(1.000000f,0.778140f,0.585588f),
	Vec3f(1.000000f,0.777846f,0.588824f),
	Vec3f(1.000000f,0.777551f,0.592059f),
	Vec3f(1.000000f,0.777257f,0.595294f),
	Vec3f(1.000000f,0.776963f,0.598530f),
	Vec3f(1.000000f,0.776669f,0.601765f),
	Vec3f(1.000000f,0.776374f,0.605000f),
	Vec3f(1.000000f,0.776080f,0.608235f),
	Vec3f(1.000000f,0.775786f,0.611471f),
	Vec3f(1.000000f,0.775492f,0.614706f),
	Vec3f(1.000000f,0.775197f,0.617941f),
	Vec3f(1.000000f,0.774903f,0.621177f),
	Vec3f(1.000000f,0.774609f,0.624412f),
	Vec3f(1.000000f,0.774315f,0.627647f),
	Vec3f(1.000000f,0.774020f,0.630883f),
	Vec3f(1.000000f,0.773726f,0.634118f),
	Vec3f(1.000000f,0.773432f,0.637353f),
	Vec3f(1.000000f,0.773138f,0.640588f),
	Vec3f(1.000000f,0.772843f,0.643824f),
	Vec3f(1.000000f,0.772549f,0.647059f),
};

static float Gamma(float gamma, float x)
{
	return ((x<=0.f) ? 0.f : std::exp(gamma*std::log(x)));
}

static Vec3f Gamma(float gamma,const Vec3f &x)
{
	return Vec3f(Gamma(gamma,x[0]),Gamma(gamma,x[1]),Gamma(gamma,x[2]));
}

// Load B-V conversion parameters from config file
void StelSkyDrawer::initColorTableFromConfigFile(QSettings* conf)
{
	std::map<float,Vec3f> color_map;
	for (int bV100=-50;bV100<=400;bV100++)
	{
		QString entry = QString::asprintf("bv_color_%+5.2f", static_cast<double>(bV100)*0.01);
		const QStringList s(conf->value(QString("stars/") + entry).toStringList());
		if (!s.isEmpty())
		{
			Vec3f c;
			if (s.size()==1)
				c = Vec3f(s[0]);
			else
				c =Vec3f(s);
			color_map[bV100*0.01f] = Gamma(eye->getDisplayGamma(),c);
		}
	}

	if (color_map.size() > 1)
	{
		for (unsigned char i=0;i<128;i++)
		{
			const float bV = StelSkyDrawer::indexToBV(i);
			auto greater = color_map.upper_bound(bV);
			if (greater == color_map.begin())
			{
				colorTable[i] = greater->second;
			}
			else
			{
				auto less = greater;--less;
				if (greater == color_map.end())
				{
					colorTable[i] = less->second;
				}
				else
				{
					colorTable[i] = Gamma(1.f/eye->getDisplayGamma(), ((bV-less->first)*greater->second + (greater->first-bV)*less->second) *(1.f/(greater->first-less->first)));
				}
			}
		}
	}

// 	QString res;
// 	for (int i=0;i<128;i++)
// 	{
// 		res += QString("Vec3f(%1,%2,%3),\n").arg(colorTable[i][0], 0, 'g', 6).arg(colorTable[i][1], 0, 'g', 6).arg(colorTable[i][2], 0, 'g', 6);
// 	}
// 	qDebug() << res;
}

double StelSkyDrawer::getWorldAdaptationLuminance() const
{
	return static_cast<double>(eye->getWorldAdaptationLuminance());
}

void StelSkyDrawer::setRelativeStarScale(double b)
{
	starRelativeScale=b;
	StelApp::immediateSave("stars/relative_scale", b);
	emit relativeStarScaleChanged(b);
}

void StelSkyDrawer::setAbsoluteStarScale(double b)
{
	starAbsoluteScaleF=b;
	StelApp::immediateSave("stars/absolute_scale", b);
	emit absoluteStarScaleChanged(b);
}

void StelSkyDrawer::setTwinkleAmount(double b)
{
	twinkleAmount=b;
	StelApp::immediateSave("stars/star_twinkle_amount", b);
	emit twinkleAmountChanged(b);
}

void StelSkyDrawer::setFlagTwinkle(bool b)
{
	if(b!=flagStarTwinkle)
	{
		flagStarTwinkle=b;
		StelApp::immediateSave("stars/flag_star_twinkle", b);
		emit flagTwinkleChanged(b);
	}
}

void StelSkyDrawer::setFlagForcedTwinkle(bool b)
{
	if(b!=flagForcedTwinkle)
	{
		flagForcedTwinkle=b;
		StelApp::immediateSave("stars/flag_forced_twinkle", b);
		emit flagForcedTwinkleChanged(b);
	}
}

void StelSkyDrawer::setFlagDrawBigStarHalo(bool b)
{
	if(b!=flagDrawBigStarHalo)
	{
		flagDrawBigStarHalo=b;
		StelApp::immediateSave("stars/flag_star_halo", b);
		emit flagDrawBigStarHaloChanged(b);
	}
}

void StelSkyDrawer::setFlagStarMagnitudeLimit(bool b)
{
	if(b!=flagStarMagnitudeLimit)
	{
		flagStarMagnitudeLimit = b;
		StelApp::immediateSave("astro/flag_star_magnitude_limit", b);
		emit flagStarMagnitudeLimitChanged(b);
	}
}

void StelSkyDrawer::setFlagNebulaMagnitudeLimit(bool b)
{
	if(b!=flagNebulaMagnitudeLimit)
	{
		flagNebulaMagnitudeLimit = b;
		StelApp::immediateSave("astro/flag_nebula_magnitude_limit", b);
		emit flagNebulaMagnitudeLimitChanged(b);
	}
}

void StelSkyDrawer::setFlagPlanetMagnitudeLimit(bool b)
{
	if(b!=flagPlanetMagnitudeLimit)
	{
		flagPlanetMagnitudeLimit = b;
		StelApp::immediateSave("astro/flag_planet_magnitude_limit", b);
		emit flagPlanetMagnitudeLimitChanged(b);
	}
}

void StelSkyDrawer::setCustomStarMagnitudeLimit(double limit)
{
	customStarMagLimit=limit;
	StelApp::immediateSave("astro/star_magnitude_limit", limit);
	emit customStarMagLimitChanged(limit);
}

void StelSkyDrawer::setCustomNebulaMagnitudeLimit(double limit)
{
	customNebulaMagLimit=limit;
	StelApp::immediateSave("astro/nebula_magnitude_limit", limit);
	emit customNebulaMagLimitChanged(limit);
}

void StelSkyDrawer::setCustomPlanetMagnitudeLimit(double limit)
{
	customPlanetMagLimit=limit;
	StelApp::immediateSave("astro/planet_magnitude_limit", limit);
	emit customPlanetMagLimitChanged(limit);
}

void StelSkyDrawer::setFlagLuminanceAdaptation(bool b)
{
	if(b!=flagLuminanceAdaptation)
	{
		flagLuminanceAdaptation=b;
		StelApp::immediateSave("viewing/use_luminance_adaptation", b);
		emit flagLuminanceAdaptationChanged(b);
	}
}

void StelSkyDrawer::setDaylightLabelThreshold(double t)
{
	daylightLabelThreshold=t;
	StelApp::immediateSave("viewing/sky_brightness_label_threshold", t);
	emit daylightLabelThresholdChanged(t);
}

void StelSkyDrawer::setFlagPhotographicMode(bool b)
{
	if (b==flagPhotographicMode)
		return;
	flagPhotographicMode=b;
	StelApp::immediateSave("viewing/flag_photographic_mode", b);
	emit flagPhotographicModeChanged(b);
	if (b && !StelMainView::getInstance().getGLInformation().isHighGraphicsMode)
		qWarning() << "Photographic mode needs high graphics mode (OpenGL 3 or later). It will have no effect.";
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
	return flagPhotographicMode && StelMainView::getInstance().getGLInformation().isHighGraphicsMode;
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

void StelSkyDrawer::setPhotoAperture(double fNumber)
{
	fNumber = qBound(0.7, fNumber, 64.);
	// The program mode chooses the aperture: a setting now is the user's, used again when it stops
	if (photoProgramRunning)
	{
		photoUserAperture = fNumber;
		StelApp::immediateSave("viewing/photographic_aperture", fNumber);
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
	lens.pupil = qMin(1.f, PHOTO_LENS_MAX_APERTURE/N);
	lens.vignetting = flagPhotoVignetting && getFlagPhotographicModeActive();
	if (flagPhotoAberrations && getFlagPhotographicModeActive())
	{
		// Third-order aberrations: the coma grows with the field and the square of the aperture,
		// the astigmatism with the square of the field and the aperture (the field factors are in the shader)
		const float diagonal = 2.f*lens.corner;
		const float open = PHOTO_LENS_MAX_APERTURE/N;
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
	if (photoFocusDistance != 0. && getFlagPhotographicModeActive())
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

void StelSkyDrawer::setFlagPhotoZoomWholeMillimeters(bool b)
{
	if (b == flagPhotoZoomWholeMillimeters)
		return;
	flagPhotoZoomWholeMillimeters = b;
	// Turning it on sets the lens to the nearest whole focal length right away
	if (b && getFlagPhotographicModeActive())
	{
		const double mm = std::round(1000.*getPhotoFocalLengthForFov(core->getMovementMgr()->getAimFov()));
		if (mm >= 1.)
			setPhotoFocalLength(mm);
	}
	StelApp::immediateSave("viewing/flag_photographic_zoom_whole_millimeters", b);
	emit flagPhotoZoomWholeMillimetersChanged(b);
}

double StelSkyDrawer::getPhotoWheelZoomFov(double currentFov, double targetFov) const
{
	if (!flagPhotoZoomWholeMillimeters || !getFlagPhotographicModeActive())
		return targetFov;
	const double current = 1000.*getPhotoFocalLengthForFov(currentFov);
	const double target = 1000.*getPhotoFocalLengthForFov(targetFov);
	if (!(current > 0.) || !(target > 0.) || !std::isfinite(target))
		return targetFov;
	// The nearest whole millimeter, but at least the next one in the direction of the zoom, so that every
	// step of the wheel (also the small steps of a touchpad) changes the lens. The tolerance keeps a lens
	// already at a whole focal length (up to rounding) from counting as between two.
	constexpr double tolerance = 0.01;
	double mm = std::round(target);
	if (targetFov < currentFov)       // zoom in: longer focal length
		mm = qMax(mm, std::floor(current + tolerance) + 1.);
	else if (targetFov > currentFov)  // zoom out: shorter focal length
		mm = qMin(mm, std::ceil(current - tolerance) - 1.);
	mm = qMax(mm, 1.);
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
		{ 0.054780, 5.30e-6, "MF", 43.8/32.9 }                         // 43.8 mm / 8256 (51 Mpx)
	};
	return specs[qBound(0, format, PHOTO_SENSOR_FORMAT_COUNT - 1)];
}

const StelSkyDrawer::PhotoFilmSpec& StelSkyDrawer::getPhotoFilmSpec(int film)
{
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
	// The sky from X, Y, Z, W is off by default ("Spectral sky" in the dialog, astro/flag_photographic_film_spectral_sky): its large coefficients
	// cancel each other, so that a few percent in W make 25% in red, and the violet it gives to the twilight is not
	// in Till Credner's twilight slides (Ektachrome, Sensia: a deep blue sky over an orange horizon), which the
	// colors of rgbToFilm match.
	// The airglow (O I 557.7 and 630.0/636.4 nm, Na 589 nm, OH bands, with a continuum fitted to its color) is
	// computed from its spectrum directly.
	// Emission nebulae (H II regions) too: their light is in lines, most of it in H-alpha at 656 nm, which the V band
	// of their catalog magnitude hardly sees (about 9%) but the red layer of a film does. A typical H II region
	// (relative to H-beta: H-alpha 2.86, [N II] 658.3/654.8 0.8/0.27, [O III] 500.7/495.9 2.0/0.67 as in the bright
	// ones, M 42, M 8, M 20, the Rosette, H-gamma and H-delta, [S II] 0.4, He I, a weak continuum; reddened by
	// E(B-V) = 0.5) exposes the red layer about 4.6-4.9 times as much as white light of the same V, green and blue
	// 0.7-0.85 times: on film they are bright and red (North America, Orion, Lagoon). The scene colors that give
	// these layers through rgbToFilm.
	static const PhotoFilmSpec films[PHOTO_FILM_COUNT] = {
		{"Digital sensor", "", 0., PhotoFilmSlide, 0, {0,0,0,0}, {0,0,0,0}, 1., {0,0,0,0}, 0., 1., 1.f, 1.f, 0.18f, {0,0,0}},
		{"Fujichrome Provia 100F", "Provia 100F", 100., PhotoFilmSlide,
		 2, {128., 240., 0, 0}, {128., 190.5, 0, 0}, 0.63, {0., 0.025, 0, 0}, 8., 1.7, 1.7f, 1.0f, 0.15f, {0,0,0},
		 {0.8823f, -0.0092f, 0.0425f, 0.1133f, 0.8423f, 0.0529f, 0.0625f, -0.0729f, 1.2184f},
		 {7.7363f, -8.6066f, -2.8156f, 2.1554f, -1.9413f, 3.4614f, 0.5767f, -0.5048f, -0.7867f, 1.2239f, 1.4820f, -0.4456f},
		 {1.3715f, 0.9966f, 0.6528f},
		 {5.183f, 0.129f, 0.291f}},
		{"Fujichrome Provia 400F", "Provia 400F", 400., PhotoFilmSlide,
		 4, {32., 60., 120., 240.}, {32., 37.8, 60., 120.}, 0.66, {0., 0.05, 0.075, 0.075}, 13., 1.7, 1.7f, 1.0f, 0.15f, {0,0,0},
		 {0.8823f, -0.0092f, 0.0425f, 0.1133f, 0.8423f, 0.0529f, 0.0625f, -0.0729f, 1.2184f},
		 {7.7363f, -8.6066f, -2.8156f, 2.1554f, -1.9413f, 3.4614f, 0.5767f, -0.5048f, -0.7867f, 1.2239f, 1.4820f, -0.4456f},
		 {1.3715f, 0.9966f, 0.6528f},
		 {5.183f, 0.129f, 0.291f}},
		{"Kodak Ektachrome E200", "E200", 200., PhotoFilmSlide,
		 1, {10., 0, 0, 0}, {10., 0, 0, 0}, 0.9, {0,0,0,0}, 12., 1.7, 1.7f, 1.0f, 0.15f, {0,0,0},
		 {0.8792f, -0.0011f, 0.0371f, 0.0948f, 0.8654f, 0.0501f, 0.1603f, -0.4778f, 1.5726f},
		 {7.5488f, -8.3193f, -2.7195f, 2.0691f, -1.6322f, 2.9534f, 0.4129f, -0.3452f, -1.9772f, 3.2321f, 2.0329f, -1.0508f},
		 {1.3868f, 1.0087f, 0.7239f},
		 {5.185f, 0.272f, 0.f}},
		{"Kodak Royal Gold 400", "Royal Gold 400", 400., PhotoFilmNegative,
		 1, {10., 0, 0, 0}, {10., 0, 0, 0}, 0.8, {0,0,0,0}, 5., 0.65, 1.15f, 11.5f, 0.18f, {0,0,0},
		 {0.9412f, -0.0826f, 0.0515f, 0.0703f, 0.8433f, 0.1094f, 0.1719f, -0.4943f, 1.5769f},
		 {8.9881f, -10.5313f, -3.4227f, 2.7209f, -1.3177f, 2.4159f, 0.2430f, -0.1723f, -2.2375f, 3.6457f, 2.1490f, -1.1702f},
		 {1.2693f, 1.0570f, 0.7534f},
		 {5.226f, 0.567f, 0.053f}},
		{"Kodak T-Max 400", "T-Max 400", 400., PhotoFilmMonochrome,
		 4, {0.4, 1.26, 15., 300.}, {0.4, 1., 10., 100.}, 0.77, {0,0,0,0}, 10., 0.65, 1.15f, 5.8f, 0.18f, {0.30f, 0.52f, 0.18f}},
		{"Kodak Tri-X 400", "Tri-X 400", 400., PhotoFilmMonochrome,
		 4, {0.17, 2., 50., 1200.}, {0.17, 1., 10., 100.}, 0.725, {0,0,0,0}, 17., 0.65, 1.15f, 5.8f, 0.18f, {0.27f, 0.50f, 0.23f}},
	};
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
	const PhotoReciprocityParams p = getPhotoReciprocityParams();
	if (!p.enabled || !(x > 0.f))
		return x;
	const double tn = qMin(1e20, static_cast<double>(p.time)*PHOTO_MIDDLE_GREY_EXPOSURE/static_cast<double>(x));
	if (tn <= static_cast<double>(p.times[0]))
		return x;
	for (int i = 1; i < p.points; ++i)
		if (tn <= static_cast<double>(p.times[i]))
		{
			const double u = std::log(tn/static_cast<double>(p.times[i-1]))/std::log(static_cast<double>(p.times[i]/p.times[i-1]));
			const double te = static_cast<double>(p.effective[i-1])*std::pow(static_cast<double>(p.effective[i]/p.effective[i-1]), u);
			return static_cast<float>(static_cast<double>(x)*te/tn);
		}
	const int last = p.points - 1;
	const double te = static_cast<double>(p.effective[last])*std::pow(tn/static_cast<double>(p.times[last]), static_cast<double>(p.exponent));
	return static_cast<float>(static_cast<double>(x)*te/tn);
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

double StelSkyDrawer::getPhotoFilmEffectiveTime(double t) const
{
	if (!getFlagPhotoFilmActive() || !(t > 0.))
		return t;
	const PhotoFilmSpec& f = getPhotoActiveFilmSpec();
	if (f.points <= 0 || t <= f.time[0])
		return t;
	for (int i = 1; i < f.points; ++i)
	{
		if (t <= f.time[i])
		{
			const double u = std::log(t/f.time[i-1]) / std::log(f.time[i]/f.time[i-1]);
			return f.effective[i-1] * std::pow(f.effective[i]/f.effective[i-1], u);
		}
	}
	const int last = f.points - 1;
	return f.effective[last] * std::pow(t/f.time[last], f.exponent);
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

const float* StelSkyDrawer::getPhotoWhiteBalanceMatrix() const
{
	if (!getFlagPhotoFilmActive())
		return photoWBMatrix;
	const PhotoFilmSpec& f = getPhotoActiveFilmSpec();
	if (f.type == PhotoFilmMonochrome)
	{
		// The black and white picture is made from the film's own sensitivity (see getPhotoFilmMonoWeights())
		static const float identity[9] = {1,0,0, 0,1,0, 0,0,1};
		return identity;
	}
	// The color shift of the reciprocity failure is done per point of the picture (getPhotoReciprocityParams())
	const Vec3f tint = flagPhotoReciprocityPerPoint ? Vec3f(1.f) : getPhotoFilmTint(getPhotoEffectiveExposureTime());
	if (getPhotoFilmSpectral(f))
	{
		// The layers of the film from the light of the scene (balanced for daylight), each with its own reciprocity
		// failure; the dyes of the layers are taken as the primaries of the display
		for (int r = 0; r < 3; ++r)
			for (int c = 0; c < 3; ++c)
				photoWBMatrixFilm[r*3 + c] = f.rgbToFilm[r*3 + c] * tint[r];
		return photoWBMatrixFilm;
	}
	// The daylight balance of the film (photoWBMatrix, see updatePhotoWhiteBalanceMatrix()), after the layers'
	// own reciprocity failure: the matrix acts on the light as the film recorded it
	for (int r = 0; r < 3; ++r)
		for (int c = 0; c < 3; ++c)
			photoWBMatrixFilm[r*3 + c] = photoWBMatrix[r*3 + c] * tint[c];
	return photoWBMatrixFilm;
}

void StelSkyDrawer::setPhotoFilmSkyUniforms(QOpenGLShaderProgram& program) const
{
	const float* m = (getFlagPhotographicModeActive() && flagPhotoFilmSpectralSky) ? getPhotoFilmSkyMatrix() : nullptr;
	program.setUniformValue("photoFilmSky", m != nullptr);
	if (!m)
		return;
	program.setUniformValue("photoFilmSkyR", m[0], m[1], m[2], m[3]);
	program.setUniformValue("photoFilmSkyG", m[4], m[5], m[6], m[7]);
	program.setUniformValue("photoFilmSkyB", m[8], m[9], m[10], m[11]);
}

const float* StelSkyDrawer::getPhotoFilmSkyMatrix() const
{
	if (!getFlagPhotoFilmActive())
		return nullptr;
	const PhotoFilmSpec& f = getPhotoActiveFilmSpec();
	return getPhotoFilmSpectral(f) ? f.skyXYZW : nullptr;
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
	if (getFlagPhotographicModeActive())
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
	photoPushedFilmSpec = getPhotoFilmSpec(photoFilm);
	if (photoFilm == PhotoFilmNone || photoFilmPush == 0)
		return photoPushedFilmSpec;
	const double p = static_cast<double>(photoFilmPush);
	const bool mono = photoPushedFilmSpec.type == PhotoFilmMonochrome;
	photoPushedFilmSpec.iso *= std::exp2(p);
	const double contrast = 1. + 0.12*p;
	photoPushedFilmSpec.gamma *= contrast;
	photoPushedFilmSpec.toneA = static_cast<float>(static_cast<double>(photoPushedFilmSpec.toneA)*contrast);
	photoPushedFilmSpec.granularity *= std::pow(mono ? 1.15 : 1.12, p);
	return photoPushedFilmSpec;
}

void StelSkyDrawer::setPhotoFilmPush(int stops)
{
	const int p = qBound(0, stops, PHOTO_FILM_MAX_PUSH);
	if (p == photoFilmPush)
		return;
	photoFilmPush = p;
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

Vec4f StelSkyDrawer::getPhotoFrame(const StelProjector& prj) const
{
	const double vx = prj.getViewportPosX(), vy = prj.getViewportPosY();
	const double vw = prj.getViewportWidth(), vh = prj.getViewportHeight();
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
	if (getFlagPhotographicModeActive())
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
	if (getFlagPhotographicModeActive())
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
	if (photoNoise == PhotoNoiseOff || !getFlagPhotographicModeActive() || getFlagPhotoFilmActive())
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

bool StelSkyDrawer::photoTextureStatistics(StelTexture& texture, float power, float& mean, float& meanSquare)
{
	mean = meanSquare = -1.f;
	if (!photoTextureMeanTried)
	{
		photoTextureMeanTried = true;
		// One fragment that averages 32 x 32 samples of a coarse mipmap level (each the mean of many texels)
		const char* vsrc = R"(
void main(void)
{
	// a triangle that covers the 1 x 1 viewport
	gl_Position = vec4(float((gl_VertexID & 1) * 4 - 1), float((gl_VertexID >> 1) * 4 - 1), 0., 1.);
}
)";
		const char* fsrc = R"(
uniform sampler2D tex;
uniform highp float lod;
uniform highp float power;
void main(void)
{
	highp vec3 sum = vec3(0.);
	highp float sum2 = 0.;
	for (int j = 0; j < 32; ++j)
		for (int i = 0; i < 32; ++i)
		{
			highp vec3 c = pow(max(textureLod(tex, (vec2(float(i), float(j)) + 0.5) / 32., lod).rgb, vec3(0.)), vec3(power));
			highp float y = dot(c, vec3(0.2126, 0.7152, 0.0722));
			sum += c;
			sum2 += y*y;
		}
	FRAG_COLOR = vec4(sum / 1024., sum2 / 1024.);
}
)";
		auto program = std::make_unique<QOpenGLShaderProgram>();
		if (program->addShaderFromSourceCode(QOpenGLShader::Vertex, StelOpenGL::globalShaderPrefix(StelOpenGL::VERTEX_SHADER) + vsrc) &&
		    program->addShaderFromSourceCode(QOpenGLShader::Fragment, StelOpenGL::globalShaderPrefix(StelOpenGL::FRAGMENT_SHADER) + fsrc) &&
		    StelPainter::linkProg(program.get(), "photographic texture mean"))
		{
			photoTextureMeanProgram = std::move(program);
			photoTextureMeanFbo.reset(new QOpenGLFramebufferObject(1, 1, QOpenGLFramebufferObject::NoAttachment, GL_TEXTURE_2D, GL_RGBA32F));
			photoTextureMeanVao.reset(new QOpenGLVertexArrayObject);
			photoTextureMeanVao->create();
			if (!photoTextureMeanFbo->isValid())
				photoTextureMeanProgram.reset();
		}
	}
	if (!photoTextureMeanProgram)
		return false;
	int w = 0, h = 0;
	if (!texture.getDimensions(w, h) || w <= 0 || h <= 0)
		return false;

	auto& gl = *QOpenGLContext::currentContext()->extraFunctions();
	GLint previousFbo = 0;
	GLint viewport[4] = {0, 0, 0, 0};
	gl.glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFbo);
	gl.glGetIntegerv(GL_VIEWPORT, viewport);
	GLint previousVao = 0;
	gl.glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &previousVao);
	const GLboolean blend = gl.glIsEnabled(GL_BLEND);
	const GLboolean scissor = gl.glIsEnabled(GL_SCISSOR_TEST);
	const GLboolean depth = gl.glIsEnabled(GL_DEPTH_TEST);
	gl.glDisable(GL_BLEND);
	gl.glDisable(GL_SCISSOR_TEST);
	gl.glDisable(GL_DEPTH_TEST);

	photoTextureMeanFbo->bind();
	gl.glViewport(0, 0, 1, 1);
	texture.bind(0);
	photoTextureMeanProgram->bind();
	photoTextureMeanProgram->setUniformValue("tex", 0);
	photoTextureMeanProgram->setUniformValue("power", power);
	// The mipmap level where 32 samples cover the image. The squares are of averaged texels, so <Y^2> comes out
	// somewhat smaller than for the full resolution: good enough for a calibration.
	photoTextureMeanProgram->setUniformValue("lod", qMax(0.f, std::log2(static_cast<float>(qMax(w, h))/32.f)));
	photoTextureMeanVao->bind();
	gl.glDrawArrays(GL_TRIANGLES, 0, 3);
	gl.glBindVertexArray(static_cast<GLuint>(previousVao));
	float pixel[4] = {0.f, 0.f, 0.f, 0.f};
	gl.glReadPixels(0, 0, 1, 1, GL_RGBA, GL_FLOAT, pixel);
	photoTextureMeanProgram->release();

	gl.glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previousFbo));
	gl.glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
	if (blend) gl.glEnable(GL_BLEND);
	if (scissor) gl.glEnable(GL_SCISSOR_TEST);
	if (depth) gl.glEnable(GL_DEPTH_TEST);

	mean = 0.2126f*pixel[0] + 0.7152f*pixel[1] + 0.0722f*pixel[2];
	meanSquare = pixel[3];
	return std::isfinite(mean) && std::isfinite(meanSquare);
}

bool StelSkyDrawer::beginPhotoBodyImage(const Vec3d& posJ2000, float radius, bool glow)
{
	PhotoBodyImage& im = photoBodyImage;
	im.active = false;
	if (!getFlagPhotographicModeActive() || !(radius > 0.f))
		return false;
	const StelProjectorP prj = core->getProjection(StelCore::FrameJ2000);
	Vec3d win;
	if (!prj->project(posJ2000, win))
		return false;
	// the star trail of the body (no visibility test: flux "infinite") and the blur of the lens
	PhotoStarInstance trailInfo;
	const float length = computePhotoTrail(*prj, posJ2000, win, 1e30f, 0.f, trailInfo);
	// The halo and the spikes are already images of the blurred lens
	const float defocus = glow ? 0.f : photoLensCache.defocus;
	if (length < 0.75f && std::fabs(defocus) < 0.5f)
		return false;
	const Vec2f A(trailInfo.trail[0], trailInfo.trail[1]), B(trailInfo.trail[2], trailInfo.trail[3]);
	const Vec2f C = -0.5f*(A + B);
	const float reach = qMax(qMax(A.norm(), B.norm()), C.norm()) + std::fabs(defocus) + 2.f;
	// The sharp image: the disk with a border, but only what can reach the viewport
	const int vx = prj->getViewportPosX(), vy = prj->getViewportPosY();
	const int vw = prj->getViewportWidth(), vh = prj->getViewportHeight();
	const float r = radius + 2.f;
	const int x0 = qMax(static_cast<int>(std::floor(win[0] - r)), static_cast<int>(std::floor(vx - reach)));
	const int y0 = qMax(static_cast<int>(std::floor(win[1] - r)), static_cast<int>(std::floor(vy - reach)));
	const int x1 = qMin(static_cast<int>(std::ceil(win[0] + r)), static_cast<int>(std::ceil(vx + vw + reach)));
	const int y1 = qMin(static_cast<int>(std::ceil(win[1] + r)), static_cast<int>(std::ceil(vy + vh + reach)));
	const int w = x1 - x0, h = y1 - y0;
	if (w <= 0 || h <= 0 || w > 8192 || h > 8192)
		return false;

	if (!photoBodyTried)
	{
		photoBodyTried = true;
		const char* vsrc = R"(
ATTRIBUTE highp vec2 position; // window coordinates
uniform highp vec4 viewport;   // x, y, width, height of the viewport
VARYING highp vec2 vWin;
void main(void)
{
	vWin = position;
	gl_Position = vec4((position - viewport.xy) / viewport.zw * 2. - 1., 0., 1.);
}
)";
		const char* fsrc = R"(
VARYING highp vec2 vWin;
uniform sampler2D tex;
uniform highp vec2 origin;      // window position of texel (0,0)
uniform highp vec2 texSize;     // size of the texture (texels)
uniform highp vec2 imageSize;   // the part of it that holds the image
uniform highp vec4 trail;       // positions at the start (xy) and the end (zw) relative to mid-exposure
uniform highp vec3 logStart;    // extinction along the trail
uniform highp vec3 logEnd;
uniform highp float defocus;    // radius of the blur disk
uniform highp float defocusSign;
uniform highp float defocusSA;
uniform int blades;
uniform highp float straightness;
uniform highp float rotation;
uniform int samples;
uniform highp float lod;
const highp float PI = 3.14159265358979;
void main(void)
{
	highp vec2 A = trail.xy;
	highp vec2 B = trail.zw;
	highp vec2 C = -0.5*(A + B);
	highp vec4 sum = vec4(0.);
	highp float weights = 0.;
	highp float n = float(samples);
	for (int i = 0; i < 1024; ++i)
	{
		if (i >= samples) break;
		highp float fi = float(i) + 0.5;
		// Time along the exposure: in order for a pure trail, else well spread (golden ratio)
		highp float t = defocus != 0. ? fract(fi*0.6180339887) : fi/n;
		highp vec2 offset = (1.-t)*(1.-t)*A + 2.*t*(1.-t)*C + t*t*B;
		highp float w = 1.;
		if (defocus != 0.)
		{
			// A point of the image of the aperture (golden-angle spiral, even density)
			highp float rr = sqrt(fi/n);
			highp float a = float(i)*2.39996323;
			highp float scale = 1.;
			if (blades >= 3 && straightness > 0.)
			{
				highp float nb = float(blades);
				highp float sector = 2.*PI/nb;
				highp float d = mod(a - rotation, sector) - 0.5*sector;
				scale = 1. / mix(1., cos(d)/cos(PI/nb), straightness);
			}
			offset += defocusSign * abs(defocus) * rr * scale * vec2(cos(a), sin(a));
			w = max(0., 1. + defocusSA*(rr*rr - 0.5));
		}
		// the light here comes from the point of the sharp image that was offset onto it
		highp vec2 p = vWin - offset - origin;
		highp vec4 c = vec4(0.);
		if (p.x >= 0. && p.y >= 0. && p.x <= imageSize.x && p.y <= imageSize.y)
			c = textureLod(tex, p / texSize, lod);
		highp vec3 tint = exp(logStart*((1.-t)*(1.-2.*t)) + logEnd*(t*(2.*t-1.)));
		sum += w * vec4(c.rgb*tint, c.a);
		weights += w;
	}
	FRAG_COLOR = weights > 0. ? sum / weights : vec4(0.); // premultiplied: the body covers what is behind it
}
)";
		auto program = std::make_unique<QOpenGLShaderProgram>();
		if (program->addShaderFromSourceCode(QOpenGLShader::Vertex, StelOpenGL::globalShaderPrefix(StelOpenGL::VERTEX_SHADER) + vsrc) &&
		    program->addShaderFromSourceCode(QOpenGLShader::Fragment, StelOpenGL::globalShaderPrefix(StelOpenGL::FRAGMENT_SHADER) + fsrc))
		{
			program->bindAttributeLocation("position", 0);
			if (StelPainter::linkProg(program.get(), "photographic body image"))
			{
				photoBodyProgram = std::move(program);
				photoBodyVao.reset(new QOpenGLVertexArrayObject);
				photoBodyVao->create();
			}
		}
	}
	if (!photoBodyProgram)
		return false;
	// One image buffer, grown when needed (with depth for the rings)
	if (!photoBodyFbo || photoBodyFbo->width() < w || photoBodyFbo->height() < h)
	{
		const int fw = qMax(w, photoBodyFbo ? photoBodyFbo->width() : 0);
		const int fh = qMax(h, photoBodyFbo ? photoBodyFbo->height() : 0);
		QOpenGLFramebufferObjectFormat format;
		format.setAttachment(QOpenGLFramebufferObject::Depth);
		format.setInternalTextureFormat(GL_RGBA16F);
		format.setMipmap(true);
		photoBodyFbo.reset(new QOpenGLFramebufferObject((fw + 63)/64*64, (fh + 63)/64*64, format));
		if (!photoBodyFbo->isValid())
		{
			photoBodyFbo.reset();
			return false;
		}
	}

	auto& gl = *QOpenGLContext::currentContext()->extraFunctions();
	gl.glGetIntegerv(GL_FRAMEBUFFER_BINDING, &im.previousFbo);
	gl.glGetIntegerv(GL_VIEWPORT, im.viewport);
	gl.glGetFloatv(GL_COLOR_CLEAR_VALUE, im.clearColor);
	im.scissor = gl.glIsEnabled(GL_SCISSOR_TEST);
	gl.glDisable(GL_SCISSOR_TEST);
	photoBodyFbo->bind();
	// The projection stays: the viewport is shifted so that window pixel (x0, y0) lands on texel (0, 0)
	gl.glViewport(im.viewport[0] - x0, im.viewport[1] - y0, im.viewport[2], im.viewport[3]);
	gl.glClearColor(0.f, 0.f, 0.f, 0.f);
	gl.glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	im.x0 = x0; im.y0 = y0; im.w = w; im.h = h;
	im.margin = reach;
	im.trail = length > 0.f ? trailInfo.trail : Vec4f(0.f);
	for (int c=0; c<3; ++c)
	{
		im.logStart[c] = std::log(qMax(1e-9f, trailInfo.trailStart[c]));
		im.logEnd[c] = std::log(qMax(1e-9f, trailInfo.trailEnd[c]));
	}
	im.defocus = std::fabs(defocus) >= 0.5f ? defocus : 0.f;
	im.glow = glow;
	im.active = true;
	return true;
}

void StelSkyDrawer::endPhotoBodyImage()
{
	PhotoBodyImage& im = photoBodyImage;
	if (!im.active)
		return;
	im.active = false;
	auto& gl = *QOpenGLContext::currentContext()->extraFunctions();
	gl.glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(im.previousFbo));
	gl.glViewport(im.viewport[0], im.viewport[1], im.viewport[2], im.viewport[3]);
	gl.glClearColor(im.clearColor[0], im.clearColor[1], im.clearColor[2], im.clearColor[3]);
	if (im.scissor)
		gl.glEnable(GL_SCISSOR_TEST);

	// Mipmaps: the samples of a long trail or a large blur disk read a correspondingly smoothed image
	gl.glActiveTexture(GL_TEXTURE0);
	gl.glBindTexture(GL_TEXTURE_2D, photoBodyFbo->texture());
	gl.glGenerateMipmap(GL_TEXTURE_2D);
	gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
	gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

	const Vec2f A(im.trail[0], im.trail[1]), B(im.trail[2], im.trail[3]);
	const float length = A.norm() + B.norm();
	const float R = std::fabs(im.defocus);
	// Samples: about one per pixel along the trail and per two square pixels of the blur disk
	const int samples = qBound(8, static_cast<int>(std::ceil(length + 0.5f*M_PIf*R*R)), 256);
	const float area = length*qMax(1.f, 2.f*R) + M_PIf*R*R;
	const float lod = 0.5f*std::log2(qMax(1.f, area/static_cast<float>(samples)));

	auto& prog = *photoBodyProgram;
	prog.bind();
	prog.setUniformValue("viewport", QVector4D(im.viewport[0], im.viewport[1], im.viewport[2], im.viewport[3]));
	prog.setUniformValue("tex", 0);
	prog.setUniformValue("origin", QVector2D(im.x0, im.y0));
	prog.setUniformValue("texSize", QVector2D(photoBodyFbo->width(), photoBodyFbo->height()));
	prog.setUniformValue("imageSize", QVector2D(im.w, im.h));
	prog.setUniformValue("trail", QVector4D(A[0], A[1], B[0], B[1]));
	prog.setUniformValue("logStart", QVector3D(im.logStart[0], im.logStart[1], im.logStart[2]));
	prog.setUniformValue("logEnd", QVector3D(im.logEnd[0], im.logEnd[1], im.logEnd[2]));
	prog.setUniformValue("defocus", im.defocus);
	prog.setUniformValue("defocusSign", im.defocus > 0.f ? -1.f : 1.f);
	prog.setUniformValue("defocusSA", im.defocus > 0.f ? -photoLensCache.sphericalAberration : photoLensCache.sphericalAberration);
	const int blades = photoApertureBlades >= 3 ? photoApertureBlades : 0;
	const float sector = blades > 0 ? 2.f*M_PIf/static_cast<float>(blades) : 0.f;
	prog.setUniformValue("blades", blades);
	prog.setUniformValue("straightness", blades > 0 ? getPhotoApertureStraightness() : 0.f);
	prog.setUniformValue("rotation", PHOTO_SPIKE_ANGLE - 0.5f*sector);
	prog.setUniformValue("samples", samples);
	prog.setUniformValue("lod", lod);

	// The quad: the image and how far the trail and the blur reach
	const float m = im.margin;
	const GLfloat quad[] = {
		im.x0 - m, im.y0 - m,   im.x0 + im.w + m, im.y0 - m,
		im.x0 - m, im.y0 + im.h + m,   im.x0 + im.w + m, im.y0 + im.h + m };
	GLint previousVao = 0;
	gl.glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &previousVao);
	GLint previousBuffer = 0;
	gl.glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &previousBuffer);
	photoBodyVao->bind();
	GLuint vbo = 0;
	gl.glGenBuffers(1, &vbo);
	gl.glBindBuffer(GL_ARRAY_BUFFER, vbo);
	gl.glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STREAM_DRAW);
	gl.glEnableVertexAttribArray(0);
	gl.glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
	const GLboolean blend = gl.glIsEnabled(GL_BLEND);
	const GLboolean depth = gl.glIsEnabled(GL_DEPTH_TEST);
	GLint blendSrcRGB, blendDstRGB, blendSrcA, blendDstA;
	gl.glGetIntegerv(GL_BLEND_SRC_RGB, &blendSrcRGB);
	gl.glGetIntegerv(GL_BLEND_DST_RGB, &blendDstRGB);
	gl.glGetIntegerv(GL_BLEND_SRC_ALPHA, &blendSrcA);
	gl.glGetIntegerv(GL_BLEND_DST_ALPHA, &blendDstA);
	gl.glEnable(GL_BLEND);
	gl.glDisable(GL_DEPTH_TEST);
	if (im.glow)
		gl.glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ZERO, GL_ONE); // light added to the scene
	else
		gl.glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	gl.glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	gl.glBlendFuncSeparate(static_cast<GLenum>(blendSrcRGB), static_cast<GLenum>(blendDstRGB), static_cast<GLenum>(blendSrcA), static_cast<GLenum>(blendDstA));
	if (!blend) gl.glDisable(GL_BLEND);
	if (depth) gl.glEnable(GL_DEPTH_TEST);
	gl.glDisableVertexAttribArray(0);
	gl.glDeleteBuffers(1, &vbo);
	gl.glBindVertexArray(static_cast<GLuint>(previousVao));
	gl.glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(previousBuffer));
	prog.release();
}

std::vector<StelSkyDrawer::PhotoImageSample> StelSkyDrawer::getPhotoImageSamples(const StelProjector& prj, const Vec3d& center) const
{
	std::vector<PhotoImageSample> samples;
	if (!getFlagPhotographicModeActive())
		return samples;
	Vec3d win;
	if (!prj.project(center, win))
		return samples;
	PhotoStarInstance trailInfo;
	const float length = qMax(0.f, computePhotoTrail(prj, center, win, 1e30f, 0.f, trailInfo));
	const float defocus = std::fabs(photoLensCache.defocus) >= 0.5f ? photoLensCache.defocus : 0.f;
	const float R = std::fabs(defocus);
	if (length < 0.75f && R == 0.f)
		return samples;
	// About one copy per pixel of the trail and per two square pixels of the blur disk (as for the bodies)
	const int n = qBound(2, static_cast<int>(std::ceil(length + 0.5f*M_PIf*R*R)), 96);
	const double pixelPerRad = static_cast<double>(prj.getPixelPerRadAtCenter());
	Vec3d view = center;
	prj.getModelViewTransform()->forward(view);
	view.normalize();
	const int blades = photoApertureBlades >= 3 ? photoApertureBlades : 0;
	const float straightness = blades > 0 ? getPhotoApertureStraightness() : 0.f;
	const float sector = blades > 0 ? 2.f*M_PIf/static_cast<float>(blades) : 0.f;
	const float rotation = PHOTO_SPIKE_ANGLE - 0.5f*sector;
	const float sign = defocus > 0.f ? -1.f : 1.f;
	const float sa = defocus > 0.f ? -photoLensCache.sphericalAberration : photoLensCache.sphericalAberration;
	float weights = 0.f;
	samples.reserve(static_cast<size_t>(n));
	for (int i=0; i<n; ++i)
	{
		const float fi = static_cast<float>(i) + 0.5f;
		// Time along the exposure: in order for a pure trail, else well spread over the disk (golden ratio)
		const float t = R > 0.f ? std::fmod(fi*0.6180339887f, 1.f) : fi/static_cast<float>(n);
		Mat4d rot = Mat4d::identity();
		Vec3f factor(1.f);
		if (length > 0.f)
		{
			// t = 0 at the start of the exposure (see photoTrailToStart)
			rot = Mat4d::rotation(photoTrailAxisView, photoTrailAngle*(0.5 - static_cast<double>(t)));
			if (photoTrailExtinction)
				factor = photoTrailExtinctionFactor(rot*view, view);
		}
		float w = 1.f;
		if (R > 0.f)
		{
			// A point of the image of the aperture (golden-angle spiral, even density), as a small turn of the view
			const float rr = std::sqrt(fi/static_cast<float>(n));
			const float a = static_cast<float>(i)*2.39996323f;
			float scale = 1.f;
			if (blades > 0 && straightness > 0.f)
			{
				const float d = std::fmod(std::fmod(a - rotation, sector) + sector, sector) - 0.5f*sector;
				scale = 1.f / (1.f + (std::cos(d)/std::cos(M_PIf/static_cast<float>(blades)) - 1.f)*straightness);
			}
			const double dx = static_cast<double>(sign*R*rr*scale*std::cos(a));
			const double dy = static_cast<double>(sign*R*rr*scale*std::sin(a));
			rot = Mat4d::yrotation(-dx/pixelPerRad) * Mat4d::xrotation(dy/pixelPerRad) * rot;
			w = qMax(0.f, 1.f + sa*(rr*rr - 0.5f));
		}
		weights += w;
		samples.push_back({rot, factor*w});
	}
	if (!(weights > 0.f))
	{
		samples.clear();
		return samples;
	}
	for (auto& s : samples)
		s.factor /= weights;
	return samples;
}

StelProjectorP StelSkyDrawer::getPhotoImageSampleProjector(const StelProjectorP& prj, const Mat4d& viewRotation) const
{
	// The turn in view coordinates, expressed in the frame of the model: M^T R M (M: the rotation of the model view)
	StelProjector::ModelViewTranformP modelView = prj->getModelViewTransform()->clone();
	Mat4d m = modelView->getApproximateLinearTransfo();
	m[12] = m[13] = m[14] = 0.;
	modelView->combine(m.transpose() * viewRotation * m);
	return core->getProjection(modelView, static_cast<StelCore::ProjectionType>(1000));
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

bool StelSkyDrawer::beginPhotoGroundImage()
{
	PhotoGroundImage& im = photoGroundImage;
	im.active = false;
	if (!photoGroundTrailActive || !getFlagPhotographicModeActive())
		return false;
	auto& gl = *QOpenGLContext::currentContext()->extraFunctions();
	GLint viewport[4];
	gl.glGetIntegerv(GL_VIEWPORT, viewport);
	// The image covers the window up to the viewport: texel = window pixel, nothing else changes
	const int w = viewport[0] + viewport[2], h = viewport[1] + viewport[3];
	if (w <= 0 || h <= 0 || w > 16384 || h > 16384)
		return false;

	// The composite program depends on the projection (its unprojection and projection functions)
	const StelProjector::ModelViewTranformP identity(new StelProjector::Mat4dTransform(Mat4d::identity(), Mat4d::identity()));
	const StelProjectorP prj = core->getProjection(identity);
	if (!photoGroundProgram || !photoGroundProjector || !prj->isSameProjection(*photoGroundProjector))
	{
		photoGroundProgram.reset();
		photoGroundProjector = prj;
		const char* vsrc = R"(
ATTRIBUTE highp vec2 position; // window coordinates
uniform highp vec4 viewport;
VARYING highp vec2 vWin;
void main(void)
{
	vWin = position;
	gl_Position = vec4((position - viewport.xy) / viewport.zw * 2. - 1., 0., 1.);
}
)";
		// The unprojection and the projection both declare some uniforms: keep the first declaration only
		const QByteArray unproject = prj->getUnProjectShader();
		QByteArray project;
		for (const QByteArray& line : prj->getProjectShader().split('\n'))
		{
			if (!(line.trimmed().startsWith("uniform ") && unproject.contains(line.trimmed())))
				project += line + '\n';
		}
		const QByteArray fsrc = unproject + project + R"(
VARYING highp vec2 vWin;
uniform sampler2D tex;
uniform highp vec2 texSize;
uniform highp vec3 axis;    // rotation axis of the sky (view coordinates)
uniform highp float angle;  // angle it turns during the exposure
uniform int samples;
void main(void)
{
	bool ok = false;
	highp vec3 dir = unProject(vWin.x, vWin.y, ok);
	highp vec2 here = clamp(vWin, vec2(0.5), texSize - 0.5) / texSize;
	if (!ok)
	{
		FRAG_COLOR = texture2D(tex, here);
		return;
	}
	dir = normalize(dir);
	// Nothing of the landscape within reach of the arc (the sky, zoomed in): skip the samples. A coarse level of
	// the mipmaps around this pixel, larger than the arc, holds the mean of everything the arc can reach.
	highp float reach = angle * PROJECTOR_pixelPerRad * length(cross(axis, dir));
	if (dot(textureLod(tex, here, log2(max(1., 2.*reach)) + 1.), vec4(1.)) <= 0.)
	{
		FRAG_COLOR = vec4(0.);
		return;
	}
	// The camera turns with the sky: this pixel sees the ground that turns past it. The samples read
	// a correspondingly smoothed image (mipmaps) where they lie far apart.
	highp float spacing = reach / float(samples);
	highp float lod = log2(max(1., spacing));
	highp vec4 sum = vec4(0.);
	for (int i = 0; i < 256; ++i)
	{
		if (i >= samples)
			break;
		highp float a = angle * ((float(i) + 0.5)/float(samples) - 0.5);
		highp float c = cos(a), s = sin(a);
		highp vec3 d = dir*c + cross(axis, dir)*s + axis*(dot(axis, dir)*(1. - c));
		highp vec3 win = project(d);
		// outside the image: the nearest edge stands for what lies beyond the frame
		highp vec2 p = clamp(win.xy, vec2(0.5), texSize - 0.5);
		sum += textureLod(tex, p / texSize, lod);
	}
	FRAG_COLOR = sum / float(samples); // premultiplied: the ground covers the sky a part of the time
}
)";
		auto program = std::make_unique<QOpenGLShaderProgram>();
		if (program->addShaderFromSourceCode(QOpenGLShader::Vertex, StelOpenGL::globalShaderPrefix(StelOpenGL::VERTEX_SHADER) + vsrc) &&
		    program->addShaderFromSourceCode(QOpenGLShader::Fragment, StelOpenGL::globalShaderPrefix(StelOpenGL::FRAGMENT_SHADER) + fsrc))
		{
			program->bindAttributeLocation("position", 0);
			if (StelPainter::linkProg(program.get(), "photographic ground image"))
				photoGroundProgram = std::move(program);
		}
		if (!photoGroundProgram)
			qWarning() << "Photographic mode: the landscape cannot be blurred for the star tracker";
	}
	if (!photoGroundProgram)
		return false;
	if (!photoGroundFbo || photoGroundFbo->width() != w || photoGroundFbo->height() != h)
	{
		QOpenGLFramebufferObjectFormat format;
		format.setAttachment(QOpenGLFramebufferObject::Depth);
		format.setInternalTextureFormat(GL_RGBA16F);
		format.setMipmap(true);
		photoGroundFbo.reset(new QOpenGLFramebufferObject(w, h, format));
		if (!photoGroundFbo->isValid())
		{
			photoGroundFbo.reset();
			return false;
		}
	}
	gl.glGetIntegerv(GL_FRAMEBUFFER_BINDING, &im.previousFbo);
	for (int i=0; i<4; ++i)
		im.viewport[i] = viewport[i];
	gl.glGetFloatv(GL_COLOR_CLEAR_VALUE, im.clearColor);
	photoGroundFbo->bind();
	gl.glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
	gl.glClearColor(0.f, 0.f, 0.f, 0.f);
	gl.glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	im.active = true;
	return true;
}

void StelSkyDrawer::endPhotoGroundImage()
{
	PhotoGroundImage& im = photoGroundImage;
	if (!im.active)
		return;
	im.active = false;
	auto& gl = *QOpenGLContext::currentContext()->extraFunctions();
	gl.glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(im.previousFbo));
	gl.glViewport(im.viewport[0], im.viewport[1], im.viewport[2], im.viewport[3]);
	gl.glClearColor(im.clearColor[0], im.clearColor[1], im.clearColor[2], im.clearColor[3]);

	gl.glActiveTexture(GL_TEXTURE0);
	gl.glBindTexture(GL_TEXTURE_2D, photoGroundFbo->texture());
	gl.glGenerateMipmap(GL_TEXTURE_2D);
	gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
	gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	// About one sample per pixel of the longest arc (90 degrees from the pole)
	const double pixelPerRad = static_cast<double>(photoGroundProjector->getPixelPerRadAtCenter());
	const int samples = qBound(2, static_cast<int>(std::ceil(photoTrailAngle*pixelPerRad)), 256);

	auto& prog = *photoGroundProgram;
	prog.bind();
	photoGroundProjector->setUnProjectUniforms(prog);
	photoGroundProjector->setProjectUniforms(prog);
	prog.setUniformValue("viewport", QVector4D(im.viewport[0], im.viewport[1], im.viewport[2], im.viewport[3]));
	prog.setUniformValue("tex", 0);
	prog.setUniformValue("texSize", QVector2D(photoGroundFbo->width(), photoGroundFbo->height()));
	prog.setUniformValue("axis", QVector3D(static_cast<float>(photoTrailAxisView[0]), static_cast<float>(photoTrailAxisView[1]), static_cast<float>(photoTrailAxisView[2])));
	prog.setUniformValue("angle", static_cast<float>(photoTrailAngle));
	prog.setUniformValue("samples", samples);

	const float x0 = static_cast<float>(im.viewport[0]), y0 = static_cast<float>(im.viewport[1]);
	const float x1 = x0 + static_cast<float>(im.viewport[2]), y1 = y0 + static_cast<float>(im.viewport[3]);
	const GLfloat quad[] = { x0, y0,   x1, y0,   x0, y1,   x1, y1 };
	GLint previousVao = 0;
	gl.glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &previousVao);
	GLint previousBuffer = 0;
	gl.glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &previousBuffer);
	if (!photoBodyVao)
	{
		photoBodyVao.reset(new QOpenGLVertexArrayObject);
		photoBodyVao->create();
	}
	photoBodyVao->bind();
	GLuint vbo = 0;
	gl.glGenBuffers(1, &vbo);
	gl.glBindBuffer(GL_ARRAY_BUFFER, vbo);
	gl.glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STREAM_DRAW);
	gl.glEnableVertexAttribArray(0);
	gl.glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
	const GLboolean blend = gl.glIsEnabled(GL_BLEND);
	const GLboolean depth = gl.glIsEnabled(GL_DEPTH_TEST);
	GLint blendSrcRGB, blendDstRGB, blendSrcA, blendDstA;
	gl.glGetIntegerv(GL_BLEND_SRC_RGB, &blendSrcRGB);
	gl.glGetIntegerv(GL_BLEND_DST_RGB, &blendDstRGB);
	gl.glGetIntegerv(GL_BLEND_SRC_ALPHA, &blendSrcA);
	gl.glGetIntegerv(GL_BLEND_DST_ALPHA, &blendDstA);
	gl.glEnable(GL_BLEND);
	gl.glDisable(GL_DEPTH_TEST);
	gl.glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	gl.glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	gl.glBlendFuncSeparate(static_cast<GLenum>(blendSrcRGB), static_cast<GLenum>(blendDstRGB), static_cast<GLenum>(blendSrcA), static_cast<GLenum>(blendDstA));
	if (!blend) gl.glDisable(GL_BLEND);
	if (depth) gl.glEnable(GL_DEPTH_TEST);
	gl.glDisableVertexAttribArray(0);
	gl.glDeleteBuffers(1, &vbo);
	gl.glBindVertexArray(static_cast<GLuint>(previousVao));
	gl.glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(previousBuffer));
	prog.release();
}

void StelSkyDrawer::setFlagPhotoStarTrails(bool b)
{
	if (b == flagPhotoStarTrails)
		return;
	flagPhotoStarTrails = b;
	StelApp::immediateSave("viewing/flag_photographic_star_trails", b);
	emit flagPhotoStarTrailsChanged(b);
}

Vec3f StelSkyDrawer::photoTrailExtinctionFactor(const Vec3d& view, const Vec3d& viewRef) const
{
	if (!photoTrailExtinction || !photoTrailAltAzTransform)
		return Vec3f(1.f);
	// Geometric altitude/azimuth, as for the extinction of stars and planets (see ZoneArray, Planet)
	Vec3d altAz = view;
	Vec3d altAzRef = viewRef;
	photoTrailAltAzTransform->backward(altAz);
	photoTrailAltAzTransform->backward(altAzRef);
	altAz.normalize();
	altAzRef.normalize();
	// Below the horizon (from about -2 degrees) the light is gone, whatever the setting for the extinction there
	auto extinctionMag = [this](const Vec3d& aa) -> float {
		if (aa[2] < -0.035)
			return 42.f*extinction.getExtinctionCoefficient();
		float mag = 0.f;
		extinction.forward(aa, &mag);
		return mag;
	};
	const float brightness = std::pow(10.f, -0.4f*(extinctionMag(altAz) - extinctionMag(altAzRef)));
	Vec3f factor(brightness);
	if (getFlagPhotoExtinctionColorActive())
	{
		const Vec3f tint = getPhotoExtinctionTint(altAz);
		const Vec3f tintRef = getPhotoExtinctionTint(altAzRef);
		for (int i=0; i<3; ++i)
			factor[i] *= tintRef[i] > 0.f ? tint[i]/tintRef[i] : 1.f;
	}
	return factor;
}

float StelSkyDrawer::computePhotoTrail(const StelProjector& prj, const Vec3d& v, const Vec3d& win, float flux, float diskRadius, PhotoStarInstance& instance) const
{
	Vec4f& trail = instance.trail;
	instance.trailStart.set(1.f, 1.f, 1.f);
	instance.trailEnd.set(1.f, 1.f, 1.f);
	// The integrated profiles of the trails in the shader (moffatSegment3/15) are written for these exponents
	static_assert(PHOTO_CORE_BETA == 3.f && PHOTO_HALO_BETA == 1.5f, "star trail profiles need beta 3 and 1.5");
	trail.set(0.f, 0.f, 0.f, 0.f);
	if (!photoTrailsActive && !photoGroundTrailActive)
		return 0.f;
	const auto mv = prj.getModelViewTransform();
	Vec3d view = v;
	mv->forward(view);
	const double norm = view.norm();
	if (!(norm > 0.))
		return 0.f;
	// A body that moves against the stars (the Moon): its own motion over half the exposure, in view coordinates
	Vec3d halfMotion(0.);
	if (photoBodyMotion.normSquared() > 0.)
	{
		Mat4d linear = mv->getApproximateLinearTransfo();
		linear[12] = linear[13] = linear[14] = 0.;
		halfMotion = linear.multiplyWithoutTranslation(photoBodyMotion) * (0.5*getPhotoEffectiveExposureTime()*norm);
	}
	// On a star tracker the stars stay in their place: only a body's own motion makes a trail. The altitude still
	// changes during the exposure, and with it the extinction: without a trail the light is its mean (Simpson's
	// rule over the start, the middle and the end), with one its change along the trail (as for the star trails).
	const bool tracker = photoGroundTrailActive;
	float estimate;
	if (tracker)
	{
		estimate = static_cast<float>(2.*halfMotion.norm()/norm * prj.getPixelPerRadAtCenter());
		if (estimate <= 2.f*diskRadius || estimate < 0.5f)
		{
			if (photoTrailExtinction)
			{
				const Vec3f fStart = photoTrailExtinctionFactor(photoTrailToStart*view, view);
				const Vec3f fEnd = photoTrailExtinctionFactor(photoTrailToEnd*view, view);
				for (int c=0; c<3; ++c)
					instance.color[c] *= (fStart[c] + 4.f + fEnd[c])/6.f;
			}
			return 0.f;
		}
	}
	else
	{
		// Quick estimate first (the arc near the image center): most faint stars drop out here, before projecting
		estimate = static_cast<float>(photoTrailAngle * (photoTrailAxisView^view).norm()/norm * prj.getPixelPerRadAtCenter());
	}
	if (estimate <= 2.f*diskRadius || estimate < 0.5f)
		return 0.f;
	// The brightest point of the trailed image: along the trail the core is spread like flux/length, across it
	// keeps its profile (peak 0.75/alpha for a Moffat profile with beta 3); a blur disk widens it.
	// The halo the same way (peak 1/(pi alpha) for beta 1.5). The spikes are kept as they are (conservative).
	{
		const float alpha = qMax(photoCoreAlpha, 1.5f*std::fabs(photoLensCache.defocus));
		const float corePeak = flux*getPhotoCoreFraction()*0.75f/(alpha*estimate);
		const float haloPeak = getFlagPhotoBloomActive() ? 0.f : flux*photoHaloFraction/(M_PIf*photoCoreAlpha*PHOTO_HALO_WIDTH_FACTOR*estimate);
		bool visible = corePeak > photoVisibleThreshold || haloPeak > photoVisibleThreshold;
		if (!visible && getPhotoSpikeFractionEffective() > 0.f)
		{
			const float sigma = photoCoreAlpha*PHOTO_SPIKE_WIDTH_FACTOR;
			const float length = getPhotoSpikeLength();
			const float peak = flux*getPhotoSpikeFractionEffective() / (static_cast<float>(getPhotoSpikeLines()) * std::sqrt(2.f*M_PIf) * sigma * M_PIf * length);
			visible = peak > photoVisibleThreshold && !(getFlagPhotoBloomActive() && length*std::sqrt(peak/photoVisibleThreshold - 1.f) <= StelPhotoBloom::NEAR_SPIKE_LENGTH);
		}
		if (!visible)
			return -1.f;
	}
	// Positions at the start and the end of the exposure: the body's own motion, then (fixed tripod) the turn of
	// the sky around its axis; then project
	Vec3d a = view - halfMotion;
	Vec3d b = view + halfMotion;
	if (!tracker)
	{
		a = photoTrailToStart*a;
		b = photoTrailToEnd*b;
	}
	mv->backward(a);
	mv->backward(b);
	Vec3d winA, winB;
	if (!prj.project(a, winA) || !prj.project(b, winB))
		return 0.f;
	trail.set(static_cast<float>(winA[0]-win[0]), static_cast<float>(winA[1]-win[1]),
	          static_cast<float>(winB[0]-win[0]), static_cast<float>(winB[1]-win[1]));
	const float length = std::hypot(trail[0], trail[1]) + std::hypot(trail[2], trail[3]);
	if (!(length > 0.5f) || length <= 2.f*diskRadius || !std::isfinite(length))
	{
		trail.set(0.f, 0.f, 0.f, 0.f);
		return 0.f;
	}
	// The altitude changes along the trail, and so do the extinction and its color (e.g. a rising planet,
	// red at first). The image at mid-exposure has its own; the start and the end relative to it.
	if (photoTrailExtinction)
	{
		instance.trailStart = photoTrailExtinctionFactor(photoTrailToStart*view, view);
		instance.trailEnd = photoTrailExtinctionFactor(photoTrailToEnd*view, view);
	}
	return length;
}

float StelSkyDrawer::getPhotoApertureShapeArea() const
{
	// Disk, or regular polygon of circumradius 1 (the straightness mixes them, see the shader)
	const int blades = photoApertureBlades >= 3 ? photoApertureBlades : 0;
	if (blades == 0)
		return M_PIf;
	const float n = static_cast<float>(blades);
	const float t = getPhotoApertureStraightness();
	return (1.f-t)*M_PIf + t*0.5f*n*std::sin(2.f*M_PIf/n);
}

float StelSkyDrawer::photoDefocusCoreRadius(float coreFlux, float R) const
{
	// The same blend as in the shader: below the PSF core size the blur only widens the core
	const float fwhm = photoCoreFwhm();
	const float x = qBound(0.f, (R/fwhm - 0.5f)/1.5f, 1.f);
	const float t = x*x*(3.f - 2.f*x);
	const float edge = photoDefocusEdge();
	float peakMoffat = 0.f, alphaWide = photoCoreAlpha;
	if (t < 1.f)
	{
		const float k = 1.6f*R/fwhm;
		alphaWide = photoCoreAlpha*std::sqrt(1.f + k*k);
		peakMoffat = (1.f-t) * coreFlux * (PHOTO_CORE_BETA-1.f) / (M_PIf*alphaWide*alphaWide);
	}
	// The brightest part of the disk: its center or its rim (spherical aberration)
	const float sa = photoLensCache.sphericalAberration;
	const float peakDisk = t * coreFlux * (1.f + 0.5f*sa) / (getPhotoApertureShapeArea()*R*R);
	if (!(peakMoffat + peakDisk > photoVisibleThreshold))
		return 0.f;
	float radius = 0.f;
	if (peakMoffat > photoVisibleThreshold)
		radius = alphaWide * std::sqrt(std::pow(peakMoffat/photoVisibleThreshold, 1.f/PHOTO_CORE_BETA) - 1.f);
	// The whole disk with its blurred edge must lie inside the part of the sprite that is not faded out
	// (the fade starts at 3/4 of the radius; the callers divide by 0.85)
	return qMax(radius, (R + 2.f*edge) * (0.85f/0.75f));
}

void StelSkyDrawer::setFlagPhotoVignetting(bool b)
{
	if (b == flagPhotoVignetting)
		return;
	flagPhotoVignetting = b;
	StelApp::immediateSave("viewing/flag_photographic_vignetting", b);
	emit flagPhotoVignettingChanged(b);
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

void StelSkyDrawer::addPhotoGhostSource(const Vec3d& v, float mag, const Vec3f& color)
{
	if (flagPhotoLensFlare && photoLensFlareStrength > 0.f && getFlagPhotographicModeActive())
		photoGhostSources.push_back({v, mag, color});
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
	conf->setValue("viewing/photographic_exposure_ev", getPhotoExposureEV());
	conf->setValue("viewing/photographic_exposure_time", getPhotoExposureTime());
	conf->setValue("viewing/photographic_aperture", photoProgramRunning ? photoUserAperture : photoAperture);
	conf->setValue("viewing/photographic_iso", photoFilm != PhotoFilmNone ? photoDigitalISO : (photoProgramRunning ? photoUserISO : photoISO));
	conf->setValue("viewing/photographic_film", photoFilm);
	conf->setValue("viewing/photographic_film_push", photoFilmPush);
	conf->setValue("viewing/flag_photographic_zoom_whole_millimeters", getFlagPhotoZoomWholeMillimeters());
	conf->setValue("viewing/flag_photographic_portrait", getFlagPhotoPortrait());
	conf->setValue("viewing/photographic_eclipse_atmosphere", getPhotoEclipseAtmosphere());
	conf->setValue("viewing/flag_photographic_frame_mask", getFlagPhotoFrameMask());
	conf->setValue("viewing/photographic_sensor_format", getPhotoSensorFormat());
	conf->setValue("viewing/photographic_focus_distance", getPhotoFocusDistance());
	conf->setValue("viewing/photographic_noise", getPhotoNoise());
	conf->setValue("viewing/photographic_mount", getPhotoMount());
	conf->setValue("viewing/flag_photographic_star_trails", getFlagPhotoStarTrails());
	conf->setValue("viewing/flag_photographic_vignetting", getFlagPhotoVignetting());
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
	conf->setValue("viewing/photographic_program_max_iso", getPhotoProgramMaxISO());
	conf->setValue("viewing/photographic_program_max_time", getPhotoProgramMaxTime());
}

// ---- White balance ----

namespace
{
typedef std::array<double, 9> M3; // row-major 3x3
M3 mul(const M3& a, const M3& b)
{
	M3 r{};
	for (int i=0; i<3; ++i)
		for (int j=0; j<3; ++j)
			r[3*i+j] = a[3*i]*b[j] + a[3*i+1]*b[3+j] + a[3*i+2]*b[6+j];
	return r;
}
Vec3d mul(const M3& a, const Vec3d& v)
{
	return Vec3d(a[0]*v[0]+a[1]*v[1]+a[2]*v[2], a[3]*v[0]+a[4]*v[1]+a[5]*v[2], a[6]*v[0]+a[7]*v[1]+a[8]*v[2]);
}
M3 inverse(const M3& m)
{
	const double c00 = m[4]*m[8]-m[5]*m[7], c01 = m[5]*m[6]-m[3]*m[8], c02 = m[3]*m[7]-m[4]*m[6];
	const double det = m[0]*c00 + m[1]*c01 + m[2]*c02;
	if (std::fabs(det) < 1e-12)
		return M3{1,0,0, 0,1,0, 0,0,1};
	const double d = 1./det;
	return M3{c00*d, (m[2]*m[7]-m[1]*m[8])*d, (m[1]*m[5]-m[2]*m[4])*d,
	          c01*d, (m[0]*m[8]-m[2]*m[6])*d, (m[2]*m[3]-m[0]*m[5])*d,
	          c02*d, (m[1]*m[6]-m[0]*m[7])*d, (m[0]*m[4]-m[1]*m[3])*d};
}
// Linear sRGB <-> CIE XYZ (D65), and the Bradford cone response
const M3 RGB_TO_XYZ{0.4124564, 0.3575761, 0.1804375, 0.2126729, 0.7151522, 0.0721750, 0.0193339, 0.1191920, 0.9503041};
const M3 XYZ_TO_RGB{3.2404542, -1.5371385, -0.4985314, -0.9692660, 1.8760108, 0.0415560, 0.0556434, -0.2040259, 1.0572252};
const M3 BRADFORD{0.8951, 0.2664, -0.1614, -0.7502, 1.7135, 0.0367, 0.0389, -0.0685, 1.0296};
const Vec3d D65_XYZ(0.95047, 1., 1.08883);

//! Chromaticity of light of a color temperature: the Planckian locus (Kim et al. 2002) for warm light,
//! the CIE daylight locus from 5000 K on (so that 6504 K is D65, like the daylight presets of cameras),
//! blended between 4000 and 5000 K.
Vec3d kelvinToXYZ(double T)
{
	T = std::clamp(T, 1667., 25000.);
	const double t1 = 1e3/T, t2 = t1*t1, t3 = t2*t1;
	// Planckian locus
	double xp = (T < 4000.) ? -0.2661239*t3 - 0.2343589*t2 + 0.8776956*t1 + 0.179910
	                        : -3.0258469*t3 + 2.1070379*t2 + 0.2226347*t1 + 0.240390;
	double yp;
	if (T < 2222.)
		yp = -1.1063814*xp*xp*xp - 1.34811020*xp*xp + 2.18555832*xp - 0.20219683;
	else if (T < 4000.)
		yp = -0.9549476*xp*xp*xp - 1.37418593*xp*xp + 2.09137015*xp - 0.16748867;
	else
		yp = 3.0817580*xp*xp*xp - 5.87338670*xp*xp + 3.75112997*xp - 0.37001483;
	double x = xp, y = yp;
	if (T > 4000.)
	{
		// CIE daylight locus
		const double xd = (T <= 7000.) ? -4.6070*t3 + 2.9678*t2 + 0.09911*t1 + 0.244063
		                               : -2.0064*t3 + 1.9018*t2 + 0.24748*t1 + 0.237040;
		const double yd = -3.000*xd*xd + 2.870*xd - 0.275;
		const double w = std::clamp((T - 4000.)/1000., 0., 1.);
		x = (1.-w)*xp + w*xd;
		y = (1.-w)*yp + w*yd;
	}
	return Vec3d(x/y, 1., (1.-x-y)/y);
}
}

double StelSkyDrawer::photoWhiteBalancePresetKelvin(int wb)
{
	switch (wb)
	{
		case PhotoWBDaylight:    return 5200.;
		case PhotoWBCloudy:      return 6000.;
		case PhotoWBShade:       return 7000.;
		case PhotoWBTungsten:    return 3200.;
		case PhotoWBFluorescent: return 4000.;
		case PhotoWBFlash:       return 5500.;
		case PhotoWBNeutral:     return 6504.;
		default:                 return 0.;
	}
}

double StelSkyDrawer::getPhotoWhiteBalanceTemperature() const
{
	// Daylight color films are balanced for about 5500 K
	if (photoFilm != PhotoFilmNone)
		return 5500.;
	if (photoWhiteBalance == PhotoWBAuto)
		return photoAutoWhiteBalanceKelvin;
	if (photoWhiteBalance == PhotoWBCustom)
		return photoWhiteBalanceKelvin;
	return photoWhiteBalancePresetKelvin(photoWhiteBalance);
}

bool StelSkyDrawer::getFlagPhotoAutoWhiteBalanceActive() const
{
	return photoWhiteBalance == PhotoWBAuto && photoFilm == PhotoFilmNone && getFlagPhotographicModeActive();
}

double StelSkyDrawer::getPhotoMeterClipLevel() const
{
	const double clip = (getPhotoActiveToneCurve() == PhotoToneLinear) ? 1. : static_cast<double>(photoToneParams.clip);
	return clip / static_cast<double>(getPhotoExposureFactor());
}

double StelSkyDrawer::photoKelvinForBlueRedRatio(double ratio)
{
	// The blue/red ratio of white light rises with the color temperature: bisection in log(T)
	const auto whiteRatio = [](double T) {
		const Vec3d rgb = mul(XYZ_TO_RGB, kelvinToXYZ(T));
		return rgb[2]/rgb[0];
	};
	double lo = std::log(PHOTO_AWB_MIN_KELVIN), hi = std::log(PHOTO_AWB_MAX_KELVIN);
	if (!(ratio > whiteRatio(PHOTO_AWB_MIN_KELVIN)))
		return PHOTO_AWB_MIN_KELVIN;
	if (!(ratio < whiteRatio(PHOTO_AWB_MAX_KELVIN)))
		return PHOTO_AWB_MAX_KELVIN;
	for (int i=0; i<40; ++i)
	{
		const double mid = 0.5*(lo+hi);
		if (whiteRatio(std::exp(mid)) < ratio)
			lo = mid;
		else
			hi = mid;
	}
	return std::exp(0.5*(lo+hi));
}

void StelSkyDrawer::updatePhotoWhiteBalanceMatrix()
{
	const double T = getPhotoWhiteBalanceTemperature();
	if (std::fabs(T - photoWBMatrixKelvin) < 0.5)
		return;
	photoWBMatrixKelvin = T;
	M3 m{1,0,0, 0,1,0, 0,0,1};
	if (photoWhiteBalance != PhotoWBNeutral || photoFilm != PhotoFilmNone)
	{
		// Von Kries adaptation in Bradford cone space: light of temperature T becomes the display white (D65).
		// The light of temperature T at luminance 1 stays at luminance 1.
		const Vec3d src = mul(BRADFORD, kelvinToXYZ(T));
		const Vec3d dst = mul(BRADFORD, D65_XYZ);
		const M3 gains{dst[0]/src[0], 0, 0, 0, dst[1]/src[1], 0, 0, 0, dst[2]/src[2]};
		m = mul(XYZ_TO_RGB, mul(inverse(BRADFORD), mul(gains, mul(BRADFORD, RGB_TO_XYZ))));
	}
	const M3 inv = inverse(m);
	for (int i=0; i<9; ++i)
	{
		photoWBMatrix[i] = static_cast<float>(m[static_cast<size_t>(i)]);
		photoWBInverse[i] = static_cast<float>(inv[static_cast<size_t>(i)]);
	}
}

void StelSkyDrawer::setPhotoWhiteBalance(int wb)
{
	wb = qBound(0, wb, 8);
	if (wb == photoWhiteBalance)
		return;
	photoWhiteBalance = wb;
	photoAutoWhiteBalanceFresh = true;
	photoWBMatrixKelvin = -1.; // e.g. neutral and 6504 K differ slightly
	updatePhotoWhiteBalanceMatrix();
	StelApp::immediateSave("viewing/photographic_white_balance", wb);
	emit photoWhiteBalanceChanged(wb);
}

void StelSkyDrawer::setPhotoWhiteBalanceKelvin(double kelvin)
{
	kelvin = qBound(2000., kelvin, 12000.);
	if (qFuzzyCompare(kelvin, photoWhiteBalanceKelvin))
		return;
	photoWhiteBalanceKelvin = kelvin;
	updatePhotoWhiteBalanceMatrix();
	StelApp::immediateSave("viewing/photographic_white_balance_kelvin", kelvin);
	emit photoWhiteBalanceKelvinChanged(kelvin);
}

// ---- Extinction colors ----

void StelSkyDrawer::updatePhotoExtinction()
{
	const double k = static_cast<double>(extinction.getExtinctionCoefficient());
	photoExtinctionActive = flagPhotoExtinctionColor && getFlagPhotographicModeActive() && getFlagHasAtmosphere()
	                        && k >= 0.01 && atmosphereTransmission.isLoaded()
	                        && core->getCurrentLocation().planetName == QStringLiteral("Earth");
	if (!photoExtinctionActive)
		return;
	const double altitude = static_cast<double>(core->getCurrentLocation().altitude);
	if (altitude == photoExtinctionAltitude && k == photoExtinctionCoefficient && !photoExtinctionTints.empty())
		return;
	photoExtinctionAltitude = altitude;
	photoExtinctionCoefficient = k;

	// The table has its own amount of aerosols etc. Scale its optical depths so that white light loses
	// as much towards the zenith as the extinction coefficient says (k magnitudes per airmass at the site):
	// the brightness keeps following the coefficient, the table gives the color.
	const double zenith = atmosphereTransmission.zenithMagnitude(altitude);
	const double depthScale = zenith > 1e-4 ? k/zenith : 1.;
	const double sinMin = atmosphereTransmission.horizonSine(altitude);
	photoExtinctionSinMin = static_cast<float>(sinMin);
	constexpr int size = 256;
	photoExtinctionTints.assign(size, Vec3f(1.f));
	// From the zenith down: very close to the horizon of the observer hardly any light passes;
	// there the last good color is kept.
	Vec3f last(1.f);
	for (int i = size-1; i >= 0; --i)
	{
		const double u = static_cast<double>(i)/(size-1);
		const double s = sinMin + (1.-sinMin)*u*u;
		const Vec3d t = atmosphereTransmission.transmission(altitude, std::asin(std::clamp(s, -1., 1.)), depthScale);
		const double Y = 0.2126*t[0] + 0.7152*t[1] + 0.0722*t[2];
		if (Y > 1e-7)
			last = Vec3f(static_cast<float>(t[0]/Y), static_cast<float>(t[1]/Y), static_cast<float>(t[2]/Y));
		photoExtinctionTints[static_cast<size_t>(i)] = last;
	}
	photoExtinctionTextureDirty = true;
}

void StelSkyDrawer::setFlagPhotoExtinctionColor(bool b)
{
	if (b == flagPhotoExtinctionColor)
		return;
	flagPhotoExtinctionColor = b;
	StelApp::immediateSave("viewing/flag_photographic_extinction_color", b);
	emit flagPhotoExtinctionColorChanged(b);
}

void StelSkyDrawer::setFlagPhotoAirglow(bool b)
{
	if (b == flagPhotoAirglow)
		return;
	flagPhotoAirglow = b;
	StelApp::immediateSave("viewing/flag_photographic_airglow", b);
	emit flagPhotoAirglowChanged(b);
}

void StelSkyDrawer::setFlagPhotoFilmSpectralSky(bool b)
{
	if (b == flagPhotoFilmSpectralSky)
		return;
	flagPhotoFilmSpectralSky = b;
	StelApp::immediateSave("astro/flag_photographic_film_spectral_sky", b);
	emit flagPhotoFilmSpectralSkyChanged(b);
}

void StelSkyDrawer::setPhotoAirglowStrength(double strength)
{
	const float v = qBound(0.25f, static_cast<float>(strength), 2.f);
	if (qFuzzyCompare(1.f+v, 1.f+photoAirglowStrength))
		return;
	photoAirglowStrength = v;
	StelApp::immediateSave("viewing/photographic_airglow_strength", static_cast<double>(v));
	emit photoAirglowStrengthChanged(static_cast<double>(v));
}

double StelSkyDrawer::getPhotoAirglowZenithLuminance() const
{
	if (!flagPhotoAirglow || !getFlagPhotographicModeActive() || !getFlagHasAtmosphere()
	    || core->getCurrentLocation().planetName != QStringLiteral("Earth"))
		return 0.;
	// The solar cycle, as in Skybright (Schaefer 1998): brightest near the maxima (1990, 2001, 2012...)
	int year, month, day;
	StelUtils::getDateFromJulianDay(core->getJD(), &year, &month, &day);
	const double y = year + (month - 0.5)/12.;
	const double cycle = 1. + 0.3*std::cos(0.57118*(y - 1992.));
	// The surface brightness at the zenith (V, magnitudes per square arc second, as observed through the atmosphere)
	// at a mean solar activity; the cycle changes it by +-30% (about +-0.3 mag), and it varies from night to night
	// (photoAirglowStrength). The whole dark sky is about 21.9-22.0 at the best sites: the airglow, plus the
	// zodiacal light and the starlight that Stellarium draws by themselves.
	// Surface brightness to luminance: L [cd/m^2] = 10.8e4 x 10^(-0.4 mag/arcsec^2)
	constexpr double zenithMagnitude = 22.5;
	return 10.8e4*std::pow(10., -0.4*zenithMagnitude)*cycle*static_cast<double>(photoAirglowStrength);
}

Vec3f StelSkyDrawer::getPhotoAirglow() const
{
	const double zenith = getPhotoAirglowZenithLuminance();
	if (!(zenith > 0.))
		return Vec3f(0.f);
	// Observed at the zenith, so above the atmosphere it is brighter by the extinction of one airmass
	const double k = static_cast<double>(extinction.getExtinctionCoefficient());
	const double luminance = zenith*std::pow(10., 0.4*k);
	// The color: the night sky at dark observatories is much redder than sunlight (B-V about 0.95, V-R about 0.8,
	// against 0.65 and 0.36 for the Sun; Benn & Ellison 1998, Patat 2003): the green line of oxygen (558 nm),
	// sodium (589 nm), the red oxygen lines (630 nm) and the OH bands. As linear sRGB against white sunlight
	// (luminance 1): a warm, brownish grey in daylight white balance.
	Vec3f color(1.227f, 0.959f, 0.748f);
	// A color film sees the lines with its own layers (see getPhotoFilmSpec())
	if (getFlagPhotoFilmActive() && getPhotoFilmSpectral(getPhotoActiveFilmSpec()))
	{
		const float* c = getPhotoActiveFilmSpec().airglow;
		color.set(c[0], c[1], c[2]);
	}
	return color*static_cast<float>(luminance*static_cast<double>(getPhotoSceneScale()));
}

Vec3f StelSkyDrawer::getPhotoAirglowExtinction() const
{
	// Blue is dimmed more than green, red less (Rayleigh scattering and aerosols); luminance weighted it is k
	const float k = extinction.getExtinctionCoefficient();
	return Vec3f(0.75f*k, k, 1.45f*k);
}

Vec3f StelSkyDrawer::getPhotoExtinctionTint(const Vec3d& altAzGeometric) const
{
	if (!photoExtinctionActive || photoExtinctionTints.size() < 2)
		return Vec3f(1.f);
	const double z = altAzGeometric[2] / altAzGeometric.norm();
	const double sinMin = static_cast<double>(photoExtinctionSinMin);
	const double u = std::sqrt(std::clamp((z - sinMin)/(1. - sinMin), 0., 1.));
	const double f = u * static_cast<double>(photoExtinctionTints.size() - 1);
	const size_t i = qMin(static_cast<size_t>(f), photoExtinctionTints.size() - 2);
	const float t = static_cast<float>(f - static_cast<double>(i));
	return photoExtinctionTints[i]*(1.f-t) + photoExtinctionTints[i+1]*t;
}

Vec3f StelSkyDrawer::photoTintedColor(const Vec3f& srgbColor, const Vec3f& linearTint)
{
	const float dim = qMax(srgbColor[0], qMax(srgbColor[1], srgbColor[2]));
	if (!(dim > 0.f))
		return srgbColor;
	const Vec3f lin = srgbToLinear(srgbColor/dim);
	Vec3f tinted(lin[0]*linearTint[0], lin[1]*linearTint[1], lin[2]*linearTint[2]);
	const float m = qMax(tinted[0], qMax(tinted[1], tinted[2]));
	if (!(m > 0.f))
		return srgbColor;
	tinted /= m;
	return Vec3f(encodeSRGB(tinted[0]), encodeSRGB(tinted[1]), encodeSRGB(tinted[2])) * dim;
}

QByteArray StelSkyDrawer::getPhotoExtinctionShader()
{
	return QByteArray(R"(
uniform sampler2D photoExtinctionTintTex;
uniform bool photoExtinctionColorEnabled;
uniform highp float photoExtinctionSinMin;
// Color of the atmospheric extinction (linear RGB at unit luminance), see StelSkyDrawer::getPhotoExtinctionTint()
highp vec3 photoExtinctionTint(highp float sinAltitude)
{
	if (!photoExtinctionColorEnabled)
		return vec3(1);
	highp float u = sqrt(clamp((sinAltitude - photoExtinctionSinMin)/(1. - photoExtinctionSinMin), 0., 1.));
	return texture2D(photoExtinctionTintTex, vec2((u*255. + 0.5)/256., 0.5)).rgb;
}
)");
}

QByteArray StelSkyDrawer::getPhotoSkyTrailShader()
{
	return QByteArray(R"(
uniform int photoSkyTrailSamples;        // number of samples (1: neither trail nor blur)
uniform highp vec3 photoSkyTrailAxis;    // rotation axis of the sky (model frame)
uniform highp float photoSkyTrailAngle;  // angle the sky turns during the exposure (radians), 0: no trail
uniform highp float photoSkyDefocus;     // signed radius of the blur disk of the lens (pixels), 0: sharp
uniform highp float photoSkyDefocusSA;   // spherical aberration: brightness change across the blur disk
uniform int photoSkyBlades;              // 0: round aperture
uniform highp float photoSkyStraightness;
uniform highp float photoSkyBladeRotation;
uniform highp float photoSkyLodBias;     // the samples lie apart: they read a correspondingly smoothed texture
// (texture2D is a macro with two arguments in Stellarium's shader prefix: the version with the bias)
#if __VERSION__ >= 130
#define PHOTO_SKY_TEXTURE(sampler, uv) texture(sampler, uv, photoSkyLodBias)
#else
#define PHOTO_SKY_TEXTURE(sampler, uv) texture2D(sampler, uv, photoSkyLodBias)
#endif
// Sample i of the exposure and of the blur disk of a defocused lens: the offset (window coordinates) of the
// point whose light the lens spreads onto this pixel and its weight (xyz), and the turn of the sky at its time (w)
highp vec4 photoSkySample(int i)
{
	highp float n = float(photoSkyTrailSamples);
	highp float fi = float(i) + 0.5;
	// Time along the exposure: in order for a pure trail, else well spread over the disk (golden ratio)
	highp float t = photoSkyDefocus != 0. ? fract(fi*0.6180339887) : fi/n;
	highp vec2 offset = vec2(0.);
	highp float w = 1.;
	if (photoSkyDefocus != 0.)
	{
		// A point of the image of the aperture (golden-angle spiral, even density)
		highp float rr = sqrt(fi/n);
		highp float a = float(i)*2.39996323;
		highp float scale = 1.;
		if (photoSkyBlades >= 3 && photoSkyStraightness > 0.)
		{
			highp float nb = float(photoSkyBlades);
			highp float sector = 6.28318530718/nb;
			highp float d = mod(a - photoSkyBladeRotation, sector) - 0.5*sector;
			scale = 1. / mix(1., cos(d)/cos(3.14159265359/nb), photoSkyStraightness);
		}
		offset = (photoSkyDefocus > 0. ? -1. : 1.) * abs(photoSkyDefocus) * rr * scale * vec2(cos(a), sin(a));
		w = max(0., 1. + photoSkyDefocusSA*(rr*rr - 0.5));
	}
	return vec4(offset, w, photoSkyTrailAngle*(t - 0.5));
}
// The direction dir turned by the angle a around the rotation axis of the sky
highp vec3 photoSkyTurn(highp vec3 dir, highp float a)
{
	highp float c = cos(a), s = sin(a);
	return dir*c + cross(photoSkyTrailAxis, dir)*s + photoSkyTrailAxis*(dot(photoSkyTrailAxis, dir)*(1. - c));
}
)");
}

void StelSkyDrawer::setPhotoSkyTrailUniforms(QOpenGLShaderProgram& program, const StelProjector::ModelViewTranformP& modelView, double texturePixelsPerRad) const
{
	const bool photo = getFlagPhotographicModeActive();
	const double pixelPerRad = static_cast<double>(core->getProjection(StelCore::FrameJ2000)->getPixelPerRadAtCenter());
	// Detail finer than half a texel is not in the texture: the samples need not be closer
	const double detail = qMin(1., 2.*texturePixelsPerRad/qMax(1e-9, pixelPerRad));
	// The trail: the longest (on the celestial equator) decides, about one sample per screen pixel
	double length = 0.;
	Vec3d axis(0., 0., 1.);
	if (photoTrailsActive && photo)
	{
		// The axis from the view frame to the frame of the texture (the inverse of the rotation; refraction
		// does not turn the sky)
		axis = modelView->getApproximateLinearTransfo().transpose().multiplyWithoutTranslation(photoTrailAxisView);
		if (axis.normSquared() > 0.)
		{
			axis.normalize();
			length = photoTrailAngle * pixelPerRad * detail;
		}
		else
			axis.set(0., 0., 1.);
	}
	// The blur disk of a defocused lens: about one sample per two square pixels (as for the nebula images)
	float defocus = photo ? photoLensCache.defocus : 0.f;
	const double R = std::fabs(static_cast<double>(defocus));
	const double Rdetail = R * detail;
	if (Rdetail < 0.5)
		defocus = 0.f;
	const double area = defocus != 0.f ? 0.5*M_PI*Rdetail*Rdetail : 0.;
	const int samples = qBound(1, static_cast<int>(std::ceil(length + area)), 128);
	if (samples <= 1)
		defocus = 0.f;
	// How much of the screen the samples cover, per sample: the texture is read smoothed to that size
	const double covered = (length > 0. ? photoTrailAngle*pixelPerRad*qMax(1., 2.*R) : 0.) + (defocus != 0.f ? M_PI*R*R : 0.);
	const float lodBias = samples > 1 ? static_cast<float>(qMax(0., 0.5*std::log2(qMax(1., covered/samples)))) : 0.f;
	const int blades = photoApertureBlades >= 3 ? photoApertureBlades : 0;
	const float sector = blades > 0 ? 2.f*M_PIf/static_cast<float>(blades) : 0.f;
	program.setUniformValue("photoSkyTrailSamples", samples);
	program.setUniformValue("photoSkyTrailAxis", QVector3D(static_cast<float>(axis[0]), static_cast<float>(axis[1]), static_cast<float>(axis[2])));
	program.setUniformValue("photoSkyTrailAngle", static_cast<float>(samples > 1 && length > 0. ? photoTrailAngle : 0.));
	program.setUniformValue("photoSkyDefocus", defocus);
	program.setUniformValue("photoSkyDefocusSA", defocus > 0.f ? -photoLensCache.sphericalAberration : photoLensCache.sphericalAberration);
	program.setUniformValue("photoSkyBlades", blades);
	program.setUniformValue("photoSkyStraightness", blades > 0 ? getPhotoApertureStraightness() : 0.f);
	program.setUniformValue("photoSkyBladeRotation", PHOTO_SPIKE_ANGLE - 0.5f*sector);
	program.setUniformValue("photoSkyLodBias", lodBias);
}

void StelSkyDrawer::setPhotoExtinctionUniforms(QOpenGLShaderProgram& program, int textureUnit)
{
	const bool enabled = photoExtinctionActive && photoExtinctionTints.size() == 256;
	program.setUniformValue("photoExtinctionColorEnabled", enabled);
	program.setUniformValue("photoExtinctionSinMin", photoExtinctionSinMin);
	program.setUniformValue("photoExtinctionTintTex", textureUnit);
	if (!enabled)
		return;
	auto& gl = *QOpenGLContext::currentContext()->functions();
	gl.glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(textureUnit));
	if (!photoExtinctionTexture)
	{
		GLuint tex = 0;
		gl.glGenTextures(1, &tex);
		photoExtinctionTexture = tex;
		photoExtinctionTextureDirty = true;
	}
	gl.glBindTexture(GL_TEXTURE_2D, photoExtinctionTexture);
	if (photoExtinctionTextureDirty)
	{
		std::vector<float> data(4*photoExtinctionTints.size());
		for (size_t i=0; i<photoExtinctionTints.size(); ++i)
		{
			data[4*i+0] = photoExtinctionTints[i][0];
			data[4*i+1] = photoExtinctionTints[i][1];
			data[4*i+2] = photoExtinctionTints[i][2];
			data[4*i+3] = 1.f;
		}
		gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, static_cast<GLsizei>(photoExtinctionTints.size()), 1, 0, GL_RGBA, GL_FLOAT, data.data());
		gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		photoExtinctionTextureDirty = false;
	}
	gl.glActiveTexture(GL_TEXTURE0);
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

void StelSkyDrawer::setPhotoPsfMinFwhm(double pixels)
{
	const float v = qBound(0.8f, static_cast<float>(pixels), 10.f);
	if (qFuzzyCompare(v, photoMinFwhmPixels))
		return;
	photoMinFwhmPixels = v;
	StelApp::immediateSave("viewing/photographic_psf_min_fwhm", static_cast<double>(v));
	emit photoPsfMinFwhmChanged(static_cast<double>(v));
}

void StelSkyDrawer::setPhotoSeeing(double arcsec)
{
	const float v = qBound(0.f, static_cast<float>(arcsec), 60.f);
	if (qFuzzyCompare(1.f+v, 1.f+photoSeeingArcsec))
		return;
	photoSeeingArcsec = v;
	StelApp::immediateSave("viewing/photographic_seeing", static_cast<double>(v));
	emit photoSeeingChanged(static_cast<double>(v));
}

void StelSkyDrawer::setPhotoHaloStrength(double fraction)
{
	// A digital camera and a film camera each keep their own value: in front of a sensor lies a stack of filters
	// (infrared cut, antialiasing, cover glass), whose reflections with the sensor add to the glow of the lens;
	// a film lies bare in the focal plane, and has the glow of the lens alone (and its own halation)
	const float v = qBound(0.f, static_cast<float>(fraction), 0.3f);
	if (qFuzzyCompare(1.f+v, 1.f+photoHaloFraction))
		return;
	photoHaloFraction = v;
	if (photoFilm != PhotoFilmNone)
	{
		photoFilmHaloFraction = v;
		StelApp::immediateSave("viewing/photographic_film_halo_strength", static_cast<double>(v));
	}
	else
	{
		photoDigitalHaloFraction = v;
		StelApp::immediateSave("viewing/photographic_halo_strength", static_cast<double>(v));
	}
	emit photoHaloStrengthChanged(static_cast<double>(v));
}

void StelSkyDrawer::setPhotoApertureBlades(int blades)
{
	const int v = (blades<=0) ? 0 : qBound(4, blades, 9);
	if (v==photoApertureBlades)
		return;
	photoApertureBlades = v;
	StelApp::immediateSave("viewing/photographic_aperture_blades", v);
	emit photoApertureBladesChanged(v);
}

float StelSkyDrawer::getPhotoApertureStraightness() const
{
	// Wide open the opening is the round stop of the lens (the blades are retracted); stopping down,
	// the blades form a polygon whose edges get straighter. Fully straight two stops down.
	const float stops = static_cast<float>(std::log2(photoAperture/static_cast<double>(PHOTO_LENS_MAX_APERTURE)));
	const float t = qBound(0.f, stops/2.f, 1.f);
	return t*t*(3.f - 2.f*t);
}

float StelSkyDrawer::getPhotoSpikeFractionEffective() const
{
	// The light that straight edges diffract into the spikes: per unit length along a spike it falls off
	// like (lambda N)/x^2 beyond the diffraction scale (lambda N), so the total stays the same while the
	// spikes get longer with the f-number (see getPhotoSpikeLength()).
	if (photoApertureBlades <= 0)
		return 0.f;
	return photoSpikeFraction * getPhotoApertureStraightness();
}

float StelSkyDrawer::getPhotoSpikeLength() const
{
	// The diffraction pattern scales with lambda N: stopped down, the spikes reach farther
	const float scale = qBound(0.25f, static_cast<float>(photoAperture)/PHOTO_SPIKE_REFERENCE_APERTURE, 4.f);
	return photoCoreAlpha*PHOTO_SPIKE_LENGTH_FACTOR*scale;
}

void StelSkyDrawer::setPhotoSpikeStrength(double fraction)
{
	const float v = qBound(0.f, static_cast<float>(fraction), 0.1f);
	if (qFuzzyCompare(1.f+v, 1.f+photoSpikeFraction))
		return;
	photoSpikeFraction = v;
	StelApp::immediateSave("viewing/photographic_spike_strength", static_cast<double>(v));
	emit photoSpikeStrengthChanged(static_cast<double>(v));
}

void StelSkyDrawer::setFlagPhotoBloom(bool b)
{
	if (b == flagPhotoBloom)
		return;
	flagPhotoBloom = b;
	StelApp::immediateSave("viewing/flag_photographic_bloom", b);
	emit flagPhotoBloomChanged(b);
}

bool StelSkyDrawer::getFlagPhotoBloomActive() const
{
	return flagPhotoBloom && (photoHaloFraction > 0.f || getPhotoSpikeFractionEffective() > 0.f) && getFlagPhotographicModeActive();
}

float StelSkyDrawer::getPhotoCoreFraction() const
{
	const float spikeFraction = getPhotoSpikeFractionEffective();
	return 1.f - photoHaloFraction - spikeFraction;
}

void StelSkyDrawer::resetPhotographicSettings()
{
	setPhotoExposureTime(20.);
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
	setFlagPhotoZoomWholeMillimeters(false);
	setFlagPhotoPortrait(false);
	setFlagPhotoFrameMask(true);
	setPhotoEclipseAtmosphere(1);
}

float StelSkyDrawer::getPhotoSceneScale() const
{
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
	return flagPhotoAutoExposure && getFlagPhotographicModeActive();
}

double StelSkyDrawer::getPhotoEffectiveExposureTime() const
{
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
	const double Nday = PHOTO_PROGRAM_DAY_APERTURE, Nsmall = PHOTO_PROGRAM_MIN_APERTURE;
	const double Nwide = PHOTO_LENS_MAX_APERTURE;
	// exposure = t S / N^2: walk along the line from bright to dark
	double t, N, S;
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
	const bool timeAtLimit = exposure > tLimit*S0/(Nwide*Nwide) && exposure <= tLimit*Smax/(Nwide*Nwide);
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

void StelSkyDrawer::drawPhotoInfoLine(StelCore* core) const
{
	if (!flagPhotoInfoLine || !getFlagPhotographicModeActive())
		return;

	const bool autoExposure = getFlagPhotoAutoExposureActive();
	const double t = getPhotoEffectiveExposureTime();
	// Exposure value at ISO 100. In manual mode the compensation brightens the picture like a lower EV;
	// with the automatic exposure it is already part of the exposure time.
	const double ev100 = std::log2(photoAperture*photoAperture/t) - std::log2(photoISO/100.) - (autoExposure ? 0. : photoExposureEV);
	const QString sep = QStringLiteral("   ");
	QStringList parts;
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
		QString lens = QString("%1 mm").arg(qRound(1000.*getPhotoFocalLength(*core->getProjection(StelCore::FrameJ2000))));
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
	if (propMgr && propMgr->getProperty("EquationOfTime.showEOT") && propMgr->getStelPropertyValue("EquationOfTime.showEOT").toBool())
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
	if (!flagPhotographicMode)
		text += QString(" (%1)").arg(q_("photographic mode is off"));

	const StelProjector::StelProjectorParams params = core->getCurrentStelProjectorParams();
	StelPainter painter(core->getProjection2d());
	const int x = qRound(params.viewportCenter[0] - painter.getFontMetrics().boundingRect(text).width()/2);
	const int y = qRound(params.viewportCenter[1] - params.viewportXywh[3]*params.viewportCenterOffset[1] - painter.getFontMetrics().height()/2);
	photoExposureMessageID = labelMgr->labelScreen(text, x, y, true, StelApp::getInstance().getScreenFontSize() + 3, "#99FF99", true, 2000);
}

void StelSkyDrawer::setExtinctionCoefficient(double extCoeff)
{
	extinction.setExtinctionCoefficient(static_cast<float>(extCoeff));
	StelApp::immediateSave("landscape/atmospheric_extinction_coefficient", extCoeff);
	emit extinctionCoefficientChanged(static_cast<double>(extinction.getExtinctionCoefficient()));
}

void StelSkyDrawer::setAtmosphereTemperature(double celsius)
{
	refraction.setTemperature(static_cast<float>(celsius));
	StelApp::immediateSave("landscape/temperature_C", celsius);
	emit atmosphereTemperatureChanged(static_cast<double>(refraction.getTemperature()));
}

void StelSkyDrawer::setAtmospherePressure(double mbar)
{
	refraction.setPressure(static_cast<float>(mbar));
	StelApp::immediateSave("landscape/pressure_mbar", mbar);
	emit atmospherePressureChanged(static_cast<double>(refraction.getPressure()));
}

// These are to find out the best sky parameters. Program feature for debugging/expert mode.
// These will be connected from AtmosphereDialog and forward the settings to SkyLight class.

void StelSkyDrawer::setFlagDrawSunAfterAtmosphere(bool val)
{
	flagDrawSunAfterAtmosphere=val;
	QSettings* conf = StelApp::getInstance().getSettings();
	 conf->setValue("landscape/draw_sun_after_atmosphere",val);
	 conf->sync();
	 emit flagDrawSunAfterAtmosphereChanged(val);
}

void StelSkyDrawer::setFlagEarlySunHalo(bool val)
{
	flagEarlySunHalo= val;
	QSettings* conf = StelApp::getInstance().getSettings();
	conf->setValue("landscape/early_solar_halo", val);
	conf->sync();
	emit flagEarlySunHaloChanged(val);
}

void StelSkyDrawer::setFlagTfromK(bool val)
{
	flagTfromK=val;
	QSettings* conf = StelApp::getInstance().getSettings();
	conf->setValue("landscape/use_T_from_k", val);
	conf->sync();
	emit flagTfromKChanged(val);
}

void StelSkyDrawer::setT(double newT)
{
    turbidity=static_cast<float>(newT);
        QSettings* conf = StelApp::getInstance().getSettings();
        conf->setValue("landscape/turbidity", newT);
        emit turbidityChanged(newT);
}
