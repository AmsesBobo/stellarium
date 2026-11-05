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

#ifndef NOCTILUCENTCLOUDMGR_HPP
#define NOCTILUCENTCLOUDMGR_HPP

#include "StelModule.hpp"
#include "StelProjectorType.hpp"
#include "VecMath.hpp"

#include <memory>

class StelCore;
class QOpenGLShaderProgram;
class QOpenGLBuffer;
class QOpenGLVertexArrayObject;

//! @class NoctilucentCloudMgr
//! The noctilucent clouds of the Earth: an optically thin layer of ice crystals about 82 km above a spherical Earth,
//! which scatters sunlight once. The light is added to the sky, so the stars shine through it.
//!
//! The clouds show when the observer is in the dark but the layer still stands in the sunlight. A shader follows the
//! line of sight of each pixel to the shell of the layer and finds there the tangent height of the ray of sunlight
//! that reaches the point: the lowest height over the Earth on its way. Below an effective height of the shadow no
//! light gets through. That the clouds stand low over the northern horizon from Sweden in summer follows from this
//! geometry alone.
//!
//! This is the first stage: an even veil of a fixed luminance, everywhere and all year round, recorded by the
//! photographic mode like the sky and dimmed by the extinction.
class NoctilucentCloudMgr : public StelModule
{
	Q_OBJECT
	//! Show the noctilucent clouds (photographic mode)
	Q_PROPERTY(bool flagShow READ getFlagShow WRITE setFlagShow NOTIFY flagShowChanged)

public:
	NoctilucentCloudMgr();
	~NoctilucentCloudMgr() override;

	void init() override;
	void update(double) override {}
	void draw(StelCore* core) override;
	double getCallOrder(StelModuleActionName actionName) const override;

	bool getFlagShow() const {return flagShow;}

	//! The tangent height (km) of the ray of sunlight that reaches the point p (km from the center of the Earth): the
	//! lowest height over the Earth on its way from the Sun, whose direction is the unit vector sun
	static double tangentHeight(const Vec3d& p, const Vec3d& sun);
	//! The lowest altitude of the Sun (radians, negative) at which any of the layer in sight of an observer at the
	//! height observerHeight (km) still gets sunlight: then only its part on the horizon towards the Sun does
	static double lowestSunAltitude(double observerHeight);

public slots:
	void setFlagShow(bool b);

signals:
	void flagShowChanged(bool b);

private:
	bool flagShow = false;

	void buildProgram(const StelProjector& projector);
	void drawQuad();
	std::unique_ptr<QOpenGLShaderProgram> program;
	StelProjectorP programProjector;  //!< the projection the program was built for
	std::unique_ptr<QOpenGLBuffer> vbo;
	std::unique_ptr<QOpenGLVertexArrayObject> vao;
};

#endif // NOCTILUCENTCLOUDMGR_HPP
