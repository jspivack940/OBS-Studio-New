#include <util/bmem.h>
#include <util/platform.h>
#include <util/threading.h>
#include <util/circlebuf.h>
#include <sstream>
#include <string>
#include <algorithm>
#include <windows.h>
#include <util/windows/WinHandle.hpp>

#define CAPTURE_INTERVAL INFINITE
#define NSEC_PER_SEC 1000000000LL

extern int               bytedepth_format(audio_format format);
extern enum audio_format get_planar_format(audio_format format);

struct device_buffer_options {
	uint32_t    buffer_size;
	uint32_t    channel_count;
	const char* name;
};

struct device_source_audio {
	uint8_t**         data;
	uint32_t          frames;
	long              input_chs;
	enum audio_format format;
	uint32_t          samples_per_sec;
	uint64_t          timestamp;
};

class device_buffer;
class asio_listener;

struct listener_pair {
	asio_listener* asio_listener;
	device_buffer* device;
};

static void wait_for_buffer(device_buffer* buffer);

class asio_listener {
private:
	uint8_t* silent_buffer      = NULL;
	size_t   silent_buffer_size = 0;
	void*    user_data;

public:
	CRITICAL_SECTION settings_mutex;

	obs_source_t*  source;
	device_buffer* buffer = nullptr;

	/*asio device and info */
	char*          device_name;
	uint64_t       device_index;
	uint64_t       first_ts;
	speaker_layout layout;

	/* channels info */
	DWORD input_channels;            // total number of input channels
	DWORD output_channels;           // number of output channels of device (not used)
	DWORD recorded_channels;         // number of channels passed from device (including muted) to OBS; is at most 8
	long  route[MAX_AUDIO_CHANNELS]; // stores the channel re-ordering info

	std::vector<short> unmuted_chs;
	std::vector<short> muted_chs;
	std::vector<short> tmp_muted_chs;
	std::vector<long>  required_signals;

	// signals
	WinHandle stop_listening_signal;

	bool isASIOActive     = false;
	bool reconnecting     = false;
	bool previouslyFailed = false;
	bool useDeviceTiming  = false;

	void* get_user_data()
	{
		return user_data;
	}

	void set_user_data(void* data)
	{
		user_data = data;
	}

	std::string get_id()
	{
		const void*       address = static_cast<const void*>(source);
		std::stringstream ss;
		ss << "0x" << std::hex << (uint64_t)address;
		std::string name = ss.str();
		return name;
	};

	asio_listener() : source(NULL), first_ts(0ULL), device_index(0ULL)
	{
		InitializeCriticalSection(&settings_mutex);

		memset(&route[0], -1, sizeof(long) * 8);

		stop_listening_signal = CreateEvent(nullptr, true, false, nullptr);
	}

	~asio_listener()
	{
		DeleteCriticalSection(&settings_mutex);
		if (silent_buffer)
			bfree(silent_buffer);
	}

	bool disconnect()
	{
		device_index = 0ULL;
		isASIOActive = false;
		// wait for 2-3 buffers to pass
		// first buffer may already be processing
		wait_for_buffer(buffer);
		// now we know device_index mismatch is processing
		wait_for_buffer(buffer);
		// now we know another whole buffer has processed
		// (listener disconnected)
		wait_for_buffer(buffer);
		buffer = nullptr;
		return true;
	}

