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
#include "StelMovementMgr.hpp"
#include "StelTranslator.hpp"
#include "StelModuleMgr.hpp"
#include "LightDomes.hpp"
#include "AuroraMgr.hpp"
#include "StelLocation.hpp"

#include <QComboBox>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QSignalBlocker>
#include <QSlider>
#include <QPushButton>
#include <QTableWidget>
#include <QHeaderView>
#include <QLocale>
#include <cmath>

namespace
{
// The focus ring: the slider position p (-100 ... 1000) gives the focus in dioptres, 1/distance.
// Quadratic, so that the ring is fine near infinity (p = 0), where the stars are nearly sharp:
// p = 1000 is the closest focus (0.3 m), p = -100 the end stop beyond infinity.
constexpr double FOCUS_SLIDER_MAX = 1000.;
double focusDistanceFromSlider(int p)
{
	if (p == 0)
		return 0.;
	const double x = std::fabs(p)/FOCUS_SLIDER_MAX;
	const double dioptres = x*x/StelSkyDrawer::PHOTO_FOCUS_MIN_DISTANCE;
	return (p > 0 ? 1. : -1.)/dioptres;
}
int focusSliderFromDistance(double meters)
{
	if (meters == 0.)
		return 0;
	const double x = std::sqrt(StelSkyDrawer::PHOTO_FOCUS_MIN_DISTANCE/std::fabs(meters));
	return (meters > 0. ? 1 : -1) * qRound(FOCUS_SLIDER_MAX*x);
}
}

// Standard values in 1/3-stop steps, as found on camera dials
const QVector<double> PhotographicModeDialog::exposureTimes = {
	1./8000, 1./6400, 1./5000, 1./4000, 1./3200, 1./2500, 1./2000, 1./1600, 1./1250, 1./1000,
	1./800, 1./640, 1./500, 1./400, 1./320, 1./250, 1./200, 1./160, 1./125, 1./100,
	1./80, 1./60, 1./50, 1./40, 1./30, 1./25, 1./20, 1./15, 1./13, 1./10,
	1./8, 1./6, 1./5, 1./4, 0.3, 0.4, 0.5, 0.6, 0.8, 1., 1.3, 1.6, 2., 2.5, 3.2, 4., 5., 6., 8., 10.,
	13., 15., 20., 25., 30., 40., 50., 60., 90., 120., 180., 240., 300., 600., 900., 1200., 1800., 3600. };
const QVector<double> PhotographicModeDialog::apertures = {
	0.95, 1.0, 1.1, 1.2, 1.4, 1.6, 1.8, 2.0, 2.2, 2.5, 2.8, 3.2, 3.5, 4.0, 4.5, 5.0, 5.6, 6.3, 7.1,
	8.0, 9.0, 10., 11., 13., 14., 16., 18., 20., 22., 32. };
const QVector<double> PhotographicModeDialog::isoValues = {
	50, 64, 80, 100, 125, 160, 200, 250, 320, 400, 500, 640, 800, 1000, 1250, 1600, 2000, 2500,
	3200, 4000, 5000, 6400, 8000, 10000, 12800, 16000, 20000, 25600, 51200, 102400, 204800 };

