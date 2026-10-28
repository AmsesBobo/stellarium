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

#include "NightLightsMap.hpp"
#include "StelApp.hpp"
#include "StelFileMgr.hpp"
#include "StelTranslator.hpp"
#include "StelUtils.hpp"

#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QSet>
#include <QtEndian>
#include <QDebug>
#include <algorithm>
#include <cmath>
#include <zlib.h>

namespace
{
const QString FILE_NAME = QStringLiteral("nightlights.average_viirs.v21_m_500m_s_20240101_20241231_go_epsg4326_v20250904.tif");
// The values are the radiance in nW/cm^2/sr times 10 ("converted from 0-200 to 0-2000"; the scale in the GDAL
// metadata of the file says 10 the other way round)
constexpr float VALUE_TO_RADIANCE = 0.1f;
constexpr double EARTH_RADIUS_KM = 6371.;
constexpr qint64 FIRST_HEADER_REQUEST = 196608;   // the index of this map (all its images) is about 130 kB
// The reduced image for the world map of the location dialog (5400 x 2700 pixels): its tiles are kept as ov_t*.bin
constexpr int OVERVIEW_WIDTH = 5400;
const QString OVERVIEW_PREFIX = QStringLiteral("ov_");

// Reading TIFF values of either byte order
struct TiffBytes
{
	const QByteArray& data;
	bool big;
	quint16 u16(qint64 pos) const
	{
		const auto* p = reinterpret_cast<const uchar*>(data.constData() + pos);
		return big ? qFromBigEndian<quint16>(p) : qFromLittleEndian<quint16>(p);
	}
	quint32 u32(qint64 pos) const
	{
		const auto* p = reinterpret_cast<const uchar*>(data.constData() + pos);
		return big ? qFromBigEndian<quint32>(p) : qFromLittleEndian<quint32>(p);
	}
	quint64 u64(qint64 pos) const
	{
		const auto* p = reinterpret_cast<const uchar*>(data.constData() + pos);
		return big ? qFromBigEndian<quint64>(p) : qFromLittleEndian<quint64>(p);
	}
	double f64(qint64 pos) const
	{
		const quint64 v = u64(pos);
		double d;
		memcpy(&d, &v, sizeof d);
		return d;
	}
};

int typeSize(int type)
{
	switch (type)
	{
		case 1: case 2: case 6: case 7: return 1;   // BYTE, ASCII, SBYTE, UNDEFINED
		case 3: case 8: return 2;                   // SHORT, SSHORT
		case 4: case 9: case 11: case 13: return 4; // LONG, SLONG, FLOAT, IFD
		case 5: case 10: case 12: case 16: case 17: case 18: return 8;  // RATIONAL, SRATIONAL, DOUBLE, LONG8, SLONG8, IFD8
		default: return 1;
	}
}
}

NightLightsMap::NightLightsMap(QObject* parent)
	: QObject(parent)
{
	cacheDir = StelFileMgr::getCacheDir() + "/lightdomes/viirs-2024";
	// A whole copy of the map: in the user directory, or in the cache
	const QStringList candidates = {StelFileMgr::getUserDir() + "/lightdomes/" + FILE_NAME,
	                                StelFileMgr::getCacheDir() + "/lightdomes/viirs/" + FILE_NAME};
	for (const QString& path : candidates)
	{
		QFile f(path);
		if (!f.open(QIODevice::ReadOnly))
			continue;
		qint64 size = FIRST_HEADER_REQUEST;
		for (int attempt=0; attempt<3; ++attempt)
		{
			f.seek(0);
			const QByteArray header = f.read(size);
			qint64 needed = 0;
			QString error;
			std::shared_ptr<Index> idx, ov;
			if (parseLevels(header, idx, ov, &needed, &error) && (needed <= size || needed > f.size() || attempt == 2))
			{
				index = idx;
				overviewIndex = ov;
				fullFile = path;
				qInfo() << "Night lights (VIIRS): using the whole map" << QDir::toNativeSeparators(path);
				return;
			}
			if (needed <= size || needed > f.size())
			{
				qWarning() << "Night lights (VIIRS): cannot read" << QDir::toNativeSeparators(path) << error;
				break;
			}
			size = needed;
		}
	}
	// The index of a former download
	QFile f(cacheDir + "/header.bin");
	if (f.open(QIODevice::ReadOnly))
	{
		qint64 needed = 0;
		QString error;
		std::shared_ptr<Index> idx, ov;
		if (parseLevels(f.readAll(), idx, ov, &needed, &error))
		{
			index = idx;
			overviewIndex = ov;
			headerNeeded = needed;  // an older, shorter index: read again for the overview
		}
		else
			qWarning() << "Night lights (VIIRS): the saved index is not usable:" << error;
	}
}

