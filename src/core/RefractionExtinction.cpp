/*
 * Stellarium
 * Copyright (C) 2010 Fabien Chereau
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
 *
 * Refraction and extinction computations.
 * Principal implementation: 2010-03-23 GZ=Georg Zotti, Georg.Zotti@univie.ac.at
 */

#include "StelApp.hpp"
#include "StelUtils.hpp"
#include "RefractionExtinction.hpp"
#include "StelUtils.hpp"

#include <QOpenGLShaderProgram>
#include <QElapsedTimer>
#include <QVector3D>
#include <QVector4D>
#include <algorithm>
#include <QDebug>

Extinction::Extinction() : ext_coeff(50), undergroundExtinctionMode(UndergroundExtinctionZero)
{
}

namespace
{
	// The sRGB channels at their effective wavelengths (micrometers) and the weights of the luminance
	constexpr double CHANNEL_WAVELENGTH[3] = {0.610, 0.550, 0.465};
	constexpr double CHANNEL_LUMINANCE[3] = {0.2126, 0.7152, 0.0722};
	// Zenith optical depth of the ozone layer (300 Dobson units) in the Chappuis band, with absorption cross sections
	// of about 4.8e-21, 3.3e-21 and 0.65e-21 cm^2 at the three wavelengths
	constexpr double OZONE_ZENITH_DEPTH[3] = {0.0387, 0.0266, 0.0052};
	constexpr int EXTINCTION_TABLE_SIZE = 96;

	// Zenith optical depth of the air at sea level (1013.25 hPa) for Rayleigh scattering (Bodhaine et al. 1999)
	double rayleighZenithDepth(double wavelength)
	{
		const double l2 = 1./(wavelength*wavelength);
		return 0.008569*l2*l2*(1. + 0.0113*l2 + 0.00013*l2*l2);
	}
}

double Extinction::columnMagnitude(const StelRefractionModel::Columns& c) const
{
	const Vec3d t = columnTransmission(c);
	const double y = CHANNEL_LUMINANCE[0]*t[0] + CHANNEL_LUMINANCE[1]*t[1] + CHANNEL_LUMINANCE[2]*t[2];
	return -2.5*std::log10(std::max(y, 1e-30));
}

Vec3d Extinction::columnTransmission(const StelRefractionModel::Columns& c) const
{
	Vec3d t;
	for (int i = 0; i < 3; ++i)
		t[i] = std::exp(-(depthRayleigh[i]*c.rayleigh + depthOzone[i]*c.ozone + depthAerosol[i]*c.aerosol));
	return t;
}

void Extinction::setModel(std::shared_ptr<const StelRefractionModel> m, bool atSeaLevel)
{
	model = std::move(m);
	coefficientAtSeaLevel = atSeaLevel;
	if (!model)
		table.clear();
}

void Extinction::updateModel()
{
	if (!model)
	{
		table.clear();
		builtModel.reset();
		return;
	}
	if (model == builtModel && ext_coeff == builtCoefficient && angstrom == builtAngstrom
	    && coefficientAtSeaLevel == builtAtSeaLevel && !table.empty())
		return;
	builtModel = model;
	builtCoefficient = ext_coeff;
	builtAngstrom = angstrom;
	builtAtSeaLevel = coefficientAtSeaLevel;

	// Split k (the luminance extinction towards the zenith, at sea level or at the observer) into the air, the
	// ozone and the aerosols. If k is smaller than air and ozone alone, both are scaled down to it.
	StelRefractionModel::Columns zenith;
	if (coefficientAtSeaLevel)
		zenith.rayleigh = zenith.aerosol = zenith.ozone = 1.;
	else
		zenith = model->getZenithColumns();
	const double k = static_cast<double>(std::max(ext_coeff, 0.f));
	auto setDepths = [this](double scale, double aerosol) {
		for (int i = 0; i < 3; ++i)
		{
			depthRayleigh[i] = scale*rayleighZenithDepth(CHANNEL_WAVELENGTH[i]);
			depthOzone[i] = scale*OZONE_ZENITH_DEPTH[i];
			depthAerosol[i] = aerosol*std::pow(CHANNEL_WAVELENGTH[i]/0.55, -static_cast<double>(angstrom));
		}
	};
	setDepths(1., 0.);
	if (columnMagnitude(zenith) >= k)
	{
		double lo = 0., hi = 1.;
		for (int it = 0; it < 50; ++it)
		{
			const double mid = 0.5*(lo + hi);
			setDepths(mid, 0.);
			(columnMagnitude(zenith) > k ? hi : lo) = mid;
		}
		setDepths(0.5*(lo + hi), 0.);
	}
	else
	{
		// the aerosol column above sea level is 1 in these units; above an observer it is less
		double lo = 0., hi = 50.;
		for (int it = 0; it < 60; ++it)
		{
			const double mid = 0.5*(lo + hi);
			setDepths(1., mid);
			(columnMagnitude(zenith) > k ? hi : lo) = mid;
		}
		setDepths(1., 0.5*(lo + hi));
	}
	zenithExtinction = static_cast<float>(columnMagnitude(model->getZenithColumns()));

	// The extinction along the rays, against the geometric altitude (densest near the true horizon)
	table.resize(EXTINCTION_TABLE_SIZE);
	tableLimb = static_cast<float>(model->getHorizonGeometric());
	tableSpan = static_cast<float>(M_PI_2 - model->getHorizonGeometric());
	for (int i = 0; i < EXTINCTION_TABLE_SIZE; ++i)
	{
		const double u = static_cast<double>(i)/(EXTINCTION_TABLE_SIZE - 1);
		const double g = model->getHorizonGeometric() + (M_PI_2 - model->getHorizonGeometric())*u*u*u;
		const double a = i == 0 ? model->getHorizonApparent() : model->toApparent(g);
		table[static_cast<size_t>(i)] = static_cast<float>(columnMagnitude(model->columns(a)));
	}
	++modelVersion;
}

