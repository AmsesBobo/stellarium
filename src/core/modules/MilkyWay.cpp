/*
 * Stellarium
 * Copyright (C) 2002 Fabien Chereau
 * Copyright (C) 2014 Georg Zotti (extinction parts)
 * Copyright (C) 2022 Ruslan Kabatsayev
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

#include "MilkyWay.hpp"
#include "StelSRGB.hpp"
#include "StelFader.hpp"
#include "StelTexture.hpp"
#include "StelUtils.hpp"
#include "StelFileMgr.hpp"
#include "StelMainView.hpp"

#include "StelProjector.hpp"
#include "StelToneReproducer.hpp"
#include "StelApp.hpp"
#include "StelOpenGLArray.hpp"
#include "SaturationShader.hpp"
#include "StelTextureMgr.hpp"
#include "StelSkyDrawer.hpp"
#include "StelPainter.hpp"
#include "StelTranslator.hpp"
#include "StelModuleMgr.hpp"
#include "LandscapeMgr.hpp"
#include "StarMgr.hpp"
#include "StelMovementMgr.hpp"
#include "Planet.hpp"
// For debugging draw() only:
// #include "StelObjectMgr.hpp"

#include <QDebug>
#include <QSettings>
#include <QOpenGLShaderProgram>

namespace
{
constexpr int FS_QUAD_COORDS_PER_VERTEX = 2;
constexpr int SKY_VERTEX_ATTRIB_INDEX = 0;
}

// Class which manages the displaying of the Milky Way
MilkyWay::MilkyWay()
	: color(1.f, 1.f, 1.f)
	, intensity(1.)
	, intensityFovScale(1.0)
	, intensityMinFov(0.25) // when zooming in further, MilkyWay is no longer visible.
	, intensityMaxFov(2.5) // when zooming out further, MilkyWay is fully visible (when enabled).
	, shaderVars()
{
	setObjectName("MilkyWay");
	fader = new LinearFader();
}

MilkyWay::~MilkyWay()
{
	delete fader;
	fader = Q_NULLPTR;
}

void MilkyWay::init()
{
	QSettings* conf = StelApp::getInstance().getSettings();
	Q_ASSERT(conf);

	auto& gl = *QOpenGLContext::currentContext()->functions();

	// Texture for the normal (eye) view. It can be replaced via config.
	const QString textureName = conf->value("astro/milky_way_texture", "milkyway.png").toString();
	QString texturePath = StelFileMgr::findFile("textures/"+textureName);
	if (texturePath.isEmpty())
	{
		qWarning() << "Milky Way texture" << textureName << "not found, using milkyway.png";
		texturePath = StelFileMgr::getInstallationDir()+"/textures/milkyway.png";
	}
	mainTex = StelApp::getInstance().getTextureManager().createTexture(texturePath);
	mainTex->bind(0);
	gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	mainTex->release();

	// Texture for the photographic mode. It must be linear in surface brightness (after sRGB decoding)
	// and free of the stars that Stellarium draws itself. photoLuminanceScale is the luminance (cd/m^2)
	// of a linear texture value of 1.0.
	// Default: milkyway_gaia.png, made from the Gaia Sky Milky Way cubemap (based on ESA Gaia EDR3 data).
	// That image is stretched for display (its contrast compressed, like most astronomical pictures), so the
	// photographic mode expands it again (photoCurve, see below): its luminance L becomes exp(f(ln L)), with
	// f(x) = c0 + c1 x + c2 x^2 between x0 and x1, and straight lines (the slopes of the parabola) outside.
	// The parabola is fitted (0.4 mag rms, 26000 points of 0.5 degrees, |b| < 30, brighter than 23.5) to Axel
	// Mellinger's panorama, which is photometrically calibrated against Pioneer 10 (Mellinger 2009, PASP 121, 1180):
	// the image needs more expansion the brighter it is, slope 1.7 at the median of the Milky Way band, 3.5 in its
	// brightest percent (a single power of 1.5 left the star clouds of the center too flat and their surroundings
	// too bright). Above the 70th percentile of the band (x1) it goes on straight (slope 2.1): fewer points hold the
	// parabola there, and so the brightest star clouds stand out against their surroundings as much as in a camera
	// picture of the center from a dark site (Namibia, Canon EOS 600D: brightest to median 5.3; Stellarium 5.6, with
	// the parabola up to the 90th percentile 7.6). Below x0 the slope is 0.68, where the parabola levels off: the
	// galactic poles come to about 24.9 mag/arcsec^2 (Pioneer 10: about 24.1), the power of 1.5 gave 27. The star
	// clouds of Sagittarius reach about 20.4 mag/arcsec^2 (V), 10 degrees below them 21.9, Scutum 21.4, Carina 21.3,
	// Cygnus 22.8, the anticenter 22.6. (x1 can be changed with astro/milky_way_photo_curve_top.)
	// The scale keeps the calibration to the mean integrated starlight at the zenith at 40N (75 nW/m^2/sr in V,
	// Masana et al. 2021, MNRAS 501, 5443) minus the ~20% contributed by Hipparcos-bright stars: the mean over
	// declinations 30-50 is that of the earlier expansion (1.0 = 1400 S10(V) with a power of 1.5). This is the
	// texture with the factor 1.0 of photoStarFactors(): the light of the stars that are drawn as points is taken
	// out of it while drawing (astro/flag_milky_way_photo_star_subtraction), so that it does not count twice.
	// astro/milky_way_photo_destretch > 0 uses a single power instead (with its own astro/milky_way_photo_luminance).
	// Without it, Axel Mellinger's panorama is used: its 99.9th percentile (linear 0.21) is set to
	// about 20.5 mag/arcsec^2 (V).
	const QString photoTextureName = conf->value("astro/milky_way_photo_texture", "milkyway_gaia.png").toString();
	const QString photoTexturePath = StelFileMgr::findFile("textures/"+photoTextureName);
	if (photoTexturePath.isEmpty())
	{
		qWarning() << "Photographic Milky Way texture" << photoTextureName << "not found, using" << textureName;
		photoTex = mainTex;
		photoLuminanceScale = conf->value("astro/milky_way_photo_luminance", 3.2e-3).toDouble();
		photoDestretch = conf->value("astro/milky_way_photo_destretch", 1.0).toFloat();
	}
	else
	{
		photoTex = (photoTexturePath == texturePath) ? mainTex
		         : StelApp::getInstance().getTextureManager().createTexture(photoTexturePath);
		photoTex->bind(0);
		gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
		photoTex->release();
		// The scale belongs to the expansion: changing astro/milky_way_photo_destretch needs a new scale
		photoDestretch = conf->value("astro/milky_way_photo_destretch", 0.0).toFloat();
		photoLuminanceScale = conf->value("astro/milky_way_photo_luminance", photoDestretch > 0.f ? 1.16e-3 : 5.096e-3).toDouble();
		photoCurve.set(-0.48191f, 4.11797f, 1.14056f, -1.50713f);
		photoCurveTop = conf->value("astro/milky_way_photo_curve_top", -0.8882).toFloat();
		// The colors of that image are a display choice: bluish white (its mean, weighted with the light after the
		// expansion, is linear sRGB 0.91 1.02 1.11 at luminance 1), while the integrated starlight of the Milky Way
		// is redder than sunlight, B-V about 0.9 against 0.65 (Leinert et al. 1998, A&AS 127, 1): as a 5000 K star
		// seen in sunlight white balance, 1.09 0.99 0.84. These gains move the mean there and keep the variations of
		// the image (the reddened dust lanes, the bluer young regions); the luminance does not change.
		flagPhotoStarSubtraction = conf->value("astro/flag_milky_way_photo_star_subtraction", true).toBool();
		// The large-scale brightness of the Gaia Sky image is corrected towards Axel Mellinger's panorama (the 3600 x 1800
		// FITS version, linear and calibrated: its scale from the stars, about 1400 isolated ones of magnitude 3.5-7.5 as
		// a whole, since each of its pixels is a single sample of the full panorama; its large-scale background from the
		// Pioneer 10/11 photometry). Over most of the sky the two agree within 0.1-0.2 mag (median 0.0), but in the inner
		// Galaxy the Gaia image is too bright on large scales: the bulge above and below the plane in Sagittarius,
		// Scorpius and Scutum by up to 1 mag (the Great Sagittarius Star Cloud 20.1 instead of 21.1 mag/arcsec^2, V).
		// Pioneer 10 (Toller 1981, as tabulated by Leinert et al. 1998, tables 35 and 36, interpolated to V) has the same
		// shape as Mellinger's panorama everywhere, the bulge included (rms 0.12 mag on its 10 x 4 degree grid), and puts
		// the Gaia image 0.3-0.9 mag too bright there. A linear raw picture (Canon EOS R6, 15 mm, 20 s, Grand Canyon, the
		// field solved from 180 stars) shows the finer structure there (clouds and dust lanes, below 3 degrees) as strong
		// as in the Gaia image relative to the disk in Aquila (1.0-1.2), but 2 times stronger than in Mellinger's panorama.
		// So the surplus is a smooth glow: it is taken off (not scaled down), the smoothed difference Gaia - Mellinger
		// (3 degrees, in the units of the expanded texture), with the median outside the inner Galaxy at zero, faded out
		// between |b| = 25 and 40; where it exceeds the image (narrow dust lanes), 20% of the image stays. Stored as
		// sign(d) sqrt(|d|/0.06386) mapped to 0..1. Note: derived from Mellinger's data (all rights reserved; the FITS is
		// offered for science use), not for redistribution without his consent.
		// Only half of it is taken off: the same raw picture, calibrated with its own stars (about 700 of magnitude
		// 5.5-7.6 near the altitude of Sagittarius), gives the Great Sagittarius Star Cloud above the sky at its altitude
		// (26 places 12-20 degrees off the plane, same altitude, within 35 degrees of azimuth) 0.80 times what the
		// uncorrected Gaia image gives, but 2.2 times Mellinger's panorama and 1.24 times the full correction: the truth
		// lies between, at about 0.56 of it (+-0.4 from the zero point of the picture, +-0.15 mag). Why Pioneer 10 and
		// Mellinger give the bulge fainter is open (the Sun lay towards Sagittarius as seen from Pioneer 10).
		// (config: astro/flag_milky_way_photo_correction, astro/milky_way_photo_correction_texture,
		// astro/milky_way_photo_correction_strength: 0 none, 1 the full difference)
		photoCorrectionStrength = qBound(0.f, conf->value("astro/milky_way_photo_correction_strength", 0.5).toFloat(), 1.f);
		const QString correctionName = conf->value("astro/milky_way_photo_correction_texture", "milkyway_gaia_correction.png").toString();
		const bool correctionWanted = conf->value("astro/flag_milky_way_photo_correction", true).toBool() && !correctionName.isEmpty();
		const QString correctionPath = correctionWanted ? StelFileMgr::findFile("textures/"+correctionName) : QString();
		if (!correctionPath.isEmpty() && photoDestretch <= 0.f)
			photoCorrectionTex = StelApp::getInstance().getTextureManager().createTexture(correctionPath,
			                        StelTexture::Params().useLinearSRGB().setWrapMode(GL_REPEAT));
		const QStringList gain = conf->value("astro/milky_way_photo_color_gain", "1.197,0.967,0.750").toString().split(',');
		if (gain.size() == 3)
			photoColorGain.set(gain[0].trimmed().toFloat(), gain[1].trimmed().toFloat(), gain[2].trimmed().toFloat());
	}
	setFlagShow(conf->value("astro/flag_milky_way").toBool());
	setIntensity(conf->value("astro/milky_way_intensity",1.).toDouble());
	setSaturation(conf->value("astro/milky_way_saturation", 1.).toDouble());
	setColor(Vec3f(conf->value("astro/milky_way_color", "1.0,1.0,1.0").toString()));

	vbo.reset(new QOpenGLBuffer(QOpenGLBuffer::VertexBuffer));
	vbo->create();
	vbo->bind();
	const GLfloat vertices[]=
	{
		// full screen quad
		-1, -1,
		 1, -1,
		-1,  1,
		 1,  1,
	};
	gl.glBufferData(GL_ARRAY_BUFFER, sizeof vertices, vertices, GL_STATIC_DRAW);
	vbo->release();

	vao.reset(new QOpenGLVertexArrayObject);
	vao->create();
	bindVAO();
	setupCurrentVAO();
	releaseVAO();

	QString displayGroup = N_("Display Options");
	addAction("actionShow_MilkyWay", displayGroup, N_("Milky Way"), "flagMilkyWayDisplayed", "M");
}

void MilkyWay::setupCurrentVAO()
{
	auto& gl = *QOpenGLContext::currentContext()->functions();
	vbo->bind();
	gl.glVertexAttribPointer(0, FS_QUAD_COORDS_PER_VERTEX, GL_FLOAT, false, 0, 0);
	vbo->release();
	gl.glEnableVertexAttribArray(SKY_VERTEX_ATTRIB_INDEX);
}

void MilkyWay::bindVAO()
{
	if(vao->isCreated())
		vao->bind();
	else
		setupCurrentVAO();
}

void MilkyWay::releaseVAO()
{
	if(vao->isCreated())
	{
		vao->release();
	}
	else
	{
		auto& gl = *QOpenGLContext::currentContext()->functions();
		gl.glDisableVertexAttribArray(SKY_VERTEX_ATTRIB_INDEX);
	}
}

// Photographic mode: the stars that are drawn as points must not be in the texture as well (their light would count
// twice). The texture holds all the light of the stars: as calibrated, about 1.46 times the light of the stars fainter
// than 10.5 (the default catalogs) - a sum over Stellarium's catalogs (to 10.5, by galactic latitude; 26 S10 at the
// galactic poles, where Pioneer 10 measured 26-30 S10 for the stars fainter than 6.5) and the star counts of its
// extra catalogs (10.5-18, about 220 million stars), continued to the faint end of Gaia. Over the whole sky the
// stars to 6 give 20% of the light, to 10.5 54%, to 12 66%, to 15.5 88% and to 18 96%.
// For a limit fainter than 10.5 the factor is the share of the fainter light, (1 - G(mag))/1.46 with G from the star
// counts (the same at all latitudes). For a brighter limit the light of the stars between it and 10.5 goes back into
// the texture: their sum in each band of galactic latitude, relative to the texture there. 1.0 is the texture as
// calibrated.
void MilkyWay::photoStarFactors(float mag, float factors[19]) const
{
	static const float brightTable[10][19] = { // magnitude 6, 6.5, ..., 10.5; latitude 0, 5, ..., 90
		{1.075f, 0.986f, 1.090f, 1.191f, 1.248f, 1.283f, 1.322f, 1.343f, 1.393f, 1.442f, 1.512f, 1.576f, 1.613f, 1.728f, 1.790f, 1.830f, 1.884f, 1.954f, 2.034f},
		{1.046f, 0.963f, 1.060f, 1.147f, 1.198f, 1.232f, 1.267f, 1.277f, 1.338f, 1.372f, 1.440f, 1.482f, 1.523f, 1.601f, 1.677f, 1.695f, 1.733f, 1.793f, 1.899f},
		{1.013f, 0.938f, 1.024f, 1.100f, 1.146f, 1.174f, 1.206f, 1.218f, 1.265f, 1.293f, 1.345f, 1.394f, 1.431f, 1.481f, 1.553f, 1.564f, 1.602f, 1.629f, 1.659f},
		{0.977f, 0.911f, 0.986f, 1.053f, 1.090f, 1.116f, 1.137f, 1.148f, 1.190f, 1.211f, 1.249f, 1.287f, 1.317f, 1.365f, 1.435f, 1.429f, 1.468f, 1.484f, 1.541f},
		{0.939f, 0.881f, 0.945f, 1.001f, 1.030f, 1.048f, 1.068f, 1.076f, 1.105f, 1.126f, 1.151f, 1.181f, 1.205f, 1.251f, 1.302f, 1.308f, 1.336f, 1.344f, 1.405f},
		{0.894f, 0.847f, 0.900f, 0.943f, 0.967f, 0.977f, 0.994f, 1.001f, 1.023f, 1.036f, 1.058f, 1.076f, 1.100f, 1.133f, 1.175f, 1.185f, 1.194f, 1.201f, 1.273f},
		{0.847f, 0.811f, 0.851f, 0.883f, 0.900f, 0.907f, 0.919f, 0.921f, 0.935f, 0.947f, 0.964f, 0.976f, 0.992f, 1.016f, 1.041f, 1.049f, 1.063f, 1.067f, 1.099f},
		{0.796f, 0.771f, 0.799f, 0.821f, 0.829f, 0.833f, 0.842f, 0.843f, 0.850f, 0.857f, 0.867f, 0.875f, 0.887f, 0.900f, 0.917f, 0.922f, 0.926f, 0.935f, 0.962f},
		{0.742f, 0.730f, 0.744f, 0.755f, 0.759f, 0.760f, 0.763f, 0.763f, 0.767f, 0.770f, 0.773f, 0.777f, 0.784f, 0.788f, 0.798f, 0.797f, 0.807f, 0.807f, 0.803f},
		{0.687f, 0.687f, 0.687f, 0.687f, 0.687f, 0.687f, 0.687f, 0.687f, 0.687f, 0.687f, 0.687f, 0.687f, 0.687f, 0.687f, 0.687f, 0.687f, 0.687f, 0.687f, 0.687f} };
	static const float faintTable[16] = { // magnitude 10.5, 11, ..., 18
		0.687f, 0.625f, 0.565f, 0.507f, 0.450f, 0.395f, 0.343f, 0.296f, 0.252f, 0.212f, 0.176f, 0.145f, 0.117f, 0.093f, 0.073f, 0.056f };
	if (!flagPhotoStarSubtraction)
	{
		std::fill(factors, factors+19, 1.f);
		return;
	}
	if (mag >= 10.5f)
	{
		const float x = qMin(15.f, (mag - 10.5f)*2.f);
		const int i = qMin(14, static_cast<int>(x));
		const float f = faintTable[i] + (faintTable[i+1] - faintTable[i])*(x - static_cast<float>(i));
		std::fill(factors, factors+19, f);
		return;
	}
	const float x = qMax(0.f, (mag - 6.f)*2.f);
	const int i = qMin(8, static_cast<int>(x));
	const float t = x - static_cast<float>(i);
	for (int j = 0; j < 19; ++j)
		factors[j] = brightTable[i][j] + (brightTable[i+1][j] - brightTable[i][j])*t;
}

// The faintest stars drawn as points: the faint end of the catalogs, the limit set by the user, and those that rise above
// the noise of the picture (StelSkyDrawer::computeRCMag()). Not fainter than 6: the light of the bright stars never goes
// into the smooth texture.
float MilkyWay::photoDrawnStarsMagnitude(const StelSkyDrawer* drawer) const
{
	constexpr float magMin = 6.f, magDeepest = 20.f;
	static StarMgr* starMgr = GETSTELMODULE(StarMgr);
	// The faintest stars the picture shows (whatever the catalogs hold)
	RCMag rcmag;
	float a = magMin, b = magDeepest;
	if (drawer->computeRCMag(magDeepest, &rcmag))
		a = magDeepest;
	else if (!drawer->computeRCMag(magMin, &rcmag))
		b = magMin;
	for (int k = 0; k < 14 && b - a > 0.01f; ++k)
	{
		const float m = 0.5f*(a + b);
		if (drawer->computeRCMag(m, &rcmag))
			a = m;
		else
			b = m;
	}
	photoDetectionMagnitude = a;
	if (!starMgr || !starMgr->getFlagStars())
		return magMin;
	float mag = qMin(a, starMgr->getCatalogsFaintestMag());
	if (drawer->getFlagStarMagnitudeLimit())
		mag = qMin(mag, static_cast<float>(drawer->getCustomStarMagnitudeLimit()));
	return qMax(magMin, mag);
}

void MilkyWay::update(double deltaTime)
{
	fader->update(static_cast<int>(deltaTime*1000));
	//calculate FOV fade value, linear fade between intensityMaxFov and intensityMinFov
	double fov = StelApp::getInstance().getCore()->getMovementMgr()->getCurrentFov();
	intensityFovScale = qBound(0.0,(fov - intensityMinFov) / (intensityMaxFov - intensityMinFov),1.0);
}

/*************************************************************************
 Reimplementation of the getCallOrder method
*************************************************************************/
double MilkyWay::getCallOrder(StelModuleActionName actionName) const
{
	if (actionName==StelModule::ActionDraw)
		return 1;
	return 0;
}

