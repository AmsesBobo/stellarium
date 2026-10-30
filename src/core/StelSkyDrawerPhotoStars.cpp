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

// Photographic mode of StelSkyDrawer: the stars, planets and the Sun as the camera records them: their point spread
// function (seeing, diffraction spikes, halo, bloom), star trails, defocus, and the shaders that draw them.

#include "StelSkyDrawer.hpp"
#include "StelPhotoBloom.hpp"
#include "StelPhotoLensFlare.hpp"
#include "StelProjector.hpp"
#include "StelTextureMgr.hpp"
#include "StelApp.hpp"
#include "StelSRGB.hpp"
#include "StelCore.hpp"
#include "StelPainter.hpp"
#include "StelMainView.hpp"
#include "Planet.hpp"

#include <QOpenGLVertexArrayObject>
#include <QOpenGLShaderProgram>
#include <QVector2D>
#include <QOpenGLBuffer>
#include <QtGlobal>

Vec3f StelSkyDrawer::photoColorTable[128];

// For a Moffat profile M(r) with the given beta, normalized to unit integral, evaluated at the distance
// max(0, r-R) from the edge of a disk of radius R: its integral is 1 + term*R/alpha + (beta-1)*R^2/alpha^2,
// with term = (beta-1) sqrt(pi) Gamma(beta-1/2) / Gamma(beta). This returns that term.
static float photoDiskTerm(float beta)
{
	return static_cast<float>((beta-1.) * std::sqrt(M_PI) * std::tgamma(beta-0.5) / std::tgamma(static_cast<double>(beta)));
}

void StelSkyDrawer::initPhotoStarRendering()
{
	// The photographic mode needs high graphics mode (OpenGL 3.3 core or GLES 3): it relies on
	// floating-point buffers and instanced drawing.
	if (!StelMainView::getInstance().getGLInformation().isHighGraphicsMode)
		return;

	photoInstances.resize(maxPhotoPointSources);

	// Linear version of the B-V color table, normalized to unit luminance: the magnitude gives
	// the V-band light, which is close to luminance. Computed once instead of per star and frame.
	for (int i=0; i<128; ++i)
	{
		const Vec3f linear = srgbToLinear(colorTable[i]);
		const float Y = 0.2126f*linear[0] + 0.7152f*linear[1] + 0.0722f*linear[2];
		photoColorTable[i] = (Y > 0.f) ? linear/Y : Vec3f(1.f);
	}

	// The profile is evaluated per pixel. Everything is in floats: a single star may record
	// hundreds of times the saturation level, and faint stars need far finer steps than 8 bits.
	// A bright body with a resolved disk (Moon, Sun, planets when zoomed in) uses the same profile,
	// evaluated at the distance from the lit part of its disk (the limb, or the terminator for a body
	// in phases), and renormalized so that its integral stays the same (see shape and vNorm).
	const char* vsrc = R"(
ATTRIBUTE highp vec2 corner;  // per vertex: corner of the sprite, in [-1, 1]
ATTRIBUTE highp vec2 center;  // per star: window position (for a body: center of its disk)
ATTRIBUTE highp vec3 color;   // per star
ATTRIBUTE highp float radius; // per star
ATTRIBUTE highp vec4 shape;   // per star: x = disk radius (0 for stars), y = weight of the PSF core,
                              //  z = radius of a disk with the area of the lit part, w = cos(phase angle)
ATTRIBUTE highp vec2 sunDir;  // per star: direction towards the Sun on the screen (bodies in phases)
ATTRIBUTE highp vec4 trail;   // per star: offsets of the image at the start (xy) and at the end (zw) of the exposure
                              //  from its position at mid-exposure (diurnal motion), pixels; 0 = no trail
ATTRIBUTE highp vec3 trailStart; // per star: light at the start of the exposure relative to mid-exposure (extinction)
ATTRIBUTE highp vec3 trailEnd;   // per star: the same at the end
uniform highp mat4 projectionMatrix;
uniform highp float coreAlpha;
uniform highp float coreBeta;
uniform highp float haloAlpha;
uniform highp float haloBeta;
uniform highp float coreDiskTerm; // (beta-1) sqrt(pi) Gamma(beta-1/2) / Gamma(beta) of the core
uniform highp float haloDiskTerm; // the same for the halo
uniform highp float spikeSigma;
uniform highp float spikeLength;
VARYING highp vec2 vOffset;
VARYING highp vec3 vColor;
VARYING highp float vRadius;
VARYING highp vec4 vNorm;  // x: core weight x norm, y: halo norm, z: spike norm, w: core weight
VARYING highp vec4 vShape;
VARYING highp vec2 vSunDir;
VARYING highp vec3 vLens;  // lens model: x = distance from the image center / half diagonal, yz = radial direction
uniform highp vec2 lensCenter;
uniform highp float lensCorner;
uniform highp float defocusRadius; // radius of the blur disk of a defocused star (pixels), 0 = in focus
uniform highp float defocusArea;   // area of the image of the aperture, in units of its radius squared
uniform highp float lensPupil;     // optical vignetting (see StelSkyDrawer::getPhotoLens())
uniform highp float lensShift;
uniform highp float lensVignetting; // 1 = on
VARYING highp vec3 vDefocus; // x: 1 = defocused image, y: radius of the blur disk, z: 1 / its area (pixels)
VARYING highp vec4 vTrail;     // the trail (see the attribute)
VARYING highp vec3 vTrailInfo; // x: number of straight segments, y: approximate length (pixels), z: 1 = trailed
VARYING highp vec3 vTrailLogStart; // log of the extinction factors at the start and the end of the exposure
VARYING highp vec3 vTrailLogEnd;
const highp float PI_V = 3.14159265358979;
// Area of the intersection of two circles with radii r1, r2 and the distance d of their centers
highp float circleOverlap(highp float r1, highp float r2, highp float d)
{
	if (d >= r1 + r2) return 0.;
	if (d <= abs(r2 - r1)) return PI_V * min(r1, r2) * min(r1, r2);
	highp float a1 = r1*r1 * acos(clamp((d*d + r1*r1 - r2*r2) / (2.*d*r1), -1., 1.));
	highp float a2 = r2*r2 * acos(clamp((d*d + r2*r2 - r1*r1) / (2.*d*r2), -1., 1.));
	highp float k = (-d + r1 + r2) * (d + r1 - r2) * (d - r1 + r2) * (d + r1 + r2);
	return a1 + a2 - 0.5 * sqrt(max(k, 0.));
}
void main(void)
{
	// A star trail is the quadratic curve through the positions at the start (A), the middle (0) and the end (B)
	// of the exposure. The sprite is a box around it (along the chord and across), with the radius of the
	// stationary image added on all sides.
	vTrail = trail;
	vTrailLogStart = log(max(trailStart, vec3(1e-9)));
	vTrailLogEnd = log(max(trailEnd, vec3(1e-9)));
	highp vec2 A = trail.xy;
	highp vec2 B = trail.zw;
	highp float trailLength = length(A) + length(B);
	vTrailInfo = vec3(1., trailLength, 0.);
	if (trailLength > 0.5)
	{
		highp vec2 C = -0.5 * (A + B); // control point: the curve passes through 0 at its middle
		// Enough straight segments that they stay within a quarter pixel of the curve (its sagitta / K^2)
		highp float sagitta = length(0.5 * (A + B));
		vTrailInfo = vec3(clamp(ceil(sqrt(sagitta / 0.25)), 1., 16.), trailLength, 1.);
		highp vec2 chord = B - A;
		highp float chordLength = length(chord);
		highp vec2 e = chordLength > 1e-3 ? chord / chordLength : vec2(1., 0.);
		highp vec2 n = vec2(-e.y, e.x);
		highp vec3 pe = vec3(dot(A, e), dot(B, e), dot(C, e));
		highp vec3 pn = vec3(dot(A, n), dot(B, n), dot(C, n));
		highp float e0 = min(min(pe.x, pe.y), min(pe.z, 0.));
		highp float e1 = max(max(pe.x, pe.y), max(pe.z, 0.));
		highp float n0 = min(min(pn.x, pn.y), min(pn.z, 0.));
		highp float n1 = max(max(pn.x, pn.y), max(pn.z, 0.));
		vOffset = e * (0.5*(e0 + e1) + corner.x * (0.5*(e1 - e0) + radius))
		        + n * (0.5*(n0 + n1) + corner.y * (0.5*(n1 - n0) + radius));
	}
	else
		vOffset = corner * radius;
	highp vec2 fromCenter = center - lensCenter;
	highp float dist = length(fromCenter);
	vLens = vec3(dist / lensCorner, dist > 0. ? fromCenter / dist : vec2(1., 0.));
	// Out of focus, a star (or a body smaller than the blur) becomes an image of the aperture. Off axis the lens
	// barrel cuts it (the "cat's eye" of the optical vignetting): its light is spread over the smaller area,
	// the light lost there is the vignetting of the camera stage.
	highp float b = abs(defocusRadius);
	highp float Reff = 0.;
	bool defocused = b > 0. && shape.x < b;
	vDefocus = vec3(0.);
	if (defocused)
	{
		Reff = sqrt(b*b + shape.z*shape.z);
		highp float cut = 1.;
		if (lensVignetting > 0.5)
			cut = circleOverlap(lensPupil, 1., lensShift * vLens.x) / (PI_V * lensPupil * lensPupil);
		vDefocus = vec3(1., Reff, 1. / (max(cut, 1e-3) * defocusArea * Reff * Reff));
	}
	gl_Position = projectionMatrix * vec4(center + vOffset, 0, 1);
	vColor = color;
	vRadius = radius;
	vShape = shape;
	vSunDir = sunDir;
	// Integral of a radial profile evaluated at max(0, r-R), relative to the plain profile:
	// 1 + (disk term) R/alpha + (beta-1) R^2/alpha^2. The last term is the part inside the disk.
	// With this normalization the profile at the edge of the lit part is the light spread from a
	// disk of the same surface brightness (within a factor 2 of the exact half-plane value).
	// Inside the lit part that light stays on the disk (it only blurs it), so it is not drawn
	// there (see the fragment shader): full for an unresolved body, none for a large disk.
	// A defocused image: the halo and the spikes spread around its blur disk.
	// A trailed image is a point source moving along the trail (its halo is integrated along it, unit integral).
	bool trailed = trailLength > 0.5;
	highp float R = defocused ? Reff : shape.z;
	highp float Rs = defocused ? Reff : (trailed ? 0. : shape.z);
	highp float Rh = trailed ? 0. : R;
	highp float w = shape.y;
	highp float cn = 1. / (1. + coreDiskTerm*R/coreAlpha + (coreBeta-1.)*R*R/(coreAlpha*coreAlpha));
	highp float hn = 1. / (1. + haloDiskTerm*Rh/haloAlpha + (haloBeta-1.)*Rh*Rh/(haloAlpha*haloAlpha));
	// A spike becomes a band as wide as the lit part
	highp float s1 = sqrt(2.*PI_V)*spikeSigma;
	highp float s2 = PI_V*spikeLength;
	highp float sn = (s1*s2) / ((s1 + 2.*Rs) * (s2 + 2.*Rs));
	vNorm = vec4(w*cn, hn, sn, w);
}
)";
	const char* fsrc = R"(