PhotographicModeDialog::PhotographicModeDialog(QObject* parent) : StelDialog("PhotographicMode", parent)
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
		sensorNoiseToolTip = ui->noiseComboBox->toolTip();
		fillComboBoxes();
		updateCameraWidgets();
		updateStarWidgets();
		// the lamp lists are made again in the new language
		ui->lightDomesTableWidget->setRowCount(0);
		updateLightDomeWidgets();
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
	fillApertureList();
	{
		const QSignalBlocker blocker(ui->lensMaxApertureComboBox);
		ui->lensMaxApertureComboBox->clear();
		for (const double n : apertures)
			if (n >= StelSkyDrawer::PHOTO_LENS_MAX_APERTURE_MIN - 1e-6 && n <= StelSkyDrawer::PHOTO_LENS_MAX_APERTURE_MAX + 1e-6)
				ui->lensMaxApertureComboBox->addItem(QString("f/%1").arg(QLocale().toString(n, 'g', 3)), n);
	}
	{
		const QSignalBlocker blocker(ui->isoComboBox);
		ui->isoComboBox->clear();
		for (auto it = isoValues.crbegin(); it != isoValues.crend(); ++it)
			ui->isoComboBox->addItem(QString::number(qRound(*it)), *it);
	}
	{
		// Program mode limits: the highest ISO and the longest time, from bright (top) to dark like the other lists
		const QSignalBlocker blocker(ui->programMaxIsoComboBox);
		ui->programMaxIsoComboBox->clear();
		for (auto it = isoValues.crbegin(); it != isoValues.crend(); ++it)
			if (*it >= StelSkyDrawer::PHOTO_PROGRAM_BASE_ISO && *it <= StelSkyDrawer::PHOTO_PROGRAM_ISO_CEILING)
				ui->programMaxIsoComboBox->addItem(QString("ISO %1").arg(qRound(*it)), *it);
		const QSignalBlocker blocker2(ui->programMaxTimeComboBox);
		ui->programMaxTimeComboBox->clear();
		for (auto it = exposureTimes.crbegin(); it != exposureTimes.crend(); ++it)
			if (*it >= StelSkyDrawer::PHOTO_PROGRAM_MIN_MAX_TIME*0.999 && *it <= StelSkyDrawer::PHOTO_AUTO_MAX_TIME*1.001)
				ui->programMaxTimeComboBox->addItem(exposureTimeText(*it), *it);
	}
	{
		const QSignalBlocker blocker(ui->spikesComboBox);
		ui->spikesComboBox->clear();
		// Diaphragm blades, with the number of spikes they make (twice the blades for an odd number)
		ui->spikesComboBox->addItem(q_("Round (no spikes)"), 0);
		for (int blades = 4; blades <= 9; ++blades)
		{
			const int spikes = (blades % 2) ? 2*blades : blades;
			ui->spikesComboBox->addItem(QString("%1 (%2)").arg(blades).arg(q_("%1 spikes").arg(spikes)), blades);
		}
	}
	{
		const QSignalBlocker blocker(ui->meteringComboBox);
		ui->meteringComboBox->clear();
		ui->meteringComboBox->addItem(q_("Center-weighted"), StelSkyDrawer::PhotoMeteringCenterWeighted);
		ui->meteringComboBox->addItem(q_("Spot"), StelSkyDrawer::PhotoMeteringSpot);
		ui->meteringComboBox->addItem(q_("Highlight-weighted"), StelSkyDrawer::PhotoMeteringHighlight);
	}
	{
		const QSignalBlocker blocker(ui->zoomStepsComboBox);
		ui->zoomStepsComboBox->clear();
		ui->zoomStepsComboBox->addItem(q_("Free"), StelSkyDrawer::PhotoZoomFree);
		ui->zoomStepsComboBox->addItem(q_("Whole millimeters"), StelSkyDrawer::PhotoZoomWholeMillimeters);
		ui->zoomStepsComboBox->addItem(q_("Common focal lengths"), StelSkyDrawer::PhotoZoomCommonFocalLengths);
	}
	{
		const QSignalBlocker blocker(ui->toneCurveComboBox);
		ui->toneCurveComboBox->clear();
		ui->toneCurveComboBox->addItem(q_("Linear"), StelSkyDrawer::PhotoToneLinear);
		ui->toneCurveComboBox->addItem(q_("Standard (camera)"), StelSkyDrawer::PhotoToneStandard);
		ui->toneCurveComboBox->addItem(q_("Film"), StelSkyDrawer::PhotoToneFilm);
	}
	{
		const QSignalBlocker blocker(ui->sensorComboBox);
		ui->sensorComboBox->clear();
		ui->sensorComboBox->addItem(q_("Medium format (44 x 33 mm)"), StelSkyDrawer::PhotoSensorMediumFormat);
		ui->sensorComboBox->addItem(q_("Full frame (36 x 24 mm)"), StelSkyDrawer::PhotoSensorFullFrame);
		ui->sensorComboBox->addItem(q_("APS-C (23.5 x 15.6 mm)"), StelSkyDrawer::PhotoSensorAPSC);
		ui->sensorComboBox->addItem(q_("APS-C Canon (22.3 x 14.9 mm)"), StelSkyDrawer::PhotoSensorAPSCCanon);
		ui->sensorComboBox->addItem(q_("Micro Four Thirds (17.3 x 13 mm)"), StelSkyDrawer::PhotoSensorMicroFourThirds);
		ui->sensorComboBox->addItem(q_("1-inch (13.2 x 8.8 mm)"), StelSkyDrawer::PhotoSensorOneInch);
		ui->sensorComboBox->addItem(q_("2/3-inch (8.8 x 6.6 mm)"), StelSkyDrawer::PhotoSensorTwoThirds);
		ui->sensorComboBox->addItem(q_("1/1.7-inch (7.6 x 5.7 mm)"), StelSkyDrawer::PhotoSensor1_1_7);
		ui->sensorComboBox->addItem(q_("1/2.3-inch (6.2 x 4.6 mm)"), StelSkyDrawer::PhotoSensor1_2_3);
	}
	{
		const QSignalBlocker blocker(ui->filmComboBox);
		ui->filmComboBox->clear();
		ui->filmComboBox->addItem(q_("None (digital sensor)"), StelSkyDrawer::PhotoFilmNone);
		const QSignalBlocker pushBlocker(ui->filmPushComboBox);
		ui->filmPushComboBox->clear();
		ui->filmPushComboBox->addItem(q_("Normal"), 0);
		for (int p = 1; p <= StelSkyDrawer::PHOTO_FILM_MAX_PUSH; ++p)
			ui->filmPushComboBox->addItem(q_("Push +%1").arg(p), p);
		for (int film = 1; film < StelSkyDrawer::PHOTO_FILM_COUNT; ++film)
		{
			const StelSkyDrawer::PhotoFilmSpec& f = StelSkyDrawer::getPhotoFilmSpec(film);
			const QString type = f.type == StelSkyDrawer::PhotoFilmSlide ? q_("slide")
			                   : (f.type == StelSkyDrawer::PhotoFilmNegative ? q_("color negative") : q_("black and white"));
			ui->filmComboBox->addItem(QString("%1 (ISO %2, %3)").arg(f.name).arg(qRound(f.iso)).arg(type), film);
		}
	}
	{
		const QSignalBlocker blocker(ui->eclipseAtmosphereComboBox);
		ui->eclipseAtmosphereComboBox->clear();
		ui->eclipseAtmosphereComboBox->addItem(q_("Clean air (bright)"), 0);
		ui->eclipseAtmosphereComboBox->addItem(q_("Typical"), 1);
		ui->eclipseAtmosphereComboBox->addItem(q_("Volcanic aerosols (dark)"), 2);
		ui->eclipseAtmosphereComboBox->addItem(q_("Major eruption (very dark)"), 3);
	}
	{
		const QSignalBlocker blocker(ui->exposureModeComboBox);
		ui->exposureModeComboBox->clear();
		ui->exposureModeComboBox->addItem(q_("Manual (M)"), 0);
		ui->exposureModeComboBox->addItem(q_("Aperture priority (Av)"), 1);
		ui->exposureModeComboBox->addItem(q_("Program, auto ISO (P)"), 2);
	}
	{
		const QSignalBlocker blocker(ui->frameOrientationComboBox);
		ui->frameOrientationComboBox->clear();
		ui->frameOrientationComboBox->addItem(q_("Landscape"), false);
		ui->frameOrientationComboBox->addItem(q_("Portrait"), true);
	}
	{
		// The motion during the exposure: off (flagPhotoStarTrails false), or the mount
		const QSignalBlocker blocker(ui->mountComboBox);
		ui->mountComboBox->clear();
		ui->mountComboBox->addItem(q_("No motion"), -1);
		ui->mountComboBox->addItem(q_("Fixed tripod (star trails)"), StelSkyDrawer::PhotoMountTripod);
		ui->mountComboBox->addItem(q_("Star tracker (landscape blurs)"), StelSkyDrawer::PhotoMountTracker);
		ui->mountComboBox->addItem(q_("As the view's mount (alt-az: tripod, equatorial: tracker)"), StelSkyDrawer::PhotoMountView);
	}
	{
		const QSignalBlocker blocker(ui->noiseComboBox);
		ui->noiseComboBox->clear();
		ui->noiseComboBox->addItem(q_("Off"), StelSkyDrawer::PhotoNoiseOff);
		ui->noiseComboBox->addItem(q_("Whole picture"), StelSkyDrawer::PhotoNoiseImage);
		ui->noiseComboBox->addItem(q_("100% view"), StelSkyDrawer::PhotoNoisePixel);
	}
	{
		const QSignalBlocker blocker(ui->whiteBalanceComboBox);
		ui->whiteBalanceComboBox->clear();
		const auto preset = [this](const QString& name, int wb) {
			ui->whiteBalanceComboBox->addItem(QString("%1 (%2 K)").arg(name).arg(qRound(StelSkyDrawer::photoWhiteBalancePresetKelvin(wb))), wb);
		};
		ui->whiteBalanceComboBox->addItem(q_("Automatic"), StelSkyDrawer::PhotoWBAuto);
		preset(q_("Daylight"), StelSkyDrawer::PhotoWBDaylight);
		preset(q_("Cloudy"), StelSkyDrawer::PhotoWBCloudy);
		preset(q_("Shade"), StelSkyDrawer::PhotoWBShade);
		preset(q_("Tungsten"), StelSkyDrawer::PhotoWBTungsten);
		preset(q_("Fluorescent"), StelSkyDrawer::PhotoWBFluorescent);
		preset(q_("Flash"), StelSkyDrawer::PhotoWBFlash);
		ui->whiteBalanceComboBox->addItem(q_("Custom"), StelSkyDrawer::PhotoWBCustom);
		ui->whiteBalanceComboBox->addItem(q_("Neutral (as rendered)"), StelSkyDrawer::PhotoWBNeutral);
	}
}

