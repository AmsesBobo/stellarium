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
#include "StelProjectorType.hpp"
#include "VecMath.hpp"

#include <memory>
#include <vector>

class StelCore;
class QOpenGLShaderProgram;
class QOpenGLBuffer;
class QOpenGLVertexArrayObject;

//! @class AuroraMgr
//! The aurorae (northern and southern lights) of the Earth.
//!
//! The aurorae shine in two ovals around the geomagnetic poles, 100 to 400 km above the ground. The model of the
//! ovals is the one of Starkov (1994): the poleward and equatorward boundaries of the discrete aurora and the
//! equatorward boundary of the diffuse aurora, in geomagnetic latitude, as Fourier series of the magnetic local time
//! whose coefficients follow the auroral electrojet index AL; AL comes from the planetary index Kp (Starkov 1994).
//! The geomagnetic coordinates are those of the dipole of the IGRF (the pole interpolated in time, 1900 to 2030).
//!
//! The light is emitted in a shell 90 to 500 km above a spherical Earth: the green line of oxygen (557.7 nm) with
//! the blue band of nitrogen ions (427.8 nm) around 110 km, the red lines of oxygen (630.0 and 636.4 nm) higher up.
//! Calm arcs (thin bands along the magnetic latitude in the discrete oval) and the diffuse aurora (equatorward of
//! them) follow the dipole field lines upwards. A shader follows the line of sight of each pixel through the shell;
//! the brightness in rayleigh becomes luminance (cd/m^2) and is recorded by the photographic mode like the sky,
//! dimmed by the extinction. From far away only the tops, red, stand over the horizon; from high latitudes the
//! arcs span the sky.
//!
//! The arcs fold and break up into rays along the field lines (a corona seen from below them) in the substorm
//! cycle (see substorm()); afterwards the diffuse aurora pulsates in patches. Their noises come from the date and
//! time, so that a moment always looks the same. Energetic aurorae get a purple lower border (the first positive
//! band of N2 and the blue N2+ band, below 100 km where the green line is quenched). The camera averages what
//! changes during its exposure: the substorm, the folds, the rays and the pulsations.
//!
//! The ovals can also be drawn as lines in the sky, at the heights of the green (115 km) and red (300 km)
//! emissions: a guide to their position.
class AuroraMgr : public StelModule
{
	Q_OBJECT
	//! Show the light of the aurorae (photographic mode)
	Q_PROPERTY(bool flagShow READ getFlagShow WRITE setFlagShow NOTIFY flagShowChanged)
	//! Draw the auroral ovals as lines (a guide to their position)
	Q_PROPERTY(bool flagShowOval READ getFlagShowOval WRITE setFlagShowOval NOTIFY flagShowOvalChanged)
	//! The geomagnetic activity: 0 quiet (Kp 1), 1 moderate (Kp 3), 2 storm (Kp 6), 3 extreme storm (Kp 9), 4 custom (kpCustom)
	Q_PROPERTY(int kpPreset READ getKpPreset WRITE setKpPreset NOTIFY kpPresetChanged)
	//! The planetary geomagnetic activity index Kp of the custom activity (0 quiet to 9 extreme storm)
	Q_PROPERTY(double kpCustom READ getKpCustom WRITE setKpCustom NOTIFY kpCustomChanged)
	//! The planetary geomagnetic activity index Kp in use (setting it makes the activity custom)
	Q_PROPERTY(double kp READ getKp WRITE setKp NOTIFY kpChanged)
	//! Where the substorm cycle stands now and when the next onset comes (for the user interface)
	Q_PROPERTY(QString substormInfo READ getSubstormInfo NOTIFY substormInfoChanged)

public:
	AuroraMgr();
	~AuroraMgr() override;

	void init() override;
	void update(double deltaTime) override;
	void draw(StelCore* core) override;
	double getCallOrder(StelModuleActionName actionName) const override;