	/* main method passing audio from listener to obs */
	bool render_audio(device_source_audio* asio_buffer, long route[])
	{
		obs_source_audio out;
		out.format = asio_buffer->format;
		if (!is_audio_planar(out.format))
			return false;
		if (out.format == AUDIO_FORMAT_UNKNOWN)
			return false;

		out.frames          = asio_buffer->frames;
		out.samples_per_sec = asio_buffer->samples_per_sec;
		out.timestamp       = asio_buffer->timestamp;
		if (!first_ts) {
			first_ts = out.timestamp;
			return false;
		}
		// cache a silent buffer
		size_t buffer_size = (out.frames * sizeof(bytedepth_format(out.format)));
		if (silent_buffer_size < buffer_size) {
			if (silent_buffer)
				bfree(silent_buffer);
			silent_buffer      = (uint8_t*)bzalloc(buffer_size);
			silent_buffer_size = buffer_size;
		}
			
		int  channels = (int)get_audio_channels(layout);
		bool muted    = true;
		for (int i = 0; i < channels; i++) {
			if (route[i] >= 0 && route[i] < asio_buffer->input_chs) {
				out.data[i] = asio_buffer->data[route[i]];
				muted       = false;
			} else {
				out.data[i] = silent_buffer;
			}
		}	
		if (muted)
			return false;
	
		out.speakers = layout;

		obs_source_output_audio(source, &out);
		return true;
	}

	static std::vector<short> _get_muted_chs(long route_array[])
	{
		std::vector<short> silent_chs;
		silent_chs.reserve(MAX_AUDIO_CHANNELS);
		for (short i = 0; i < MAX_AUDIO_CHANNELS; i++) {
			if (route_array[i] == -1)
				silent_chs.push_back(i);
		}
		return silent_chs;
	}

	static std::vector<short> _get_unmuted_chs(long route_array[])
	{
		std::vector<short> unmuted_chs;
		unmuted_chs.reserve(MAX_AUDIO_CHANNELS);
		for (short i = 0; i < MAX_AUDIO_CHANNELS; i++) {
			if (route_array[i] >= 0)
				unmuted_chs.push_back(i);
		}
		return unmuted_chs;
	}
};

class device_buffer {
private:
	size_t write_index;
	size_t buffer_count;

	size_t   buffer_size;
	uint32_t frames;
	// long input_chs;
	uint32_t input_chs;
	// not in use
	uint32_t     output_chs;
	audio_format format;
	// not in use...
	WinHandle* receive_signals;
	// create a square tick signal w/ two events
	WinHandle all_recieved_signal;
	// to close out the device
	WinHandle stop_listening_signal;

	bool all_prepped           = false;
	bool buffer_prepped        = false;
	bool circle_buffer_prepped = false;
	bool reallocate_buffer     = false;
	bool events_prepped        = false;
	bool isBufferActive        = false;

	circlebuf audio_buffer;

	void* user_data = NULL;

	uint32_t                    listener_count;
	std::vector<asio_listener*> _listeners;
	WinHandle                   captureThread;

public:
	uint32_t samples_per_sec;

	audio_format get_format()
	{
		return format;
	}

	uint32_t get_listener_count()
	{
		return listener_count;
	}

	void* get_user_data()
	{
		return user_data;
	}

	void set_user_data(void* data)
	{
		user_data = data;
	}

	const WinHandle* get_handles()
	{
		return receive_signals;
	}

	WinHandle on_buffer()
	{
		return all_recieved_signal;
	}

	long get_input_channels()
	{
		return input_chs;
	}

	uint64_t              device_index;
	device_buffer_options device_options;

	device_source_audio* get_writeable_source_audio()
	{
		return (device_source_audio*)circlebuf_data(&audio_buffer, write_index * sizeof(device_source_audio));
	}

	device_source_audio* get_source_audio(size_t index)
	{
		return (device_source_audio*)circlebuf_data(&audio_buffer, index * sizeof(device_source_audio));
	}

	device_buffer()
	{
		listener_count        = 0;
		all_prepped           = false;
		buffer_prepped        = false;
		circle_buffer_prepped = false;
		reallocate_buffer     = false;
		events_prepped        = false;

		format       = AUDIO_FORMAT_UNKNOWN;
		write_index  = 0;
		buffer_count = 32;

		all_recieved_signal   = CreateEvent(nullptr, true, false, nullptr);
		stop_listening_signal = CreateEvent(nullptr, true, false, nullptr);
	}