void PhotographicModeDialog::fillApertureList()
{
	const double widest = StelApp::getInstance().getCore()->getSkyDrawer()->getPhotoLensMaxAperture();
	const QSignalBlocker blocker(ui->apertureComboBox);
	ui->apertureComboBox->clear();
	for (const double n : apertures)
		if (n >= widest - 1e-6)
			ui->apertureComboBox->addItem(QString("f/%1").arg(QLocale().toString(n, 'g', 3)), n);
	// a widest aperture between the steps of the list (from the configuration file) heads it
	if (ui->apertureComboBox->count() == 0 || ui->apertureComboBox->itemData(0).toDouble() > widest + 1e-6)
		ui->apertureComboBox->insertItem(0, QString("f/%1").arg(QLocale().toString(widest, 'g', 3)), widest);
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
	sensorNoiseToolTip = ui->noiseComboBox->toolTip();

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
	connect(ui->exposureModeComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, drawer](int index)
	{
		if (index < 0)
			return;
		const int mode = ui->exposureModeComboBox->itemData(index).toInt();
		drawer->setFlagPhotoProgramExposure(mode == 2);
		drawer->setFlagPhotoAutoExposure(mode != 0);
	});
	connect(drawer, &StelSkyDrawer::flagPhotoProgramExposureChanged,   this, &PhotographicModeDialog::updateCameraWidgets);
	connectBoolProperty(ui->infoLineCheckBox,         "StelSkyDrawer.flagPhotoInfoLine");
	connectBoolProperty(ui->extinctionColorCheckBox,  "StelSkyDrawer.flagPhotoExtinctionColor");
	connectBoolProperty(ui->airglowCheckBox,          "StelSkyDrawer.flagPhotoAirglow");
	connectBoolProperty(ui->auroraCheckBox,           "AuroraMgr.flagShow");
	connectBoolProperty(ui->auroraOvalCheckBox,       "AuroraMgr.flagShowOval");
	connectBoolProperty(ui->auroraHalfResolutionCheckBox, "AuroraMgr.flagHalfResolution");
	// The aurorae: the activity is a preset or a custom Kp; the substorm cycle shows where it stands
	connectIntProperty(ui->auroraActivityComboBox,   "AuroraMgr.kpPreset");
	connectDoubleProperty(ui->auroraKpDoubleSpinBox,  "AuroraMgr.kpCustom");
	connectDoubleProperty(ui->auroraDstDoubleSpinBox, "AuroraMgr.dstCustom");
	{
		auto updateActivity = [this](){
			const bool custom = ui->auroraActivityComboBox->currentIndex() == AuroraMgr::KpPresetCustom;
			ui->auroraKpDoubleSpinBox->setEnabled(custom);
			ui->auroraKpLabel->setEnabled(custom);
			ui->auroraDstDoubleSpinBox->setEnabled(custom);
			ui->auroraDstLabel->setEnabled(custom);
		};
		connect(ui->auroraActivityComboBox, qOverload<int>(&QComboBox::currentIndexChanged), this, updateActivity);
		updateActivity();
		AuroraMgr* aurora = GETSTELMODULE(AuroraMgr);
		ui->auroraSubstormLabel->setText(aurora->getSubstormInfo());
		connect(aurora, &AuroraMgr::substormInfoChanged, ui->auroraSubstormLabel, &QLabel::setText);
	}
	connectBoolProperty(ui->filmSpectralSkyCheckBox,  "StelSkyDrawer.flagPhotoFilmSpectralSky");
	connectBoolProperty(ui->programApertureLockCheckBox, "StelSkyDrawer.flagPhotoProgramApertureLock");
	connect(drawer, &StelSkyDrawer::flagPhotoProgramApertureLockChanged, this, &PhotographicModeDialog::updateCameraWidgets);
	// Airglow: percent in the dialog, factor in the property
	connect(ui->airglowStrengthSpinBox, QOverload<int>::of(&QSpinBox::valueChanged), this, [drawer](int percent)
	{
		drawer->setPhotoAirglowStrength(percent/100.);
	});
	connect(drawer, &StelSkyDrawer::photoAirglowStrengthChanged,      this, &PhotographicModeDialog::updateCameraWidgets);
	connect(drawer, &StelSkyDrawer::flagPhotoAirglowChanged,          this, &PhotographicModeDialog::updateCameraWidgets);
	// Humidity (percent)
	connect(ui->humiditySpinBox, QOverload<int>::of(&QSpinBox::valueChanged), this, [drawer](int percent)
	{
		drawer->setPhotoHumidity(percent);
	});
	connect(drawer, &StelSkyDrawer::photoHumidityChanged,             this, &PhotographicModeDialog::updateCameraWidgets);
	connectBoolProperty(ui->aureoleCheckBox,          "StelSkyDrawer.flagPhotoAureole");
	connectBoolProperty(ui->humidityCheckBox,         "StelSkyDrawer.flagPhotoHumidity");
	connect(drawer, &StelSkyDrawer::flagPhotoHumidityChanged,         this, &PhotographicModeDialog::updateCameraWidgets);
	connect(drawer, &StelSkyDrawer::extinctionCoefficientChanged,     this, &PhotographicModeDialog::updateCameraWidgets);
	connectBoolProperty(ui->vignettingCheckBox,       "StelSkyDrawer.flagPhotoVignetting");
	connectBoolProperty(ui->halphaModifiedCheckBox,   "StelSkyDrawer.flagPhotoHalphaModified");
	connectBoolProperty(ui->aberrationsCheckBox,      "StelSkyDrawer.flagPhotoAberrations");
	connectBoolProperty(ui->lensFlareCheckBox,        "StelSkyDrawer.flagPhotoLensFlare");
	connectBoolProperty(ui->frameMaskCheckBox,        "StelSkyDrawer.flagPhotoFrameMask");

	fillComboBoxes();

	// Camera settings: combo boxes with standard values
	connect(ui->exposureTimeComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, drawer](int index)
	{
		if (index >= 0)
			drawer->setPhotoExposureTime(ui->exposureTimeComboBox->itemData(index).toDouble());
	});
	connect(ui->lensMaxApertureComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, drawer](int index)
	{
		if (index >= 0)
			drawer->setPhotoLensMaxAperture(ui->lensMaxApertureComboBox->itemData(index).toDouble());
	});
	connect(drawer, &StelSkyDrawer::photoLensMaxApertureChanged, this, [this]()
	{
		fillApertureList();
		updateCameraWidgets();
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
	connect(ui->programMaxIsoComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, drawer](int index)
	{
		if (index >= 0)
			drawer->setPhotoProgramMaxISO(ui->programMaxIsoComboBox->itemData(index).toDouble());
	});
	connect(ui->programMaxTimeComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, drawer](int index)
	{
		if (index >= 0)
			drawer->setPhotoProgramMaxTime(ui->programMaxTimeComboBox->itemData(index).toDouble());
	});
	connect(drawer, &StelSkyDrawer::photoProgramMaxISOChanged,  this, &PhotographicModeDialog::updateCameraWidgets);
	connect(drawer, &StelSkyDrawer::photoProgramMaxTimeChanged, this, &PhotographicModeDialog::updateCameraWidgets);
	connect(ui->meteringComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, drawer](int index)
	{
		if (index >= 0)
			drawer->setPhotoMeteringMode(ui->meteringComboBox->itemData(index).toInt());
	});
	connect(ui->toneCurveComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, drawer](int index)
	{
		if (index >= 0)
			drawer->setPhotoToneCurve(ui->toneCurveComboBox->itemData(index).toInt());
	});
	connect(drawer, &StelSkyDrawer::photoToneCurveChanged,             this, &PhotographicModeDialog::updateCameraWidgets);
	connect(ui->zoomStepsComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, drawer](int index)
	{
		if (index >= 0)
			drawer->setPhotoZoomSteps(ui->zoomStepsComboBox->itemData(index).toInt());
	});
	connect(drawer, &StelSkyDrawer::photoZoomStepsChanged,             this, &PhotographicModeDialog::updateCameraWidgets);
	connect(ui->whiteBalanceComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, drawer](int index)
	{
		if (index >= 0)
			drawer->setPhotoWhiteBalance(ui->whiteBalanceComboBox->itemData(index).toInt());
	});
	connect(ui->whiteBalanceKelvinSpinBox, QOverload<int>::of(&QSpinBox::valueChanged), this, [drawer](int kelvin)
	{
		drawer->setPhotoWhiteBalanceKelvin(kelvin);
	});
	connect(drawer, &StelSkyDrawer::photoWhiteBalanceChanged,          this, &PhotographicModeDialog::updateCameraWidgets);
	connect(ui->noiseComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, drawer](int index)
	{
		if (index >= 0)
			drawer->setPhotoNoise(ui->noiseComboBox->itemData(index).toInt());
	});
	connect(drawer, &StelSkyDrawer::photoNoiseChanged,                 this, &PhotographicModeDialog::updateCameraWidgets);
	connect(ui->mountComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, drawer](int index)
	{
		if (index < 0)
			return;
		const int mount = ui->mountComboBox->itemData(index).toInt();
		if (mount >= 0)
			drawer->setPhotoMount(mount);
		drawer->setFlagPhotoStarTrails(mount >= 0);
	});
	connect(drawer, &StelSkyDrawer::flagPhotoStarTrailsChanged,        this, &PhotographicModeDialog::updateCameraWidgets);
	connect(drawer, &StelSkyDrawer::photoMountChanged,                 this, &PhotographicModeDialog::updateCameraWidgets);
	connect(ui->frameOrientationComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, drawer](int index)
	{
		if (index >= 0)
			drawer->setFlagPhotoPortrait(ui->frameOrientationComboBox->itemData(index).toBool());
	});
	connect(drawer, &StelSkyDrawer::flagPhotoPortraitChanged,          this, &PhotographicModeDialog::updateCameraWidgets);
	connect(ui->eclipseAtmosphereComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, drawer](int index)
	{
		if (index >= 0)
			drawer->setPhotoEclipseAtmosphere(ui->eclipseAtmosphereComboBox->itemData(index).toInt());
	});
	connect(drawer, &StelSkyDrawer::photoEclipseAtmosphereChanged,     this, &PhotographicModeDialog::updateCameraWidgets);
	// Sensor and lens
	connect(ui->filmPushComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, drawer](int index)
	{
		if (index >= 0)
			drawer->setPhotoFilmPush(ui->filmPushComboBox->itemData(index).toInt());
	});
	connect(drawer, &StelSkyDrawer::photoFilmPushChanged, this, &PhotographicModeDialog::updateCameraWidgets);
	connect(ui->filmComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, drawer](int index)
	{
		if (index >= 0)
			drawer->setPhotoFilm(ui->filmComboBox->itemData(index).toInt());
	});
	connect(drawer, &StelSkyDrawer::photoFilmChanged,                 this, &PhotographicModeDialog::updateCameraWidgets);
	connect(ui->sensorComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, drawer](int index)
	{
		if (index >= 0)
			drawer->setPhotoSensorFormat(ui->sensorComboBox->itemData(index).toInt());
	});
	connect(drawer, &StelSkyDrawer::photoSensorFormatChanged,          this, &PhotographicModeDialog::updateCameraWidgets);
	ui->focalLengthSpinBox->setStepType(QAbstractSpinBox::AdaptiveDecimalStepType);
	connect(ui->focalLengthSpinBox, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [drawer](double mm)
	{
		drawer->setPhotoFocalLength(mm);
	});
	// The field of view changes while zooming: show the focal length of the field aimed at
	connect(StelApp::getInstance().getCore()->getMovementMgr(), &StelMovementMgr::currentFovChanged, this, &PhotographicModeDialog::updateFocalLength);
	// Lens: percent in the dialog, factor in the property
	connect(ui->aberrationStrengthSpinBox, QOverload<int>::of(&QSpinBox::valueChanged), this, [drawer](int percent)
	{
		drawer->setPhotoAberrationStrength(percent/100.);
	});
	connect(drawer, &StelSkyDrawer::photoAberrationStrengthChanged,    this, &PhotographicModeDialog::updateCameraWidgets);
	// Detection limit of the stars (signal to noise ratio)
	connect(ui->detectionSnrSpinBox, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [drawer](double snr)
	{
		drawer->setPhotoDetectionSnr(snr);
	});
	connect(drawer, &StelSkyDrawer::photoDetectionSnrChanged,         this, &PhotographicModeDialog::updateCameraWidgets);
	connect(drawer, &StelSkyDrawer::flagPhotoAberrationsChanged,       this, &PhotographicModeDialog::updateCameraWidgets);
	connect(ui->lensFlareStrengthSpinBox, QOverload<int>::of(&QSpinBox::valueChanged), this, [drawer](int percent)
	{
		drawer->setPhotoLensFlareStrength(percent/100.);
	});
	connect(drawer, &StelSkyDrawer::photoLensFlareStrengthChanged,     this, &PhotographicModeDialog::updateCameraWidgets);
	connect(drawer, &StelSkyDrawer::flagPhotoLensFlareChanged,         this, &PhotographicModeDialog::updateCameraWidgets);
	connect(drawer, &StelSkyDrawer::photoWhiteBalanceKelvinChanged,    this, &PhotographicModeDialog::updateCameraWidgets);
	// Focus ring
	connect(ui->focusSlider, &QSlider::valueChanged, this, [drawer](int p)
	{
		drawer->setPhotoFocusDistance(focusDistanceFromSlider(p));
	});
	connect(ui->focusInfinityPushButton, &QPushButton::clicked, this, [drawer]()
	{
		drawer->setPhotoFocusDistance(0.);
	});
	connect(drawer, &StelSkyDrawer::photoFocusDistanceChanged,         this, &PhotographicModeDialog::updateCameraWidgets);
	connect(drawer, &StelSkyDrawer::flagPhotoAutoExposureChanged,      this, &PhotographicModeDialog::updateCameraWidgets);
	connect(drawer, &StelSkyDrawer::photoMeteringModeChanged,          this, &PhotographicModeDialog::updateCameraWidgets);
	connect(drawer, &StelSkyDrawer::photoEffectiveExposureTimeChanged, this, &PhotographicModeDialog::updateCameraWidgets);

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
			drawer->setPhotoApertureBlades(ui->spikesComboBox->itemData(index).toInt());
	});
	connect(drawer, &StelSkyDrawer::photoHaloStrengthChanged,  this, &PhotographicModeDialog::updateStarWidgets);
	connect(drawer, &StelSkyDrawer::photoSpikeStrengthChanged, this, &PhotographicModeDialog::updateStarWidgets);
	connect(drawer, &StelSkyDrawer::photoApertureBladesChanged, this, &PhotographicModeDialog::updateStarWidgets);

	connect(ui->resetPushButton, &QPushButton::clicked, drawer, &StelSkyDrawer::resetPhotographicSettings);

	// Light domes of towns
	LightDomes* domes = GETSTELMODULE(LightDomes);
	if (domes)
	{
		connectBoolProperty(ui->lightDomesCheckBox, "LightDomes.flagEnabled");
		connectDoubleProperty(ui->lightDomesLumensDoubleSpinBox, "LightDomes.lumensPerInhabitant");
		ui->lightDomesTableWidget->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
		ui->lightDomesTableWidget->verticalHeader()->setVisible(false);
		connect(ui->lightDomesTableWidget, &QTableWidget::cellChanged, this, &PhotographicModeDialog::applyLightDomeTable);
		connect(ui->lightDomesAddPushButton, &QPushButton::clicked, this, [this, domes]()
		{
			QString s = domes->getSources();
			if (!s.isEmpty())
				s += ';';
			domes->setSources(s + "0,20,50000,0");
		});
		connect(ui->lightDomesRemovePushButton, &QPushButton::clicked, this, [this]()
		{
			QTableWidget* t = ui->lightDomesTableWidget;
			QList<int> rows;
			for (const auto* item : t->selectedItems())
				if (!rows.contains(item->row()))
					rows << item->row();
			if (rows.isEmpty() && t->currentRow() >= 0)
				rows << t->currentRow();
			std::sort(rows.begin(), rows.end(), std::greater<int>());
			{
				const QSignalBlocker blocker(t);
				for (const int r : rows)
					t->removeRow(r);
			}
			applyLightDomeTable();
		});
		connect(domes, &LightDomes::sourcesChanged, this, &PhotographicModeDialog::updateLightDomeWidgets);
		connect(domes, &LightDomes::mapChanged, this, &PhotographicModeDialog::updateLightDomeWidgets);
		connect(domes, &LightDomes::flagEnabledChanged, this, &PhotographicModeDialog::updateLightDomeWidgets);
		connect(drawer, &StelSkyDrawer::flagPhotographicModeChanged, this, &PhotographicModeDialog::updateLightDomeWidgets);

		// the towns of the location database
		connectBoolProperty(ui->lightDomesDatabaseCheckBox, "LightDomes.flagDatabaseTowns");
		connectBoolProperty(ui->lightDomesLabelsCheckBox, "LightDomes.flagTownLabels");
		connect(ui->lightDomesRecomputePushButton, &QPushButton::clicked, domes, &LightDomes::recompute);
		fillLampComboBox(ui->lightDomesDatabaseLampComboBox);
		ui->lightDomesDatabaseTableWidget->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
		ui->lightDomesDatabaseTableWidget->verticalHeader()->setVisible(false);
		connect(ui->lightDomesDatabaseLampComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, domes]()
		{
			domes->setDatabaseTownLamp(ui->lightDomesDatabaseLampComboBox->currentData().toInt());
		});
		connect(domes, &LightDomes::databaseTownLampChanged, this, &PhotographicModeDialog::updateDatabaseTownWidgets);
		connect(domes, &LightDomes::databaseTownsChanged, this, &PhotographicModeDialog::updateDatabaseTownWidgets);
		connect(domes, &LightDomes::flagDatabaseTownsChanged, this, &PhotographicModeDialog::updateDatabaseTownWidgets);
		connect(domes, &LightDomes::flagDatabaseTownsChanged, this, &PhotographicModeDialog::updateLightDomeWidgets);
		connect(domes, &LightDomes::flagEnabledChanged, this, &PhotographicModeDialog::updateDatabaseTownWidgets);
		// the light of the towns: population or satellite
		fillSourceComboBox();
		connect(ui->lightDomesSourceComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, domes]()
		{
			domes->setLightSource(ui->lightDomesSourceComboBox->currentData().toInt());
		});
		connect(ui->lightDomesDownloadPushButton, &QPushButton::clicked, domes, &LightDomes::downloadSatelliteData);
		connect(domes, &LightDomes::lightSourceChanged, this, &PhotographicModeDialog::updateDatabaseTownWidgets);
		connect(domes, &LightDomes::satelliteDataChanged, this, &PhotographicModeDialog::updateDatabaseTownWidgets);
		connect(domes, &LightDomes::mapChanged, this, &PhotographicModeDialog::updateDatabaseTownWidgets);
	}
	else
		ui->lightDomesGroupBox->hide();

	updateCameraWidgets();
	updateStarWidgets();
	updateLightDomeWidgets();
	updateDatabaseTownWidgets();
}

