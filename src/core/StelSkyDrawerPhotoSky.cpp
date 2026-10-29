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

// Photographic mode of StelSkyDrawer: the light of the sky as the camera records it: white balance, the colors of
// extinction, the airglow and the sky during star trails.

#include "StelSkyDrawer.hpp"
#include "LightDomes.hpp"
#include "SolarSystem.hpp"
#include "StelProjector.hpp"
#include "StelApp.hpp"
#include "StelSRGB.hpp"
#include "StelCore.hpp"
#include "StelUtils.hpp"

#include <QOpenGLContext>
#include <QOpenGLShaderProgram>
#include <QtGlobal>
#include <array>

namespace
{
//! Encode a linear value (0..1) with the sRGB transfer function
float encodeSRGB(float c)
{
	return c <= 0.0031308f ? 12.92f*c : 1.055f*std::pow(c, 1.f/2.4f) - 0.055f;
}
}

const float* StelSkyDrawer::getPhotoWhiteBalanceMatrix() const
{
	if (!getFlagPhotoFilmActive())
		return photoWBMatrix;
	const PhotoFilmSpec& f = getPhotoActiveFilmSpec();
	if (f.type == PhotoFilmMonochrome)
	{
		// The black and white picture is made from the film's own sensitivity (see getPhotoFilmMonoWeights())
		static const float identity[9] = {1,0,0, 0,1,0, 0,0,1};
		return identity;
	}
	// The color shift of the reciprocity failure is done per point of the picture (getPhotoReciprocityParams())
	const Vec3f tint = flagPhotoReciprocityPerPoint ? Vec3f(1.f) : getPhotoFilmTint(getPhotoEffectiveExposureTime());
	if (getPhotoFilmSpectral(f))
	{
		// The layers of the film from the light of the scene (balanced for daylight), each with its own reciprocity
		// failure; the dyes of the layers are taken as the primaries of the display
		for (int r = 0; r < 3; ++r)
			for (int c = 0; c < 3; ++c)
				photoWBMatrixFilm[r*3 + c] = f.rgbToFilm[r*3 + c] * tint[r];
		return photoWBMatrixFilm;
	}
	// The daylight balance of the film (photoWBMatrix, see updatePhotoWhiteBalanceMatrix()), after the layers'
	// own reciprocity failure: the matrix acts on the light as the film recorded it
	for (int r = 0; r < 3; ++r)
		for (int c = 0; c < 3; ++c)
			photoWBMatrixFilm[r*3 + c] = photoWBMatrix[r*3 + c] * tint[c];
	return photoWBMatrixFilm;
}

void StelSkyDrawer::setPhotoFilmSkyUniforms(QOpenGLShaderProgram& program) const
{
	const float* m = (getFlagPhotographicModeActive() && flagPhotoFilmSpectralSky) ? getPhotoFilmSkyMatrix() : nullptr;
	program.setUniformValue("photoFilmSky", m != nullptr);
	if (!m)
		return;
	program.setUniformValue("photoFilmSkyR", m[0], m[1], m[2], m[3]);
	program.setUniformValue("photoFilmSkyG", m[4], m[5], m[6], m[7]);
	program.setUniformValue("photoFilmSkyB", m[8], m[9], m[10], m[11]);
}

// ---- White balance ----

