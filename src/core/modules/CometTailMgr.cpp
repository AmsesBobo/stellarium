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

#include <QSettings>
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
constexpr int SyndynePoints = 60;
constexpr int SynchronePoints = 40;
constexpr double IonTailLength = 0.5;   // AU
constexpr double GuideMagnitude = 8.;   // comets brighter than this get guide lines (and the selected one)
constexpr double MaxDistance = 5.;      // AU from the Sun: no tails beyond
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

Vec3d CometTailMgr::propagate(const Vec3d& r0, const Vec3d& v0, double mu, double dt)
{
	// No force (beta = 1): a straight line
	if (mu < 1e-12)
		return r0 + v0*dt;
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
	for (const double beta : SyndyneBetas)
	{
		Guide g;
		g.color = Vec3f(1.f, 0.75f, 0.3f);
		g.label = QString("β %1").arg(beta);
		for (int i = 0; i <= SyndynePoints; ++i)
		{
			const double f = static_cast<double>(i)/SyndynePoints;
			g.points.push_back(grain(beta, MaxAge*f*f));
		}
		guides.push_back(std::move(g));
	}
	// Synchrones: one age, all beta from 0.001 to 1
	for (const double age : SynchroneAges)
	{
		Guide g;
		g.color = Vec3f(0.55f, 0.75f, 1.f);
		g.label = QString("%1 d").arg(age);
		for (int i = 0; i <= SynchronePoints; ++i)
		{
			const double beta = std::pow(10., -3. + 3.*i/SynchronePoints);
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

void CometTailMgr::draw(StelCore* core)
{
	if (!flagShowGuides)
		return;
	static SolarSystem* ssystem = GETSTELMODULE(SolarSystem);
	if (!ssystem)
		return;
	const QList<StelObjectP>& selected = StelApp::getInstance().getStelObjectMgr().getSelectedObject();
	for (const PlanetP& p : ssystem->getAllMinorBodies())
	{
		if (p->getPlanetType() != Planet::isComet)
			continue;
		const Comet* comet = static_cast<const Comet*>(p.data());
		bool isSelected = false;
		for (const StelObjectP& s : selected)
			isSelected = isSelected || s.data() == p.data();
		if (!isSelected && !(comet->getVMagnitude(core) < GuideMagnitude))
			continue;
		drawGuides(core, comet);
	}
}
