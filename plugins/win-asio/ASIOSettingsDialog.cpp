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

#include "ASIOSettingsDialog.hpp"

#include <util/util.hpp>

OBSData load_settings()
{
	BPtr<char> path = obs_module_get_config_path(obs_current_module(), "asioOutputProps.json");
	BPtr<char> jsonData = os_quick_read_utf8_file(path);
	if (!!jsonData) {
		obs_data_t *data = obs_data_create_from_json(jsonData);
		OBSData dataRet(data);
		obs_data_release(data);

		return dataRet;
	}

	return nullptr;
}

ASIOSettingsDialog::ASIOSettingsDialog(QWidget *parent)
	: QDialog(parent),
	  ui(new Ui::Output)
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

	OBSData data = load_settings();
	if (data)
		obs_data_apply(settings, data);

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

	obs_data_t *settings = propertiesView->GetSettings();
	if (settings)
		obs_data_save_json_safe(settings, path, "tmp", "bak");
}

void ASIOSettingsDialog::on_outputButton_clicked()
{
	SaveSettings();
}

void ASIOSettingsDialog::PropertiesChanged()
{
	SaveSettings();
}

void ASIOSettingsDialog::OutputStateChanged(bool active)
{
	QString text;
	if (active) {
		text = QString(obs_module_text("Monitoring.OFF"));
	} else {
		text = QString(obs_module_text("Ponitoring.ON"));
	}

	ui->outputButton->setChecked(active);
	ui->outputButton->setText(text);
}


