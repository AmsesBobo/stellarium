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

#ifndef AURORAMGR_HPP
#define AURORAMGR_HPP

#include "StelModule.hpp"
#include "VecMath.hpp"

#include <vector>

class StelCore;

//! @class AuroraMgr
//! The aurorae (northern and southern lights) of the Earth.
//!
//! The aurorae shine in two ovals around the geomagnetic poles, 100 to 400 km above the ground. The model of the
//! ovals is the one of Starkov (1994): the poleward and equatorward boundaries of the discrete aurora and the
//! equatorward boundary of the diffuse aurora, in geomagnetic latitude, as Fourier series of the magnetic local time
//! whose coefficients follow the auroral electrojet index AL; AL comes from the planetary index Kp (Starkov 1994).
//! The geomagnetic coordinates are those of the dipole of the IGRF (the pole interpolated in time, 1900 to 2030).
//!
//! This first step draws the ovals as lines in the sky at the heights of the green (115 km) and red (300 km)
//! emissions, seen from the observer over a spherical Earth: from far away they stand low over the horizon or are
//! hidden behind the Earth, from high latitudes they arch overhead.
class AuroraMgr : public StelModule
{
	Q_OBJECT
	//! Draw the auroral ovals as lines (a guide to their position)
	Q_PROPERTY(bool flagShowOval READ getFlagShowOval WRITE setFlagShowOval NOTIFY flagShowOvalChanged)
	//! The planetary geomagnetic activity index Kp (0 quiet to 9 extreme storm)
	Q_PROPERTY(double kp READ getKp WRITE setKp NOTIFY kpChanged)

public:
	AuroraMgr();
	~AuroraMgr() override;

	void init() override;
	void update(double deltaTime) override;
	void draw(StelCore* core) override;
	double getCallOrder(StelModuleActionName actionName) const override;

	bool getFlagShowOval() const {return flagShowOval;}
	double getKp() const {return kp;}

	//! The boundaries of the auroral oval at the magnetic local time mlt (hours, 0 at magnetic midnight) for the
	//! index kp: geomagnetic latitudes (degrees) of the poleward and equatorward boundaries of the discrete aurora and
	//! of the equatorward boundary of the diffuse aurora (Starkov 1994)
	struct OvalBoundaries
	{
		double poleward = 0., equatorward = 0., diffuse = 0.;
	};
	static OvalBoundaries ovalBoundaries(double kp, double mlt);
	//! The geomagnetic (dipole) north pole at the given Julian day: latitude and longitude (degrees, east positive)
	static void dipolePole(double jd, double& latitude, double& longitude);

public slots:
	void setFlagShowOval(bool b);
	void setKp(double value);

signals:
	void flagShowOvalChanged(bool b);
	void kpChanged(double value);

private:
	bool flagShowOval = false;
	double kp = 3.;

	//! The geomagnetic frame (axes in Earth-fixed geographic coordinates) at the given Julian day
	struct MagneticFrame
	{
		Vec3d x, y, z;
		//! Earth-fixed unit vector of a point at the geomagnetic latitude and longitude (radians)
		Vec3d toGeographic(double lat, double lon) const
		{
			return x*(std::cos(lat)*std::cos(lon)) + y*(std::cos(lat)*std::sin(lon)) + z*std::sin(lat);
		}
	};
	static MagneticFrame magneticFrame(double jd);
	//! A line of the ovals in the AltAz frame of the observer: the points of one boundary of one hemisphere at one
	//! height, with whether each one is in sight (not behind the Earth)
	struct OvalLine
	{
		std::vector<Vec3d> points;
		std::vector<bool> visible;
		Vec3f color;
	};
	std::vector<OvalLine> computeOvalLines(const StelCore* core) const;
};

#endif // AURORAMGR_HPP