VARYING highp vec2 vOffset;
VARYING highp vec3 vColor;
VARYING highp float vRadius;
VARYING highp vec4 vNorm;
VARYING highp vec4 vShape;
VARYING highp vec2 vSunDir;
VARYING highp vec3 vLens;
uniform highp float comaLength;   // lens: length of the coma in the corner (pixels), 0 = none
uniform highp float astigLength;  // lens: length of the astigmatic line in the corner (pixels), 0 = none
uniform bool chromaEnabled;       // lens: chromatic aberration
uniform highp vec3 chromaShift;   // radial shift of the red, green and blue images in the corner (pixels)
uniform highp vec3 chromaBlur;    // radius of their blur disks (pixels): axial chromatic aberration
uniform highp float coreAlpha;
uniform highp float coreBeta;
uniform highp float haloAlpha;
uniform highp float haloBeta;
uniform highp float haloFraction; // 0 when the halo is applied to the whole image (bloom)
uniform highp float coreFraction; // 1 - halo fraction - spike fraction
uniform int spikeLines;           // number of lines through the star (spikes/2), 0 = no spikes
uniform highp float spikeFraction;
uniform highp float spikeSigma;   // half-width of a spike (Gaussian sigma), pixels
uniform highp float spikeLength;  // length scale of the 1/r^2 falloff, pixels
uniform highp vec2 spikeDirs[9];  // directions of the lines (up to PHOTO_MAX_SPIKE_LINES)
uniform highp float visibleThreshold; // fainter contributions cannot change a pixel
uniform highp float spikeStart;   // with bloom: the spikes start here (the inner part comes from the image)
VARYING highp vec3 vDefocus;
VARYING highp vec4 vTrail;
VARYING highp vec3 vTrailInfo;
VARYING highp vec3 vTrailLogStart;
VARYING highp vec3 vTrailLogEnd;
uniform highp float defocusSign;  // -1: the image of the aperture is inverted (the rays crossed before the sensor)
uniform highp float defocusSA;    // spherical aberration: > 0 bright rim, < 0 bright center
uniform highp float defocusEdge;  // half width of the blurred edge of the disk (pixels)
uniform highp float coreFwhm;     // FWHM of the PSF core (pixels)
uniform int apertureBlades;       // 0 = round
uniform highp float apertureStraightness;
uniform highp float apertureRotation;
uniform highp float lensPupil;
uniform highp float lensShift;
uniform highp float lensVignetting;
const highp float PI = 3.14159265358979;
// Moffat profile normalized to unit integral over the plane
highp float moffat(highp float r2, highp float alpha, highp float beta)
{
	return (beta - 1.) / (PI * alpha * alpha) * pow(1. + r2 / (alpha * alpha), -beta);
}
// Anisotropic Moffat profile, unit integral: widths au along the radial direction, av across
highp float moffat2(highp float u, highp float v, highp float au, highp float av, highp float beta)
{
	return (beta - 1.) / (PI * au * av) * pow(1. + u*u/(au*au) + v*v/(av*av), -beta);
}
// The PSF core of a star away from the image center, with the off-axis aberrations of the lens.
// Coma: the image of a point is a family of circles whose radius grows like t^2 and whose centers move
// outwards by twice that (a comet with its tail away from the center); here a few blurred blobs along it.
// Astigmatism: the image is drawn out into a short line, across the radial direction (tangential focus).
highp float aberratedCore(highp vec2 offset, highp float alpha)
{
	highp float rho = vLens.x;
	highp vec2 er = vLens.yz;
	highp float u = dot(offset, er);
	highp float v = dot(offset, vec2(-er.y, er.x));
	highp float coma = comaLength * rho;
	highp float astig = astigLength * rho * rho;
	highp float a2 = alpha * alpha;
	// a line of length L convolved with the core: about the width of a uniform segment, L^2/12, added in quadrature
	highp float av2 = a2 + astig * astig / 3.;
	const int K = 6;
	highp float sum = 0.;
	for (int k = 0; k < K; ++k)
	{
		highp float t = (float(k) + 0.5) / float(K);
		highp float ring = coma * t * t;       // radius of this comatic circle
		highp float au = sqrt(a2 + ring*ring);
		highp float av = sqrt(av2 + ring*ring);
		sum += moffat2(u - 2.*ring, v, au, av, coreBeta);
	}
	return sum / float(K);
}
// The Moffat profiles (beta 3: core, beta 1.5: halo) of a point source moving uniformly along a straight
// segment of length L (u: along the segment from its start, v: across), i.e. integrated along it. Exact; unit integral.
highp float segmentG3(highp float x, highp float A)
{
	highp float s = A + x*x;
	highp float rA = sqrt(A);
	return x / (4.*A*s*s) + 3.*x / (8.*A*A*s) + 3. / (8.*A*A*rA) * atan(x / rA);
}
highp float moffatSegment3(highp float u, highp float v, highp float L, highp float alpha)
{
	if (L < 1e-3 * alpha)
		return moffat(u*u + v*v, alpha, 3.);
	highp float a2 = alpha * alpha;
	highp float A = a2 + v*v;
	return 2.*a2*a2 / (PI*L) * (segmentG3(u, A) - segmentG3(u - L, A));
}
highp float moffatSegment15(highp float u, highp float v, highp float L, highp float alpha)
{
	if (L < 1e-3 * alpha)
		return moffat(u*u + v*v, alpha, 1.5);
	highp float A = alpha*alpha + v*v;
	highp float w = u - L;
	return 0.5*alpha / (PI*L) * (u / (A*sqrt(A + u*u)) - w / (A*sqrt(A + w*w)));
}
// A defocused point source: the image of the aperture (disk, or polygon of the blades) of radius R,
// cut by the lens barrel off axis (cat's eye), with the edge blurred by the PSF core and the aberrations.
// Unit integral (vDefocus.z is 1 / area).
highp float defocusDisk(highp vec2 p, highp float R)
{
	highp vec2 q = defocusSign * p / R;
	highp float r = length(q);
	highp float shape = r;
	if (apertureBlades >= 3 && apertureStraightness > 0.)
	{
		highp float n = float(apertureBlades);
		highp float sector = 2.*PI/n;
		highp float d = mod(atan(q.y, q.x) - apertureRotation, sector) - 0.5*sector; // angle from the normal of the nearest edge
		shape = mix(r, r * cos(d) / cos(PI/n), apertureStraightness);
	}
	highp float sd = shape - 1.;
	if (lensVignetting > 0.5)
	{
		// the lens barrel: radius 1/pupil, shifted with the distance from the image center
		highp vec2 barrel = -vLens.yz * (lensShift * vLens.x / lensPupil);
		sd = max(sd, length(q - barrel) - 1./lensPupil);
	}
	highp float rho = vLens.x;
	highp float coma = 0.5 * comaLength * rho;
	highp float astig = 0.5 * astigLength * rho * rho;
	highp float edge = sqrt(defocusEdge*defocusEdge + coma*coma + astig*astig);
	highp float coverage = 1. - smoothstep(-edge, edge, sd * R);
	// Spherical aberration: the zones of the lens focus at slightly different distances, so the light is
	// not spread evenly; on one side of the focus the rim is bright, on the other the center
	highp float sa = max(0., 1. + defocusSA * (r*r - 0.5));
	return coverage * sa * vDefocus.z;
}
// The core of one color with the chromatic aberration: its image shifted along the radial direction, and a small
// blur disk that widens it (as a small defocus widens the core, see main())
highp vec2 chromaOffset(highp float shift)
{
	return vOffset - vLens.yz * (shift * vLens.x);
}
highp float chromaAlpha(highp float blur)
{
	highp float k = 1.6 * blur / coreFwhm;
	return coreAlpha * sqrt(1. + k*k);
}
void main(void)
{
	highp float r = length(vOffset);
	// Beyond the radius of the sprite the fade below makes the pixel black: skip the work (the trailed image
	// measures its fade from the trail, see below). The stars are added up, so a skipped pixel adds nothing.
	bool trailed = vTrailInfo.z > 0.5;
	if (!trailed && r >= vRadius)
		discard;
	highp float Rd = vShape.x;     // disk radius
	highp float Rl = vShape.z;     // radius of a disk with the area of the lit part
	highp float cosPhase = vShape.w;
	// Coordinates along the direction to the Sun (u) and across it (v)
	highp float u = dot(vOffset, vSunDir);
	highp float v = dot(vOffset, vec2(-vSunDir.y, vSunDir.x));
	// Distance from the lit part of the disk (stars: from the center). The terminator is the half
	// ellipse u = -cos(phase) sqrt(Rd^2 - v^2); the lit part is on the sunward side of it.
	highp float d;
	if (Rd <= 0.)
		d = r;
	else if (r <= Rd)
	{
		highp float ut = -cosPhase * sqrt(max(0., Rd*Rd - v*v));
		d = max(0., ut - u);
	}
	else if (u >= -cosPhase * r)
		d = r - Rd;  // the nearest point of the limb is lit
	else
	{
		// beyond the dark limb: distance to the nearer cusp
		highp vec2 cusp = vec2(-cosPhase*Rd, sqrt(max(0., 1. - cosPhase*cosPhase))*Rd);
		d = min(distance(vec2(u, v), cusp), distance(vec2(u, v), vec2(cusp.x, -cusp.y)));
	}
	highp float r2 = d * d;
	highp float brightness = max(vColor.r, max(vColor.g, vColor.b));
	highp float profile = 0.;
	highp vec3 chroma = vec3(0.); // change of the core in each color (chromatic aberration of the lens)
	// The colored fringes show only where the change of the core makes a visible color difference (a few
	// steps of the screen): bright stars. The others (most) skip it. The change is about (shift + blur)/alpha
	// of the peak of the core.
	bool showChroma = false;
	if (chromaEnabled)
	{
		highp float peak = brightness * coreFraction * vNorm.x * (coreBeta - 1.) / (PI * coreAlpha * coreAlpha);
		highp float change = min(1., (max(abs(chromaShift.r), abs(chromaShift.b)) * vLens.x + max(chromaBlur.r, chromaBlur.b)) / coreAlpha);
		showChroma = peak * change > 4. * visibleThreshold;
	}
	// Where the halo and the spikes start: the lit part, or the blur disk of a defocused image
	highp vec2 spikeP = vOffset - vSunDir * (Rd - Rl*Rl/max(Rd, 1e-6)); // from the photocenter of the lit part
	highp float spikeR = Rl;
	// The halo and the spikes matter only for bright stars: skip them where even their
	// peak would stay below what can show on the screen (most stars)
	bool showHalo = brightness * haloFraction * vNorm.y * (haloBeta - 1.) / (PI * haloAlpha * haloAlpha) > visibleThreshold;
	highp float haloProfile = -1.; // < 0: the halo of the stationary image
	highp vec3 trailTint = vec3(1.); // the extinction along the trail
	highp float fadeDistance = r;
	highp float trailLength = 0.;
	highp vec2 trailNormal = vec2(0.);
	// Star trail: during the exposure the image moves along an arc around the celestial pole. The trailed image
	// is the stationary one integrated along the arc: straight segments of equal exposure time, each integrated exactly.
	// It is computed first: a long trail does not need the stationary image at all (tau = 1 below).
	highp float tau = 0.;
	highp float coreT = 0.;
	highp float haloT = 0.;
	highp float dmin = 1e30;
	highp vec2 nearest = vec2(0.);
	highp float tNearest = 0.5;
	highp float Rdf = vDefocus.x > 0.5 ? vDefocus.y : 0.;
	if (trailed)
	{
		highp vec2 A = vTrail.xy;
		highp vec2 B = vTrail.zw;
		highp vec2 C = -0.5 * (A + B);
		highp vec2 p = spikeP; // from the photocenter
		int K = int(vTrailInfo.x + 0.5);
		highp float invK = 1. / float(K);
		// The distance from the trail first: beyond the radius of the sprite the pixel is black (see the fade)
		highp vec2 P0 = A;
		for (int k = 1; k <= 16; ++k)
		{
			if (k > K) break;
			highp float t = float(k) * invK;
			highp vec2 P1 = (1.-t)*(1.-t)*A + 2.*t*(1.-t)*C + t*t*B;
			highp vec2 seg = P1 - P0;
			highp float L = length(seg);
			highp vec2 e = L > 1e-4 ? seg / L : vec2(1., 0.);
			highp vec2 q = p - P0;
			dmin = min(dmin, length(q - e*clamp(dot(q, e), 0., L)));
			P0 = P1;
		}
		if (dmin >= vRadius)
			discard;
		dmin = 1e30;
		// a small blur disk only widens the core, as for the stationary image
		highp float kd = 1.6 * Rdf / coreFwhm;
		highp float alphaCore = coreAlpha * sqrt(1. + kd*kd);
		highp float alphaHalo = sqrt(haloAlpha*haloAlpha + 0.5*Rdf*Rdf);
		P0 = A;
		for (int k = 1; k <= 16; ++k)
		{
			if (k > K) break;
			highp float t = float(k) * invK;
			highp vec2 P1 = (1.-t)*(1.-t)*A + 2.*t*(1.-t)*C + t*t*B;
			highp vec2 seg = P1 - P0;
			highp float L = length(seg);
			highp vec2 e = L > 1e-4 ? seg / L : vec2(1., 0.);
			highp vec2 q = p - P0;
			highp float u = dot(q, e);
			highp float v = dot(q, vec2(-e.y, e.x));
			coreT += moffatSegment3(u, v, L, alphaCore);
			if (showHalo)
				haloT += moffatSegment15(u, v, L, alphaHalo);
			highp float uc = clamp(u, 0., L);
			highp float dd = length(q - e*uc);
			if (dd < dmin)
			{
				dmin = dd;
				nearest = P0 + e*uc;
				tNearest = (float(k - 1) + (L > 0. ? uc / L : 0.)) * invK;
			}
			trailLength += L;
			P0 = P1;
		}
		coreT *= invK;
		haloT *= invK;
		if (Rdf > 0.)
		{
			// Out of focus: the blur disk swept along the trail, of even brightness
			highp float tD = smoothstep(0.5, 2.0, Rdf / coreFwhm);
			highp float coverage = 1. - smoothstep(-defocusEdge, defocusEdge, dmin - Rdf);
			coreT = mix(coreT, coverage / (PI*Rdf*Rdf + 2.*Rdf*trailLength), tD);
		}
		// A short trail keeps the details of the stationary image (aberrations, the shape of the blur disk)
		tau = smoothstep(0.5, 2.0, trailLength / max(coreFwhm, 2.*Rdf));
	}
	// The stationary image (not needed under a long trail)
	if (tau < 1.)
	{
		if (vDefocus.x > 0.5)
		{
			highp float Reff = vDefocus.y;
			highp float rp = length(spikeP);
			// A blur disk smaller than the PSF core only widens the core
			highp float t = smoothstep(0.5, 2.0, Reff / coreFwhm);
			highp float core = 0.;
			if (t < 1.)
			{
				highp float k = 1.6 * Reff / coreFwhm;
				highp float alphaWide = coreAlpha * sqrt(1. + k*k);
				core += (1. - t) * moffat(rp*rp, alphaWide, coreBeta);
			}
			if (t > 0.)
				core += t * defocusDisk(spikeP, Reff);
			profile = coreFraction * vNorm.w * core;
			highp float dh = max(0., rp - Reff);
			r2 = dh * dh;
			spikeR = Reff;
		}
		else if (Rd <= 0. && (comaLength > 0. || astigLength > 0.) && vLens.x > 0.05)
		{
			highp float base = aberratedCore(vOffset, coreAlpha);
			profile = coreFraction * vNorm.x * base;
			// Chromatic aberration of a star: the red and the blue core differ from the green one (green is the reference)
			if (showChroma)
				chroma = coreFraction * vNorm.x * vec3(aberratedCore(chromaOffset(chromaShift.r), chromaAlpha(chromaBlur.r)) - base, 0.,
				                                       aberratedCore(chromaOffset(chromaShift.b), chromaAlpha(chromaBlur.b)) - base);
		}
		else
		{
			highp float base = moffat(r2, coreAlpha, coreBeta);
			profile = coreFraction * vNorm.x * base;
			if (showChroma && Rd <= 0.)
			{
				highp vec2 oR = chromaOffset(chromaShift.r);
				highp vec2 oB = chromaOffset(chromaShift.b);
				chroma = coreFraction * vNorm.x * vec3(moffat(dot(oR, oR), chromaAlpha(chromaBlur.r), coreBeta) - base, 0.,
				                                       moffat(dot(oB, oB), chromaAlpha(chromaBlur.b), coreBeta) - base);
			}
		}
	}
	if (trailed)
	{
		profile = mix(profile, coreFraction * vNorm.w * coreT, tau);
		chroma *= 1. - tau; // (a trail smears the colored fringes)
		// (the whole trailed image takes the tint of its nearest point, see trailTint below)
		haloProfile = haloT;
		// Extinction along the trail: the light at this point of the trail (mostly from the nearest part), from
		// the factors at the start (t = 0), the middle (1) and the end; interpolated in the log (magnitudes)
		highp float tn = tNearest;
		trailTint = exp(vTrailLogStart * ((1.-tn)*(1.-2.*tn)) + vTrailLogEnd * (tn*(2.*tn-1.)));
		// the spikes move with the image: they start from the trail (see below)
		spikeP = spikeP - nearest;
		spikeR = Rdf;
		fadeDistance = dmin;
		highp vec2 chord = vTrail.zw - vTrail.xy;
		highp float chordLength = length(chord);
		trailNormal = chordLength > 1e-3 ? vec2(-chord.y, chord.x) / chordLength : vec2(0.);
	}
	if (showHalo)
		profile += haloFraction * vNorm.y * (haloProfile >= 0. ? haloProfile : moffat(r2, haloAlpha, haloBeta));
	// Diffraction spikes from straight edges in the aperture: thin lines whose intensity
	// falls off like 1/r^2 along them. Each line is normalized to unit integral.
	if (spikeLines > 0)
	{
		highp float lineNorm = vNorm.z * spikeFraction / (float(spikeLines) * sqrt(2.*PI) * spikeSigma * PI * spikeLength);
		if (brightness * lineNorm > visibleThreshold)
		{
			// Spikes start from the lit part (or the blur disk): around its photocenter, as wide as it
			highp vec2 p = spikeP;
			for (int k = 0; k < 9; ++k)
			{
				if (k >= spikeLines) break;
				highp vec2 dir = spikeDirs[k];
				highp float acrossC = abs(dot(p, vec2(-dir.y, dir.x)));
				highp float along = max(0., abs(dot(p, dir)) - sqrt(max(0., spikeR*spikeR - acrossC*acrossC)));
				highp float across = max(0., acrossC - spikeR);
				highp float line = lineNorm * exp(-0.5 * across * across / (spikeSigma * spikeSigma))
				                            / (1. + along * along / (spikeLength * spikeLength));
				if (spikeStart > 0.)
					line *= smoothstep(spikeStart - 0.5, spikeStart + 0.5, along);
				// Trailed: a spike across the trail is swept over it, its light spread over the trail length
				if (trailLength > 0.)
				{
					highp float width = sqrt(2.*PI) * spikeSigma + 2.*spikeR;
					line *= width / (width + trailLength * abs(dot(dir, trailNormal)));
				}
				profile += line;
			}
		}
	}
	// On the lit part (d = 0) the halo and spikes keep their peak value: the light the lens spreads
	// stays on the disk there (the disk itself is drawn with the remaining fraction), and it fills
	// small dark details such as crater shadows, as on a real overexposed photo.
	// Fade the outermost part of the sprite, so that its edge never shows
	highp float fade = 1. - smoothstep(Rd + 0.75 * (vRadius - Rd), vRadius, fadeDistance);
	FRAG_COLOR = vec4(min(max(vColor * trailTint * (vec3(profile) + chroma) * fade, vec3(0.)), vec3(1e30)), 1.); // guard against overflow
}
)";

	photoStarShaderProgram.reset(new QOpenGLShaderProgram);
	photoStarShaderProgram->addShaderFromSourceCode(QOpenGLShader::Vertex, StelOpenGL::globalShaderPrefix(StelOpenGL::VERTEX_SHADER) + vsrc);
	photoStarShaderProgram->addShaderFromSourceCode(QOpenGLShader::Fragment, StelOpenGL::globalShaderPrefix(StelOpenGL::FRAGMENT_SHADER) + fsrc);
	if (!StelPainter::linkProg(photoStarShaderProgram.get(), "photographic star shader"))
	{
		photoStarShaderProgram.reset();
		return;
	}
	auto& prog = *photoStarShaderProgram;
	photoStarShaderVars.projectionMatrix = prog.uniformLocation("projectionMatrix");
	photoStarShaderVars.corner = prog.attributeLocation("corner");
	photoStarShaderVars.center = prog.attributeLocation("center");
	photoStarShaderVars.color = prog.attributeLocation("color");
	photoStarShaderVars.radius = prog.attributeLocation("radius");
	photoStarShaderVars.shape = prog.attributeLocation("shape");
	photoStarShaderVars.sunDir = prog.attributeLocation("sunDir");
	photoStarShaderVars.trail = prog.attributeLocation("trail");
	photoStarShaderVars.trailStart = prog.attributeLocation("trailStart");
	photoStarShaderVars.trailEnd = prog.attributeLocation("trailEnd");
	photoStarShaderVars.coreAlpha = prog.uniformLocation("coreAlpha");
	photoStarShaderVars.coreBeta = prog.uniformLocation("coreBeta");
	photoStarShaderVars.haloAlpha = prog.uniformLocation("haloAlpha");
	photoStarShaderVars.haloBeta = prog.uniformLocation("haloBeta");
	photoStarShaderVars.haloFraction = prog.uniformLocation("haloFraction");
	photoStarShaderVars.coreFraction = prog.uniformLocation("coreFraction");
	photoStarShaderVars.coreDiskTerm = prog.uniformLocation("coreDiskTerm");
	photoStarShaderVars.haloDiskTerm = prog.uniformLocation("haloDiskTerm");
	photoStarShaderVars.spikeLines = prog.uniformLocation("spikeLines");
	photoStarShaderVars.spikeFraction = prog.uniformLocation("spikeFraction");
	photoStarShaderVars.spikeSigma = prog.uniformLocation("spikeSigma");
	photoStarShaderVars.spikeLength = prog.uniformLocation("spikeLength");
	photoStarShaderVars.spikeDirs = prog.uniformLocation("spikeDirs");
	photoStarShaderVars.visibleThreshold = prog.uniformLocation("visibleThreshold");
	photoStarShaderVars.spikeStart = prog.uniformLocation("spikeStart");
	photoStarShaderVars.lensCenter = prog.uniformLocation("lensCenter");
	photoStarShaderVars.lensCorner = prog.uniformLocation("lensCorner");
	photoStarShaderVars.comaLength = prog.uniformLocation("comaLength");
	photoStarShaderVars.astigLength = prog.uniformLocation("astigLength");
	photoStarShaderVars.defocusRadius = prog.uniformLocation("defocusRadius");
	photoStarShaderVars.defocusArea = prog.uniformLocation("defocusArea");
	photoStarShaderVars.defocusSign = prog.uniformLocation("defocusSign");
	photoStarShaderVars.defocusSA = prog.uniformLocation("defocusSA");
	photoStarShaderVars.defocusEdge = prog.uniformLocation("defocusEdge");
	photoStarShaderVars.coreFwhm = prog.uniformLocation("coreFwhm");
	photoStarShaderVars.apertureBlades = prog.uniformLocation("apertureBlades");
	photoStarShaderVars.apertureStraightness = prog.uniformLocation("apertureStraightness");
	photoStarShaderVars.apertureRotation = prog.uniformLocation("apertureRotation");
	photoStarShaderVars.lensPupil = prog.uniformLocation("lensPupil");
	photoStarShaderVars.lensShift = prog.uniformLocation("lensShift");
	photoStarShaderVars.lensVignetting = prog.uniformLocation("lensVignetting");

	const GLfloat corners[] = {-1,-1,  1,-1,  1,1,  -1,-1,  1,1,  -1,1};
	photoCornerVbo.reset(new QOpenGLBuffer(QOpenGLBuffer::VertexBuffer));
	photoCornerVbo->create();
	photoCornerVbo->bind();
	photoCornerVbo->allocate(corners, sizeof(corners));
	photoCornerVbo->release();

	photoVbo.reset(new QOpenGLBuffer(QOpenGLBuffer::VertexBuffer));
	photoVbo->create();
	photoVbo->setUsagePattern(QOpenGLBuffer::StreamDraw);
	photoVbo->bind();
	photoVbo->allocate(static_cast<int>(maxPhotoPointSources*sizeof(PhotoStarInstance)));
	photoVbo->release();

	photoVao.reset(new QOpenGLVertexArrayObject);
	photoVao->create();
	photoVao->bind();
	auto* hg = StelOpenGL::highGraphicsFunctions();
	photoCornerVbo->bind();
	prog.setAttributeBuffer(photoStarShaderVars.corner, GL_FLOAT, 0, 2, 0);
	prog.enableAttributeArray(photoStarShaderVars.corner);
	photoVbo->bind();
	prog.setAttributeBuffer(photoStarShaderVars.center, GL_FLOAT, offsetof(PhotoStarInstance, center), 2, sizeof(PhotoStarInstance));
	prog.setAttributeBuffer(photoStarShaderVars.color,  GL_FLOAT, offsetof(PhotoStarInstance, color),  3, sizeof(PhotoStarInstance));
	prog.setAttributeBuffer(photoStarShaderVars.radius, GL_FLOAT, offsetof(PhotoStarInstance, radius), 1, sizeof(PhotoStarInstance));
	prog.setAttributeBuffer(photoStarShaderVars.shape,  GL_FLOAT, offsetof(PhotoStarInstance, shape),  4, sizeof(PhotoStarInstance));
	prog.setAttributeBuffer(photoStarShaderVars.sunDir, GL_FLOAT, offsetof(PhotoStarInstance, sunDir), 2, sizeof(PhotoStarInstance));
	prog.setAttributeBuffer(photoStarShaderVars.trail,  GL_FLOAT, offsetof(PhotoStarInstance, trail),  4, sizeof(PhotoStarInstance));
	prog.setAttributeBuffer(photoStarShaderVars.trailStart, GL_FLOAT, offsetof(PhotoStarInstance, trailStart), 3, sizeof(PhotoStarInstance));
	prog.setAttributeBuffer(photoStarShaderVars.trailEnd,   GL_FLOAT, offsetof(PhotoStarInstance, trailEnd),   3, sizeof(PhotoStarInstance));
	prog.enableAttributeArray(photoStarShaderVars.center);
	prog.enableAttributeArray(photoStarShaderVars.color);
	prog.enableAttributeArray(photoStarShaderVars.radius);
	prog.enableAttributeArray(photoStarShaderVars.shape);
	prog.enableAttributeArray(photoStarShaderVars.sunDir);
	prog.enableAttributeArray(photoStarShaderVars.trail);
	prog.enableAttributeArray(photoStarShaderVars.trailStart);
	prog.enableAttributeArray(photoStarShaderVars.trailEnd);
	// One record per star (instance), not per vertex
	hg->glVertexAttribDivisor(static_cast<GLuint>(photoStarShaderVars.center), 1);
	hg->glVertexAttribDivisor(static_cast<GLuint>(photoStarShaderVars.color), 1);
	hg->glVertexAttribDivisor(static_cast<GLuint>(photoStarShaderVars.radius), 1);
	hg->glVertexAttribDivisor(static_cast<GLuint>(photoStarShaderVars.shape), 1);
	hg->glVertexAttribDivisor(static_cast<GLuint>(photoStarShaderVars.sunDir), 1);
	hg->glVertexAttribDivisor(static_cast<GLuint>(photoStarShaderVars.trail), 1);
	hg->glVertexAttribDivisor(static_cast<GLuint>(photoStarShaderVars.trailStart), 1);
	hg->glVertexAttribDivisor(static_cast<GLuint>(photoStarShaderVars.trailEnd), 1);
	photoVao->release();
	photoVbo->release();
}