	device_buffer(size_t buffers, audio_format audioformat)
	{
		listener_count        = 0;
		all_prepped           = false;
		buffer_prepped        = false;
		circle_buffer_prepped = false;
		reallocate_buffer     = false;
		events_prepped        = false;

		format       = audioformat;
		write_index  = 0;
		buffer_count = buffers ? buffers : 32;

		all_recieved_signal   = CreateEvent(nullptr, true, false, nullptr);
		stop_listening_signal = CreateEvent(nullptr, true, false, nullptr);
	}

	void set_audio_format(audio_format audioformat)
	{
		all_prepped    = false;
		buffer_prepped = false;
		format         = audioformat;
	}

	void disconnect()
	{
		isBufferActive = false;
		SetEvent(stop_listening_signal);
		if (captureThread.Valid())
			WaitForSingleObject(captureThread, INFINITE);
		ResetEvent(stop_listening_signal);
	}

	~device_buffer()
	{
		disconnect();
		// free resources?
		if (events_prepped)
			delete receive_signals;
		if (circle_buffer_prepped) {
			for (int i = 0; i < buffer_count; i++) {
				device_source_audio* _source_audio = get_source_audio(i);
				int                  input_chs     = _source_audio->input_chs;
				for (int j = 0; j < input_chs; j++) {
					if (_source_audio->data[j]) {
						bfree(_source_audio->data[j]);
					}
				}
				bfree(_source_audio->data);
			}
			circlebuf_free(&audio_buffer);
		}
	}

	// check that all the required device settings have been set
	void check_all()
	{
		all_prepped = buffer_prepped && circle_buffer_prepped && events_prepped;
	}

	bool device_buffer_preppared()
	{
		return all_prepped;
	}

	void prep_circle_buffer(device_buffer_options& options)
	{
		prep_circle_buffer(options.buffer_size);
	}

	void prep_circle_buffer(DWORD bufpref)
	{
		if (!circle_buffer_prepped) {
			// create a buffer w/ a minimum of 4 slots and a target of a fraction of 2048 samples
			buffer_count               = (size_t)max(4, ceil(2048 / bufpref));
			device_options.buffer_size = bufpref;
			circlebuf_init(&audio_buffer);
			circlebuf_reserve(&audio_buffer, buffer_count * sizeof(device_source_audio));
			device_source_audio temp = {0};
			// initialize # of buffers
			for (int i = 0; i < buffer_count; i++)
				circlebuf_push_back(&audio_buffer, &temp, sizeof(device_source_audio));
			circle_buffer_prepped = true;
		}
	}

	void prep_events(device_buffer_options& options)
	{
		prep_events(options.channel_count);
	}

	void prep_events(long input_chs)
	{
		if (!events_prepped) {
			device_options.channel_count = input_chs;
			receive_signals              = (WinHandle*)calloc(input_chs, sizeof(WinHandle));
			for (int i = 0; i < input_chs; i++)
				receive_signals[i] = CreateEvent(nullptr, true, false, nullptr);
			events_prepped = true;
		}
	}

	void re_prep_buffers()
	{
		all_prepped    = false;
		buffer_prepped = false;
		prep_buffers(device_options, format, samples_per_sec);
	}

	void re_prep_buffers(device_buffer_options& options)
	{
		all_prepped = false;
		prep_buffers(options, format, samples_per_sec);
	}

	void update_sample_rate(uint32_t in_samples_per_sec)
	{
		all_prepped           = false;
		this->samples_per_sec = in_samples_per_sec;
		check_all();
	}

	void prep_buffers(device_buffer_options& options, audio_format in_format, uint32_t in_samples_per_sec)
	{
		prep_buffers(options.buffer_size, options.channel_count, in_format, in_samples_per_sec);
	}

