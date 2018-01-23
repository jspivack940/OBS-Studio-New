/*
Copyright (C) 2017 by pkv <pkv.stream@gmail.com>, andersama <anderson.john.alexander@gmail.com>

Based on Pulse Input plugin by Leonhard Oelke.

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
#pragma once

#include <util/bmem.h>
#include <util/platform.h>
#include <util/threading.h>
#include <util/circlebuf.h>
#include <obs-module.h>
#include <vector>
#include <list>
#include <stdio.h>
#include <string>
#include <windows.h>
#include <util/windows/WinHandle.hpp>
#include <bassasio.h>

//#include "RtAudio.h"

//#include <obs-frontend-api.h>
//
//#include <QFileInfo>
//#include <QProcess>
//#include <QLibrary>
//#include <QMainWindow>
//#include <QAction>
//#include <QMessageBox>
//#include <QString>
//#include <QStringList>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("win-asio", "en-US")

#define blog(level, msg, ...) blog(level, "asio-input: " msg, ##__VA_ARGS__)

#define NSEC_PER_SEC  1000000000LL

#define TEXT_BUFFER_SIZE                obs_module_text("BufferSize")
#define TEXT_BUFFER_64_SAMPLES          obs_module_text("64_samples")
#define TEXT_BUFFER_128_SAMPLES         obs_module_text("128_samples")
#define TEXT_BUFFER_256_SAMPLES         obs_module_text("256_samples")
#define TEXT_BUFFER_512_SAMPLES         obs_module_text("512_samples")
#define TEXT_BUFFER_1024_SAMPLES        obs_module_text("1024_samples")
#define TEXT_BITDEPTH                   obs_module_text("BitDepth")

struct asio_source_audio {
	uint8_t       *data[MAX_AUDIO_CHANNELS];
	uint32_t            frames;

	//enum speaker_layout speakers;
	volatile long		speakers;
	enum audio_format   format;
	uint32_t            samples_per_sec;

	uint64_t            timestamp;
};

audio_format get_planar_format(audio_format format) {
	switch (format) {
	case AUDIO_FORMAT_U8BIT:
		return AUDIO_FORMAT_U8BIT_PLANAR;

	case AUDIO_FORMAT_16BIT:
		return AUDIO_FORMAT_16BIT_PLANAR;

	case AUDIO_FORMAT_32BIT:
		return AUDIO_FORMAT_32BIT_PLANAR;

	case AUDIO_FORMAT_FLOAT:
		return AUDIO_FORMAT_FLOAT_PLANAR;
	}

	return format;
}

audio_format get_interleaved_format(audio_format format) {
	switch (format) {
	case AUDIO_FORMAT_U8BIT_PLANAR:
		return AUDIO_FORMAT_U8BIT;

	case AUDIO_FORMAT_16BIT_PLANAR:
		return AUDIO_FORMAT_16BIT;

	case AUDIO_FORMAT_32BIT_PLANAR:
		return AUDIO_FORMAT_32BIT;

	case AUDIO_FORMAT_FLOAT_PLANAR:
		return AUDIO_FORMAT_FLOAT;
	}

	return format;
}

int bytedepth_format(audio_format format);


#define CAPTURE_INTERVAL INFINITE
/*
class device_buffer {
private:
	uint8_t *doubleBuffer[2];
	bool index;
	std::list<>
public:
	uint8_t * get_writable_buffer() {
		return doubleBuffer[!index];
	}

	uint8_t * get_readable_buffer() {
		return doubleBuffer[index];
	}

	bool processBuffers() {
		for()
	}

	device_buffer() {

	}
};
*/
class asio_data;
std::vector<std::list<asio_data*>> source_list;

struct asio_settings {
	const char *device = NULL;
	uint8_t device_index;
	BASS_ASIO_INFO *info = NULL;

	audio_format bit_depth;
	double sample_rate; //44100 or 48000 Hz
	uint16_t buffer_size; // number of samples in buffer
	uint64_t first_ts; //first timestamp

	DWORD input_channels; //total number of input channels
	DWORD output_channels; // number of output channels of device (not used)

	long route[MAX_AUDIO_CHANNELS];
};

class asio_data {
	size_t write_index;
	size_t read_index;
	asio_settings current_settings;
public:
	obs_source_t *source;

	asio_settings new_settings;

	/*asio device and info */
	const char *device;
	uint8_t device_index;
	BASS_ASIO_INFO *info;

	audio_format BitDepth;  //32 bit float only
	double SampleRate;       //44100 or 48000 Hz
	uint16_t BufferSize;     // number of samples in buffer
	uint64_t first_ts;       //first timestamp

	/* channels info */
	DWORD input_channels; //total number of input channels
	DWORD output_channels; // number of output channels of device (not used)
	DWORD recorded_channels; // number of channels passed from device (including muted) to OBS; is at most 8
	long route[MAX_AUDIO_CHANNELS]; // stores the channel re-ordering info
	std::vector<std::vector<short>> route_map;
	std::vector<std::vector<short>> silent_map;
	std::vector<short> unmuted_chs;
	std::vector<short> muted_chs;
	//circlebuf buffers[MAX_AUDIO_CHANNELS];
	circlebuf audio_buffer;

	//signals
	WinHandle stopSignal;
	WinHandle receiveSignal;

	WinHandle reconnectThread;
	WinHandle captureThread;

	bool isASIOActive = false;
	bool reconnecting = false;
	bool previouslyFailed = false;
	bool useDeviceTiming = false;

	CRITICAL_SECTION buffer_section;

	volatile bool settings_updated = false;
	volatile bool route_updated = false;
	volatile bool device_changed = false;