void PhotographicModeDialog::fillLampComboBox(QComboBox* box)
{
	const int current = box->count() > 0 ? box->currentData().toInt() : -1;
	box->clear();
	box->addItem(q_("Mixed (sodium and LED)"), LightDomes::LampMixed);
	box->addItem(q_("High pressure sodium"), LightDomes::LampHPS);
	box->addItem(q_("Low pressure sodium"), LightDomes::LampLPS);
	box->addItem(q_("LED 3000 K"), LightDomes::LampLED3000);
	box->addItem(q_("LED 4000 K"), LightDomes::LampLED4000);
	if (current >= 0)
		box->setCurrentIndex(qMax(0, box->findData(current)));
}

void PhotographicModeDialog::fillSourceComboBox()
{
	QComboBox* box = ui->lightDomesSourceComboBox;
	const int current = box->count() > 0 ? box->currentData().toInt() : -1;
	box->clear();
	box->addItem(q_("Population (location database)"), LightDomes::SourcePopulation);
	box->addItem(q_("Satellite measurements (VIIRS 2024)"), LightDomes::SourceSatellite);
	if (current >= 0)
		box->setCurrentIndex(qMax(0, box->findData(current)));
}

void PhotographicModeDialog::updateSatelliteWidgets()
{
	const LightDomes* domes = GETSTELMODULE(LightDomes);
	if (!dialog || !domes)
		return;
	{
		const QSignalBlocker blocker(ui->lightDomesSourceComboBox);
		ui->lightDomesSourceComboBox->setCurrentIndex(qMax(0, ui->lightDomesSourceComboBox->findData(domes->getLightSource())));
	}
	const bool on = domes->getFlagDatabaseTowns();
	const bool satellite = domes->getLightSource() == LightDomes::SourceSatellite;
	ui->lightDomesSourceComboBox->setEnabled(on);
	ui->lightDomesDownloadPushButton->setVisible(satellite);
	ui->lightDomesSatelliteLabel->setVisible(satellite);
	if (!satellite)
		return;
	const NightLightsMap* map = domes->getNightLights();
	const StelLocation& here = StelApp::getInstance().getCore()->getCurrentLocation();
	const bool onEarth = here.planetName == QStringLiteral("Earth");
	const bool downloading = map && map->isDownloading();
	const bool available = domes->hasSatelliteData();
	ui->lightDomesDownloadPushButton->setEnabled(on && onEarth && map && !downloading && !available);
	QString status;
	if (!map || !onEarth)
		status = q_("Satellite data only for places on the Earth.");
	else if (downloading)
		status = q_("Downloading the satellite data...");
	else if (available)
		status = map->hasFullFile() ? q_("Satellite data: the whole map is installed.")
		                            : q_("Satellite data for this place are downloaded.");
	else
	{
		const qint64 size = map->regionDownloadSize(static_cast<double>(here.getLatitude()), static_cast<double>(here.getLongitude()), LightDomes::maxDistance());
		const QString amount = size < 0 ? q_("below 1 MB") : QString("%1 kB").arg(qMax(1LL, (size + 512)/1024));
		status = q_("No satellite data for this place yet (%1 to download): the population is used meanwhile.").arg(amount);
		if (!domes->getSatelliteMessage().isEmpty())
			status += QStringLiteral(" ") + q_("The last download failed: %1").arg(domes->getSatelliteMessage());
	}
	ui->lightDomesSatelliteLabel->setText(QString("%1<br/><small>%2</small>").arg(status.toHtmlEscaped(), NightLightsMap::attribution().toHtmlEscaped()));
}

