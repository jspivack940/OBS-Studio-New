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

#include <obs-module.h>
#include <util/threading.h>
#include <obs-frontend-api.h>

const char *PLUGIN_VERSION = "4.0.0";
OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("win-asio", "en-US")
MODULE_EXPORT const char *obs_module_description(void)
{
	return "ASIO audio plugin";
}


void register_asio_source();
void register_asio_output();
void retrieve_device_list();
void free_device_list();
void OBSEvent(enum obs_frontend_event event, void *);


extern os_event_t *shutting_down;
void load_asio_output_settings();

#include "ASIOSettingsDialog.hpp"

struct asio_ui_output {
	bool enabled;
	obs_output_t *output;
};
static struct asio_ui_output context = {0};
bool ui_shutting_down = false;
bool main_output_running = false;
ASIOSettingsDialog *_settingsDialog = nullptr;


#include <util/util.hpp>

static OBSData load_settings_initial()
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
void output_stop()
{
	obs_output_stop(context.output);
	obs_output_release(context.output);

	main_output_running = false;

	if (!ui_shutting_down)
		_settingsDialog->OutputStateChanged(false);
}

void output_start()
{
	OBSData settings = load_settings_initial();
//	if (settings != nullptr) {
		obs_output_t *const output = obs_output_create("asio_output", "asio_output", settings, NULL);
		if (output != nullptr) {
			context.output = output;
		}
		bool started = obs_output_start(context.output);
		main_output_running = started;
		if (!ui_shutting_down)
			_settingsDialog->OutputStateChanged(started);
		if (!started)
			output_stop();
//	}
}

void output_toggle()
{
	if (main_output_running)
		output_stop();
	else
		output_start();
}

void OBSEventUI(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
		OBSData settings = load_settings_initial();

//		if (settings)
			output_start();

	} else if (event == OBS_FRONTEND_EVENT_EXIT) {
		ui_shutting_down = true;

		if (main_output_running)
			output_stop();
	}
}

void load_asio_output_settings()
{
	/* Add output settings in Tools mrnu*/
	// Initialize the settings dialog
	obs_frontend_push_ui_translation(obs_module_get_string);
	QMainWindow *mainWindow = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	_settingsDialog = new ASIOSettingsDialog(mainWindow);
	obs_frontend_pop_ui_translation();

	// Add the settings dialog to the tools menu
	const char *menuActionText = obs_module_text("Win-asio.Settings.DialogTitle");
	QAction *menuAction = (QAction *)obs_frontend_add_tools_menu_qaction(menuActionText);
	QObject::connect(menuAction, &QAction::triggered, [] { _settingsDialog->ShowHideDialog(); });
}


bool obs_module_load(void)
{
	retrieve_device_list();
	register_asio_source();
	blog(LOG_INFO, "ASIO plugin loaded successfully (version %s)", PLUGIN_VERSION);
	if (os_event_init(&shutting_down, OS_EVENT_TYPE_AUTO))
		return false;
	/* Add ASIO Outout Settings to Tools menu */
	register_asio_output();
	load_asio_output_settings();

	return true;
}

void obs_module_unload()
{
	free_device_list();
	os_event_destroy(shutting_down);
}

void obs_module_post_load(void)
{
	if (!obs_get_module("win-asio"))
		return;

	obs_frontend_add_event_callback(OBSEvent, nullptr);
	obs_frontend_add_event_callback(OBSEventUI, nullptr);
}