	asio_data() : source(NULL), BitDepth(AUDIO_FORMAT_UNKNOWN),
	SampleRate(0.0), BufferSize(0), first_ts(0), read_index(0),
	write_index(0), device_index(-1){
		InitializeCriticalSection(&buffer_section);
		memset(&route[0], -1, sizeof(DWORD) * 8);
		circlebuf_init(&audio_buffer);
		circlebuf_reserve(&audio_buffer, 480 * sizeof(asio_source_audio));
		//0 out everything
		//memset(audio_buffer.data, 0, 480 * sizeof(asio_source_audio));
		for (int i = 0; i < 480; i++) {
			circlebuf_push_back(&audio_buffer, &asio_source_audio(), sizeof(asio_source_audio));
		}

		stopSignal = CreateEvent(nullptr, true, false, nullptr);
		receiveSignal = CreateEvent(nullptr, false, false, nullptr);

		captureThread = CreateThread(nullptr, 0, asio_data::capture_thread, this, 0, nullptr);
	}

	~asio_data() {
		DeleteCriticalSection(&buffer_section);
	}

	asio_source_audio* get_writeable_source_audio() {
		return (asio_source_audio*)circlebuf_data(&audio_buffer, write_index * sizeof(asio_source_audio));
	}

	asio_source_audio* get_readable_source_audio() {
		return (asio_source_audio*)circlebuf_data(&audio_buffer, read_index * sizeof(asio_source_audio));
	}
/*
	static const obs_audio_info obs_audio_information;
	obs_get_audio_info(&obs_audio_information);
*/
	//obs_source_audio out
	void write_buffer(DWORD ch, uint8_t* buffer, size_t buffer_size) {
		static volatile long callback_count = 0;
		static volatile long ignore_callbacks = 0;

		if (os_atomic_load_long(&ignore_callbacks) > 0) {
			os_atomic_dec_long(&ignore_callbacks);
			return;
		}

		//protect against a change of settings mid callbacks
		if (!os_atomic_load_bool(&settings_updated)) {
			//validate the device index is correct
			if (current_settings.device_index < source_list.size()) {
				//validate the incoming ch is within the size of the binned chs
				if (ch < route_map.size()) {
					//iterate through all the route locations that need this ch
					for (size_t i = 0; i < route_map[ch].size(); i++) {
						//validate the routing location fits within the needed routing numbers
						if (route_map[ch][i] >= 0 && route_map[ch][i] < MAX_AUDIO_CHANNELS) {
							uint8_t* data = (uint8_t*)malloc(buffer_size);
							memcpy(data, buffer, buffer_size);
							asio_source_audio* _source_audio = (asio_source_audio*)circlebuf_data(&audio_buffer, write_index * sizeof(asio_source_audio));
							_source_audio->data[route_map[ch][i]] = data;
							os_atomic_inc_long(&(_source_audio->speakers));
							if (os_atomic_load_long(&(_source_audio->speakers)) == unmuted_chs.size()) {
								//write_info();
								_source_audio->frames = buffer_size / bytedepth_format(current_settings.bit_depth);//bytedepth_format(BitDepth);
								_source_audio->format = get_planar_format(current_settings.bit_depth);
								_source_audio->samples_per_sec = current_settings.sample_rate;//SampleRate;
								_source_audio->timestamp = os_gettime_ns() - ((_source_audio->frames * NSEC_PER_SEC) / current_settings.sample_rate);

								//move write_index one over
								write_index++;
								//wrap
								write_index = write_index % 480;

								SetEvent(receiveSignal);
							}
						}
					}
				}
			}
		}
		else {
			//settings updating force reset
			asio_source_audio* _source_audio = (asio_source_audio*)circlebuf_data(&audio_buffer, write_index * sizeof(asio_source_audio));
			os_atomic_set_long(&(_source_audio->speakers), 0);
			apply_settings();
			//ignore the remaining number of callbacks
			os_atomic_set_long(&ignore_callbacks, current_settings.input_channels - os_atomic_load_long(&callback_count));
			os_atomic_set_long(&callback_count, 0);
		}

		os_atomic_inc_long(&callback_count);
		if (os_atomic_load_long(&callback_count) >= current_settings.input_channels) {
			os_atomic_set_long(&callback_count, 0);
		}
		//no need to write, wasn't 
	}

	void update_settings(asio_settings settings) {
		if (settings.device == NULL || settings.device[0] == '\0') {
			blog(LOG_INFO, "Device not yet set \n");
		}
		else if (new_settings.device == NULL || new_settings.device[0] == '\0') {
			new_settings.device = bstrdup(settings.device);
			os_atomic_set_bool(&settings_updated, true);
		}
		else {
			if (strcmp(settings.device, new_settings.device) != 0) {
				new_settings.device = bstrdup(settings.device);
				os_atomic_set_bool(&device_changed, true);
				os_atomic_set_bool(&settings_updated, true);
			}
		}

		if (settings.bit_depth != current_settings.bit_depth) {
			new_settings.bit_depth = settings.bit_depth;
			os_atomic_set_bool(&settings_updated, true);
		}
		if (settings.buffer_size != current_settings.buffer_size) {
			new_settings.buffer_size = settings.buffer_size;
			os_atomic_set_bool(&settings_updated, true);
		}
		if (settings.info != current_settings.info) {
			new_settings.info = settings.info;
			if (settings.info != NULL && current_settings.info != NULL && strcmp(settings.info->name, current_settings.info->name) != 0) {
				os_atomic_set_bool(&settings_updated, true);
			}
		} 
		if (settings.sample_rate != current_settings.sample_rate) {
			new_settings.sample_rate = settings.sample_rate;
			os_atomic_set_bool(&settings_updated, true);
		}
		if (settings.input_channels != current_settings.input_channels) {
			new_settings.input_channels = settings.input_channels;
			os_atomic_set_bool(&settings_updated, true);
		}
		if (settings.output_channels != current_settings.output_channels) {
			new_settings.output_channels = settings.output_channels;
			os_atomic_set_bool(&settings_updated, true);
		}


		for (short i = 0; i < MAX_AUDIO_CHANNELS; i++) {
			if (settings.route[i] != current_settings.route[i]) {
				new_settings.route[i] = settings.route[i];
				os_atomic_set_bool(&route_updated, true);
				os_atomic_set_bool(&settings_updated, true);
			}
		}

		new_settings = settings;
	}

