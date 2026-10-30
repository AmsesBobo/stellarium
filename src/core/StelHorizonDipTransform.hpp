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

#ifndef STELHORIZONDIPTRANSFORM_HPP
#define STELHORIZONDIPTRANSFORM_HPP

#include "StelProjector.hpp"

//! A model-view transformation that lowers the horizon of a landscape to the true horizon of an elevated observer
//! (the dip of the horizon). In the AltAz frame the altitudes are mapped linearly: the horizon (0 degrees) of the
//! landscape goes to -dip, while the zenith and the nadir stay in place. So a generated landscape like the ocean
//! meets the sky at the sea-level horizon seen from altitude, its far parts squeezed towards it.
//!
//! The transformation is: input vertex -> (pre) AltAz of the landscape -> dip -> (post) view.
//! combine() adds to the pre matrix (rotations and shifts of the landscape); the post matrix is the AltAz-to-view
//! matrix of the core.
class StelHorizonDipTransform : public StelProjector::ModelViewTranform
{
public:
	//! @param altAzToView the AltAz model-view matrix of the core (no refraction)
	//! @param dip depression of the horizon (radians, positive: the horizon lies below 0 degrees)
	StelHorizonDipTransform(const Mat4d& altAzToView, double dip);

	void forward(Vec3d& v) const override;
	void backward(Vec3d& v) const override;
	void forward(Vec3f& v) const override;
	void backward(Vec3f& v) const override;
	void combine(const Mat4d& m) override;
	Mat4d getApproximateLinearTransfo() const override {return post*pre;}
	StelProjector::ModelViewTranformP clone() const override;
	QByteArray getForwardTransformShader() const override;
	void setForwardTransformUniforms(QOpenGLShaderProgram& program) const override;
	QByteArray getBackwardTransformShader() const override;
	void setBackwardTransformUniforms(QOpenGLShaderProgram& program) const override;

	//! Altitude (radians) where a direction of the landscape at the given altitude is shown
	static double dipAltitude(double altitude, double dip);
	//! The inverse of dipAltitude(): the altitude in the landscape of a direction seen at the given altitude
	static double undipAltitude(double altitude, double dip);
	//! Apply dipAltitude() or undipAltitude() to a direction vector of the AltAz frame (its length is kept)
	static void dipVector(Vec3d& v, double dip);
	static void undipVector(Vec3d& v, double dip);

private:
	Mat4d pre, preInverse, post, postInverse;
	double dip;
};

#endif // STELHORIZONDIPTRANSFORM_HPP