	void prep_buffers(uint32_t frames, uint32_t in_chs, audio_format format, uint32_t samples_per_sec)
	{
		if (frames * bytedepth_format(format) > this->buffer_size) {
			if (buffer_prepped) {
				reallocate_buffer = true;
			}
		} else {
			reallocate_buffer = false;
		}
		prep_events(in_chs);
		if (circle_buffer_prepped && (!buffer_prepped || reallocate_buffer)) {
			this->frames                 = frames;
			device_options.buffer_size   = frames;
			this->input_chs              = in_chs;
			device_options.channel_count = in_chs;
			this->format                 = format;
			this->samples_per_sec        = samples_per_sec;
			this->buffer_size            = frames * bytedepth_format(format);

			for (int i = 0; i < buffer_count; i++) {
				device_source_audio* _source_audio = get_source_audio(i);
				_source_audio->data                = (uint8_t**)bzalloc(input_chs * sizeof(uint8_t*));
				for (int j = 0; j < (int)input_chs; j++) {
					if (!buffer_prepped) {
						_source_audio->data[j] = (uint8_t*)bzalloc(buffer_size);
					} else if (reallocate_buffer) {
						uint8_t* tmp = (uint8_t*)brealloc(_source_audio->data[j], buffer_size);
						if (tmp == NULL) {
							buffer_prepped = false;
							all_prepped    = false;
							return;
						} else if (tmp == _source_audio->data[j]) {
							bfree(tmp);
							tmp = NULL;
						} else {
							_source_audio->data[j] = tmp;
							tmp                    = NULL;
						}
					}
				}
				_source_audio->input_chs       = input_chs;
				_source_audio->frames          = frames;
				_source_audio->format          = format;
				_source_audio->samples_per_sec = samples_per_sec;
			}
			buffer_prepped = true;
		}
		check_all();
	}

	std::vector<asio_listener*> get_listeners()
	{
		return _listeners;
	}

	void write_buffer_planar(const void* buffer, int channels, size_t samples, uint64_t timestamp_on_callback)
	{
		UNUSED_PARAMETER(channels);
		if (!all_prepped) {
			blog(LOG_INFO, "%s device %i is not prepared", __FUNCTION__, device_index);
			return;
		}
		int byte_depth = bytedepth_format(format);
		if (!byte_depth)
			return;
		device_source_audio* _source_audio = get_writeable_source_audio();
		if (!_source_audio)
			return;
		// ResetEvent(all_recieved_signal);
		// SetEvent(all_recieved_signal_2);
		SetEvent(all_recieved_signal);

		uint8_t** input_buffer   = (uint8_t**)buffer;
		size_t    ch_buffer_size = samples * byte_depth;
		if (ch_buffer_size > buffer_size) {
			blog(LOG_WARNING, "%s device needs to reallocate memory %ui to %ui", __FUNCTION__, buffer_size,
					2 * ch_buffer_size);
			frames = (uint32_t)(ch_buffer_size / byte_depth);
			re_prep_buffers();
		}

		audio_format planar_format = get_planar_format(format);
		// deinterleave directly into buffer (planar)
		if (input_buffer) {
			for (size_t j = 0; j < device_options.channel_count; j++) {
				if (!_source_audio->data[j])
					blog(LOG_INFO, "PANIC %llu", j);
				if (input_buffer[j])
					memcpy(_source_audio->data[j], input_buffer[j], ch_buffer_size);
				else
					memset(_source_audio->data[j], 0, ch_buffer_size);
			}
		} else {
			for (size_t j = 0; j < device_options.channel_count; j++) {
				memset(_source_audio->data[j], 0, ch_buffer_size);
			}
		}

		_source_audio->format          = planar_format;
		_source_audio->frames          = (uint32_t)samples;
		_source_audio->input_chs       = device_options.channel_count;
		_source_audio->samples_per_sec = samples_per_sec;
		_source_audio->timestamp = _source_audio->timestamp = timestamp_on_callback -
				((_source_audio->frames * NSEC_PER_SEC) / _source_audio->samples_per_sec);

		write_index++;
		write_index = write_index % buffer_count;
		ResetEvent(all_recieved_signal);
		// SetEvent(all_recieved_signal);
		// ResetEvent(all_recieved_signal_2);
	}