NightLightsMap::~NightLightsMap()
{
	if (headerReply)
		headerReply->abort();
	for (auto* r : std::as_const(pending))
		r->abort();
}

QString NightLightsMap::sourceUrl()
{
	return QStringLiteral("https://zenodo.org/records/17294744/files/") + FILE_NAME + QStringLiteral("?download=1");
}

QString NightLightsMap::attribution()
{
	return q_("VIIRS night lights 2024 (VNL V2): Earth Observation Group, Payne Institute for Public Policy, Colorado School of Mines (Elvidge et al. 2021); via OpenGeoHub (Hengl 2025), CC BY 4.0");
}

bool NightLightsMap::parseLevels(const QByteArray& header, std::shared_ptr<Index>& full, std::shared_ptr<Index>& overview, qint64* needed, QString* error)
{
	*needed = 0;
	full.reset();
	overview.reset();
	if (header.size() < 8)
	{
		*needed = 8;
		return false;
	}
	const bool big = header.startsWith("MM");
	if (!big && !header.startsWith("II"))
	{
		*error = "not a TIFF file";
		return false;
	}
	const TiffBytes b{header, big};
	if (b.u16(2) != 42)
	{
		*error = "not a classic TIFF file (BigTIFF is not supported)";
		return false;
	}
	qint64 next = b.u32(4);
	full = parseIfd(header, next, nullptr, needed, error, &next);
	if (!full)
		return false;
	// the reduced images (overviews) that follow: the one of the size of the world map of the location dialog
	while (next > 0)
	{
		auto level = parseIfd(header, next, full.get(), needed, error, &next);
		if (!level)
		{
			// not usable, or more to read (needed): no overview yet, but the full image is there
			error->clear();
			return true;
		}
		if (level->width >= OVERVIEW_WIDTH && (!overview || level->width < overview->width))
			overview = level;
	}
	return true;
}