float Extinction::magnitude(float sinGeo) const
{
	if (!isModelInUse())
		return airmass(sinGeo, false) * ext_coeff;
	float x = std::asin(std::clamp(sinGeo, -1.f, 1.f)) - tableLimb;
	if (x < 0.f)
	{
		// behind the Earth: hidden by the ground, which may not be drawn
		switch (undergroundExtinctionMode)
		{
			case UndergroundExtinctionZero:
				return 0.f;
			case UndergroundExtinctionMax:
				return std::max(42.f*ext_coeff, table.front());
			case UndergroundExtinctionMirror:
				x = -x;
		}
	}
	const float f = std::cbrt(std::min(x/tableSpan, 1.f))*static_cast<float>(EXTINCTION_TABLE_SIZE - 1);
	const int i = std::min(static_cast<int>(f), EXTINCTION_TABLE_SIZE - 2);
	const float t = f - static_cast<float>(i);
	return table[static_cast<size_t>(i)] + t*(table[static_cast<size_t>(i+1)] - table[static_cast<size_t>(i)]);
}

float Extinction::getZenithExtinction() const
{
	return isModelInUse() ? zenithExtinction : ext_coeff;
}

Vec3d Extinction::transmission(double geometricAltitude) const
{
	if (!isModelInUse())
		return Vec3d(1.);
	return columnTransmission(model->columns(std::max(model->toApparent(geometricAltitude), model->getHorizonApparent())));
}

float Extinction::relativeAirmass(double apparentAltitude) const
{
	if (!isModelInUse() || apparentAltitude < model->getHorizonApparent())
		return -1.f;
	const double zenith = model->getZenithColumns().rayleigh;
	return zenith > 0. ? static_cast<float>(model->columns(apparentAltitude).rayleigh/zenith) : -1.f;
}

double Extinction::getHorizonSineGeometric() const
{
	return isModelInUse() ? std::sin(static_cast<double>(tableLimb)) : -0.035;
}

// airmass computation for cosine of zenith angle z
float Extinction::airmass(float cosZ, const bool apparent_z) const
{
	if (cosZ<-0.035f) // about -2 degrees. Here, RozenbergZ>574 and climbs fast!
	{
		switch (undergroundExtinctionMode)
		{
			case UndergroundExtinctionZero:
				return 0.f;
			case UndergroundExtinctionMax:
				return 42.f;
			case UndergroundExtinctionMirror:
				cosZ = std::min(1.f, -0.035f - (cosZ+0.035f));
		}
	}

	if (apparent_z)
	{
		// Rozenberg 1966, reported by Schaefer (1993-2000).
		return 1.0f/(cosZ+0.025f*std::exp(-11.f*cosZ));
	}
	else
	{
		//Young 1994
		const float nom=(1.002432f*cosZ+0.148386f)*cosZ+0.0096467f;
		const float denum=((cosZ+0.149864f)*cosZ+0.0102963f)*cosZ+0.000303978f;
		return nom/denum;
	}
}

QByteArray Extinction::getForwardTransformShader() const
{
	return QByteArray(1+R"(
uniform int EXTINCTION_undergroundExtinctionMode;
float EXTINCTION_airmass(float cosZ, bool apparent_z)
{
	if(cosZ<-0.035) // about -2 degrees. Here, RozenbergZ>574 and climbs fast!
	{
		if(EXTINCTION_undergroundExtinctionMode == UndergroundExtinctionZero)
			return 0.;
		else if(EXTINCTION_undergroundExtinctionMode == UndergroundExtinctionMax)
			return 42.;
		else if(EXTINCTION_undergroundExtinctionMode == UndergroundExtinctionMirror)
			cosZ = min(1., -0.035 - (cosZ+0.035));
	}

	if(apparent_z)
	{
		// Rozenberg 1966, reported by Schaefer (1993-2000).
		return 1.0/(cosZ+0.025*exp(-11.*cosZ));
	}
	else
	{
		//Young 1994
		float nom=(1.002432*cosZ+0.148386)*cosZ+0.0096467;
		float denum=((cosZ+0.149864)*cosZ+0.0102963)*cosZ+0.000303978;
		return nom/denum;
	}
}

uniform float EXTINCTION_ext_coeff;
// The extinction along the rays of the ray-traced refraction model (see Extinction::updateModel())
uniform bool EXTINCTION_useTable;
uniform highp vec4 EXTINCTION_table[@VECTORS@];
uniform highp vec3 EXTINCTION_tableLimits; // geometric altitude of the true horizon, span to the zenith (radians), maximum
highp float EXTINCTION_tableEntry(int i)
{
	int v = i/4;
	int c = i - 4*v;
	highp vec4 e = EXTINCTION_table[v];
	return c == 0 ? e.x : (c == 1 ? e.y : (c == 2 ? e.z : e.w));
}
float extinctionMagnitude(vec3 altAzPos)
{
	if (EXTINCTION_useTable)
	{
		highp float x = asin(clamp(altAzPos[2], -1., 1.)) - EXTINCTION_tableLimits.x;
		if (x < 0.)
		{
			if (EXTINCTION_undergroundExtinctionMode == UndergroundExtinctionZero)
				return 0.;
			else if (EXTINCTION_undergroundExtinctionMode == UndergroundExtinctionMax)
				return EXTINCTION_tableLimits.z;
			x = -x;
		}
		highp float f = pow(clamp(x/EXTINCTION_tableLimits.y, 0., 1.), 1./3.)*float(@SIZE@ - 1);
		int i = int(min(floor(f), float(@SIZE@ - 2)));
		return mix(EXTINCTION_tableEntry(i), EXTINCTION_tableEntry(i + 1), f - float(i));
	}
	return EXTINCTION_airmass(altAzPos[2], false) * EXTINCTION_ext_coeff;
}
)").replace("UndergroundExtinctionZero", std::to_string(int(UndergroundExtinctionZero)).c_str())
   .replace("UndergroundExtinctionMax", std::to_string(int(UndergroundExtinctionMax)).c_str())
   .replace("UndergroundExtinctionMirror", std::to_string(int(UndergroundExtinctionMirror)).c_str())
   .replace("@VECTORS@", std::to_string(EXTINCTION_TABLE_SIZE/4).c_str())
   .replace("@SIZE@", std::to_string(EXTINCTION_TABLE_SIZE).c_str());
}

