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

/* For full GPL v2 compatibility it is required to build libs with
 * our open source sdk instead of steinberg sdk , see our fork:
 * https://github.com/pkviet/portaudio , branch : openasio
 * If you build with original asio sdk, you are free to do so to the
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
#include "circle-buffer.h"
#include <JuceHeader.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("win-asio", "en-US")

#define blog(level, msg, ...) blog(level, "asio-input: " msg, ##__VA_ARGS__)

static void                         fill_out_devices(obs_property_t *prop);
static std::vector<asio_listener *> listener_list;

static juce::AudioIODeviceType *deviceTypeAsio = AudioIODeviceType::createAudioIODeviceType_ASIO();

class ASIOPlugin;
class AudioCB;

static bool asio_device_changed(obs_properties_t *props, obs_property_t *list, obs_data_t *settings);
static bool asio_layout_changed(obs_properties_t *props, obs_property_t *list, obs_data_t *settings);
static bool fill_out_channels_modified(obs_properties_t *props, obs_property_t *list, obs_data_t *settings);

static std::vector<AudioCB *>       callbacks;
static std::vector<device_buffer *> buffers;

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

static std::vector<speaker_layout> known_layouts = {
		SPEAKERS_MONO,    /**< Channels: MONO */
		SPEAKERS_STEREO,  /**< Channels: FL, FR */
		SPEAKERS_2POINT1, /**< Channels: FL, FR, LFE */
		SPEAKERS_4POINT0, /**< Channels: FL, FR, FC, RC */
		SPEAKERS_4POINT1, /**< Channels: FL, FR, FC, LFE, RC */
		SPEAKERS_5POINT1, /**< Channels: FL, FR, FC, LFE, RL, RR */
		SPEAKERS_7POINT1, /**< Channels: FL, FR, FC, LFE, RL, RR, SL, SR */
};

static std::vector<std::string> known_layouts_str = {"Mono", "Stereo", "2.1", "4.0", "4.1", "5.1", "7.1"};

class AudioCB : public juce::AudioIODeviceCallback {
private:
	device_buffer *_buffer = nullptr;
	AudioIODevice *_device = nullptr;
	char *         _name   = nullptr;

public:
	AudioIODevice *getDevice()
	{
		return _device;
	}

	const char *getName()
	{
		return _name;
	}

	device_buffer *getBuffer()
	{
		return _buffer;
	}

	void setDevice(AudioIODevice *device, const char *name)
	{
		_device = device;
		if (_name)
			bfree(_name);
		_name = bstrdup(name);
	}

	AudioCB(device_buffer *buffer, AudioIODevice *device, const char *name)
	{
		_buffer = buffer;
		_device = device;
		_name   = bstrdup(name);
	}

	~AudioCB()
	{
		bfree(_name);
	}

	void audioDeviceIOCallback(const float **inputChannelData, int numInputChannels, float **outputChannelData,
			int numOutputChannels, int numSamples)
	{
		uint64_t ts = os_gettime_ns();
		_buffer->write_buffer_planar(inputChannelData, numInputChannels, numSamples, ts);
		UNUSED_PARAMETER(numOutputChannels);
		UNUSED_PARAMETER(outputChannelData);
	}

	void audioDeviceAboutToStart(juce::AudioIODevice *device)
	{
		blog(LOG_INFO, "Starting (%s)", device->getName().toStdString().c_str());
		device_buffer_options opts;
		opts.buffer_size   = device->getCurrentBufferSizeSamples();
		opts.channel_count = (uint32_t)device->getActiveInputChannels().countNumberOfSetBits();
		opts.name = device->getName().toStdString().c_str();
		_buffer->set_audio_format(AUDIO_FORMAT_FLOAT_PLANAR);
		_buffer->update_sample_rate((uint32_t)device->getCurrentSampleRate());
		_buffer->prep_circle_buffer(opts.buffer_size);
		_buffer->re_prep_buffers(opts);
	}

	void audioDeviceStopped()
	{
		blog(LOG_INFO, "Stopped (%s)", _device->getName().toStdString().c_str());
	}

	void audioDeviceErrror(const juce::String &errorMessage)
	{
		std::string error = errorMessage.toStdString();
		blog(LOG_ERROR, "Device Error!\n%s", error.c_str());
	}
};