std::shared_ptr<NightLightsMap::Index> NightLightsMap::parseIfd(const QByteArray& header, qint64 ifd, const Index* base, qint64* needed, QString* error, qint64* nextIfd)
{
	*nextIfd = 0;
	const bool big = header.startsWith("MM");
	const TiffBytes b{header, big};
	if (ifd + 2 > header.size())
	{
		*needed = ifd + 2 + 12*64 + 4;
		return nullptr;
	}
	const int entries = b.u16(ifd);
	if (ifd + 2 + 12*entries + 4 > header.size())
	{
		*needed = ifd + 2 + 12*entries + 4;
		return nullptr;
	}
	*nextIfd = b.u32(ifd + 2 + 12*entries);
	auto idx = std::make_shared<Index>();
	int compression = 1, bits = 0, sampleFormat = 1, samples = 1, planar = 1;
	std::vector<double> pixelScale, tiePoint;
	qint64 neededEnd = 0;
	// The values of an entry (or nothing when they are not within the header yet)
	const auto values = [&](qint64 entry, std::vector<double>* out, std::vector<quint64>* outInt) -> bool {
		const int type = b.u16(entry + 2);
		const qint64 count = b.u32(entry + 4);
		const int size = typeSize(type);
		const qint64 bytes = count*size;
		const qint64 pos = bytes <= 4 ? entry + 8 : static_cast<qint64>(b.u32(entry + 8));
		if (pos + bytes > header.size())
		{
			neededEnd = qMax(neededEnd, pos + bytes);
			return false;
		}
		for (qint64 i=0; i<count; ++i)
		{
			const qint64 p = pos + i*size;
			quint64 v = 0;
			double d = 0.;
			switch (type)
			{
				case 3: v = b.u16(p); d = v; break;
				case 4: v = b.u32(p); d = v; break;
				case 16: v = b.u64(p); d = static_cast<double>(v); break;
				case 12: d = b.f64(p); v = static_cast<quint64>(qMax(0., d)); break;
				default: v = static_cast<uchar>(header.at(static_cast<int>(p))); d = v; break;
			}
			if (out)
				out->push_back(d);
			if (outInt)
				outInt->push_back(v);
		}
		return true;
	};
	for (int i=0; i<entries; ++i)
	{
		const qint64 entry = ifd + 2 + 12*i;
		const int tag = b.u16(entry);
		std::vector<quint64> v;
		switch (tag)
		{
			case 256: if (values(entry, nullptr, &v)) idx->width = static_cast<int>(v[0]); break;
			case 257: if (values(entry, nullptr, &v)) idx->height = static_cast<int>(v[0]); break;
			case 258: if (values(entry, nullptr, &v)) bits = static_cast<int>(v[0]); break;
			case 259: if (values(entry, nullptr, &v)) compression = static_cast<int>(v[0]); break;
			case 277: if (values(entry, nullptr, &v)) samples = static_cast<int>(v[0]); break;
			case 284: if (values(entry, nullptr, &v)) planar = static_cast<int>(v[0]); break;
			case 322: if (values(entry, nullptr, &v)) idx->tileWidth = static_cast<int>(v[0]); break;
			case 323: if (values(entry, nullptr, &v)) idx->tileHeight = static_cast<int>(v[0]); break;
			case 324: values(entry, nullptr, &idx->offsets); break;
			case 325: values(entry, nullptr, &idx->counts); break;
			case 339: if (values(entry, nullptr, &v)) sampleFormat = static_cast<int>(v[0]); break;
			case 33550: values(entry, &pixelScale, nullptr); break;
			case 33922: values(entry, &tiePoint, nullptr); break;
			default: break;
		}
	}
	if (neededEnd > 0)
	{
		*needed = neededEnd;
		return nullptr;
	}
	if (bits != 16 || sampleFormat != 2 || samples != 1 || planar != 1 || (compression != 8 && compression != 32946 && compression != 1))
	{
		*error = QString("unexpected image format (bits %1, sample format %2, compression %3)").arg(bits).arg(sampleFormat).arg(compression);
		return nullptr;
	}
	if (base)
	{
		// an overview: the same area as the full image
		pixelScale = {base->dLon*base->width/qMax(1, idx->width), base->dLat*base->height/qMax(1, idx->height)};
		tiePoint = {0., 0., 0., base->lon0, base->lat0, 0.};
	}
	if (idx->width <= 0 || idx->height <= 0 || idx->tileWidth <= 0 || idx->tileHeight <= 0 || pixelScale.size() < 2 || tiePoint.size() < 6)
	{
		*error = "no tiles or no georeference";
		return nullptr;
	}
	idx->tilesAcross = (idx->width + idx->tileWidth - 1)/idx->tileWidth;
	idx->tilesDown = (idx->height + idx->tileHeight - 1)/idx->tileHeight;
	if (static_cast<int>(idx->offsets.size()) < idx->tilesAcross*idx->tilesDown || idx->counts.size() != idx->offsets.size())
	{
		*error = "incomplete tile index";
		return nullptr;
	}
	idx->dLon = pixelScale[0];
	idx->dLat = pixelScale[1];
	idx->lon0 = tiePoint[3] - tiePoint[0]*idx->dLon;
	idx->lat0 = tiePoint[4] + tiePoint[1]*idx->dLat;
	idx->compressed = compression != 1;
	idx->bigEndian = big;
	return idx;
}