	void write_dual_buffer_planar(const void* buffer, const void* buffer2, int in, int out, size_t samples,
			uint64_t timestamp_on_callback)
	{
		if (!all_prepped) {
			blog(LOG_INFO, "%s device %i is not prepared", __FUNCTION__, device_index);
			return;
		}
		int byte_depth = bytedepth_format(format);
		if (!byte_depth)
			return;
		device_source_audio* _source_audio = get_writeable_source_audio();
		if (!_source_audio)
			return;

		SetEvent(all_recieved_signal);

		uint8_t** input_buffer   = (uint8_t**)buffer;
		uint8_t** output_buffer  = (uint8_t**)buffer2;
		size_t    ch_buffer_size = samples * byte_depth;
		if (ch_buffer_size > buffer_size) {
			blog(LOG_WARNING, "%s device needs to reallocate memory %ui to %ui", __FUNCTION__, buffer_size,
					2 * ch_buffer_size);
			frames = (uint32_t)(ch_buffer_size / byte_depth);
			re_prep_buffers();
		}

		audio_format planar_format = get_planar_format(format);
		// deinterleave directly into buffer (planar)
		size_t in_idx  = in < device_options.channel_count ? in : device_options.channel_count;
		size_t out_idx = (in_idx + out) < device_options.channel_count ? (in_idx + out)
									       : device_options.channel_count;

		size_t j = 0;
		if (input_buffer) {
			for (j = 0; j < in_idx; j++) {
				if (input_buffer[j])
					memcpy(_source_audio->data[j], input_buffer[j], ch_buffer_size);
				else
					memset(_source_audio->data[j], 0, ch_buffer_size);
			}
		} else {
			for (j = 0; j < in_idx; j++) {
				memset(_source_audio->data[j], 0, ch_buffer_size);
			}
		}

		if (output_buffer) {
			size_t jj = 0;
			for (; j < out_idx; j++) {
				if (output_buffer[jj])
					memcpy(_source_audio->data[j], output_buffer[jj], ch_buffer_size);
				else
					memset(_source_audio->data[j], 0, ch_buffer_size);
				jj++;
			}
		} else {
			for (; j < out_idx; j++) {
				memset(_source_audio->data[j], 0, ch_buffer_size);
			}
		}
		// 0 out remaining
		for (; j < device_options.channel_count; j++) {
			memset(_source_audio->data[j], 0, ch_buffer_size);
		}

		_source_audio->format          = planar_format;
		_source_audio->frames          = (uint32_t)samples;
		_source_audio->input_chs       = device_options.channel_count;
		_source_audio->samples_per_sec = samples_per_sec;
		_source_audio->timestamp = _source_audio->timestamp = timestamp_on_callback -
				((_source_audio->frames * NSEC_PER_SEC) / _source_audio->samples_per_sec);

		write_index++;
		write_index = write_index % buffer_count;
		ResetEvent(all_recieved_signal);
	}

