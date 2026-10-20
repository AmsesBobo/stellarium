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

#include "StelPhotoLensFlare.hpp"
#include "StelOpenGL.hpp"
#include "StelPainter.hpp"

#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLBuffer>
#include <QOpenGLVertexArrayObject>
#include <QDebug>
#include <cmath>
#include <limits>

namespace
{
constexpr double INF = std::numeric_limits<double>::infinity();

//! A surface of the lens: radius of curvature (mm, positive: center of curvature behind the surface),
//! distance to the next surface (mm), refractive index behind it, and its kind
enum class Kind { Coated, Cemented, Stop, Sensor };
struct Surface
{
	double radius;
	double thickness;
	double index;
	Kind kind;
};

//! Double Gauss, the Zemax sample "Double Gauss 28 degree field" (focal length 99.5 mm, back focus 57.5 mm).
//! Glasses: SK2 (nd 1.60738), SK16 (1.62041), F5 (1.60342).
const Surface DOUBLE_GAUSS[] = {
	{  54.153,  8.747, 1.60738, Kind::Coated },
	{ 152.522,  0.500, 1.0,     Kind::Coated },
	{  35.951, 14.000, 1.62041, Kind::Coated },
	{     INF,  3.776, 1.60342, Kind::Cemented },
	{  22.270, 14.253, 1.0,     Kind::Coated },
	{     INF, 12.428, 1.0,     Kind::Stop },
	{ -25.685,  3.776, 1.60342, Kind::Coated },
	{     INF, 10.834, 1.62041, Kind::Cemented },
	{ -36.980,  0.500, 1.0,     Kind::Coated },
	{ 196.417,  6.858, 1.62041, Kind::Coated },
	{ -67.148,  0.0,   1.0,     Kind::Coated }, // the back focal distance is computed
};

//! 2x2 ray transfer matrix acting on (height, angle)
struct M2
{
	double a = 1., b = 0., c = 0., d = 1.;
	M2 operator*(const M2& o) const { return {a*o.a + b*o.c, a*o.b + b*o.d, c*o.a + d*o.c, c*o.b + d*o.d}; }
};
M2 translation(double t) { return {1., t, 0., 1.}; }
M2 refraction(double radius, double n1, double n2)
{
	return {1., 0., std::isinf(radius) ? 0. : (n1 - n2)/(n2*radius), n1/n2};
}
// Reflection, in the unfolded coordinates of the path
M2 reflection(double radius) { return {1., 0., std::isinf(radius) ? 0. : 2./radius, 1.}; }

//! Reflectance at normal incidence of an air-glass surface with a single-layer quarter-wave coating
//! (MgF2, n = 1.38, designed for 550 nm), at the given wavelength (nm)
double coatedReflectance(double glassIndex, double wavelength)
{
	const double n0 = 1.0, n1 = 1.38;
	const double r01 = (n0 - n1)/(n0 + n1);
	const double r12 = (n1 - glassIndex)/(n1 + glassIndex);
	const double cosDelta = std::cos(M_PI*550./wavelength); // phase 2 pi n1 (2 d) / lambda, d = 550/(4 n1)
	return (r01*r01 + r12*r12 + 2.*r01*r12*cosDelta) / (1. + r01*r01*r12*r12 + 2.*r01*r12*cosDelta);
}

const char* vertexSource = R"(
ATTRIBUTE highp vec2 corner;
uniform highp mat4 projectionMatrix;
uniform highp vec2 center;
uniform highp float radius;
VARYING highp vec2 vPos;
void main()
{
	vPos = corner * 1.2;
	gl_Position = projectionMatrix * vec4(center + vPos * radius, 0., 1.);
}
)";

// The image of the aperture: a disk, or a polygon of the diaphragm blades (straighter when stopped down)
const char* fragmentSource = R"(
VARYING highp vec2 vPos;   // in units of the ghost radius (circumradius of the polygon)
uniform highp vec3 color;  // irradiance inside the ghost
uniform int blades;
uniform highp float straightness;
uniform highp float rotation;
uniform highp float softness;
void main()
{
	const highp float PI = 3.14159265358979;
	highp float r = length(vPos);
	highp float shape = r;
	if (blades >= 3 && straightness > 0.)
	{
		highp float n = float(blades);
		highp float sector = 2.*PI/n;
		highp float a = atan(vPos.y, vPos.x) - rotation;
		highp float d = mod(a, sector) - 0.5*sector; // angle from the normal of the nearest edge
		highp float polygon = r * cos(d) / cos(PI/n);
		shape = mix(r, polygon, straightness);
	}
	highp float coverage = 1. - smoothstep(1. - softness, 1. + softness, shape);
	FRAG_COLOR = vec4(color * coverage, 1.);
}
)";
}

StelPhotoLensFlare::StelPhotoLensFlare()
{
	computeGhosts();
}