void Extinction::setForwardTransformUniforms(QOpenGLShaderProgram& program) const
{
	program.setUniformValue("EXTINCTION_undergroundExtinctionMode", int(undergroundExtinctionMode));
	program.setUniformValue("EXTINCTION_ext_coeff", GLfloat(ext_coeff));
	const bool use = isModelInUse();
	program.setUniformValue("EXTINCTION_useTable", use);
	if (!use)
		return;
	QVector4D vectors[EXTINCTION_TABLE_SIZE/4];
	for (int i = 0; i < EXTINCTION_TABLE_SIZE/4; ++i)
		vectors[i] = QVector4D(table[static_cast<size_t>(4*i)], table[static_cast<size_t>(4*i+1)],
		                       table[static_cast<size_t>(4*i+2)], table[static_cast<size_t>(4*i+3)]);
	program.setUniformValueArray("EXTINCTION_table", vectors, EXTINCTION_TABLE_SIZE/4);
	program.setUniformValue("EXTINCTION_tableLimits", QVector3D(tableLimb, tableSpan, std::max(42.f*ext_coeff, table.front())));
}

/* ***************************************************************************************************** */

// The following 4 are to be configured, the rest is derived.
// Recommendations: -4.9/-4.3/0.1/0.1: sharp but continuous transition, no effects below -5.
//                  -4.3/-4.3/0.7/0.7: sharp but continuous transition, no effects below -5. Maybe better for picking?
//                  -3/-3/2/2: "strange" zone 2 degrees wide. Both formulae are close near -3. Actually, refraction formulae dubious below 0
//                   0/0/1/1: "strange zone 1 degree wide just below horizon, no effect below -1. Actually, refraction formulae dubious below 0! But it looks stupid, see the sun!
//                  Optimum:-3.54/-3.21783/1.46/1.78217. Here forward/backward are almost perfect inverses (-->good picking!), and nothing happens below -5 degrees.
// This must be -5 or higher.
static const float MIN_GEO_ALTITUDE_DEG=-3.54f;
// this must be -4.3 or higher. -3.21783 is an optimal value to fit against forward refraction!
static const float MIN_APP_ALTITUDE_DEG=-3.21783f;
// this must be positive. Transition zone goes that far below the values just specified.
static const float TRANSITION_WIDTH_GEO_DEG=1.46f;
static const float TRANSITION_WIDTH_APP_DEG=1.78217f;

Refraction::Refraction() : pressure(1013.f), temperature(10.f),
	preTransfoMat(Mat4d::identity()), invertPreTransfoMat(Mat4d::identity()), preTransfoMatf(Mat4f::identity()), invertPreTransfoMatf(Mat4f::identity()),
	postTransfoMat(Mat4d::identity()), invertPostTransfoMat(Mat4d::identity()), postTransfoMatf(Mat4f::identity()), invertPostTransfoMatf(Mat4f::identity())
{
	updatePrecomputed();
}

void Refraction::setPreTransfoMat(const Mat4d& m)
{
	preTransfoMat=m;
	invertPreTransfoMat=m.inverse();
	preTransfoMatf=toMat4f(m);
	invertPreTransfoMatf=toMat4f(invertPreTransfoMat);
}

void Refraction::setPostTransfoMat(const Mat4d& m)
{
	postTransfoMat=m;
	invertPostTransfoMat=m.inverse();
	postTransfoMatf=toMat4f(m);
	invertPostTransfoMatf=toMat4f(invertPostTransfoMat);
}

void Refraction::updatePrecomputed()
{
	press_temp_corr=pressure/1010.f * 283.f/(273.f+temperature) / 60.f;
}