namespace
{
typedef std::array<double, 9> M3; // row-major 3x3
M3 mul(const M3& a, const M3& b)
{
	M3 r{};
	for (int i=0; i<3; ++i)
		for (int j=0; j<3; ++j)
			r[3*i+j] = a[3*i]*b[j] + a[3*i+1]*b[3+j] + a[3*i+2]*b[6+j];
	return r;
}
Vec3d mul(const M3& a, const Vec3d& v)
{
	return Vec3d(a[0]*v[0]+a[1]*v[1]+a[2]*v[2], a[3]*v[0]+a[4]*v[1]+a[5]*v[2], a[6]*v[0]+a[7]*v[1]+a[8]*v[2]);
}
M3 inverse(const M3& m)
{
	const double c00 = m[4]*m[8]-m[5]*m[7], c01 = m[5]*m[6]-m[3]*m[8], c02 = m[3]*m[7]-m[4]*m[6];
	const double det = m[0]*c00 + m[1]*c01 + m[2]*c02;
	if (std::fabs(det) < 1e-12)
		return M3{1,0,0, 0,1,0, 0,0,1};
	const double d = 1./det;
	return M3{c00*d, (m[2]*m[7]-m[1]*m[8])*d, (m[1]*m[5]-m[2]*m[4])*d,
	          c01*d, (m[0]*m[8]-m[2]*m[6])*d, (m[2]*m[3]-m[0]*m[5])*d,
	          c02*d, (m[1]*m[6]-m[0]*m[7])*d, (m[0]*m[4]-m[1]*m[3])*d};
}
// Linear sRGB <-> CIE XYZ (D65), and the Bradford cone response
const M3 RGB_TO_XYZ{0.4124564, 0.3575761, 0.1804375, 0.2126729, 0.7151522, 0.0721750, 0.0193339, 0.1191920, 0.9503041};
const M3 XYZ_TO_RGB{3.2404542, -1.5371385, -0.4985314, -0.9692660, 1.8760108, 0.0415560, 0.0556434, -0.2040259, 1.0572252};
const M3 BRADFORD{0.8951, 0.2664, -0.1614, -0.7502, 1.7135, 0.0367, 0.0389, -0.0685, 1.0296};
const Vec3d D65_XYZ(0.95047, 1., 1.08883);

//! Chromaticity of light of a color temperature: the Planckian locus (Kim et al. 2002) for warm light,
//! the CIE daylight locus from 5000 K on (so that 6504 K is D65, like the daylight presets of cameras),
//! blended between 4000 and 5000 K.
Vec3d kelvinToXYZ(double T)
{
	T = std::clamp(T, 1667., 25000.);
	const double t1 = 1e3/T, t2 = t1*t1, t3 = t2*t1;
	// Planckian locus
	double xp = (T < 4000.) ? -0.2661239*t3 - 0.2343589*t2 + 0.8776956*t1 + 0.179910
	                        : -3.0258469*t3 + 2.1070379*t2 + 0.2226347*t1 + 0.240390;
	double yp;
	if (T < 2222.)
		yp = -1.1063814*xp*xp*xp - 1.34811020*xp*xp + 2.18555832*xp - 0.20219683;
	else if (T < 4000.)
		yp = -0.9549476*xp*xp*xp - 1.37418593*xp*xp + 2.09137015*xp - 0.16748867;
	else
		yp = 3.0817580*xp*xp*xp - 5.87338670*xp*xp + 3.75112997*xp - 0.37001483;
	double x = xp, y = yp;
	if (T > 4000.)
	{
		// CIE daylight locus
		const double xd = (T <= 7000.) ? -4.6070*t3 + 2.9678*t2 + 0.09911*t1 + 0.244063
		                               : -2.0064*t3 + 1.9018*t2 + 0.24748*t1 + 0.237040;
		const double yd = -3.000*xd*xd + 2.870*xd - 0.275;
		const double w = std::clamp((T - 4000.)/1000., 0., 1.);
		x = (1.-w)*xp + w*xd;
		y = (1.-w)*yp + w*yd;
	}
	return Vec3d(x/y, 1., (1.-x-y)/y);
}
}

double StelSkyDrawer::photoWhiteBalancePresetKelvin(int wb)
{
	switch (wb)
	{
		case PhotoWBDaylight:    return 5200.;
		case PhotoWBCloudy:      return 6000.;
		case PhotoWBShade:       return 7000.;
		case PhotoWBTungsten:    return 3200.;
		case PhotoWBFluorescent: return 4000.;
		case PhotoWBFlash:       return 5500.;
		case PhotoWBNeutral:     return 6504.;
		default:                 return 0.;
	}
}

double StelSkyDrawer::getPhotoWhiteBalanceTemperature() const
{
	// Daylight color films are balanced for about 5500 K
	if (photoFilm != PhotoFilmNone)
		return 5500.;
	if (photoWhiteBalance == PhotoWBAuto)
		return photoAutoWhiteBalanceKelvin;
	if (photoWhiteBalance == PhotoWBCustom)
		return photoWhiteBalanceKelvin;
	return photoWhiteBalancePresetKelvin(photoWhiteBalance);
}

bool StelSkyDrawer::getFlagPhotoAutoWhiteBalanceActive() const
{
	return photoWhiteBalance == PhotoWBAuto && photoFilm == PhotoFilmNone && getFlagPhotographicModeActive();
}

double StelSkyDrawer::photoKelvinForBlueRedRatio(double ratio)
{
	// The blue/red ratio of white light rises with the color temperature: bisection in log(T)
	const auto whiteRatio = [](double T) {
		const Vec3d rgb = mul(XYZ_TO_RGB, kelvinToXYZ(T));
		return rgb[2]/rgb[0];
	};
	double lo = std::log(PHOTO_AWB_MIN_KELVIN), hi = std::log(PHOTO_AWB_MAX_KELVIN);
	if (!(ratio > whiteRatio(PHOTO_AWB_MIN_KELVIN)))
		return PHOTO_AWB_MIN_KELVIN;
	if (!(ratio < whiteRatio(PHOTO_AWB_MAX_KELVIN)))
		return PHOTO_AWB_MAX_KELVIN;
	for (int i=0; i<40; ++i)
	{
		const double mid = 0.5*(lo+hi);
		if (whiteRatio(std::exp(mid)) < ratio)
			lo = mid;
		else
			hi = mid;
	}
	return std::exp(0.5*(lo+hi));
}

