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
//! There are two layers. The settings of the clouds belong to the layer that the property layer chooses: a script sets
//! layer to 2 to change the second one. The higher layer is drawn first, the lower one over it, and the higher one casts
//! its shadows on the lower one.
//!
//! The wind carries the clouds, they grow and dissolve, and the camera averages its exposure over it. All of that, and the
//! cover if the setting follows the date, comes from the date alone: a moment always looks the same. It is made up, and
//! has nothing to do with the real weather of that day.
class CloudMgr : public StelModule
{
	Q_OBJECT
	//! Show the clouds (photographic mode)
	Q_PROPERTY(bool flagShow READ getFlagShow WRITE setFlagShow NOTIFY flagShowChanged)
	//! The layer that the settings below change: 1 or 2
	Q_PROPERTY(int layer READ getLayer WRITE setLayer NOTIFY layerChanged)
	//! Whether that layer shows (the first one does by default, the second one not)
	Q_PROPERTY(bool flagLayerOn READ getFlagLayerOn WRITE setFlagLayerOn NOTIFY flagLayerOnChanged)
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
	//! Show cirrus: thin fibrous clouds of ice high up, above the two layers
	Q_PROPERTY(bool flagCirrus READ getFlagCirrus WRITE setFlagCirrus NOTIFY flagCirrusChanged)
	//! How much of the sky the cirrus cover, from 0 to 1
	Q_PROPERTY(double cirrusCover READ getCirrusCover WRITE setCirrusCover NOTIFY cirrusCoverChanged)
	//! The height of the cirrus over the ground (km), from 5 to 13
	Q_PROPERTY(double cirrusHeight READ getCirrusHeight WRITE setCirrusHeight NOTIFY cirrusHeightChanged)
	//! The vertical optical depth of the densest cirrus, from 0.02 to 3 (typical 0.1 to 1)
	Q_PROPERTY(double cirrusOpticalDepth READ getCirrusOpticalDepth WRITE setCirrusOpticalDepth NOTIFY cirrusOpticalDepthChanged)
	//! Set the wind by hand, in place of the wind that follows from the date
	Q_PROPERTY(bool flagManualWind READ getFlagManualWind WRITE setFlagManualWind NOTIFY flagManualWindChanged)
	//! The speed of the wind set by hand (m/s), from 0 to 60, for the layer that the properties change. The cirrus go with
	//! the wind of the first layer, faster by their height
	Q_PROPERTY(double windSpeed READ getWindSpeed WRITE setWindSpeed NOTIFY windSpeedChanged)
	//! The direction that the wind set by hand blows from (degrees from the north through the east)
	Q_PROPERTY(double windDirection READ getWindDirection WRITE setWindDirection NOTIFY windDirectionChanged)
	//! Let the date set the cover, in place of the setting covers: weather fronts that the wind carries over the observer,
	//! with clear skies between them. This is no weather forecast.
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
	int getLayer() const {return edited + 1;}
	bool getFlagLayerOn() const {return layerOn[edited];}
	double getCover() const {return layers[edited].cover;}
	double getSize() const {return layers[edited].size;}
	bool getFlagFollowDate() const {return flagFollowDate;}
	double getBaseHeight() const {return layers[edited].base;}
	double getDensity() const {return layers[edited].density;}
	double getFlatness() const {return layers[edited].flatness;}
	double getBankAzimuth() const {return layers[edited].bankAzimuth;}
	double getBankWidth() const {return layers[edited].bankWidth;}
	double getBankDistance() const {return layers[edited].bankDistance;}
	double getBankDepth() const {return layers[edited].bankDepth;}
	bool getFlagBank() const {return layers[edited].bank;}
	int getType() const;
	static constexpr int TypeCustom = 0, TypeCumulus = 1, TypeStratocumulus = 2, TypeAltocumulus = 3;
	bool getFlagHalfResolution() const {return flagHalfResolution;}
	bool getFlagCirrus() const {return flagCirrus;}
	bool getFlagManualWind() const {return flagManualWind;}
	double getWindSpeed() const {return windSpeeds[edited];}
	double getWindDirection() const {return windDirections[edited];}
	double getCirrusCover() const {return cirrusCover;}
	double getCirrusHeight() const {return cirrusHeight;}
	double getCirrusOpticalDepth() const {return cirrusOpticalDepth;}
	QString getWeatherInfo() const {return weatherInfo;}

