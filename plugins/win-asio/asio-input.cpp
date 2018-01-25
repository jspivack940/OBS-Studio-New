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
#include <unordered_map>
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


/* radix sort from: https://www.geeksforgeeks.org/radix-sort/*/
/* radix sort from: https://github.com/gorset/radix/blob/master/radix.cc*/
/* radix sort from: https://www.quora.com/What-is-the-most-efficient-way-to-sort-a-million-32-bit-integers*/
/* radix sort from: http://pseudoprogrammer.blogspot.com/2012/05/binary-radix-sort.html*/
/*
void radix_sort(unsigned *begin, unsigned *end)
{
	unsigned *begin1 = new unsigned[end - begin];
	unsigned *end1 = begin1 + (end - begin);
	for (unsigned shift = 0; shift < 32; shift += 8) {
		size_t count[0x100] = {};
		for (unsigned *p = begin; p != end; p++)
			count[(*p >> shift) & 0xFF]++;
		unsigned *bucket[0x100], *q = begin1;
		for (int i = 0; i < 0x100; q += count[i++])
			bucket[i] = q;
		for (unsigned *p = begin; p != end; p++)
			*bucket[(*p >> shift) & 0xFF]++ = *p;
		std::swap(begin, begin1);
		std::swap(end, end1);
	}
	delete[] begin1;
}
*/
void radix_sort(unsigned char* begin, unsigned char* end) {
	unsigned char *begin1 = new unsigned char[end - begin];
	unsigned char *end1 = begin1 + (end - begin);
	unsigned bit_depth = sizeof(unsigned char) * 8;
	//technically...this only runs once... (unsigned char is 8 bits and all)
	for (unsigned long shift = 0; shift < bit_depth; shift += 8) {
		size_t count[0x100] = {};
		for (unsigned char *p = begin; p != end; p++)
			count[(*p >> shift) & 0xFF]++;
		unsigned char *bucket[0x100];
		unsigned char *q = begin1;
		for (int i = 0; i < 0x100; q += count[i++])
			bucket[i] = q;
		for (unsigned char*p = begin; p != end; p++)
			*bucket[(*p >> shift) & 0xFF]++ = *p;
		std::swap(begin, begin1);
		std::swap(end, end1);
	}
}

void radix_sort(unsigned short *begin, unsigned short *end) {
	unsigned short *begin1 = new unsigned short[end - begin];
	unsigned short *end1 = begin1 + (end - begin);
	unsigned bit_depth = sizeof(unsigned short) * 8;
	for (unsigned long shift = 0; shift < bit_depth; shift += 8) {
		size_t count[0x100] = {};
		for (unsigned short *p = begin; p != end; p++)
			count[(*p >> shift) & 0xFF]++;
		unsigned short *bucket[0x100];
		unsigned short *q = begin1;
		for (int i = 0; i < 0x100; q += count[i++])
			bucket[i] = q;
		for (unsigned short*p = begin; p != end; p++)
			*bucket[(*p >> shift) & 0xFF]++ = *p;
		std::swap(begin, begin1);
		std::swap(end, end1);
	}
}

void radix_sort(unsigned long *begin, unsigned long *end) {
	unsigned long *begin1 = new unsigned long[end - begin];
	unsigned long *end1 = begin1 + (end - begin);
	unsigned bit_depth = sizeof(unsigned long) * 8;
	for (unsigned long shift = 0; shift < bit_depth; shift += 8) {
		size_t count[0x100] = {};
		for (unsigned long *p = begin; p != end; p++)
			count[(*p >> shift) & 0xFF]++;
		unsigned long *bucket[0x100];
		unsigned long *q = begin1;
		for (int i = 0; i < 0x100; q += count[i++])
			bucket[i] = q;
		for (unsigned long*p = begin; p != end; p++)
			*bucket[(*p >> shift) & 0xFF]++ = *p;
		std::swap(begin, begin1);
		std::swap(end, end1);
	}
	delete[] begin1;
}

