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

#ifndef PHOTOGRAPHICMODEDIALOG_HPP
#define PHOTOGRAPHICMODEDIALOG_HPP

#include "StelDialog.hpp"

#include <QVector>

class QComboBox;

class Ui_PhotographicModeDialogForm;
class QComboBox;

//! Settings of the photographic (camera-like) rendering mode:
//! camera settings (exposure time, aperture, ISO, exposure compensation)
//! and the look of star images (PSF width, seeing, lens halo, diffraction spikes).
class PhotographicModeDialog : public StelDialog
{
	Q_OBJECT

public:
	//! @param parent the graphics widget of the main view (needed when created early, as in StelGui::init())
	PhotographicModeDialog(QObject* parent = nullptr);
	~PhotographicModeDialog() override;

public slots:
	void retranslate() override;

protected:
	void createDialogContent() override;

private slots:
	//! Show the current camera settings in the combo boxes and the EV readout
	void updateCameraWidgets();
	//! Show the current star image settings in the widgets
	void updateStarWidgets();
	//! Show the focal length of the lens for the current field of view
	void updateFocalLength();
	//! Show the towns of the light domes in the table, and the light at the zenith
	void updateLightDomeWidgets();
	//! The towns of the table to the light domes
	void applyLightDomeTable();
	//! Show the towns found in the location database
	void updateDatabaseTownWidgets();
	//! Show the state of the satellite data
	void updateSatelliteWidgets();

private:
	//! Fill a list with the kinds of street lamps (LightDomes::Lamp as data)
	static void fillLampComboBox(QComboBox* box);
	//! Fill the list of the sources of the light of the towns
	void fillSourceComboBox();
	Ui_PhotographicModeDialogForm *ui;
	//! The tooltip of the noise list for a digital sensor (with a film the list sets the grain)
	QString sensorNoiseToolTip;

	//! Standard camera values offered in the combo boxes
	static const QVector<double> exposureTimes;
	static const QVector<double> apertures;
	static const QVector<double> isoValues;

	//! Fill the combo boxes (also after a change of language)
	void fillComboBoxes();
	//! The f-numbers the lens allows (none wider than its widest aperture)
	void fillApertureList();
	//! Select the entry closest to value (on a logarithmic scale) without emitting signals
	static void selectNearest(QComboBox* comboBox, const QVector<double>& values, double value);
	//! Human-readable exposure time, e.g. "1/250 s", "0.5 s", "30 s", "2 min"
	static QString exposureTimeText(double seconds);
};

#endif // PHOTOGRAPHICMODEDIALOG_HPP
