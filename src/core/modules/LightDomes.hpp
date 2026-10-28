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

#ifndef LIGHTDOMES_HPP
#define LIGHTDOMES_HPP

#include "StelModule.hpp"
#include "VecMath.hpp"

#include <QFuture>
#include <QString>
#include <memory>
#include <vector>
#include <qopengl.h>

//! The light domes of towns over the sky (photographic mode): the light of their street lamps, scattered by the
//! air and the aerosols towards the observer, brightest above the horizon in their direction and larger the closer
//! they are.
//!
//! The model is that of Garstang (1986, PASP 98, 364; 1991, PASP 103, 1109): a town sends its light upwards with
//! the emission function I(z') = L/(2 pi) [2 G (1 - F) cos z' + 0.554 F z'^4] (G = 0.15 of the light that falls on
//! the ground is reflected, F = 0.15 goes directly upwards, mostly at low angles); the light is scattered once by
//! molecules (Rayleigh) and aerosols (Henyey-Greenstein, g = 0.7) in an exponential atmosphere over a curved Earth,
//! dimmed on its way up and on its way to the observer. L is the population times the lumens per inhabitant; 1000 lm
//! reproduce Walker's law (the sky at 45 degrees towards a town brightens by 0.01 P d^-2.5 of the natural sky,
//! Walker 1977) between 10 and 40 km. The aerosols follow the extinction coefficient of the sky settings.
//!
//! The scattered light of a town of 1 lumen is computed once for a table of distances, zenith angles and
//! azimuths from the town, in red, green and blue (610, 550 and 465 nm: the blue light of LEDs is scattered more),
//! in a background thread; the sky of all the towns is then a sum over the table. The result is a map of the sky
//! luminance (cd/m^2, linear sRGB of the scene) in altitude and azimuth, which the atmospheres add to the sky in the
//! photographic mode (photoLightDomeLight() in xyYToRGB.glsl). Only the artificial light: the natural sky (airglow,
//! zodiacal light, starlight) is drawn by itself. The light pollution setting of the sky is then the whole zenith:
//! what the domes do not give there is spread evenly (local lights, see getZenithLuminance()).
//!
//! The towns are given by hand (sources), or found in the location database of Stellarium, which has the population
//! of about 34000 places (of 15000 inhabitants and more): all those within 400 km. The database lists the districts
//! of large cities as places of their own (Soedermalm, Islington), while the city itself has the population of the
//! whole: a place inside the disk of a larger one is part of it, and left out.
class LightDomes : public StelModule
{
	Q_OBJECT
	Q_PROPERTY(bool flagEnabled READ getFlagEnabled WRITE setFlagEnabled NOTIFY flagEnabledChanged)
	//! The towns: "azimuth,distance,population,lamp" (degrees from north towards east, km, inhabitants, Lamp),
	//! separated by semicolons
	Q_PROPERTY(QString sources READ getSources WRITE setSources NOTIFY sourcesChanged)
	Q_PROPERTY(double lumensPerInhabitant READ getLumensPerInhabitant WRITE setLumensPerInhabitant NOTIFY lumensPerInhabitantChanged)
	//! The artificial luminance at the zenith of the computed domes (cd/m^2), for the dialog
	Q_PROPERTY(double zenithLuminance READ getZenithLuminance NOTIFY mapChanged)
	//! Add the towns of the location database (with their population) within 400 km of the observer, besides the
	//! towns given in sources
	Q_PROPERTY(bool flagDatabaseTowns READ getFlagDatabaseTowns WRITE setFlagDatabaseTowns NOTIFY flagDatabaseTownsChanged)
	//! The street lamps of the towns of the location database (Lamp)
	Q_PROPERTY(int databaseTownLamp READ getDatabaseTownLamp WRITE setDatabaseTownLamp NOTIFY databaseTownLampChanged)
	//! Write the names of the towns of the location database whose domes show at the horizon (like the gazetteer of
	//! a landscape, in its colors)
	Q_PROPERTY(bool flagTownLabels READ getFlagTownLabels WRITE setFlagTownLabels NOTIFY flagTownLabelsChanged)

public:
	//! The lamps of a town: the color of their light (and with it how much of it the air scatters)
	enum Lamp {
		LampMixed = 0,   //!< half high pressure sodium, half white LEDs (4000 K)
		LampHPS = 1,     //!< high pressure sodium (about 2100 K)
		LampLPS = 2,     //!< low pressure sodium (589 nm)
		LampLED3000 = 3, //!< white LEDs, 3000 K
		LampLED4000 = 4  //!< white LEDs, 4000 K
	};
	Q_ENUM(Lamp)
	struct Source
	{
		double azimuth = 0.;     //!< degrees from north towards east
		double distance = 10.;   //!< km
		double population = 1e4;
		int lamp = LampMixed;
	};
	//! A town of the location database around the observer
	struct DatabaseTown
	{
		QString name;
		double azimuth = 0.;     //!< degrees from north towards east
		double distance = 0.;    //!< km
		double population = 0.;
		int districts = 0;       //!< smaller places of the database inside it (counted in its population)
	};