void PhotographicModeDialog::updateDatabaseTownWidgets()
{
	const LightDomes* domes = GETSTELMODULE(LightDomes);
	if (!dialog || !domes)
		return;
	updateSatelliteWidgets();
	{
		const QSignalBlocker blocker(ui->lightDomesDatabaseLampComboBox);
		ui->lightDomesDatabaseLampComboBox->setCurrentIndex(qMax(0, ui->lightDomesDatabaseLampComboBox->findData(domes->getDatabaseTownLamp())));
	}
	const bool on = domes->getFlagDatabaseTowns();
	ui->lightDomesDatabaseLampComboBox->setEnabled(on);
	ui->lightDomesDatabaseTableWidget->setEnabled(on);
	ui->lightDomesLabelsCheckBox->setEnabled(on);
	ui->lightDomesRecomputePushButton->setEnabled(on && domes->getFlagEnabled());
	const auto& towns = domes->getDatabaseTowns();
	QTableWidget* t = ui->lightDomesDatabaseTableWidget;
	t->setRowCount(static_cast<int>(towns.size()));
	for (int r=0; r<static_cast<int>(towns.size()); ++r)
	{
		const LightDomes::DatabaseTown& town = towns[static_cast<size_t>(r)];
		const QString texts[4] = {
			town.name,
			QLocale().toString(town.azimuth, 'f', 0),
			QLocale().toString(town.distance, 'f', 1),
			QLocale().toString(qRound64(town.population))
		};
		for (int c=0; c<4; ++c)
		{
			QTableWidgetItem* item = t->item(r, c);
			if (!item)
				t->setItem(r, c, item = new QTableWidgetItem());
			item->setText(texts[c]);
			item->setTextAlignment(c == 0 ? (Qt::AlignLeft | Qt::AlignVCenter) : (Qt::AlignRight | Qt::AlignVCenter));
			item->setToolTip(c == 0 && town.districts > 0
			                 ? q_("%1 smaller places of the database inside it are counted in its population").arg(town.districts)
			                 : QString());
		}
	}
	if (!on)
		ui->lightDomesDatabaseInfoLabel->setText(QStringLiteral("-"));
	else if (!domes->getFlagEnabled())
		ui->lightDomesDatabaseInfoLabel->setText(q_("Switch on the light domes to find the towns"));
	else
	{
		double people = 0.;
		for (const auto& town : towns)
			people += town.population;
		ui->lightDomesDatabaseInfoLabel->setText(q_("%1 towns within %2 km, %3 inhabitants (%4 districts counted in larger towns)")
		                                         .arg(towns.size()).arg(qRound(LightDomes::maxDistance()))
		                                         .arg(QLocale().toString(qRound64(people))).arg(domes->getDatabaseDistricts()));
	}
}

