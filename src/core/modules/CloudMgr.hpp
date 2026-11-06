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
#include "CloudShape.hpp"

#include <memory>
#include <vector>

class StelCore;
class QOpenGLShaderProgram;
class QOpenGLBuffer;
class QOpenGLVertexArrayObject;
class QOpenGLFramebufferObject;

//! @class CloudMgr
//! The ordinary clouds of the Earth, in the photographic mode: fair weather cumulus in a layer from 1.5 km above a
//! spherical Earth, in front of the sky. The shader follows the line of sight through the layer over a height map (see
//! CloudShape), so perspective alone packs the clouds together and shows their sides low over the horizon.
//!
//! All the other lights of the sky are added to the scene, the atmosphere too. Clouds must take light away instead: the
//! stars and the sky behind them go, and their own light and that of the air in front of them come. With the transmission
//! T of the cloud, the scene S with the atmosphere, and the light of the cloud and the air (see CloudLight),
//!   S -> S T + (L_cloud T_air + L_air) (1 - T)
//! LandscapeMgr::draw() calls this between the atmosphere and the landscape, so the clouds do not cover the landscape.
//! The bloom and the metering of the camera work on the finished scene, so they see what the clouds leave.
//!
//! The wind carries the clouds, they grow and dissolve, and the camera averages its exposure over it. All of that, and the
//! cover if the setting follows the date, comes from the date alone: a moment always looks the same. It is made up, and
//! has nothing to do with the real weather of that day.
class CloudMgr : public StelModule
{
	Q_OBJECT
	//! Show the clouds (photographic mode)
	Q_PROPERTY(bool flagShow READ getFlagShow WRITE setFlagShow NOTIFY flagShowChanged)
	//! How much of the sky the clouds cover, from 0 to 1 (a few broken clouds, not an overcast sky)
	Q_PROPERTY(double cover READ getCover WRITE setCover NOTIFY coverChanged)
	//! The size of the clouds against the usual ones (bodies of 0.6 to 2 km), from 0.4 to 2.5
	Q_PROPERTY(double size READ getSize WRITE setSize NOTIFY sizeChanged)
	//! The height of the base of the clouds over the ground (km), from 0.3 to 9
	Q_PROPERTY(double baseHeight READ getBaseHeight WRITE setBaseHeight NOTIFY baseHeightChanged)
	//! How dense the clouds are, from 0.05 (thin, the sky shows through) to 1 (opaque cumulus)
	Q_PROPERTY(double density READ getDensity WRITE setDensity NOTIFY densityChanged)
	//! From 0, heaped cumulus, to 1, flat and drawn out layers such as stratocumulus
	Q_PROPERTY(double flatness READ getFlatness WRITE setFlatness NOTIFY flatnessChanged)
	//! The direction of the middle of the bank (degrees from the north through the east)
	Q_PROPERTY(double bankAzimuth READ getBankAzimuth WRITE setBankAzimuth NOTIFY bankAzimuthChanged)
	//! The width of the bank (degrees of azimuth)
	Q_PROPERTY(double bankWidth READ getBankWidth WRITE setBankWidth NOTIFY bankWidthChanged)
	//! The distance of the near edge of the bank over the ground (km)
	Q_PROPERTY(double bankDistance READ getBankDistance WRITE setBankDistance NOTIFY bankDistanceChanged)
	//! The depth of the bank away from the observer (km)
	Q_PROPERTY(double bankDepth READ getBankDepth WRITE setBankDepth NOTIFY bankDepthChanged)
	//! The type of the clouds: 0 custom, 1 cumulus, 2 stratocumulus, 3 altocumulus. A type sets the base, size, density,
	//! flatness and cover; any change of the first four of them makes the type custom again
	Q_PROPERTY(int type READ getType WRITE setType NOTIFY typeChanged)
	//! Let the clouds only stand in a bank: a sector of the sky at a distance, for the clouds of a photograph
	Q_PROPERTY(bool flagBank READ getFlagBank WRITE setFlagBank NOTIFY flagBankChanged)
	//! Let the date set the cover, in place of the setting cover. This is no weather forecast.
	Q_PROPERTY(bool flagFollowDate READ getFlagFollowDate WRITE setFlagFollowDate NOTIFY flagFollowDateChanged)
	//! Compute the clouds at half the resolution of the screen and scale them up: four times faster, a little softer
	Q_PROPERTY(bool flagHalfResolution READ getFlagHalfResolution WRITE setFlagHalfResolution NOTIFY flagHalfResolutionChanged)
	//! The cover and the wind at this moment (for the user interface)
	Q_PROPERTY(QString weatherInfo READ getWeatherInfo NOTIFY weatherInfoChanged)

public:
	CloudMgr();
	~CloudMgr() override;