void Refraction::innerRefractionForward(Vec3d& altAzPos) const
{
	const double length = altAzPos.norm();	
	if (qFuzzyCompare(length, 0.0) || qIsNaN(length))
	{
		// Under some circumstances there are zero coordinates. Just leave them alone.
		//qDebug() << "Refraction::innerRefractionForward(): Zero vector detected - Continue with zero vector.";
		return;
	}

	// NOTE: the calculations here *must* be in double, otherwise we'll get wobble of "small" objects like Callisto or Thebe
	Q_ASSERT(length>0.0);
	const double sinGeo = altAzPos[2]/length;
	Q_ASSERT(fabs(sinGeo)<=1.0);
	double geom_alt_rad = std::asin(sinGeo);
	if (isModelInUse())
	{
		// ray-traced model (see StelRefractionModel), resampled in the table
		const double r = static_cast<double>(table->forwardRefraction(static_cast<float>(geom_alt_rad)));
		if (r == 0.)
			return;
		const double refr_alt_rad = std::min(geom_alt_rad + r, M_PI_2);
		const double sinRef = std::sin(refr_alt_rad);
		const double shortenxy = ((fabs(sinGeo)>=1.0) ? 1.0 : std::sqrt((1.-sinRef*sinRef)/(1.-sinGeo*sinGeo)));
		altAzPos[0]*=shortenxy;
		altAzPos[1]*=shortenxy;
		altAzPos[2]=sinRef*length;
		return;
	}
	double geom_alt_deg = M_180_PI*geom_alt_rad;
	if (geom_alt_deg > MIN_GEO_ALTITUDE_DEG)
	{
		// refraction from Saemundsson, S&T1986 p70 / in Meeus, Astr.Alg.
		double r=press_temp_corr * ( 1.02 / std::tan((geom_alt_deg+10.3/(geom_alt_deg+5.11))*M_PI_180) + 0.0019279);
		geom_alt_deg += r;
		if (geom_alt_deg > 90.)
			geom_alt_deg=90.;
	}
	else if(geom_alt_deg>MIN_GEO_ALTITUDE_DEG-TRANSITION_WIDTH_GEO_DEG)
	{
		// Avoids the jump below -5 by interpolating linearly between MIN_GEO_ALTITUDE_DEG and bottom of transition zone
		double r_m5=press_temp_corr * ( 1.02 / std::tan((MIN_GEO_ALTITUDE_DEG+10.3/(MIN_GEO_ALTITUDE_DEG+5.11))*M_PI_180) + 0.0019279);
		geom_alt_deg += r_m5*(geom_alt_deg-(MIN_GEO_ALTITUDE_DEG-TRANSITION_WIDTH_GEO_DEG))/TRANSITION_WIDTH_GEO_DEG;
	}
	else return;
	// At this point we have corrected geometric altitude. Note that if we just change altAzPos[2], we would change vector length, so this would change our angles.
	// We have to shorten X,Y components of the vector as well by the change in cosines of altitude, or (sqrt(1-sin(alt))

	const double refr_alt_rad=geom_alt_deg*M_PI_180;
	const double sinRef=std::sin(refr_alt_rad);

	const double shortenxy=((fabs(sinGeo)>=1.0) ? 1.0 : std::sqrt((1.-sinRef*sinRef)/(1.-sinGeo*sinGeo)));

	altAzPos[0]*=shortenxy;
	altAzPos[1]*=shortenxy;
	altAzPos[2]=sinRef*length;
}

// going from observed position to geometrical position.
void Refraction::innerRefractionBackward(Vec3d& altAzPos) const
{
	const double length = altAzPos.norm();
	if (qFuzzyCompare(length, 0.0) || qIsNaN(length))
	{
		// Under some circumstances there are zero coordinates. Just leave them alone.
		//qDebug() << "Refraction::innerRefractionBackward(): Zero vector detected - Continue with zero vector.";
		return;
	}
	Q_ASSERT(length>0.0);
	const double sinObs = altAzPos[2]/length;
	Q_ASSERT(fabs(sinObs)<=1.0);
	if (isModelInUse())
	{
		// ray-traced model (see StelRefractionModel), resampled in the table
		const double obs_alt_rad = std::asin(sinObs);
		const double r = static_cast<double>(table->backwardRefraction(static_cast<float>(obs_alt_rad)));
		if (r == 0.)
			return;
		const double sinGeo = std::sin(obs_alt_rad - r);
		const double longerxy = ((fabs(sinObs)>=1.0) ? 1.0 : std::sqrt((1.-sinGeo*sinGeo)/(1.-sinObs*sinObs)));
		altAzPos[0]*=longerxy;
		altAzPos[1]*=longerxy;
		altAzPos[2]=sinGeo*length;
		return;
	}
	float obs_alt_deg=static_cast<float>(M_180_PI*std::asin(sinObs));
	if (obs_alt_deg > 0.22879f)
	{
		// refraction from Bennett, in Meeus, Astr.Alg.
		float r=press_temp_corr * (1.f / std::tan((obs_alt_deg+7.31f/(obs_alt_deg+4.4f))*M_PI_180f) + 0.0013515f);
		obs_alt_deg -= r;
	}
	else if (obs_alt_deg > MIN_APP_ALTITUDE_DEG)
	{
		// backward refraction from polynomial fit against Saemundson[-5...-0.3]
		float r=(((((0.0444f*obs_alt_deg+.7662f)*obs_alt_deg+4.9746f)*obs_alt_deg+13.599f)*obs_alt_deg+8.052f)*obs_alt_deg-11.308f)*obs_alt_deg+34.341f;
		obs_alt_deg -= press_temp_corr*r;
	}
	else if (obs_alt_deg > MIN_APP_ALTITUDE_DEG-TRANSITION_WIDTH_APP_DEG)
	{
		// Compute top value from polynome, apply linear interpolation
		static const float r_min=(((((0.0444f*MIN_APP_ALTITUDE_DEG+.7662f)*MIN_APP_ALTITUDE_DEG
				+4.9746f)*MIN_APP_ALTITUDE_DEG+13.599f)*MIN_APP_ALTITUDE_DEG
			      +8.052f)*MIN_APP_ALTITUDE_DEG-11.308f)*MIN_APP_ALTITUDE_DEG+34.341f;

		obs_alt_deg -= r_min*press_temp_corr*(obs_alt_deg-(MIN_APP_ALTITUDE_DEG-TRANSITION_WIDTH_APP_DEG))/TRANSITION_WIDTH_APP_DEG;
	}
	else return;
	// At this point we have corrected observed altitude. Note that if we just change altAzPos[2], we would change vector length, so this would change our angles.
	// We have to make X,Y components of the vector a bit longer as well by the change in cosines of altitude, or (sqrt(1-sin(alt))

	const double geo_alt_rad=static_cast<double>(obs_alt_deg)*M_PI_180;
	const double sinGeo=std::sin(geo_alt_rad);
	const double longerxy=((fabs(sinObs)>=1.0) ? 1.0 :
			std::sqrt((1.-sinGeo*sinGeo)/(1.-sinObs*sinObs)));
	altAzPos[0]*=longerxy;
	altAzPos[1]*=longerxy;
	altAzPos[2]=sinGeo*length;
}

