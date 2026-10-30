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

#ifndef STELREFRACTIONMODEL_HPP
#define STELREFRACTIONMODEL_HPP

#include <vector>

//! Astronomical refraction and the air along the line of sight for an observer at any altitude
//! (sea level to the stratosphere), from rays traced through a spherically stratified atmosphere.
//!
//! The atmosphere is the U.S. Standard Atmosphere 1976, shifted to the temperature of the observer
//! in the troposphere, with the pressure integrated hydrostatically from the pressure at the observer.
//! The refractivity of dry air follows Edlén (1966). A ray is traced from the observer towards each
//! apparent altitude, from the true horizon (the ray that grazes the sea-level surface of the Earth)
//! to the zenith, through the whole atmosphere: a ray below the astronomical horizon first goes down
//! into denser air and up again, so from altitude the refraction just above the true horizon is
//! larger than at sea level, while the refraction at 0 degrees is smaller.
//!
//! Along each ray the columns of three components are integrated: the air (Rayleigh scattering), the
//! aerosols and the ozone (a layer between 10 and 40 km, densest at 25 km). The aerosols fill the boundary
//! layer, well mixed up to its top (the inversion that caps it, from about 0.3 km on a calm night to 2.5 km on
//! a summer afternoon); above it the free troposphere holds a thin rest (7% of the density, scale height 2 km).
//! The humidity of the boundary layer makes its aerosols swell (aerosolLayerGrowth). The columns are given in
//! units of the vertical column above sea level (aerosols: at the normal humidity), so they can be multiplied
//! with sea-level zenith optical depths.
//!
//! An object is immutable: build a new one for new conditions. Building takes a few milliseconds.
//! All angles are in radians, altitudes above the astronomical horizon of the observer.
class StelRefractionModel
{
public:
	struct Conditions
	{
		double altitude = 0.;        //!< of the observer above sea level, m
		double pressure = 1013.25;   //!< at the observer, hPa (mbar)
		double temperature = 15.;    //!< at the observer, degrees Celsius
		double wavelength = 0.55;    //!< micrometers (0.55: the visual band)
		double aerosolLayerTop = 1500.; //!< top of the boundary layer that holds the aerosols, m above sea level
		double aerosolLayerGrowth = 1.; //!< extinction of its aerosols against the normal humidity (hygroscopic growth)
		bool operator==(const Conditions& o) const
		{
			return altitude == o.altitude && pressure == o.pressure && temperature == o.temperature && wavelength == o.wavelength
			       && aerosolLayerTop == o.aerosolLayerTop && aerosolLayerGrowth == o.aerosolLayerGrowth;
		}
		bool operator!=(const Conditions& o) const {return !(*this == o);}
	};

	//! Columns of the components along a line of sight, each in units of its vertical column above sea level
	struct Columns
	{
		double rayleigh = 0.;
		double aerosol = 0.;
		double ozone = 0.;
	};

	explicit StelRefractionModel(const Conditions& conditions);

	const Conditions& getConditions() const {return conditions;}

	//! Apparent altitude of the true horizon: the ray that just grazes the sea-level surface (0 for an observer at or below sea level)
	double getHorizonApparent() const {return horizonApparent;}
	//! Geometric altitude of the direction the grazing ray comes from
	double getHorizonGeometric() const {return horizonGeometric;}
	//! Refraction at the true horizon
	double getHorizonRefraction() const {return horizonApparent - horizonGeometric;}
	//! Geometric altitude of the depression of the sea-level surface (without refraction): -acos(R/(R+h))
	double getGeometricDip() const;

	//! Geometric altitude of the object seen at the given apparent altitude.
	//! Below the true horizon the refraction at the horizon is kept (such a direction is behind the Earth).
	double toGeometric(double apparentAltitude) const;
	//! Apparent altitude of an object at the given geometric altitude (the inverse of toGeometric())
	double toApparent(double geometricAltitude) const;

	//! Columns along the line of sight towards the given apparent altitude, to the top of the atmosphere
	//! (below the true horizon: those at the horizon)
	Columns columns(double apparentAltitude) const;
	//! Columns above the observer, straight up
	const Columns& getZenithColumns() const {return zenith;}
	//! The same at the normal humidity (no growth of the aerosols)
	const Columns& getZenithColumnsNormal() const {return zenithNormal;}
	//! Density of the aerosols at the given altitude (m) at the normal humidity, per m, so that the vertical column
	//! from sea level is 1
	double aerosolDensityNormal(double altitude) const;
	//! The same with the growth of the humidity in the boundary layer
	double aerosolDensity(double altitude) const;

	//! Refractivity n-1 of the air at the observer
	double getRefractivityAtObserver() const {return refractivityObserver;}
	//! Pressure (hPa) and temperature (degrees Celsius) of the model atmosphere at the given altitude (m)
	double pressureAt(double altitude) const;
	double temperatureAt(double altitude) const;

	//! The traced samples, in order of increasing apparent (and geometric) altitude; the first one is the true horizon
	const std::vector<double>& getApparentSamples() const {return apparent;}
	const std::vector<double>& getGeometricSamples() const {return geometric;}

	//! Fraction of the boundary layer with the given top (m) at the altitude h (m): 1 inside, 0 above, a smooth top
	static double boundaryLayerFraction(double h, double top);
	//! Density of the aerosols at the altitude h (m) at the normal humidity, against the density in the boundary layer
	//! with the given top (m): 1 in the layer, the rest of the free troposphere above
	static double aerosolShape(double h, double top);
	//! Pressure (hPa) of the U.S. Standard Atmosphere 1976 at the given altitude (m)
	static double standardPressure(double altitude);
	//! Temperature (degrees Celsius) of the U.S. Standard Atmosphere 1976 at the given altitude (m)
	static double standardTemperature(double altitude);
	//! Refractivity coefficient c (1/hPa*K) of dry air at the given wavelength (micrometers): n-1 = c*P/T
	static double refractivityCoefficient(double wavelength);

	static constexpr double EarthRadius = 6371000.;  // m
	static constexpr double AtmosphereTop = 100000.; // m

private:
	Conditions conditions;
	// The atmosphere on a regular grid from the ground (sea level, or the observer if lower) to the top
	double gridBase = 0., gridStep = 10.;
	std::vector<double> gridLnRefractivity, gridTemperature, gridPressure, gridDensity;

	double refractivityObserver = 0.;
	double horizonApparent = 0., horizonGeometric = 0.;
	std::vector<double> apparent, geometric;
	std::vector<Columns> lineColumns;
	Columns zenith, zenithNormal;
	double aerosolNorm = 1.;
	double boundaryLayerFraction(double h) const {return boundaryLayerFraction(h, conditions.aerosolLayerTop);}
	double aerosolShape(double h) const {return aerosolShape(h, conditions.aerosolLayerTop);}

	void buildAtmosphere();
	void traceRays();
	// Refractivity and its derivative along the radius at the height h above sea level
	void refractivity(double h, double& nm1, double& dnm1) const;
	Columns density(double h) const;
	void trace(double apparentAltitude, double& geometricAltitude, Columns& cols, bool& hitGround) const;
};

#endif // STELREFRACTIONMODEL_HPP