QList<QPoint> NightLightsMap::tilesFor(const Index& idx, double lat, double lon, double radiusKm)
{
	QList<QPoint> tiles;
	const double dLatDeg = radiusKm/111.2;
	const double north = qMin(lat + dLatDeg, 89.9), south = qMax(lat - dLatDeg, -89.9);
	const double cosMin = qMax(0.01, std::cos(qMax(std::fabs(north), std::fabs(south))*M_PI/180.));
	const double dLonDeg = qMin(180., radiusKm/(111.32*cosMin));
	const int r0 = qMax(0, static_cast<int>(std::floor((idx.lat0 - north)/idx.dLat)));
	const int r1 = qMin(idx.height - 1, static_cast<int>(std::floor((idx.lat0 - south)/idx.dLat)));
	if (r0 > r1)
		return tiles;
	int c0 = static_cast<int>(std::floor((lon - dLonDeg - idx.lon0)/idx.dLon));
	int c1 = static_cast<int>(std::floor((lon + dLonDeg - idx.lon0)/idx.dLon));
	if (c1 - c0 + 1 >= idx.width)
	{
		c0 = 0;
		c1 = idx.width - 1;
	}
	const auto floorDiv = [](int a, int b) {return a >= 0 ? a/b : -((-a + b - 1)/b);};
	QSet<int> seen;
	for (int ty = r0/idx.tileHeight; ty <= r1/idx.tileHeight; ++ty)
		for (int t = floorDiv(c0, idx.tileWidth); t <= floorDiv(c1, idx.tileWidth); ++t)
		{
			const int tx = ((t % idx.tilesAcross) + idx.tilesAcross) % idx.tilesAcross;
			if (!seen.contains(idx.tileIndex(tx, ty)))
			{
				seen.insert(idx.tileIndex(tx, ty));
				tiles << QPoint(tx, ty);
			}
		}
	return tiles;
}

QString NightLightsMap::tileFile(const QString& dir, int tx, int ty, const QString& prefix)
{
	return QString("%1/%2t%3_%4.bin").arg(dir, prefix).arg(ty).arg(tx);
}

bool NightLightsMap::hasRegion(double lat, double lon, double radiusKm) const
{
	if (!index)
		return false;
	if (!fullFile.isEmpty())
		return true;
	for (const QPoint& t : tilesFor(*index, lat, lon, radiusKm))
		if (index->counts[static_cast<size_t>(index->tileIndex(t.x(), t.y()))] > 0 && !QFileInfo::exists(tileFile(cacheDir, t.x(), t.y())))
			return false;
	return true;
}

qint64 NightLightsMap::regionDownloadSize(double lat, double lon, double radiusKm) const
{
	if (!index)
		return -1;
	if (!fullFile.isEmpty())
		return 0;
	qint64 size = 0;
	for (const QPoint& t : tilesFor(*index, lat, lon, radiusKm))
	{
		const size_t i = static_cast<size_t>(index->tileIndex(t.x(), t.y()));
		if (index->counts[i] > 0 && !QFileInfo::exists(tileFile(cacheDir, t.x(), t.y())))
			size += static_cast<qint64>(index->counts[i]);
	}
	return size;
}

NightLightsMap::Snapshot NightLightsMap::snapshot() const
{
	return Snapshot{index, cacheDir, fullFile};
}

QByteArray NightLightsMap::tileBytes(const Index& idx, const QString& fullFile, const QString& tileDir, const QString& prefix, int tx, int ty)
{
	const size_t i = static_cast<size_t>(idx.tileIndex(tx, ty));
	const qint64 count = static_cast<qint64>(idx.counts[i]);
	if (count <= 0)
		return QByteArray();
	if (!fullFile.isEmpty())
	{
		QFile f(fullFile);
		if (!f.open(QIODevice::ReadOnly) || !f.seek(static_cast<qint64>(idx.offsets[i])))
			return QByteArray();
		return f.read(count);
	}
	QFile f(tileFile(tileDir, tx, ty, prefix));
	if (!f.open(QIODevice::ReadOnly))
		return QByteArray();
	return f.readAll();
}