void StelSkyDrawer::updatePhotoWhiteBalanceMatrix()
{
	const double T = getPhotoWhiteBalanceTemperature();
	if (std::fabs(T - photoWBMatrixKelvin) < 0.5)
		return;
	photoWBMatrixKelvin = T;
	M3 m{1,0,0, 0,1,0, 0,0,1};
	if (photoWhiteBalance != PhotoWBNeutral || photoFilm != PhotoFilmNone)
	{
		// Von Kries adaptation in Bradford cone space: light of temperature T becomes the display white (D65).
		// The light of temperature T at luminance 1 stays at luminance 1.
		const Vec3d src = mul(BRADFORD, kelvinToXYZ(T));
		const Vec3d dst = mul(BRADFORD, D65_XYZ);
		const M3 gains{dst[0]/src[0], 0, 0, 0, dst[1]/src[1], 0, 0, 0, dst[2]/src[2]};
		m = mul(XYZ_TO_RGB, mul(inverse(BRADFORD), mul(gains, mul(BRADFORD, RGB_TO_XYZ))));
	}
	const M3 inv = inverse(m);
	for (int i=0; i<9; ++i)
	{
		photoWBMatrix[i] = static_cast<float>(m[static_cast<size_t>(i)]);
		photoWBInverse[i] = static_cast<float>(inv[static_cast<size_t>(i)]);
	}
}

void StelSkyDrawer::setPhotoWhiteBalance(int wb)
{
	wb = qBound(0, wb, 8);
	if (wb == photoWhiteBalance)
		return;
	photoWhiteBalance = wb;
	photoAutoWhiteBalanceFresh = true;
	photoWBMatrixKelvin = -1.; // e.g. neutral and 6504 K differ slightly
	updatePhotoWhiteBalanceMatrix();
	StelApp::immediateSave("viewing/photographic_white_balance", wb);
	emit photoWhiteBalanceChanged(wb);
}

void StelSkyDrawer::setPhotoWhiteBalanceKelvin(double kelvin)
{
	kelvin = qBound(2000., kelvin, 12000.);
	if (qFuzzyCompare(kelvin, photoWhiteBalanceKelvin))
		return;
	photoWhiteBalanceKelvin = kelvin;
	updatePhotoWhiteBalanceMatrix();
	StelApp::immediateSave("viewing/photographic_white_balance_kelvin", kelvin);
	emit photoWhiteBalanceKelvinChanged(kelvin);
}

// ---- Extinction colors ----

void StelSkyDrawer::updatePhotoExtinction()
{
	const double kInUse = static_cast<double>(extinction.getExtinctionCoefficient());
	// the coefficient as set: the table (scaled to it) gives the color at the normal humidity; the extra aerosols of
	// a higher (or lower) humidity are added with their own, flatter color
	const double k = static_cast<double>(extinctionBase);
	photoExtinctionActive = flagPhotoExtinctionColor && getFlagPhotographicModeActive() && getFlagHasAtmosphere()
	                        && kInUse >= 0.01 && atmosphereTransmission.isLoaded()
	                        && core->getCurrentLocation().planetName == QStringLiteral("Earth");
	if (!photoExtinctionActive)
		return;
	const double altitude = static_cast<double>(core->getCurrentLocation().altitude);
	if (altitude == photoExtinctionAltitude && k == photoExtinctionCoefficient && kInUse == photoExtinctionInUse && !photoExtinctionTints.empty())
		return;
	photoExtinctionAltitude = altitude;
	photoExtinctionCoefficient = k;
	photoExtinctionInUse = kInUse;
	const double extraDepth = photoAerosol.depth - photoAerosol.baseDepth;
	const double extraRgb[3] = {extraDepth*std::pow(0.610/0.55, -photoAerosol.angstrom), extraDepth,
	                            extraDepth*std::pow(0.465/0.55, -photoAerosol.angstrom)};

	// The table has its own amount of aerosols etc. Scale its optical depths so that white light loses
	// as much towards the zenith as the extinction coefficient says (k magnitudes per airmass at the site):
	// the brightness keeps following the coefficient, the table gives the color.
	const double zenith = atmosphereTransmission.zenithMagnitude(altitude);
	const double depthScale = zenith > 1e-4 ? k/zenith : 1.;
	const double sinMin = atmosphereTransmission.horizonSine(altitude);
	photoExtinctionSinMin = static_cast<float>(sinMin);
	constexpr int size = 256;
	photoExtinctionTints.assign(size, Vec3f(1.f));
	// From the zenith down: very close to the horizon of the observer hardly any light passes;
	// there the last good color is kept.
	Vec3f last(1.f);
	for (int i = size-1; i >= 0; --i)
	{
		const double u = static_cast<double>(i)/(size-1);
		const double s = sinMin + (1.-sinMin)*u*u;
		Vec3d t = atmosphereTransmission.transmission(altitude, std::asin(std::clamp(s, -1., 1.)), depthScale);
		if (extraDepth != 0.)
		{
			// the airmass of the aerosols (a layer of 1.2 km over a spherical Earth)
			constexpr double RH = 6371./1.2;
			const double airmass = std::sqrt(RH*s*RH*s + 2.*RH + 1.) - RH*s;
			for (int c=0; c<3; ++c)
				t[c] = qBound(0., t[c]*std::exp(-extraRgb[c]*airmass), 1.);
		}
		const double Y = 0.2126*t[0] + 0.7152*t[1] + 0.0722*t[2];
		if (Y > 1e-7)
			last = Vec3f(static_cast<float>(t[0]/Y), static_cast<float>(t[1]/Y), static_cast<float>(t[2]/Y));
		photoExtinctionTints[static_cast<size_t>(i)] = last;
	}
	photoExtinctionTextureDirty = true;
}

