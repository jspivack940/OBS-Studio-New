/*
Copyright (C) 2018 by pkv <pkv.stream@gmail.com>, andersama <anderson.john.alexander@gmail.com>

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

/* For full GPL v2 compatibility it is required to build portaudio libs with
 * our open source sdk instead of steinberg sdk , see our fork:
 * https://github.com/pkviet/portaudio , branch : openasio
 * If you build Portaudio with original asio sdk, you are free to do so to the
 * extent that you do not distribute your binaries.
 */

#include <util/bmem.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <vector>
#include <stdio.h>
#include <windows.h>
#include "asioselector.h"
#include "circle-buffer.h"
#include "JuceLibraryCode/JuceHeader.h"

#include <QWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMainWindow>
#include <QWindow>
#include <QAction>
#include <QMessageBox>
#include <QString>
#include <QLabel>
#include <QHBoxLayout>
#include <QVBoxLayout>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("win-asio", "en-US")

#define blog(level, msg, ...) blog(level, "asio-input: " msg, ##__VA_ARGS__)

std::vector<asio_listener *> listener_list;

static juce::AudioDeviceManager manager;

/* ========================================================================== */
/*          conversions between portaudio and obs and utility functions       */
/* ========================================================================== */

enum audio_format string_to_obs_audio_format(std::string format)
{
	if (format == "32 Bit Int") {
		return AUDIO_FORMAT_32BIT;
	} else if (format == "32 Bit Float") {
		return AUDIO_FORMAT_FLOAT;
	} else if (format == "16 Bit Int") {
		return AUDIO_FORMAT_16BIT;
	}

	return AUDIO_FORMAT_UNKNOWN;
}

// returns corresponding planar format on entering some interleaved one
enum audio_format get_planar_format(audio_format format)
{
	if (is_audio_planar(format))
		return format;

	switch (format) {
	case AUDIO_FORMAT_U8BIT:
		return AUDIO_FORMAT_U8BIT_PLANAR;
	case AUDIO_FORMAT_16BIT:
		return AUDIO_FORMAT_16BIT_PLANAR;
	case AUDIO_FORMAT_32BIT:
		return AUDIO_FORMAT_32BIT_PLANAR;
	case AUDIO_FORMAT_FLOAT:
		return AUDIO_FORMAT_FLOAT_PLANAR;
		// should NEVER get here
	default:
		return AUDIO_FORMAT_UNKNOWN;
	}
}

// returns the size in bytes of a sample from an obs audio_format
int bytedepth_format(audio_format format)
{
	return (int)get_audio_bytes_per_channel(format);
}

// get number of output channels (this is set in obs general audio settings
int get_obs_output_channels()
{
	// get channel number from output speaker layout set by obs
	struct obs_audio_info aoi;
	obs_get_audio_info(&aoi);
	return (int)get_audio_channels(aoi.speakers);
}

// main callback when a device is switched; takes care of the logic for updating the clients (listeners)
static bool asio_device_changed(obs_properties_t *props, obs_property_t *list, obs_data_t *settings)
{
	size_t          i;
	bool            reset       = false;
	const char *    curDeviceId = obs_data_get_string(settings, "device_id");
	obs_property_t *sample_rate = obs_properties_get(props, "sample rate");
	obs_property_t *bit_depth   = obs_properties_get(props, "bit depth");
	obs_property_t *buffer_size = obs_properties_get(props, "buffer");
	obs_property_t *route[MAX_AUDIO_CHANNELS];

	long         cur_rate, cur_buffer;
	audio_format cur_format;

	long minBuf, maxBuf, prefBuf, gran;

	// get channel number from output speaker layout set by obs
	DWORD recorded_channels = get_obs_output_channels();
	// be sure to set device as current one

	size_t itemCount = obs_property_list_item_count(list);
	bool   itemFound = false;

	for (i = 0; i < itemCount; i++) {
		const char *DeviceId = obs_property_list_item_string(list, i);
		if (strcmp(DeviceId, curDeviceId) == 0) {
			itemFound = true;
			break;
		}
	}

	if (!itemFound) {
		obs_property_list_insert_string(list, 0, " ", curDeviceId);
		obs_property_list_item_disable(list, 0, true);
	} else {
		//DWORD device_index = get_device_index(curDeviceId);
		for (i = 0; i < recorded_channels; i++) {
			std::string name = "route " + std::to_string(i);
			route[i]         = obs_properties_get(props, name.c_str());
			obs_property_list_clear(route[i]);
			//obs_property_set_modified_callback(route[i], fill_out_channels_modified);
		}
	}

	return true;
}

