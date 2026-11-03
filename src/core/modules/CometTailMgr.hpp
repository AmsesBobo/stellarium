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

#ifndef COMETTAILMGR_HPP
#define COMETTAILMGR_HPP

#include "StelModule.hpp"
#include "VecMath.hpp"

#include <vector>

class StelCore;
class Comet;

//! @class CometTailMgr
//! The tails of the comets, from their physics, for the photographic mode.
//!
//! The dust tail follows the model of Finson and Probstein (1968): a grain that leaves the nucleus feels the
//! gravitation of the Sun lessened by the pressure of its light, by the factor (1 - beta), where beta is the
//! ratio of the radiation pressure to the gravitation (about 0.57/(rho s) for a grain of density rho in g/cm^3 and
//! radius s in micrometers). It moves on a Kepler orbit around a "lighter" Sun from the place and the velocity of
//! the nucleus at its release. The grains of one beta released at all times lie on a syndyne; those released at
//! one time with all beta on a synchrone. Together they give the shape of the dust tail: the small grains (beta near
//! 1) near the direction away from the Sun, the large ones trailing along the orbit.
//!
//! The ion tail follows the solar wind: away from the Sun, aberrated by the motion of the comet against the wind
//! (about 400 km/s).
//!
//! This first step draws them as guide lines in the sky, to compare their shapes with pictures of comets.
class CometTailMgr : public StelModule
{
	Q_OBJECT
	//! Draw the syndynes, synchrones and the direction of the ion tail of the bright comets as lines
	Q_PROPERTY(bool flagShowGuides READ getFlagShowGuides WRITE setFlagShowGuides NOTIFY flagShowGuidesChanged)

public:
	CometTailMgr();
	~CometTailMgr() override;

	void init() override;
	void update(double) override {}
	void draw(StelCore* core) override;
	double getCallOrder(StelModuleActionName actionName) const override;

	bool getFlagShowGuides() const {return flagShowGuides;}

	//! Position after the time dt (days) of a body at r0 (AU) with the velocity v0 (AU/day) in the field of a central
	//! mass with the gravitational parameter mu (AU^3/day^2, 0 for no force): the Kepler problem in universal
	//! variables (Curtis 2014, algorithm 3.4)
	static Vec3d propagate(const Vec3d& r0, const Vec3d& v0, double mu, double dt);
	//! The gravitational parameter of the Sun (AU^3/day^2): the square of the Gaussian constant
	static constexpr double SunMu = 0.01720209895*0.01720209895;
	//! Light time per AU (days)
	static constexpr double LightTimePerAU = 0.0057755183;
	//! The speed of the solar wind (AU/day): 400 km/s
	static constexpr double SolarWindSpeed = 400.*86400./1.495978707e8;

public slots:
	void setFlagShowGuides(bool b);

signals:
	void flagShowGuidesChanged(bool b);

private:
	bool flagShowGuides = false;

	//! A guide line: heliocentric ecliptic positions (AU) of the grains, with a color and a label at its end
	struct Guide
	{
		std::vector<Vec3d> points;
		Vec3f color;
		QString label;
	};
	//! The guide lines of a comet at the time jde (seen at the time jde less the light time)
	std::vector<Guide> computeGuides(const Comet* comet, double jde, double lightTime) const;
	void drawGuides(StelCore* core, const Comet* comet) const;
};

#endif // COMETTAILMGR_HPP