void StelSkyDrawer::setFlagPhotoExtinctionColor(bool b)
{
	if (b == flagPhotoExtinctionColor)
		return;
	flagPhotoExtinctionColor = b;
	StelApp::immediateSave("viewing/flag_photographic_extinction_color", b);
	emit flagPhotoExtinctionColorChanged(b);
}

void StelSkyDrawer::setFlagPhotoAirglow(bool b)
{
	if (b == flagPhotoAirglow)
		return;
	flagPhotoAirglow = b;
	StelApp::immediateSave("viewing/flag_photographic_airglow", b);
	emit flagPhotoAirglowChanged(b);
}

void StelSkyDrawer::setFlagPhotoFilmSpectralSky(bool b)
{
	if (b == flagPhotoFilmSpectralSky)
		return;
	flagPhotoFilmSpectralSky = b;
	StelApp::immediateSave("astro/flag_photographic_film_spectral_sky", b);
	emit flagPhotoFilmSpectralSkyChanged(b);
}

void StelSkyDrawer::setPhotoAirglowStrength(double strength)
{
	const float v = qBound(0.25f, static_cast<float>(strength), 2.f);
	if (qFuzzyCompare(1.f+v, 1.f+photoAirglowStrength))
		return;
	photoAirglowStrength = v;
	StelApp::immediateSave("viewing/photographic_airglow_strength", static_cast<double>(v));
	emit photoAirglowStrengthChanged(static_cast<double>(v));
}

void StelSkyDrawer::setPhotoHumidity(double percent)
{
	percent = qBound(20., percent, 98.);
	if (qFuzzyCompare(percent, photoHumidity))
		return;
	photoHumidity = percent;
	StelApp::immediateSave("viewing/photographic_humidity", percent);
	updatePhotoHumidity();
	emit photoHumidityChanged(percent);
}

void StelSkyDrawer::updatePhotoHumidity()
{
	const bool active = getFlagPhotographicModeActive() && core->getCurrentLocation().planetName == QStringLiteral("Earth");
	const double humidity = active ? photoHumidity : PHOTO_NORMAL_HUMIDITY;
	const double altitude = qMax(0., static_cast<double>(core->getCurrentLocation().altitude)/1000.);  // km
	const double base = static_cast<double>(extinctionBase);
	if (humidity == photoHumidityInputs[0] && base == photoHumidityInputs[1] && altitude == photoHumidityInputs[2])
		return;
	photoHumidityInputs[0] = humidity;
	photoHumidityInputs[1] = base;
	photoHumidityInputs[2] = altitude;

	// The aerosols at the normal humidity: what the air (Rayleigh, above the observer) and ozone (0.016) do not
	// explain of the extinction coefficient (magnitudes per airmass = 1.0857 optical depth), at least 0.01
	const double rayleigh = 0.0088*std::pow(0.55, -4.05)*std::exp(-altitude/8.);
	const double baseDepth = qMax(0.01, base/1.0857 - rayleigh - 0.016);
	// Hygroscopic growth: the scattering of the aerosols rises as (1 - RH)^-gamma (Hanel 1976; gamma about 0.5 for
	// continental air): x2 at 80%, x2.8 at 95% against 60%. The grown particles are larger: the haze becomes whiter
	// (the Angstrom exponent falls from about 1.3 towards 0.7), scatters more forwards (g from 0.70 towards 0.76)
	// and absorbs less (albedo from 0.90 towards 0.96), as in the continental aerosols of OPAC (Hess et al. 1998).
	const double rh = humidity/100., rh0 = PHOTO_NORMAL_HUMIDITY/100.;
	const double growth = std::pow((1. - rh)/(1. - rh0), -0.5);
	PhotoAerosol a;
	a.baseDepth = baseDepth;
	a.depth = baseDepth*growth;
	a.angstrom = 1.3*std::pow(growth, -0.6);
	a.asymmetry = qBound(0.6, 0.70 + 0.06*std::log2(growth), 0.8);
	a.albedo = qBound(0.8, 1. - 0.1*std::pow(growth, -0.9), 0.98);
	photoAerosol = a;
	// The extinction in use: the coefficient as set, with the change of the aerosols
	const double k = qMax(0., base + 1.0857*(a.depth - baseDepth));
	extinction.setExtinctionCoefficient(static_cast<float>(k));
	// The light of lamps nearby scattered towards the zenith, against the normal humidity (single scattering)
	PhotoAerosol normal;
	normal.baseDepth = normal.depth = baseDepth;
	photoHazeLightFactor = std::fabs(growth - 1.) < 1e-6 ? 1.
	                       : LightDomes::nearbyLightRatio(a.depth, a.angstrom, a.asymmetry, a.albedo,
	                                                      normal.depth, normal.angstrom, normal.asymmetry, normal.albedo, altitude);
}

void StelSkyDrawer::setFlagPhotoAureole(bool b)
{
	if (b == flagPhotoAureole)
		return;
	flagPhotoAureole = b;
	StelApp::immediateSave("viewing/flag_photographic_aureole", b);
	emit flagPhotoAureoleChanged(b);
}

