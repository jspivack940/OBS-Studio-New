#pragma once
#include <obs-module.h>
#include <util/curl/curl-helper.h>
#include <util/platform.h>
#include <util/threading.h>
#include <util/darray.h>
#include <util/dstr.h>
#include <util/base.h>
#include <rtc/rtc.h>

#ifdef _WIN32
#include <Iphlpapi.h>
#else
#include <sys/ioctl.h>
#endif

#define do_log(level, output, format, ...)                      \
	blog(level, "[obs-webrtc] [whip_output: '%s'] " format, \
	     obs_output_get_name(output), ##__VA_ARGS__)

/* global constants */
//char *msid = "obs-studio";
//
//uint32_t audio_ssrc = 5002;
//char *audio_cname = "audio";
//char *audio_mid = "0";
//uint32_t audio_clockrate = 48000;
//uint8_t audio_payload_type = 111;
//
//uint32_t video_ssrc = 5000;
//char *video_cname = "video";
//char *video_mid = "1";
//uint32_t video_clockrate = 90000;
//uint8_t video_payload_type = 96;

struct whip_output {
	obs_output_t *output;

	const char *endpoint_url;
	const char *bearer_token;
	const char *resource_url;

	volatile bool running;
	pthread_mutex_t start_stop_mutex;
	pthread_t start_stop_thread;

	int peer_connection;
	uint64_t total_bytes_sent;

	int audio_track;
	int video_track;
	int64_t last_audio_timestamp;
	int64_t last_video_timestamp;

	int dropped_frames;
	int connect_time_ms;
};

static size_t curl_writefunction(char *data, size_t size, size_t nmemb,
				 void *priv_data)
{
	struct dstr *read_buffer = (struct dstr *)priv_data;

	size_t real_size = size * nmemb;

	dstr_cat(read_buffer, data);
	dstr_resize(read_buffer, real_size);
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

	if (!strncmp(data, "location: ", LOCATION_HEADER_LENGTH)) {
		char *val = data + LOCATION_HEADER_LENGTH;
		struct dstr val2 = {0};
		dstr_copy(&val2, val);
		dstr_resize(&val2, real_size - LOCATION_HEADER_LENGTH);
		dstr_cat(header_buffer, strdepad(val2.array));
	}
	return real_size;
}