void StelSkyDrawer::flushPhotoPointSources(StelPainter* sPainter, PhotoBatchMode mode)
{
	if (nbPhotoPointSources==0)
		return;
	if (!photoStarShaderProgram || !photoVao)
	{
		nbPhotoPointSources = 0;
		return;
	}

	sPainter->setBlending(true, GL_ONE, GL_ONE);
	const QMatrix4x4 qMat=sPainter->getProjector()->getProjectionMatrix().toQMatrix();

	photoVbo->bind();
	// Re-specify the whole buffer ("orphaning"): the driver gives us fresh memory instead of
	// waiting until the GPU has finished drawing the previous batch from it.
	photoVbo->allocate(photoInstances.data(), static_cast<int>(nbPhotoPointSources*sizeof(PhotoStarInstance)));
	photoVbo->release();

	auto& prog = *photoStarShaderProgram;
	prog.bind();
	prog.setUniformValue(photoStarShaderVars.projectionMatrix, qMat);
	prog.setUniformValue(photoStarShaderVars.coreAlpha, photoCoreAlpha);
	prog.setUniformValue(photoStarShaderVars.coreBeta, PHOTO_CORE_BETA);
	prog.setUniformValue(photoStarShaderVars.haloAlpha, photoCoreAlpha*PHOTO_HALO_WIDTH_FACTOR);
	prog.setUniformValue(photoStarShaderVars.haloBeta, PHOTO_HALO_BETA);
	prog.setUniformValue(photoStarShaderVars.haloFraction, (getFlagPhotoBloomActive() || mode == PhotoBatchCoreOnly) ? 0.f : photoHaloFraction);
	prog.setUniformValue(photoStarShaderVars.coreFraction, getPhotoCoreFraction());
	prog.setUniformValue(photoStarShaderVars.coreDiskTerm, photoDiskTerm(PHOTO_CORE_BETA));
	prog.setUniformValue(photoStarShaderVars.haloDiskTerm, photoDiskTerm(PHOTO_HALO_BETA));
	// With bloom, the halo and the inner part of the spikes come from the whole image (StelPhotoBloom)
	const int spikeLines = (getPhotoSpikeFractionEffective() > 0.f && mode != PhotoBatchCoreOnly) ? getPhotoSpikeLines() : 0;
	prog.setUniformValue(photoStarShaderVars.spikeLines, spikeLines);
	prog.setUniformValue(photoStarShaderVars.spikeFraction, spikeLines > 0 ? getPhotoSpikeFractionEffective() : 0.f);
	prog.setUniformValue(photoStarShaderVars.spikeSigma, photoCoreAlpha*PHOTO_SPIKE_WIDTH_FACTOR);
	prog.setUniformValue(photoStarShaderVars.spikeLength, getPhotoSpikeLength());
	QVector2D spikeDirs[PHOTO_MAX_SPIKE_LINES];
	for (int k=0; k<PHOTO_MAX_SPIKE_LINES; ++k)
	{
		const float a = PHOTO_SPIKE_ANGLE + (spikeLines > 0 ? static_cast<float>(k)*M_PIf/static_cast<float>(spikeLines) : 0.f);
		spikeDirs[k] = QVector2D(std::cos(a), std::sin(a));
	}
	prog.setUniformValueArray(photoStarShaderVars.spikeDirs, spikeDirs, PHOTO_MAX_SPIKE_LINES);
	prog.setUniformValue(photoStarShaderVars.visibleThreshold, photoVisibleThreshold);
	prog.setUniformValue(photoStarShaderVars.spikeStart, getFlagPhotoBloomActive() ? StelPhotoBloom::NEAR_SPIKE_LENGTH : 0.f);
	{
		const PhotoLens lens = getPhotoLens(*sPainter->getProjector());
		// The sprites are positioned in window coordinates (with the viewport offset)
		prog.setUniformValue(photoStarShaderVars.lensCenter, QVector2D(lens.center[0] + static_cast<float>(sPainter->getProjector()->getViewportPosX()),
		                                                                lens.center[1] + static_cast<float>(sPainter->getProjector()->getViewportPosY())));
		prog.setUniformValue(photoStarShaderVars.lensCorner, lens.corner);
		prog.setUniformValue(photoStarShaderVars.comaLength, lens.coma);
		prog.setUniformValue(photoStarShaderVars.astigLength, lens.astigmatism);
		prog.setUniformValue("chromaEnabled", lens.chromaShift[0] != 0.f || lens.chromaBlur[2] != 0.f);
		prog.setUniformValue("chromaShift", QVector3D(lens.chromaShift[0], lens.chromaShift[1], lens.chromaShift[2]));
		prog.setUniformValue("chromaBlur", QVector3D(lens.chromaBlur[0], lens.chromaBlur[1], lens.chromaBlur[2]));
		// Defocus: the image of the aperture. Focused nearer than infinity, the rays of a star cross in front
		// of the sensor: the image is inverted, and (spherical aberration of a typical, undercorrected lens)
		// soft-edged; beyond infinity it is upright with a bright rim.
		prog.setUniformValue(photoStarShaderVars.defocusRadius, lens.defocus);
		prog.setUniformValue(photoStarShaderVars.defocusArea, getPhotoApertureShapeArea());
		prog.setUniformValue(photoStarShaderVars.defocusSign, lens.defocus > 0.f ? -1.f : 1.f);
		prog.setUniformValue(photoStarShaderVars.defocusSA, lens.defocus > 0.f ? -lens.sphericalAberration : lens.sphericalAberration);
		prog.setUniformValue(photoStarShaderVars.defocusEdge, photoDefocusEdge());
		prog.setUniformValue(photoStarShaderVars.coreFwhm, photoCoreFwhm());
		const int blades = photoApertureBlades >= 3 ? photoApertureBlades : 0;
		const float sector = blades > 0 ? 2.f*M_PIf/static_cast<float>(blades) : 0.f;
		prog.setUniformValue(photoStarShaderVars.apertureBlades, blades);
		prog.setUniformValue(photoStarShaderVars.apertureStraightness, blades > 0 ? getPhotoApertureStraightness() : 0.f);
		// the same orientation as the spikes and the lens reflections: one blade edge normal at PHOTO_SPIKE_ANGLE
		prog.setUniformValue(photoStarShaderVars.apertureRotation, PHOTO_SPIKE_ANGLE - 0.5f*sector);
		prog.setUniformValue(photoStarShaderVars.lensPupil, lens.pupil);
		prog.setUniformValue(photoStarShaderVars.lensShift, PHOTO_VIGNETTING_SHIFT);
		prog.setUniformValue(photoStarShaderVars.lensVignetting, lens.vignetting ? 1.f : 0.f);
	}

	photoVao->bind();
	StelOpenGL::highGraphicsFunctions()->glDrawArraysInstanced(GL_TRIANGLES, 0, 6, static_cast<GLsizei>(nbPhotoPointSources));
	photoVao->release();
	prog.release();

	nbPhotoPointSources = 0;
}