void radix_sort(unsigned long long *begin, unsigned long long *end) {
	unsigned long long *begin1 = new unsigned long long[end - begin];
	unsigned long long *end1 = begin1 + (end - begin);
	unsigned bit_depth = sizeof(unsigned long long) * 8;
	for (unsigned shift = 0; shift < bit_depth; shift += 8) {
		size_t count[0x100] = {};
		for (unsigned long long *p = begin; p != end; p++)
			count[(*p >> shift) & 0xFF]++;
		unsigned long long *bucket[0x100], *q = begin1;
		for (int i = 0; i < 0x100; q += count[i++])
			bucket[i] = q;
		for (unsigned long long*p = begin; p != end; p++)
			*bucket[(*p >> shift) & 0xFF]++ = *p;
		std::swap(begin, begin1);
		std::swap(end, end1);
	}
	delete[] begin1;
}


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

/* ======================================================================= */
/* asio structs and classes */

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
struct device_source_audio {
	uint8_t				**data;
	uint32_t			frames;
	long				input_chs;
	enum audio_format	format;
	uint32_t			samples_per_sec;
	uint64_t			timestamp;
};

class device_data;
class asio_data;

struct listener_pair {
	asio_data *asio_listener;
	device_data *device;
};

class asio_data {
public:
	obs_source_t *source;

	/*asio device and info */
	const char *device;
	uint8_t device_index;

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
	std::vector<long> required_signals;

	//signals
	WinHandle stopSignal;
	WinHandle receiveSignal;

	WinHandle reconnectThread;
	WinHandle captureThread;

	bool isASIOActive = false;
	bool reconnecting = false;
	bool previouslyFailed = false;
	bool useDeviceTiming = false;

	asio_data() : source(NULL), first_ts(0), device_index(-1) {
		memset(&route[0], -1, sizeof(DWORD) * 8);

		stopSignal = CreateEvent(nullptr, true, false, nullptr);
		receiveSignal = CreateEvent(nullptr, false, false, nullptr);

		//captureThread = CreateThread(nullptr, 0, asio_data::capture_thread, this, 0, nullptr);
	}