	LightDomes();
	~LightDomes() override;
	void init() override;
	void update(double deltaTime) override;
	void draw(StelCore* core) override;
	double getCallOrder(StelModuleActionName actionName) const override;

	//! Whether the domes are drawn now: on, photographic mode, on the Earth with its atmosphere, and computed
	bool isActive() const;
	//! For the atmospheres (GL context current): bind the map to the texture unit, uploading a new one first.
	//! Returns false when there is none.
	bool bindMap(int unit);
	//! The artificial luminance at the zenith (cd/m^2, luminance) of the domes when active, else 0
	double getZenithLuminance() const;
	//! The light of the domes on the ground (horizontal illuminance, lux, linear sRGB of the scene) when active
	Vec3f getIlluminance() const;
	//! The color of a lamp (linear sRGB of the scene per unit luminance, daylight white = 1, 1, 1)
	static Vec3f lampColor(int lamp);
	//! Map size: azimuth columns (1 degree) and altitude rows (altitude = 90 degrees (row/(rows-1))^2)
	static constexpr int MAP_AZIMUTHS = 360;
	static constexpr int MAP_ALTITUDES = 48;

	bool getFlagEnabled() const {return flagEnabled;}
	QString getSources() const;
	double getLumensPerInhabitant() const {return lumensPerInhabitant;}
	bool getFlagDatabaseTowns() const {return flagDatabaseTowns;}
	int getDatabaseTownLamp() const {return databaseTownLamp;}
	bool getFlagTownLabels() const {return flagTownLabels;}
	//! The towns of the location database used now (empty when off), the largest contribution first
	const std::vector<DatabaseTown>& getDatabaseTowns() const {return databaseTowns;}
	//! How many places of the database were left out as parts of larger towns
	int getDatabaseDistricts() const {return databaseDistricts;}
	//! The largest distance of a town (km)
	static double maxDistance();

public slots:
	void setFlagEnabled(bool b);
	void setSources(const QString& s);
	void setLumensPerInhabitant(double lm);
	void setFlagDatabaseTowns(bool b);
	void setDatabaseTownLamp(int lamp);
	void setFlagTownLabels(bool b);
	//! Find the towns of the location database again and compute the domes anew
	void recompute();

signals:
	void flagEnabledChanged(bool b);
	void sourcesChanged(const QString& s);
	void lumensPerInhabitantChanged(double lm);
	void mapChanged();
	void flagDatabaseTownsChanged(bool b);
	void databaseTownLampChanged(int lamp);
	void flagTownLabelsChanged(bool b);
	//! The towns of the location database changed (another location, or switched on or off)
	void databaseTownsChanged();

private:
	struct Kernel;
	struct Result;
	//! The atmosphere the kernel was (or is being) computed for
	struct AtmosphereKey
	{
		double aerosolDepth = -1.; //!< vertical aerosol optical depth at sea level, 550 nm
		double altitude = -1.;     //!< of the observer, km
		bool operator==(const AtmosphereKey& o) const {return aerosolDepth == o.aerosolDepth && altitude == o.altitude;}
	};
	AtmosphereKey currentAtmosphere() const;
	void startComputation();
	//! Find the towns of the location database around the current location
	void updateDatabaseTowns();
	static std::shared_ptr<Kernel> computeKernel(const AtmosphereKey& key);
	//! names: of the sources, for the labels (empty: no label)
	static std::shared_ptr<Result> computeMap(std::shared_ptr<Kernel> kernel, const std::vector<Source>& sources, const std::vector<QString>& names, double lumensPerInhabitant);

	bool flagEnabled = false;
	std::vector<Source> sources;
	double lumensPerInhabitant = 1000.;
	bool flagDatabaseTowns = false;
	int databaseTownLamp = LampMixed;
	bool flagTownLabels = false;
	std::vector<DatabaseTown> databaseTowns;
	int databaseDistricts = 0;
	//! The location the database towns were found for
	QString databasePlanet;
	double databaseLatitude = 1000., databaseLongitude = 1000.;

	std::shared_ptr<Kernel> kernel;
	std::shared_ptr<Result> result;
	QFuture<std::pair<std::shared_ptr<Kernel>, std::shared_ptr<Result>>> job;
	bool jobRunning = false;
	bool dirty = true;          //!< inputs changed: compute again
	AtmosphereKey jobAtmosphere;
	GLuint texture = 0;
	bool textureStale = true;
};

#endif // LIGHTDOMES_HPP