// Compute RMag and CMag from magnitude for a point source.
bool StelSkyDrawer::computePhotoRCMag(float mag, RCMag* rcMag) const
{
	// Total recorded light of the star (sum over all pixels, scene units: middle grey 0.18, the sensor saturates
	// at PHOTO_SENSOR_SATURATION)
	const float flux = photoFluxMag0 * std::pow(10.f, -0.4f*mag);
	// Radius where a Moffat component of the given total light drops to the visibility threshold
	auto radiusAtThreshold = [this](float componentFlux, float alpha, float beta) -> float
	{
		const float peak = componentFlux * (beta-1.f) / (M_PIf*alpha*alpha);
		if (!(peak > photoVisibleThreshold))
			return 0.f;
		return alpha * std::sqrt(std::pow(peak/photoVisibleThreshold, 1.f/beta) - 1.f);
	};
	const float spikeFraction = getPhotoSpikeFractionEffective();
	// Out of focus, the light of the core is spread over the blur disk (the image of the aperture):
	// faint stars disappear, as on a real photo
	const float defocusR = std::fabs(photoLensCache.defocus);
	const float coreFlux = flux*(1.f-photoHaloFraction-spikeFraction);
	const float rCore = defocusR > 0.f ? photoDefocusCoreRadius(coreFlux, defocusR)
	                                   : radiusAtThreshold(coreFlux, photoCoreAlpha, PHOTO_CORE_BETA);
	// It must also rise above the noise of the sky: signal-to-noise ratio over its image (matched filter: the
	// noise of the effective area of the PSF, pi alpha^2 (2 beta - 1)/(beta - 1)^2 for a Moffat profile, or of
	// the blur disk). The faint stars drowned in the noise are not in the picture, and the long lists of them
	// are not drawn. This limit grows with the focal length (a larger aperture at the same f-number collects
	// more light of a star, the sky per pixel stays the same) and the exposure, and drops in a bright sky.
	const float effectiveArea = M_PIf*photoCoreAlpha*photoCoreAlpha*(2.f*PHOTO_CORE_BETA - 1.f)/((PHOTO_CORE_BETA - 1.f)*(PHOTO_CORE_BETA - 1.f))
	                            + M_PIf*defocusR*defocusR;
	const bool detected = coreFlux > photoDetectionSnr * photoNoiseSigma * std::sqrt(effectiveArea);
	if (rCore <= 0.f || !detected)
	{
		rcMag->radius = 0.f;
		rcMag->luminance = 0.f;
		return false;
	}
	// The halo spreads around the blur disk (see the shader): the profile at max(0, r - R), renormalized
	float rHalo = 0.f;
	if (!getFlagPhotoBloomActive())
	{
		const float alpha = photoCoreAlpha*PHOTO_HALO_WIDTH_FACTOR;
		const float norm = 1.f/(1.f + photoDiskTerm(PHOTO_HALO_BETA)*defocusR/alpha + (PHOTO_HALO_BETA-1.f)*defocusR*defocusR/(alpha*alpha));
		const float r = radiusAtThreshold(flux*photoHaloFraction*norm, alpha, PHOTO_HALO_BETA);
		if (r > 0.f)
			rHalo = r + defocusR;
	}
	// Length of the diffraction spikes: where the intensity along a spike drops to the threshold
	float rSpike = 0.f;
	if (spikeFraction > 0.f)
	{
		const float lines = static_cast<float>(getPhotoSpikeLines());
		const float sigma = photoCoreAlpha*PHOTO_SPIKE_WIDTH_FACTOR;
		const float length = getPhotoSpikeLength();
		const float s1 = std::sqrt(2.f*M_PIf)*sigma;
		const float s2 = M_PIf*length;
		const float norm = (s1*s2)/((s1+2.f*defocusR)*(s2+2.f*defocusR)); // a band as wide as the blur disk
		const float peak = flux*spikeFraction*norm / (lines * s1 * s2);
		if (peak > photoVisibleThreshold)
			rSpike = defocusR + length * std::sqrt(peak/photoVisibleThreshold - 1.f);
		// With bloom, the inner part of the spikes comes from the image
		if (getFlagPhotoBloomActive() && rSpike <= StelPhotoBloom::NEAR_SPIKE_LENGTH)
			rSpike = 0.f;
	}
	// The sprite ends where the profile fades out; leave room for that fade (see the shader)
	rcMag->radius = qMin(PHOTO_MAX_SPRITE_RADIUS, qMax(qMax(qMax(rCore, rHalo), rSpike)/0.85f, 1.5f*photoCoreAlpha));
	rcMag->luminance = flux;
	return true;
}

