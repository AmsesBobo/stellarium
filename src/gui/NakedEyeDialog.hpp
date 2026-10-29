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

#ifndef NAKEDEYEDIALOG_HPP
#define NAKEDEYEDIALOG_HPP

#include "StelDialog.hpp"

class Ui_NakedEyeDialogForm;
class QTimer;

//! Settings of the naked eye rendering mode: the eye of the observer
class NakedEyeDialog : public StelDialog
{
	Q_OBJECT

public:
	NakedEyeDialog();
	~NakedEyeDialog() override;

public slots:
	void retranslate() override;

protected:
	void createDialogContent() override;

private slots:
	//! Show the state of the adaptation of the eye
	void updateAdaptationInfo();

private:
	Ui_NakedEyeDialogForm *ui;
	QTimer* infoTimer = nullptr;
};

#endif // NAKEDEYEDIALOG_HPP
