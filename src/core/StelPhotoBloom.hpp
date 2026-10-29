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
#include <QVector2D>

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
	//! @param halationRadius film only: radius of the halation ring (total reflection in the film base), pixels; 0: none
	//! @param halationDensity film only: optical density of the antihalation layer (one pass, at normal incidence)
	//! @param aureoleCore, aureoleExtent the atmospheric aureole (see aureoleTexture()): the radius of its core and its
	//! extent, pixels; 0: none
	//! @param aureoleShare a texture over the scene (red): the share of the light of each point that goes into its
	//! aureole (it depends on the airmass; 0 for the ground)
	//! The halo is left out when haloAlpha is 0 (only the aureole).
	bool process(GLuint sceneTexture, int w, int h, float haloAlpha, float haloBeta,
	             int spikeLines, float spikeAngle, float spikeLength, QOpenGLVertexArrayObject* vao,
	             float halationRadius = 0.f, float halationDensity = 0.f,
	             float aureoleCore = 0.f, float aureoleExtent = 0.f, GLuint aureoleShare = 0);
	//! The aureole of the haze (full resolution), or 0: the light scattered forwards by the aerosols around every bright
	//! point (stars, planets, the Moon: the scene above its smooth background, so that the sky does not get an aureole
	//! of its own), each point times its share (aureoleShare), spread with the profile 1/(r^2 + core^2) out to about the
	//! extent (unit integral). Add it to the scene.
	GLuint aureoleTexture() const;
	//! The halation image (unit integral of its profile), or 0. Its texture coordinates are those of the scene
	//! times halationScale().
	GLuint halationTexture() const;
	//! The share of the diffuse light at the back of the emulsion that the film base sends back (see
	//! computeHalationWeights()), for an antihalation layer of the given density
	static double halationReturned(float density);
	QVector2D halationScale() const;
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
		std::unique_ptr<QOpenGLFramebufferObject> aureole; //!< accumulated aureole (made when needed)
		std::unique_ptr<QOpenGLFramebufferObject> auDown, auTemp, auBlur; //!< the pyramid of the aureole's source
	};
	std::unique_ptr<QOpenGLFramebufferObject> aureoleSourceFBO;  //!< the light that gets an aureole (full resolution)
	std::unique_ptr<QOpenGLShaderProgram> aureoleSourceProgram;
	std::vector<Level> levels;
	int sceneW = 0, sceneH = 0;

	std::unique_ptr<QOpenGLShaderProgram> downProgram;
	std::unique_ptr<QOpenGLShaderProgram> blurProgram;
	std::unique_ptr<QOpenGLShaderProgram> combineProgram;
	std::unique_ptr<QOpenGLShaderProgram> spikeProgram;
	std::unique_ptr<QOpenGLFramebufferObject> spikeFBO;
	bool spikesDone = false;
	//! Halation of a film: the ring at the level of the pyramid that suits its radius
	std::unique_ptr<QOpenGLShaderProgram> halationProgram;
	std::unique_ptr<QOpenGLFramebufferObject> halationFBO;
	int halationLevel = -1;
	bool halationDone = false;
	float halationWeightsDensity = -1.f;
	int halationRings = 0;
	float halationOffsets[12] = {};   //!< radii of the rings of taps, in units of the critical radius
	float halationWeights[12] = {};   //!< their shares of the light (sum 1)
	void computeHalationWeights(float density);
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
	//! The aureole: weights of the levels for its profile
	std::vector<float> aureoleWeights;
	float aureoleWeightsCore = -1.f, aureoleWeightsExtent = -1.f;
	int aureoleWeightsLevels = 0;
	bool aureoleDone = false;
	void computeAureoleWeights(float core, float extent);
	static double levelSigma(int k);
};

#endif // STELPHOTOBLOOM_HPP