StelPhotoLensFlare::~StelPhotoLensFlare() = default;

void StelPhotoLensFlare::computeGhosts()
{
	std::vector<Surface> surfaces(std::begin(DOUBLE_GAUSS), std::end(DOUBLE_GAUSS));
	const size_t lensCount = surfaces.size();
	std::vector<double> indexBefore(lensCount);
	for (size_t i=0; i<lensCount; ++i)
		indexBefore[i] = (i == 0) ? 1.0 : surfaces[i-1].index;

	// The lens forward: focal length and back focal distance (the sensor sits in the paraxial focus)
	M2 forward;
	for (size_t i=0; i<lensCount; ++i)
	{
		forward = refraction(surfaces[i].radius, indexBefore[i], surfaces[i].index) * forward;
		if (i+1 < lensCount)
			forward = translation(surfaces[i].thickness) * forward;
	}
	const double backFocus = -forward.a/forward.c;
	surfaces.back().thickness = backFocus;
	const M2 imaging = translation(backFocus) * forward; // entrance -> sensor
	// Entrance -> stop: the stop radius for an entrance pupil of radius f/(2N) is |a| f/(2N)
	M2 toStop;
	for (size_t i=0; i<lensCount && surfaces[i].kind != Kind::Stop; ++i)
		toStop = translation(surfaces[i].thickness) * refraction(surfaces[i].radius, indexBefore[i], surfaces[i].index) * toStop;
	surfaces.push_back({INF, 0., 1.0, Kind::Sensor});
	indexBefore.push_back(1.0);
	const size_t count = surfaces.size();
	const double focal = -1./forward.c;

	// Reflectance of each surface for red, green and blue light
	const double wavelengths[3] = {610., 550., 465.};
	std::vector<Vec3d> reflectance(count, Vec3d(0.));
	for (size_t i=0; i<count; ++i)
	{
		for (int ch=0; ch<3; ++ch)
		{
			switch (surfaces[i].kind)
			{
				case Kind::Coated:
					reflectance[i][ch] = coatedReflectance(std::max(indexBefore[i], surfaces[i].index), wavelengths[ch]);
					break;
				case Kind::Cemented:
				{
					const double q = (indexBefore[i] - surfaces[i].index)/(indexBefore[i] + surfaces[i].index);
					reflectance[i][ch] = q*q;
					break;
				}
				case Kind::Sensor:
					reflectance[i][ch] = 0.01;
					break;
				default:
					break;
			}
		}
	}

	// The folded path of a ghost: forward to surface b, reflection, back to surface a, reflection, forward
	// to the sensor. Records the matrix at the first passage through the stop.
	auto ghostPath = [&](size_t a, size_t b, M2& atStop) {
		M2 m;
		bool stopSeen = false;
		auto passStop = [&](size_t i) { if (surfaces[i].kind == Kind::Stop && !stopSeen) { atStop = m; stopSeen = true; } };
		for (size_t i=0; i<b; ++i)
		{
			m = refraction(surfaces[i].radius, indexBefore[i], surfaces[i].index) * m;
			passStop(i);
			m = translation(surfaces[i].thickness) * m;
		}
		m = reflection(surfaces[b].radius) * m;
		for (size_t i=b-1; i>a; --i)
		{
			m = translation(surfaces[i].thickness) * m;
			m = refraction(-surfaces[i].radius, surfaces[i].index, indexBefore[i]) * m;
			passStop(i);
		}
		m = translation(surfaces[a].thickness) * m;
		m = reflection(-surfaces[a].radius) * m;
		m = translation(surfaces[a].thickness) * m;
		for (size_t i=a+1; i+1<count; ++i)
		{
			m = refraction(surfaces[i].radius, indexBefore[i], surfaces[i].index) * m;
			passStop(i);
			m = translation(surfaces[i].thickness) * m;
		}
		return m;
	};

	ghosts.clear();
	for (size_t a=0; a<count; ++a)
	{
		if (surfaces[a].kind == Kind::Stop)
			continue;
		for (size_t b=a+1; b<count; ++b)
		{
			if (surfaces[b].kind == Kind::Stop)
				continue;
			const Vec3d r(reflectance[a][0]*reflectance[b][0], reflectance[a][1]*reflectance[b][1], reflectance[a][2]*reflectance[b][2]);
			if (std::max(r[0], std::max(r[1], r[2])) < 1e-7)
				continue; // a cemented surface: nothing to see
			M2 atStop;
			const M2 m = ghostPath(a, b, atStop);
			if (std::fabs(atStop.a) < 1e-12)
				continue;
			// A ray with angle u through the stop at height h: sensor height = (m.a/s.a) h + (m.b - m.a s.b/s.a) u
			const double scale = m.a/atStop.a;
			Ghost g;
			g.position = static_cast<float>((m.b - scale*atStop.b)/imaging.b);
			g.size = static_cast<float>(std::fabs(scale*toStop.a*focal)/(2.*std::fabs(focal)));
			g.flipped = scale*toStop.a*imaging.b/focal < 0.;
			g.reflectance = Vec3f(static_cast<float>(r[0]), static_cast<float>(r[1]), static_cast<float>(r[2]));
			if (!std::isfinite(g.position) || !std::isfinite(g.size) || !(g.size > 0.f))
				continue;
			ghosts.push_back(g);
		}
	}
}