	void apply_settings() {
		if (os_atomic_load_bool(&settings_updated)) {
			//find our data in the device list
			if (os_atomic_load_bool(&device_changed)) {
				std::list<asio_data*>::iterator src;
				src = std::find(source_list[current_settings.device_index].begin(), source_list[current_settings.device_index].end(), this);
				//splice into the new location
				source_list[new_settings.device_index].splice(source_list[new_settings.device_index].end(), source_list[current_settings.device_index], src);
			}
			else {
				source_list[new_settings.device_index].push_back(this);
			}
			current_settings = new_settings;
			route_map = _bin_map_unmuted(current_settings.route);
			silent_map = _bin_map_muted(current_settings.route);
			unmuted_chs = _get_unmuted_chs(current_settings.route);
			muted_chs = _get_muted_chs(current_settings.route);
		}
		os_atomic_set_bool(&settings_updated, false);
	}

	bool read_buffer() {
		while ((read_index % 480) != (write_index % 480)) {
			obs_source_audio data;
			//PLANAR DATA NEEDS TO BE SET UP AHEAD OF TIME
			asio_source_audio* asiobuf = get_readable_source_audio();
			if (!asiobuf) {
				read_index++;
				continue;
			}

			data.format = asiobuf->format;
			if (data.format == AUDIO_FORMAT_UNKNOWN) {
				//we can't output this...this'll be junk
				read_index++;
				continue;
			}

			data.frames = asiobuf->frames;
			data.samples_per_sec = asiobuf->samples_per_sec;
			data.timestamp = asiobuf->timestamp;
			if (!first_ts) {
				first_ts = data.timestamp;
				read_index;
				continue;
			}
			//HANDLING PLANAR ONLY FROM THIS POINT FORWARD!
			for (short i = 0; i < MAX_AUDIO_CHANNELS; i++) {
				data.data[i] = asiobuf->data[i];
			}
			long speakers = unmuted_chs.size();//input_channels;//os_atomic_load_long(&(asiobuf->speakers));
			if (speakers == 0) {
				//also can't output this...this'll be junk (no speakers)
				read_index++;
				continue;
			}
			//upscale if needed
			if (speakers == 7) {
				speakers = 8;
				data.data[7] = (uint8_t*)calloc(data.frames,bytedepth_format(BitDepth));
			}
			data.speakers = (speaker_layout)speakers;
			//so long as the data's properly formatted, we're good to go
			
			obs_source_output_audio(source, &data);
			//obs is done processing the audio

			//reset the speaker count (the only thing we need to do w/ this frame to "delete") it
			os_atomic_set_long(&(asiobuf->speakers), 0);
			//cleanup

			//apply_settings();
			/*
			for (short i = 0; i < MAX_AUDIO_CHANNELS; i++) {
				if (asiobuf->data[i]) {
					free(asiobuf->data[i]);
				}
			}
			*/

			read_index++;
		}
		//
		read_index = read_index % 480;
		write_index = write_index % 480;

		return true;
	}

	static DWORD WINAPI capture_thread(void *data) {
		asio_data *source = static_cast<asio_data*>(data);
		std::string thread_name = "asio capture thread";//source->device;
		//thread_name += " capture thread";
		os_set_thread_name(thread_name.c_str());

		HANDLE signals[2] = { source->receiveSignal, source->stopSignal };

		source->route_map = _bin_map_unmuted( source->route );
		source->silent_map = _bin_map_muted( source->route );

		while (true) {
			int waitResult = WaitForMultipleObjects(2, signals, false, CAPTURE_INTERVAL);
			if (waitResult == WAIT_OBJECT_0 || waitResult == WAIT_TIMEOUT) {
				source->read_buffer();
			}
			else if (waitResult == WAIT_OBJECT_0 + 1) {
				break;
			}
			else {
				blog(LOG_ERROR, "[%s::%s] Abnormal termination of %s", typeid(*source).name(), __FUNCTION__, thread_name.c_str());
				break;
			}
		}

		return 0;
	}

	static std::vector<std::vector<short>> _bin_map_unmuted(long route_array[]) {
		std::vector<std::vector<short>> bins;
		//std::vector<short> out;// = std::vector<short>(256);
		long max_size = 0;

		for (short i = 0; i < MAX_AUDIO_CHANNELS; i++) {
			long ch_index = route_array[i] + 1;
			max_size = max(ch_index, max_size);
		}
		bins.resize(max_size);
		
		for (short i = 0; i < MAX_AUDIO_CHANNELS; i++) {
			if (route_array[i] >= 0) {
				bins[route_array[i]].push_back(i);
			}
		}

		return bins;
	}
	
	static std::vector<std::vector<short>> _bin_map_muted(long route_array[]) {
		std::vector<std::vector<short>> bins;
		//std::vector<short> out;// = std::vector<short>(256);
		std::vector<short> unmuted_chs;
		std::vector<short> muted_chs;

		long max_size = 0;
		for (short i = 0; i < MAX_AUDIO_CHANNELS; i++) {
			long ch_index = route_array[i] + 1;
			max_size = max(ch_index, max_size);
			if (route_array[i] == -1) {
				muted_chs.push_back(i);
			}
			else if (route_array[i] >= 0) {
				unmuted_chs.push_back(i);
			}
		}
		bins.resize(max_size);

		for (size_t j = 0; j < max_size; j++) {
			bool found = false;
			for (size_t k = 0; k < unmuted_chs.size(); k++) {
				if (unmuted_chs[k] == j) {
					found = true;
					break;
				}
			}
			if (!found) {
				short index = muted_chs.back();
				muted_chs.pop_back();
				bins[j].push_back( index );
			}
		}

		return bins;
	}

	static std::vector<short> _get_muted_chs(long route_array[]) {
		std::vector<short> silent_chs;
		long max_size = 0;
		for (short i = 0; i < MAX_AUDIO_CHANNELS; i++) {
			if (route_array[i] == -1) {
				silent_chs.push_back(i);
			}
		}
		return silent_chs;
	}