namespace
{
// The aureole of the haze: the forward peak of the scattering by aerosols. Its shape is that of the phase function of
// the aerosols of the default ShowMySky model, 1/(theta^2 + c^2) with c = 0.74 degrees (1/((1 - cos theta) 600 +
// 0.05)), out to about 15 degrees. In the delta-M approximation (see LightDomes) the fraction g^2 of the scattered light
// is this peak: the haze takes the optical depth tau albedo g^2 out of the beam into the aureole, which then goes on
// almost like the direct light. Against the direct light the aureole is exp(tau albedo g^2 X) - 1 for the airmass X of
// the aerosols (a layer of 1.2 km over a spherical Earth).
constexpr double AUREOLE_CORE = 0.74*M_PI/180.;
constexpr double AUREOLE_EXTENT = 15.*M_PI/180.;
constexpr float AUREOLE_MAX = 3.f;  // near the horizon: the single scattering estimate no longer holds
}

bool StelSkyDrawer::getFlagPhotoAureoleActive() const
{
	if (!flagPhotoAureole || !getFlagPhotographicModeActive() || !getFlagHasAtmosphere()
	    || core->getCurrentLocation().planetName != QStringLiteral("Earth"))
		return false;
	return photoAerosol.depth*photoAerosol.albedo*photoAerosol.asymmetry*photoAerosol.asymmetry > 0.002;
}

void StelSkyDrawer::getPhotoAureoleSize(const StelProjector& prj, float& coreSize, float& extent) const
{
	const double ppr = static_cast<double>(prj.getPixelPerRadAtCenter());
	coreSize = static_cast<float>(AUREOLE_CORE*ppr);
	extent = static_cast<float>(AUREOLE_EXTENT*ppr);
}

std::vector<float> StelSkyDrawer::getPhotoAureoleMap(int gw, int gh, int w, int h) const
{
	std::vector<float> map(static_cast<size_t>(gw)*gh, 0.f);
	const double forward = photoAerosol.depth*photoAerosol.albedo*photoAerosol.asymmetry*photoAerosol.asymmetry;
	const StelProjectorP prj = core->getProjection(StelCore::FrameAltAz, StelCore::RefractionOff);
	constexpr double RH = 6371./1.2;
	for (int j=0; j<gh; ++j)
		for (int i=0; i<gw; ++i)
		{
			Vec3d v;
			prj->unProject((i + 0.5)*w/gw, (j + 0.5)*h/gh, v);
			v.normalize();
			if (v[2] < 0.)
				continue;  // the ground: no haze between it and the observer
			const double s = v[2];
			const double airmass = std::sqrt(RH*s*RH*s + 2.*RH + 1.) - RH*s;
			map[static_cast<size_t>(j)*gw + i] = qMin(AUREOLE_MAX, static_cast<float>(std::expm1(forward*airmass)));
		}
	return map;
}

void StelSkyDrawer::setPhotoHazeUniforms(QOpenGLShaderProgram& program, double meanSkyLuminance, bool addLight) const
{
	const bool active = getFlagPhotographicModeActive() && getFlagHasAtmosphere()
	                    && core->getCurrentLocation().planetName == QStringLiteral("Earth");
	const double altitude = qMax(0., static_cast<double>(core->getCurrentLocation().altitude)/1000.);
	// The aerosols of the ShowMySky models (atmosphere/default/params.atmo: 1.0333e8 m^-3 over 1.2 km, 0.042968 um^2):
	// a vertical optical depth of only 0.0053 at sea level. The rest of the haze of the extinction coefficient is added.
	const double modelDepth = 0.0053*std::exp(-altitude/1.2);
	const double extra = active ? qMax(0., photoAerosol.depth - modelDepth) : 0.;
	program.setUniformValue("photoHaze", extra > 1e-4);
	if (!(extra > 1e-4))
		return;
	const PhotoAerosol& a = photoAerosol;
	const double f = a.asymmetry*a.asymmetry;  // delta-M: the forward peak (the aureole) goes on with the beam
	const double wl[3] = {0.610, 0.550, 0.465};
	float ext[3];
	for (int c=0; c<3; ++c)
		ext[c] = static_cast<float>(extra*std::pow(wl[c]/0.55, -a.angstrom)*(1. - a.albedo*f));
	const float albedo = static_cast<float>(a.albedo*(1. - f)/(1. - a.albedo*f));
	program.setUniformValue("photoHazeExtinction", ext[0], ext[1], ext[2]);
	program.setUniformValue("photoHazeAlbedo", albedo, albedo, albedo);
	program.setUniformValue("photoHazeG", static_cast<float>(a.asymmetry/(1. + a.asymmetry)));
	program.setUniformValue("photoHazeAdd", addLight);
	// The Sun and the Moon: their light on the haze, dimmed by the air above it (Rayleigh, ozone), none when they are
	// below the horizon (the haze lies in the shadow of the Earth)
	static SolarSystem* ssys = GETSTELMODULE(SolarSystem);
	const auto light = [&](const PlanetP& body, const char* dirName, const char* lightName) {
		Vec3f E(0.f), dir(0.f, 0.f, 1.f);
		if (body)
		{
			Vec3d v = core->j2000ToAltAz(body->getJ2000EquatorialPos(core), StelCore::RefractionOff);
			v.normalize();
			dir = v.toVec3f();
			const double s = v[2];
			const double fade = qBound(0., (s + 0.02)/0.03, 1.);
			if (fade > 0.)
			{
				const double illuminance = std::pow(10., -0.4*(static_cast<double>(body->getVMagnitude(core)) + 14.18));  // lux
				const double airmass = 1./(qMax(s, 0.) + 0.025*std::exp(-11.*qMax(s, 0.)));
				const double rayleighAtObserver = std::exp(-altitude/8.);
				const double ozone[3] = {0.020, 0.016, 0.005};
				for (int c=0; c<3; ++c)
				{
					const double tau = 0.0088*std::pow(wl[c], -4.05)*rayleighAtObserver + ozone[c];
					E[c] = static_cast<float>(fade*illuminance*std::exp(-tau*airmass));
				}
			}
		}
		program.setUniformValue(dirName, dir[0], dir[1], dir[2]);
		program.setUniformValue(lightName, E[0], E[1], E[2]);
	};
	light(ssys ? ssys->getSun() : PlanetP(), "photoHazeSunDir", "photoHazeSunLight");
	light(ssys ? ssys->getMoon() : PlanetP(), "photoHazeMoonDir", "photoHazeMoonLight");
	// The light of the sky itself, which the haze scatters too: its mean luminance, white
	const float diffuse = static_cast<float>(qMax(0., meanSkyLuminance));
	program.setUniformValue("photoHazeDiffuse", diffuse, diffuse, diffuse);
}