// Draw a point source halo.
bool StelSkyDrawer::drawPhotoPointSource(StelPainter* sPainter, const Vec3d& v, const RCMag& rcMag, const Vec3f& linearUnitColor, float dim, bool checkInScreen)
{
	if (photoInstances.empty())
		return false;
	if (!(rcMag.luminance > 0.f) || !(rcMag.radius > 0.f) || !(dim > 0.f))
		return false;
	Vec3d win;
	if (!(checkInScreen ? sPainter->getProjector()->projectCheck(v, win) : sPainter->getProjector()->project(v, win)))
		return false;

	// No twinkling: a photo integrates over the exposure
	PhotoStarInstance& star = photoInstances[nbPhotoPointSources];
	star.center.set(static_cast<float>(win[0]), static_cast<float>(win[1]));
	star.color = linearUnitColor * (rcMag.luminance * dim);
	star.radius = rcMag.radius;
	if (photoLensAberrations)
	{
		// Room for the coma and the astigmatism of the lens (see aberratedCore() in the shader)
		const float rho = std::hypot(star.center[0]-photoLensCache.center[0], star.center[1]-photoLensCache.center[1]) / photoLensCache.corner;
		star.radius += 2.5f*photoLensCache.coma*rho + 0.7f*photoLensCache.astigmatism*rho*rho;
	}
	star.shape.set(0.f, 1.f, 0.f, 1.f);
	star.sunDir.set(1.f, 0.f);
	// Star trail (the sprite box around it is made in the shader)
	if (computePhotoTrail(*sPainter->getProjector(), v, win, rcMag.luminance*dim, 0.f, star) < 0.f)
		return false;
	++nbPhotoPointSources;
	if (nbPhotoPointSources>=maxPhotoPointSources)
		flushPhotoPointSources(sPainter);
	return true;
}

