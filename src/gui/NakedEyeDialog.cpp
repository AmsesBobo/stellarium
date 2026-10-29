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

#include "NakedEyeDialog.hpp"
#include "ui_nakedEyeDialog.h"

#include "StelApp.hpp"
#include "StelCore.hpp"
#include "StelSkyDrawer.hpp"
#include "StelTranslator.hpp"
#include "StelEyeAdaptation.hpp"

#include <QTimer>
#include <QLocale>
#include <cmath>

NakedEyeDialog::NakedEyeDialog() : StelDialog("NakedEye")
{
	ui = new Ui_NakedEyeDialogForm;
}

NakedEyeDialog::~NakedEyeDialog()
{
	delete ui;
	ui = nullptr;
}

void NakedEyeDialog::retranslate()
{
	if (dialog)
	{
		ui->retranslateUi(dialog);
		updateAdaptationInfo();
	}
}

void NakedEyeDialog::createDialogContent()
{
	ui->setupUi(dialog);

	connect(&StelApp::getInstance(), &StelApp::languageChanged, this, &NakedEyeDialog::retranslate);
	connect(ui->titleBar, &TitleBar::closeClicked, this, &StelDialog::close);
	connect(ui->titleBar, &TitleBar::movedTo, this, &NakedEyeDialog::handleMovedTo);

	connectBoolProperty(ui->nakedEyeModeCheckBox, "StelSkyDrawer.flagNakedEyeMode");
	connectBoolProperty(ui->instantAdaptationCheckBox, "StelSkyDrawer.flagEyeInstantAdaptation");
	connectBoolProperty(ui->infoLineCheckBox, "StelSkyDrawer.flagPhotoInfoLine");
	ui->ageSpinBox->setRange(StelSkyDrawer::EYE_MIN_AGE, StelSkyDrawer::EYE_MAX_AGE);
	connectDoubleProperty(ui->ageSpinBox, "StelSkyDrawer.eyeAge");
	StelSkyDrawer* drawer = StelApp::getInstance().getCore()->getSkyDrawer();
	connect(ui->adaptNowPushButton, &QPushButton::clicked, drawer, &StelSkyDrawer::resetEyeAdaptation);

	// The adaptation changes by itself: show it while the dialog is open
	infoTimer = new QTimer(this);
	infoTimer->setInterval(500);
	connect(infoTimer, &QTimer::timeout, this, &NakedEyeDialog::updateAdaptationInfo);
	connect(dialog, &QObject::destroyed, infoTimer, &QTimer::stop);
	infoTimer->start();
	updateAdaptationInfo();
}

void NakedEyeDialog::updateAdaptationInfo()
{
	if (!dialog || !dialog->isVisible())
		return;
	const StelSkyDrawer* drawer = StelApp::getInstance().getCore()->getSkyDrawer();
	if (!drawer->getFlagNakedEyeModeActive())
	{
		ui->adaptationInfoLabel->setText(q_("The naked eye mode is off (it needs OpenGL 3 or later)."));
		ui->pupilInfoLabel->clear();
		return;
	}
	const StelEyeAdaptation& eye = drawer->getEyeAdaptation();
	QLocale locale;
	locale.setNumberOptions(QLocale::OmitGroupSeparator);
	const double L = drawer->getEyeAdaptationLuminance();
	const QString luminance = (L >= 1e-3) ? locale.toString(L, 'g', 3) : locale.toString(L, 'e', 1);
	QString text = q_("Adapted to %1 cd/m%2, seeing with the %3.")
	                   .arg(luminance, QChar(0x00B2), eye.getLimitingSystem() == StelEyeAdaptation::Rods ? q_("rods") : q_("cones"));
	const double elevation = 2.5*eye.getThresholdElevation();
	if (elevation >= 0.05)
		text += QStringLiteral(" ") + q_("Still adapting to the dark: the faintest visible light is %1 mag brighter than when adapted.")
		                                  .arg(locale.toString(elevation, 'f', 1));
	ui->adaptationInfoLabel->setText(text);
	ui->pupilInfoLabel->setText(q_("Pupil: %1 mm.").arg(locale.toString(drawer->getEyePupilDiameter(), 'f', 1)));
}