	static std::vector<short> _get_unmuted_chs(long route_array[]) {
		std::vector<short> unmuted_chs;
		long max_size = 0;
		for (short i = 0; i < MAX_AUDIO_CHANNELS; i++) {
			if (route_array[i] >= 0) {
				unmuted_chs.push_back(i);
			}
		}
		return unmuted_chs;
	}

	static long* make_route(std::vector<short> hash_map) {
		short mapped = 0;
		long *arr = (long*)malloc(MAX_AUDIO_CHANNELS * sizeof(long));
		memset(arr, -1, MAX_AUDIO_CHANNELS * sizeof(long));

		for (size_t i = 0; i < hash_map.size(); i++) {
			if (hash_map[i] >= 0 && hash_map[i] < MAX_AUDIO_CHANNELS) {
				arr[hash_map[i]] = i;
			}
		}

		return arr;
	}
};

CRITICAL_SECTION source_list_mutex;

/* ======================================================================= */

/* conversion between BASS_ASIO and obs */

enum audio_format asio_to_obs_audio_format(DWORD format)
{
	switch (format) {
	case BASS_ASIO_FORMAT_16BIT:   return AUDIO_FORMAT_16BIT;
	case BASS_ASIO_FORMAT_32BIT:   return AUDIO_FORMAT_32BIT;
	case BASS_ASIO_FORMAT_FLOAT:   return AUDIO_FORMAT_FLOAT;
	default:                       break;
	}

	return AUDIO_FORMAT_UNKNOWN;
}

int bytedepth_format(audio_format format)
{
	return (int)get_audio_bytes_per_channel(format);
}

int bytedepth_format(DWORD format) {
	return bytedepth_format(asio_to_obs_audio_format(format));
}

DWORD obs_to_asio_audio_format(audio_format format)
{
	switch (format) {

	case AUDIO_FORMAT_16BIT:
		return BASS_ASIO_FORMAT_16BIT;
		// obs doesn't have 24 bit
	case AUDIO_FORMAT_32BIT:
		return BASS_ASIO_FORMAT_32BIT;

	case AUDIO_FORMAT_FLOAT:
	default:
		return BASS_ASIO_FORMAT_FLOAT;
	}
	// default to 32 float samples for best quality

}

enum speaker_layout asio_channels_to_obs_speakers(unsigned int channels)
{
	switch (channels) {
	case 1:   return SPEAKERS_MONO;
	case 2:   return SPEAKERS_STEREO;
	case 3:   return SPEAKERS_2POINT1;
	case 4:   return SPEAKERS_4POINT0;
	case 5:   return SPEAKERS_4POINT1;
	case 6:   return SPEAKERS_5POINT1;
		/* no layout for 7 channels */
	case 8:   return SPEAKERS_7POINT1;
	}
	return SPEAKERS_UNKNOWN;
}

/*****************************************************************************/
// get number of output channels
DWORD get_obs_output_channels() {
	// get channel number from output speaker layout set by obs
	struct obs_audio_info aoi;
	obs_get_audio_info(&aoi);
	DWORD recorded_channels = get_audio_channels(aoi.speakers);
	return recorded_channels;
}

// get device number
uint8_t getDeviceCount() {
	uint8_t a, count = 0;
	BASS_ASIO_DEVICEINFO info;
	for (a = 0; BASS_ASIO_GetDeviceInfo(a, &info); a++) {
		blog(LOG_INFO, "device index is : %i and name is : %s", a, info.name);
		count++;
	}

	return count;
}

// get the device index from a device name : the current index can be retrieved from DWORD BASS_ASIO_GetDevice();
DWORD get_device_index(const char *device) {
	DWORD device_index = 0;
	int i, res;
	BASS_ASIO_SetUnicode(false);
	BASS_ASIO_DEVICEINFO info;
	bool ret;
	int numOfDevices = getDeviceCount();
	for (i = 0; i < numOfDevices; i++) {
		ret = BASS_ASIO_GetDeviceInfo(i, &info);
		if (!ret)
			blog(LOG_ERROR, "Invalid device index");
		res = strcmp(info.name, device);
		if (res == 0) {
			device_index = i;
			break;
		}
	}
	return device_index;
}

// call the control panel
static bool DeviceControlPanel(obs_properties_t *props,
	obs_property_t *property, void *data) {
	if (!BASS_ASIO_ControlPanel()) {
		switch (BASS_ASIO_ErrorGetCode()) {
		case BASS_ERROR_INIT:
			blog(LOG_ERROR, "Init not called\n");
			break;
		case BASS_ERROR_UNKNOWN:
			blog(LOG_ERROR, "Unknown error\n");
		}
		return false;
	}
	else {
		int device_index = BASS_ASIO_GetDevice();
		BASS_ASIO_INFO info;
		BASS_ASIO_GetInfo(&info);
		blog(LOG_INFO, "Console loaded for device %s with index %i\n", 
			info.name, device_index);
	}
	return true;
}

/*****************************************************************************/

void asio_update(void *vptr, obs_data_t *settings);
void asio_destroy(void *vptr);

//creates the device list
void fill_out_devices(obs_property_t *list) {
	int numOfDevices = (int)getDeviceCount();
	char** names = new char*[numOfDevices];
	blog(LOG_INFO, "ASIO Devices: %i\n", numOfDevices);
	BASS_ASIO_SetUnicode(false);
	BASS_ASIO_DEVICEINFO info;
	for (int i = 0; i < numOfDevices; i++) {
		BASS_ASIO_GetDeviceInfo(i, &info);
		blog(LOG_INFO, "device  %i = %ls\n", i, info.name);
		std::string test = info.name;
		char* cstr = new char[test.length() + 1];
		strcpy(cstr, test.c_str());
		names[i] = cstr;
		blog(LOG_INFO, "Number of ASIO Devices: %i\n", numOfDevices);
		blog(LOG_INFO, "device %i  = %s added successfully.\n", i, names[i]);
		obs_property_list_add_string(list, names[i], names[i]);
	}
}

