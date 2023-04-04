#pragma once

#include <string>
#include <atomic>
#include <mutex>
#include <thread>

#include <obs.hpp>

#include <rtc/rtc.h>

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

	int audio_track, video_track;
	int64_t last_audio_timestamp, last_video_timestamp;
};

void register_whip_output();