bool NightLightsMap::decodeTile(const Index& idx, const QByteArray& bytes, std::vector<qint16>& values)
{
	const size_t tileSize = static_cast<size_t>(idx.tileWidth)*idx.tileHeight;
	values.resize(tileSize);
	if (idx.compressed)
	{
		uLongf length = static_cast<uLongf>(tileSize*sizeof(qint16));
		if (uncompress(reinterpret_cast<Bytef*>(values.data()), &length, reinterpret_cast<const Bytef*>(bytes.constData()), static_cast<uLong>(bytes.size())) != Z_OK
		    || length != tileSize*sizeof(qint16))
			return false;
	}
	else
	{
		if (static_cast<size_t>(bytes.size()) != tileSize*sizeof(qint16))
			return false;
		memcpy(values.data(), bytes.constData(), tileSize*sizeof(qint16));
	}
	if (idx.bigEndian != (Q_BYTE_ORDER == Q_BIG_ENDIAN))
		for (auto& v : values)
			v = static_cast<qint16>(qbswap(static_cast<quint16>(v)));
	return true;
}

bool NightLightsMap::hasOverview() const
{
	if (!overviewIndex)
		return false;
	if (!fullFile.isEmpty())
		return true;
	for (int ty=0; ty<overviewIndex->tilesDown; ++ty)
		for (int tx=0; tx<overviewIndex->tilesAcross; ++tx)
			if (overviewIndex->counts[static_cast<size_t>(overviewIndex->tileIndex(tx, ty))] > 0
			    && !QFileInfo::exists(tileFile(cacheDir, tx, ty, OVERVIEW_PREFIX)))
				return false;
	return true;
}

QImage NightLightsMap::composeNightMap(const QImage& dayMap) const
{
	if (!hasOverview() || dayMap.isNull())
		return QImage();
	const Index& ov = *overviewIndex;
	// the reduced image of the lights (radiance times 10)
	std::vector<qint16> lights(static_cast<size_t>(ov.width)*ov.height, 0);
	std::vector<qint16> values;
	for (int ty=0; ty<ov.tilesDown; ++ty)
		for (int tx=0; tx<ov.tilesAcross; ++tx)
		{
			const QByteArray bytes = tileBytes(ov, fullFile, cacheDir, OVERVIEW_PREFIX, tx, ty);
			if (bytes.isEmpty() || !decodeTile(ov, bytes, values))
				continue;
			for (int r=0; r<ov.tileHeight && ty*ov.tileHeight + r < ov.height; ++r)
				for (int c=0; c<ov.tileWidth && tx*ov.tileWidth + c < ov.width; ++c)
					lights[static_cast<size_t>(ty*ov.tileHeight + r)*ov.width + tx*ov.tileWidth + c] = values[static_cast<size_t>(r)*ov.tileWidth + c];
		}
	// Night: the day map darkened, the lights on a logarithmic scale from 0.15 to 40 nW/cm^2/sr (the reduced image
	// averages the pixels: towns are fainter in it than in the full image), from dark orange to yellow and white
	QImage night = dayMap.convertToFormat(QImage::Format_RGB32);
	const int W = night.width(), H = night.height();
	const double logLow = std::log10(0.15), logRange = std::log10(40.) - logLow;
	for (int y=0; y<H; ++y)
	{
		const double lat = 90. - (y + 0.5)*180./H;
		const int row = static_cast<int>(std::floor((ov.lat0 - lat)/ov.dLat));
		QRgb* line = reinterpret_cast<QRgb*>(night.scanLine(y));
		for (int x=0; x<W; ++x)
		{
			double v = 0.;
			if (row >= 0 && row < ov.height)
			{
				const double lon = -180. + (x + 0.5)*360./W;
				int col = static_cast<int>(std::floor((lon - ov.lon0)/ov.dLon));
				col = ((col % ov.width) + ov.width) % ov.width;
				const int raw = lights[static_cast<size_t>(row)*ov.width + col];
				if (raw > 0)
					v = qBound(0., (std::log10(raw*static_cast<double>(VALUE_TO_RADIANCE)) - logLow)/logRange, 1.);
			}
			const double glow = std::sqrt(v);
			const double red = qMin(1., 1.5*v), green = qBound(0., 1.2*v - 0.05, 1.), blue = qBound(0., 1.4*v - 0.6, 1.);
			const QRgb p = line[x];
			const auto mix = [&](int base, double light) {return qBound(0, qRound(0.35*base + 255.*light*glow), 255);};
			line[x] = qRgb(mix(qRed(p), red), mix(qGreen(p), green), mix(qBlue(p), blue));
		}
	}
	return night;
}

