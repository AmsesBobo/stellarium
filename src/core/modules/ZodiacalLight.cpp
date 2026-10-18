/*
 * Stellarium
 * Copyright (C) 2002 Fabien Chereau (MilkyWay class)
 * Copyright (C) 2014-17 Georg Zotti (followed pattern for ZodiacalLight)
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

#include "ZodiacalLight.hpp"
#include <cmath>
#include <vector>
#include "SolarSystem.hpp"
#include "StelFader.hpp"
#include "StelSRGB.hpp"
// For debugging draw() only:
//#include "StelObjectMgr.hpp"
#include "StelTexture.hpp"
#include "StelUtils.hpp"
#include "StelFileMgr.hpp"
#include "StelMainView.hpp"
#include "StelModuleMgr.hpp"
#include "LandscapeMgr.hpp"

#include "StelProjector.hpp"
#include "StelToneReproducer.hpp"
#include "StelApp.hpp"
#include "StelTextureMgr.hpp"
#include "StelCore.hpp"
#include "StelMovementMgr.hpp"
#include "StelSkyDrawer.hpp"
#include "StelPainter.hpp"
#include "StelTranslator.hpp"
#include "precession.h"

#include <QDebug>
#include <QSettings>
#include <QOpenGLBuffer>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>

namespace
{
constexpr int FS_QUAD_COORDS_PER_VERTEX = 2;
constexpr int SKY_VERTEX_ATTRIB_INDEX = 0;
}

// Class which manages the displaying of the Zodiacal Light
namespace
{
// Zodiacal light brightness seen from Earth, in S10 units (number of 10th magnitude stars per
// square degree, V band), as a function of the difference in ecliptic longitude from the Sun
// (rows) and the ecliptic latitude (columns).
// Source: Leinert Ch. et al. (1998), "The 1997 reference of diffuse night sky brightness",
// A&AS 127, 1-99, Table 16. https://doi.org/10.1051/aas:1998105
// Zero marks the region near the Sun without data; the ecliptic pole (90 deg) has 60 S10.
const float zlLongitudes[] = {0, 5, 10, 15, 20, 25, 30, 35, 40, 45, 60, 75, 90, 105, 120, 135, 150, 165, 180};
const float zlLatitudes[]  = {0, 5, 10, 15, 20, 25, 30, 45, 60, 75, 90};
const float zlTable[19][11] = {
	{   0,    0,    0, 2450, 1260, 770, 500, 215, 117, 78, 60},
	{   0,    0,    0, 2300, 1200, 740, 490, 212, 117, 78, 60},
	{   0,    0, 3700, 1930, 1070, 675, 460, 206, 116, 78, 60},
	{9000, 5300, 2690, 1450,  870, 590, 410, 196, 114, 78, 60},
	{5000, 3500, 1880, 1100,  710, 495, 355, 185, 110, 77, 60},
	{3000, 2210, 1350,  860,  585, 425, 320, 174, 106, 76, 60},
	{1940, 1460,  955,  660,  480, 365, 285, 162, 102, 74, 60},
	{1290,  990,  710,  530,  400, 310, 250, 151,  98, 73, 60},
	{ 925,  735,  545,  415,  325, 264, 220, 140,  94, 72, 60},
	{ 710,  570,  435,  345,  278, 228, 195, 130,  91, 70, 60},
	{ 395,  345,  275,  228,  190, 163, 143, 105,  81, 67, 60},
	{ 264,  248,  210,  177,  153, 134, 118,  91,  73, 64, 60},
	{ 202,  196,  176,  151,  130, 115, 103,  81,  67, 62, 60},
	{ 166,  164,  154,  133,  117, 104,  93,  75,  64, 60, 60},
	{ 147,  145,  138,  120,  108,  98,  88,  70,  60, 58, 60},
	{ 140,  139,  130,  115,  105,  95,  86,  70,  60, 57, 60},
	{ 140,  139,  129,  116,  107,  99,  91,  75,  62, 56, 60},
	{ 153,  150,  140,  129,  118, 110, 102,  81,  64, 56, 60},
	{ 180,  166,  152,  139,  127, 116, 105,  82,  65, 56, 60},
};

// Index and fraction of value x in an ascending grid
void gridPosition(const float* grid, int n, float x, int& i, float& f)
{
	i = 0;
	while (i < n-2 && x > grid[i+1])
		++i;
	f = qBound(0.f, (x-grid[i])/(grid[i+1]-grid[i]), 1.f);
}

// Zodiacal light in S10 for a longitude difference from the Sun and a latitude (degrees, >= 0).
// Interpolated in log space between the table entries; inside 15 degrees elongation, where the
// table has no data, it follows the inner power law of the zodiacal light (~ elongation^-2.3).
float zodiacalLightS10(float dLambda, float beta)
{
	const float elongation = std::acos(std::cos(dLambda*M_PI_180f) * std::cos(beta*M_PI_180f)) * M_180_PIf;
	int i, j;
	float fi, fj;
	gridPosition(zlLongitudes, 19, dLambda, i, fi);
	gridPosition(zlLatitudes, 11, beta, j, fj);
	const float v00 = zlTable[i][j], v01 = zlTable[i][j+1], v10 = zlTable[i+1][j], v11 = zlTable[i+1][j+1];
	if (elongation >= 15.f && v00 > 0.f && v01 > 0.f && v10 > 0.f && v11 > 0.f)
	{
		const float l = (1.f-fi)*(1.f-fj)*std::log(v00) + (1.f-fi)*fj*std::log(v01)
		              + fi*(1.f-fj)*std::log(v10) + fi*fj*std::log(v11);
		return std::exp(l);
	}
	// Near the Sun: anchored at 15 degrees on the ecliptic (9000 S10)
	return 9000.f * std::pow(qMax(elongation, 2.f)/15.f, -2.3f);
}
}

void ZodiacalLight::createPhotoTexture()
{
	if (photoTex || !StelMainView::getInstance().getGLInformation().isHighGraphicsMode)
		return;
	// Same layout as the 2004 texture: x = longitude from anti-sun (0) through the Sun (0.5),
	// y = angle from the south ecliptic pole (0) to the north ecliptic pole (1).
	constexpr int W = 360, H = 180;
	std::vector<float> data(W*H);
	for (int y=0; y<H; ++y)
	{
		const float beta = std::fabs(90.f - (y+0.5f)*180.f/H);
		for (int x=0; x<W; ++x)
		{
			const float dLambda = std::fabs((x+0.5f)*360.f/W - 180.f);
			// Limit the value near the Sun to stay well inside the range of half floats (max 65504).
			// There the Sun or the daylit sky outshines it anyway.
			data[static_cast<size_t>(y*W+x)] = qMin(zodiacalLightS10(dLambda, beta), 30000.f);
		}
	}
	auto& gl = *QOpenGLContext::currentContext()->functions();
	GLuint tex = 0;
	gl.glGenTextures(1, &tex);
	gl.glBindTexture(GL_TEXTURE_2D, tex);
	gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_R16F, W, H, 0, GL_RED, GL_FLOAT, data.data());
	gl.glBindTexture(GL_TEXTURE_2D, 0);
	photoTex = tex;
}

ZodiacalLight::ZodiacalLight()
	: propMgr(Q_NULLPTR)
	, color(1.f, 1.f, 1.f)
	, intensity(1.)
	, intensityFovScale(1.0)
	, intensityMinFov(0.25) // when zooming in further, Z.L. is no longer visible.
	, intensityMaxFov(2.5) // when zooming out further, Z.L. is fully visible (when enabled).
	, lastJD(-1.0E6)
	, lambdaSun(0.0)
	, shaderVars()
{
	setObjectName("ZodiacalLight");
	fader = new LinearFader();
	propMgr=StelApp::getInstance().getStelPropertyManager();
}

ZodiacalLight::~ZodiacalLight()
{
	delete fader;
	fader = Q_NULLPTR;
}

void ZodiacalLight::init()
{
	QSettings* conf = StelApp::getInstance().getSettings();
	Q_ASSERT(conf);

	auto& gl = *QOpenGLContext::currentContext()->functions();

	// The Paper describes brightness values over the complete sky, so also the texture covers the full sky. 
	// The data hole around the sun has been filled by useful values.
	mainTex = StelApp::getInstance().getTextureManager().createTexture(StelFileMgr::getInstallationDir()+"/textures/zodiacallight_2004.png");
	mainTex->bind(0);
	gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	mainTex->release();
	setFlagShow(conf->value("astro/flag_zodiacal_light", true).toBool());
	setIntensity(conf->value("astro/zodiacal_light_intensity",1.).toDouble());
	setColor(Vec3f(conf->value("astro/zodiacal_light_color", "1.0,1.0,1.0").toString()));

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
	addAction("actionShow_ZodiacalLight", displayGroup, N_("Zodiacal Light"), "flagZodiacalLightDisplayed", "Ctrl+Shift+Z");

	StelCore* core=StelApp::getInstance().getCore();
	connect(core, &StelCore::locationChanged, this, &ZodiacalLight::handleLocationChanged);
}

void ZodiacalLight::setupCurrentVAO()
{
	auto& gl = *QOpenGLContext::currentContext()->functions();
	vbo->bind();
	gl.glVertexAttribPointer(0, FS_QUAD_COORDS_PER_VERTEX, GL_FLOAT, false, 0, 0);
	vbo->release();
	gl.glEnableVertexAttribArray(SKY_VERTEX_ATTRIB_INDEX);
}

void ZodiacalLight::bindVAO()
{
	if(vao->isCreated())
		vao->bind();
	else
		setupCurrentVAO();
}

void ZodiacalLight::releaseVAO()
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

void ZodiacalLight::handleLocationChanged(const StelLocation &loc)
{
	// This just forces update() to re-compute longitude.
	Q_UNUSED(loc)
	lastJD=-1e12;
}

void ZodiacalLight::update(double deltaTime)
{
	fader->update(static_cast<int>(deltaTime*1000));
	if (!fader->getInterstate()  || (getIntensity()<0.01) )
		return;

	//calculate FOV fade value, linear fade between intensityMaxFov and intensityMinFov
	double fov = StelApp::getInstance().getCore()->getMovementMgr()->getCurrentFov();
	intensityFovScale = qBound(0.0,(fov - intensityMinFov) / (intensityMaxFov - intensityMinFov),1.0);

	StelCore* core=StelApp::getInstance().getCore();
	// Test if we are not on Earth or Moon. Texture would not fit, so don't draw then.
	if (! QString("Earth Moon").contains(core->getCurrentLocation().planetName)) return;

	double currentJD=core->getJD();
	if (qAbs(currentJD - lastJD) > 0.25) // should be enough to update position every 6 hours.
	{
		// Allowed locations are only Earth or Moon. For Earth, we can compute ZL along ecliptic of date.
		// For the Moon, we can only show ZL along J2000 ecliptic.
		// In draw() we have different projector frames. But we also need separate solar longitude computations here.
		// The ZL texture has its bright spot in the winter solstice point.
		// The computation here returns solar longitude for Earth, and antilongitude for the Moon, therefore we either add or subtract pi/2.

		if (core->getCurrentLocation().planetName=="Earth")
		{
			static SolarSystem *ss=GETSTELMODULE(SolarSystem);
			static PlanetP earth=ss->getEarth();
			static PlanetP sun=ss->getSun();
			double eclJDE = earth->getRotObliquity(core->getJDE());
			double ra_equ, dec_equ, betaJDE;
			StelUtils::rectToSphe(&ra_equ,&dec_equ,sun->getEquinoxEquatorialPos(core));
			StelUtils::equToEcl(ra_equ, dec_equ, eclJDE, &lambdaSun, &betaJDE);
			lambdaSun+= M_PI*0.5;
		}
		else // currently Moon only...
		{
			Vec3d obsPos=core->getObserverHeliocentricEclipticPos();
			lambdaSun=atan2(obsPos[1], obsPos[0])  -M_PI*0.5;
		}

		lastJD=currentJD;
	}
}

/*************************************************************************
 Reimplementation of the getCallOrder method
*************************************************************************/
double ZodiacalLight::getCallOrder(StelModuleActionName actionName) const
{
	if (actionName==StelModule::ActionDraw)
		return 8;
	return 0;
}