void Refraction::forward(Vec3d& altAzPos) const
{
	altAzPos.transfo4d(preTransfoMat);
	innerRefractionForward(altAzPos);
	altAzPos.transfo4d(postTransfoMat);
}

//Bennett's formula is not a strict inverse of Saemundsson's. There is a notable discrepancy (alt!=backward(forward(alt))) for
// geometric altitudes <-.3deg.  This is not a problem in real life, but if a user switches off landscape, click-identify may pose a problem.
// Below this altitude, we therefore use a polynomial fit that should represent a close inverse of Saemundsson.
void Refraction::backward(Vec3d& altAzPos) const
{
	altAzPos.transfo4d(invertPostTransfoMat);
	innerRefractionBackward(altAzPos);
	altAzPos.transfo4d(invertPreTransfoMat);
}

void Refraction::forward(Vec3f& altAzPos) const
{
	Vec3d vf=altAzPos.toVec3d();
	vf.transfo4d(preTransfoMat);
	innerRefractionForward(vf);
	vf.transfo4d(postTransfoMat);
	altAzPos.set(static_cast<float>(vf[0]), static_cast<float>(vf[1]), static_cast<float>(vf[2]));
}

void Refraction::backward(Vec3f& altAzPos) const
{
	altAzPos.transfo4d(invertPostTransfoMatf);
	Vec3d vf=altAzPos.toVec3d();
	innerRefractionBackward(vf);
	altAzPos.set(static_cast<float>(vf[0]), static_cast<float>(vf[1]), static_cast<float>(vf[2]));
	altAzPos.transfo4d(invertPreTransfoMatf);
}

static QByteArray refractionTableShader()
{
	return QByteArray(R"(
#ifndef REFRACTION_TABLE_DEFINED
#define REFRACTION_TABLE_DEFINED
// The ray-traced refraction (see Refraction::Table): forward table in the first @QUARTER@ vectors, backward table in the next ones
uniform bool REFRACTION_useTable;
uniform highp vec4 REFRACTION_table[@VECTORS@];
uniform highp vec4 REFRACTION_tableLimits; // geometric limb, geometric span, apparent limb, apparent span (radians)
uniform highp float REFRACTION_horizonRefraction;
highp float REFRACTION_tableEntry(int i)
{
	int v = i/4;
	int c = i - 4*v;
	highp vec4 e = REFRACTION_table[v];
	return c == 0 ? e.x : (c == 1 ? e.y : (c == 2 ? e.z : e.w));
}
highp float REFRACTION_tableLookup(int base, highp float x, highp float span)
{
	highp float f = pow(clamp(x/span, 0., 1.), 1./3.)*float(@SIZE@ - 1);
	int i = int(min(floor(f), float(@SIZE@ - 2)));
	highp float t = f - float(i);
	return mix(REFRACTION_tableEntry(base + i), REFRACTION_tableEntry(base + i + 1), t);
}
// Refraction (radians) of an object at the given geometric altitude
highp float REFRACTION_forwardRefraction(highp float geo)
{
	highp float x = geo - REFRACTION_tableLimits.x;
	if (x < 0.)
		return REFRACTION_horizonRefraction*max(0., (x + @FADE@)/@FADE@);
	return REFRACTION_tableLookup(0, x, REFRACTION_tableLimits.y);
}
// Refraction (radians) at the given apparent altitude
highp float REFRACTION_backwardRefraction(highp float app)
{
	highp float x = app - REFRACTION_tableLimits.z;
	if (x < 0.)
	{
		highp float r0 = REFRACTION_horizonRefraction;
		highp float lower = REFRACTION_tableLimits.x - @FADE@;
		if (app <= lower)
			return 0.;
		highp float geo = (app + r0*lower/@FADE@)/(1. + r0/@FADE@);
		return app - geo;
	}
	return REFRACTION_tableLookup(@SIZE@, x, REFRACTION_tableLimits.w);
}
#endif
)").replace("@QUARTER@", QByteArray::number(Refraction::Table::SIZE/4))
	        .replace("@VECTORS@", QByteArray::number(Refraction::Table::SIZE/2))
	        .replace("@SIZE@", QByteArray::number(Refraction::Table::SIZE))
	        .replace("@FADE@", QByteArray::number(static_cast<double>(Refraction::Table::FADE), 'g', 9));
}

