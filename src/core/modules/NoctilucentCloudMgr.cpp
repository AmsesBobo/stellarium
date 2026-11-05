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

#include "NoctilucentCloudMgr.hpp"
#include "StelApp.hpp"
#include "StelCore.hpp"
#include "StelLocation.hpp"
#include "StelModuleMgr.hpp"
#include "StelPainter.hpp"
#include "StelProjector.hpp"
#include "StelTranslator.hpp"
#include "SolarSystem.hpp"
#include "Planet.hpp"
#include "StelSkyDrawer.hpp"
#include "StelOpenGL.hpp"
#include "RefractionExtinction.hpp"

#include <QSettings>
#include <QOpenGLBuffer>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QVector2D>
#include <QVector3D>
#include <algorithm>
#include <cmath>

namespace
{
constexpr double EarthRadius = 6371.;     // km
constexpr double CloudHeight = 82.;       // km
// Sunlight that passes lower than this does not reach the clouds. The effective height of the Earth's shadow: the
// real light fades out over some tens of km, from the air and the ozone along the way (a later stage).
constexpr double ShadowHeight = 15.;      // km
constexpr double ShadowEdge = 3.;         // km: half the width of the edge of the shadow
// A provisional luminance of the veil (cd/m^2), of the order of the twilight sky at the Sun's altitude of -10 degrees
constexpr double ProvisionalLuminance = 0.1;
// With the Sun above this the sunlit clouds are lost in the bright sky (radians)
constexpr double HighestSunAltitude = -3.*M_PI/180.;
constexpr int VertexAttribIndex = 0;
}

NoctilucentCloudMgr::NoctilucentCloudMgr()
{
	setObjectName("NoctilucentCloudMgr");
}

NoctilucentCloudMgr::~NoctilucentCloudMgr()
{
}

void NoctilucentCloudMgr::init()
{
	QSettings* conf = StelApp::getInstance().getSettings();
	setFlagShow(conf->value("noctilucent_clouds/flag_show", false).toBool());
	addAction("actionShow_Noctilucent_Clouds", N_("Display Options"), N_("Noctilucent clouds"), "flagShow");

	// A full screen quad
	auto& gl = *QOpenGLContext::currentContext()->functions();
	vbo.reset(new QOpenGLBuffer(QOpenGLBuffer::VertexBuffer));
	vbo->create();
	vbo->bind();
	const GLfloat vertices[] = {-1, -1,  1, -1,  -1, 1,  1, 1};
	gl.glBufferData(GL_ARRAY_BUFFER, sizeof vertices, vertices, GL_STATIC_DRAW);
	vbo->release();
	vao.reset(new QOpenGLVertexArrayObject);
	vao->create();
	if (vao->isCreated())
	{
		vao->bind();
		vbo->bind();
		gl.glVertexAttribPointer(VertexAttribIndex, 2, GL_FLOAT, false, 0, nullptr);
		gl.glEnableVertexAttribArray(VertexAttribIndex);
		vbo->release();
		vao->release();
	}
}

double NoctilucentCloudMgr::getCallOrder(StelModuleActionName actionName) const
{
	// Before the landscape, which hides what is below its horizon; the light is added, so the order against the sky
	// and the stars does not matter
	if (actionName == StelModule::ActionDraw)
		return StelApp::getInstance().getModuleMgr().getModule("LandscapeMgr")->getCallOrder(actionName) - 1.;
	return 0.;
}

void NoctilucentCloudMgr::setFlagShow(bool b)
{
	if (b != flagShow)
	{
		flagShow = b;
		StelApp::immediateSave("noctilucent_clouds/flag_show", b);
		emit flagShowChanged(b);
	}
}

double NoctilucentCloudMgr::tangentHeight(const Vec3d& p, const Vec3d& sun)
{
	// On the sunlit side of the Earth the ray comes down to the point itself; behind it, the ray passes closest to the
	// center of the Earth before it arrives
	const double along = p*sun;
	return (along > 0. ? p.norm() : (p ^ sun).norm()) - EarthRadius;
}