double StelSkyDrawer::getPhotoAirglowZenithLuminance() const
{
	if (!flagPhotoAirglow || !getFlagPhotographicModeActive() || !getFlagHasAtmosphere()
	    || core->getCurrentLocation().planetName != QStringLiteral("Earth"))
		return 0.;
	// The solar cycle, as in Skybright (Schaefer 1998): brightest near the maxima (1990, 2001, 2012...)
	int year, month, day;
	StelUtils::getDateFromJulianDay(core->getJD(), &year, &month, &day);
	const double y = year + (month - 0.5)/12.;
	const double cycle = 1. + 0.3*std::cos(0.57118*(y - 1992.));
	// The surface brightness at the zenith (V, magnitudes per square arc second, as observed through the atmosphere)
	// at a mean solar activity; the cycle changes it by +-30% (about +-0.3 mag), and it varies from night to night
	// (photoAirglowStrength). The whole dark sky is about 21.9-22.0 at the best sites: the airglow, plus the
	// zodiacal light and the starlight that Stellarium draws by themselves.
	// Surface brightness to luminance: L [cd/m^2] = 10.8e4 x 10^(-0.4 mag/arcsec^2)
	constexpr double zenithMagnitude = 22.5;
	return 10.8e4*std::pow(10., -0.4*zenithMagnitude)*cycle*static_cast<double>(photoAirglowStrength);
}

Vec3f StelSkyDrawer::getPhotoAirglow() const
{
	const double zenith = getPhotoAirglowZenithLuminance();
	if (!(zenith > 0.))
		return Vec3f(0.f);
	// Observed at the zenith, so above the atmosphere it is brighter by the extinction of one airmass
	const double k = static_cast<double>(extinction.getExtinctionCoefficient());
	const double luminance = zenith*std::pow(10., 0.4*k);
	// The color: the night sky at dark observatories is much redder than sunlight (B-V about 0.95, V-R about 0.8,
	// against 0.65 and 0.36 for the Sun; Benn & Ellison 1998, Patat 2003): the green line of oxygen (558 nm),
	// sodium (589 nm), the red oxygen lines (630 nm) and the OH bands. As linear sRGB against white sunlight
	// (luminance 1): a warm, brownish grey in daylight white balance.
	Vec3f color(1.227f, 0.959f, 0.748f);
	// A color film sees the lines with its own layers (see getPhotoFilmSpec())
	if (getFlagPhotoFilmActive() && getPhotoFilmSpectral(getPhotoActiveFilmSpec()))
	{
		const std::array<float, 3>& c = getPhotoActiveFilmSpec().airglow;
		color.set(c[0], c[1], c[2]);
	}
	return color*static_cast<float>(luminance*static_cast<double>(getPhotoSceneScale()));
}

Vec3f StelSkyDrawer::getPhotoAirglowExtinction() const
{
	// Blue is dimmed more than green, red less (Rayleigh scattering and aerosols); luminance weighted it is k
	const float k = extinction.getExtinctionCoefficient();
	return Vec3f(0.75f*k, k, 1.45f*k);
}