/* callback when sample rate, buffer, sample bitdepth are changed. All the
listeners are updated and the stream is restarted. */
static bool asio_settings_changed(obs_properties_t *props, obs_property_t *list, obs_data_t *settings)
{
	size_t       i;
	bool         reset       = false;
	const char * curDeviceId = obs_data_get_string(settings, "device_id");
	long         cur_rate    = obs_data_get_int(settings, "sample rate");
	long         cur_buffer  = obs_data_get_int(settings, "buffer");
	audio_format cur_format  = (audio_format)obs_data_get_int(settings, "bit depth");

	return true;
}

class AudioCB : public juce::AudioIODeviceCallback {
private:
	device_buffer *      buffer;
	juce::AudioIODevice *device;

public:
	~AudioCB()
	{
	}

	void audioDeviceIOCallback(const float **inputChannelData, int numInputChannels, float **outputChannelData,
			int numOutputChannels, int numSamples)
	{
		uint64_t     ts       = os_gettime_ns();
		audio_format format   = buffer->get_format();
		uint32_t     channels = buffer->get_input_channels();
		size_t buf_size = buffer->get_input_channels() * numSamples * bytedepth_format(buffer->get_format());
		// device->write_buffer_planar(inputBuffer, buf_size, ts);
	}

	void audioDeviceAboutToStart(juce::AudioIODevice *device)
	{
	}

	void audioDeviceStopped()
	{
	}

	void audioDeviceErrror(const juce::String &errorMessage)
	{
		std::string error = errorMessage.toStdString();
	}
};

/* ========================================================================== */
/*                           main module methods                              */
/*                                                                            */
/* ========================================================================== */

static const char *asio_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("asioInput");
}