QByteArray Refraction::getForwardTransformShader() const
{
	return refractionTableShader() + QByteArray(1+R"(
uniform float REFRACTION_press_temp_corr;
// Workaround for Intel's unusable implementation of sin & cos, which leads to broken geometry of the Moon every 0.9° of elevation.
highp float REFRACTION_sin(highp float x)
{
	const float PI = 3.14159265;
	x = mod(x+PI, 2.0*PI)-PI;
	return x*(0.999999599920672 + x*x*(-0.166665526354071 + x*x*(0.00833240298869917 + x*x*(-0.0001980863334175 + x*x*(2.69971463693744e-6 - 2.03622449118901e-8*x*x)))));
}
// Workaround for Intel's and AMD's unusable implementation of asin, which leads to time-dependent shifts of the Moon from its refracted positions.
highp float REFRACTION_asin(highp float x)
{
	float sign = x < 0. ? -1. : 1.;
	if(x < 0.) x = -x;
	x = 2. * sqrt(1. - x) - 1.;
	float v = 0.848061881596496 + x*(-0.755929497461161 + x*(-0.0539853235799928 + x*(-0.025701166121295 + x*(-0.00723634508887381 +
	           x*(-0.00314276748524976 + x*(-0.00101365621070969 + x*(-0.000411380592372285 + x*(-0.000428167561924693 - 0.000213293541786718*x))))))));
	return v * sign;
}
highp vec3 innerRefractionForward(highp vec3 altAzPos)
{
	const highp float PI = 3.14159265;
	const highp float M_180_PI = 180./PI;
	const highp float M_PI_180 = PI/180.;
	const highp float MIN_GEO_ALTITUDE_DEG=@MIN_GEO_ALTITUDE_DEG@;
	const highp float TRANSITION_WIDTH_GEO_DEG=@TRANSITION_WIDTH_GEO_DEG@;

	float press_temp_corr = REFRACTION_press_temp_corr;

	float len = length(altAzPos);
	if (len==0.0)
	{
		// Under some circumstances there are zero coordinates. Just leave them alone.
		return altAzPos;
	}

	highp float sinGeo = altAzPos[2]/len;
	highp float geom_alt_rad = REFRACTION_asin(sinGeo);
	if (REFRACTION_useTable)
	{
		highp float r = REFRACTION_forwardRefraction(geom_alt_rad);
		if (r == 0.)
			return altAzPos;
		highp float sinRefT = REFRACTION_sin(min(geom_alt_rad + r, 0.5*PI));
		highp float shortenxyT = abs(sinGeo)>=1.0 ? 1.0 : sqrt((1.-sinRefT*sinRefT)/(1.-sinGeo*sinGeo));
		altAzPos[0]*=shortenxyT;
		altAzPos[1]*=shortenxyT;
		altAzPos[2]=sinRefT*len;
		return altAzPos;
	}
	highp float geom_alt_deg = M_180_PI*geom_alt_rad;
	if (geom_alt_deg > MIN_GEO_ALTITUDE_DEG)
	{
		// refraction from Saemundsson, S&T1986 p70 / in Meeus, Astr.Alg.
		highp float r=press_temp_corr * ( 1.02 / tan((geom_alt_deg+10.3/(geom_alt_deg+5.11))*M_PI_180) + 0.0019279);
		geom_alt_deg += r;
		if (geom_alt_deg > 90.)
			geom_alt_deg=90.;
	}
	else if(geom_alt_deg>MIN_GEO_ALTITUDE_DEG-TRANSITION_WIDTH_GEO_DEG)
	{
		// Avoids the jump below -5 by interpolating linearly between MIN_GEO_ALTITUDE_DEG and bottom of transition zone
		highp float r_m5=press_temp_corr * ( 1.02 / tan((MIN_GEO_ALTITUDE_DEG+10.3/(MIN_GEO_ALTITUDE_DEG+5.11))*M_PI_180) + 0.0019279);
		geom_alt_deg += r_m5*(geom_alt_deg-(MIN_GEO_ALTITUDE_DEG-TRANSITION_WIDTH_GEO_DEG))/TRANSITION_WIDTH_GEO_DEG;
	}
	else return altAzPos;
	// At this point we have corrected geometric altitude. Note that if we just change altAzPos[2], we would change vector length, so this would change our angles.
	// We have to shorten X,Y components of the vector as well by the change in cosines of altitude, or (sqrt(1-sin(alt))

	highp float refr_alt_rad=geom_alt_deg*M_PI_180;
	highp float sinRef = REFRACTION_sin(refr_alt_rad);

	// FIXME: do we really need double's mantissa length here as a comment in the C++ code says?
	highp float shortenxy = abs(sinGeo)>=1.0 ? 1.0 : sqrt((1.-sinRef*sinRef)/(1.-sinGeo*sinGeo));

	altAzPos[0]*=shortenxy;
	altAzPos[1]*=shortenxy;
	altAzPos[2]=sinRef*len;

	return altAzPos;
}

uniform highp mat4 REFRACTION_preTransfoMat;
uniform highp mat4 REFRACTION_postTransfoMat;

highp vec3 vertexToAltAzPos(highp vec3 v)
{
	highp vec3 altAzPosNotRefracted = (REFRACTION_preTransfoMat * vec4(v,1)).xyz;
	return innerRefractionForward(altAzPosNotRefracted);
}

highp vec3 modelViewForwardTransform(highp vec3 v)
{
	highp vec3 altAzPos = vertexToAltAzPos(v);
	return (REFRACTION_postTransfoMat * vec4(altAzPos, 1)).xyz;
}

#define HAVE_REFRACTION
)").replace("@MIN_GEO_ALTITUDE_DEG@", std::to_string(MIN_GEO_ALTITUDE_DEG).c_str())
   .replace("@TRANSITION_WIDTH_GEO_DEG@", std::to_string(TRANSITION_WIDTH_GEO_DEG).c_str());
}