std::vector<NightLightsMap::Pixel> NightLightsMap::readRegion(const Snapshot& snap, double lat, double lon, double radiusKm, float* pixelSizeDeg)
{
	std::vector<Pixel> pixels;
	if (!snap.index)
		return pixels;
	const Index& idx = *snap.index;
	if (pixelSizeDeg)
		*pixelSizeDeg = static_cast<float>(idx.dLat);
	const double cosMax = std::cos(radiusKm/EARTH_RADIUS_KM);
	const double sinLat0 = std::sin(lat*M_PI/180.), cosLat0 = std::cos(lat*M_PI/180.);
	std::vector<qint16> values;
	for (const QPoint& t : tilesFor(idx, lat, lon, radiusKm))
	{
		const QByteArray bytes = tileBytes(idx, snap.fullFile, snap.tileDir, QString(), t.x(), t.y());
		if (bytes.isEmpty())
			continue;
		if (!decodeTile(idx, bytes, values))
		{
			qWarning() << "Night lights (VIIRS): damaged tile" << t;
			continue;
		}
		const int row0 = t.y()*idx.tileHeight, col0 = t.x()*idx.tileWidth;
		for (int r=0; r<idx.tileHeight && row0 + r < idx.height; ++r)
		{
			const double pLat = idx.lat0 - (row0 + r + 0.5)*idx.dLat;
			const double sinLat = std::sin(pLat*M_PI/180.), cosLat = std::cos(pLat*M_PI/180.);
			if (sinLat*sinLat0 + cosLat*cosLat0 < cosMax)
				continue;  // the whole row is too far (north or south)
			const qint16* line = values.data() + static_cast<size_t>(r)*idx.tileWidth;
			for (int c=0; c<idx.tileWidth && col0 + c < idx.width; ++c)
			{
				if (line[c] <= 0)
					continue;  // dark, or no data (-32768)
				double pLon = idx.lon0 + (col0 + c + 0.5)*idx.dLon;
				const double cosD = sinLat*sinLat0 + cosLat*cosLat0*std::cos((pLon - lon)*M_PI/180.);
				if (cosD < cosMax)
					continue;
				pLon = std::fmod(pLon + 540., 360.) - 180.;
				pixels.push_back({static_cast<float>(pLat), static_cast<float>(pLon), VALUE_TO_RADIANCE*line[c]});
			}
		}
	}
	return pixels;
}

void NightLightsMap::download(double lat, double lon, double radiusKm)
{
	if (isDownloading())
		return;
	wantLat = lat;
	wantLon = lon;
	wantRadius = radiusKm;
	if (!fullFile.isEmpty() && index)
	{
		emit downloadFinished(true, QString());
		return;
	}
	job = JobRegion;
	if (!index)
		requestHeader(FIRST_HEADER_REQUEST);
	else
		startJob();
}

void NightLightsMap::downloadOverview()
{
	if (isDownloading())
		return;
	if (hasOverview())
	{
		emit overviewChanged();
		return;
	}
	job = JobOverview;
	if (!index || (!overviewIndex && headerNeeded > 0))
		requestHeader(qMax(FIRST_HEADER_REQUEST, headerNeeded));
	else
		startJob();
}