float StelSkyDrawer::getPhotoCoreWeight(float diskPixRadius) const
{
	// Same limits as the classic halo of the planets: below 3 pixels the body looks like a star
	const float scale = StelApp::getInstance().getScreenScale();
	const float tStart = 3.f*scale;
	const float tStop = 6.f*scale;
	return qBound(0.f, (tStop-diskPixRadius)/(tStop-tStart), 1.f);
}

float StelSkyDrawer::getPhotoDiskFraction(float diskPixRadius) const
{
	const float spikeFraction = getPhotoSpikeFractionEffective();
	return (1.f - photoHaloFraction - spikeFraction) * (1.f - getPhotoCoreWeight(diskPixRadius));
}

bool StelSkyDrawer::drawPhotoBody(StelPainter* sPainter, const Vec3d& v, float diskPixRadius, float mag, const Vec3f& color,
                                  float illuminatedFraction, const Vec3d& sunDirection)
{
	if (photoInstances.empty() || !photoStarShaderProgram)
		return false;
	const float dim = qMax(color[0], qMax(color[1], color[2]));
	if (!(dim > 0.f))
		return false;
	const Vec3f linear = srgbToLinear(color/dim);
	const float Y = 0.2126f*linear[0] + 0.7152f*linear[1] + 0.0722f*linear[2];
	if (!(Y > 0.f))
		return false;

	// Total recorded light of the body (sum over all pixels, linear units)
	const float flux = photoFluxMag0 * std::pow(10.f, -0.4f*mag);
	if (!(flux > 0.f))
		return false;
	const float diskR = qMax(0.f, diskPixRadius);
	const float coreWeight = getPhotoCoreWeight(diskR);
	// A body in phases: its light comes from the lit part. For the normalization, that part counts
	// as a disk of the same area; the shader measures the distance from the true lit shape.
	const float k = qBound(0.f, illuminatedFraction, 1.f);
	const float Rlit = diskR * std::sqrt(k);
	const float spikeFraction = getPhotoSpikeFractionEffective();
	// Out of focus, a body smaller than the blur disk becomes an image of the aperture, like a star
	// (the shader makes the same choice). Its halo and spikes then spread around the blur disk.
	const float defocusR = std::fabs(photoLensCache.defocus);
	const bool defocused = defocusR > 0.f && diskR < defocusR;
	const float R = defocused ? std::sqrt(defocusR*defocusR + Rlit*Rlit) : Rlit;

	// How far beyond the disk edge each component stays visible (same as for stars, with the
	// normalization of a profile spread around the disk)
	auto wingRadius = [this, R](float componentFlux, float alpha, float beta) -> float
	{
		const float norm = 1.f/(1.f + photoDiskTerm(beta)*R/alpha + (beta-1.f)*R*R/(alpha*alpha));
		const float peak = componentFlux * norm * (beta-1.f) / (M_PIf*alpha*alpha);
		if (!(peak > photoVisibleThreshold))
			return 0.f;
		return alpha * std::sqrt(std::pow(peak/photoVisibleThreshold, 1.f/beta) - 1.f);
	};
	const float rCore = defocused ? photoDefocusCoreRadius(flux*(1.f-photoHaloFraction-spikeFraction)*coreWeight, R)
	                              : wingRadius(flux*(1.f-photoHaloFraction-spikeFraction)*coreWeight, photoCoreAlpha, PHOTO_CORE_BETA);
	const float rHalo = getFlagPhotoBloomActive() ? 0.f : wingRadius(flux*photoHaloFraction, photoCoreAlpha*PHOTO_HALO_WIDTH_FACTOR, PHOTO_HALO_BETA);
	float rSpike = 0.f;
	if (spikeFraction > 0.f)
	{
		const float lines = static_cast<float>(getPhotoSpikeLines());
		const float sigma = photoCoreAlpha*PHOTO_SPIKE_WIDTH_FACTOR;
		const float length = getPhotoSpikeLength();
		const float s1 = std::sqrt(2.f*M_PIf)*sigma;
		const float s2 = M_PIf*length;
		const float norm = (s1*s2)/((s1+2.f*R)*(s2+2.f*R));
		const float peak = flux*spikeFraction*norm / (lines * s1 * s2);
		if (peak > photoVisibleThreshold)
			rSpike = length * std::sqrt(peak/photoVisibleThreshold - 1.f);
		// With bloom, the inner part of the spikes comes from the image
		if (getFlagPhotoBloomActive() && rSpike <= StelPhotoBloom::NEAR_SPIKE_LENGTH)
			rSpike = 0.f;
	}
	// Defocused: the halo and the spikes start at the blur disk (the core radius already includes it)
	const float wingOffset = defocused ? R : 0.f;
	const float wing = qMax(qMax(rCore, rHalo > 0.f ? rHalo + wingOffset : 0.f), rSpike > 0.f ? rSpike + wingOffset : 0.f);
	if (!(wing > 0.f))
		return false;
	// The spikes of the Sun can cross the whole screen
	const auto& prj = sPainter->getProjector();
	const float maxRadius = qMax(PHOTO_MAX_SPRITE_RADIUS, 1.5f*static_cast<float>(std::hypot(prj->getViewportWidth(), prj->getViewportHeight())));
	const float radius = qMin(diskR + maxRadius, diskR + qMax(wing/0.85f, 1.5f*photoCoreAlpha));

	// Only a body imaged on the sensor (its disk at least partly in the frame) produces this image
	Vec3d win;
	if (!prj->projectCheck(v, win))
	{
		if (!prj->project(v, win))
			return false;
		const double margin = static_cast<double>(diskR) + 2.;
		if (win[0] < prj->getViewportPosX() - margin || win[0] > prj->getViewportPosX() + prj->getViewportWidth() + margin ||
		    win[1] < prj->getViewportPosY() - margin || win[1] > prj->getViewportPosY() + prj->getViewportHeight() + margin)
			return false;
	}

	// Direction towards the Sun on the screen
	Vec2f sunDir(1.f, 0.f);
	if (k < 1.f && sunDirection.normSquared() > 0.)
	{
		const Vec3d vn = v/v.norm();
		Vec3d winSun;
		if (prj->project(vn + sunDirection/sunDirection.norm()*1e-4, winSun))
		{
			Vec2d dir(winSun[0]-win[0], winSun[1]-win[1]);
			const double len = dir.norm();
			if (len > 0.)
				sunDir.set(static_cast<float>(dir[0]/len), static_cast<float>(dir[1]/len));
		}
	}
	// Illuminated fraction k = (1 + cos(phase angle))/2
	const float cosPhase = 2.f*k - 1.f;

	flushPhotoPointSources(sPainter); // keep the order of drawing
	PhotoStarInstance& body = photoInstances[nbPhotoPointSources];
	body.center.set(static_cast<float>(win[0]), static_cast<float>(win[1]));
	body.color = (linear/Y) * (flux*dim);
	body.radius = radius;
	body.shape.set(diskR, coreWeight, Rlit, cosPhase);
	body.sunDir = sunDir;
	// A trail only for a body whose disk is smaller than the trail: it is drawn like a star then
	if (computePhotoTrail(*prj, v, win, flux*dim, diskR, body) < 0.f)
		return false;
	// The PSF core here, in the order of drawing: nearer bodies (the Moon) and the landscape can cover it.
	// The halo and the spikes arise in the lens and lie over everything: they are drawn at the end.
	PhotoStarInstance wings = body;
	wings.shape[1] = 0.f; // no core
	++nbPhotoPointSources;
	flushPhotoPointSources(sPainter, PhotoBatchCoreOnly);
	if (rHalo > 0.f || rSpike > 0.f)
		photoDeferredBodies.push_back({wings, v, photoBodyMotion});
	return true;
}

