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
//#include "asioselector.h"
#include "circle-buffer.h"
#include "JuceLibraryCode/JuceHeader.h"
/*
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
*/

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("win-asio", "en-US")

#define blog(level, msg, ...) blog(level, "asio-input: " msg, ##__VA_ARGS__)

static void                  fill_out_devices(obs_property_t *prop);
std::vector<asio_listener *> listener_list;

static juce::AudioDeviceManager manager;
class ASIOPlugin;
class AudioCB;

static bool asio_device_changed(obs_properties_t *props, obs_property_t *list, obs_data_t *settings);
static bool fill_out_channels_modified(obs_properties_t *props, obs_property_t *list, obs_data_t *settings);

std::vector<AudioCB *>       callbacks;
std::vector<device_buffer *> buffers;

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
		//_buffer->write_buffer_planar(inputChannelData, numInputChannels * numSamples * sizeof(float), ts);
		_buffer->write_buffer_planar(inputChannelData, numInputChannels, numSamples, ts);
	}

	void audioDeviceAboutToStart(juce::AudioIODevice *device)
	{
		blog(LOG_INFO, "Starting (%s)", device->getName());
		device_buffer_options opts;
		opts.buffer_size   = device->getCurrentBufferSizeSamples();
		opts.channel_count = (uint32_t)device->getActiveInputChannels().countNumberOfSetBits();
		opts.name          = device->getName().toStdString().c_str();
		_buffer->set_audio_format(AUDIO_FORMAT_FLOAT_PLANAR);
		_buffer->update_sample_rate((uint32_t)device->getCurrentSampleRate());
		_buffer->prep_circle_buffer(opts.buffer_size);
		_buffer->re_prep_buffers(opts);
	}

	void audioDeviceStopped()
	{
		blog(LOG_INFO, "Stopped (%s)", _device->getName());
	}

	void audioDeviceErrror(const juce::String &errorMessage)
	{
		std::string error = errorMessage.toStdString();
		blog(LOG_ERROR, "Device Error!\n%s", error.c_str());
	}
};

class ASIOPlugin {
private:
	AudioIODevice *_device = nullptr;
	asio_listener *listener = nullptr;
	std::vector<uint16_t> _route;
	speaker_layout        _speakers;
public:
	ASIOPlugin::ASIOPlugin(obs_data_t *settings, obs_source_t *source)
	{
		listener = new asio_listener();
		listener->source = source;
		listener->first_ts = 0;
		update(settings);
	}

	ASIOPlugin::~ASIOPlugin()
	{
		listener->disconnect();
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
		ASIOPlugin *      plugin = static_cast<ASIOPlugin *>(vptr);
		obs_properties_t *props;
		obs_property_t *  devices;
		obs_property_t *  rate;
		obs_property_t *  bit_depth;
		obs_property_t *  buffer_size;
		obs_property_t *  route[MAX_AUDIO_CHANNELS];

		props = obs_properties_create();
		//obs_properties_set_flags(props, OBS_PROPERTIES_DEFER_UPDATE);
		devices = obs_properties_add_list(props, "device_id", obs_module_text("Device"), OBS_COMBO_TYPE_LIST,
				OBS_COMBO_FORMAT_STRING);
		obs_property_set_modified_callback(devices, asio_device_changed);
		fill_out_devices(devices);
		obs_property_set_long_description(devices, obs_module_text("ASIO Devices"));

		unsigned int recorded_channels = get_obs_output_channels();

		for (size_t i = 0; i < recorded_channels; i++) {
			route[i] = obs_properties_add_list(props, ("route " + std::to_string(i)).c_str(),
					obs_module_text(("Route." + std::to_string(i)).c_str()), OBS_COMBO_TYPE_LIST,
					OBS_COMBO_FORMAT_INT);
			obs_property_set_long_description(
					route[i], obs_module_text(("Route.Desc." + std::to_string(i)).c_str()));
		}

		return props;
	}