static void asio_get_defaults(obs_data_t *settings)
{
	// For the second and later clients, use the first listener settings as defaults.
	int recorded_channels = get_obs_output_channels();
	for (unsigned int i = 0; i < recorded_channels; i++) {
		std::string name = "route " + std::to_string(i);
		obs_data_set_default_int(settings, name.c_str(), -1); // default is muted channels
	}
}
/*
static bool device_menu(obs_properties_t *props, obs_property_t *property, void *vptr)
{
	asio_listener *listener = (asio_listener *)vptr;
	if (device_selector) {
		device_selector->setSelectedDevice(listener->device_index);
		device_selector->show();
		device_selector->activateWindow();
		device_selector->raise();
	}
	return false;
};
*/
obs_properties_t *asio_get_properties(void *vptr)
{
	obs_properties_t *props;
	obs_property_t *  devices;
	obs_property_t *  rate;
	obs_property_t *  bit_depth;
	obs_property_t *  buffer_size;
	obs_property_t *  console;
	obs_property_t *  route[MAX_AUDIO_CHANNELS];

	props = obs_properties_create();
	obs_properties_set_flags(props, OBS_PROPERTIES_DEFER_UPDATE);
	devices = obs_properties_add_list(
			props, "device_id", obs_module_text("Device"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_set_modified_callback(devices, asio_device_changed);
	//fill_out_devices(devices);
	obs_property_set_long_description(devices, obs_module_text("ASIO Devices"));

	unsigned int recorded_channels = get_obs_output_channels();

	for (size_t i = 0; i < recorded_channels; i++) {
		route[i] = obs_properties_add_list(props, ("route " + std::to_string(i)).c_str(),
				obs_module_text(("Route." + std::to_string(i)).c_str()), OBS_COMBO_TYPE_LIST,
				OBS_COMBO_FORMAT_INT);
		obs_property_set_long_description(
				route[i], obs_module_text(("Route.Desc." + std::to_string(i)).c_str()));
	}
	/*
	obs_properties_add_button2(
			props, "device_settings", obs_module_text("ASIO Device Settings"), device_menu, vptr);
	console = obs_properties_add_button(
			props, "console", obs_module_text("ASIO Device Control Panel"), DeviceControlPanel);
	*/
	obs_property_set_long_description(console, obs_module_text("Console.Desc"));

	return props;
}

static QActionGroup *device_switch_actions;

char *os_replace_slash(const char *dir)
{
	dstr dir_str;
	int  ret;

	dstr_init_copy(&dir_str, dir);
	dstr_replace(&dir_str, "\\", "/");
	return dir_str.array;
}

class ASIOPlugin {
private:
public:
	ASIOPlugin::ASIOPlugin(obs_data_t *settings, obs_source_t *source)
	{

	}

	static void *Create(obs_data_t *settings, obs_source_t *source)
	{
		return new ASIOPlugin(settings, source);
	}

	static void Destroy(void *vptr)
	{
		ASIOPlugin *plugin = static_cast<ASIOPlugin*>(vptr);
		delete plugin;
		plugin = nullptr;
	}

	static obs_properties_t *Properties(void *vptr)
	{
		ASIOPlugin *plugin = static_cast<ASIOPlugin *>(vptr);
		obs_properties_t *props  = obs_properties_create();
		return props;
	}

	static void Update(void *vptr, obs_data_t *settings)
	{
		ASIOPlugin *plugin = static_cast<ASIOPlugin *>(vptr);
	}

	static void Defaults(obs_data_t *settings)
	{

	}

	static const char *Name(void *unused)
	{
		UNUSED_PARAMETER(unused);
		return obs_module_text("ASIO");
	}
};

static void audio_device_selector()
{

}

bool obs_module_load(void)
{
	obs_audio_info aoi;
	obs_get_audio_info(&aoi);
	manager.initialise(256, 256, nullptr, true);
	AudioDeviceManager::AudioDeviceSetup setup =
			manager.getAudioDeviceSetup();
	
	blog(LOG_INFO, "INFO");
	blog(LOG_INFO, "BUF[%d]", setup.bufferSize);
	blog(LOG_INFO, "IN  '%s'", setup.inputDeviceName.toStdString().c_str());
	blog(LOG_INFO, "OUT '%s'", setup.outputDeviceName.toStdString().c_str());
	blog(LOG_INFO, "ICH[%d]", setup.inputChannels.toInteger());
	blog(LOG_INFO, "OCH[%d]", setup.outputChannels.toInteger());

	OwnedArray<AudioIODeviceType> types;
	manager.createAudioDeviceTypes(types);

	for (int i = 0; i < types.size(); i++) {
		blog(LOG_INFO, "TYPE: '%s'", types[i]->getTypeName().toStdString().c_str());
		types[i]->scanForDevices();
		StringArray deviceNames(types[i]->getDeviceNames());
		for (int j = 0; j < deviceNames.size(); j++) {
			AudioIODevice *device = types[i]->createDevice(deviceNames[j], deviceNames[j]);
			blog(LOG_INFO, "NAME: '%s'", device->getName());
		}
	}

	AudioCB *cb = new AudioCB();
	manager.addAudioCallback(cb);

	struct obs_source_info asio_input_capture = {};
	asio_input_capture.id                     = "asio_input_capture";
	asio_input_capture.type                   = OBS_SOURCE_TYPE_INPUT;
	asio_input_capture.output_flags           = OBS_SOURCE_AUDIO;
	asio_input_capture.create                 = ASIOPlugin::Create;
	asio_input_capture.destroy                = ASIOPlugin::Destroy;
	asio_input_capture.update                 = ASIOPlugin::Update;
	asio_input_capture.get_defaults           = ASIOPlugin::Defaults;
	asio_input_capture.get_name               = ASIOPlugin::Name;
	asio_input_capture.get_properties         = ASIOPlugin::Properties;

	obs_register_source(&asio_input_capture);
	return true;
}

void obs_module_post_load(void)
{
}

void obs_module_unload(void)
{
	
}
