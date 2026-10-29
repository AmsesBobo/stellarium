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

#ifndef STELEYEVISION_HPP
#define STELEYEVISION_HPP

#include <memory>
#include <qopengl.h>

class QOpenGLShaderProgram;
class QOpenGLFramebufferObject;
class QOpenGLVertexArrayObject;

//! Night vision for the naked eye mode, after Thompson, Shirley & Ferwerda (2002, "A spatial post-processing
//! algorithm for images of night scenes", Journal of Graphics Tools 7(1)): the loss of acuity in dim light, and
//! the noise of the few photons the rods catch.
//!
//! - Acuity: detail finer than the acuity of the eye at its adaptation (the highest spatial frequency R, see
//!   acuityLimit()) is seen only where it has more contrast than the light around it (at R the finest detail
//!   needs the full contrast). The coarse light is a Gaussian blur that keeps half of the contrast at R; of
//!   the finer detail only what stands out by more than the coarse light remains: the stars and the bright
//!   planets stay points, faint texture and dark lines are lost.
//! - Noise: the rods count photons. The rods within the area of spatial summation (a circle of the diameter
//!   of two periods of the finest detail) during the integration time of the eye catch N photons on average,
//!   with a relative fluctuation of 1/sqrt(N) (Poisson). The noise is smooth on the scale of a period, and
//!   moves on from one pattern to the next over each integration time. It is the share of the rods in the
//!   vision (1 - m, see StelEyeAdaptation::getMesopicM()) that is noisy.
//!
//! The lines and labels (their coverage in the alpha channel of the scene, see StelPainter) are neither blurred
//! nor noisy, and their light is not spread over the sky around them.
class StelEyeVision
{
public:
	StelEyeVision();
	~StelEyeVision();

	//! The highest spatial frequency the eye resolves (cycles per degree) at the adaptation luminance (cd/m^2):
	//! Ferwerda et al. (1996), a fit to the data of Shaler (1937)
	static double acuityLimit(double luminance);
	//! The standard deviation of the blur (degrees) for the limit R (cycles per degree): the Gaussian whose
	//! transfer function is 1/2 at R
	static double blurSigma(double limit);

	//! The rods: photoisomerizations per rod and second for one scotopic troland (a commonly used value,
	//! e.g. Pugh & Lamb 2000), their density in the periphery of the retina (per mm^2, Curcio et al. 1990),
	//! and the size of a degree on the retina (mm)
	static constexpr double ISOMERIZATIONS_PER_TROLAND = 8.6;
	static constexpr double RODS_PER_MM2 = 1.5e5;
	static constexpr double MM_PER_DEGREE = 0.29;

	struct Params
	{
		float sigma = 0.f;          //!< blur (pixels), 0: none
		float noiseScale = 0.f;     //!< photoisomerizations in a noise cell per unit of scotopic scene value, 0: no noise
		float noiseShare = 0.f;     //!< the noisy share of the vision (that of the rods)
		float cellSize = 4.f;       //!< size of a noise cell (pixels)
		float seed = 0.f;           //!< integration times (a new pattern after each, the fraction moves between them)
		float scotopic[3] = {0.f, 0.f, 0.f}; //!< the scotopic luminance of linear sRGB
	};

	//! Process the scene texture (w x h, linear light, the coverage of the annotations in alpha) into the output
	//! texture. Returns false when nothing was done (no shaders, nothing to do).
	bool process(GLuint sceneTexture, int w, int h, const Params& params, QOpenGLVertexArrayObject* vao);
	//! The processed scene (valid after a successful process())
	GLuint outputTexture() const;

private:
	void init();
	std::unique_ptr<QOpenGLShaderProgram> blurProgram, finalProgram;
	std::unique_ptr<QOpenGLFramebufferObject> horizontalFBO, verticalFBO, outputFBO;
	int width = 0, height = 0;
	bool initTried = false;
	bool ok = false;
};

#endif // STELEYEVISION_HPP