	bool render_audio(device_source_audio *asio_buffer, long route[]) {
		static uint8_t* silent_buffer = NULL;
		static size_t silent_buffer_size = 0;

		struct obs_audio_info aoi;
		obs_get_audio_info(&aoi);

		obs_source_audio out;
		out.format = asio_buffer->format;
		if (out.format == AUDIO_FORMAT_UNKNOWN) {
			return false;
		}

		out.frames = asio_buffer->frames;
		out.samples_per_sec = asio_buffer->samples_per_sec;
		out.timestamp = asio_buffer->timestamp;
		if (!first_ts) {
			first_ts = out.timestamp;
			return false;
		}
		//cache a silent buffer
		size_t buffer_size = (out.frames * sizeof(bytedepth_format(out.format)));
		if (silent_buffer_size < buffer_size) {
			if (silent_buffer) {
				free(silent_buffer);
			}
			silent_buffer = (uint8_t*)calloc(buffer_size, sizeof(uint8_t));
			silent_buffer_size = buffer_size;
		}

		for (short i = 0; i < aoi.speakers; i++) {
			if (route[i] >= 0 && route[i] < aoi.speakers) {
				out.data[i] = asio_buffer->data[route[i]];
			}
			else if (route[i] == -1) {
				out.data[i] = silent_buffer;
			}
		}

		out.speakers = aoi.speakers;

		obs_source_output_audio(source, &out);

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
				bins[j].push_back(index);
			}
		}

		return bins;
	}

	static std::vector<short> _get_muted_chs(long route_array[]) {
		std::vector<short> silent_chs;
		silent_chs.reserve(MAX_AUDIO_CHANNELS);
		for (short i = 0; i < MAX_AUDIO_CHANNELS; i++) {
			if (route_array[i] == -1) {
				silent_chs.push_back(i);
			}
		}
		return silent_chs;
	}

	static std::vector<short> _get_unmuted_chs(long route_array[]) {
		std::vector<short> unmuted_chs;
		unmuted_chs.reserve(MAX_AUDIO_CHANNELS);
		for (short i = 0; i < MAX_AUDIO_CHANNELS; i++) {
			if (route_array[i] >= 0) {
				unmuted_chs.push_back(i);
			}
		}
		return unmuted_chs;
	}

	static std::vector<long> _get_required_chs(long route_array[]) {
		std::unordered_map<long, short> hash_map;
		hash_map.reserve(MAX_AUDIO_CHANNELS);
		for (short i = 0; i < MAX_AUDIO_CHANNELS; i++) {
			if (route_array[i] >= 0) {
				hash_map[route_array[i]] = i;//.push_back(route_array[i]);
			}
		}
		//radix_sort((unsigned long*)tmpArray, (unsigned long*)tmpArray[MAX_AUDIO_CHANNELS]);
		//std::vector<long> ordered_chs (tmpArray,tmpArray+MAX_AUDIO_CHANNELS);
		std::vector<long> selected_chs;
		selected_chs.reserve(hash_map.size());
		std::unordered_map<long, short>::iterator it;
		for (it = hash_map.begin(); it != hash_map.end(); it++) {
			//(key,value) (it->first, it->second)
			selected_chs.push_back(it->first);
		}

		return selected_chs;
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

class device_data {
private:
	size_t write_index;
	size_t read_index;
	size_t buffer_count;

	circlebuf audio_buffer;

	uint32_t frames;
	long input_chs;
	audio_format format;
	uint32_t samples_per_sec;

	size_t buffer_size;
	//uint64_t first_timestamp;

	WinHandle *receive_signals;
	WinHandle all_recieved_signal;

	bool all_prepped = false;
	bool buffer_prepped = false;
	bool circle_buffer_prepped = false;
	bool reallocate_buffer = false;
	bool events_prepped = false;
public:
	long device_index;

	device_source_audio* get_writeable_source_audio() {
		return (device_source_audio*)circlebuf_data(&audio_buffer, write_index * sizeof(device_source_audio));
	}

	device_source_audio* get_source_audio(size_t index) {
		return (device_source_audio*)circlebuf_data(&audio_buffer, index * sizeof(device_source_audio));
	}

	device_data() {
		all_prepped = false;
		buffer_prepped = false;
		circle_buffer_prepped = false;
		reallocate_buffer = false;
		events_prepped = false;
		format = AUDIO_FORMAT_UNKNOWN;
		write_index = 0;
		read_index = 0;
		buffer_count = 32;
	}

	device_data(size_t buffers, audio_format audioformat) {
		all_prepped = false;
		buffer_prepped = false;
		circle_buffer_prepped = false;
		reallocate_buffer = false;
		events_prepped = false;

		format = audioformat;
		write_index = 0;
		read_index = 0;
		buffer_count = buffers ? buffers : 32;
	}

	void check_all() {
		if (buffer_prepped && circle_buffer_prepped && events_prepped) {
			all_prepped = true;
		}
		else {
			all_prepped = false;
		}
	}

	void prep_circle_buffer(BASS_ASIO_INFO &info) {
		prep_circle_buffer(info.bufpref);
	}

	void prep_circle_buffer(DWORD bufpref) {
		if (!circle_buffer_prepped) {
			//info.bufpref
			//BASS_ASIO_ChannelGetFormat
			buffer_count = max(4, ceil(2048 / bufpref));
			circlebuf_init(&audio_buffer);
			circlebuf_reserve(&audio_buffer, buffer_count * sizeof(device_source_audio));
			for (int i = 0; i < buffer_count; i++) {
				circlebuf_push_back(&audio_buffer, &device_source_audio(), sizeof(device_source_audio));
				//initialize # of buffers
			}
			circle_buffer_prepped = true;
		}
	}

	void prep_events(BASS_ASIO_INFO &info) {
		prep_events(info.inputs);
	}

	void prep_events(long input_chs) {
		if (!events_prepped) {
			receive_signals = (WinHandle*)calloc(input_chs, sizeof(WinHandle));
			for (int i = 0; i < input_chs; i++) {
				receive_signals[i] = CreateEvent(nullptr, true, false, nullptr);
			}
			events_prepped = true;
		}
	}

	void prep_buffers(BASS_ASIO_INFO &info, audio_format in_format, uint32_t in_samples_per_sec) {
		prep_buffers(info.bufpref, info.inputs, in_format, in_samples_per_sec);
	}

	void prep_buffers(uint32_t frames, long in_chs, audio_format format, uint32_t samples_per_sec) {
		if (frames * bytedepth_format(format) > this->buffer_size) {
			if (buffer_prepped) {
				reallocate_buffer = true;
			}
		}
		else {
			reallocate_buffer = false;
		}
		prep_events(in_chs);
		if (circle_buffer_prepped && (!buffer_prepped || reallocate_buffer)) {
			this->frames = frames;
			this->input_chs = in_chs;
			this->format = format;
			this->samples_per_sec = samples_per_sec;
			this->buffer_size = frames * bytedepth_format(format);

			for (int i = 0; i < buffer_count; i++) {
				device_source_audio* _source_audio = get_source_audio(i);
				_source_audio->data = (uint8_t **)calloc(input_chs, sizeof(uint8_t*));
				for (int j = 0; j < input_chs; j++) {
					if (!buffer_prepped) {
						_source_audio->data[j] = (uint8_t*)calloc(buffer_size, 1);
					}
					else if (reallocate_buffer) {
						uint8_t* tmp = (uint8_t*)realloc(_source_audio->data[j], buffer_size);
						if (tmp == NULL) {
							buffer_prepped = false;
							all_prepped = false;
							return;
						}
						else if (tmp == _source_audio->data[j]) {
							tmp = NULL;
						}
						else {
							_source_audio->data[j] = tmp;
							tmp = NULL;
						}
					}
				}
				_source_audio->input_chs = input_chs;
				_source_audio->frames = frames;
				_source_audio->format = format;
				_source_audio->samples_per_sec = samples_per_sec;
			}
			buffer_prepped = true;
		}
		check_all();
	}

	void write_buffer(DWORD ch, uint8_t* buffer, size_t buffer_size) {
		static volatile long callback_count = 0;
		static BASS_ASIO_INFO info;
		BASS_ASIO_GetInfo(&info);
		if (!all_prepped) {
			/* this should've happened in advance of the device callbacks... */
			//init the device for proper use from the callback (we should have enough info)
			/*
			if (!circle_buffer_prepped) {
				prep_circle_buffer(info);
			}
			if (!events_prepped) {
				prep_events(info);
			}
			if (!buffer_prepped) {
				BASS_ASIO_CHANNELINFO ch_info;
				BASS_ASIO_ChannelGetInfo(1, 0, &ch_info);

				double sample_rate = BASS_ASIO_ChannelGetRate(1, 0);

				prep_buffers(info, asio_to_obs_audio_format(ch_info.format), (DWORD)sample_rate);
			}
			*/
			return;
		}
		if (os_atomic_load_long(&callback_count) == 0) {
			for (int i = 0; i < input_chs; i++) {
				ResetEvent(receive_signals[ch]);
			}
			//apply settings
		}

		device_source_audio* _source_audio = get_writeable_source_audio();
		/*
		size_t source_audio_buffer_size = _source_audio->frames * bytedepth_format(_source_audio->format);
		if(buffer_size > source_audio_buffer_size){
		uint8_t *tmp = (uint8_t*)realloc(_source_audio->data[ch], buffer_size);
		if(tmp == NULL){
		//problem
		_source_audio->data[ch] = NULL;
		} else if(tmp == _source_audio->data[ch]){
		tmp = NULL;
		} else {
		_source_audio->data[ch] = tmp;
		tmp = NULL;
		}
		}
		*/
		if (_source_audio && _source_audio->data[ch]) {
			memcpy(_source_audio->data[ch], buffer, buffer_size);
			SetEvent(receive_signals[ch]);
			//blog(LOG_INFO, "Ch %lu/%lu: %s, %lu samples", ch, info.inputs, info.name, info.bufpref);
		}

		//loop
		os_atomic_inc_long(&callback_count);
		//os_atomic_set_long(&callback_count, (os_atomic_load_long(&callback_count)+1) % input_chs);
		if (os_atomic_load_long(&callback_count) >= input_chs) {
			_source_audio->timestamp = os_gettime_ns() - ((_source_audio->frames * NSEC_PER_SEC) / _source_audio->samples_per_sec);
			write_index++;
			write_index = write_index % buffer_count;
			//os_atomic_set_long(&callback_count, (os_atomic_load_long(&callback_count)) % input_chs);
			/*
			for (int i = 0; i < input_chs; i++) {
			ResetEvent(receive_signals[ch]);
			}
			*/
			//os_atomic_set_long(&callback_count, 0);
		}
	}

	static DWORD WINAPI capture_thread(void *data) {
		listener_pair *pair = static_cast<listener_pair*>(data);
		asio_data *source = pair->asio_listener;//static_cast<asio_data*>(data);
		device_data *device = pair->device;//static_cast<device_data*>(data);
		struct obs_audio_info aoi;
		obs_get_audio_info(&aoi);

		std::string thread_name = "asio capture thread";//source->device;
														//thread_name += " capture thread";
		os_set_thread_name(thread_name.c_str());
		//size_t ch_count = source->unmuted_chs.size();
		//HANDLE *signals = (HANDLE*)calloc(ch_count, sizeof(HANDLE));
		HANDLE signals[MAX_AUDIO_CHANNELS];
		long route[MAX_AUDIO_CHANNELS];

		size_t read_index = 0;
		source->isASIOActive = true;

		blog(LOG_INFO, "listener for device %lu created", device->device_index );

		while (true) {
			//set the chs we need to wait on
			for (short i = 0; i < source->required_signals.size(); i++) { //source->unmuted_chs.size()
				//signals[i] = device->receive_signals[source->route[source->unmuted_chs[i]]];
				signals[i] = device->receive_signals[source->required_signals[i]];
			}
			for (short i = 0; i < aoi.speakers; i++) {
				route[i] = source->route[i];
			}
			int waitResult = WaitForMultipleObjects((DWORD)source->required_signals.size(), signals, true, 1000);
			if (waitResult == WAIT_OBJECT_0) {
				//device->read_index()
				while (read_index != device->write_index) {
					device_source_audio* in = device->get_source_audio(read_index);//device->get_writeable_source_audio();
					source->render_audio(in, route);
					read_index++;
					read_index = read_index % device->buffer_count;
				}
			}
			else if (waitResult == WAIT_ABANDONED_0) {
				blog(LOG_INFO, "a mutex for listener thread %lu was abandoned", device->device_index);
				return 0;
			}
			else if (waitResult == WAIT_TIMEOUT) {
				blog(LOG_INFO, "listener thread for %lu timed out", device->device_index);
				return 0;
			}
			else if (waitResult == WAIT_FAILED) {
				blog(LOG_INFO, "listener thread wait %lu failed with 0x%x", device->device_index, GetLastError());
			}
			else {
				blog(LOG_INFO, "waitResult = %i", waitResult);
			}
			if (source->device_index != device->device_index) {
				blog(LOG_INFO, "source device index %lu is not device index %lu", source->device_index, device->device_index);
				break;
			}
			if (!source->isASIOActive) {
				blog(LOG_INFO, "source for %lu indicated it wanted to disconnect", source->isASIOActive );
				return 0;
			}
			/*
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
			*/
		}

		return 0;
	}

	void add_listener(asio_data *listener) {
		if (!events_prepped) {
			return;
		}
		listener_pair* parameters = new listener_pair();

		parameters->asio_listener = listener;
		parameters->device = this;

		blog(LOG_INFO, "adding listener for %lu (source: %lu)", device_index, listener->device_index);
		listener->isASIOActive = false;
		//wait on the ch to return, signaling the previous listener should've disconnected by now;
		WaitForSingleObject(this->receive_signals[0], 200);

		listener->captureThread = CreateThread(nullptr, 0, this->capture_thread, parameters, 0, nullptr);
	}
};

std::vector<device_data*> device_list;

CRITICAL_SECTION source_list_mutex;
std::vector<std::list<asio_data*>> source_list;

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

DWORD CALLBACK create_asio_buffer(BOOL input, DWORD channel, void *buffer, DWORD BufSize, void *device_ptr) {
	//DWORD BASS_ASIO_GetDevice()
	BASS_ASIO_INFO info;
	BASS_ASIO_GetInfo(&info);

	device_data *device = (device_data*)device_ptr;

	device->write_buffer(channel, (uint8_t*)buffer, BufSize);

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

	uint8_t deviceNumber = getDeviceCount();
	if (deviceNumber < 1) {
		blog(LOG_INFO, "\nNo audio devices found!\n");
		return;
	}

	// get channel number from output speaker layout set by obs
	DWORD recorded_channels = info.inputs;//get_obs_output_channels();

	// check buffer size is legit; if not set it to bufpref
	// to be implemented : to avoid issues, force to bufpref
	// this ignores any setting; bufpref is most likely set in asio control panel
	//data->BufferSize = info.bufpref;
	//check channel setup
	DWORD checkrate = BASS_ASIO_GetRate();
	blog(LOG_INFO, "sample rate is set in device to %i.\n", checkrate);
	DWORD checkbitdepth = BASS_ASIO_ChannelGetFormat(true, 0);
	blog(LOG_INFO, "bitdepth is set in device to %i.\n", checkbitdepth);
	//audio_format format = asio_to_obs_audio_format(checkbitdepth);

	//get the device_index
	DWORD device_index = get_device_index(info.name);

	//start asio device if it hasn't already been
	if (!BASS_ASIO_IsStarted()) {
		DWORD obs_optimal_format = BASS_ASIO_FORMAT_FLOAT;
		DWORD asio_native_format = BASS_ASIO_ChannelGetFormat(true, 0);
		DWORD selected_format;
		if (obs_optimal_format == asio_native_format) {
			//all good...I don't think this needs to happen
			for (DWORD i = 0; i < info.inputs; i++) {
				ret = BASS_ASIO_ChannelSetFormat(true, i, asio_native_format);
				if (!ret) {
					blog(LOG_ERROR, "ASIO: unable to use native format\n"
						"error number: %i \n; check BASS_ASIO_ErrorGetCode\n",
						BASS_ASIO_ErrorGetCode());
					return;
				}
			}
			selected_format = asio_native_format;
		}
		else {
			for (DWORD i = 0; i < info.inputs; i++) {
				ret = BASS_ASIO_ChannelSetFormat(true, i, obs_optimal_format);
				if (!ret) {
					blog(LOG_ERROR, "ASIO: unable to use optimal format (float)\n"
						"error number: %i \n; check BASS_ASIO_ErrorGetCode\n",
						BASS_ASIO_ErrorGetCode());
					break;
				}
			}
			if (!ret) {
				for (DWORD i = 0; i < info.inputs; i++) {
					ret = BASS_ASIO_ChannelSetFormat(true, i, asio_native_format);
					if (!ret) {
						blog(LOG_ERROR, "ASIO: unable to use native format\n"
							"error number: %i \n; check BASS_ASIO_ErrorGetCode\n",
							BASS_ASIO_ErrorGetCode());
						return;
					}
				}
				selected_format = asio_native_format;
			}
			else {
				selected_format = obs_optimal_format;
			}
		}

		blog(LOG_INFO, "(best) bitdepth supported %i", selected_format);
		audio_format format = asio_to_obs_audio_format(selected_format);

		// enable all chs and link to callback w/ the device buffer class
		for (DWORD i = 0; i < info.inputs; i++) {
			ret = BASS_ASIO_ChannelEnable(true, i, &create_asio_buffer, device_list[device_index]);//data
			if (!ret)
			{
				blog(LOG_ERROR, "Could not enable channel %i; error code : %i", i, BASS_ASIO_ErrorGetCode());
			}
		}

		/*prep the device buffers*/
		blog(LOG_INFO, "prepping device %lu", device_index);
		device_list[device_index]->prep_circle_buffer(info);
		device_list[device_index]->prep_events(info);
		device_list[device_index]->prep_buffers(info.bufpref, info.inputs, format, checkrate);

		blog(LOG_INFO, "starting device %lu", device_index);
		BASS_ASIO_Start(info.bufpref, recorded_channels);
		switch (BASS_ASIO_ErrorGetCode()) {
		case BASS_ERROR_INIT:
			blog(LOG_ERROR, "Error: Bass asio not initialized.\n");
		case BASS_ERROR_ALREADY:
			blog(LOG_ERROR, "Error: device already started\n");
			//BASS_ASIO_Stop(); 
			//BASS_ASIO_Start(data->BufferSize, recorded_channels);
		case BASS_ERROR_NOCHAN:
			blog(LOG_ERROR, "Error: channels have not been enabled so can not start\n");
		case BASS_ERROR_UNKNOWN:
		default:
			blog(LOG_ERROR, "ASIO init: Unknown error when trying to start the device\n");
		}
	}

	//Connect listener thread
	//data->captureThread =  device_list[device_index]->capture_thread();
	blog(LOG_INFO, "starting listener thread for: %lu", device_index);
	device_list[device_index]->add_listener(data);
}

static void * asio_create(obs_data_t *settings, obs_source_t *source)
{
	asio_data *data = new asio_data;

	data->source = source;
	data->first_ts = 0;
	data->device = NULL;

	asio_update(data, settings);

	//if (obs_data_get_string(settings, "device_id")) {
	//	asio_init(data);
	//}

	return data;
}

void asio_destroy(void *vptr)
{
	struct asio_data *data = (asio_data *)vptr;

	/*
	if (BASS_ASIO_IsStarted())
	BASS_ASIO_Stop();

	BASS_ASIO_Free();
	*/

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

	// get channel number from output speaker layout set by obs
	DWORD recorded_channels = get_obs_output_channels();
	data->recorded_channels = recorded_channels;

	// get device from settings
	device = obs_data_get_string(settings, "device_id");

	if (device == NULL || device[0] == '\0') {
		blog(LOG_INFO, "Device not yet set \n");
	}
	else if (data->device == NULL || data->device[0] == '\0') {
		data->device = bstrdup(device);
	}
	else {
		if (strcmp(device, data->device) != 0) {
			prev_device = bstrdup(data->device);
			data->device = bstrdup(device);
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
		bool first_initialization = false;

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
			first_initialization = true;
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
			if (data->route[i] != route[i]) {
				data->route[i] = route[i];
				route_changed = true;
			}
		}

		data->input_channels = info.inputs;
		data->output_channels = info.outputs;
		data->device_index = device_index;//get_device_index(device);
										  //move the data to the device appropriate list

										  //EnterCriticalSection(&source_list_mutex);
										  /*
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
										  */
		data->route_map = data->_bin_map_unmuted(data->route);
		data->silent_map = data->_bin_map_muted(data->route);
		data->muted_chs = data->_get_muted_chs(data->route);
		data->unmuted_chs = data->_get_unmuted_chs(data->route);
		data->required_signals = data->_get_required_chs(data->route);
		//LeaveCriticalSection(&source_list_mutex);

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
	asio_input_capture.id = "asio_input_capture";
	asio_input_capture.type = OBS_SOURCE_TYPE_INPUT;
	asio_input_capture.output_flags = OBS_SOURCE_AUDIO;
	asio_input_capture.create = asio_create;
	asio_input_capture.destroy = asio_destroy;
	asio_input_capture.update = asio_update;
	asio_input_capture.get_defaults = asio_get_defaults;
	asio_input_capture.get_name = asio_get_name;
	asio_input_capture.get_properties = asio_get_properties;

	InitializeCriticalSection(&source_list_mutex);

	unsigned long testarray[MAX_AUDIO_CHANNELS] = { 1,2,3,4,5,6,7,8 };
	radix_sort(&testarray[0], &testarray[MAX_AUDIO_CHANNELS]);

	unsigned long testarray2[MAX_AUDIO_CHANNELS] = { 1,2,3,4,2,2982746,7,8 };
	radix_sort(&testarray2[0], &testarray2[MAX_AUDIO_CHANNELS]);

	unsigned long long testarray3[MAX_AUDIO_CHANNELS] = { 0xFFFFFFFFFFFFFFFF, 0, 0xAAAAAAAABBBBBBBB, 0xBBBBBBBBAAAAAAAA, 1, 9, 0x77777777, 0x66666666 };
	radix_sort(&testarray3[0], &testarray3[MAX_AUDIO_CHANNELS]);

	uint8_t devices = getDeviceCount();
	//preallocate before we push vectors
	//source_list.reserve(devices);
	device_list.reserve(devices);
	for (uint8_t i = 0; i < devices; i++) {
		//source_list.push_back(std::list<asio_data*>());

		device_data *device = new device_data();
		device->device_index = i;
		device_list.push_back(device);
		//BASS_ASIO_INFO info;
		//BASS_ASIO_DEVICEINFO info2;
		//BASS_ASIO_GetDeviceInfo(i,&info);
	}

	obs_register_source(&asio_input_capture);
	return true;
}