static bool show_panel(obs_properties_t *props, obs_property_t *property, void *data);

class ASIOPlugin {
private:
	AudioIODevice *       _device  = nullptr;
	asio_listener *       listener = nullptr;
	std::vector<uint16_t> _route;
	speaker_layout        _speakers;

public:
	AudioIODevice *getDevice()
	{
		return _device;
	}

	ASIOPlugin::ASIOPlugin(obs_data_t *settings, obs_source_t *source)
	{
		listener           = new asio_listener();
		listener->source   = source;
		listener->first_ts = 0;
		update(settings);
	}

	ASIOPlugin::~ASIOPlugin()
	{
		listener->disconnect();
		delete listener;
	}

	static void *Create(obs_data_t *settings, obs_source_t *source)
	{
		return new ASIOPlugin(settings, source);
	}

	static void Destroy(void *vptr)
	{
		ASIOPlugin *plugin = static_cast<ASIOPlugin *>(vptr);
		delete plugin;
		plugin = nullptr;
	}

	static obs_properties_t *Properties(void *vptr)
	{
		UNUSED_PARAMETER(vptr);
		obs_properties_t *props;
		obs_property_t *  devices;
		obs_property_t *  format;
		obs_property_t *  panel;
		obs_property_t *  route[MAX_AUDIO_CHANNELS];

		props   = obs_properties_create();
		devices = obs_properties_add_list(props, "device_id", obs_module_text("Device"), OBS_COMBO_TYPE_LIST,
				OBS_COMBO_FORMAT_STRING);
		obs_property_set_modified_callback(devices, asio_device_changed);
		fill_out_devices(devices);
		obs_property_set_long_description(devices, obs_module_text("ASIO Devices"));

		format = obs_properties_add_list(props, "speaker_layout", obs_module_text("Format"),
				OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
		for (size_t i = 0; i < known_layouts.size(); i++)
			obs_property_list_add_int(format, known_layouts_str[i].c_str(), known_layouts[i]);
		obs_property_set_modified_callback(format, asio_layout_changed);

		unsigned int recorded_channels = get_obs_output_channels();
		for (size_t i = 0; i < MAX_AUDIO_CHANNELS; i++) {
			route[i] = obs_properties_add_list(props, ("route " + std::to_string(i)).c_str(),
					obs_module_text(("Route." + std::to_string(i)).c_str()), OBS_COMBO_TYPE_LIST,
					OBS_COMBO_FORMAT_INT);
			obs_property_set_long_description(
					route[i], obs_module_text(("Route.Desc." + std::to_string(i)).c_str()));
		}

		panel = obs_properties_add_button2(props, "ctrl", obs_module_text("Control Panel"), show_panel, vptr);
		ASIOPlugin *   plugin = static_cast<ASIOPlugin *>(vptr);
		AudioIODevice *device = nullptr;
		if (plugin)
			device = plugin->getDevice();

		obs_property_set_visible(panel, device && device->hasControlPanel());

		return props;
	}

	void update(obs_data_t *settings)
	{
		std::string    name     = obs_data_get_string(settings, "device_id");
		speaker_layout layout   = (speaker_layout)obs_data_get_int(settings, "speaker_layout");
		AudioCB *      callback = nullptr;

		AudioIODevice *previous_device = (AudioIODevice *)listener->device_index;
		device_buffer *previous_buffer = listener->buffer;

		AudioIODevice *selected_device = nullptr;
		for (int i = 0; i < callbacks.size(); i++) {
			AudioCB *      cb     = callbacks[i];
			AudioIODevice *device = cb->getDevice();
			std::string    n      = cb->getName();
			if (n == name) {
				if (!device) {
					String deviceName = name.c_str();
					device = deviceTypeAsio->createDevice(deviceName, deviceName);
					cb->getBuffer()->device_index = (uint64_t)device;
					cb->setDevice(device, name.c_str());
				}
				selected_device = device;
				_device         = device;
				callback        = cb;
				break;
			}
		}

		if (selected_device == nullptr) {
			listener->device_index = (uint64_t)0;
			listener->disconnect();
			return;
		}

		StringArray in_chs  = _device->getInputChannelNames();
		StringArray out_chs = _device->getOutputChannelNames();
		BigInteger  in      = 0;
		BigInteger  out     = 0;
		in.setRange(0, in_chs.size(), true);
		out.setRange(0, out_chs.size(), true);
		juce::String err;

		/* Open Up Particular Device */
		if (!_device->isOpen()) {
			err = _device->open(in, out, _device->getCurrentSampleRate(),
					_device->getCurrentBufferSizeSamples());
			if (!err.toStdString().empty()) {
				blog(LOG_WARNING, "%s", err.toStdString().c_str());
				listener->device_index = (uint64_t)_device;
				listener->disconnect();
				return;
			}
		}

		if (_device->isOpen() && !_device->isPlaying())
			_device->start(callback);

		if (callback) {
			listener->device_index = (uint64_t)_device;
			device_buffer *buffer  = callback->getBuffer();
			if (previous_device != _device)
				listener->disconnect();

			int recorded_channels = get_audio_channels(layout);
			for (int i = 0; i < recorded_channels; i++) {
				std::string route_str = "route " + std::to_string(i);
				listener->route[i]    = (int)obs_data_get_int(settings, route_str.c_str());
			}
			for (int i = recorded_channels; i < MAX_AUDIO_CHANNELS; i++) {
				listener->route[i]    = -1;
			}

			listener->layout      = layout;
			listener->muted_chs   = listener->_get_muted_chs(listener->route);
			listener->unmuted_chs = listener->_get_unmuted_chs(listener->route);

			if (previous_device != _device)
				buffer->add_listener(listener);
		} else {
			listener->disconnect();
		}
	}

	static void Update(void *vptr, obs_data_t *settings)
	{
		ASIOPlugin *plugin = static_cast<ASIOPlugin *>(vptr);
		if (plugin)
			plugin->update(settings);
	}

	static void Defaults(obs_data_t *settings)
	{
		struct obs_audio_info aoi;
		obs_get_audio_info(&aoi);
		int recorded_channels = get_audio_channels(aoi.speakers);

		// default is muted channels
		for (int i = 0; i < recorded_channels; i++) {
			std::string name = "route " + std::to_string(i);
			obs_data_set_default_int(settings, name.c_str(), -1);
		}

		obs_data_set_default_int(settings, "speaker_layout", aoi.speakers);
	}

	static const char *Name(void *unused)
	{
		UNUSED_PARAMETER(unused);
		return obs_module_text("ASIO");
	}
};

static bool show_panel(obs_properties_t *props, obs_property_t *property, void *data)
{
	if (!data)
		return false;
	ASIOPlugin *   plugin = static_cast<ASIOPlugin *>(data);
	AudioIODevice *device = plugin->getDevice();
	if (device && device->hasControlPanel())
		device->showControlPanel();
	return false;
}

static bool fill_out_channels_modified(obs_properties_t *props, obs_property_t *list, obs_data_t *settings)
{
	UNUSED_PARAMETER(props);
	std::string    name      = obs_data_get_string(settings, "device_id");
	AudioCB *      _callback = nullptr;
	AudioIODevice *_device   = nullptr;

	for (int i = 0; i < callbacks.size(); i++) {
		AudioCB *      cb = callbacks[i];
		AudioIODevice *device = cb->getDevice();
		std::string    n = cb->getName();
		if (n == name) {
			if (!device) {
				String deviceName = name.c_str();
				device = deviceTypeAsio->createDevice(deviceName, deviceName);
				cb->getBuffer()->device_index = (uint64_t)device;
				cb->setDevice(device, name.c_str());
			}
			_device = device;
			_callback = cb;
			break;
		}
	}

	obs_property_list_clear(list);
	obs_property_list_add_int(list, obs_module_text("Mute"), -1);

	if (!_callback || !_device)
		return true;

	juce::StringArray in_names       = _device->getInputChannelNames();
	int               input_channels = in_names.size();

	int i = 0;

	for (; i < input_channels; i++)
		obs_property_list_add_int(list, in_names[i].toStdString().c_str(), i);

	return true;
}

// main callback when a device is switched; takes care of the logic for updating the clients (listeners)
static bool asio_device_changed(obs_properties_t *props, obs_property_t *list, obs_data_t *settings)
{
	size_t          i;
	const char *    curDeviceId = obs_data_get_string(settings, "device_id");
	obs_property_t *route[MAX_AUDIO_CHANNELS];
	speaker_layout  layout = (speaker_layout)obs_data_get_int(settings, "speaker_layout");

	int recorded_channels = get_audio_channels(layout);
	// get channel number from output speaker layout set by obs
	// DWORD recorded_channels = get_obs_output_channels();
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
		for (i = 0; i < MAX_AUDIO_CHANNELS; i++) {
			std::string name = "route " + std::to_string(i);
			route[i]         = obs_properties_get(props, name.c_str());
			obs_property_list_clear(route[i]);
			obs_property_set_modified_callback(route[i], fill_out_channels_modified);
			obs_property_set_visible(route[i], i < recorded_channels);
		}
	}