void NightLightsMap::startJob()
{
	if (job == JobOverview)
	{
		if (!overviewIndex)
		{
			fail(q_("The map has no reduced image of the whole world"));
			return;
		}
		QList<QPoint> all;
		for (int ty=0; ty<overviewIndex->tilesDown; ++ty)
			for (int tx=0; tx<overviewIndex->tilesAcross; ++tx)
				all << QPoint(tx, ty);
		requestTiles(overviewIndex, all, OVERVIEW_PREFIX);
	}
	else
		requestTiles(index, tilesFor(*index, wantLat, wantLon, wantRadius), QString());
}

namespace
{
QNetworkRequest rangeRequest(qint64 first, qint64 last)
{
	QNetworkRequest req{QUrl(NightLightsMap::sourceUrl())};
	req.setRawHeader("Range", QString("bytes=%1-%2").arg(first).arg(last).toLatin1());
	req.setRawHeader("User-Agent", StelUtils::getUserAgentString().toUtf8());
	req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
	return req;
}

// A server that ignores the range would send the whole map (64 MB): stop it at once
void guardRange(QNetworkReply* reply)
{
	QObject::connect(reply, &QNetworkReply::metaDataChanged, reply, [reply]() {
		const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
		if (status == 200)
		{
			reply->setProperty("rangeIgnored", true);
			reply->abort();
		}
	});
}
}

void NightLightsMap::requestHeader(qint64 size)
{
	headerReply = StelApp::getInstance().getNetworkAccessManager()->get(rangeRequest(0, size - 1));
	guardRange(headerReply);
	headerReply->setProperty("requested", size);
	connect(headerReply, &QNetworkReply::finished, this, &NightLightsMap::headerReceived);
	emit downloadProgress(0, 0);
}

void NightLightsMap::headerReceived()
{
	QNetworkReply* reply = headerReply;
	if (!reply || reply != sender())
		return;
	headerReply = nullptr;
	reply->deleteLater();
	if (reply->property("rangeIgnored").toBool())
	{
		fail(q_("The server sends only the whole map (64 MB), not parts of it. You can download the whole map yourself and put it into the folder lightdomes of your user directory."));
		return;
	}
	if (reply->error() != QNetworkReply::NoError)
	{
		fail(reply->errorString());
		return;
	}
	const QByteArray data = reply->readAll();
	qint64 needed = 0;
	QString error;
	std::shared_ptr<Index> idx, ov;
	const bool parsed = parseLevels(data, idx, ov, &needed, &error);
	if (parsed && !ov && job == JobOverview && needed > data.size() && needed < 16*1024*1024)
	{
		requestHeader(needed);
		return;
	}
	if (!parsed)
	{
		if (needed > data.size() && needed > reply->property("requested").toLongLong() && needed < 16*1024*1024)
		{
			requestHeader(needed);
			return;
		}
		fail(q_("The map cannot be read: %1").arg(error));
		return;
	}
	QDir().mkpath(cacheDir);
	QFile f(cacheDir + "/header.bin");
	if (f.open(QIODevice::WriteOnly))
		f.write(data);
	index = idx;
	overviewIndex = ov;
	headerNeeded = 0;
	startJob();
}