double NoctilucentCloudMgr::lowestSunAltitude(double observerHeight)
{
	// The last point in the sunlight lies on the horizon of the observer towards the Sun. Seen from the center of the
	// Earth it lies this far from the observer (radians), and the Sun's rays reach it this far past their tangent point
	const double r = EarthRadius + CloudHeight;
	const double toHorizon = std::acos(EarthRadius/r) + std::acos(EarthRadius/(EarthRadius + std::max(0., observerHeight)));
	const double pastTangent = std::acos((EarthRadius + ShadowHeight - ShadowEdge)/r);
	return -(toHorizon + pastTangent);
}

void NoctilucentCloudMgr::buildProgram(const StelProjector& projector)
{
	program.reset();
	const Extinction& extinction = StelApp::getInstance().getCore()->getSkyDrawer()->getExtinction();
	std::unique_ptr<QOpenGLShaderProgram> newProgram(new QOpenGLShaderProgram);

	const QByteArray vert = StelOpenGL::globalShaderPrefix(StelOpenGL::VERTEX_SHADER) + R"(
ATTRIBUTE highp vec3 vertex;
VARYING highp vec3 ndcPos;
void main()
{
	gl_Position = vec4(vertex, 1.);
	ndcPos = vertex;
}
)";
	if (!newProgram->addShaderFromSourceCode(QOpenGLShader::Vertex, vert))
	{
		qWarning().noquote() << "NoctilucentCloudMgr: error in the vertex shader:\n" << newProgram->log();
		return;
	}

	// Everything is in the AltAz frame of the observer with its origin in the center of the Earth (km): the observer
	// stands on its z axis.
	const QByteArray frag = StelOpenGL::globalShaderPrefix(StelOpenGL::FRAGMENT_SHADER)
	                        + projector.getUnProjectShader()
	                        + extinction.getForwardTransformShader()
	                        + StelSkyDrawer::getPhotoExtinctionShader()
	                        + R"(
VARYING highp vec3 ndcPos;
uniform highp mat4 projectionMatrixInverse;
uniform bool extinctionEnabled;
uniform highp vec3 cloudSun;        // unit vector towards the Sun
uniform highp vec3 cloudRadii;      // of the observer, the Earth and the layer (km)
uniform highp vec2 cloudShadow;     // the effective height of the Earth's shadow (km) and half the width of its edge
uniform highp vec3 cloudLuminance;  // scene buffer

void main(void)
{
	highp vec4 winPos = projectionMatrixInverse * vec4(ndcPos, 1);
	bool ok = false;
	highp vec3 altAz = unProject(winPos.x, winPos.y, ok).xyz;
	if (!ok)
	{
		FRAG_COLOR = vec4(0);
		return;
	}
	altAz = normalize(altAz);
	highp float r0 = cloudRadii.x;
	highp float b = r0*altAz.z;
	if (b < 0. && b*b - r0*r0 + cloudRadii.y*cloudRadii.y > 0.)
	{
		FRAG_COLOR = vec4(0); // the line of sight meets the Earth
		return;
	}
	// where the line of sight, from inside the shell, leaves it
	highp float t = -b + sqrt(b*b - r0*r0 + cloudRadii.z*cloudRadii.z);
	highp vec3 p = vec3(0., 0., r0) + t*altAz;
	// the ray of sunlight to that point: the lowest it passes over the Earth (see tangentHeight() in the C++ code)
	highp float tangent = (dot(p, cloudSun) > 0. ? length(p) : length(cross(p, cloudSun))) - cloudRadii.y;
	highp float sunlit = smoothstep(cloudShadow.x - cloudShadow.y, cloudShadow.x + cloudShadow.y, tangent);

	highp vec3 color = sunlit*cloudLuminance;
	if (extinctionEnabled)
		color *= pow(10., -0.4*extinctionMagnitude(altAz)) * photoExtinctionTint(altAz.z);
	FRAG_COLOR = vec4(color, 1.);
}
)";
	if (!newProgram->addShaderFromSourceCode(QOpenGLShader::Fragment, frag))
	{
		qWarning().noquote() << "NoctilucentCloudMgr: error in the fragment shader:\n" << newProgram->log();
		return;
	}
	newProgram->bindAttributeLocation("vertex", VertexAttribIndex);
	if (!StelPainter::linkProg(newProgram.get(), "NoctilucentCloudMgr program"))
		return;
	program = std::move(newProgram);
}