	void update(obs_data_t *settings)
	{
		std::string name     = obs_data_get_string(settings, "device_id");
		AudioCB *   callback = nullptr;

		for (int i = 0; i < callbacks.size(); i++) {
			AudioCB *      cb     = callbacks[i];
			AudioIODevice *device = cb->getDevice();
			if (device->getName().toStdString() == name) {
				_device  = device;
				callback = cb;
				break;
			}
		}

		if (_device == nullptr)
			return;

		StringArray in_chs  = _device->getInputChannelNames();
		StringArray out_chs = _device->getOutputChannelNames();
		BigInteger  in      = 0;
		BigInteger  out     = 0;
		in.setRange(0, in_chs.size(), true);
		out.setRange(0, out_chs.size(), true);
		juce::String err;
		if (!_device)
			return;
		/* Open Up Particular Device */
		if (!_device->isOpen()) {
			err = _device->open(in, out, _device->getCurrentSampleRate(),
					_device->getCurrentBufferSizeSamples());
			if (!err.toStdString().empty()) {
				blog(LOG_WARNING, "%s", err.toStdString().c_str());
				return;
			}
		}
		if (_device->isOpen() && !_device->isPlaying())
			_device->start(callback);
		if (callback) {
			listener->device_index = (uint64_t)_device;
			device_buffer *buffer = callback->getBuffer();
			listener->disconnect();

			int recorded_channels = get_obs_output_channels();
			for (int i = 0; i < recorded_channels; i++) {
				std::string route_str = "route " + std::to_string(i);
				listener->route[i]    = (int)obs_data_get_int(settings, route_str.c_str());
			}

			listener->muted_chs   = listener->_get_muted_chs(listener->route);
			listener->unmuted_chs = listener->_get_unmuted_chs(listener->route);

			buffer->add_listener(listener);
		} else {
			//listener->device_index = (uint64_t)_device;
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
		int recorded_channels = get_obs_output_channels();
		for (unsigned int i = 0; i < recorded_channels; i++) {
			std::string name = "route " + std::to_string(i);
			// default is muted channels
			obs_data_set_default_int(settings, name.c_str(), -1);
		}
	}

	static const char *Name(void *unused)
	{
		UNUSED_PARAMETER(unused);
		return obs_module_text("ASIO");
	}
};

static bool fill_out_channels_modified(obs_properties_t *props, obs_property_t *list, obs_data_t *settings)
{
	std::string    name      = obs_data_get_string(settings, "device_id");
	AudioCB *      _callback = nullptr;
	AudioIODevice *_device   = nullptr;

	for (int i = 0; i < callbacks.size(); i++) {
		AudioCB *      cb     = callbacks[i];
		AudioIODevice *device = cb->getDevice();
		if (device->getName().toStdString() == name) {
			_device   = device;
			_callback = cb;
			break;
		}
	}

	obs_property_list_clear(list);
	obs_property_list_add_int(list, obs_module_text("Mute"), -1);

	if (!_callback || !_device)
		return true;

	juce::StringArray names = _device->getInputChannelNames();
	int input_channels = names.size();

	for (unsigned int i = 0; i < input_channels; i++)
		obs_property_list_add_int(list, names[i].toStdString().c_str(), i);

	return true;
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
		for (i = 0; i < recorded_channels; i++) {
			std::string name = "route " + std::to_string(i);
			route[i]         = obs_properties_get(props, name.c_str());
			obs_property_list_clear(route[i]);
			obs_property_set_modified_callback(route[i], fill_out_channels_modified);
		}
	}

	return true;
}

static void fill_out_devices(obs_property_t *prop)
{
	OwnedArray<AudioIODeviceType> types;
	manager.createAudioDeviceTypes(types);

	obs_property_list_clear(prop);

	for (int i = 0; i < callbacks.size(); i++) {
		AudioCB *   cb = callbacks[i];
		const char *n  = cb->getName();
		obs_property_list_add_string(prop, n, n);
	}
}

/* ========================================================================== */
/*                           main module methods                              */
/*                                                                            */
/* ========================================================================== */

char *os_replace_slash(const char *dir)
{
	dstr dir_str;
	int  ret;

	dstr_init_copy(&dir_str, dir);
	dstr_replace(&dir_str, "\\", "/");
	return dir_str.array;
}

bool obs_module_load(void)
{
	obs_audio_info aoi;
	obs_get_audio_info(&aoi);

	char *                            file = obs_module_file("settings.xml");
	std::unique_ptr<juce::XmlElement> xml;
	if (os_file_exists(file))
		xml = juce::parseXML(juce::File(file));
	else
		xml = nullptr;
	bfree(file);

	if (xml.get() == nullptr)
		manager.initialiseWithDefaultDevices(256, 256);
	else
		manager.initialise(256, 256, xml.get(), true);
	AudioDeviceManager::AudioDeviceSetup setup = manager.getAudioDeviceSetup();

	OwnedArray<AudioIODeviceType> types;
	manager.createAudioDeviceTypes(types);

	for (int i = 0; i < types.size(); i++) {
		if (types[i]->getTypeName().toStdString() != "ASIO")
			continue;
		types[i]->scanForDevices();
		StringArray deviceNames(types[i]->getDeviceNames());
		for (int j = 0; j < deviceNames.size(); j++) {
			AudioIODevice *device = types[i]->createDevice(deviceNames[j], deviceNames[j]);
			device_buffer *buffer = new device_buffer();
			char *         name   = bstrdup(deviceNames[j].toStdString().c_str());
			buffer->device_index = (uint64_t)device;
			buffer->device_options.name = bstrdup(deviceNames[j].toStdString().c_str());
			AudioCB *cb = new AudioCB(buffer, device, name);
			bfree(name);
			callbacks.push_back(cb);
			buffers.push_back(buffer);
		}
	}

	AudioIODevice *selected = manager.getCurrentAudioDevice();
	if (selected && !selected->isPlaying())
		selected->start(nullptr);

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
	juce::XmlElement *xml  = manager.createStateXml();
	char *            file = obs_module_file("settings.xml");
	if (xml) {
		const juce::String &str    = xml->getText();
		std::string         xmlstr = str.toStdString();
		blog(LOG_DEBUG, "xml\n%s", xmlstr.c_str());
		xml->writeToFile(juce::File(file), "");
	} else {
		os_quick_write_utf8_file(file, "", 2, false);
	}
	bfree(file);

	AudioCB *      _callback = nullptr;
	AudioIODevice *_device   = nullptr;
	for (int i = 0; i < callbacks.size(); i++) {
		AudioCB *      cb     = callbacks[i];
		AudioIODevice *device = cb->getDevice();
		device_buffer *buffer = cb->getBuffer();
		bfree((char*)buffer->device_options.name);
		delete buffer;
	}
}