Vec3f StelSkyDrawer::getPhotoExtinctionTint(const Vec3d& altAzGeometric) const
{
	if (!photoExtinctionActive || photoExtinctionTints.size() < 2)
		return Vec3f(1.f);
	const double z = altAzGeometric[2] / altAzGeometric.norm();
	const double sinMin = static_cast<double>(photoExtinctionSinMin);
	const double u = std::sqrt(std::clamp((z - sinMin)/(1. - sinMin), 0., 1.));
	const double f = u * static_cast<double>(photoExtinctionTints.size() - 1);
	const size_t i = qMin(static_cast<size_t>(f), photoExtinctionTints.size() - 2);
	const float t = static_cast<float>(f - static_cast<double>(i));
	return photoExtinctionTints[i]*(1.f-t) + photoExtinctionTints[i+1]*t;
}

Vec3f StelSkyDrawer::photoTintedColor(const Vec3f& srgbColor, const Vec3f& linearTint)
{
	const float dim = qMax(srgbColor[0], qMax(srgbColor[1], srgbColor[2]));
	if (!(dim > 0.f))
		return srgbColor;
	const Vec3f lin = srgbToLinear(srgbColor/dim);
	Vec3f tinted(lin[0]*linearTint[0], lin[1]*linearTint[1], lin[2]*linearTint[2]);
	const float m = qMax(tinted[0], qMax(tinted[1], tinted[2]));
	if (!(m > 0.f))
		return srgbColor;
	tinted /= m;
	return Vec3f(encodeSRGB(tinted[0]), encodeSRGB(tinted[1]), encodeSRGB(tinted[2])) * dim;
}

QByteArray StelSkyDrawer::getPhotoExtinctionShader()
{
	return QByteArray(R"(
uniform sampler2D photoExtinctionTintTex;
uniform bool photoExtinctionColorEnabled;
uniform highp float photoExtinctionSinMin;
// Color of the atmospheric extinction (linear RGB at unit luminance), see StelSkyDrawer::getPhotoExtinctionTint()
highp vec3 photoExtinctionTint(highp float sinAltitude)
{
	if (!photoExtinctionColorEnabled)
		return vec3(1);
	highp float u = sqrt(clamp((sinAltitude - photoExtinctionSinMin)/(1. - photoExtinctionSinMin), 0., 1.));
	return texture2D(photoExtinctionTintTex, vec2((u*255. + 0.5)/256., 0.5)).rgb;
}
)");
}

QByteArray StelSkyDrawer::getPhotoSkyTrailShader()
{
	return QByteArray(R"(
uniform int photoSkyTrailSamples;        // number of samples (1: neither trail nor blur)
uniform highp vec3 photoSkyTrailAxis;    // rotation axis of the sky (model frame)
uniform highp float photoSkyTrailAngle;  // angle the sky turns during the exposure (radians), 0: no trail
uniform highp float photoSkyDefocus;     // signed radius of the blur disk of the lens (pixels), 0: sharp
uniform highp float photoSkyDefocusSA;   // spherical aberration: brightness change across the blur disk
uniform int photoSkyBlades;              // 0: round aperture
uniform highp float photoSkyStraightness;
uniform highp float photoSkyBladeRotation;
uniform highp float photoSkyLodBias;     // the samples lie apart: they read a correspondingly smoothed texture
// (texture2D is a macro with two arguments in Stellarium's shader prefix: the version with the bias)
#if __VERSION__ >= 130
#define PHOTO_SKY_TEXTURE(sampler, uv) texture(sampler, uv, photoSkyLodBias)
#else
#define PHOTO_SKY_TEXTURE(sampler, uv) texture2D(sampler, uv, photoSkyLodBias)
#endif
// Sample i of the exposure and of the blur disk of a defocused lens: the offset (window coordinates) of the
// point whose light the lens spreads onto this pixel and its weight (xyz), and the turn of the sky at its time (w)
highp vec4 photoSkySample(int i)
{
	highp float n = float(photoSkyTrailSamples);
	highp float fi = float(i) + 0.5;
	// Time along the exposure: in order for a pure trail, else well spread over the disk (golden ratio)
	highp float t = photoSkyDefocus != 0. ? fract(fi*0.6180339887) : fi/n;
	highp vec2 offset = vec2(0.);
	highp float w = 1.;
	if (photoSkyDefocus != 0.)
	{
		// A point of the image of the aperture (golden-angle spiral, even density)
		highp float rr = sqrt(fi/n);
		highp float a = float(i)*2.39996323;
		highp float scale = 1.;
		if (photoSkyBlades >= 3 && photoSkyStraightness > 0.)
		{
			highp float nb = float(photoSkyBlades);
			highp float sector = 6.28318530718/nb;
			highp float d = mod(a - photoSkyBladeRotation, sector) - 0.5*sector;
			scale = 1. / mix(1., cos(d)/cos(3.14159265359/nb), photoSkyStraightness);
		}
		offset = (photoSkyDefocus > 0. ? -1. : 1.) * abs(photoSkyDefocus) * rr * scale * vec2(cos(a), sin(a));
		w = max(0., 1. + photoSkyDefocusSA*(rr*rr - 0.5));
	}
	return vec4(offset, w, photoSkyTrailAngle*(t - 0.5));
}
// The direction dir turned by the angle a around the rotation axis of the sky
highp vec3 photoSkyTurn(highp vec3 dir, highp float a)
{
	highp float c = cos(a), s = sin(a);
	return dir*c + cross(photoSkyTrailAxis, dir)*s + photoSkyTrailAxis*(dot(photoSkyTrailAxis, dir)*(1. - c));
}
)");
}

