/*
 * Copyright (C) 2008 Fabien Chereau
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

#include "StelSkyImageTile.hpp"
#include "StelTextureMgr.hpp"
#include "StelApp.hpp"
#include "StelFileMgr.hpp"
#include "StelUtils.hpp"
#include "StelTexture.hpp"
#include "StelProjector.hpp"
#include "StelToneReproducer.hpp"
#include "StelCore.hpp"
#include "StelSkyDrawer.hpp"
#include "StelPainter.hpp"
#include "StelModuleMgr.hpp"
#include "SolarSystem.hpp"
#include "NebulaMgr.hpp"
#include "Nebula.hpp"
#include <QDebug>
#include <QHash>

#include <cstdio>

StelSkyImageTile::StelSkyImageTile()
{
	initCtor();
}

void StelSkyImageTile::initCtor()
{
	minResolution = -1;
	luminance = -1;
	alphaBlend = false;
	noTexture = false;
	texFader = Q_NULLPTR;
	birthJD = -1e10;
	withAberration = true;
	decimation = 1;
	flagVisible = true;
}

// Constructor
StelSkyImageTile::StelSkyImageTile(const QString& url, StelSkyImageTile* parent, int decimateBy) : MultiLevelJsonBase(parent)
{
	initCtor();
	decimation=decimateBy;
	if (parent)
	{
		luminance = parent->luminance;
		alphaBlend = parent->alphaBlend;
	}
	initFromUrl(url);
}

// Constructor from a map used for JSON files with more than 1 level
StelSkyImageTile::StelSkyImageTile(const QVariantMap& map, StelSkyImageTile* parent, int decimateBy) : MultiLevelJsonBase(parent)
{
	initCtor();
	decimation=decimateBy;
	if (parent)
	{
		luminance = parent->luminance;
		alphaBlend = parent->alphaBlend;
	}
	initFromQVariantMap(map);
}

// Destructor
StelSkyImageTile::~StelSkyImageTile()
{
}

void StelSkyImageTile::draw(StelCore* core, StelPainter& sPainter, float opacity)
{
	Q_UNUSED(opacity)
	const StelProjectorP prj = core->getProjection(StelCore::FrameJ2000);

	// compute aberration correction vector
	Vec3d vel(0.0);
	if ((core) && (core->getUseAberration()) && (core->getCurrentPlanet()) && (withAberration))
	{
		vel = core->getAberrationVec(core->getJDE());
	}

	// In the photographic mode the exposure decides what shows (see drawTile()), not the eye
	const float limitLuminance = core->getSkyDrawer()->getFlagPhotographicModeActive() ? 0.f : core->getSkyDrawer()->getLimitLuminance();
	QMultiMap<double, StelSkyImageTile*> result;
	// TODO: adjust that viewportconvexpolygon by aberration to select the right tiles.
	// I thought the viewportpolygon needs to be enlarged just a bit. (I use 20 arcseconds here as estimate of max. aberration from earth.)
	//getTilesToDraw(result, core, prj->getViewportConvexPolygon(0,0)->getEnlarged(20./3600.*M_PI_180 *core->getAberrationFactor()), limitLuminance, true);
	// But it seems not even the AllSky region prevents clipping, so it must be caused somewhere else.
//	const SphericalCap& hp = prj->getBoundingCap();
//	getTilesToDraw(result, core, SphericalRegionP(new SphericalCap(hp)), limitLuminance, true);
	getTilesToDraw(result, core, SphericalRegionP(new AllSkySphericalRegion()), limitLuminance, true);

	int numToBeLoaded=0;
	for (auto* t : std::as_const(result))
		if (t->isReadyToDisplay()==false)
			++numToBeLoaded;
	updatePercent(result.size(), numToBeLoaded);

	// Draw in the good order
	sPainter.setBlending(true, GL_ONE, GL_ONE);
	auto i = result.end();
	while (i!=result.begin())
	{
		--i;
		i.value()->drawTile(core, sPainter, vel);
	}

	deleteUnusedSubTiles();
}

// Return the list of tiles which should be drawn.
void StelSkyImageTile::getTilesToDraw(QMultiMap<double, StelSkyImageTile*>& result, StelCore* core, const SphericalRegionP& viewPortPoly, float limitLuminance, bool recheckIntersect)
{
#ifndef NDEBUG
	// When this method is called, we can assume that:
	// - the parent tile min resolution was reached
	// - the parent tile is intersecting FOV
	// - the parent tile is not scheduled for deletion
	const StelSkyImageTile* parent = qobject_cast<StelSkyImageTile*>(QObject::parent());
	if (parent!=Q_NULLPTR)
	{
		Q_ASSERT(isDeletionScheduled()==false);
		const double degPerPixel = 1./core->getProjection(StelCore::FrameJ2000)->getPixelPerRadAtCenter()*M_180_PI;
		Q_ASSERT(degPerPixel<parent->minResolution);

		Q_ASSERT(parent->isDeletionScheduled()==false);
	}
#endif

	// An error occurred during loading
	if (errorOccured)
		return;

	// The JSON file is currently being downloaded
	if (downloading)
	{
		//qDebug() << "Downloading " << contructorUrl;
		return;
	}

	if (luminance>0 && luminance<limitLuminance)
	{
		// Schedule a deletion
		scheduleChildsDeletion();
		return;
	}

	if (birthJD>-1e10 && birthJD>core->getJD())
	{
		// Schedule a deletion
		scheduleChildsDeletion();
		return;
	}

	if (flagVisible == false)
	{
		// Schedule a deletion
		scheduleChildsDeletion();
		return;
	}

	// Check that we are in the screen
	bool fullInScreen = true;
	bool intersectScreen = false;
	if (recheckIntersect)
	{
		if (skyConvexPolygons.isEmpty())
		{
			// If no polygon is defined, we assume that the tile covers the whole sky
			fullInScreen=false;
			intersectScreen=true;
		}
		else
		{
			for (const auto& poly : std::as_const(skyConvexPolygons))
			{
				if (viewPortPoly->contains(poly))
				{
					intersectScreen = true;
				}
				else
				{
					fullInScreen = false;
					if (viewPortPoly->intersects(poly))
						intersectScreen = true;
				}
			}
		}
	}
	// The tile is outside screen
	if (fullInScreen==false && intersectScreen==false)
	{
		// Schedule a deletion
		scheduleChildsDeletion();
		return;
	}

	// The tile is in screen, and it is a precondition that its resolution is higher than the limit
	// make sure that it's not going to be deleted
	cancelDeletion();

	if (noTexture==false)
	{
		if (!tex)
		{
			// The tile has an associated texture, but it is not yet loaded: load it now
			StelTextureMgr& texMgr=StelApp::getInstance().getTextureManager();
			tex = texMgr.createTextureThread(absoluteImageURI, StelTexture::Params().enableMipmapGen().setDecimation(decimation));
			if (!tex)
			{
				qWarning() << "Can't create tile: " << absoluteImageURI;
				errorOccured = true;
				return;
			}
		}

		// The tile is in screen and has a texture: every test passed :) The tile will be displayed
		result.insert(minResolution, this);
	}

	// Check if we reach the resolution limit
	const float degPerPixel = 1.f/core->getProjection(StelCore::FrameJ2000)->getPixelPerRadAtCenter()*M_180_PIf;
	if (degPerPixel < minResolution)
	{
		if (subTiles.isEmpty() && !subTilesUrls.isEmpty())
		{
			// Load the sub tiles because we reached the maximum resolution and they are not yet loaded
			for (const auto& s : std::as_const(subTilesUrls))
			{
				StelSkyImageTile* nt;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
				if (s.metaType()==QMetaType(QMetaType::QVariantMap))
#else
				if (s.type()==QVariant::Map)
#endif
					nt = new StelSkyImageTile(s.toMap(), this, decimation);
				else
				{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
					Q_ASSERT(s.metaType()==QMetaType(QMetaType::QString));
#else
					Q_ASSERT(s.type()==QVariant::String);
#endif
					nt = new StelSkyImageTile(s.toString(), this, decimation);
				}
				subTiles.append(nt);
			}
		}
		// Try to add the subtiles
		for (auto* tile : std::as_const(subTiles))
		{
			qobject_cast<StelSkyImageTile*>(tile)->getTilesToDraw(result, core, viewPortPoly, limitLuminance, !fullInScreen);
		}
	}
	else
	{
		// The subtiles should not be displayed because their resolution is too high
		scheduleChildsDeletion();
	}
}

// The images of nebulae are processed pictures, not measurements: their brightness is set so that the light
// of the whole image is the catalog magnitude of the brightest object in it. Then they appear with the exposure
// and stand in the right proportion to the stars and the Milky Way, while the shape within keeps the
// processing of the picture.
// Emission nebulae have no useful catalog magnitude, most of them none at all (Sharpless, RCW, the California
// Nebula), or the light of the cluster in them (M 16, IC 1396). Their images are measured against the all-sky
// H-alpha map of Finkbeiner (2003, ApJS 146, 407; WHAM, VTSS and SHASSA, 6 arcmin): the texture, smoothed to the
// resolution of the map, against the H-alpha intensity above the local background ("halphaScale" in textures.json,
// the median ratio over the bright parts of the nebula). The V light per Rayleigh is PHOTO_HALPHA_LUMINANCE.
namespace
{
	//! Luminance (cd/m^2) of an emission nebula per Rayleigh of H-alpha. From its line spectrum (H-beta 1/2.86 of
	//! H-alpha, [O III] about twice H-beta, as in StelSkyDrawerPhotoFilm.cpp), through the V band: 100 R make about
	//! 25.1 V mag/arcsec^2, 0.98e-7 cd/m^2 per R. NGC 7000 and M 42, measured by their catalog magnitudes (4.0) and the
	//! map, give 1.01e-7 and 1.08e-7.
	constexpr double PHOTO_HALPHA_LUMINANCE = 1.0e-7;
}
// Photographic mode: the calibration of each image, kept for the session. Measuring it reads the texture back
// from the graphics card (photoTextureStatistics()), which stalls the drawing; a tile is deleted when it leaves
// the view and made again when it comes back, the image stays the same.
namespace
{
	struct PhotoCalibration
	{
		float luminanceScale;
		int objectType;
	};
	QHash<QString, PhotoCalibration> photoCalibrations;
}

void StelSkyImageTile::computePhotoLuminanceScale(StelCore* core)
{
	if (!absoluteImageURI.isEmpty())
	{
		const auto known = photoCalibrations.constFind(absoluteImageURI);
		if (known != photoCalibrations.cend())
		{
			photoLuminanceScale = known->luminanceScale;
			photoObjectType = known->objectType;
			return;
		}
	}
	photoLuminanceScale = -1.f;
	if (skyConvexPolygons.isEmpty() || !tex)
		return;
	// luminance (cd/m^2) of a surface brightness in V mag/arcsec^2
	auto surfaceLuminance = [](double mu) { return 10.8e4 * std::pow(10., -0.4*mu); };
	if (photoHalphaScale > 0.f)
	{
		// An emission nebula measured by H-alpha (its brightest texel not brighter than 15 mag/arcsec^2, as below)
		photoObjectType = static_cast<int>(Nebula::NebHII);
		photoLuminanceScale = static_cast<float>(qMin(PHOTO_HALPHA_LUMINANCE*static_cast<double>(photoHalphaScale), surfaceLuminance(15.)));
		if (!absoluteImageURI.isEmpty())
			photoCalibrations.insert(absoluteImageURI, {photoLuminanceScale, photoObjectType});
		return;
	}
	float mean = 0.f, meanSquare = 0.f;
	Vec3f meanColor(0.f);
	if (!core->getSkyDrawer()->photoTextureStatistics(*tex, StelSkyDrawer::PHOTO_DSO_DESTRETCH, mean, meanSquare, &meanColor) || !(mean > 1e-7f) || !(meanSquare > 0.f))
		return;
	// A red picture without a catalog object (Sharpless, RCW, parts of Barnard's Loop): an emission nebula, H-alpha
	// (on average 2.5 times as much red as green and blue)
	if (meanColor[0] > 1.25f*(meanColor[1] + meanColor[2]))
		photoObjectType = static_cast<int>(Nebula::NebEn);
	// typical texture value of the lit parts of the image, whatever their share of it
	const double lit = static_cast<double>(meanSquare/mean);
	double area = 0.;
	for (const auto& poly : std::as_const(skyConvexPolygons))
		area += poly->getArea();
	if (!(area > 0.))
		return;
	// Images without a catalog object to measure them by (faint nebulae, dust, star fields): their lit parts
	// at a faint surface brightness of 22 mag/arcsec^2
	double scale = surfaceLuminance(22.) / lit;
	NebulaMgr* nebulae = GETSTELMODULE(NebulaMgr);
	if (nebulae && getFrameType() == StelCore::FrameJ2000)
	{
		const SphericalCap cap = skyConvexPolygons.first()->getBoundingCap();
		const double radius = std::acos(qBound(-1., cap.d, 1.)) * M_180_PI;
		float mag = 99.f;
		// The object must be what the image shows: large in it, or in its middle (a small planetary nebula)
		NebulaP object = nebulae->getBrightestWithin(cap.n, radius, 0.1*radius, mag);
		float centralMag = 99.f;
		const NebulaP central = nebulae->getBrightestWithin(cap.n, 0.1*radius, 0.01*radius, centralMag);
		if (central && centralMag < mag)
		{
			object = central;
			mag = centralMag;
		}
		if (object && mag < 50.f)
		{
			photoObjectType = static_cast<int>(object->getDSOType());
			// Globular clusters: about half of their light is in stars too faint for the star catalogs
			if (object->getDSOType() == Nebula::NebGc)
				mag += 0.75f;
			// Reflection nebulae: the catalogs (van den Bergh) give the magnitude of the star that lights them; the
			// nebula reflects a part of its light, here a tenth (the Pleiades: its brightest parts about 21 mag/arcsec^2)
			if (object->getDSOType() == Nebula::NebRn)
				mag += 2.5f;
			// The total light of the object (lux) is the light of the image: its mean luminance (cd/m^2) over
			// the solid angle of the image. The lit parts between 17 mag/arcsec^2 (the cores of bright nebulae
			// and galaxies) and 24 (the faintest nebulae a camera records): beyond, the image shows other things
			// than the object (a wide field, other objects).
			const double meanLuminance = 2.54e-6 * std::pow(10., -0.4*static_cast<double>(mag)) / area;
			scale = qBound(surfaceLuminance(24.) / lit, meanLuminance / static_cast<double>(mean), surfaceLuminance(17.) / lit);
		}
	}
	// Whatever the image: its brightest texel (value 1) not brighter than 15 mag/arcsec^2. An image that is
	// nearly black (a faint nebula processed dark) would otherwise turn its compression noise into light.
	photoLuminanceScale = static_cast<float>(qMin(scale, surfaceLuminance(15.)));
	if (!absoluteImageURI.isEmpty())
		photoCalibrations.insert(absoluteImageURI, {photoLuminanceScale, photoObjectType});
}

// Draw the image on the screen.
// Assume GL_TEXTURE_2D is enabled
bool StelSkyImageTile::drawTile(StelCore* core, StelPainter& sPainter, const Vec3d &vel)
{
	if (!tex->bind())
		return false;

	if (!texFader)
	{
		texFader = new QTimeLine(1000, this);
		texFader->start();
	}

	// Draw the real texture for this image
	float ad_lum = (luminance>0) ? qMin(1.0f, core->getToneReproducer()->adaptLuminanceScaled(luminance)) : 1.f;
	// Photographic mode: the image in physical units, like the stars and the Milky Way (scene units)
	StelSkyDrawer* drawer = core->getSkyDrawer();
	const bool photo = drawer->getFlagPhotographicModeActive();
	float photoValue = 0.f;
	if (photo)
	{
		if (photoLuminanceScale == 0.f)
			computePhotoLuminanceScale(core);
		if (photoLuminanceScale > 0.f)
			photoValue = photoLuminanceScale * drawer->getPhotoSceneScale();
		ad_lum = photoValue;
		if (!tex->bind())
			return false;
		// An emission nebula: its light is in lines, mostly H-alpha. The pictures of it come from cameras modified for
		// H-alpha or from narrow-band filters, which make it deep red; the camera in use records its line spectrum in
		// its own colors (StelSkyDrawer::getPhotoEmissionColor(): a stock digital camera pink, a color film red). The
		// red of the picture beyond its green and blue is taken as that emission, with the same luminance (V); the
		// rest (stars, blue reflection nebulae in it) keeps the colors of the picture.
		const bool emission = photoObjectType == Nebula::NebEn || photoObjectType == Nebula::NebHII
		                   || photoObjectType == Nebula::NebSNR || photoObjectType == Nebula::NebEMO
		                   || photoObjectType == Nebula::NebCn;
		photoEmissionColor = emission ? drawer->getPhotoEmissionColor() : Vec3f(-1.f);
		// Give back a part of the contrast of the processed image: value^p (the shader computes (value*color)^p)
		sPainter.setTextureDisplayAdjustment(1.f/StelSkyDrawer::PHOTO_DSO_DESTRETCH, 1.f, 1.f, 1.f,
		                                     photoEmissionColor[0] >= 0.f ? 5 : 0, false, false);
		sPainter.setTextureEmissionColor(photoEmissionColor);
	}
	Vec4f color;
	if (alphaBlend || texFader->state()==QTimeLine::Running)
	{
		if (!alphaBlend)
			sPainter.setBlending(true); // Normal transparency mode
		else
			sPainter.setBlending(true, GL_ONE, GL_ONE); // additive blending
		color.set(ad_lum,ad_lum,ad_lum, static_cast<float>(texFader->currentValue()));
	}
	else
	{
		sPainter.setBlending(false);
		color.set(ad_lum,ad_lum,ad_lum, 1.f);
	}

	const bool withExtinction=(getFrameType()!=StelCore::FrameAltAz && core->getSkyDrawer()->getFlagHasAtmosphere() && core->getSkyDrawer()->getExtinction().getExtinctionCoefficient()>=0.01f);

	for (const auto& poly : std::as_const(skyConvexPolygons))
	{
		// Not sure: Are all skyConvexPolygons in J2000 frame? This would also simplify code below...
		// No, by scripting we can have other frames!
		//Q_ASSERT(getFrameType() == StelCore::FrameJ2000);

		Vec4f extinctedColor = color;
		if (withExtinction)
		{
			Vec3d bary= poly->getPointInside(); // This is a "frame" vector that points "somewhere" in the first triangle.

			// 2017-03: we need a definite J2000 vector:
			Vec3d baryJ2000;
			double lng, lat, ra, dec; // aux. values for coordinate transformations
			double eclJ2000, eclJDE;
			//qDebug() << "Frame: " << getFrameType();
			switch (getFrameType()) // all possible but AzAlt!
			{
				case StelCore::FrameJ2000:
					baryJ2000=bary;
					break;
				case	StelCore::FrameEquinoxEqu:
					baryJ2000=core->equinoxEquToJ2000(bary, StelCore::RefractionOff);
					break;
				case	StelCore::FrameObservercentricEclipticJ2000:
					// For the ecliptic cases, apply clumsy trafos from StelUtil!
					eclJ2000=GETSTELMODULE(SolarSystem)->getEarth()->getRotObliquity(2451545.0);
					StelUtils::rectToSphe(&lng, &lat, bary);
					StelUtils::eclToEqu(lng, lat, eclJ2000, &ra, &dec);
					StelUtils::spheToRect(ra, dec, baryJ2000);
					break;
				case	StelCore::FrameObservercentricEclipticOfDate:
					// Trafo to eqDate, then as above.
					eclJDE = GETSTELMODULE(SolarSystem)->getEarth()->getRotObliquity(core->getJDE());
					StelUtils::rectToSphe(&lng, &lat, bary);
					StelUtils::eclToEqu(lng, lat, eclJDE, &ra, &dec); // convert to Equatorial/equinox of date
					StelUtils::spheToRect(ra, dec, bary);
					baryJ2000=core->equinoxEquToJ2000(bary, StelCore::RefractionOff);
					break;
				case	StelCore::FrameGalactic:
					baryJ2000=core->galacticToJ2000(bary);
					break;
				case	StelCore::FrameSupergalactic:
					baryJ2000=core->supergalacticToJ2000(bary);
					break;
				default:
					Q_ASSERT(0);
					qDebug() << "StelSkyImageTile: unknown FrameType. Assume J2000.";
					baryJ2000=bary;
			}
			Vec3d altAz = core->j2000ToAltAz(baryJ2000, StelCore::RefractionOff);
			float extinctionMagnitude=0.0f;
			altAz.normalize();
			core->getSkyDrawer()->getExtinction().forward(altAz, &extinctionMagnitude);
			// compute a simple factor from magnitude loss.
			float extinctionFactor=std::pow(0.4f , extinctionMagnitude); // drop of one magnitude: factor 2.5 or 40%
			extinctedColor[0]*=fabs(extinctionFactor);
			extinctedColor[1]*=fabs(extinctionFactor);
			extinctedColor[2]*=fabs(extinctionFactor);
			if (photo)
			{
				// the color of the extinction (the atmosphere takes the blue first)
				const Vec3f tint = drawer->getPhotoExtinctionTint(altAz);
				for (int c=0; c<3; ++c)
					extinctedColor[c] *= tint[c];
			}
		}
		if (photo)
		{
			// The painter takes sRGB-encoded colors (colorToShader() decodes them): encode the linear values,
			// taken to the power 1/p (see setTextureDisplayAdjustment() above). The shader computes
			// (value*color)^p, so the light of the image scales with the linear color given here.
			auto encode = [](const Vec4f& linear, const Vec3f& factor) -> Vec4f
			{
				Vec4f encoded = linear;
				for (int c=0; c<3; ++c)
				{
					const float x = std::pow(qMax(0.f, linear[c]*factor[c]), 1.f/StelSkyDrawer::PHOTO_DSO_DESTRETCH);
					encoded[c] = x <= 0.0031308f ? 12.92f*x : 1.055f*std::pow(x, 1.f/2.4f) - 0.055f;
				}
				return encoded;
			};
			// The camera records the image along the star trail of the exposure and over the blur disk of the
			// lens: copies of it, each with its share of the light, added up (only for images that add light)
			// (and only for images in or near the frame: the copies cost)
			const std::vector<StelSkyDrawer::PhotoImageSample> samples =
				alphaBlend && sPainter.getProjector()->getViewportConvexPolygon(64.f, 64.f)->intersects(poly)
				? drawer->getPhotoImageSamples(*sPainter.getProjector(), poly->getPointInside())
				: std::vector<StelSkyDrawer::PhotoImageSample>();
			if (!samples.empty())
			{
				const StelProjectorP projector = sPainter.getProjector();
				for (const auto& sample : samples)
				{
					sPainter.setProjector(drawer->getPhotoImageSampleProjector(projector, sample.viewRotation));
					sPainter.setColor(encode(extinctedColor, sample.factor));
					sPainter.drawSphericalRegion(poly.data(), StelPainter::SphericalPolygonDrawModeTextureFill, Q_NULLPTR, true, 5., vel);
				}
				sPainter.setProjector(projector);
				continue;
			}
			extinctedColor = encode(extinctedColor, Vec3f(1.f));
		}
		sPainter.setColor(extinctedColor);
		sPainter.drawSphericalRegion(poly.data(), StelPainter::SphericalPolygonDrawModeTextureFill, Q_NULLPTR, true, 5., vel);
	}
	if (photo)
		sPainter.resetTextureDisplayAdjustment();

#ifdef DEBUG_STELSKYIMAGE_TILE
	if (debugFont==Q_NULLPTR)
	{
		debugFont = &StelApp::getInstance().getFontManager().getStandardFont(StelApp::getInstance().getLocaleMgr().getSkyLanguage(), 12);
	}
	color.set(1.0,0.5,0.5,1.0);
	for (const auto& poly : skyConvexPolygons)
	{
		Vec3d win;
		Vec3d bary = poly->getPointInside();
		sPainter.getProjector()->project(bary,win);
		sPainter.drawText(debugFont, win[0], win[1], getAbsoluteImageURI());
		sPainter.enableTexture2d(false);
		sPainter.drawSphericalRegion(poly.get(), StelPainter::SphericalPolygonDrawModeBoundary, &color);
		sPainter.enableTexture2d(true);
	}
#endif

	return true;
}

// Return true if the tile is fully loaded and can be displayed
bool StelSkyImageTile::isReadyToDisplay() const
{
	return tex && tex->canBind();
}

// Load the tile from a valid QVariantMap
void StelSkyImageTile::loadFromQVariantMap(const QVariantMap& map)
{
	if (map.contains("imageCredits"))
	{
		QVariantMap dsCredits = map.value("imageCredits").toMap();
		dataSetCredits.shortCredits = dsCredits.value("short").toString();
		dataSetCredits.fullCredits = dsCredits.value("full").toString();
		dataSetCredits.infoURL = dsCredits.value("infoUrl").toString();
	}
	if (map.contains("serverCredits"))
	{
		QVariantMap sCredits = map.value("serverCredits").toMap();
		serverCredits.shortCredits = sCredits.value("short").toString();
		serverCredits.fullCredits = sCredits.value("full").toString();
		serverCredits.infoURL = sCredits.value("infoUrl").toString();
	}
	if (map.contains("description"))
	{
		htmlDescription = map.value("description").toString();
		if (parent()==Q_NULLPTR)
		{
			htmlDescription+= "<h3>URL: "+constructorUrl+"</h3>";
		}
	}
	else
	{
		if (parent()==Q_NULLPTR)
		{
			htmlDescription= "<h3>URL: "+constructorUrl+"</h3>";
		}
	}

	shortName = map.value("shortName").toString();
	if (shortName.isEmpty())
		shortName = "no name";
	bool ok=false;
	if (!map.contains("minResolution"))
		throw std::runtime_error(qPrintable(QString("minResolution is mandatory")));
	minResolution = map.value("minResolution").toFloat(&ok);
	if (!ok)
	{
		throw std::runtime_error(qPrintable(QString("minResolution expects a double value, found: \"%1\"").arg(map.value("minResolution").toString())));
	}

	if (map.contains("luminance"))
	{
		luminance = map.value("luminance").toFloat(&ok);
		if (!ok)
			throw std::runtime_error("luminance expects a float value");
		qWarning() << "luminance in preview JSON files is deprecated. Replace with maxBrightness.";
	}

	if (map.contains("maxBrightness"))
	{
		// maxBrightness is the maximum nebula brightness in Vmag/arcmin^2
		luminance = map.value("maxBrightness").toFloat(&ok);
		if (!ok)
			throw std::runtime_error("maxBrightness expects a float value");
		luminance = StelApp::getInstance().getCore()->getSkyDrawer()->surfaceBrightnessToLuminance(luminance);
	}

	if (map.contains("alphaBlend"))
	{
		alphaBlend = map.value("alphaBlend").toBool();
	}

	if (map.contains("halphaScale"))
	{
		photoHalphaScale = map.value("halphaScale").toFloat(&ok);
		if (!ok || !(photoHalphaScale > 0.f))
			photoHalphaScale = 0.f;
	}

	// Load the convex polygons (if any)
	QVariantList polyList = map.value("skyConvexPolygons").toList();
	if (polyList.empty())
		polyList = map.value("worldCoords").toList();
	else
		qWarning() << "skyConvexPolygons in preview JSON files is deprecated. Replace with worldCoords.";

	// Load the matching textures positions (if any)
	QVariantList texCoordList = map.value("textureCoords").toList();
	if (!texCoordList.isEmpty() && polyList.size()!=texCoordList.size())
			throw std::runtime_error("the number of convex polygons does not match the number of texture space polygon");

	for (int i=0;i<polyList.size();++i)
	{
		const QVariant& polyRaDec = polyList.at(i);
		QVector<Vec3d> vertices;
		const QList<QVariant> polyRaDecList=polyRaDec.toList();
		for (const auto& vRaDec : polyRaDecList)
		{
			const QVariantList vl = vRaDec.toList();
			Vec3d v;
			StelUtils::spheToRect(vl.at(0).toFloat(&ok)*M_PI_180f, vl.at(1).toFloat(&ok)*M_PI_180f, v);
			if (!ok)
				throw std::runtime_error("wrong Ra and Dec, expect a double value");
			vertices.append(v);
		}
		Q_ASSERT(vertices.size()==4);

		if (!texCoordList.isEmpty())
		{
			const QVariant& polyXY = texCoordList.at(i);
			QVector<Vec2f> texCoords;
			const QList<QVariant> polyXYlist=polyXY.toList();
			for (const auto& vXY : polyXYlist)
			{
				const QVariantList vl = vXY.toList();
				texCoords.append(Vec2f(vl.at(0).toFloat(&ok), vl.at(1).toFloat(&ok)));
				if (!ok)
					throw std::runtime_error("wrong X and Y, expect a double value");
			}
			Q_ASSERT(texCoords.size()==4);

			SphericalTexturedConvexPolygon* pol = new SphericalTexturedConvexPolygon(vertices, texCoords);
			Q_ASSERT(pol->checkValid());
			skyConvexPolygons.append(SphericalRegionP(pol));
		}
		else
		{
			SphericalConvexPolygon* pol = new SphericalConvexPolygon(vertices);
			Q_ASSERT(pol->checkValid());
			skyConvexPolygons.append(SphericalRegionP(pol));
		}
	}

	if (map.contains("imageUrl"))
	{
		QString imageUrl = map.value("imageUrl").toString();
		if (baseUrl.startsWith("http", Qt::CaseInsensitive))
		{
			absoluteImageURI = baseUrl+imageUrl;
		}
		else
		{
			absoluteImageURI = StelFileMgr::findFile(baseUrl+imageUrl);
			if (absoluteImageURI.isEmpty())
			{
				// Maybe the user meant a file in stellarium local files
				absoluteImageURI = imageUrl;
			}
		}
	}
	else
		noTexture = true;

	birthJD = map.value("birthJD", -1e10).toDouble();

	// This is a list of URLs to the child tiles or a list of already loaded map containing child information
	// (in this later case, the StelSkyImageTile objects will be created later)
	subTilesUrls = map.value("subTiles").toList();
	for (auto& variant : subTilesUrls)
	{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
		if (variant.metaType() == QMetaType(QMetaType::QVariantMap))
#else
		if (variant.type() == QVariant::Map)
#endif
		{
			// Check if the JSON object is a reference, i.e. if it contains a $ref key
			QVariantMap m = variant.toMap();
			if (m.size()==1 && m.contains("$ref"))
			{
				variant = QString(m["$ref"].toString());
			}
		}
	}
// 	if (subTilesUrls.size()>10)
// 	{
// 		qWarning() << "Large tiles number for " << shortName << ": " << subTilesUrls.size();
// 	}

	withAberration = map.value("withAberration", true).toBool();
}

// Convert the image information to a map following the JSON structure.
QVariantMap StelSkyImageTile::toQVariantMap() const
{
	QVariantMap res;

	// Image credits
	QVariantMap imCredits;
	if (!dataSetCredits.shortCredits.isEmpty())
		imCredits["short"]=dataSetCredits.shortCredits;
	if (!dataSetCredits.fullCredits.isEmpty())
		imCredits["full"]=dataSetCredits.fullCredits;
	if (!dataSetCredits.infoURL.isEmpty())
		imCredits["infoUrl"]=dataSetCredits.infoURL;
	if (!imCredits.empty())
		res["imageCredits"]=imCredits;

	// Server credits
	QVariantMap serCredits;
	if (!serverCredits.shortCredits.isEmpty())
		imCredits["short"]=serverCredits.shortCredits;
	if (!serverCredits.fullCredits.isEmpty())
		imCredits["full"]=serverCredits.fullCredits;
	if (!serverCredits.infoURL.isEmpty())
		imCredits["infoUrl"]=serverCredits.infoURL;
	if (!serCredits.empty())
		res["serverCredits"]=serCredits;

	// Misc
	if (!shortName.isEmpty())
		res["shortName"] = shortName;
	if (minResolution>0)
		res["minResolution"]=minResolution;
	if (luminance>0)
		res["maxBrightness"]=StelApp::getInstance().getCore()->getSkyDrawer()->luminanceToSurfacebrightness(luminance);
	if (alphaBlend)
		res["alphaBlend"]=true;
	if (noTexture==false)
		res["imageUrl"]=absoluteImageURI;
	if (birthJD>-1e10)
		res["birthJD"]=birthJD;
	res["withAberration"] = withAberration;

	// Polygons
	// TODO

	// textures positions
	// TODO

	if (!subTilesUrls.empty())
	{
		res["subTiles"] = subTilesUrls;
	}

	return res;
}
