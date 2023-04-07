#pragma once
#include <obs-module.h>
#include <util/curl/curl-helper.h>
#include <util/platform.h>
#include <util/threading.h>
#include <util/darray.h>
#include <util/dstr.h>
#include <util/base.h>
#include <rtc/rtc.h>

#define do_log(level, output, format, ...)                      \
	blog(level, "[obs-webrtc] [whip_output: '%s'] " format, \
	     obs_output_get_name(output), ##__VA_ARGS__)

struct whip_output {
	obs_output_t *output;
	const char *endpoint_url;
	const char *bearer_token;
	char *resource_url;

	volatile bool running;
	volatile bool stop_signal;
	pthread_mutex_t start_stop_mutex;
	pthread_t start_stop_thread;
	bool start_stop_thread_active;

	int peer_connection;
	uint64_t total_bytes_sent;

	int audio_track;
	int video_track;
	int64_t last_audio_timestamp;
	int64_t last_video_timestamp;

	int connect_time_ms;
	int64_t start_time_ns;
};

static size_t curl_writefunction(char *data, size_t size, size_t nmemb,
				 void *priv_data)
{
	struct dstr *read_buffer = (struct dstr *)priv_data;
	size_t real_size = size * nmemb;

	dstr_cat(read_buffer, data);

	return real_size;
}

#define LOCATION_HEADER_LENGTH 10

static size_t curl_headerfunction(char *data, size_t size, size_t nmemb,
				  void *priv_data)
{
	struct dstr *header_buffer = (struct dstr *)priv_data;

	size_t real_size = size * nmemb;

	if (real_size < LOCATION_HEADER_LENGTH)
		return real_size;

	if (!astrcmpi_n(data, "location: ", LOCATION_HEADER_LENGTH)) {
		char *val = data + LOCATION_HEADER_LENGTH;
		struct dstr val2 = {0};
		dstr_copy(&val2, val);
		dstr_cat(header_buffer, strdepad(val2.array));
	}
	return real_size;
}
