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

#ifndef STELPHOTOLENSFLARE_HPP
#define STELPHOTOLENSFLARE_HPP

#include "VecMath.hpp"

#include <memory>
#include <vector>

class QOpenGLShaderProgram;
class QOpenGLBuffer;
class QOpenGLVertexArrayObject;
class StelProjector;

//! Lens reflections ("ghosts") of the photographic mode.
//!
//! Light of a bright source (the Sun, the Moon) that is reflected twice inside the lens, at two of its
//! surfaces, reaches the sensor as a defocused image of the aperture: a disk or polygon on the line
//! through the image center and the source. Paraxial (ray transfer matrix) model after
//! S. Lee, E. Eisemann: "Practical Real-Time Lens-Flare Rendering", Computer Graphics Forum 32(4), 2013:
//! - the lens is a real design, a double Gauss of 11 surfaces (the classic Zemax sample "Double Gauss
//!   28 degree field", focal length 100 mm), plus the sensor, which also reflects;
//! - for each pair of reflecting surfaces, the 2x2 matrices of the folded ray path give the position of
//!   the ghost (a multiple of the position of the source image) and its size (the image of the aperture
//!   stop, at the first passage through it), which scales with 1/N;
//! - each reflection takes the Fresnel reflectance of the surface: air-glass surfaces have a single-layer
//!   anti-reflection coating (MgF2, a quarter wave at 550 nm), which gives the ghosts their colors;
//!   cemented surfaces reflect almost nothing; the sensor reflects about 1%.
//! The ghost carries the flux of the source times both reflectances, spread over its area. The design
//! is scaled to the focal length of the view, so the layout of the ghosts does not depend on the field.
class StelPhotoLensFlare
{
public:
	//! A bright source: its window position (it may lie outside the viewport) and total flux (linear RGB,
	//! scene units, as recorded in the image)
	struct Source
	{
		Vec2f position;
		Vec3f flux;
	};
	//! The camera for the ghosts
	struct Camera
	{
		Vec2f center;           //!< optical axis, window pixels
		float focal = 1.f;      //!< focal length, pixels
		float corner = 1.f;     //!< half diagonal of the image, pixels
		float fNumber = 8.f;
		int blades = 0;         //!< diaphragm blades, 0 = round
		float straightness = 0.f; //!< 0 = round aperture (wide open), 1 = polygon
		float bladeAngle = 0.f; //!< direction of the normal of one blade edge, radians
		float strength = 1.f;   //!< factor on the reflectances (1 = single-layer coating)
		float visibleThreshold = 0.f; //!< scene value below which nothing can show
	};

	StelPhotoLensFlare();
	~StelPhotoLensFlare();

	//! Draw the ghosts of the given sources (additive, into the bound scene buffer, window coordinates)
	//! @param projectionMatrix window coordinates -> clip space
	void draw(const std::vector<Source>& sources, const Camera& camera, const Mat4f& projectionMatrix);

	//! Number of ghosts of the lens model
	int ghostCount() const {return static_cast<int>(ghosts.size());}

private:
	struct Ghost
	{
		float position;   //!< ghost center / source image position (from the image center)
		float size;       //!< ghost radius / focal length, at f/1
		bool flipped;     //!< the aperture image is inverted (rotated by 180 degrees)
		Vec3f reflectance; //!< product of both reflectances, for red, green and blue
	};
	std::vector<Ghost> ghosts;

	std::unique_ptr<QOpenGLShaderProgram> program;
	std::unique_ptr<QOpenGLBuffer> vbo;
	std::unique_ptr<QOpenGLVertexArrayObject> vao;
	bool glTried = false;
	bool glOk = false;

	void computeGhosts();
	void initGL();
};

#endif // STELPHOTOLENSFLARE_HPP