	static DWORD WINAPI capture_thread(void* data)
	{
		device_buffer* device = static_cast<device_buffer*>(data);
		if (!device)
			return 0;

		device->isBufferActive  = true;
		std::string thread_name = "asio capture: ";
		thread_name += device->device_options.name;
		os_set_thread_name(thread_name.c_str());

		HANDLE signals_1[2] = {device->all_recieved_signal, device->stop_listening_signal};
		// HANDLE signals_2[2] = {device->all_recieved_signal_2, device->stop_listening_signal};

		size_t read_index = device->write_index;
		int    waitResult;
		long   route[MAX_AUDIO_CHANNELS];

		while (device->isBufferActive) {
			waitResult = WaitForMultipleObjects(2, signals_1, false, INFINITE);
			// waitResult = WaitForMultipleObjects(2, signals_2, false, INFINITE);
			// not entirely sure that all of these conditions are correct (at the very least this is)
			if (waitResult == WAIT_OBJECT_0) {
				std::vector<asio_listener*> listeners = device->get_listeners();
				while (read_index != device->write_index) {
					device_source_audio* in = device->get_source_audio(read_index);
					for (size_t i = 0; i < listeners.size(); i++) {
						if (listeners[i]->device_index != device->device_index) {
							blog(LOG_INFO, "(%x vs %x)", listeners[i]->device_index,
									device->device_index);
							// listeners.erase(listeners.begin() + i--);
							device->remove_listener(listeners[i]);
							continue;
						} else if (!listeners[i]->isASIOActive) {
							continue;
						}
						memcpy(&route[0], &listeners[i]->route[0],
								MAX_AUDIO_CHANNELS * sizeof(long));
						listeners[i]->render_audio(in, &route[0]);
					}
					read_index++;
					read_index = read_index % device->buffer_count;
				}
			} else if (waitResult == WAIT_OBJECT_0 + 1) {
				blog(LOG_INFO, "device %l indicated it wanted to disconnect", device->device_index);
				blog(LOG_INFO, "%s closing", thread_name.c_str());

				break;
			} else if (waitResult == WAIT_ABANDONED_0) {
				blog(LOG_INFO, "a mutex for %s was abandoned while listening to", thread_name.c_str(),
						device->device_index);
				blog(LOG_INFO, "%s closing", thread_name.c_str());

				break;
			} else if (waitResult == WAIT_ABANDONED_0 + 1) {
				blog(LOG_INFO, "a mutex for %s was abandoned while listening to", thread_name.c_str(),
						device->device_index);
				blog(LOG_INFO, "%s closing", thread_name.c_str());

				break;
			} else if (waitResult == WAIT_TIMEOUT) {
				blog(LOG_INFO, "%s timed out while listening to %l", thread_name.c_str(),
						device->device_index);
				blog(LOG_INFO, "%s closing", thread_name.c_str());

				break;
			} else if (waitResult == WAIT_FAILED) {
				blog(LOG_INFO, "listener thread wait %lu failed with 0x%x", device->device_index,
						GetLastError());
				blog(LOG_INFO, "%s closing", thread_name.c_str());

				break;
			} else {
				blog(LOG_INFO, "unexpected wait result = %i", waitResult);
				blog(LOG_INFO, "%s closing", thread_name.c_str());

				break;
			}
		}

		device->isBufferActive = false;
		return 0;
	}

	void wait()
	{
		if (captureThread.Valid()) {
			DWORD  waitResult;
			HANDLE signals_1[2] = {all_recieved_signal, stop_listening_signal};
			waitResult          = WaitForMultipleObjects(2, signals_1, false, INFINITE);
		}
	}
	// adds a listener thread between an asio_listener object and this device
	void add_listener(asio_listener* listener)
	{
		if (!all_prepped)
			return;

		for (size_t i = 0; i < _listeners.size(); i++) {
			if (_listeners[i] == listener) {
				blog(LOG_INFO, "(source_id: %x) already connected!", _listeners[i]->get_id().c_str());
				return;
			}
		}

		if (!captureThread.Valid())
			captureThread = CreateThread(nullptr, 0, this->capture_thread, this, 0, nullptr);

		//listener->disconnect();
		listener->buffer       = this;
		listener->device_index = device_index;
		listener->isASIOActive = true;
		_listeners.push_back(listener);
	}

	void remove_listener(asio_listener* listener)
	{
		_listeners.erase(std::remove_if(_listeners.begin(), _listeners.end(),
						 [&listener](asio_listener* item) { return item == listener; }),
				_listeners.end());
	}
};

// utility function
void add_listener_to_device(asio_listener* listener, device_buffer* buffer)
{
	if (!buffer || !listener)
		return;
	buffer->add_listener(listener);
}

static void wait_for_buffer(device_buffer* buffer)
{
	if (buffer)
		buffer->wait();
}