	//! The transmission of the clouds towards a direction (AltAz, without refraction, as the shader sees it) at this moment,
	//! from 0 to 1, for what is drawn over them:
	//! the lens effects of the bright bodies. The same field as the shader's.
	double getTransmission(const StelCore* core, Vec3d altAz) const;

	//! Draw the clouds over the sky. To be called after the atmosphere and before the landscape.
	void drawOverSky(StelCore* core);

public slots:
	void setFlagShow(bool b);
	void setLayer(int layer);
	void setFlagLayerOn(bool b);
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
	void setFlagCirrus(bool b);
	void setFlagManualWind(bool b);
	void setWindSpeed(double value);
	void setWindDirection(double value);
	void setCirrusCover(double value);
	void setCirrusHeight(double value);
	void setCirrusOpticalDepth(double value);

signals:
	void flagShowChanged(bool b);
	void layerChanged(int layer);
	void flagLayerOnChanged(bool b);
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
	void flagCirrusChanged(bool b);
	void flagManualWindChanged(bool b);
	void windSpeedChanged(double value);
	void windDirectionChanged(double value);
	void cirrusCoverChanged(double value);
	void cirrusHeightChanged(double value);
	void cirrusOpticalDepthChanged(double value);
	void weatherInfoChanged(const QString& info);

private:
	bool isShown(const StelCore* core) const;
	//! Whether the layer i shows: it is on, and the observer stands below it
	bool isLayerShown(const StelCore* core, int i) const;
	void setLightUniforms(const StelCore* core, QOpenGLShaderProgram& p, int i) const;
	//! Draw the layer i, under the layer other (-1 for none), which casts its shadows on it
	void drawLayer(StelCore* core, const StelProjector& prj, int i, int other);
	//! The key of config.ini of a setting of the layer that the settings change
	QString settingKey(const char* name) const;
	//! Tell the user interface that all the settings of the layer have changed
	void emitLayerSettings();

	bool flagShow = false;
	bool flagFollowDate = false;
	bool flagCirrus = false;
	bool flagManualWind = false;
	double windSpeeds[2] = {8., 12.};
	double windDirections[2] = {250., 260.};
	//! The winds: of the layers 0 and 1, and of the cirrus
	static constexpr int CirrusWind = 2;
	//! The height of the clouds of a wind (km)
	double windHeight(int wind) const;
	//! Where the wind has carried the clouds of a wind at the Julian day jd: from the date, faster higher up, or from the
	//! wind set by hand. The fronts go with the wind of the first layer
	CloudShape::Motion motionAt(const StelCore* core, double jd, int wind) const;
	double cirrusCover = 0.4;
	double cirrusHeight = 8.;
	double cirrusOpticalDepth = 0.15;
	double currentCirrusCover = 0.4;  //!< the cover of the cirrus at this moment
	bool isCirrusShown(const StelCore* core) const;
	//! The transmission of the cirrus towards a direction (AltAz, unit vector)
	double cirrusTransmission(const StelCore* core, const Vec3d& altAz) const;
	void buildCirrusProgram(const StelProjector& projector);
	void drawCirrus(StelCore* core, const StelProjector& prj);
	std::unique_ptr<QOpenGLShaderProgram> cirrusProgram;
	StelProjectorP cirrusProjector;
	static constexpr int LayerCount = 2;
	CloudShape::Layer layers[LayerCount];
	bool layerOn[LayerCount] = {true, false};
	int edited = 0;  //!< the layer that the settings change
	//! The layer i as it is drawn at this moment
	CloudShape::Layer currentLayer(int i) const;
	bool flagHalfResolution = true;
	QString weatherInfo;
	double currentCovers[LayerCount] = {0.2, 0.2};  //!< the cover at this moment, over the observer
	CloudShape::Front fronts[LayerCount];           //!< the fronts of the layers, if the date sets the cover
	CloudShape::Front cirrusFront;
	double windEast[2] = {0., 0.}, windNorth[2] = {0., 0.};  //!< the winds of the layers at the observer at this moment (km/s)
	//! The wind at the observer (km/s) on the map of the clouds, from where it carries them
	void computeWind(const StelCore* core, int wind, double* mapX, double* mapY) const;
	void setUniformsOfLayer(QOpenGLShaderProgram& p, int i, const char* prefix) const;

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