void NightLightsMap::requestTiles(std::shared_ptr<const Index> idx, const QList<QPoint>& tiles, const QString& prefix)
{
	QList<QPoint> missing;
	for (const QPoint& t : tiles)
		if (idx->counts[static_cast<size_t>(idx->tileIndex(t.x(), t.y()))] > 0 && !QFileInfo::exists(tileFile(cacheDir, t.x(), t.y(), prefix)))
			missing << t;
	tilesTotal = static_cast<int>(missing.size());
	tilesDone = 0;
	if (missing.isEmpty())
	{
		finishJob();
		return;
	}
	QDir().mkpath(cacheDir);
	std::sort(missing.begin(), missing.end(), [](const QPoint& a, const QPoint& b) {return a.y() < b.y() || (a.y() == b.y() && a.x() < b.x());});
	// One request for each run of neighbouring tiles of a row (they follow each other in the file)
	int i = 0;
	while (i < missing.size())
	{
		int j = i;
		while (j + 1 < missing.size() && missing[j + 1].y() == missing[i].y() && missing[j + 1].x() == missing[j].x() + 1)
			++j;
		const int ty = missing[i].y(), tx0 = missing[i].x(), tx1 = missing[j].x();
		const size_t first = static_cast<size_t>(idx->tileIndex(tx0, ty)), last = static_cast<size_t>(idx->tileIndex(tx1, ty));
		qint64 start = static_cast<qint64>(idx->offsets[first]), end = 0;
		for (size_t k=first; k<=last; ++k)
		{
			start = qMin(start, static_cast<qint64>(idx->offsets[k]));
			end = qMax(end, static_cast<qint64>(idx->offsets[k] + idx->counts[k]));
		}
		QNetworkReply* reply = StelApp::getInstance().getNetworkAccessManager()->get(rangeRequest(start, end - 1));
		guardRange(reply);
		reply->setProperty("start", start);
		pending << reply;
		connect(reply, &QNetworkReply::finished, this, [this, reply, idx, prefix, ty, tx0, tx1]() {tilesReceived(reply, *idx, prefix, ty, tx0, tx1);});
		i = j + 1;
	}
	emit downloadProgress(0, tilesTotal);
}

void NightLightsMap::tilesReceived(QNetworkReply* reply, const Index& idx, const QString& prefix, int ty, int tx0, int tx1)
{
	reply->deleteLater();
	if (!pending.removeOne(reply))
		return;  // cancelled
	if (reply->property("rangeIgnored").toBool())
	{
		fail(q_("The server sends only the whole map (64 MB), not parts of it. You can download the whole map yourself and put it into the folder lightdomes of your user directory."));
		return;
	}
	if (reply->error() != QNetworkReply::NoError)
	{
		fail(reply->errorString());
		return;
	}
	const QByteArray data = reply->readAll();
	const qint64 start = reply->property("start").toLongLong();
	for (int tx=tx0; tx<=tx1; ++tx)
	{
		const size_t k = static_cast<size_t>(idx.tileIndex(tx, ty));
		const qint64 pos = static_cast<qint64>(idx.offsets[k]) - start;
		const qint64 count = static_cast<qint64>(idx.counts[k]);
		if (pos < 0 || pos + count > data.size())
		{
			fail(q_("The server sent less than asked for"));
			return;
		}
		QFile f(tileFile(cacheDir, tx, ty, prefix));
		if (!f.open(QIODevice::WriteOnly) || f.write(data.constData() + pos, count) != count)
		{
			f.remove();
			fail(q_("Cannot write into the cache directory"));
			return;
		}
	}
	tilesDone += tx1 - tx0 + 1;
	emit downloadProgress(tilesDone, tilesTotal);
	if (pending.isEmpty())
		finishJob();
}

void NightLightsMap::finishJob()
{
	const Job done = job;
	job = JobNone;
	emit downloadFinished(true, QString());
	if (done == JobOverview)
		emit overviewChanged();
}

void NightLightsMap::fail(const QString& message)
{
	const QList<QNetworkReply*> replies = pending;
	pending.clear();
	for (auto* r : replies)
		r->abort();
	if (headerReply)
	{
		QNetworkReply* r = headerReply;
		headerReply = nullptr;
		r->abort();
	}
	qWarning() << "Night lights (VIIRS): download failed:" << message;
	const Job failed = job;
	job = JobNone;
	emit downloadFinished(false, message);
	if (failed == JobOverview)
		emit overviewChanged();
}

void NightLightsMap::cancel()
{
	if (isDownloading())
		fail(q_("Cancelled"));
}
