/*  Copyright (c) 2022 pkv <pkv@obsproject.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program (see COPYING); if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301 USA.
 */
#pragma once
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <obs.hpp>
#include "../../UI/properties-view.hpp"

#include <QDialog>
#include <QAction>
#include <QMainWindow>

#include "./forms/ui_output.h"

#include <util/platform.h>

class ASIOSettingsDialog : public QDialog {
	Q_OBJECT

public:
	explicit ASIOSettingsDialog(QWidget *parent = 0);
	std::unique_ptr<Ui_Output> ui;
	void ShowHideDialog();
	void SetupPropertiesView();
	void SaveSettings();

public slots:
	void on_outputButton_clicked();
	void PropertiesChanged();
	void OutputStateChanged(bool);

private:
	OBSPropertiesView *propertiesView;
};