void StelSkyDrawer::drawDeferredPhotoBodies(StelCore* core)
{
	if (!getFlagPhotographicModeActive() || photoInstances.empty())
	{
		photoDeferredBodies.clear();
		photoGhostSources.clear();
		return;
	}
	const StelProjectorP prj = core->getProjection(StelCore::FrameJ2000);
	if (!photoDeferredBodies.empty())
	{
		StelPainter sPainter(prj);
		for (const auto& body : photoDeferredBodies)
		{
			const Vec4f& trail = body.instance.trail;
			// A body larger than its trail is drawn without one (the shader trails point-like images only):
			// its halo and spikes are trailed as an image of their own then
			const bool trailImage = (photoTrailsActive || photoGroundTrailActive) && trail[0] == 0.f && trail[1] == 0.f && trail[2] == 0.f && trail[3] == 0.f;
			if (trailImage)
			{
				flushPhotoPointSources(&sPainter, PhotoBatchWingsOnly);
				photoBodyMotion = body.motion;
				const bool image = beginPhotoBodyImage(body.posJ2000, body.instance.radius, true);
				photoBodyMotion = Vec3d(0.);
				if (image)
				{
					photoInstances[nbPhotoPointSources++] = body.instance;
					flushPhotoPointSources(&sPainter, PhotoBatchWingsOnly);
					endPhotoBodyImage();
					continue;
				}
			}
			photoInstances[nbPhotoPointSources++] = body.instance;
			if (nbPhotoPointSources >= maxPhotoPointSources)
				flushPhotoPointSources(&sPainter, PhotoBatchWingsOnly);
		}
		flushPhotoPointSources(&sPainter, PhotoBatchWingsOnly);
		photoDeferredBodies.clear();
	}

	// Lens reflections of the bright bodies, also of those outside the frame
	if (!photoGhostSources.empty())
	{
		std::vector<StelPhotoLensFlare::Source> sources;
		for (const auto& g : photoGhostSources)
		{
			const float dim = qMax(g.color[0], qMax(g.color[1], g.color[2]));
			if (!(dim > 0.f))
				continue;
			const Vec3f linear = srgbToLinear(g.color/dim);
			const float Y = 0.2126f*linear[0] + 0.7152f*linear[1] + 0.0722f*linear[2];
			const float flux = photoFluxMag0 * std::pow(10.f, -0.4f*g.mag) * dim;
			Vec3d win;
			if (!(Y > 0.f) || !(flux > 0.f) || !prj->project(g.v, win))
				continue;
			// in front of the camera only (a perspective projection maps the back half too)
			Vec3d view = g.v;
			prj->getModelViewTransform()->forward(view);
			if (view[2] >= 0.)
				continue;
			sources.push_back({Vec2f(static_cast<float>(win[0]), static_cast<float>(win[1])), (linear/Y)*flux});
		}
		photoGhostSources.clear();
		if (!sources.empty())
		{
			if (!photoLensFlare)
				photoLensFlare.reset(new StelPhotoLensFlare);
			StelPhotoLensFlare::Camera camera;
			camera.center = photoLensCache.center;
			camera.focal = qMax(1.f, prj->getPixelPerRadAtCenter());
			camera.corner = photoLensCache.corner;
			camera.fNumber = static_cast<float>(photoAperture);
			camera.blades = photoApertureBlades;
			camera.straightness = photoApertureBlades > 0 ? getPhotoApertureStraightness() : 0.f;
			camera.bladeAngle = PHOTO_SPIKE_ANGLE;
			camera.strength = photoLensFlareStrength;
			camera.visibleThreshold = photoVisibleThreshold;
			photoLensFlare->draw(sources, camera, prj->getProjectionMatrix());
		}
	}
}

void StelSkyDrawer::setFlagPhotoStarTrails(bool b)
{
	if (b == flagPhotoStarTrails)
		return;
	flagPhotoStarTrails = b;
	StelApp::immediateSave("viewing/flag_photographic_star_trails", b);
	emit flagPhotoStarTrailsChanged(b);
}

Vec3f StelSkyDrawer::photoTrailExtinctionFactor(const Vec3d& view, const Vec3d& viewRef) const
{
	if (!photoTrailExtinction || !photoTrailAltAzTransform)
		return Vec3f(1.f);
	// Geometric altitude/azimuth, as for the extinction of stars and planets (see ZoneArray, Planet)
	Vec3d altAz = view;
	Vec3d altAzRef = viewRef;
	photoTrailAltAzTransform->backward(altAz);
	photoTrailAltAzTransform->backward(altAzRef);
	altAz.normalize();
	altAzRef.normalize();
	// Below the horizon (the true horizon, or about -2 degrees) the light is gone, whatever the setting for the extinction there
	const double horizonSine = extinction.getHorizonSineGeometric();
	auto extinctionMag = [this, horizonSine](const Vec3d& aa) -> float {
		if (aa[2] < horizonSine)
			return 42.f*extinction.getExtinctionCoefficient();
		float mag = 0.f;
		extinction.forward(aa, &mag);
		return mag;
	};
	const float brightness = std::pow(10.f, -0.4f*(extinctionMag(altAz) - extinctionMag(altAzRef)));
	Vec3f factor(brightness);
	if (getFlagPhotoExtinctionColorActive())
	{
		const Vec3f tint = getPhotoExtinctionTint(altAz);
		const Vec3f tintRef = getPhotoExtinctionTint(altAzRef);
		for (int i=0; i<3; ++i)
			factor[i] *= tintRef[i] > 0.f ? tint[i]/tintRef[i] : 1.f;
	}
	return factor;
}

float StelSkyDrawer::computePhotoTrail(const StelProjector& prj, const Vec3d& v, const Vec3d& win, float flux, float diskRadius, PhotoStarInstance& instance) const
{
	Vec4f& trail = instance.trail;
	instance.trailStart.set(1.f, 1.f, 1.f);
	instance.trailEnd.set(1.f, 1.f, 1.f);
	// The integrated profiles of the trails in the shader (moffatSegment3/15) are written for these exponents
	static_assert(PHOTO_CORE_BETA == 3.f && PHOTO_HALO_BETA == 1.5f, "star trail profiles need beta 3 and 1.5");
	trail.set(0.f, 0.f, 0.f, 0.f);
	if (!photoTrailsActive && !photoGroundTrailActive)
		return 0.f;
	const auto mv = prj.getModelViewTransform();
	Vec3d view = v;
	mv->forward(view);
	const double norm = view.norm();
	if (!(norm > 0.))
		return 0.f;
	// A body that moves against the stars (the Moon): its own motion over half the exposure, in view coordinates
	Vec3d halfMotion(0.);
	if (photoBodyMotion.normSquared() > 0.)
	{
		Mat4d linear = mv->getApproximateLinearTransfo();
		linear[12] = linear[13] = linear[14] = 0.;
		halfMotion = linear.multiplyWithoutTranslation(photoBodyMotion) * (0.5*getPhotoEffectiveExposureTime()*norm);
	}
	// On a star tracker the stars stay in their place: only a body's own motion makes a trail. The altitude still
	// changes during the exposure, and with it the extinction: without a trail the light is its mean (Simpson's
	// rule over the start, the middle and the end), with one its change along the trail (as for the star trails).
	const bool tracker = photoGroundTrailActive;
	float estimate;
	if (tracker)
	{
		estimate = static_cast<float>(2.*halfMotion.norm()/norm * prj.getPixelPerRadAtCenter());
		if (estimate <= 2.f*diskRadius || estimate < 0.5f)
		{
			if (photoTrailExtinction)
			{
				const Vec3f fStart = photoTrailExtinctionFactor(photoTrailToStart*view, view);
				const Vec3f fEnd = photoTrailExtinctionFactor(photoTrailToEnd*view, view);
				for (int c=0; c<3; ++c)
					instance.color[c] *= (fStart[c] + 4.f + fEnd[c])/6.f;
			}
			return 0.f;
		}
	}
	else
	{
		// Quick estimate first (the arc near the image center): most faint stars drop out here, before projecting
		estimate = static_cast<float>(photoTrailAngle * (photoTrailAxisView^view).norm()/norm * prj.getPixelPerRadAtCenter());
	}
	if (estimate <= 2.f*diskRadius || estimate < 0.5f)
		return 0.f;
	// The brightest point of the trailed image: along the trail the core is spread like flux/length, across it
	// keeps its profile (peak 0.75/alpha for a Moffat profile with beta 3); a blur disk widens it.
	// The halo the same way (peak 1/(pi alpha) for beta 1.5). The spikes are kept as they are (conservative).
	{
		const float alpha = qMax(photoCoreAlpha, 1.5f*std::fabs(photoLensCache.defocus));
		const float corePeak = flux*getPhotoCoreFraction()*0.75f/(alpha*estimate);
		const float haloPeak = getFlagPhotoBloomActive() ? 0.f : flux*photoHaloFraction/(M_PIf*photoCoreAlpha*PHOTO_HALO_WIDTH_FACTOR*estimate);
		bool visible = corePeak > photoVisibleThreshold || haloPeak > photoVisibleThreshold;
		if (!visible && getPhotoSpikeFractionEffective() > 0.f)
		{
			const float sigma = photoCoreAlpha*PHOTO_SPIKE_WIDTH_FACTOR;
			const float length = getPhotoSpikeLength();
			const float peak = flux*getPhotoSpikeFractionEffective() / (static_cast<float>(getPhotoSpikeLines()) * std::sqrt(2.f*M_PIf) * sigma * M_PIf * length);
			visible = peak > photoVisibleThreshold && !(getFlagPhotoBloomActive() && length*std::sqrt(peak/photoVisibleThreshold - 1.f) <= StelPhotoBloom::NEAR_SPIKE_LENGTH);
		}
		if (!visible)
			return -1.f;
	}
	// Positions at the start and the end of the exposure: the body's own motion, then (fixed tripod) the turn of
	// the sky around its axis; then project
	Vec3d a = view - halfMotion;
	Vec3d b = view + halfMotion;
	if (!tracker)
	{
		a = photoTrailToStart*a;
		b = photoTrailToEnd*b;
	}
	mv->backward(a);
	mv->backward(b);
	Vec3d winA, winB;
	if (!prj.project(a, winA) || !prj.project(b, winB))
		return 0.f;
	trail.set(static_cast<float>(winA[0]-win[0]), static_cast<float>(winA[1]-win[1]),
	          static_cast<float>(winB[0]-win[0]), static_cast<float>(winB[1]-win[1]));
	const float length = std::hypot(trail[0], trail[1]) + std::hypot(trail[2], trail[3]);
	if (!(length > 0.5f) || length <= 2.f*diskRadius || !std::isfinite(length))
	{
		trail.set(0.f, 0.f, 0.f, 0.f);
		return 0.f;
	}
	// The altitude changes along the trail, and so do the extinction and its color (e.g. a rising planet,
	// red at first). The image at mid-exposure has its own; the start and the end relative to it.
	if (photoTrailExtinction)
	{
		instance.trailStart = photoTrailExtinctionFactor(photoTrailToStart*view, view);
		instance.trailEnd = photoTrailExtinctionFactor(photoTrailToEnd*view, view);
	}
	return length;
}