void MilkyWay::setFlagShow(bool b)
{
	if (*fader != b)
	{
		*fader = b;
		StelApp::immediateSave("astro/flag_milky_way", b);
		emit milkyWayDisplayedChanged(b);
	}
}

bool MilkyWay::getFlagShow() const {return *fader;}

// Set Milky Way intensity. Default value: 1.
void MilkyWay::setIntensity(double aintensity)
{
	if(!qFuzzyCompare(aintensity, intensity))
	{
		intensity = aintensity;
		StelApp::immediateSave("astro/milky_way_intensity", aintensity);
		emit intensityChanged(intensity);
	}
}

// Set Milky Way saturation (color strength).
void MilkyWay::setSaturation(double sat)
{
	if(!qFuzzyCompare(sat, saturation))
	{
		saturation = sat;
		StelApp::immediateSave("astro/milky_way_saturation", sat);
		emit saturationChanged(saturation);
	}
}

void MilkyWay::setColor(const Vec3f& c)
{
	if (c!=color)
	{
		color=c;
		emit colorChanged(c);
	}
}


void MilkyWay::draw(StelCore* core)
{
	if (!core->getFlagClearSky())
		return;

	if (!fader->getInterstate())
		return;

	const auto modelView = core->getJ2000ModelViewTransform();
	const auto projector = core->getProjection(modelView);
	const auto drawer=core->getSkyDrawer();
	const auto& extinction=drawer->getExtinction();

	if(!renderProgram || !prevProjector || !projector->isSameProjection(*prevProjector))
	{
		renderProgram.reset(new QOpenGLShaderProgram);
		prevProjector = projector;

		const auto vert =
			StelOpenGL::globalShaderPrefix(StelOpenGL::VERTEX_SHADER) +
			R"(
ATTRIBUTE highp vec3 vertex;
VARYING highp vec3 ndcPos;
void main()
{
	gl_Position = vec4(vertex, 1.);
	ndcPos = vertex;
}
)";
		bool ok = renderProgram->addShaderFromSourceCode(QOpenGLShader::Vertex, vert);
		if(!renderProgram->log().isEmpty())
			qWarning().noquote() << "MilkyWay: Warnings while compiling vertex shader:\n" << renderProgram->log();
		if(!ok) return;

		const auto frag =
			StelOpenGL::globalShaderPrefix(StelOpenGL::FRAGMENT_SHADER) +
			projector->getUnProjectShader() +
			core->getAberrationShader() +
			extinction.getForwardTransformShader() +
			StelSkyDrawer::getPhotoExtinctionShader() +
			StelSkyDrawer::getPhotoSkyTrailShader() +
			makeSaturationShader()+
			R"(
VARYING highp vec3 ndcPos;
uniform sampler2D mainTex;
uniform lowp float saturation;
uniform vec3 brightness;
uniform mat4 projectionMatrixInverse;
uniform bool extinctionEnabled;
uniform float bortleIntensity;
uniform bool photoMode;  // photographic mode: texture is linear luminance, brightness holds the calibration
uniform float photoDestretch; // photographic mode: power applied to the luminance of the texture (keeps the color), 0: photoCurve
uniform vec4 photoCurve;      // photographic mode: the expansion of the texture (c0, c1, c2, x0), see MilkyWay::init()
uniform float photoCurveTop;  // photographic mode: x1 of that expansion
uniform float photoStarFactor[19]; // photographic mode: factor for the galactic latitudes 0, 5, ..., 90 (stars drawn as points)
uniform vec3 photoGalacticPole;    // photographic mode: north galactic pole (J2000)
uniform bool photoCorrection;      // photographic mode: large-scale correction of the texture (see MilkyWay::init())
uniform sampler2D photoCorrectionTex;
uniform float photoCorrectionStrength;
float photoStarLight(vec3 dir)
{
	// The share of the texture's light that is not drawn as stars, by galactic latitude
	float x = degrees(asin(min(1., abs(dot(dir, photoGalacticPole)))))/5.;
	float factor = photoStarFactor[18];
	for (int k = 0; k < 18; ++k)
	{
		if (x < float(k+1))
		{
			factor = mix(photoStarFactor[k], photoStarFactor[k+1], x - float(k));
			break;
		}
	}
	return factor;
}
void main(void)
{
	const float PI = 3.14159265;
	vec4 winPos = projectionMatrixInverse * vec4(ndcPos, 1);
	bool ok = false;
	vec3 modelPos = unProject(winPos.x, winPos.y, ok).xyz;
	if(!ok)
	{
		FRAG_COLOR = vec4(0);
		return;
	}
	modelPos = normalize(modelPos);
	modelPos = applyAberrationToViewDir(modelPos);
	float modelZenithAngle = acos(-modelPos.z);
	float modelLongitude = atan(modelPos.x, modelPos.y);
	vec2 texc = vec2(modelLongitude/(2.*PI), modelZenithAngle/PI);

	float extinctionFactor;
	vec3 extinctionColor = vec3(1); // photographic mode: color of the extinction
	if(extinctionEnabled)
	{
		vec3 worldPos = winPosToWorldPos(winPos.x, winPos.y, ok);
		vec3 altAzViewDir = worldPosToAltAzPos(worldPos);
		float mag = extinctionMagnitude(altAzViewDir);
		if (photoMode)
		{
			extinctionFactor = pow(10., -0.4*mag); // physical extinction; light pollution is part of the sky luminance
			extinctionColor = photoExtinctionTint(normalize(altAzViewDir).z);
		}
		else
			extinctionFactor=pow(0.3, mag) * (1.1-bortleIntensity*0.1); // drop of one magnitude: should be factor 2.5 or 40%. We take 30%, it looks more realistic.
	}
	else
	{
		extinctionFactor = 1.;
	}

    vec4 texColor;
	if (photoMode)
	{
		// Averaged over the exposure (the sky turns past the pixel: star trails) and over the blur disk of a
		// defocused lens (the light of the points around). The display stretch of the image is undone for each
		// sample, before the light is added up.
		texColor = vec4(0);
		float weights = 0.;
		for (int i = 0; i < 128; ++i)
		{
			if (i >= photoSkyTrailSamples)
				break;
			vec4 smp = photoSkySample(i);
			vec3 p = modelPos;
			if (photoSkyDefocus != 0.)
			{
				bool okSample = false;
				p = applyAberrationToViewDir(normalize(unProject(winPos.x + smp.x, winPos.y + smp.y, okSample).xyz));
			}
			vec3 dir = photoSkyTurn(p, smp.w);
			vec2 uv = vec2(atan(dir.x, dir.y)/(2.*PI), acos(-dir.z)/PI);
			vec4 c = PHOTO_SKY_TEXTURE(mainTex, uv);
			if (photoDestretch <= 0.)
			{
				// L -> exp(f(ln L)): a parabola in log-log between x0 and x1, straight lines outside (keeps the color)
				float luminance = dot(c.rgb, vec3(0.2126, 0.7152, 0.0722));
				if (luminance > 0.)
				{
					float x = log(luminance);
					float xe = clamp(x, photoCurve.w, photoCurveTop);
					float f = photoCurve.x + xe*(photoCurve.y + xe*photoCurve.z) + (photoCurve.y + 2.*photoCurve.z*xe)*(x - xe);
					c.rgb *= exp(f - x);
				}
				else
					c.rgb = vec3(0.);
			}
			else if (photoDestretch != 1.)
			{
				float luminance = dot(c.rgb, vec3(0.2126, 0.7152, 0.0722));
				c.rgb *= luminance > 0. ? pow(luminance, photoDestretch - 1.) : 0.;
			}
			texColor += smp.z * c;
			weights += smp.z;
		}
		texColor /= max(weights, 1e-6);
		if (photoDestretch <= 0.)
		{
			// Both change slowly over the sky (the correction over 3 degrees, the stars over 5 degrees of galactic
			// latitude): once for the pixel, not for each of up to 128 samples
			if (photoCorrection)
			{
				// The smooth surplus of the image over the photometric panorama (see MilkyWay::init()),
				// taken off its luminance (the finer structure stays)
				float t = texture2D(photoCorrectionTex, texc).r*2. - 1.;
				float surplus = sign(t)*t*t*0.06386*photoCorrectionStrength;
				float lum = dot(texColor.rgb, vec3(0.2126, 0.7152, 0.0722));
				if (lum > 0.)
					texColor.rgb *= max(lum - surplus, 0.2*lum)/lum;
			}
			texColor.rgb *= photoStarLight(modelPos);
		}
	}
	else
		texColor = texture2D(mainTex, texc);
    vec4 color = texColor*vec4(brightness*extinctionColor,1)*extinctionFactor;
	if(saturation != 1.0)
		color.rgb = saturate(color.rgb, saturation);
	FRAG_COLOR = color;
}
)";
		ok = renderProgram->addShaderFromSourceCode(QOpenGLShader::Fragment, frag);
		if(!renderProgram->log().isEmpty())
			qWarning().noquote() << "MilkyWay: Warnings while compiling fragment shader:\n" << renderProgram->log();

		if(!ok) return;

		renderProgram->bindAttributeLocation("vertex", SKY_VERTEX_ATTRIB_INDEX);

		if(!StelPainter::linkProg(renderProgram.get(), "Milky Way render program"))
			return;

		renderProgram->bind();
		shaderVars.mainTex          = renderProgram->uniformLocation("mainTex");
		shaderVars.saturation       = renderProgram->uniformLocation("saturation");
		shaderVars.brightness       = renderProgram->uniformLocation("brightness");
		shaderVars.bortleIntensity  = renderProgram->uniformLocation("bortleIntensity");
		shaderVars.extinctionEnabled= renderProgram->uniformLocation("extinctionEnabled");
		shaderVars.projectionMatrixInverse = renderProgram->uniformLocation("projectionMatrixInverse");
		shaderVars.photoMode        = renderProgram->uniformLocation("photoMode");
		shaderVars.photoDestretch   = renderProgram->uniformLocation("photoDestretch");
		shaderVars.photoCurve       = renderProgram->uniformLocation("photoCurve");
		shaderVars.photoCurveTop    = renderProgram->uniformLocation("photoCurveTop");
		shaderVars.photoStarFactor  = renderProgram->uniformLocation("photoStarFactor");
		shaderVars.photoGalacticPole= renderProgram->uniformLocation("photoGalacticPole");
		shaderVars.photoCorrection  = renderProgram->uniformLocation("photoCorrection");
		shaderVars.photoCorrectionTex = renderProgram->uniformLocation("photoCorrectionTex");
		shaderVars.photoCorrectionStrength = renderProgram->uniformLocation("photoCorrectionStrength");
		renderProgram->release();
	}
	if(!renderProgram || !renderProgram->isLinked())
		return;

	StelToneReproducer* eye = core->getToneReproducer();

	Q_ASSERT(mainTex);	// A texture must be loaded before calling this

	// Photographic mode: the texture (linear, thanks to sRGB decoding) times its calibrated
	// luminance, recorded with the camera settings. No eye adaptation, no ad-hoc factors.
	const bool photoMode = drawer->getFlagPhotographicModeActive();
	if (photoMode)
	{
		const QVector4D lin = colorToShader(Vec4f(color[0], color[1], color[2], 1.f));
		const float k = static_cast<float>(photoLuminanceScale * intensity) * drawer->getPhotoSceneScale() * fader->getInterstate();
		renderProgram->bind();
		renderProgram->setUniformValue(shaderVars.photoMode, true);
		renderProgram->setUniformValue(shaderVars.photoDestretch, photoDestretch);
		renderProgram->setUniformValue(shaderVars.photoCurve, photoCurve[0], photoCurve[1], photoCurve[2], photoCurve[3]);
		renderProgram->setUniformValue(shaderVars.photoCurveTop, photoCurveTop);
		// The stars drawn as points take their light out of the texture
		float starFactors[19];
		const float drawnMag = photoDrawnStarsMagnitude(drawer);
		photoStarFactors(drawnMag, starFactors);
		if (std::fabs(drawnMag - photoLastStarsMagnitude) > 0.25f)
		{
			photoLastStarsMagnitude = drawnMag;
			qDebug().noquote() << "Photographic Milky Way: stars drawn down to magnitude" << QString::number(drawnMag, 'f', 1)
			                   << "(the picture shows them to" << QString::number(photoDetectionMagnitude, 'f', 1) << ")"
			                   << "- texture factor in the galactic plane" << QString::number(starFactors[0], 'f', 3)
			                   << ", at the poles" << QString::number(starFactors[18], 'f', 3);
		}
		renderProgram->setUniformValueArray(shaderVars.photoStarFactor, starFactors, 19, 1);
		const Vec3d pole = StelCore::matGalacticToJ2000 * Vec3d(0., 0., 1.);
		renderProgram->setUniformValue(shaderVars.photoGalacticPole, QVector3D(static_cast<float>(pole[0]), static_cast<float>(pole[1]), static_cast<float>(pole[2])));
		renderProgram->setUniformValue(shaderVars.extinctionEnabled, drawer->getFlagHasAtmosphere() && extinction.getExtinctionCoefficient()>=0.01f);
		renderProgram->setUniformValue(shaderVars.bortleIntensity, 1.f);
		const int mainTexSampler = 0;
		photoTex->bind(mainTexSampler);
		renderProgram->setUniformValue(shaderVars.mainTex, mainTexSampler);
		// Unit 1 is the extinction colors (setPhotoExtinctionUniforms below)
		const int correctionSampler = 2;
		const bool correction = photoCorrectionTex && photoCorrectionTex->bind(correctionSampler);
		renderProgram->setUniformValue(shaderVars.photoCorrection, correction);
		renderProgram->setUniformValue(shaderVars.photoCorrectionTex, correctionSampler);
		renderProgram->setUniformValue(shaderVars.photoCorrectionStrength, photoCorrectionStrength);
		QOpenGLContext::currentContext()->functions()->glActiveTexture(GL_TEXTURE0);
		renderProgram->setUniformValue(shaderVars.projectionMatrixInverse, projector->getProjectionMatrix().toQMatrix().inverted());
		const Vec3f& g = photoColorGain;
		renderProgram->setUniformValue(shaderVars.brightness, QVector3D(lin.x()*k*g[0], lin.y()*k*g[1], lin.z()*k*g[2]));
		renderProgram->setUniformValue(shaderVars.saturation, GLfloat(saturation));
		core->setAberrationUniforms(*renderProgram);
		projector->setUnProjectUniforms(*renderProgram);
		extinction.setForwardTransformUniforms(*renderProgram);
		drawer->setPhotoExtinctionUniforms(*renderProgram, 1);
		int texWidth = 4096, texHeight = 2048;
		photoTex->getDimensions(texWidth, texHeight);
		drawer->setPhotoSkyTrailUniforms(*renderProgram, modelView, texWidth/(2.*M_PI));
		auto& glf = *QOpenGLContext::currentContext()->functions();
		glf.glEnable(GL_BLEND);
		glf.glBlendFunc(GL_ONE,GL_ONE);
		bindVAO();
		glf.glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		releaseVAO();
		glf.glDisable(GL_BLEND);
		return;
	}

	// This RGB color corresponds to the night blue scotopic color = 0.25, 0.25 in xyY mode.
	// since milky way is always seen white RGB value in the texture (1.0,1.0,1.0)
	// Vec3f c = Vec3f(0.34165f, 0.429666f, 0.63586f);
	// This is the same color, just brighter to have Blue=1.
	//Vec3f c = Vec3f(0.53730381f, .675724216f, 1.0f);
	// The new texture (V0.13.1) is quite blue to start with. It is better to apply white color for it.
	//Vec3f c = Vec3f(1.0f, 1.0f, 1.0f);
	// Still better: Re-activate the configurable color!
	Vec3f c = color;

	// We must also adjust milky way to light pollution.
	// Is there any way to calibrate this?
	static LandscapeMgr *lMgr=GETSTELMODULE(LandscapeMgr);
	const float atmFadeIntensity = lMgr->getAtmosphereFadeIntensity();
	const float nelm = StelCore::luminanceToNELM(drawer->getLightPollutionLuminance());
	const float bortleIntensity = 1.f+(15.5f-2*nelm)*atmFadeIntensity; // smoothed Bortle index moderated by atmosphere fader.

	const float lum = drawer->surfaceBrightnessToLuminance(12.f+0.15f*bortleIntensity); // was 11.5; Source? How to calibrate the new texture?

	// Get the luminance scaled between 0 and 1
	float aLum =eye->adaptLuminanceScaled(lum*fader->getInterstate());

	// Bound a maximum luminance. GZ: Is there any reference/reason, or just trial and error?
	aLum = qMin(0.38f, aLum*2.f);


	// intensity of 1.0 is "proper", but allow boost for dim screens
	c*=aLum*static_cast<float>(intensity*intensityFovScale);

	//StelObjectMgr *omgr=GETSTELMODULE(StelObjectMgr); // Activate for debugging only
	//Q_ASSERT(omgr);
	// TODO: Find an even better balance with sky brightness, MW should be hard to see during Full Moon and at least somewhat reduced in smaller phases.
	// adapt brightness by atmospheric brightness. This block developed for ZodiacalLight, hopefully similarly applicable...
	const float atmLum = lMgr->getAtmosphereAverageLuminance();
	// Approximate values for Preetham: 10cd/m^2 at sunset, 3.3 at civil twilight (sun at -6deg). 0.0145 sun at -12, 0.0004 sun at -18,  0.01 at Full Moon!?
	//omgr->setExtraInfoString(StelObject::DebugAid, QString("AtmLum: %1<br/>").arg(QString::number(atmLum, 'f', 4)));
	// The atmLum of Bruneton's model is about 1/2 higher than that of Preetham/Schaefer. We must rebalance that!
	float atmFactor=0.35;
	const auto atmoModel = lMgr->getAtmosphereModel();
	const bool smsLikeAtmosphere = atmoModel.compare(L1S("showmysky"), Qt::CaseInsensitive) == 0 ||
	                               atmoModel.compare(L1S("lightweight"), Qt::CaseInsensitive) == 0;
	if (StelMainView::getInstance().getGLInformation().isHighGraphicsMode)
	{
		// These are adapted from the low-graphics branch
		if (smsLikeAtmosphere)
		{
			atmFactor=qMax(0.45f, 55.0f*(0.02f-0.2f*atmLum));
		}
		else
		{
			atmFactor=qMax(0.45f, 55.0f*(0.02f-atmLum));
		}
	}
	else
	{
		if (smsLikeAtmosphere)
		{
			atmFactor=qMax(0.35f, 50.0f*(0.02f-0.2f*atmLum)); // The factor 0.2f was found empirically. Nominally it should be 0.667, but 0.2 or at least 0.4 looks better.
		}
		else
		{
			atmFactor=qMax(0.35f, 50.0f*(0.02f-atmLum)); // keep visible in twilight, but this is enough for some effect with the moon.
		}
	}
	//omgr->addToExtraInfoString(StelObject::DebugAid, QString("AtmFactor: %1<br/>").arg(QString::number(atmFactor, 'f', 4)));
	c*=atmFactor*atmFactor;

	if (c[0]<0) c[0]=0;
	if (c[1]<0) c[1]=0;
	if (c[2]<0) c[2]=0;

	renderProgram->bind();

	renderProgram->setUniformValue(shaderVars.extinctionEnabled, drawer->getFlagHasAtmosphere() && extinction.getExtinctionCoefficient()>=0.01f);
	renderProgram->setUniformValue(shaderVars.bortleIntensity, bortleIntensity);
	renderProgram->setUniformValue(shaderVars.photoMode, false);

	const int mainTexSampler = 0;
	mainTex->bind(mainTexSampler);
	renderProgram->setUniformValue(shaderVars.mainTex, mainTexSampler);

	renderProgram->setUniformValue(shaderVars.projectionMatrixInverse, projector->getProjectionMatrix().toQMatrix().inverted());
	renderProgram->setUniformValue(shaderVars.brightness, colorToShader(c));
	renderProgram->setUniformValue(shaderVars.saturation, GLfloat(saturation));

	core->setAberrationUniforms(*renderProgram);
	projector->setUnProjectUniforms(*renderProgram);
	extinction.setForwardTransformUniforms(*renderProgram);

	auto& gl = *QOpenGLContext::currentContext()->functions();

	gl.glEnable(GL_BLEND);
	gl.glBlendFunc(GL_ONE,GL_ONE); // allow colored sky background
	bindVAO();
	gl.glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	releaseVAO();
	gl.glDisable(GL_BLEND);
}
