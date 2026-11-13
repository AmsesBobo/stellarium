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
#include <QMap>
#include <memory>

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
//! The guide lines show these shapes in the sky, to compare them with pictures of comets.
//!
//! In the photographic mode the module draws the light of the bright comets (in place of the drawing of Comet): the
//! dust as many grains on a grid of beta and age, each spread out by the speed it left the nucleus with (the coma
//! of the dust with its 1/rho profile comes from the young grains), with the size distribution of the grains, the
//! production of dust with the distance from the Sun, and the phase function of cometary dust (forward scattering);
//! and the gas of the coma (Haser profile). Dust and gas together give the magnitude of the comet in the coma.
//! The ion tail is drawn from streams of CO+ ions released at earlier times along the solar wind as the comet saw
//! it then: they accelerate to some 200 km/s and fade over a few days; the wind changes its direction and speed
//! a little with time, so knots and kinks move down the tail, and rays leave the head at angles and fold onto it.
//! Now and then (as the comet crosses a boundary between the sectors of the field of the solar wind) the tail is
//! cut off from the head, and the cut moves down the tail (disconnection events).
//! The sodium tail is drawn from atoms pushed away from the Sun by the light they scatter in the D lines, weaker
//! when the comet moves slowly to or from the Sun (Swings effect); the dust is released in bursts now and then,
//! which give striae along the synchrones. The large grains (beta below 0.01) linger along the orbit for years,
//! released also far from the Sun: the anti-tail when the Earth passes the plane of the orbit. The emissions
//! (CO+, the gas of the coma, sodium) take the colors the color films record them with.
//!
//! A comet can have its own parameters of the model in ssystem_minor.ini (see Comet::TailParameters): the share of
//! the dust in the coma or its Afrho, the range of beta and the size distribution of the grains, the strength of
//! the striae, the shares of the ion and the sodium tails.
class CometTailMgr : public StelModule
{
	Q_OBJECT
	//! Draw the light of the bright comets in the photographic mode (in place of their usual drawing)
	Q_PROPERTY(bool flagShow READ getFlagShow WRITE setFlagShow NOTIFY flagShowChanged)
	//! Draw the syndynes, synchrones and the direction of the ion tail of the bright comets as lines
	Q_PROPERTY(bool flagShowGuides READ getFlagShowGuides WRITE setFlagShowGuides NOTIFY flagShowGuidesChanged)
	Q_PROPERTY(bool flagHalfResolution READ getFlagHalfResolution WRITE setFlagHalfResolution NOTIFY flagHalfResolutionChanged)

public:
	CometTailMgr();
	~CometTailMgr() override;

	void init() override;
	void update(double) override {}
	void draw(StelCore* core) override;
	double getCallOrder(StelModuleActionName actionName) const override;

	bool getFlagShowGuides() const {return flagShowGuides;}
	bool getFlagShow() const {return flagShow;}
	bool getFlagHalfResolution() const {return flagHalfResolution;}
	//! Whether this module draws the light of the comet now (photographic mode): Comet then draws only its marks
	bool drawsComet(const StelCore* core, const Comet* comet) const;

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
	void setFlagShow(bool b);
	//! Draw the light of the tails at half the resolution of the screen (the gas coma at the head stays sharp)
	void setFlagHalfResolution(bool b);

signals:
	void flagShowGuidesChanged(bool b);
	void flagShowChanged(bool b);
	void flagHalfResolutionChanged(bool b);

private:
	bool flagShowGuides = false;
	bool flagShow = true;
	bool flagHalfResolution = false;

	//! The grains of the dust of a comet, computed for one time (they are kept while the time changes little)
	struct DustGrid
	{
		double jde = -1e30;          //!< the time of the observation they were computed for
		std::vector<Vec3d> offsets;  //!< positions relative to the nucleus (AU), beta by beta, age by age
		std::vector<float> weights;  //!< cross section of the grains a splat stands for (relative)
		std::vector<float> spread;   //!< the radius of the cloud from the speed of ejection (AU)
	};
	mutable QMap<const Comet*, DustGrid> grids;
	const DustGrid& dustGrid(const Comet* comet, double jde, double lightTime) const;
	//! Draw the light of the comet (photographic mode)
	void drawLight(StelCore* core, const Comet* comet);
	//! Light time to the comet (days) at the time jde
	static double lightTime(const Comet* comet, double jde, const Vec3d& observer);

	//! The splats of the light: quads with a Gaussian (dust) or a Haser profile (gas)
	std::unique_ptr<class QOpenGLShaderProgram> splatProgram;
	std::unique_ptr<class QOpenGLBuffer> splatBuffer;
	std::unique_ptr<class QOpenGLVertexArrayObject> splatVao;
	bool splatProgramTried = false;
	bool initSplats();
	std::vector<float> vertexData;
	// At half resolution: the buffer the tails are drawn into, and the program that adds it to the scene
	std::unique_ptr<class QOpenGLFramebufferObject> lowResBuffer;
	std::unique_ptr<class QOpenGLShaderProgram> compositeProgram;
	bool compositeProgramTried = false;
	bool beginHalfResolution(const float area[4], int previous[5]);
	void endHalfResolution(const int previous[5]);

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
