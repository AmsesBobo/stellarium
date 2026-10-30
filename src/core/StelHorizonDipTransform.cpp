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

#include "StelHorizonDipTransform.hpp"

#include <QOpenGLShaderProgram>
#include <cmath>

StelHorizonDipTransform::StelHorizonDipTransform(const Mat4d& altAzToView, double d)
	: pre(Mat4d::identity()), preInverse(Mat4d::identity()), post(altAzToView), postInverse(altAzToView.inverse()), dip(d)
{
}

double StelHorizonDipTransform::dipAltitude(double a, double d)
{
	// below the horizon from the nadir to -dip, above it from -dip to the zenith
	return a <= 0. ? -d + a*(M_PI_2 - d)/M_PI_2 : -d + a*(M_PI_2 + d)/M_PI_2;
}

double StelHorizonDipTransform::undipAltitude(double a, double d)
{
	return a <= -d ? (a + d)*M_PI_2/(M_PI_2 - d) : (a + d)*M_PI_2/(M_PI_2 + d);
}

namespace
{
	void mapVector(Vec3d& v, double d, bool inverse)
	{
		const double rxy = std::sqrt(v[0]*v[0] + v[1]*v[1]);
		const double len = std::sqrt(rxy*rxy + v[2]*v[2]);
		if (!(len > 0.))
			return;
		const double a = std::atan2(v[2], rxy);
		const double b = inverse ? StelHorizonDipTransform::undipAltitude(a, d) : StelHorizonDipTransform::dipAltitude(a, d);
		const double newRxy = len*std::cos(b);
		if (rxy > 1e-12*len)
		{
			v[0] *= newRxy/rxy;
			v[1] *= newRxy/rxy;
		}
		v[2] = len*std::sin(b);
	}
}

void StelHorizonDipTransform::dipVector(Vec3d& v, double d)
{
	mapVector(v, d, false);
}

void StelHorizonDipTransform::undipVector(Vec3d& v, double d)
{
	mapVector(v, d, true);
}

void StelHorizonDipTransform::forward(Vec3d& v) const
{
	v.transfo4d(pre);
	mapVector(v, dip, false);
	v.transfo4d(post);
}

void StelHorizonDipTransform::backward(Vec3d& v) const
{
	v.transfo4d(postInverse);
	mapVector(v, dip, true);
	v.transfo4d(preInverse);
}

void StelHorizonDipTransform::forward(Vec3f& v) const
{
	Vec3d d = v.toVec3d();
	forward(d);
	v.set(static_cast<float>(d[0]), static_cast<float>(d[1]), static_cast<float>(d[2]));
}

void StelHorizonDipTransform::backward(Vec3f& v) const
{
	Vec3d d = v.toVec3d();
	backward(d);
	v.set(static_cast<float>(d[0]), static_cast<float>(d[1]), static_cast<float>(d[2]));
}

void StelHorizonDipTransform::combine(const Mat4d& m)
{
	pre = pre*m;
	preInverse = pre.inverse();
}

StelProjector::ModelViewTranformP StelHorizonDipTransform::clone() const
{
	return StelProjector::ModelViewTranformP(new StelHorizonDipTransform(*this));
}

static QByteArray horizonDipCommonShader()
{
	return QByteArray(1+R"(
#ifndef HORIZON_DIP_DEFINED
#define HORIZON_DIP_DEFINED
uniform highp float HORIZONDIP_dip;
// See StelHorizonDipTransform::dipAltitude() and undipAltitude()
highp vec3 HORIZONDIP_map(highp vec3 v, bool inverse)
{
	const highp float HALF_PI = 1.5707963268;
	highp float rxy = length(v.xy);
	highp float len = length(v);
	if (len <= 0.)
		return v;
	highp float d = HORIZONDIP_dip;
	highp float a = atan(v.z, rxy);
	highp float b;
	if (inverse)
		b = a <= -d ? (a + d)*HALF_PI/(HALF_PI - d) : (a + d)*HALF_PI/(HALF_PI + d);
	else
		b = a <= 0. ? -d + a*(HALF_PI - d)/HALF_PI : -d + a*(HALF_PI + d)/HALF_PI;
	highp float newRxy = len*cos(b);
	if (rxy > 1e-12*len)
		v.xy *= newRxy/rxy;
	v.z = len*sin(b);
	return v;
}
#endif
)");
}

QByteArray StelHorizonDipTransform::getForwardTransformShader() const
{
	return horizonDipCommonShader() + QByteArray(1+R"(
uniform highp mat4 HORIZONDIP_pre;
uniform highp mat4 HORIZONDIP_post;
highp vec3 vertexToAltAzPos(highp vec3 v)
{
	return HORIZONDIP_map((HORIZONDIP_pre * vec4(v,1)).xyz, false);
}
highp vec3 modelViewForwardTransform(highp vec3 v)
{
	return (HORIZONDIP_post * vec4(vertexToAltAzPos(v),1)).xyz;
}
)");
}

void StelHorizonDipTransform::setForwardTransformUniforms(QOpenGLShaderProgram& program) const
{
	program.setUniformValue("HORIZONDIP_dip", static_cast<GLfloat>(dip));
	program.setUniformValue("HORIZONDIP_pre", toMat4f(pre).toQMatrix());
	program.setUniformValue("HORIZONDIP_post", toMat4f(post).toQMatrix());
}

QByteArray StelHorizonDipTransform::getBackwardTransformShader() const
{
	return horizonDipCommonShader() + QByteArray(1+R"(
uniform highp mat4 HORIZONDIP_preInverse;
uniform highp mat4 HORIZONDIP_postInverse;
highp vec3 worldPosToAltAzPos(highp vec3 worldPos)
{
	return (HORIZONDIP_postInverse * vec4(worldPos,1)).xyz;
}
highp vec3 modelViewBackwardTransform(highp vec3 worldPos)
{
	return (HORIZONDIP_preInverse * vec4(HORIZONDIP_map(worldPosToAltAzPos(worldPos), true),1)).xyz;
}
)");
}

void StelHorizonDipTransform::setBackwardTransformUniforms(QOpenGLShaderProgram& program) const
{
	program.setUniformValue("HORIZONDIP_dip", static_cast<GLfloat>(dip));
	program.setUniformValue("HORIZONDIP_preInverse", toMat4f(preInverse).toQMatrix());
	program.setUniformValue("HORIZONDIP_postInverse", toMat4f(postInverse).toQMatrix());
}
