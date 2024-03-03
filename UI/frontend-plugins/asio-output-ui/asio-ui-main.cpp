#include <obs-module.h>
#include <obs-frontend-api.h>
#include <QMainWindow>
#include <QAction>
#include <util/util.hpp>
#include <util/platform.h>
#include "ASIOSettingsDialog.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("asio-output-ui", "en-US")

struct asio_ui_output {
	bool enabled;
	obs_output_t *output;
	OBSData settings;
};

// We use a global context for asio output.
struct asio_ui_output context = {0};
bool output_running = false;
ASIOSettingsDialog *_settingsDialog = nullptr;

OBSData load_settings()
{
	BPtr<char> path = obs_module_get_config_path(obs_current_module(),
						     "asioOutputProps.json");
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
	if (context.output) {
		obs_output_stop(context.output);
	}
	output_running = false;
}

void output_start()
{
	if (context.output != nullptr) {

		output_running = obs_output_start(context.output);
		if (!output_running)
			output_stop();
	}
}

void addOutputUI(void)
{
	QAction *action = (QAction *)obs_frontend_add_tools_menu_qaction(
		obs_module_text("AsioOutput.Menu"));

	QMainWindow *mainWindow = (QMainWindow *)obs_frontend_get_main_window();

	obs_frontend_push_ui_translation(obs_module_get_string);
	_settingsDialog = new ASIOSettingsDialog(mainWindow, context.output,
						 context.settings);
	obs_frontend_pop_ui_translation();

	auto cb = []() {
		_settingsDialog->ShowHideDialog();
	};

	action->connect(action, &QAction::triggered, cb);
}

static void OBSEvent(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
		OBSData settings = load_settings();
		// auto-start the ASIO audio output
		if (settings)
			output_start();
	} else if (event == OBS_FRONTEND_EVENT_EXIT) {
		if (output_running)
			output_stop();
	}
}

bool obs_module_load(void)
{
	return true;
}

void obs_module_unload(void)
{
	if (output_running)
		output_stop();
	obs_output_release(context.output);
	context.output = nullptr;
	obs_data_release(context.settings);
	context.settings = nullptr;
	obs_frontend_remove_event_callback(OBSEvent, nullptr);
}

void obs_module_post_load(void)
{
	if (!obs_get_module("win-asio"))
		return;
	/* create the output */
	context.settings = load_settings();
	obs_output_t *const output = obs_output_create(
		"asio_output", "asio_output", context.settings, NULL);
	if (output != nullptr) {
		context.output = output;
		addOutputUI();
		obs_frontend_add_event_callback(OBSEvent, nullptr);
	} else {
		blog(LOG_INFO, "Failed to create ASIO output");
	}
}