QByteArray Refraction::getBackwardTransformShader() const
{
	return refractionTableShader() + QByteArray(1+R"(
uniform float REFRACTION_press_temp_corr;

vec3 innerRefractionBackward(vec3 altAzPos)
{
	const float PI = 3.14159265;
	const float M_180_PI = 180./PI;
	const float M_PI_180 = PI/180.;
	const float MIN_APP_ALTITUDE_DEG=@MIN_APP_ALTITUDE_DEG@;
	const float TRANSITION_WIDTH_APP_DEG=@TRANSITION_WIDTH_APP_DEG@;
	float press_temp_corr = REFRACTION_press_temp_corr;

	float len = length(altAzPos);
	if (len==0.0)
	{
		// Under some circumstances there are zero coordinates. Just leave them alone.
		return altAzPos;
	}
	float sinObs = altAzPos[2]/len;
	if (REFRACTION_useTable)
	{
		highp float obsRad = asin(sinObs);
		highp float r = REFRACTION_backwardRefraction(obsRad);
		if (r == 0.)
			return altAzPos;
		highp float sinGeoT = sin(obsRad - r);
		highp float longerxyT = abs(sinObs)>=1.0 ? 1.0 : sqrt((1.-sinGeoT*sinGeoT)/(1.-sinObs*sinObs));
		altAzPos[0]*=longerxyT;
		altAzPos[1]*=longerxyT;
		altAzPos[2]=sinGeoT*len;
		return altAzPos;
	}
	float obs_alt_deg=M_180_PI*asin(sinObs);
	if (obs_alt_deg > 0.22879)
	{
		// refraction from Bennett, in Meeus, Astr.Alg.
		float r=press_temp_corr * (1. / tan((obs_alt_deg+7.31/(obs_alt_deg+4.4))*M_PI_180) + 0.0013515);
		obs_alt_deg -= r;
	}
	else if (obs_alt_deg > MIN_APP_ALTITUDE_DEG)
	{
		// backward refraction from polynomial fit against Saemundson[-5...-0.3]
		float r=(((((0.0444*obs_alt_deg+.7662)*obs_alt_deg+4.9746)*obs_alt_deg+13.599)*obs_alt_deg+8.052)*obs_alt_deg-11.308)*obs_alt_deg+34.341;
		obs_alt_deg -= press_temp_corr*r;
	}
	else if (obs_alt_deg > MIN_APP_ALTITUDE_DEG-TRANSITION_WIDTH_APP_DEG)
	{
		// Compute top value from polynome, apply linear interpolation
		const float r_min=(((((0.0444*MIN_APP_ALTITUDE_DEG+.7662)*MIN_APP_ALTITUDE_DEG
				+4.9746)*MIN_APP_ALTITUDE_DEG+13.599)*MIN_APP_ALTITUDE_DEG
			      +8.052)*MIN_APP_ALTITUDE_DEG-11.308)*MIN_APP_ALTITUDE_DEG+34.341;

		obs_alt_deg -= r_min*press_temp_corr*(obs_alt_deg-(MIN_APP_ALTITUDE_DEG-TRANSITION_WIDTH_APP_DEG))/TRANSITION_WIDTH_APP_DEG;
	}
	else return altAzPos;
	// At this point we have corrected observed altitude. Note that if we just change altAzPos[2], we would change vector length, so this would change our angles.
	// We have to make X,Y components of the vector a bit longer as well by the change in cosines of altitude, or (sqrt(1-sin(alt))

	float geo_alt_rad=obs_alt_deg*M_PI_180;
	float sinGeo=sin(geo_alt_rad);
	float longerxy=((abs(sinObs)>=1.0) ? 1.0 : sqrt((1.-sinGeo*sinGeo)/(1.-sinObs*sinObs)));
	altAzPos[0]*=longerxy;
	altAzPos[1]*=longerxy;
	altAzPos[2]=sinGeo*len;

	return altAzPos;
}

uniform mat4 REFRACTION_inversePreTransfoMat;
uniform mat4 REFRACTION_inversePostTransfoMat;

vec3 worldPosToAltAzPos(vec3 worldPos)
{
	return (REFRACTION_inversePostTransfoMat * vec4(worldPos,1)).xyz;
}

vec3 modelViewBackwardTransform(vec3 worldPos)
{
	vec3 altAzPosApparent = worldPosToAltAzPos(worldPos);
	vec3 altAzPosGeometric = innerRefractionBackward(altAzPosApparent);
	return (REFRACTION_inversePreTransfoMat * vec4(altAzPosGeometric,1)).xyz;
}

#define HAVE_REFRACTION
)").replace("@MIN_APP_ALTITUDE_DEG@", std::to_string(MIN_APP_ALTITUDE_DEG).c_str())
   .replace("@TRANSITION_WIDTH_APP_DEG@", std::to_string(TRANSITION_WIDTH_APP_DEG).c_str());
}