/* Creates list of input channels ; a muted channel has route value -1 and
* is recorded. The user can unmute the channel later.
*/
static bool fill_out_channels_modified(obs_properties_t *props, obs_property_t *list, obs_data_t *settings) {
	BASS_ASIO_INFO info;
	bool ret = BASS_ASIO_GetInfo(&info);
	if (!ret) {
		blog(LOG_ERROR, "Unable to retrieve info on the current driver \n"
			"error number is : %i \n; check BASS_ASIO_ErrorGetCode\n",
			BASS_ASIO_ErrorGetCode());
	}
	// DEBUG: check that the current device in bass thread is the correct one
	// once code is fine the check can be removed
	const char* device = obs_data_get_string(settings, "device_id");
	if (!strcmp(device, info.name)) {
		blog(LOG_ERROR, "Device loaded is not the one in settings\n");
	}
	//get the device info
	DWORD input_channels = info.inputs;
	obs_property_list_clear(list);
	obs_property_list_add_int(list, "mute", -1);
	for (DWORD i = 0; i < input_channels; i++) {
		char** names = new char*[34];
		std::string test = info.name;
		test = test + " " + std::to_string(i);
		char* cstr = new char[test.length() + 1];
		strcpy(cstr, test.c_str());
		names[i] = cstr;
		obs_property_list_add_int(list, names[i], i);
	}
	return true;
}

//creates list of input sample rates supported by the device and OBS (obs supports only 44100 and 48000)
static bool fill_out_sample_rates(obs_properties_t *props, obs_property_t *list, obs_data_t *settings) {
	BASS_ASIO_INFO info;
	bool ret = BASS_ASIO_GetInfo(&info);
	if (!ret) {
		blog(LOG_ERROR, "Unable to retrieve info on the current driver \n"
			"error number is : %i \n; check BASS_ASIO_ErrorGetCode\n",
			BASS_ASIO_ErrorGetCode());
	}
	// DEBUG: check that the current device in bass thread is the correct one
	// once code is fine the check can be removed
	const char* device = obs_data_get_string(settings, "device_id");
	if (!strcmp(device, info.name)) {
		blog(LOG_ERROR, "Device loaded is not the one in settings\n");
	}

	obs_property_list_clear(list);
	//get the device info
	ret = BASS_ASIO_CheckRate(44100);
	if (ret) {
		std::string rate = "44100 Hz";
		char* cstr = new char[rate.length() + 1];
		strcpy(cstr, rate.c_str());
		obs_property_list_add_int(list, cstr, 44100);
	}
	else {
		blog(LOG_INFO, "Device loaded does not support 44100 Hz sample rate\n");
	}
	ret = BASS_ASIO_CheckRate(48000);
	if (ret) {
		std::string rate = "48000 Hz";
		char* cstr = new char[rate.length() + 1];
		strcpy(cstr, rate.c_str());
		obs_property_list_add_int(list, cstr, 48000);
	}
	else {
		blog(LOG_INFO, "Device loaded does not support 48000 Hz sample rate\n");
	}
	return true;
}

//create list of supported audio formats
static bool fill_out_bit_depths(obs_properties_t *props, obs_property_t *list, obs_data_t *settings) {
	BASS_ASIO_INFO info;
	bool ret = BASS_ASIO_GetInfo(&info);
	if (!ret) {
		blog(LOG_ERROR, "Unable to retrieve info on the current driver \n"
			"error number is : %i \n; check BASS_ASIO_ErrorGetCode\n",
			BASS_ASIO_ErrorGetCode());
	}
	// DEBUG: check that the current device in bass thread is the correct one
	// once code is fine the check can be removed
	const char* device = obs_data_get_string(settings, "device_id");
	if (!strcmp(device, info.name)) {
		blog(LOG_ERROR, "Device loaded is not the one in settings\n");
	}

	//get the device channel info
	BASS_ASIO_CHANNELINFO channelInfo;
	ret = BASS_ASIO_ChannelGetInfo(true, 0, &channelInfo);
	if (!ret) {
		blog(LOG_ERROR, "Unable to retrieve channel info on the current driver \n"
			"error number is : %i \n; check BASS_ASIO_ErrorGetCode\n",
			BASS_ASIO_ErrorGetCode());
	}
	
	obs_property_list_clear(list);
	if (channelInfo.format == BASS_ASIO_FORMAT_16BIT) {
		obs_property_list_add_int(list, "16 bit (native)", AUDIO_FORMAT_16BIT);
		obs_property_list_add_int(list, "32 bit", AUDIO_FORMAT_32BIT);
		obs_property_list_add_int(list, "32 bit float", AUDIO_FORMAT_FLOAT);
	}
	else if (channelInfo.format == BASS_ASIO_FORMAT_32BIT) {
		obs_property_list_add_int(list, "16 bit", AUDIO_FORMAT_16BIT);
		obs_property_list_add_int(list, "32 bit (native)", AUDIO_FORMAT_32BIT);
		obs_property_list_add_int(list, "32 bit float", AUDIO_FORMAT_FLOAT);
	}
	else if (channelInfo.format == BASS_ASIO_FORMAT_FLOAT) {
		obs_property_list_add_int(list, "16 bit", AUDIO_FORMAT_16BIT);
		obs_property_list_add_int(list, "32 bit", AUDIO_FORMAT_32BIT);
		obs_property_list_add_int(list, "32 bit float (native)", AUDIO_FORMAT_FLOAT);
	}
	else {
		blog(LOG_ERROR, "Your device uses unsupported bit depth.\n"
			"Only 16 bit, 32 bit signed int and 32 bit float are supported.\n"
			"Change accordingly your device settings.\n"
			"Forcing bit depth to 32 bit float");
		obs_property_list_add_int(list, "32 bit float", AUDIO_FORMAT_FLOAT);
		return false;
	}
	return true;
}