	void init() override;
	void update(double deltaTime) override;

	bool getFlagShow() const {return flagShow;}
	double getCover() const {return cover;}
	double getSize() const {return size;}
	bool getFlagFollowDate() const {return flagFollowDate;}
	double getBaseHeight() const {return layer.base;}
	double getDensity() const {return layer.density;}
	double getFlatness() const {return layer.flatness;}
	double getBankAzimuth() const {return layer.bankAzimuth;}
	double getBankWidth() const {return layer.bankWidth;}
	double getBankDistance() const {return layer.bankDistance;}
	double getBankDepth() const {return layer.bankDepth;}
	bool getFlagBank() const {return layer.bank;}
	int getType() const;
	static constexpr int TypeCustom = 0, TypeCumulus = 1, TypeStratocumulus = 2, TypeAltocumulus = 3;
	bool getFlagHalfResolution() const {return flagHalfResolution;}
	QString getWeatherInfo() const {return weatherInfo;}

	//! The transmission of the clouds towards a direction (AltAz, without refraction, as the shader sees it) at this moment,
	//! from 0 to 1, for what is drawn over them:
	//! the lens effects of the bright bodies. The same field as the shader's.
	double getTransmission(const StelCore* core, Vec3d altAz) const;

	//! Draw the clouds over the sky. To be called after the atmosphere and before the landscape.
	void drawOverSky(StelCore* core);

public slots:
	void setFlagShow(bool b);
	void setCover(double value);
	void setSize(double value);
	void setFlagFollowDate(bool b);
	void setBaseHeight(double value);
	void setDensity(double value);
	void setFlatness(double value);
	void setBankAzimuth(double value);
	void setBankWidth(double value);
	void setBankDistance(double value);
	void setBankDepth(double value);
	void setFlagBank(bool b);
	void setType(int type);
	void setFlagHalfResolution(bool b);

signals:
	void flagShowChanged(bool b);
	void coverChanged(double value);
	void sizeChanged(double value);
	void flagFollowDateChanged(bool b);
	void baseHeightChanged(double value);
	void densityChanged(double value);
	void flatnessChanged(double value);
	void bankAzimuthChanged(double value);
	void bankWidthChanged(double value);
	void bankDistanceChanged(double value);
	void bankDepthChanged(double value);
	void flagBankChanged(bool b);
	void typeChanged(int type);
	void flagHalfResolutionChanged(bool b);
	void weatherInfoChanged(const QString& info);

private:
	bool isShown(const StelCore* core) const;
	void setLightUniforms(const StelCore* core, QOpenGLShaderProgram& p) const;

	bool flagShow = false;
	double cover = 0.2;
	double size = 1.;
	bool flagFollowDate = false;
	//! The settings of the layer that are not above (its cover and size are filled in when it is used)
	CloudShape::Layer layer;
	//! The layer as it is drawn at this moment
	CloudShape::Layer currentLayer() const;
	bool flagHalfResolution = true;
	QString weatherInfo;
	double currentCover = 0.2;  //!< the cover at this moment
	double windEast = 0., windNorth = 0.;  //!< the wind at the observer at this moment (km/s)
	//! The wind at the observer (km/s) on the map of the clouds, from where it carries them
	void computeWind(const StelCore* core, double* mapX, double* mapY) const;

	//! The height map (see CloudShape), computed when it is first needed
	mutable std::vector<float> tile;
	unsigned int tileTexture = 0;
	void bindTileTexture(int unit);

	void buildProgram(const StelProjector& projector);
	void drawQuad();
	//! Draw the clouds into a buffer of half the size, ready to scale up with compositeProgram. False if that cannot be done.
	bool drawToLowResBuffer();
	std::unique_ptr<QOpenGLShaderProgram> program;
	StelProjectorP programProjector;  //!< the projection the program was built for
	std::unique_ptr<QOpenGLBuffer> vbo;
	std::unique_ptr<QOpenGLVertexArrayObject> vao;
	std::unique_ptr<QOpenGLFramebufferObject> lowResBuffer;
	std::unique_ptr<QOpenGLShaderProgram> compositeProgram;
};

#endif // CLOUDMGR_HPP