void PhotographicModeDialog::updateLightDomeWidgets()
{
	const LightDomes* domes = GETSTELMODULE(LightDomes);
	if (!dialog || !domes)
		return;
	QTableWidget* t = ui->lightDomesTableWidget;
	const QSignalBlocker blocker(t);
	const QStringList towns = domes->getSources().split(';', Qt::SkipEmptyParts);
	t->setRowCount(static_cast<int>(towns.size()));
	for (int r=0; r<static_cast<int>(towns.size()); ++r)
	{
		const QStringList f = towns[r].split(',');
		for (int c=0; c<3; ++c)
		{
			const double v = f.value(c).toDouble();
			const QString text = c == 2 ? QString::number(qRound64(v)) : QLocale().toString(v, 'f', c == 0 ? 0 : 1);
			QTableWidgetItem* item = t->item(r, c);
			if (!item)
				t->setItem(r, c, item = new QTableWidgetItem());
			item->setText(text);
			item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
		}
		QComboBox* lamps = qobject_cast<QComboBox*>(t->cellWidget(r, 3));
		if (!lamps)
		{
			lamps = new QComboBox();
			fillLampComboBox(lamps);
			connect(lamps, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &PhotographicModeDialog::applyLightDomeTable);
			t->setCellWidget(r, 3, lamps);
		}
		const QSignalBlocker lampBlocker(lamps);
		lamps->setCurrentIndex(qMax(0, lamps->findData(f.value(3).toInt())));
	}
	// The light at the zenith (artificial)
	const double L = domes->getZenithLuminance();
	if (!domes->getFlagEnabled() || (towns.isEmpty() && domes->getDatabaseTowns().empty()))
		ui->lightDomesZenithLabel->setText(QStringLiteral("-"));
	else if (!domes->isActive())
		ui->lightDomesZenithLabel->setText(q_("Only in the photographic mode, with the atmosphere shown"));
	else if (L > 0.)
		ui->lightDomesZenithLabel->setText(q_("At the zenith: %1 mag/arcsec² (%2 mcd/m², artificial)")
		                                   .arg(-2.5*std::log10(L/10.8e4), 0, 'f', 2).arg(QLocale().toString(L*1000., 'g', 3)));
	else
		ui->lightDomesZenithLabel->setText(QStringLiteral("-"));
}