static bool asio_device_changed(obs_properties_t *props,
	obs_property_t *list, obs_data_t *settings)
{
	const char *curDeviceId = obs_data_get_string(settings, "device_id");
	obs_property_t *sample_rate = obs_properties_get(props, "sample rate");
	obs_property_t *bit_depth = obs_properties_get(props, "bit depth");
	// be sure to set device as current one

	size_t itemCount = obs_property_list_item_count(list);
	bool itemFound = false;

	for (size_t i = 0; i < itemCount; i++) {
		const char *DeviceId = obs_property_list_item_string(list, i);
		if (strcmp(DeviceId, curDeviceId) == 0) {
			itemFound = true;
			break;
		}
	}

	if (!itemFound) {
		obs_property_list_insert_string(list, 0, " ", curDeviceId);
		obs_property_list_item_disable(list, 0, true);
	}
	else {
		DWORD device_index = get_device_index(curDeviceId);
		bool ret = BASS_ASIO_SetDevice(device_index);
		if (!ret) {
			blog(LOG_ERROR, "Unable to set device %i\n", device_index);
			if (BASS_ASIO_ErrorGetCode() == BASS_ERROR_INIT) {
				BASS_ASIO_Init(device_index, BASS_ASIO_THREAD);
				BASS_ASIO_SetDevice(device_index);
			}
			else if (BASS_ASIO_ErrorGetCode() == BASS_ERROR_DEVICE) {
				blog(LOG_ERROR, "Device index is invalid\n");
			}
		}		
	}
	// get channel number from output speaker layout set by obs
	DWORD recorded_channels = get_obs_output_channels();

	obs_property_t *route[MAX_AUDIO_CHANNELS];
	int pad_digits = (int)floor(log10(abs(MAX_AUDIO_CHANNELS))) + 1;
	const char* route_name_format = "route %i";
	char* route_name = new char[strlen(route_name_format) + pad_digits];
	if (itemFound) {
		for (unsigned int i = 0; i < recorded_channels; i++) {
			std::string name = "route " + std::to_string(i);
			route[i] = obs_properties_get(props, name.c_str());
			obs_property_list_clear(route[i]);
			sprintf(route_name, route_name_format, i);
			obs_data_set_default_int(settings, route_name, -1); // default is muted channels
			obs_property_set_modified_callback(route[i], fill_out_channels_modified);
		}
	}
	obs_property_list_clear(sample_rate);
	obs_property_list_clear(bit_depth);

	obs_property_set_modified_callback(sample_rate, fill_out_sample_rates);
	obs_property_set_modified_callback(bit_depth, fill_out_bit_depths);

	return true;
}

int mix(uint8_t *inputBuffer, obs_source_audio *out, size_t bytes_per_ch, int route[], unsigned int recorded_device_chs = UINT_MAX) {
	DWORD recorded_channels = get_obs_output_channels();
	short j = 0;
	for (size_t i = 0; i < recorded_channels; i++) {
		if (route[i] > -1 && route[i] < (int)recorded_device_chs) {
			out->data[j++] = inputBuffer + route[i] * bytes_per_ch;
		}
		else if (route[i] == -1) {
			uint8_t * silent_buffer;
			silent_buffer = (uint8_t *)calloc(bytes_per_ch, 1);
			out->data[j++] = silent_buffer;
		}
	}
	return true;
}

DWORD CALLBACK create_asio_buffer(BOOL input, DWORD channel, void *buffer, DWORD BufSize, void *source_list) {
	std::list<asio_data*> *sources = (std::list<asio_data*> *)source_list;
	//BASS_ASIO_INFO info;
	//BASS_ASIO_GetInfo(&info); 
	for (std::list<asio_data*>::iterator it = sources->begin(); it != sources->end(); it++) {
		asio_data* source = *it;
		source->write_buffer(channel, (uint8_t*)buffer, BufSize);
	}

	return 0;
}

void asio_init(struct asio_data *data)
{
	// get info, useful for debug
	BASS_ASIO_INFO info; 
	bool ret = BASS_ASIO_GetInfo(&info);
	if (!ret) {
		blog(LOG_ERROR, "Unable to retrieve info on the current driver \n"
			"error number is : %i \n; check BASS_ASIO_ErrorGetCode\n",
			BASS_ASIO_ErrorGetCode());
	}
	data->info = &info;
	audio_format BitDepth = data->BitDepth;
	DWORD bassBitdepth = obs_to_asio_audio_format(BitDepth);

	uint8_t deviceNumber = getDeviceCount();
	if (deviceNumber < 1) {
		blog(LOG_INFO, "\nNo audio devices found!\n");
	}

	// get channel number from output speaker layout set by obs
	DWORD recorded_channels = get_obs_output_channels();

	// initialize channels: enable first channel, join all the others
	// we initialize ALL the device input channels
	// First : enable channel 0

	// convert all samples to float irrespective of what the user sets in the settings ==> disable this setting later
	ret = BASS_ASIO_ChannelSetFormat(true, 0, bassBitdepth);
	if (!ret)
	{
		blog(LOG_ERROR, "Could not set channel bitdepth; error code : %i", BASS_ASIO_ErrorGetCode());
	}
	// enable channels 0 and link to callback
	DWORD device_index = get_device_index(info.name);
	for (DWORD i = 0; i < info.inputs; i++) {
		ret = BASS_ASIO_ChannelEnable(true, i, &create_asio_buffer, &source_list[device_index]);//data
		if (!ret)
		{
			blog(LOG_ERROR, "Could not enable channel %i; error code : %i", i, BASS_ASIO_ErrorGetCode());
		}
	}

	//don't join the other channels
	/*
	for (DWORD i = 1; i < data->channels; i++) {
		BASS_ASIO_ChannelJoin(true, i, 0);
	}
	*/
	// check buffer size is legit; if not set it to bufpref
	// to be implemented : to avoid issues, force to bufpref
	// this ignores any setting; bufpref is most likely set in asio control panel
	data->BufferSize = info.bufpref;
	//check channel setup
	DWORD checkrate = BASS_ASIO_GetRate();
	blog(LOG_INFO, "sample rate is set in device to %i.\n", checkrate);
	DWORD checkbitdepth = BASS_ASIO_ChannelGetFormat(true, 0);
	blog(LOG_INFO, "bitdepth is set in device to %i.\n", checkbitdepth);

	//start asio device
	if (!BASS_ASIO_IsStarted())
		BASS_ASIO_Start(data->BufferSize, recorded_channels);
	switch (BASS_ASIO_ErrorGetCode()) {
	case BASS_ERROR_INIT:
		blog(LOG_ERROR, "Error: Bass asio not initialized.\n");
	case BASS_ERROR_ALREADY:
		blog(LOG_ERROR, "Error: device already started\n");
		BASS_ASIO_Stop(); 
		BASS_ASIO_Start(data->BufferSize, recorded_channels);
	case BASS_ERROR_NOCHAN:
		blog(LOG_ERROR, "Error: channels have not been enabled so can not start\n");
	case BASS_ERROR_UNKNOWN:
	default:
			blog(LOG_ERROR, "ASIO init: Unknown error when trying to start the device\n");
	}
}