void ZodiacalLight::setFlagShow(bool b)
{
	if (*fader != b)
	{
		*fader = b;
		StelApp::immediateSave("astro/flag_zodiacal_light", b);
		emit zodiacalLightDisplayedChanged(b);
	}
}

bool ZodiacalLight::getFlagShow() const
{
	return *fader;
}

void ZodiacalLight::setIntensity(double aintensity)
{
	if(!qFuzzyCompare(aintensity, intensity))
	{
		intensity = aintensity;
		StelApp::immediateSave("astro/zodiacal_light_intensity", aintensity);
		emit intensityChanged(intensity);
	}
}

void ZodiacalLight::draw(StelCore* core)
{
	if (!core->getFlagClearSky())
		return;

	if ((fader->getInterstate() == 0.f) || (getIntensity()<0.01) || !(propMgr->getStelPropertyValue("SolarSystem.planetsDisplayed").toBool()))
		return;

	// Test if we are not on Earth. Texture would not fit, so don't draw then.
	if (! QString("Earth Moon").contains(core->getCurrentLocation().planetName)) return;

	// The ZL is best observed from Earth only. On the Moon, we must be happy with ZL along the J2000 ecliptic. (Sorry for LP:1628765, I don't find a general solution.)
	StelProjector::ModelViewTranformP transfo;
	if (core->getCurrentLocation().planetName == "Earth")
		transfo = core->getObservercentricEclipticOfDateModelViewTransform();
	else
		transfo = core->getObservercentricEclipticJ2000ModelViewTransform();

	const StelProjectorP projector = core->getProjection(transfo);
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
			qWarning().noquote() << "ZodiacalLight: Warnings while compiling vertex shader:\n" << renderProgram->log();
		if(!ok) return;

		const auto frag =
			StelOpenGL::globalShaderPrefix(StelOpenGL::FRAGMENT_SHADER) +
			projector->getUnProjectShader() +
			extinction.getForwardTransformShader() +
			R"(
VARYING highp vec3 ndcPos;
uniform sampler2D mainTex;
uniform vec3 brightness;
uniform mat4 projectionMatrixInverse;
uniform bool extinctionEnabled;
uniform float bortleIntensity;
uniform float lambdaSun;
uniform bool photoMode;      // photographic mode: absolute brightness from photoTex
uniform sampler2D photoTex;  // zodiacal light in S10 units (Leinert et al. 1998)
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
	float modelZenithAngle = acos(-modelPos.z);
	float modelLongitude = atan(modelPos.x, modelPos.y) + lambdaSun;
	vec2 texc = vec2(modelLongitude/(2.*PI), modelZenithAngle/PI);

	float extinctionFactor;
	if(extinctionEnabled)
	{
		vec3 worldPos = winPosToWorldPos(winPos.x, winPos.y, ok);
		vec3 altAzViewDir = worldPosToAltAzPos(worldPos);
		float mag = extinctionMagnitude(altAzViewDir);
		if (photoMode)
			extinctionFactor = pow(10., -0.4*mag); // physical extinction; light pollution is part of the sky luminance
		else
			extinctionFactor=pow(0.4, mag) * (1.1-bortleIntensity*0.1); // Drop of one magnitude: factor 2.5 or 40%, and further reduced by light pollution.
	}
	else
	{
		extinctionFactor = 1.;
	}

	if (photoMode)
	{
		FRAG_COLOR = vec4(texture2D(photoTex, texc).r * brightness * extinctionFactor, 1);
		return;
	}
    vec4 color = texture2D(mainTex, texc)*vec4(brightness,1)*extinctionFactor;
	FRAG_COLOR = color;
}
)";
		ok = renderProgram->addShaderFromSourceCode(QOpenGLShader::Fragment, frag);
		if(!renderProgram->log().isEmpty())
			qWarning().noquote() << "ZodiacalLight: Warnings while compiling fragment shader:\n" << renderProgram->log();

		if(!ok) return;

		renderProgram->bindAttributeLocation("vertex", SKY_VERTEX_ATTRIB_INDEX);

		if(!StelPainter::linkProg(renderProgram.get(), "ZodiacalLight render program"))
			return;

		renderProgram->bind();
		shaderVars.mainTex          = renderProgram->uniformLocation("mainTex");
		shaderVars.lambdaSun        = renderProgram->uniformLocation("lambdaSun");
		shaderVars.brightness       = renderProgram->uniformLocation("brightness");
		shaderVars.bortleIntensity  = renderProgram->uniformLocation("bortleIntensity");
		shaderVars.extinctionEnabled= renderProgram->uniformLocation("extinctionEnabled");
		shaderVars.projectionMatrixInverse = renderProgram->uniformLocation("projectionMatrixInverse");
		shaderVars.photoMode        = renderProgram->uniformLocation("photoMode");
		shaderVars.photoTex         = renderProgram->uniformLocation("photoTex");
		renderProgram->release();
	}
	if(!renderProgram || !renderProgram->isLinked())
		return;

	// Photographic mode: the zodiacal light with its measured absolute brightness, recorded by
	// the camera like the sky. It is simply added to the sky light, so it stays (realistically)
	// invisible under a bright or light-polluted sky without any special rules.
	if (drawer->getFlagPhotographicModeActive())
	{
		createPhotoTexture();
		if (!photoTex)
			return;
		// 1 S10 = one 10th magnitude star (2.54e-10 lx) per square degree (3.0462e-4 sr)
		constexpr float luminancePerS10 = 2.54e-10f / 3.0462e-4f; // cd/m^2
		const Vec3f linearColor(colorToShader(color).x(), colorToShader(color).y(), colorToShader(color).z());
		const Vec3f brightnessPhoto = linearColor * (luminancePerS10 * drawer->getPhotoSceneScale()
		                                             * static_cast<float>(intensity) * fader->getInterstate());
		renderProgram->bind();
		const bool withExtinctionPhoto=(drawer->getFlagHasAtmosphere() && drawer->getExtinction().getExtinctionCoefficient()>=0.01f);
		renderProgram->setUniformValue(shaderVars.extinctionEnabled, withExtinctionPhoto);
		renderProgram->setUniformValue(shaderVars.bortleIntensity, 1.f);
		renderProgram->setUniformValue(shaderVars.photoMode, true);
		auto& glf = *QOpenGLContext::currentContext()->functions();
		const int mainTexSampler = 0, photoTexSampler = 1;
		mainTex->bind(mainTexSampler);
		renderProgram->setUniformValue(shaderVars.mainTex, mainTexSampler);
		glf.glActiveTexture(GL_TEXTURE0 + photoTexSampler);
		glf.glBindTexture(GL_TEXTURE_2D, photoTex);
		glf.glActiveTexture(GL_TEXTURE0);
		renderProgram->setUniformValue(shaderVars.photoTex, photoTexSampler);
		renderProgram->setUniformValue(shaderVars.projectionMatrixInverse, projector->getProjectionMatrix().toQMatrix().inverted());
		renderProgram->setUniformValue(shaderVars.brightness, brightnessPhoto.toQVector());
		renderProgram->setUniformValue(shaderVars.lambdaSun, GLfloat(lambdaSun));
		projector->setUnProjectUniforms(*renderProgram);
		extinction.setForwardTransformUniforms(*renderProgram);
		glf.glEnable(GL_BLEND);
		glf.glBlendFunc(GL_ONE,GL_ONE);
		bindVAO();
		glf.glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		releaseVAO();
		glf.glDisable(GL_BLEND);
		return;
	}

	const float nelm = StelCore::luminanceToNELM(drawer->getLightPollutionLuminance());
	// Test for light pollution, return if too bad.
	if (drawer->getFlagHasAtmosphere() && nelm < 5.5) return;

	static LandscapeMgr *lMgr=GETSTELMODULE(LandscapeMgr);
	const float atmFadeIntensity = lMgr->getAtmosphereFadeIntensity();
	const float bortleIntensity = 1.f+(15.5f-2*nelm)*atmFadeIntensity; // smoothed Bortle index moderated by atmosphere fader.

	StelToneReproducer* eye = core->getToneReproducer();

	Q_ASSERT(mainTex);	// A texture must be loaded before calling this

	// Default ZL color is white (sunlight), or whatever has been set e.g. by script.
	Vec3f c = color;

	// ZL is quite sensitive to light pollution. I scale to make it less visible.
	const float lum = drawer->surfaceBrightnessToLuminance(13.5f + 0.5f*bortleIntensity); // (8.0f + 0.5*bortle);

	// Get the luminance scaled between 0 and 1
	float aLum =eye->adaptLuminanceScaled(lum*fader->getInterstate());

	// Bound a maximum luminance
	aLum = qMin(0.15f, aLum*2.f); // 2*aLum at dark sky with mag13.5 at Bortle=1 gives 0.13
	//qDebug() << "aLum=" << aLum;

	// intensity of 1.0 is "proper", but allow boost for dim screens
	c*=aLum*static_cast<float>(intensity*intensityFovScale);

	// Better: adapt brightness by atmospheric brightness
	const float atmLum = lMgr->getAtmosphereAverageLuminance();
	if (atmLum>0.05f) return; // Approximate values for Preetham: 10cd/m^2 at sunset, 3.3 at civil twilight (sun at -6deg). 0.0145 sun at -12, 0.0004 sun at -18,  0.01 at Full Moon!?
	// The atmLum of Bruneton's model is about 1/2 higher than that of Preetham/Schaefer. We must rebalance that!
	float atmFactor=20.0f;
	const auto atmoModel = lMgr->getAtmosphereModel();
	const bool smsLikeAtmosphere = atmoModel.compare(L1S("showmysky"), Qt::CaseInsensitive) == 0 ||
	                               atmoModel.compare(L1S("lightweight"), Qt::CaseInsensitive) == 0;
	if (smsLikeAtmosphere)
		atmFactor=20.0f*(0.05f-0.2f*atmLum); // The factor 0.2f was found empirically. Nominally it should be 0.667, but 0.2 or at least 0.4 looks better.
	else
		atmFactor=20.0f*(0.05f-atmLum);

	if (StelMainView::getInstance().getGLInformation().isHighGraphicsMode)
		atmFactor *= 1.1f;

	//GETSTELMODULE(StelObjectMgr)->addToExtraInfoString(StelObject::DebugAid, QString("ZL AtmFactor: %1<br/>").arg(QString::number(atmFactor, 'f', 4)));
	Q_ASSERT(atmFactor<=1.0f);
	Q_ASSERT(atmFactor>=0.0f);
	c*=atmFactor*atmFactor;

	if (c[0]<0) c[0]=0;
	if (c[1]<0) c[1]=0;
	if (c[2]<0) c[2]=0;

	renderProgram->bind();

	const bool withExtinction=(drawer->getFlagHasAtmosphere() && drawer->getExtinction().getExtinctionCoefficient()>=0.01f);

	renderProgram->setUniformValue(shaderVars.extinctionEnabled, withExtinction);
	renderProgram->setUniformValue(shaderVars.bortleIntensity, bortleIntensity);
	renderProgram->setUniformValue(shaderVars.photoMode, false);

	const int mainTexSampler = 0;
	mainTex->bind(mainTexSampler);
	renderProgram->setUniformValue(shaderVars.mainTex, mainTexSampler);

	renderProgram->setUniformValue(shaderVars.projectionMatrixInverse, projector->getProjectionMatrix().toQMatrix().inverted());
	renderProgram->setUniformValue(shaderVars.brightness, colorToShader(c));
	renderProgram->setUniformValue(shaderVars.lambdaSun, GLfloat(lambdaSun));

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
