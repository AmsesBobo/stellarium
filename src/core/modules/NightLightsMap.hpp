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

#ifndef NIGHTLIGHTSMAP_HPP
#define NIGHTLIGHTSMAP_HPP

#include <QObject>
#include <QString>
#include <QByteArray>
#include <QList>
#include <QPoint>
#include <QImage>
#include <memory>
#include <vector>

class QNetworkReply;

//! The night lights of the Earth seen by the VIIRS satellite (the Day/Night Band of Suomi NPP): the annual map of
//! the Earth Observation Group (Payne Institute, Colorado School of Mines; Elvidge et al. 2021, Remote Sensing 13,
//! 922), VNL V2 "average masked" of 2024, as made available by OpenGeoHub (Hengl 2025, Zenodo 17294744, CC BY 4.0):
//! a Cloud Optimized GeoTIFF of 86400 x 35849 pixels of 15 arc seconds (87.37 N to 62 S), the radiance of the light
//! sent upwards in nW/cm^2/sr times 10 (16 bit integers), in deflate compressed tiles of 512 x 512 pixels.
//!
//! Only the tiles around the observer are downloaded, when the user asks for them (HTTP range requests: about
//! 100 kB for the index of the tiles, and below 1 MB for 400 km around a place), and kept in the cache directory.
//! A whole copy of the file in the user directory (lightdomes/) or in the cache (lightdomes/viirs/) is used instead
//! when there is one: no download then.
class NightLightsMap : public QObject
{
	Q_OBJECT
public:
	//! A lit pixel: its centre (degrees) and radiance (nW/cm^2/sr), and its size (degrees)
	struct Pixel
	{
		float lat, lon, radiance;
	};
	//! The layout of the file (the first image: the full resolution)
	struct Index
	{
		int width = 0, height = 0, tileWidth = 0, tileHeight = 0;
		int tilesAcross = 0, tilesDown = 0;
		double lon0 = 0., lat0 = 0.;       //!< corner of the first pixel (north west), degrees
		double dLon = 0., dLat = 0.;       //!< pixel size, degrees
		std::vector<quint64> offsets, counts;
		bool compressed = true;            //!< deflate (else raw)
		bool bigEndian = false;
		int tileIndex(int tx, int ty) const {return ty*tilesAcross + tx;}
	};
	//! What a background thread needs to read the data (copied in the main thread)
	struct Snapshot
	{
		std::shared_ptr<const Index> index;
		QString tileDir;     //!< the downloaded tiles
		QString fullFile;    //!< a whole copy of the map, or empty
	};

	explicit NightLightsMap(QObject* parent = nullptr);
	~NightLightsMap() override;

	//! The address of the map (Zenodo)
	static QString sourceUrl();
	//! The credits of the data, for the dialog
	static QString attribution();

	//! Whether the data within radiusKm of the place are there (downloaded, or a whole copy)
	bool hasRegion(double lat, double lon, double radiusKm) const;
	//! The bytes still to download for the region; -1 when not known yet (the index is downloaded first)
	qint64 regionDownloadSize(double lat, double lon, double radiusKm) const;
	bool isDownloading() const {return !pending.isEmpty() || headerReply;}
	//! Whether a whole copy of the map is used
	bool hasFullFile() const {return !fullFile.isEmpty() && index;}
	//! Whether the reduced image of the whole world (5400 pixels wide, 0.36 MB) is there
	bool hasOverview() const;
	//! The world map of the location dialog (equirectangular, 90 N to 90 S) at night: darkened, with the lights of
	//! the reduced image (a null image when it is not there)
	QImage composeNightMap(const QImage& dayMap) const;

	Snapshot snapshot() const;
	//! The lit pixels within radiusKm of the place (in any thread: it only reads files)
	static std::vector<Pixel> readRegion(const Snapshot& snap, double lat, double lon, double radiusKm, float* pixelSizeDeg = nullptr);

public slots:
	//! Download what is missing within radiusKm of the place
	void download(double lat, double lon, double radiusKm);
	//! Download the reduced image of the whole world
	void downloadOverview();
	void cancel();

signals:
	void downloadProgress(int done, int total);
	//! The download ended (ok, or the reason it failed)
	void downloadFinished(bool ok, const QString& message);
	//! The download of the reduced image ended (well or not: see hasOverview())
	void overviewChanged();

private:
	enum Job {JobNone, JobRegion, JobOverview};
	//! The full image and the overview of the size of the world map, from the beginning of the file (needed: the
	//! bytes to read first when they are not all there)
	static bool parseLevels(const QByteArray& header, std::shared_ptr<Index>& full, std::shared_ptr<Index>& overview, qint64* needed, QString* error);
	static std::shared_ptr<Index> parseIfd(const QByteArray& header, qint64 ifd, const Index* base, qint64* needed, QString* error, qint64* nextIfd);
	static QList<QPoint> tilesFor(const Index& idx, double lat, double lon, double radiusKm);
	static QString tileFile(const QString& dir, int tx, int ty, const QString& prefix = QString());
	static QByteArray tileBytes(const Index& idx, const QString& fullFile, const QString& tileDir, const QString& prefix, int tx, int ty);
	//! Decompress a tile into values (tileWidth x tileHeight)
	static bool decodeTile(const Index& idx, const QByteArray& bytes, std::vector<qint16>& values);
	void requestHeader(qint64 size);
	void headerReceived();
	void startJob();
	void finishJob();
	void requestTiles(std::shared_ptr<const Index> idx, const QList<QPoint>& tiles, const QString& prefix);
	void tilesReceived(QNetworkReply* reply, const Index& idx, const QString& prefix, int ty, int tx0, int tx1);
	void fail(const QString& message);

	std::shared_ptr<const Index> index;
	std::shared_ptr<const Index> overviewIndex;
	Job job = JobNone;
	qint64 headerNeeded = 0;  //!< bytes of the index still missing for the overview
	QString cacheDir;     //!< lightdomes/viirs-2024
	QString fullFile;
	// the download in progress
	QNetworkReply* headerReply = nullptr;
	QList<QNetworkReply*> pending;
	double wantLat = 0., wantLon = 0., wantRadius = 0.;
	int tilesTotal = 0, tilesDone = 0;
};

#endif // NIGHTLIGHTSMAP_HPP