static void * asio_create(obs_data_t *settings, obs_source_t *source)
{
	asio_data *data = new asio_data;

	data->source = source;
	data->first_ts = 0;
	data->device = NULL;
	data->info = NULL;

	asio_update(data, settings);

	//if (obs_data_get_string(settings, "device_id")) {
	//	asio_init(data);
	//}

	return data;
}

void asio_destroy(void *vptr)
{
	struct asio_data *data = (asio_data *)vptr;

	if (BASS_ASIO_IsStarted())
		BASS_ASIO_Stop();

	BASS_ASIO_Free();

	delete data;
}

/* set all settings to asio_data struct and pass to driver */
void asio_update(void *vptr, obs_data_t *settings)
{
	struct asio_data *data = (asio_data *)vptr;
	const char *device;
	unsigned int rate;
	audio_format BitDepth;
	uint16_t BufferSize;
	unsigned int channels;
	BASS_ASIO_INFO info;
	bool ret;
	int res;
	DWORD route[MAX_AUDIO_CHANNELS];
	DWORD device_index;
	int numDevices = getDeviceCount();
	bool device_changed = false;
	const char *prev_device;
	DWORD prev_device_index;
	asio_settings new_settings = asio_settings();

	// get channel number from output speaker layout set by obs
	DWORD recorded_channels = get_obs_output_channels();
	data->recorded_channels = recorded_channels;

	// get device from settings
	device = obs_data_get_string(settings, "device_id");

	if (device == NULL || device[0] == '\0') {
		blog(LOG_INFO, "Device not yet set \n");
	} else if (data->device == NULL || data->device[0] == '\0') {
		//data->device = bstrdup(device);
		new_settings.device = bstrdup(device);
	} else {
		if (strcmp(device, data->device) != 0) {
			prev_device = bstrdup(data->device);
			data->device = bstrdup(device);
			new_settings.device = bstrdup(device);
			device_changed = true;
		}
	}

	if (device != NULL && device[0] != '\0') {
		device_index = get_device_index(device);
		if (!device_changed) {
			prev_device_index = device_index;
		}
		else {
			prev_device_index = get_device_index(prev_device);
		}
		// check if device is already initialized
		ret = BASS_ASIO_Init(device_index, BASS_ASIO_THREAD);

		if (!ret) {
			res = BASS_ASIO_ErrorGetCode();
			switch (res) {
			case BASS_ERROR_DEVICE:
				blog(LOG_ERROR, "The device number specified is invalid.\n");
				break;
			case BASS_ERROR_ALREADY:
				blog(LOG_ERROR, "The device has already been initialized\n");
				break;
			case BASS_ERROR_DRIVER:
				blog(LOG_ERROR, "The driver could not be initialized\n");
				break;
			}
		}
		else {
			blog(LOG_INFO, "Device %i was successfully initialized\n", device_index);
		}
		ret = BASS_ASIO_SetDevice(device_index);
		if (!ret) {
			res = BASS_ASIO_ErrorGetCode();
			switch (res) {
			case BASS_ERROR_DEVICE:
				blog(LOG_ERROR, "The device number specified is invalid.\n");
				break;
			case BASS_ERROR_INIT:
				blog(LOG_ERROR, "The device has not been initialized\n");
				break;
			}
		}

		ret = BASS_ASIO_GetInfo(&info);
		if (!ret) {
			blog(LOG_ERROR, "Unable to retrieve info on the current driver \n"
				"driver is not initialized\n");
		}
		// DEBUG: check that the current device in bass thread is the correct one
		// once code is fine the check can be removed
		if (!strcmp(device, info.name)) {
			blog(LOG_ERROR, "Device loaded is not the one in settings\n");
		}

		bool route_changed = false;
		for (unsigned int i = 0; i < recorded_channels; i++) {
			std::string route_str = "route " + std::to_string(i);
			route[i] = (int)obs_data_get_int(settings, route_str.c_str());
			new_settings.route[i] = route[i];
			if (data->route[i] != route[i]) {
				data->route[i] = route[i];
				route_changed = true;
			}
		}

		rate = (double)obs_data_get_int(settings, "sample rate");
		if (data->SampleRate != rate) {
			data->SampleRate = rate;
			bool ret = BASS_ASIO_SetRate(rate);
			switch (BASS_ASIO_ErrorGetCode()) {
			case BASS_ERROR_NOTAVAIL:
				blog(LOG_ERROR, "Selected sample rate not supported by device\n");
			case BASS_ERROR_INIT:
				blog(LOG_ERROR, "Device not initialized; sample rate can not be set\n");
			case BASS_ERROR_UNKNOWN:
				blog(LOG_ERROR, "Unknown error when trying to set the sample rate\n");
			}
		}
		new_settings.sample_rate = rate;

		BufferSize = (uint16_t)obs_data_get_int(settings, "buffer");
		if (data->BufferSize != BufferSize) {
			data->BufferSize = BufferSize;
		}
		new_settings.buffer_size = BufferSize;

		BitDepth = (audio_format)obs_data_get_int(settings, "bit depth");
		if (data->BitDepth != BitDepth) {
			data->BitDepth = BitDepth;
		}
		new_settings.bit_depth = BitDepth;

		data->info = &info;
		new_settings.info = &info;
		data->input_channels = info.inputs;
		new_settings.input_channels = info.inputs;
		data->output_channels = info.outputs;
		new_settings.output_channels = info.outputs;
		data->device_index = device_index;//get_device_index(device);
		new_settings.device_index = device_index;
		//move the data to the device appropriate list
		EnterCriticalSection(&source_list_mutex);
		data->update_settings(new_settings);
		if (device_changed) {
			std::list<asio_data*>::iterator src;
			src = std::find(source_list[prev_device_index].begin(), source_list[prev_device_index].end(), data);
			//splice into the new location
			source_list[device_index].splice(source_list[device_index].end(), source_list[prev_device_index], src);
			//source_list[prev_device_index].remove(data);
			//source_list[device_index].push_back(data);
		}
		else {
			source_list[device_index].push_back(data);
		}
		//data->route_map = data->_bin_map_unmuted(data->route);
		//data->silent_map = data->_bin_map_muted(data->route);
		//data->muted_chs = data->_get_muted_chs(data->route);
		//data->unmuted_chs = data->_get_unmuted_chs(data->route);
		LeaveCriticalSection(&source_list_mutex);

		asio_init(data);
	}

}