void StelPhotoLensFlare::initGL()
{
	glTried = true;
	program.reset(new QOpenGLShaderProgram);
	program->addShaderFromSourceCode(QOpenGLShader::Vertex, StelOpenGL::globalShaderPrefix(StelOpenGL::VERTEX_SHADER) + vertexSource);
	program->addShaderFromSourceCode(QOpenGLShader::Fragment, StelOpenGL::globalShaderPrefix(StelOpenGL::FRAGMENT_SHADER) + fragmentSource);
	program->bindAttributeLocation("corner", 0);
	if (!StelPainter::linkProg(program.get(), "photographic mode: lens reflections"))
	{
		program.reset();
		return;
	}
	const GLfloat corners[] = {-1,-1,  1,-1,  -1,1,  1,1};
	vao.reset(new QOpenGLVertexArrayObject);
	vao->create();
	vao->bind();
	vbo.reset(new QOpenGLBuffer(QOpenGLBuffer::VertexBuffer));
	vbo->create();
	vbo->bind();
	vbo->allocate(corners, sizeof(corners));
	auto& gl = *QOpenGLContext::currentContext()->functions();
	gl.glEnableVertexAttribArray(0);
	gl.glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
	vao->release();
	vbo->release();
	glOk = true;
}

void StelPhotoLensFlare::draw(const std::vector<Source>& sources, const Camera& camera, const Mat4f& projectionMatrix)
{
	if (sources.empty() || ghosts.empty())
		return;
	if (!glTried)
		initGL();
	if (!glOk)
		return;

	const float n = camera.blades >= 3 ? static_cast<float>(camera.blades) : 0.f;
	const float sector = n > 0.f ? 2.f*static_cast<float>(M_PI)/n : 0.f;
	// Area of the aperture image in units of its radius squared: disk, or regular polygon (circumradius 1)
	const float shapeArea = n > 0.f ? (1.f-camera.straightness)*static_cast<float>(M_PI) + camera.straightness*0.5f*n*std::sin(sector)
	                                : static_cast<float>(M_PI);

	auto& gl = *QOpenGLContext::currentContext()->functions();
	const GLboolean blendWasOn = gl.glIsEnabled(GL_BLEND);
	gl.glEnable(GL_BLEND);
	gl.glBlendFunc(GL_ONE, GL_ONE);
	program->bind();
	program->setUniformValue("projectionMatrix", projectionMatrix.toQMatrix());
	program->setUniformValue("blades", n > 0.f ? camera.blades : 0);
	program->setUniformValue("straightness", camera.straightness);
	vao->bind();

	for (const auto& source : sources)
	{
		const Vec2f p = source.position - camera.center;
		const float distance = std::hypot(p[0], p[1]);
		// Far outside the frame the lens barrel and the hood keep the light from the inner surfaces
		const float t = qBound(0.f, (distance/camera.corner - 1.2f)/0.8f, 1.f);
		const float fade = 1.f - t*t*(3.f - 2.f*t);
		if (!(fade > 0.f))
			continue;
		for (const auto& g : ghosts)
		{
			const float radius = qMax(1.5f, g.size/camera.fNumber*camera.focal);
			const float area = shapeArea*radius*radius;
			const Vec3f irradiance(source.flux[0]*g.reflectance[0], source.flux[1]*g.reflectance[1], source.flux[2]*g.reflectance[2]);
			const Vec3f color = irradiance * (camera.strength*fade/area);
			if (std::max(color[0], std::max(color[1], color[2])) < 0.5f*camera.visibleThreshold)
				continue;
			const Vec2f center = camera.center + p*g.position;
			// Off the screen entirely?
			if (std::fabs(center[0]-camera.center[0]) > 2.f*camera.corner + 1.2f*radius ||
			    std::fabs(center[1]-camera.center[1]) > 2.f*camera.corner + 1.2f*radius)
				continue;
			program->setUniformValue("center", QVector2D(center[0], center[1]));
			program->setUniformValue("radius", radius);
			program->setUniformValue("color", QVector3D(color[0], color[1], color[2]));
			program->setUniformValue("rotation", camera.bladeAngle - 0.5f*sector + (g.flipped ? static_cast<float>(M_PI) : 0.f));
			program->setUniformValue("softness", qMax(0.03f, 1.f/radius));
			gl.glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		}
	}
	vao->release();
	program->release();
	if (!blendWasOn)
		gl.glDisable(GL_BLEND);
}
