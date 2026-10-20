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
	photoHaloFraction = qBound(0.f, conf->value("viewing/photographic_halo_strength", 0.05).toFloat(), 0.3f);
	// Diaphragm blades; older settings stored the number of spikes, which is the same for even numbers
	photoApertureBlades = conf->value("viewing/photographic_aperture_blades", conf->value("viewing/photographic_spike_count", 6)).toInt();
	photoApertureBlades = (photoApertureBlades<=0) ? 0 : qBound(4, photoApertureBlades, 9);
	photoSpikeFraction = qBound(0.f, conf->value("viewing/photographic_spike_strength", 0.02).toFloat(), 0.1f);
	flagPhotoBloom = conf->value("viewing/flag_photographic_bloom", true).toBool();
	flagPhotoAutoExposure = conf->value("viewing/flag_photographic_auto_exposure", false).toBool();
	photoMeteringMode = qBound(0, conf->value("viewing/photographic_metering_mode", 0).toInt(), 2);
	flagPhotoInfoLine = conf->value("viewing/flag_photographic_info_line", true).toBool();
	photoToneCurve = qBound(0, conf->value("viewing/photographic_tone_curve", static_cast<int>(PhotoToneStandard)).toInt(), 2);
	updatePhotoToneCurve();
	flagPhotoExtinctionColor = conf->value("viewing/flag_photographic_extinction_color", true).toBool();
	photoWhiteBalance = qBound(0, conf->value("viewing/photographic_white_balance", static_cast<int>(PhotoWBDaylight)).toInt(), 8);
	photoWhiteBalanceKelvin = qBound(2000., conf->value("viewing/photographic_white_balance_kelvin", 5200.).toDouble(), 12000.);
	flagPhotoVignetting = conf->value("viewing/flag_photographic_vignetting", true).toBool();
	flagPhotoAberrations = conf->value("viewing/flag_photographic_aberrations", true).toBool();
	photoAberrationStrength = qBound(0.f, conf->value("viewing/photographic_aberration_strength", 1.0).toFloat(), 3.f);
	flagPhotoLensFlare = conf->value("viewing/flag_photographic_lens_flare", true).toBool();
	photoLensFlareStrength = qBound(0.f, conf->value("viewing/photographic_lens_flare_strength", 1.0).toFloat(), 3.f);
	setPhotoFocusDistance(conf->value("viewing/photographic_focus_distance", 0.0).toDouble());
	flagPhotoStarTrails = conf->value("viewing/flag_photographic_star_trails", true).toBool();
	photoSensorFormat = qBound(static_cast<int>(PhotoSensorFullFrame), conf->value("viewing/photographic_sensor_format", static_cast<int>(PhotoSensorFullFrame)).toInt(), static_cast<int>(PhotoSensorAPSC));
	flagPhotoZoomWholeMillimeters = conf->value("viewing/flag_photographic_zoom_whole_millimeters", false).toBool();
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
highp float aberratedCore(highp vec2 offset)
{
	highp float rho = vLens.x;
	highp vec2 er = vLens.yz;
	highp float u = dot(offset, er);
	highp float v = dot(offset, vec2(-er.y, er.x));
	highp float coma = comaLength * rho;
	highp float astig = astigLength * rho * rho;
	highp float a2 = coreAlpha * coreAlpha;
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
		profile = coreFraction * vNorm.x * aberratedCore(vOffset);
	else
		profile = coreFraction * vNorm.x * moffat(r2, coreAlpha, coreBeta);
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
	profile *= 1. - smoothstep(Rd + 0.75 * (vRadius - Rd), vRadius, fadeDistance);
	FRAG_COLOR = vec4(min(vColor * trailTint * profile, vec3(1e30)), 1.); // guard against overflow
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
		// Half a step of the display above the sky, through the tone curve and the sRGB encoding
		const float skyOnScreen = photoSkyBackground * exposure;
		const float skyDisplay = photoToneCurveValue(skyOnScreen);
		const float skyEncoded = skyDisplay <= 0.0031308f ? 12.92f*skyDisplay : 1.055f*std::pow(skyDisplay, 1.f/2.4f) - 0.055f;
		const float nextDisplay = srgbToLinear(qMin(skyEncoded + 0.5f/255.f, 1.f));
		const float nextOnScreen = photoToneCurveInverse(nextDisplay);
		// A saturated (white) sky hides everything
		photoVisibleThreshold = (nextOnScreen > skyOnScreen) ? (nextOnScreen - skyOnScreen) / exposure : 1e30f;
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
		// (one turn per sidereal day). The exposure is centered on the current time.
		photoTrailsActive = false;
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
					photoTrailsActive = true;
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
	// Total recorded light of the star (sum over all pixels, linear units, 1.0 = one saturated pixel)
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
	if (rCore <= 0.f)
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
		photoDeferredBodies.push_back(wings);
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
			photoInstances[nbPhotoPointSources++] = body;
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
	// Standard: the shape of a camera JPEG (more contrast than linear: darker shadows, brighter
	// mid-highlights), with the sensor saturating about 3 stops above middle grey, as the
	// saturation-based ISO speed (ISO 12232) puts it: 0.18 * 7.8 = 1.4.
	// Film: like a negative film, slightly soft, with 6 stops of room above middle grey (0.18 * 64).
	float a = 1.f, d = 1.f, clip = 1.f;
	switch (photoToneCurve)
	{
		case PhotoToneStandard: a = 1.30f; d = 1.f; clip = 1.4f;  break;
		case PhotoToneFilm:     a = 1.15f; d = 1.f; clip = 11.5f; break;
		default: break;
	}
	constexpr double midIn = 0.18, midOut = 0.18;
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
	if (photoToneCurve == PhotoToneLinear)
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
	if (photoToneCurve == PhotoToneLinear)
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
	lens.corner = qMax(1.f, 0.5f*static_cast<float>(std::hypot(prj.getViewportWidth(), prj.getViewportHeight())));
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
	// The diagonal of the view is the diagonal of the sensor
	const double diagonal = std::hypot(prj.getViewportWidth(), prj.getViewportHeight());
	return diagonal > 0. ? getPhotoSensorDiagonal() * prj.getPixelPerRadAtCenter() / diagonal : 0.05;
}