const char * asio_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("asioInput");
}

void asio_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "sample rate", 48000);
	obs_data_set_default_int(settings, "bit depth", AUDIO_FORMAT_FLOAT);
}

obs_properties_t * asio_get_properties(void *unused)
{
	obs_properties_t *props;
	obs_property_t *devices;
	obs_property_t *rate;
	obs_property_t *bit_depth;
	obs_property_t *buffer_size;
	obs_property_t *route[MAX_AUDIO_CHANNELS];
	obs_property_t *console;
	int pad_digits = (int)floor(log10(abs(MAX_AUDIO_CHANNELS))) + 1;

	UNUSED_PARAMETER(unused);

	props = obs_properties_create();
	devices = obs_properties_add_list(props, "device_id",
			obs_module_text("Device"), OBS_COMBO_TYPE_LIST,
			OBS_COMBO_FORMAT_STRING);
	obs_property_set_modified_callback(devices, asio_device_changed);
	fill_out_devices(devices);
	std::string dev_descr = "ASIO devices.\n"
			"OBS-Studio supports for now a single ASIO source.\n"
			"But duplication of an ASIO source in different scenes is still possible";
	obs_property_set_long_description(devices, dev_descr.c_str());
	// get channel number from output speaker layout set by obs
	DWORD recorded_channels = get_obs_output_channels();

	std::string route_descr = "For each OBS output channel, pick one\n of the input channels of your ASIO device.\n";
	const char* route_name_format = "route %i";
	char* route_name = new char[strlen(route_name_format) + pad_digits];

	const char* route_obs_format = "Route.%i";
	char* route_obs = new char[strlen(route_obs_format) + pad_digits];
	for (size_t i = 0; i < recorded_channels; i++) {
		sprintf(route_name, route_name_format, i);
		sprintf(route_obs, route_obs_format, i);
		route[i] = obs_properties_add_list(props, route_name, obs_module_text(route_obs),
			OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
		obs_property_set_long_description(route[i], route_descr.c_str());
	}

	free(route_name);
	free(route_obs);

	rate = obs_properties_add_list(props, "sample rate",
			obs_module_text("SampleRate"), OBS_COMBO_TYPE_LIST,
			OBS_COMBO_FORMAT_INT);
	std::string rate_descr = "Sample rate : number of samples per channel in one second.\n";
	obs_property_set_long_description(rate, rate_descr.c_str());
	
	bit_depth = obs_properties_add_list(props, "bit depth", TEXT_BITDEPTH,
			OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	std::string bit_descr = "Bit depth : size of a sample in bits and format.\n"
			"Float should be preferred.";
	obs_property_set_long_description(bit_depth, bit_descr.c_str());

	buffer_size = obs_properties_add_list(props, "buffer", TEXT_BUFFER_SIZE,
			OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(buffer_size, "64", 64);
	obs_property_list_add_int(buffer_size, "128", 128);
	obs_property_list_add_int(buffer_size, "256", 256);
	obs_property_list_add_int(buffer_size, "512", 512);
	obs_property_list_add_int(buffer_size, "1024", 1024);
	std::string buffer_descr = "Buffer : number of samples in a single frame.\n"
			"A lower value implies lower latency.\n"
			"256 should be OK for most cards.\n"
			"Warning: the real buffer returned by the device may differ";
	obs_property_set_long_description(buffer_size, buffer_descr.c_str());

	console = obs_properties_add_button(props, "console", 
			obs_module_text("ASIO driver control panel"), DeviceControlPanel);
	std::string console_descr = "Make sure your settings in the Driver Control Panel\n"
		"for sample rate and buffer are consistent with what you\n"
		"have set in OBS.";
	obs_property_set_long_description(console, console_descr.c_str());

	return props;
}

bool obs_module_load(void)
{
	struct obs_source_info asio_input_capture = {};
	asio_input_capture.id             = "asio_input_capture";
	asio_input_capture.type           = OBS_SOURCE_TYPE_INPUT;
	asio_input_capture.output_flags   = OBS_SOURCE_AUDIO;
	asio_input_capture.create         = asio_create;
	asio_input_capture.destroy        = asio_destroy;
	asio_input_capture.update         = asio_update;
	asio_input_capture.get_defaults   = asio_get_defaults;
	asio_input_capture.get_name       = asio_get_name;
	asio_input_capture.get_properties = asio_get_properties;

	InitializeCriticalSection(&source_list_mutex);

	uint8_t devices = getDeviceCount();
	//preallocate before we push vectors
	source_list.reserve(devices);
	for (uint8_t i = 0; i < devices; i++) {
		source_list.push_back(std::list<asio_data*>());
	}

	obs_register_source(&asio_input_capture);
	return true;
}