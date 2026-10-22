/*
 * Stellarium
 * Copyright (C) 2002-2022 Fabien Chereau and Stellarium contributors
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

const highp float pi = 3.1415926535897931;
const highp float ln10 = 2.3025850929940459;

uniform highp float alphaWaOverAlphaDa;
uniform highp float oneOverGamma;
uniform highp float term2TimesOneOverMaxdLpOneOverGamma;
uniform highp float term2TimesOneOverMaxdL;
uniform highp float brightnessScale; // Only the atmosphere fader value [0...1], i.e. drawn or not, or in transition.
uniform bool doSRGB;
uniform bool flagUseTmGamma;
// Photographic (camera-like) mode: the sky luminance is recorded linearly, like a camera sensor.
// photoScale converts luminance in cd/m^2 to the linear value of the scene buffer (1.0 = saturation),
// computed from the camera settings (exposure time, f-number, ISO). The exposure compensation
// is applied later, in the final camera pass.
uniform bool photoMode;
uniform highp float photoScale;
// Photographic mode: the airglow at the zenith above the atmosphere (linear RGB of the scene buffer, 0 = none),
// and its extinction per airmass in red, green and blue (magnitudes)
uniform highp vec3 photoAirglow;
uniform highp vec3 photoAirglowExtinction;
// Photographic mode with a color film: the sky from its spectrum (see StelSkyDrawer::getPhotoFilmSpec()): the linear
// sRGB of the scene from the luminance X, Y, Z and the scotopic W (rows), which the film's layers record as the film
// would record the spectrum of the sky
uniform bool photoFilmSky;
uniform highp vec4 photoFilmSkyR;
uniform highp vec4 photoFilmSkyG;
uniform highp vec4 photoFilmSkyB;

// The color film's record of the sky with luminances X, Y, Z, W (cd/m^2), as the linear color of the framebuffer
vec3 photoFilmSkyRGB(highp vec4 XYZW)
{
	highp vec4 cam = XYZW * photoScale;
	highp vec3 rgb = vec3(dot(photoFilmSkyR, cam), dot(photoFilmSkyG, cam), dot(photoFilmSkyB, cam));
	return linearColorToFramebuffer(max(rgb, vec3(0)) * brightnessScale);
}

// The scotopic luminance W of the sky from X, Y, Z, where W is not known (the lightweight atmosphere, made from
// ShowMySky): a quadratic in X/Y and Z/Y fitted to the X, Y, Z, W of ShowMySky over the whole sky (latitude 45,
// the Sun from +10 to -16 degrees), 2% rms. The film's colors still differ from ShowMySky's by about 7% (in red):
// X, Y and Z do not hold all that the film sees.
highp float photoSkyScotopicRatio(highp vec3 XYZ)
{
	if (!(XYZ.y > 0.))
		return 3.;
	highp float a = clamp(XYZ.x / XYZ.y, 0.85, 1.55), b = clamp(XYZ.z / XYZ.y, 0.05, 3.3);
	return clamp(0.71408 + 2.46953*a + 1.25660*b - 1.86276*a*a - 0.16285*a*b + 0.01493*b*b, 0.6, 10.);
}

// Photographic mode: the airglow towards a direction at the sine of the altitude sinElev (below the horizon: as
// above, like the sky). It shines from a layer about 90 km high: looking lower, the line of sight runs longer
// through it (van Rhijn), so it brightens towards the horizon until the extinction takes over, about 10 degrees
// above the horizon.
highp vec3 photoAirglowLight(highp float sinElev)
{
	if (!photoMode)
		return vec3(0.);
	const highp float r = 6371. / (6371. + 90.);
	highp float cosZ = clamp(abs(sinElev), 0., 1.);
	highp float vanRhijn = 1. / sqrt(max(1e-4, 1. - r*r*(1. - cosZ*cosZ)));
	highp float airmass = 1. / (cosZ + 0.025*exp(-11.*cosZ));
	return photoAirglow * vanRhijn * exp(-0.921034 * photoAirglowExtinction * airmass) * brightnessScale;
}

float applyTMAndGamma(highp float Y)
{
	if (flagUseTmGamma)
	{
		// Y = std::pow(adaptLuminanceScaled(Y), oneOverGamma);
		return pow(abs(Y*pi*1e-4), alphaWaOverAlphaDa*oneOverGamma) * term2TimesOneOverMaxdLpOneOverGamma;
	}
	else
	{
		// Y = adaptLuminanceScaled(Y);
		return pow(abs(Y*pi*1e-4), alphaWaOverAlphaDa) * term2TimesOneOverMaxdL;
	}
}


vec3 xyYToRGB(highp float x, highp float y, highp float Y)
{
	if (photoMode)
	{
		// A camera has no scotopic (night) vision and no adaptation: just linear light.
		if (photoFilmSky)
		{
			highp vec3 XYZ = (y > 0.) ? vec3(x * Y / y, Y, (1. - x - y) * Y / y) : vec3(0);
			return photoFilmSkyRGB(vec4(XYZ, Y * photoSkyScotopicRatio(XYZ)));
		}
		highp float Ycam = Y * photoScale;
		highp vec3 XYZcam = (y > 0.) ? vec3(x * Ycam / y, Ycam, (1. - x - y) * Ycam / y) : vec3(0);
		// The scene buffer holds linear sRGB, so the sRGB (D65) matrix is always the right one here.
		highp mat3 XYZ2sRGB = mat3(vec3( 3.2404542, -0.9692660,  0.0556434),
		                           vec3(-1.5371385,  1.8760108, -0.2040259),
		                           vec3(-0.4985314,  0.0415560,  1.0572252));
		return linearColorToFramebuffer(max(XYZ2sRGB * XYZcam, vec3(0)) * brightnessScale);
	}

	///////////////////////////////////////////////////////////////////////////
	// Now we have the xyY components, need to convert to RGB

	vec3 color = vec3(1,0,1);
	// 1. Hue conversion
	// if log10Y>0.6, photopic vision only (with the cones, colors are seen)
	// else scotopic vision if log10Y<-2 (with the rods, no colors, everything blue),
	// else mesopic vision (with rods and cones, transition state)
	if (Y <= 0.01)
	{
		// special case for s = 0 (x=0.25, y=0.25)
		Y *= 0.5121445;
		Y = applyTMAndGamma(Y);
		color = vec3(0.787077, 0.9898434, 1.9256125) * Y * brightnessScale;
	}
	else
	{
		if (Y<3.9810717055349722)
		{
			// Compute s, ratio between scotopic and photopic vision
			float op = (log(Y)/ln10 + 2.)/2.6;
			float s = op * op *(3. - 2. * op);
			// Do the blue shift for scotopic vision simulation (night vision) [3]
			// The "night blue" is x,y(0.25, 0.25)
			x = (1. - s) * 0.25 + s * x;	// Add scotopic + photopic components
			y = (1. - s) * 0.25 + s * y;	// Add scotopic + photopic components
			// Take into account the scotopic luminance approximated by V [3] [4]
			float V = Y * (1.33 * (1. + y / x + x * (1. - x - y)) - 1.68);
			Y = 0.4468 * (1. - s) * V + s * Y;
		}

		// 2. Adapt the luminance value and scale it to fit in the RGB range [2]
		Y = applyTMAndGamma(Y);

		// Convert from xyY to XYZ
		highp vec3 XYZ = vec3(x * Y / y, Y, (1. - x - y) * Y / y);

		highp mat3 XYZ2RGB;
		if(doSRGB)
		{
			// Use the XYZ to sRGB matrix which uses a D65 reference white
			// Ref: http://www.brucelindbloom.com/index.html?Eqn_RGB_XYZ_Matrix.html
			XYZ2RGB = mat3(vec3( 3.2404542, -0.9692660,  0.0556434),
			               vec3(-1.5371385,  1.8760108, -0.2040259),
			               vec3(-0.4985314,  0.0415560,  1.0572252));
		}
		else
		{
		        // Use a XYZ to Adobe RGB (1998) matrix which uses a D65 reference white. Use values from same source:
			XYZ2RGB = mat3(vec3( 2.0413690, -0.9692660,  0.0134474),
			               vec3(-0.5649464, +1.8760108, -0.1183897),
			               vec3(-0.3446944, +0.0415560, +1.0154096));
		}

		color = XYZ2RGB * XYZ * brightnessScale;
	}

	color = flagUseTmGamma ? color : pow(abs(color), vec3(oneOverGamma));
	// The tonemapper produced a value to be shown directly on the sRGB screen.
	// The color^(1/gamma) done above is not strictly what we should want to do,
	// because linearToSRGB is not a simple power function, but let's allow the
	// legacy tonemapper reproduce its colors until we get a better one.
	return srgbToFramebuffer(color);
}