// The projection maps the field radius to pixels: pixelPerRad = (fov disk diameter)/2 / scaling(fov/2).
// The current projector gives the diameter, then the focal length in pixels follows for any field.
double StelSkyDrawer::getPhotoFocalLengthForFov(double fov) const
{
	const StelProjectorP prj = core->getProjection(StelCore::FrameJ2000);
	const double diagonal = std::hypot(prj->getViewportWidth(), prj->getViewportHeight());
	const double halfDisk = static_cast<double>(prj->getPixelPerRadAtCenter()) * static_cast<double>(prj->fovToViewScalingFactor(static_cast<float>(M_PI/360.*static_cast<double>(prj->getFov()))));
	const double scaling = static_cast<double>(prj->fovToViewScalingFactor(static_cast<float>(M_PI/360.*fov)));
	if (!(diagonal > 0.) || !(scaling > 0.))
		return 0.05;
	return getPhotoSensorDiagonal() * (halfDisk/scaling) / diagonal;
}

double StelSkyDrawer::getPhotoFovForFocalLength(double f) const
{
	const StelProjectorP prj = core->getProjection(StelCore::FrameJ2000);
	const double diagonal = std::hypot(prj->getViewportWidth(), prj->getViewportHeight());
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

void StelSkyDrawer::setPhotoSensorFormat(int format)
{
	const int f = qBound(static_cast<int>(PhotoSensorFullFrame), format, static_cast<int>(PhotoSensorAPSC));
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
	if (photoNoise == PhotoNoiseOff || !getFlagPhotographicModeActive())
		return p;
	p.enabled = true;
	// The sensor collects the light in electrons. The scene buffer is scaled so that 1.0 is a saturated
	// photosite at the current ISO: at the base ISO that is the full well, at a higher ISO the amplifier
	// reaches white with proportionally fewer electrons. That is why a high ISO is noisy: for the same
	// brightness in the picture the sensor has caught fewer photons (for the same exposure time and aperture,
	// the photons are the same, but so is the noise relative to the signal).
	const float iso = qMax(static_cast<float>(photoISO), PHOTO_SENSOR_BASE_ISO);
	// A smaller photosite (APS-C) holds fewer electrons and collects less dark current, in proportion to its area
	const double pitch = getPhotoSensorPitch();
	const float area = static_cast<float>((pitch*pitch)/(PHOTO_SENSOR_PITCH*PHOTO_SENSOR_PITCH));
	p.white = PHOTO_SENSOR_FULL_WELL * area * PHOTO_SENSOR_BASE_ISO / iso;
	// Read noise: the part after the amplifier shrinks (in electrons) at higher ISO ("ISO invariance")
	const float downstream = PHOTO_READ_NOISE_DOWNSTREAM * PHOTO_SENSOR_BASE_ISO / iso;
	p.read2 = PHOTO_READ_NOISE_UPSTREAM*PHOTO_READ_NOISE_UPSTREAM + downstream*downstream;
	// Dark current grows with the temperature of the sensor: the air temperature (atmosphere settings) plus its own warming
	const float t = static_cast<float>(getPhotoEffectiveExposureTime());
	const float celsius = static_cast<float>(getAtmosphereTemperature()) + PHOTO_SENSOR_WARMING;
	const float thermal = std::exp2((celsius - 20.f)/PHOTO_DARK_DOUBLING);
	p.dark = PHOTO_DARK_CURRENT * area * thermal * t;
	p.hot = PHOTO_HOT_PIXEL_CURRENT * thermal * t;
	// Photosites per screen pixel: the view is the sensor (its diagonal), see getPhotoFocalLength()
	if (photoNoise == PhotoNoisePixel)
		p.sites = 1.f;
	else
	{
		const double diagonal = std::hypot(prj.getViewportWidth(), prj.getViewportHeight());
		const double pixel = diagonal > 0. ? getPhotoSensorDiagonal()/diagonal : pitch;
		p.sites = static_cast<float>(qMax(1., (pixel*pixel)/(pitch*pitch)));
	}
	p.hotProbability = qMin(1.f, 1.f - std::pow(1.f - PHOTO_HOT_PIXEL_FRACTION, p.sites));
	p.seed = static_cast<float>(photoNoiseFrame) + 0.5f;
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

bool StelSkyDrawer::beginPhotoBodyImage(const Vec3d& posJ2000, float radius)
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
	const float defocus = photoLensCache.defocus;
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
	if (w <= 0 || h <= 0 || w > 4096 || h > 4096)
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
	if (!photoTrailsActive)
		return 0.f;
	const auto mv = prj.getModelViewTransform();
	Vec3d view = v;
	mv->forward(view);
	const double norm = view.norm();
	if (!(norm > 0.))
		return 0.f;
	// Quick estimate first (the arc near the image center): most faint stars drop out here, before projecting
	const float estimate = static_cast<float>(photoTrailAngle * (photoTrailAxisView^view).norm()/norm * prj.getPixelPerRadAtCenter());
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
	// Positions at the start and the end of the exposure: turn the direction around the axis, then project
	Vec3d a = photoTrailToStart*view;
	Vec3d b = photoTrailToEnd*view;
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
	if (photoWhiteBalance == PhotoWBAuto)
		return photoAutoWhiteBalanceKelvin;
	if (photoWhiteBalance == PhotoWBCustom)
		return photoWhiteBalanceKelvin;
	return photoWhiteBalancePresetKelvin(photoWhiteBalance);
}

bool StelSkyDrawer::getFlagPhotoAutoWhiteBalanceActive() const
{
	return photoWhiteBalance == PhotoWBAuto && getFlagPhotographicModeActive();
}

double StelSkyDrawer::getPhotoMeterClipLevel() const
{
	const double clip = (photoToneCurve == PhotoToneLinear) ? 1. : static_cast<double>(photoToneParams.clip);
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
	if (photoWhiteBalance != PhotoWBNeutral)
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
	const float v = qBound(0.f, static_cast<float>(fraction), 0.3f);
	if (qFuzzyCompare(1.f+v, 1.f+photoHaloFraction))
		return;
	photoHaloFraction = v;
	StelApp::immediateSave("viewing/photographic_halo_strength", static_cast<double>(v));
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
	setPhotoHaloStrength(0.05);
	setPhotoApertureBlades(6);
	setPhotoSpikeStrength(0.02);
	setFlagPhotoBloom(true);
	setFlagPhotoAutoExposure(false);
	setPhotoMeteringMode(0);
	setPhotoToneCurve(PhotoToneStandard);
	setFlagPhotoExtinctionColor(true);
	setPhotoWhiteBalance(PhotoWBDaylight);
	setPhotoWhiteBalanceKelvin(5200.);
	setFlagPhotoVignetting(true);
	setFlagPhotoAberrations(true);
	setPhotoAberrationStrength(1.);
	setFlagPhotoLensFlare(true);
	setPhotoLensFlareStrength(1.);
	setPhotoFocusDistance(0.);
	setFlagPhotoStarTrails(true);
	setPhotoNoise(PhotoNoiseImage);
	setPhotoSensorFormat(PhotoSensorFullFrame);
	setFlagPhotoZoomWholeMillimeters(false);
}

float StelSkyDrawer::getPhotoSceneScale() const
{
	// Reflected-light meter calibration constant K (ISO 2720), and middle grey
	constexpr double K = 12.5;
	constexpr double middleGrey = 0.18;
	return static_cast<float>(middleGrey * getPhotoEffectiveExposureTime() * photoISO / (K * photoAperture * photoAperture));
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
	13., 15., 20., 25., 30., 45., 60., 90., 120., 180., 240., 300., 600., 900., 1200., 1800., 3600. };

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
	parts << (autoExposure ? qc_("Av", "camera mode: aperture priority") : qc_("M", "camera mode: manual"));
	parts << photoExposureTimeText(t);
	parts << QString("f/%1").arg(QLocale().toString(photoAperture, 'g', 3));
	// Focal length of the lens for this field on the sensor in use, and the focus when it is not at infinity
	{
		QString lens = QString("%1 mm").arg(qRound(1000.*getPhotoFocalLength(*core->getProjection(StelCore::FrameJ2000))));
		if (photoSensorFormat == PhotoSensorAPSC)
			lens += QStringLiteral(" APS-C");
		parts << lens;
	}
	if (photoFocusDistance != 0.)
		parts << QString("%1 %2").arg(qc_("focus", "camera: focus distance"), photoFocusDistanceText(photoFocusDistance));
	parts << QString("ISO %1").arg(qRound(photoISO));
	parts << QString("EV100 %1").arg(QLocale().toString(ev100, 'f', 1));
	if (std::fabs(photoExposureEV) > 0.01)
		parts << QString("%1 EV").arg(QString::asprintf("%+.1f", photoExposureEV));
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
