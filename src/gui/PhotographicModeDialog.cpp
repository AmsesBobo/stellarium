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

#include "PhotographicModeDialog.hpp"
#include "ui_photographicModeDialog.h"

#include "StelApp.hpp"
#include "StelCore.hpp"
#include "StelSkyDrawer.hpp"
#include "StelTranslator.hpp"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QSignalBlocker>
#include <cmath>

// Standard values in 1/3-stop steps, as found on camera dials
const QVector<double> PhotographicModeDialog::exposureTimes = {
	1./8000, 1./6400, 1./5000, 1./4000, 1./3200, 1./2500, 1./2000, 1./1600, 1./1250, 1./1000,
	1./800, 1./640, 1./500, 1./400, 1./320, 1./250, 1./200, 1./160, 1./125, 1./100,
	1./80, 1./60, 1./50, 1./40, 1./30, 1./25, 1./20, 1./15, 1./13, 1./10,
	1./8, 1./6, 1./5, 1./4, 0.3, 0.4, 0.5, 0.6, 0.8, 1., 1.3, 1.6, 2., 2.5, 3.2, 4., 5., 6., 8., 10.,
	13., 15., 20., 25., 30., 45., 60., 90., 120., 180., 240., 300., 600., 900., 1200., 1800., 3600. };
const QVector<double> PhotographicModeDialog::apertures = {
	0.95, 1.0, 1.1, 1.2, 1.4, 1.6, 1.8, 2.0, 2.2, 2.5, 2.8, 3.2, 3.5, 4.0, 4.5, 5.0, 5.6, 6.3, 7.1,
	8.0, 9.0, 10., 11., 13., 14., 16., 18., 20., 22., 32. };
const QVector<double> PhotographicModeDialog::isoValues = {
	50, 64, 80, 100, 125, 160, 200, 250, 320, 400, 500, 640, 800, 1000, 1250, 1600, 2000, 2500,
	3200, 4000, 5000, 6400, 8000, 10000, 12800, 16000, 20000, 25600, 51200, 102400, 204800 };

PhotographicModeDialog::PhotographicModeDialog() : StelDialog("PhotographicMode")
{
	ui = new Ui_PhotographicModeDialogForm;
}

PhotographicModeDialog::~PhotographicModeDialog()
{
	delete ui;
	ui = nullptr;
}

void PhotographicModeDialog::retranslate()
{
	if (dialog)
	{
		ui->retranslateUi(dialog);
		fillComboBoxes();
		updateCameraWidgets();
		updateStarWidgets();
	}
}

QString PhotographicModeDialog::exposureTimeText(double seconds)
{
	if (seconds < 0.25)
		return QString("1/%1 %2").arg(qRound(1./seconds)).arg(qc_("s", "unit of time: seconds"));
	if (seconds < 60.)
		return QString("%1 %2").arg(QLocale().toString(seconds, 'g', 3), qc_("s", "unit of time: seconds"));
	return QString("%1 %2").arg(QLocale().toString(seconds/60., 'g', 3), qc_("min", "unit of time: minutes"));
}

void PhotographicModeDialog::fillComboBoxes()
{
	{
		const QSignalBlocker blocker(ui->exposureTimeComboBox);
		ui->exposureTimeComboBox->clear();
		// All camera lists run from bright (top) to dark (bottom): scrolling the mouse wheel up,
		// or pressing the up arrow key, makes the picture brighter.
		for (auto it = exposureTimes.crbegin(); it != exposureTimes.crend(); ++it)
			ui->exposureTimeComboBox->addItem(exposureTimeText(*it), *it);
	}
	{
		const QSignalBlocker blocker(ui->apertureComboBox);
		ui->apertureComboBox->clear();
		for (const double n : apertures)
			ui->apertureComboBox->addItem(QString("f/%1").arg(QLocale().toString(n, 'g', 3)), n);
	}
	{
		const QSignalBlocker blocker(ui->isoComboBox);
		ui->isoComboBox->clear();
		for (auto it = isoValues.crbegin(); it != isoValues.crend(); ++it)
			ui->isoComboBox->addItem(QString::number(qRound(*it)), *it);
	}
	{
		const QSignalBlocker blocker(ui->spikesComboBox);
		ui->spikesComboBox->clear();
		ui->spikesComboBox->addItem(qc_("None", "diffraction spikes"), 0);
		ui->spikesComboBox->addItem(QString::number(4), 4);
		ui->spikesComboBox->addItem(QString::number(6), 6);
		ui->spikesComboBox->addItem(QString::number(8), 8);
	}
}

void PhotographicModeDialog::selectNearest(QComboBox* comboBox, const QVector<double>& values, double value)
{
	// The order of the items in the list may differ from the order of the values
	Q_UNUSED(values)
	int best = 0;
	double bestDistance = 1e300;
	for (int i=0; i<comboBox->count(); ++i)
	{
		const double item = comboBox->itemData(i).toDouble();
		if (!(item > 0.) || !(value > 0.))
			continue;
		const double distance = std::fabs(std::log(item) - std::log(value));
		if (distance < bestDistance)
		{
			bestDistance = distance;
			best = i;
		}
	}
	const QSignalBlocker blocker(comboBox);
	comboBox->setCurrentIndex(best);
}