	return true;
}

static bool asio_layout_changed(obs_properties_t *props, obs_property_t *list, obs_data_t *settings)
{
	obs_property_t *route[MAX_AUDIO_CHANNELS];
	speaker_layout  layout            = (speaker_layout)obs_data_get_int(settings, "speaker_layout");
	int             recorded_channels = get_audio_channels(layout);
	int             i                 = 0;
	for (i = 0; i < MAX_AUDIO_CHANNELS; i++) {
		std::string name = "route " + std::to_string(i);
		route[i]         = obs_properties_get(props, name.c_str());
		obs_property_list_clear(route[i]);
		obs_property_set_modified_callback(route[i], fill_out_channels_modified);
		obs_property_set_visible(route[i], i < recorded_channels);
	}
	return true;
}

static void fill_out_devices(obs_property_t *prop)
{

	StringArray deviceNames(deviceTypeAsio->getDeviceNames());
	for (int j = 0; j < deviceNames.size(); j++) {
		bool found = false;
		for (int i = 0; i < callbacks.size(); i++) {
			AudioCB *      cb     = callbacks[i];
			AudioIODevice *device = cb->getDevice();
			std::string    n      = cb->getName();
			if (deviceNames[j].toStdString() == n) {
				found = true;
				break;
			}
		}
		if (!found) {
			device_buffer *buffer       = new device_buffer();
			char *         name         = bstrdup(deviceNames[j].toStdString().c_str());
			buffer->device_options.name = bstrdup(deviceNames[j].toStdString().c_str());
			AudioCB *cb                 = new AudioCB(buffer, nullptr, name);
			bfree(name);
			callbacks.push_back(cb);
			buffers.push_back(buffer);
		}
	}

	obs_property_list_clear(prop);

	for (int i = 0; i < callbacks.size(); i++) {
		AudioCB *   cb = callbacks[i];
		const char *n  = cb->getName();
		obs_property_list_add_string(prop, n, n);
	}
}

