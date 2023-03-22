#pragma once

#include <string>

#include <obs.hpp>
#define MAX_CODECS 3

struct WHIPService {
	std::string server;
	std::string bearer_token;
	const char *video_codecs[MAX_CODECS] = {NULL};
	const char *audio_codecs[MAX_CODECS] = {NULL};

	WHIPService(obs_data_t *settings, obs_service_t *service);

	void Update(obs_data_t *settings);
	static obs_properties_t *Properties();
	static void ApplyEncoderSettings(obs_data_t *video_settings,
					 obs_data_t *audio_settings);
	static const char *GetConnectInfo(void *data, uint32_t type);
	static bool CanTryToConnect(void *data);
};

void register_whip_service();
