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

#ifndef STELPHOTOMETER_HPP
#define STELPHOTOMETER_HPP

#include <memory>
#include <vector>
#include <qopengl.h>

class QOpenGLShaderProgram;
class QOpenGLFramebufferObject;
class QOpenGLVertexArrayObject;

//! The exposure meter of the photographic mode.
//!
//! Like the metering sensor of a camera, it divides the image into segments (a grid of
//! GRID x GRID cells) and measures the mean luminance of each. The scene image holds linear
//! light, scaled by the current exposure, so the readings are in the same units.
//! The segments are computed on the GPU and read back asynchronously (one frame late),
//! so that the measurement does not stall the rendering.
//!
//! From the segments, the meter gives the readings of the usual metering modes:
//! - center-weighted average: 75% of the weight in a central circle of 1/3 of the image height
//!   (like the classic 8 mm circle of a 24 mm high frame), 25% spread over the whole image
//! - spot: a central circle of 2.5% of the image area
//! - highlight: the brightest sub-segment (each segment is divided into 4 x 4 for this)
//! It also sums up the colors of the segments for the automatic white balance (grey world).
class StelPhotoMeter
{
public:
	//! Number of segments in each direction
	static constexpr int GRID = 48;

	struct Reading
	{
		double centerWeighted = 0.; //!< center-weighted mean (scene units)
		double spot = 0.;           //!< mean of the central spot (scene units)
		double highlight = 0.;      //!< brightest sub-segment (scene units)
		double sceneScale = 0.;     //!< scene units per cd/m^2 of the frame that was measured
		double exposureTime = 0.;   //!< exposure time of the frame that was measured (seconds)
		//! Sum of the linear red, green and blue of the segments that do not saturate (grey world white balance)
		double greyRed = 0., greyGreen = 0., greyBlue = 0.;
	};

	StelPhotoMeter();
	~StelPhotoMeter();

	//! Measure the scene texture (linear light, w x h pixels). The reading becomes available with
	//! a later call of takeReading(). sceneScale and exposureTime describe how the frame was exposed.
	//! @param vao a vertex array object with a full screen quad (2D vertices in [-1,1], attribute 0)
	//! Only the part of the texture from (x0, y0) of the size w x h is measured (the frame of the sensor); the
	//! texture itself is texWidth x texHeight (0: w x h).
	void measure(GLuint sceneTexture, int w, int h, double sceneScale, double exposureTime, QOpenGLVertexArrayObject* vao,
	             int x0 = 0, int y0 = 0, int texWidth = 0, int texHeight = 0);
	//! Get the newest finished reading. Returns false if there is none (yet).
	//! @param clipLevel scene value where the sensor saturates; brighter segments are left out of the grey world sums
	bool takeReading(Reading& reading, double clipLevel = 1e30);
	//! Forget pending measurements (e.g. when the metering was off for a while)
	void reset();

private:
	std::unique_ptr<QOpenGLShaderProgram> program;
	std::unique_ptr<QOpenGLFramebufferObject> fbo;
	GLuint pbo[2] = {0, 0};
	bool pending[2] = {false, false};
	Reading pendingInfo[2];
	int pendingW[2] = {0, 0}, pendingH[2] = {0, 0};
	int current = 0;
	bool initTried = false;
	bool ok = false;

	void init();
	Reading evaluate(const float* cells, int w, int h, double clipLevel) const;
};

#endif // STELPHOTOMETER_HPP
