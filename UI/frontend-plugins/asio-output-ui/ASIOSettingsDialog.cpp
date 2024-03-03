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

#include "ASIOSettingsDialog.h"
#include <util/util.hpp>

ASIOSettingsDialog::ASIOSettingsDialog(QWidget *parent, obs_output_t *output, OBSData settings)
	: QDialog(parent),
	  ui(new Ui::Output),
	  _output(output),
	  _settings(settings)
{
	ui->setupUi(this);
	setSizeGripEnabled(true);
	setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
	propertiesView = nullptr;
}

void ASIOSettingsDialog::ShowHideDialog()
{
	SetupPropertiesView();
	setVisible(!isVisible());
}

void ASIOSettingsDialog::SetupPropertiesView()
{
	if (propertiesView)
		delete propertiesView;

	obs_data_t *settings = obs_data_create();
	obs_data_apply(settings, _settings);

	propertiesView = new OBSPropertiesView(settings, "asio_output",
					       (PropertiesReloadCallback)obs_get_output_properties, 170);

	ui->propertiesLayout->addWidget(propertiesView);
	obs_data_release(settings);

	connect(propertiesView, &OBSPropertiesView::Changed, this, &ASIOSettingsDialog::PropertiesChanged);
}

void ASIOSettingsDialog::SaveSettings()
{
	BPtr<char> modulePath = obs_module_get_config_path(obs_current_module(), "");

	os_mkdirs(modulePath);

	BPtr<char> path = obs_module_get_config_path(obs_current_module(), "asioOutputProps.json");

	_settings = propertiesView->GetSettings();
	if (_settings)
		obs_data_save_json_safe(_settings, path, "tmp", "bak");
}

void ASIOSettingsDialog::PropertiesChanged()
{
	SaveSettings();
	obs_output_update(_output, _settings);
}