void Refraction::setTableUniforms(QOpenGLShaderProgram& program) const
{
	const bool use = isModelInUse();
	program.setUniformValue("REFRACTION_useTable", use);
	if (!use)
		return;
	QVector4D vectors[Table::SIZE/2];
	for (int i = 0; i < Table::SIZE/4; ++i)
	{
		vectors[i] = QVector4D(table->forward[4*i], table->forward[4*i+1], table->forward[4*i+2], table->forward[4*i+3]);
		vectors[i + Table::SIZE/4] = QVector4D(table->backward[4*i], table->backward[4*i+1], table->backward[4*i+2], table->backward[4*i+3]);
	}
	program.setUniformValueArray("REFRACTION_table", vectors, Table::SIZE/2);
	program.setUniformValue("REFRACTION_tableLimits", QVector4D(table->geometricLimb, table->geometricSpan, table->apparentLimb, table->apparentSpan));
	program.setUniformValue("REFRACTION_horizonRefraction", table->horizonRefraction);
}

void Refraction::setForwardTransformUniforms(QOpenGLShaderProgram& program) const
{
	setTableUniforms(program);
	program.setUniformValue("REFRACTION_press_temp_corr", GLfloat(press_temp_corr));
	program.setUniformValue("REFRACTION_preTransfoMat", preTransfoMatf.toQMatrix());
	program.setUniformValue("REFRACTION_postTransfoMat", postTransfoMatf.toQMatrix());
}

void Refraction::setBackwardTransformUniforms(QOpenGLShaderProgram& program) const
{
	setTableUniforms(program);
	program.setUniformValue("REFRACTION_press_temp_corr", GLfloat(press_temp_corr));
	program.setUniformValue("REFRACTION_inversePreTransfoMat", invertPreTransfoMatf.toQMatrix());
	program.setUniformValue("REFRACTION_inversePostTransfoMat", invertPostTransfoMatf.toQMatrix());
}

void Refraction::setPressure(float p)
{
	pressure=p;
	updatePrecomputed();
}

void Refraction::setTemperature(float t)
{
	temperature=t;
	updatePrecomputed();
}

float Refraction::Table::forwardRefraction(float g) const
{
	const float x = g - geometricLimb;
	if (x < 0.f)
		return horizonRefraction*std::max(0.f, (x + FADE)/FADE);
	const float f = std::cbrt(std::min(x/geometricSpan, 1.f))*static_cast<float>(SIZE - 1);
	const int i = std::min(static_cast<int>(f), SIZE - 2);
	const float t = f - static_cast<float>(i);
	return forward[i] + t*(forward[i+1] - forward[i]);
}

float Refraction::Table::backwardRefraction(float a) const
{
	const float x = a - apparentLimb;
	if (x < 0.f)
	{
		// the inverse of forwardRefraction() below the true horizon
		const float lower = geometricLimb - FADE;
		if (a <= lower)
			return 0.f;
		const float g = (a + horizonRefraction*lower/FADE)/(1.f + horizonRefraction/FADE);
		return a - g;
	}
	const float f = std::cbrt(std::min(x/apparentSpan, 1.f))*static_cast<float>(SIZE - 1);
	const int i = std::min(static_cast<int>(f), SIZE - 2);
	const float t = f - static_cast<float>(i);
	return backward[i] + t*(backward[i+1] - backward[i]);
}

void Refraction::updateModel()
{
	StelRefractionModel::Conditions c;
	c.altitude = observerAltitude;
	c.pressure = static_cast<double>(pressure);
	c.temperature = static_cast<double>(temperature);
	if (model && model->getConditions() == c)
		return;
	QElapsedTimer timer;
	timer.start();
	model = std::make_shared<const StelRefractionModel>(c);

	// Resample the model for the transformations: denser near the true horizon, where the refraction changes fast
	auto t = std::make_shared<Table>();
	t->geometricLimb = static_cast<float>(model->getHorizonGeometric());
	t->geometricSpan = static_cast<float>(M_PI_2 - model->getHorizonGeometric());
	t->apparentLimb = static_cast<float>(model->getHorizonApparent());
	t->apparentSpan = static_cast<float>(M_PI_2 - model->getHorizonApparent());
	t->horizonRefraction = static_cast<float>(model->getHorizonRefraction());
	for (int i = 0; i < Table::SIZE; ++i)
	{
		const double u = static_cast<double>(i)/(Table::SIZE - 1);
		const double g = model->getHorizonGeometric() + (M_PI_2 - model->getHorizonGeometric())*u*u*u;
		const double a = model->getHorizonApparent() + (M_PI_2 - model->getHorizonApparent())*u*u*u;
		t->forward[i] = static_cast<float>(model->toApparent(g) - g);
		t->backward[i] = static_cast<float>(a - model->toGeometric(a));
	}
	t->forward[0] = t->backward[0] = t->horizonRefraction;
	t->forward[Table::SIZE-1] = t->backward[Table::SIZE-1] = 0.f;
	table = t;
	const double toArcmin = 60.*M_180_PI;
	qInfo().noquote() << QString("Refraction model: observer at %1 m, %2 hPa, %3 C: true horizon at %4 deg (geometric dip %5 deg), "
	                             "refraction there %6', at 0 deg %7', at 5 deg %8' (%9 ms)")
	                     .arg(c.altitude, 0, 'f', 0).arg(c.pressure, 0, 'f', 1).arg(c.temperature, 0, 'f', 1)
	                     .arg(model->getHorizonApparent()*M_180_PI, 0, 'f', 3).arg(model->getGeometricDip()*M_180_PI, 0, 'f', 3)
	                     .arg(model->getHorizonRefraction()*toArcmin, 0, 'f', 2)
	                     .arg((0. - model->toGeometric(0.))*toArcmin, 0, 'f', 2)
	                     .arg((5.*M_PI_180 - model->toGeometric(5.*M_PI_180))*toArcmin, 0, 'f', 2)
	                     .arg(timer.elapsed());
}