void StelSkyDrawer::setPhotoSkyTrailUniforms(QOpenGLShaderProgram& program, const StelProjector::ModelViewTranformP& modelView, double texturePixelsPerRad) const
{
	const bool photo = getFlagPhotographicModeActive();
	const double pixelPerRad = static_cast<double>(core->getProjection(StelCore::FrameJ2000)->getPixelPerRadAtCenter());
	// Detail finer than half a texel is not in the texture: the samples need not be closer
	const double detail = qMin(1., 2.*texturePixelsPerRad/qMax(1e-9, pixelPerRad));
	// The trail: the longest (on the celestial equator) decides, about one sample per screen pixel
	double length = 0.;
	Vec3d axis(0., 0., 1.);
	if (photoTrailsActive && photo)
	{
		// The axis from the view frame to the frame of the texture (the inverse of the rotation; refraction
		// does not turn the sky)
		axis = modelView->getApproximateLinearTransfo().transpose().multiplyWithoutTranslation(photoTrailAxisView);
		if (axis.normSquared() > 0.)
		{
			axis.normalize();
			length = photoTrailAngle * pixelPerRad * detail;
		}
		else
			axis.set(0., 0., 1.);
	}
	// The blur disk of a defocused lens: about one sample per two square pixels (as for the nebula images)
	float defocus = photo ? photoLensCache.defocus : 0.f;
	const double R = std::fabs(static_cast<double>(defocus));
	const double Rdetail = R * detail;
	if (Rdetail < 0.5)
		defocus = 0.f;
	const double area = defocus != 0.f ? 0.5*M_PI*Rdetail*Rdetail : 0.;
	const int samples = qBound(1, static_cast<int>(std::ceil(length + area)), 128);
	if (samples <= 1)
		defocus = 0.f;
	// How much of the screen the samples cover, per sample: the texture is read smoothed to that size
	const double covered = (length > 0. ? photoTrailAngle*pixelPerRad*qMax(1., 2.*R) : 0.) + (defocus != 0.f ? M_PI*R*R : 0.);
	const float lodBias = samples > 1 ? static_cast<float>(qMax(0., 0.5*std::log2(qMax(1., covered/samples)))) : 0.f;
	const int blades = photoApertureBlades >= 3 ? photoApertureBlades : 0;
	const float sector = blades > 0 ? 2.f*M_PIf/static_cast<float>(blades) : 0.f;
	program.setUniformValue("photoSkyTrailSamples", samples);
	program.setUniformValue("photoSkyTrailAxis", QVector3D(static_cast<float>(axis[0]), static_cast<float>(axis[1]), static_cast<float>(axis[2])));
	program.setUniformValue("photoSkyTrailAngle", static_cast<float>(samples > 1 && length > 0. ? photoTrailAngle : 0.));
	program.setUniformValue("photoSkyDefocus", defocus);
	program.setUniformValue("photoSkyDefocusSA", defocus > 0.f ? -photoLensCache.sphericalAberration : photoLensCache.sphericalAberration);
	program.setUniformValue("photoSkyBlades", blades);
	program.setUniformValue("photoSkyStraightness", blades > 0 ? getPhotoApertureStraightness() : 0.f);
	program.setUniformValue("photoSkyBladeRotation", PHOTO_SPIKE_ANGLE - 0.5f*sector);
	program.setUniformValue("photoSkyLodBias", lodBias);
}

void StelSkyDrawer::setPhotoExtinctionUniforms(QOpenGLShaderProgram& program, int textureUnit)
{
	const bool enabled = photoExtinctionActive && photoExtinctionTints.size() == 256;
	program.setUniformValue("photoExtinctionColorEnabled", enabled);
	program.setUniformValue("photoExtinctionSinMin", photoExtinctionSinMin);
	program.setUniformValue("photoExtinctionTintTex", textureUnit);
	if (!enabled)
		return;
	auto& gl = *QOpenGLContext::currentContext()->functions();
	gl.glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(textureUnit));
	if (!photoExtinctionTexture)
	{
		GLuint tex = 0;
		gl.glGenTextures(1, &tex);
		photoExtinctionTexture = tex;
		photoExtinctionTextureDirty = true;
	}
	gl.glBindTexture(GL_TEXTURE_2D, photoExtinctionTexture);
	if (photoExtinctionTextureDirty)
	{
		std::vector<float> data(4*photoExtinctionTints.size());
		for (size_t i=0; i<photoExtinctionTints.size(); ++i)
		{
			data[4*i+0] = photoExtinctionTints[i][0];
			data[4*i+1] = photoExtinctionTints[i][1];
			data[4*i+2] = photoExtinctionTints[i][2];
			data[4*i+3] = 1.f;
		}
		gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, static_cast<GLsizei>(photoExtinctionTints.size()), 1, 0, GL_RGBA, GL_FLOAT, data.data());
		gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		photoExtinctionTextureDirty = false;
	}
	gl.glActiveTexture(GL_TEXTURE0);
}