void PhotographicModeDialog::createDialogContent()
{
	ui->setupUi(dialog);

	connect(&StelApp::getInstance(), &StelApp::languageChanged, this, &PhotographicModeDialog::retranslate);
	connect(ui->titleBar, &TitleBar::closeClicked, this, &StelDialog::close);
	connect(ui->titleBar, &TitleBar::movedTo,      this, &PhotographicModeDialog::handleMovedTo);

	StelSkyDrawer* drawer = StelApp::getInstance().getCore()->getSkyDrawer();

	// Properties with a direct widget equivalent
	connectBoolProperty(ui->photographicModeCheckBox, "StelSkyDrawer.flagPhotographicMode");
	connectDoubleProperty(ui->evDoubleSpinBox,        "StelSkyDrawer.photoExposureEV");
	connectDoubleProperty(ui->fwhmDoubleSpinBox,      "StelSkyDrawer.photoPsfMinFwhm");
	connectDoubleProperty(ui->seeingDoubleSpinBox,    "StelSkyDrawer.photoSeeing");
	connectBoolProperty(ui->bloomCheckBox,            "StelSkyDrawer.flagPhotoBloom");

	fillComboBoxes();

	// Camera settings: combo boxes with standard values
	connect(ui->exposureTimeComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, drawer](int index)
	{
		if (index >= 0)
			drawer->setPhotoExposureTime(ui->exposureTimeComboBox->itemData(index).toDouble());
	});
	connect(ui->apertureComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, drawer](int index)
	{
		if (index >= 0)
			drawer->setPhotoAperture(ui->apertureComboBox->itemData(index).toDouble());
	});
	connect(ui->isoComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, drawer](int index)
	{
		if (index >= 0)
			drawer->setPhotoISO(ui->isoComboBox->itemData(index).toDouble());
	});
	connect(drawer, &StelSkyDrawer::photoExposureTimeChanged, this, &PhotographicModeDialog::updateCameraWidgets);
	connect(drawer, &StelSkyDrawer::photoApertureChanged,     this, &PhotographicModeDialog::updateCameraWidgets);
	connect(drawer, &StelSkyDrawer::photoISOChanged,          this, &PhotographicModeDialog::updateCameraWidgets);
	connect(drawer, &StelSkyDrawer::photoExposureEVChanged,   this, &PhotographicModeDialog::updateCameraWidgets);

	// Star images: percentages in the dialog, fractions in the properties
	connect(ui->haloDoubleSpinBox, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [drawer](double percent)
	{
		drawer->setPhotoHaloStrength(percent/100.);
	});
	connect(ui->spikeStrengthDoubleSpinBox, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [drawer](double percent)
	{
		drawer->setPhotoSpikeStrength(percent/100.);
	});
	connect(ui->spikesComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, drawer](int index)
	{
		if (index >= 0)
			drawer->setPhotoSpikeCount(ui->spikesComboBox->itemData(index).toInt());
	});
	connect(drawer, &StelSkyDrawer::photoHaloStrengthChanged,  this, &PhotographicModeDialog::updateStarWidgets);
	connect(drawer, &StelSkyDrawer::photoSpikeStrengthChanged, this, &PhotographicModeDialog::updateStarWidgets);
	connect(drawer, &StelSkyDrawer::photoSpikeCountChanged,    this, &PhotographicModeDialog::updateStarWidgets);

	connect(ui->resetPushButton, &QPushButton::clicked, drawer, &StelSkyDrawer::resetPhotographicSettings);

	updateCameraWidgets();
	updateStarWidgets();
}

void PhotographicModeDialog::updateCameraWidgets()
{
	const StelSkyDrawer* drawer = StelApp::getInstance().getCore()->getSkyDrawer();
	const double t = drawer->getPhotoExposureTime();
	const double n = drawer->getPhotoAperture();
	const double iso = drawer->getPhotoISO();
	selectNearest(ui->exposureTimeComboBox, exposureTimes, t);
	selectNearest(ui->apertureComboBox, apertures, n);
	selectNearest(ui->isoComboBox, isoValues, iso);

	// Exposure value at ISO 100, the usual way photographers compare exposures.
	// The compensation brightens the picture, i.e. acts like a lower EV.
	const double ev100 = std::log2(n*n/t) - std::log2(iso/100.) - drawer->getPhotoExposureEV();
	ui->exposureValueLabel->setText(QString("%1: EV<sub>100</sub> = %2")
	                                .arg(q_("Exposure value"), QLocale().toString(ev100, 'f', 1)));
}

void PhotographicModeDialog::updateStarWidgets()
{
	const StelSkyDrawer* drawer = StelApp::getInstance().getCore()->getSkyDrawer();
	{
		const QSignalBlocker blocker(ui->haloDoubleSpinBox);
		ui->haloDoubleSpinBox->setValue(100.*drawer->getPhotoHaloStrength());
	}
	{
		const QSignalBlocker blocker(ui->spikeStrengthDoubleSpinBox);
		ui->spikeStrengthDoubleSpinBox->setValue(100.*drawer->getPhotoSpikeStrength());
	}
	{
		const QSignalBlocker blocker(ui->spikesComboBox);
		const int index = ui->spikesComboBox->findData(drawer->getPhotoSpikeCount());
		ui->spikesComboBox->setCurrentIndex(qMax(0, index));
	}
	ui->spikeStrengthDoubleSpinBox->setEnabled(drawer->getPhotoSpikeCount() > 0);
}
