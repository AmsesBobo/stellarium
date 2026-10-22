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

#include <QComboBox>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QSignalBlocker>
#include <QSlider>
#include <QPushButton>
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
	}
	{
		const QSignalBlocker blocker(ui->filmComboBox);
		ui->filmComboBox->clear();
		ui->filmComboBox->addItem(q_("None (digital sensor)"), StelSkyDrawer::PhotoFilmNone);
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
	// Airglow: percent in the dialog, factor in the property
	connect(ui->airglowStrengthSpinBox, QOverload<int>::of(&QSpinBox::valueChanged), this, [drawer](int percent)
	{
		drawer->setPhotoAirglowStrength(percent/100.);
	});
	connect(drawer, &StelSkyDrawer::photoAirglowStrengthChanged,      this, &PhotographicModeDialog::updateCameraWidgets);
	connect(drawer, &StelSkyDrawer::flagPhotoAirglowChanged,          this, &PhotographicModeDialog::updateCameraWidgets);
	connectBoolProperty(ui->vignettingCheckBox,       "StelSkyDrawer.flagPhotoVignetting");
	connectBoolProperty(ui->aberrationsCheckBox,      "StelSkyDrawer.flagPhotoAberrations");
	connectBoolProperty(ui->lensFlareCheckBox,        "StelSkyDrawer.flagPhotoLensFlare");
	connectBoolProperty(ui->zoomWholeMillimetersCheckBox, "StelSkyDrawer.flagPhotoZoomWholeMillimeters");
	connectBoolProperty(ui->frameMaskCheckBox,        "StelSkyDrawer.flagPhotoFrameMask");

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
	connect(ui->focalLengthSpinBox, QOverload<int>::of(&QSpinBox::valueChanged), this, [drawer](int mm)
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

	updateCameraWidgets();
	updateStarWidgets();
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
	selectNearest(ui->isoComboBox, isoValues, iso);
	ui->exposureTimeComboBox->setEnabled(!autoExposure);
	// In the program mode the camera chooses the aperture and the ISO as well: the lists show them
	const bool program = autoExposure && drawer->getFlagPhotoProgramExposure();
	ui->apertureComboBox->setEnabled(!program);
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
		if (film)
		{
			ui->isoComboBox->setEnabled(false);
			ui->sensorComboBox->setEnabled(false);
			ui->noiseComboBox->setEnabled(false);
			ui->toneCurveComboBox->setEnabled(false);
			ui->whiteBalanceComboBox->setEnabled(false);
			ui->whiteBalanceKelvinSpinBox->setEnabled(false);
			ui->whiteBalanceKelvinLabel->setEnabled(false);
		}
		else
		{
			ui->sensorComboBox->setEnabled(true);
			ui->noiseComboBox->setEnabled(true);
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
	ui->focalLengthSpinBox->setValue(qRound(1000.*drawer->getPhotoFocalLengthForFov(core->getMovementMgr()->getAimFov())));
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