float StelSkyDrawer::getPhotoApertureShapeArea() const
{
	// Disk, or regular polygon of circumradius 1 (the straightness mixes them, see the shader)
	const int blades = photoApertureBlades >= 3 ? photoApertureBlades : 0;
	if (blades == 0)
		return M_PIf;
	const float n = static_cast<float>(blades);
	const float t = getPhotoApertureStraightness();
	return (1.f-t)*M_PIf + t*0.5f*n*std::sin(2.f*M_PIf/n);
}

float StelSkyDrawer::photoDefocusCoreRadius(float coreFlux, float R) const
{
	// The same blend as in the shader: below the PSF core size the blur only widens the core
	const float fwhm = photoCoreFwhm();
	const float x = qBound(0.f, (R/fwhm - 0.5f)/1.5f, 1.f);
	const float t = x*x*(3.f - 2.f*x);
	const float edge = photoDefocusEdge();
	float peakMoffat = 0.f, alphaWide = photoCoreAlpha;
	if (t < 1.f)
	{
		const float k = 1.6f*R/fwhm;
		alphaWide = photoCoreAlpha*std::sqrt(1.f + k*k);
		peakMoffat = (1.f-t) * coreFlux * (PHOTO_CORE_BETA-1.f) / (M_PIf*alphaWide*alphaWide);
	}
	// The brightest part of the disk: its center or its rim (spherical aberration)
	const float sa = photoLensCache.sphericalAberration;
	const float peakDisk = t * coreFlux * (1.f + 0.5f*sa) / (getPhotoApertureShapeArea()*R*R);
	if (!(peakMoffat + peakDisk > photoVisibleThreshold))
		return 0.f;
	float radius = 0.f;
	if (peakMoffat > photoVisibleThreshold)
		radius = alphaWide * std::sqrt(std::pow(peakMoffat/photoVisibleThreshold, 1.f/PHOTO_CORE_BETA) - 1.f);
	// The whole disk with its blurred edge must lie inside the part of the sprite that is not faded out
	// (the fade starts at 3/4 of the radius; the callers divide by 0.85)
	return qMax(radius, (R + 2.f*edge) * (0.85f/0.75f));
}

void StelSkyDrawer::addPhotoGhostSource(const Vec3d& v, float mag, const Vec3f& color)
{
	if (flagPhotoLensFlare && photoLensFlareStrength > 0.f && getFlagPhotographicModeActive())
		photoGhostSources.push_back({v, mag, color});
}

void StelSkyDrawer::setPhotoPsfMinFwhm(double pixels)
{
	const float v = qBound(0.8f, static_cast<float>(pixels), 10.f);
	if (qFuzzyCompare(v, photoMinFwhmPixels))
		return;
	photoMinFwhmPixels = v;
	StelApp::immediateSave("viewing/photographic_psf_min_fwhm", static_cast<double>(v));
	emit photoPsfMinFwhmChanged(static_cast<double>(v));
}

void StelSkyDrawer::setPhotoSeeing(double arcsec)
{
	const float v = qBound(0.f, static_cast<float>(arcsec), 60.f);
	if (qFuzzyCompare(1.f+v, 1.f+photoSeeingArcsec))
		return;
	photoSeeingArcsec = v;
	StelApp::immediateSave("viewing/photographic_seeing", static_cast<double>(v));
	emit photoSeeingChanged(static_cast<double>(v));
}

void StelSkyDrawer::setPhotoHaloStrength(double fraction)
{
	// A digital camera and a film camera each keep their own value: in front of a sensor lies a stack of filters
	// (infrared cut, antialiasing, cover glass), whose reflections with the sensor add to the glow of the lens;
	// a film lies bare in the focal plane, and has the glow of the lens alone (and its own halation)
	const float v = qBound(0.f, static_cast<float>(fraction), 0.3f);
	if (qFuzzyCompare(1.f+v, 1.f+photoHaloFraction))
		return;
	photoHaloFraction = v;
	if (photoFilm != PhotoFilmNone)
	{
		photoFilmHaloFraction = v;
		StelApp::immediateSave("viewing/photographic_film_halo_strength", static_cast<double>(v));
	}
	else
	{
		photoDigitalHaloFraction = v;
		StelApp::immediateSave("viewing/photographic_halo_strength", static_cast<double>(v));
	}
	emit photoHaloStrengthChanged(static_cast<double>(v));
}

void StelSkyDrawer::setPhotoApertureBlades(int blades)
{
	const int v = (blades<=0) ? 0 : qBound(4, blades, 9);
	if (v==photoApertureBlades)
		return;
	photoApertureBlades = v;
	StelApp::immediateSave("viewing/photographic_aperture_blades", v);
	emit photoApertureBladesChanged(v);
}

float StelSkyDrawer::getPhotoApertureStraightness() const
{
	// Wide open the opening is the round stop of the lens (the blades are retracted); stopping down,
	// the blades form a polygon whose edges get straighter. Fully straight two stops down.
	const float stops = static_cast<float>(std::log2(photoAperture/photoLensMaxAperture));
	const float t = qBound(0.f, stops/2.f, 1.f);
	return t*t*(3.f - 2.f*t);
}

float StelSkyDrawer::getPhotoSpikeFractionEffective() const
{
	// The light that straight edges diffract into the spikes: per unit length along a spike it falls off
	// like (lambda N)/x^2 beyond the diffraction scale (lambda N), so the total stays the same while the
	// spikes get longer with the f-number (see getPhotoSpikeLength()).
	if (photoApertureBlades <= 0)
		return 0.f;
	return photoSpikeFraction * getPhotoApertureStraightness();
}

float StelSkyDrawer::getPhotoSpikeLength() const
{
	// The diffraction pattern scales with lambda N: stopped down, the spikes reach farther
	const float scale = qBound(0.25f, static_cast<float>(photoAperture)/PHOTO_SPIKE_REFERENCE_APERTURE, 4.f);
	return photoCoreAlpha*PHOTO_SPIKE_LENGTH_FACTOR*scale;
}

void StelSkyDrawer::setPhotoSpikeStrength(double fraction)
{
	const float v = qBound(0.f, static_cast<float>(fraction), 0.1f);
	if (qFuzzyCompare(1.f+v, 1.f+photoSpikeFraction))
		return;
	photoSpikeFraction = v;
	StelApp::immediateSave("viewing/photographic_spike_strength", static_cast<double>(v));
	emit photoSpikeStrengthChanged(static_cast<double>(v));
}

void StelSkyDrawer::setFlagPhotoBloom(bool b)
{
	if (b == flagPhotoBloom)
		return;
	flagPhotoBloom = b;
	StelApp::immediateSave("viewing/flag_photographic_bloom", b);
	emit flagPhotoBloomChanged(b);
}

bool StelSkyDrawer::getFlagPhotoBloomActive() const
{
	return flagPhotoBloom && (photoHaloFraction > 0.f || getPhotoSpikeFractionEffective() > 0.f) && getFlagPhotographicModeActive();
}

float StelSkyDrawer::getPhotoCoreFraction() const
{
	const float spikeFraction = getPhotoSpikeFractionEffective();
	return 1.f - photoHaloFraction - spikeFraction;
}