	bool getFlagShow() const {return flagShow;}
	bool getFlagShowOval() const {return flagShowOval;}
	int getKpPreset() const {return kpPreset;}
	double getKpCustom() const {return kpCustom;}
	double getKp() const;
	QString getSubstormInfo() const {return substormInfo;}
	//! The Kp of the presets of the activity (kpPreset)
	static constexpr double KpPresets[4] = {1., 3., 6., 9.};

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

	//! The brightness of the aurorae for the index kp: the green column (rayleigh, 557.7 nm, looking straight up
	//! through it) of the brightest calm arc at magnetic midnight and of the diffuse aurora; the red emission
	//! (630.0 nm) against the green and the height of its peak (km)
	struct Emission
	{
		double arcGreen = 0., diffuseGreen = 0., redRatio = 0., redHeight = 0.;
	};
	static Emission emission(double kp);

	//! The substorm cycle at the Julian day jd (UT) for the index kp. A substorm comes every 1.7 to 3.5 hours (more
	//! often in storms): in the growth phase the arcs drift towards the equator; at the onset, near magnetic midnight,
	//! they brighten suddenly, fold and break up into rays, the oval bulges poleward and the activity spreads in
	//! magnetic local time (the expansion, 12 to 24 minutes); in the recovery it fades and the diffuse aurora pulsates.
	//! Each cycle is drawn from its time: the same moment always looks the same.
	struct Substorm
	{
		int phase = 0;           //!< 0 growth, 1 expansion, 2 recovery
		double onsetMLT = 23.;   //!< magnetic local time of the onset (h)
		double spread = 1.;      //!< extent of the activity in magnetic local time (h)
		double activity = 0.;    //!< 0 quiet to 1 the brightest
		double bulge = 0.;       //!< poleward bulge of the oval (degrees)
		double drift = 0.;       //!< equatorward drift of the arcs (degrees)
		double pulsating = 0.;   //!< pulsating aurora (0 to 1)
		double onset = 0.;       //!< Julian day of the onset of this cycle
		double nextOnset = 0.;   //!< Julian day of the onset of the next cycle
	};
	static Substorm substorm(double jd, double kp);

public slots:
	void setFlagShow(bool b);
	void setFlagShowOval(bool b);
	void setKpPreset(int preset);
	void setKpCustom(double value);
	void setKp(double value);

signals:
	void flagShowChanged(bool b);
	void flagShowOvalChanged(bool b);
	void kpPresetChanged(int preset);
	void kpCustomChanged(double value);
	void kpChanged(double value);
	void substormInfoChanged(const QString& info);

private:
	bool flagShow = false;
	bool flagShowOval = false;
	int kpPreset = 1;
	double kpCustom = 3.;
	QString substormInfo;

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
	//! The observer and the Sun in the Earth-fixed frame
	struct Geometry
	{
		Vec3d up, east, north;  //!< local axes of the observer
		Vec3d observer;         //!< position of the observer (km, over a spherical Earth)
		MagneticFrame frame;
		double sunMagLon = 0.;  //!< geomagnetic longitude of the Sun (radians): magnetic noon below it
		double sunAltitude = 0.; //!< geometric altitude of the Sun (radians)
	};
	Geometry computeGeometry(const StelCore* core) const;

	//! A line of the ovals in the AltAz frame of the observer: the points of one boundary of one hemisphere at one
	//! height, with whether each one is in sight (not behind the Earth)
	struct OvalLine
	{
		std::vector<Vec3d> points;
		std::vector<bool> visible;
		Vec3f color;
	};
	std::vector<OvalLine> computeOvalLines(const Geometry& g) const;
	void drawOval(StelCore* core, const Geometry& g) const;

	//! The light of the aurorae (photographic mode), in a full screen pass that follows the line of sight of each pixel
	void drawLight(StelCore* core, const Geometry& g);
	void buildLightProgram(const StelProjector& projector);
	std::unique_ptr<QOpenGLShaderProgram> lightProgram;
	StelProjectorP lightProjector;  //!< the projection the program was built for
	std::unique_ptr<QOpenGLBuffer> vbo;
	std::unique_ptr<QOpenGLVertexArrayObject> vao;
};

#endif // AURORAMGR_HPP