void PhotographicModeDialog::applyLightDomeTable()
{
	LightDomes* domes = GETSTELMODULE(LightDomes);
	if (!domes)
		return;
	QTableWidget* t = ui->lightDomesTableWidget;
	QStringList towns;
	for (int r=0; r<t->rowCount(); ++r)
	{
		double v[3] = {0., 20., 50000.};
		for (int c=0; c<3; ++c)
		{
			const QTableWidgetItem* item = t->item(r, c);
			if (!item)
				continue;
			bool ok = false;
			const double x = QLocale().toDouble(item->text(), &ok);
			if (ok)
				v[c] = x;
			else
			{
				const double y = item->text().toDouble(&ok);  // with a decimal point
				if (ok)
					v[c] = y;
			}
		}
		const QComboBox* lamps = qobject_cast<QComboBox*>(t->cellWidget(r, 3));
		const int lamp = lamps ? lamps->currentData().toInt() : 0;
		towns << QString("%1,%2,%3,%4").arg(v[0], 0, 'f', 1).arg(v[1], 0, 'f', 2).arg(v[2], 0, 'f', 0).arg(lamp);
	}
	domes->setSources(towns.join(';'));
}

void PhotographicModeDialog::updateCameraWidgets()
{
	const StelSkyDrawer* drawer = StelApp::getInstance().getCore()->getSkyDrawer();
	// With the automatic exposure the camera chooses the time: the list shows it, but cannot be changed
	const bool autoExposure = drawer->getFlagPhotoAutoExposure();
	const double t = autoExposure ? drawer->getPhotoEffectiveExposureTime() : drawer->getPhotoExposureTime();
	const double n = drawer->getPhotoAperture();
	const double iso = drawer->getPhotoISO();
	selectNearest(ui->exposureTimeComboBox, exposureTimes, t);
	selectNearest(ui->apertureComboBox, apertures, n);
	{
		const QSignalBlocker blocker(ui->lensMaxApertureComboBox);
		selectNearest(ui->lensMaxApertureComboBox, apertures, drawer->getPhotoLensMaxAperture());
	}
	selectNearest(ui->isoComboBox, isoValues, iso);
	ui->exposureTimeComboBox->setEnabled(!autoExposure);
	// In the program mode the camera chooses the aperture and the ISO as well: the lists show them
	const bool program = autoExposure && drawer->getFlagPhotoProgramExposure();
	ui->apertureComboBox->setEnabled(!program || drawer->getFlagPhotoProgramApertureLock());
	ui->programApertureLockCheckBox->setEnabled(program);
	ui->isoComboBox->setEnabled(!program);
	{
		const QSignalBlocker blocker(ui->exposureModeComboBox);
		ui->exposureModeComboBox->setCurrentIndex(qMax(0, ui->exposureModeComboBox->findData(!autoExposure ? 0 : (program ? 2 : 1))));
	}
	ui->meteringComboBox->setEnabled(autoExposure);
	ui->meteringLabel->setEnabled(autoExposure);
	{
		// The limits of the program mode: the highest ISO (not with a film, its speed is fixed) and the longest time
		const bool film = drawer->getPhotoFilm() != StelSkyDrawer::PhotoFilmNone;
		selectNearest(ui->programMaxIsoComboBox, isoValues, drawer->getPhotoProgramMaxISO());
		selectNearest(ui->programMaxTimeComboBox, exposureTimes, drawer->getPhotoProgramMaxTime());
		ui->programLimitsLabel->setEnabled(program);
		ui->programMaxIsoComboBox->setEnabled(program && !film);
		ui->programMaxTimeComboBox->setEnabled(program);
	}
	{
		const QSignalBlocker blocker(ui->meteringComboBox);
		ui->meteringComboBox->setCurrentIndex(qMax(0, ui->meteringComboBox->findData(drawer->getPhotoMeteringMode())));
	}
	{
		const QSignalBlocker blocker(ui->toneCurveComboBox);
		ui->toneCurveComboBox->setCurrentIndex(qMax(0, ui->toneCurveComboBox->findData(drawer->getPhotoToneCurve())));
	}
	{
		const QSignalBlocker blocker(ui->zoomStepsComboBox);
		ui->zoomStepsComboBox->setCurrentIndex(qMax(0, ui->zoomStepsComboBox->findData(drawer->getPhotoZoomSteps())));
	}
	{
		const QSignalBlocker blocker(ui->sensorComboBox);
		ui->sensorComboBox->setCurrentIndex(qMax(0, ui->sensorComboBox->findData(drawer->getPhotoSensorFormat())));
	}
	updateFocalLength();
	{
		const QSignalBlocker blocker(ui->noiseComboBox);
		ui->noiseComboBox->setCurrentIndex(qMax(0, ui->noiseComboBox->findData(drawer->getPhotoNoise())));
	}
	{
		const QSignalBlocker blocker(ui->frameOrientationComboBox);
		ui->frameOrientationComboBox->setCurrentIndex(drawer->getFlagPhotoPortrait() ? 1 : 0);
	}
	{
		const QSignalBlocker blocker(ui->eclipseAtmosphereComboBox);
		ui->eclipseAtmosphereComboBox->setCurrentIndex(qMax(0, ui->eclipseAtmosphereComboBox->findData(drawer->getPhotoEclipseAtmosphere())));
	}
	{
		const QSignalBlocker blocker(ui->mountComboBox);
		const int mount = drawer->getFlagPhotoStarTrails() ? drawer->getPhotoMount() : -1;
		ui->mountComboBox->setCurrentIndex(qMax(0, ui->mountComboBox->findData(mount)));
	}
	{
		const QSignalBlocker blocker(ui->whiteBalanceComboBox);
		ui->whiteBalanceComboBox->setCurrentIndex(qMax(0, ui->whiteBalanceComboBox->findData(drawer->getPhotoWhiteBalance())));
		const QSignalBlocker blocker2(ui->whiteBalanceKelvinSpinBox);
		ui->whiteBalanceKelvinSpinBox->setValue(qRound(drawer->getPhotoWhiteBalanceKelvin()));
		const bool custom = drawer->getPhotoWhiteBalance() == StelSkyDrawer::PhotoWBCustom;
		ui->whiteBalanceKelvinSpinBox->setEnabled(custom);
		ui->whiteBalanceKelvinLabel->setEnabled(custom);
	}
	{
		const QSignalBlocker blocker(ui->aberrationStrengthSpinBox);
		ui->aberrationStrengthSpinBox->setValue(qRound(100.*drawer->getPhotoAberrationStrength()));
		ui->aberrationStrengthSpinBox->setEnabled(drawer->getFlagPhotoAberrations());
		ui->aberrationStrengthLabel->setEnabled(drawer->getFlagPhotoAberrations());
	}
	{
		const QSignalBlocker blocker(ui->airglowStrengthSpinBox);
		ui->airglowStrengthSpinBox->setValue(qRound(100.*drawer->getPhotoAirglowStrength()));
		ui->airglowStrengthSpinBox->setEnabled(drawer->getFlagPhotoAirglow());
	}
	{
		const QSignalBlocker blocker(ui->humiditySpinBox);
		ui->humiditySpinBox->setValue(qRound(drawer->getPhotoHumidity()));
		// the extinction coefficient it gives (the one set is for 60%)
		ui->humiditySpinBox->setSuffix(QString(" %   (k = %1)").arg(QLocale().toString(static_cast<double>(drawer->getExtinction().getZenithExtinction()), 'f', 2)));
		ui->humiditySpinBox->setEnabled(drawer->getFlagPhotoHumidity());
		ui->aureoleCheckBox->setEnabled(drawer->getFlagPhotoHumidity());
	}
	{
		const QSignalBlocker blocker(ui->detectionSnrSpinBox);
		ui->detectionSnrSpinBox->setValue(drawer->getPhotoDetectionSnr());
	}
	{
		const QSignalBlocker blocker(ui->lensFlareStrengthSpinBox);
		ui->lensFlareStrengthSpinBox->setValue(qRound(100.*drawer->getPhotoLensFlareStrength()));
		ui->lensFlareStrengthSpinBox->setEnabled(drawer->getFlagPhotoLensFlare());
		ui->lensFlareStrengthLabel->setEnabled(drawer->getFlagPhotoLensFlare());
	}
	{
		const QSignalBlocker blocker(ui->focusSlider);
		const double s = drawer->getPhotoFocusDistance();
		// keep the slider where the user put it if it gives the same distance (rounding)
		if (focusDistanceFromSlider(ui->focusSlider->value()) != s)
			ui->focusSlider->setValue(focusSliderFromDistance(s));
		ui->focusValueLabel->setText(StelSkyDrawer::photoFocusDistanceText(s));
	}

	{
		// A film sets the speed, the color balance, the curve and the grain, and it is 35 mm
		const bool film = drawer->getPhotoFilm() != StelSkyDrawer::PhotoFilmNone;
		const QSignalBlocker blocker(ui->filmComboBox);
		ui->filmComboBox->setCurrentIndex(qMax(0, ui->filmComboBox->findData(drawer->getPhotoFilm())));
		const QSignalBlocker pushBlocker(ui->filmPushComboBox);
		ui->filmPushComboBox->setCurrentIndex(qMax(0, ui->filmPushComboBox->findData(drawer->getPhotoFilmPush())));
		ui->filmPushComboBox->setEnabled(film);
		ui->filmSpectralSkyCheckBox->setEnabled(film && StelSkyDrawer::getPhotoFilmSpectral(StelSkyDrawer::getPhotoFilmSpec(drawer->getPhotoFilm())));
		// The same list switches the sensor noise or the grain of the film
		ui->noiseLabel->setText(film ? q_("Film grain:") : q_("Sensor noise:"));
		ui->noiseComboBox->setToolTip(film ? q_("The grain of the film: the silver grains or dye clouds lie at random, more of them where the film is denser (the RMS granularity of the data sheet, growing as the square root of the density). A slide is densest in the dark parts, so the grain shows most in a dark night sky; a faster film has coarser grain. \"Whole picture\": as the whole frame looks on the screen. \"100% view\": a scan at 4000 dpi.")
		                                   : sensorNoiseToolTip);
		if (film)
		{
			ui->isoComboBox->setEnabled(false);
			ui->sensorComboBox->setEnabled(false);
			ui->halphaModifiedCheckBox->setEnabled(false);
			ui->toneCurveComboBox->setEnabled(false);
			ui->whiteBalanceComboBox->setEnabled(false);
			ui->whiteBalanceKelvinSpinBox->setEnabled(false);
			ui->whiteBalanceKelvinLabel->setEnabled(false);
		}
		else
		{
			ui->sensorComboBox->setEnabled(true);
			ui->halphaModifiedCheckBox->setEnabled(true);
			ui->toneCurveComboBox->setEnabled(true);
			ui->whiteBalanceComboBox->setEnabled(true);
		}
	}

	// Exposure value at ISO 100, the usual way photographers compare exposures.
	// In manual mode the compensation brightens the picture, i.e. acts like a lower EV.
	// With the automatic exposure it is part of the exposure time already.
	const double ev100 = std::log2(n*n/t) - std::log2(iso/100.) - (autoExposure ? 0. : drawer->getPhotoExposureEV());
	ui->exposureValueLabel->setText(QString("%1: EV<sub>100</sub> = %2")
	                                .arg(q_("Exposure value"), QLocale().toString(ev100, 'f', 1)));
}

void PhotographicModeDialog::updateFocalLength()
{
	// (not while the user is typing a value)
	if (!dialog || ui->focalLengthSpinBox->hasFocus())
		return;
	const StelCore* core = StelApp::getInstance().getCore();
	const StelSkyDrawer* drawer = core->getSkyDrawer();
	const QSignalBlocker blocker(ui->focalLengthSpinBox);
	// As the lens is marked: below 10 mm with a tenth (small sensors), whole millimeters above
	const double mm = StelSkyDrawer::photoNominalFocalLength(1000.*drawer->getPhotoFocalLengthForFov(core->getMovementMgr()->getAimFov()));
	ui->focalLengthSpinBox->setDecimals(mm < 10. ? 1 : 0);
	ui->focalLengthSpinBox->setValue(mm);
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
		const int index = ui->spikesComboBox->findData(drawer->getPhotoApertureBlades());
		ui->spikesComboBox->setCurrentIndex(qMax(0, index));
	}
	ui->spikeStrengthDoubleSpinBox->setEnabled(drawer->getPhotoApertureBlades() > 0);
}
