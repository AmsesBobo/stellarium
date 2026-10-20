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

#ifndef STELPHOTOBLOOM_HPP
#define STELPHOTOBLOOM_HPP

#include <memory>
#include <vector>
#include <qopengl.h>

class QOpenGLShaderProgram;
class QOpenGLFramebufferObject;
class QOpenGLVertexArrayObject;

//! The lens halo ("bloom") of the photographic mode, applied to the whole scene image.
//!
//! A camera lens spreads a small fraction of the light of every point of the image into a wide,
//! faint halo. For point sources the photographic mode draws this as part of the star image, but
//! for extended bright objects (a lunar crescent, the Sun behind branches, lit windows of a
//! landscape) only a convolution of the image with the halo profile gives the right shape.
//!
//! The halo is a Moffat profile. A Moffat profile is a continuous mixture of Gaussians, so it is
//! built from a pyramid of Gaussian blurs: level k is the scene downsampled k times and blurred,
//! which is a Gaussian of width about 1.6 * 2^k pixels. The levels are added up with weights that
//! follow the Gaussian mixture of the Moffat profile. All in floating point, linear light.
//!
//! The diffraction spikes are done the same way: every pixel is smeared along the spike directions
//! with the spike profile (a Lorentzian, 1/(1+x^2/l^2)). Near taps read the full image, far taps
//! the downsampled levels (which keeps the far parts of the spikes smooth).
class StelPhotoBloom
{
public:
	StelPhotoBloom();
	~StelPhotoBloom();

	//! Compute the halo image of the given scene texture (linear light, w x h pixels).
	//! @param haloAlpha Moffat alpha of the halo, in pixels
	//! @param haloBeta Moffat beta of the halo
	//! @param vao a vertex array object with a full screen quad (2D vertices in [-1,1], attribute 0)
	//! @return true if the halo image (see haloTexture()) was computed
	//! @param spikeLines number of lines through each point (spikes/2, at most 9); 0 for no spikes
	//! @param spikeAngle direction of the first line on the screen, radians
	//! @param spikeLength length scale l of the spike profile, pixels
	bool process(GLuint sceneTexture, int w, int h, float haloAlpha, float haloBeta,
	             int spikeLines, float spikeAngle, float spikeLength, QOpenGLVertexArrayObject* vao);
	//! The halo image (full resolution, unit integral of the halo profile): add it, times the halo
	//! fraction, to the scene. Only valid after a successful process().
	GLuint haloTexture() const;
	//! Weight of the unblurred scene in the halo (the narrowest Gaussians of the mixture, which do
	//! not blur by more than a pixel): add the scene times this weight times the halo fraction.
	float identityWeight() const {return weightIdentity;}
	//! The diffraction spike image (full resolution), or 0 if there are no spikes. It holds the inner part
	//! of the spikes, up to NEAR_SPIKE_LENGTH pixels from each point (unit integral over the whole spikes).
	GLuint spikeTexture() const;
	//! The spikes are smeared from the image up to this distance (pixels); the far part is drawn with the stars.
	static constexpr float NEAR_SPIKE_LENGTH = 24.5f;

private:
	struct Level
	{
		int w = 0, h = 0;
		std::unique_ptr<QOpenGLFramebufferObject> down; //!< downsampled scene (later: accumulated halo)
		std::unique_ptr<QOpenGLFramebufferObject> temp; //!< after the horizontal blur
		std::unique_ptr<QOpenGLFramebufferObject> blur; //!< after both blurs
	};
	std::vector<Level> levels;
	int sceneW = 0, sceneH = 0;

	std::unique_ptr<QOpenGLShaderProgram> downProgram;
	std::unique_ptr<QOpenGLShaderProgram> blurProgram;
	std::unique_ptr<QOpenGLShaderProgram> combineProgram;
	std::unique_ptr<QOpenGLShaderProgram> spikeProgram;
	std::unique_ptr<QOpenGLFramebufferObject> spikeFBO;
	bool spikesDone = false;
	bool shadersOk = false;
	bool shadersTried = false;

	//! Weights of the levels, for the current halo profile
	std::vector<float> weights;
	float weightIdentity = 0.f;
	float weightsAlpha = -1.f, weightsBeta = -1.f;
	int weightsLevels = 0;

	//! Width (sigma, in scene pixels) of the Gaussian blur of each pyramid level
	static constexpr float BLUR_SIGMA = 1.5f;

	void initShaders();
	void resize(int w, int h);
	void computeWeights(float alpha, float beta);
	static double levelSigma(int k);
};

#endif // STELPHOTOBLOOM_HPP
