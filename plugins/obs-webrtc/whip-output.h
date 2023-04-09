#pragma once
#include <obs-module.h>
#include <util/curl/curl-helper.h>
#include <util/platform.h>
#include <util/base.h>

#include <string>
#include <atomic>
#include <mutex>
#include <thread>

#include <rtc/rtc.h>

#define do_log(level, output, format, ...)                      \
	blog(level, "[obs-webrtc] [whip_output: '%s'] " format, \
	     obs_output_get_name(output), ##__VA_ARGS__)

class WHIPOutput {
public:
	WHIPOutput(obs_data_t *settings, obs_output_t *output);
	~WHIPOutput();

	bool Start();
	void Stop(uint64_t ts = 0);
	void Data(struct encoder_packet *packet);
	uint64_t TotalBytes();

private:
	void ConfigureAudioTrack();
	void ConfigureVideoTrack();
	bool Setup();
	bool Connect();
	void StartThread();

	void SendDelete();
	void StopThread();

	void Send(void *data, uintptr_t size, uint64_t duration, int track);

	obs_output_t *output;

	std::string endpoint_url;
	std::string bearer_token;
	std::string resource_url;

	std::atomic<bool> running;

	std::mutex start_stop_mutex;
	std::thread start_stop_thread;

	int peer_connection;
	std::atomic<size_t> total_bytes_sent;

	int audio_track;
	int video_track;
	int64_t last_audio_timestamp;
	int64_t last_video_timestamp;
	int connect_time_ms;
	int64_t start_time;
};

void register_whip_output();

static std::string trim_string(const std::string &source)
{
	std::string ret(source);
	ret.erase(0, ret.find_first_not_of(" \n\r\t"));
	ret.erase(ret.find_last_not_of(" \n\r\t") + 1);
	return ret;
}

static size_t curl_writefunction(char *data, size_t size, size_t nmemb,
				 void *priv_data)
{
	auto read_buffer = static_cast<std::string *>(priv_data);

	size_t real_size = size * nmemb;

	read_buffer->append(data, real_size);
	return real_size;
}

#define LOCATION_HEADER_LENGTH 10

static size_t curl_headerfunction(char *data, size_t size, size_t nmemb,
				  void *priv_data)
{
	auto header_buffer = static_cast<std::string *>(priv_data);

	size_t real_size = size * nmemb;

	if (real_size < LOCATION_HEADER_LENGTH)
		return real_size;

	if (!strncmp(data, "location: ", LOCATION_HEADER_LENGTH)) {
		char *val = data + LOCATION_HEADER_LENGTH;
		header_buffer->append(val, real_size - LOCATION_HEADER_LENGTH);
		*header_buffer = trim_string(*header_buffer);
	}

	return real_size;
}