void NoctilucentCloudMgr::draw(StelCore* core)
{
	if (!flagShow)
		return;
	StelSkyDrawer* drawer = core->getSkyDrawer();
	if (!drawer->getFlagPhotographicModeActive() || core->getCurrentPlanet()->getEnglishName() != QStringLiteral("Earth"))
		return;

	// The direction of the Sun (AltAz: x to the south, y to the east, z up)
	SolarSystem* ssystem = GETSTELMODULE(SolarSystem);
	Vec3d sun = core->j2000ToAltAz(ssystem->getSun()->getJ2000EquatorialPos(core), StelCore::RefractionOff);
	sun.normalize();
	const double sunAltitude = std::asin(std::clamp(sun[2], -1., 1.));

	// The observer, over a spherical Earth, is below the layer
	const double observerHeight = std::max(0., core->getCurrentLocation().altitude*0.001);
	if (observerHeight >= CloudHeight - 1. || sunAltitude > HighestSunAltitude || sunAltitude < lowestSunAltitude(observerHeight))
		return;

	const StelProjectorP prj = core->getProjection(StelCore::FrameAltAz, StelCore::RefractionAuto);
	if (!programProjector || !prj->isSameProjection(*programProjector))
	{
		buildProgram(*prj);
		programProjector = prj;
	}
	if (!program)
		return;

	const Extinction& extinction = drawer->getExtinction();
	const float luminance = static_cast<float>(ProvisionalLuminance)*drawer->getPhotoSceneScale();

	QOpenGLShaderProgram& p = *program;
	p.bind();
	p.setUniformValue("projectionMatrixInverse", prj->getProjectionMatrix().toQMatrix().inverted());
	prj->setUnProjectUniforms(p);
	extinction.setForwardTransformUniforms(p);
	drawer->setPhotoExtinctionUniforms(p, 1);
	p.setUniformValue("extinctionEnabled", drawer->getFlagHasAtmosphere() && extinction.getExtinctionCoefficient() >= 0.01f);
	p.setUniformValue("cloudSun", QVector3D(static_cast<float>(sun[0]), static_cast<float>(sun[1]), static_cast<float>(sun[2])));
	p.setUniformValue("cloudRadii", QVector3D(static_cast<float>(EarthRadius + observerHeight), static_cast<float>(EarthRadius),
	                                          static_cast<float>(EarthRadius + CloudHeight)));
	p.setUniformValue("cloudShadow", QVector2D(static_cast<float>(ShadowHeight), static_cast<float>(ShadowEdge)));
	p.setUniformValue("cloudLuminance", QVector3D(luminance, luminance, luminance));

	auto& gl = *QOpenGLContext::currentContext()->functions();
	gl.glEnable(GL_BLEND);
	gl.glBlendFunc(GL_ONE, GL_ONE);
	drawQuad();
	gl.glDisable(GL_BLEND);
	p.release();
}

void NoctilucentCloudMgr::drawQuad()
{
	auto& gl = *QOpenGLContext::currentContext()->functions();
	if (vao->isCreated())
		vao->bind();
	else
	{
		vbo->bind();
		gl.glVertexAttribPointer(VertexAttribIndex, 2, GL_FLOAT, false, 0, nullptr);
		gl.glEnableVertexAttribArray(VertexAttribIndex);
		vbo->release();
	}
	gl.glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	if (vao->isCreated())
		vao->release();
	else
		gl.glDisableVertexAttribArray(VertexAttribIndex);
}
