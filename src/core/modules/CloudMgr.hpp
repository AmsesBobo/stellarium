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

#ifndef CLOUDMGR_HPP
#define CLOUDMGR_HPP

#include "StelModule.hpp"
#include "StelProjectorType.hpp"

#include <memory>

class StelCore;
class QOpenGLShaderProgram;
class QOpenGLBuffer;
class QOpenGLVertexArrayObject;

//! @class CloudMgr
//! The ordinary clouds of the Earth, in the photographic mode: a layer about 1.5 km above a spherical Earth, in front of
//! the sky. So far it has simple test shapes.
//!
//! All the other lights of the sky are added to the scene, the atmosphere too. Clouds must take light away instead: the
//! stars and the sky behind them go, and their own light and that of the air in front of them come. With the transmission
//! T of the cloud, the scene S with the atmosphere, and the light of the cloud and the air (see CloudLight),
//!   S -> S T + (L_cloud T_air + L_air) (1 - T)
//! LandscapeMgr::draw() calls this between the atmosphere and the landscape, so the clouds do not cover the landscape.
//! The bloom and the metering of the camera work on the finished scene, so they see what the clouds leave.
class CloudMgr : public StelModule
{
	Q_OBJECT
	//! Show the clouds (photographic mode)
	Q_PROPERTY(bool flagShow READ getFlagShow WRITE setFlagShow NOTIFY flagShowChanged)

public:
	CloudMgr();
	~CloudMgr() override;

	void init() override;
	void update(double) override {}

	bool getFlagShow() const {return flagShow;}

	//! The transmission of the clouds towards a direction (AltAz) at this moment, from 0 to 1, for what is drawn over them:
	//! the lens effects of the bright bodies. The same field as the shader's.
	double getTransmission(const StelCore* core, Vec3d altAz) const;

	//! Draw the clouds over the sky. To be called after the atmosphere and before the landscape.
	void drawOverSky(StelCore* core);

public slots:
	void setFlagShow(bool b);

signals:
	void flagShowChanged(bool b);

private:
	bool isShown(const StelCore* core) const;
	void setLightUniforms(const StelCore* core, QOpenGLShaderProgram& p) const;

	bool flagShow = false;

	void buildProgram(const StelProjector& projector);
	void drawQuad();
	std::unique_ptr<QOpenGLShaderProgram> program;
	StelProjectorP programProjector;  //!< the projection the program was built for
	std::unique_ptr<QOpenGLBuffer> vbo;
	std::unique_ptr<QOpenGLVertexArrayObject> vao;
};

#endif // CLOUDMGR_HPP
