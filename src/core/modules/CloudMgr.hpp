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
//! the sky. So far it shows simple dark test shapes, to prove the way the clouds take light away.
//!
//! All the other lights of the sky are added to the scene, the atmosphere too. Clouds must take light away instead, and
//! one pass is not enough for that: a line of sight towards a cloud sees the stars and the air behind the cloud through
//! the cloud, but the air in front of it directly. With the transmission T of the cloud, the part f of the light of the
//! air that lies in front of it, the scene S of what lies beyond the atmosphere and the light A of the atmosphere, the
//! result is
//!   S T + A (f + (1 - f) T)
//! Two passes around the atmosphere give this: with a = f + (1 - f) T,
//!   before the atmosphere: S -> S T/a
//!   after the atmosphere:  S T/a + A -> (S T/a + A) a
//! LandscapeMgr::draw() calls them, because the atmosphere and the landscape are drawn there: all that lies beyond the
//! atmosphere is drawn by then, and the landscape comes after the clouds, so they do not cover it. The bloom and the
//! metering of the camera work on the finished scene, so they see what the clouds leave.
//!
//! (With the setting landscape/draw_sun_after_atmosphere the Sun is only dimmed by a, not by T: it is off by default.)
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

	//! Take away what the clouds hide of the scene beyond the atmosphere. To be called before the atmosphere is drawn.
	void drawBeforeAtmosphere(StelCore* core);
	//! Take away what the clouds hide of the atmosphere. To be called after it and before the landscape.
	void drawAfterAtmosphere(StelCore* core);

public slots:
	void setFlagShow(bool b);

signals:
	void flagShowChanged(bool b);

private:
	enum Pass {BeforeAtmosphere = 0, AfterAtmosphere = 1};
	void drawPass(StelCore* core, Pass pass);
	bool isShown(const StelCore* core) const;

	bool flagShow = false;

	void buildProgram(const StelProjector& projector);
	void drawQuad();
	std::unique_ptr<QOpenGLShaderProgram> program;
	StelProjectorP programProjector;  //!< the projection the program was built for
	std::unique_ptr<QOpenGLBuffer> vbo;
	std::unique_ptr<QOpenGLVertexArrayObject> vao;
};

#endif // CLOUDMGR_HPP