char *os_replace_slash(const char *dir)
{
	dstr dir_str;
	dstr_init_copy(&dir_str, dir);
	dstr_replace(&dir_str, "\\", "/");
	return dir_str.array;
}

bool obs_module_load(void)
{
	obs_audio_info aoi;
	obs_get_audio_info(&aoi);

	MessageManager::getInstance();

	deviceTypeAsio->scanForDevices();
	StringArray deviceNames(deviceTypeAsio->getDeviceNames());
	for (int j = 0; j < deviceNames.size(); j++) {
		device_buffer *buffer       = new device_buffer();
		char *         name         = bstrdup(deviceNames[j].toStdString().c_str());
		buffer->device_options.name = bstrdup(deviceNames[j].toStdString().c_str());
		AudioCB *cb                 = new AudioCB(buffer, nullptr, name);
		bfree(name);
		callbacks.push_back(cb);
		buffers.push_back(buffer);
	}

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

void obs_module_unload(void)
{
	for (int i = 0; i < callbacks.size(); i++) {
		AudioCB *      cb     = callbacks[i];
		AudioIODevice *device = cb->getDevice();
		if (device) {
			if (device->isPlaying())
				device->stop();
			if (device->isOpen())
				device->close();
			delete device;
		}
		device                = nullptr;
		device_buffer *buffer = cb->getBuffer();
		bfree((char *)buffer->device_options.name);
		delete buffer;
		delete cb;
	}

	MessageManager *m = MessageManager::getInstanceWithoutCreating();
	if (m) {
		m->stopDispatchLoop();
		m->deleteInstance();
	}
	delete deviceTypeAsio;
}
