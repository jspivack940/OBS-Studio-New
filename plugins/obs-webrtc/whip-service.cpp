#include <obs-module.h>

#include "whip-service.h"

WHIPService::WHIPService(obs_data_t *settings, obs_service_t *)
	: server(), bearer_token()
{
	video_codecs[0] = "h264";
	audio_codecs[0] = "opus";

	Update(settings);
}

void WHIPService::Update(obs_data_t *settings)
{
	server = obs_data_get_string(settings, "server");
	bearer_token = obs_data_get_string(settings, "bearer_token");
}

obs_properties_t *WHIPService::Properties()
{
	obs_properties_t *ppts = obs_properties_create();

	obs_properties_add_text(ppts, "server", "URL", OBS_TEXT_DEFAULT);
	obs_properties_add_text(ppts, "bearer_token",
				obs_module_text("Service.BearerToken"),
				OBS_TEXT_PASSWORD);

	return ppts;
}

void WHIPService::ApplyEncoderSettings(obs_data_t *video_settings, obs_data_t *)
{
	// For now, ensure maximum compatibility with webrtc peers
	if (video_settings) {
		obs_data_set_int(video_settings, "bf", 0);
		obs_data_set_string(video_settings, "profile", "baseline");
		obs_data_set_string(video_settings, "rate_control", "CBR");
		obs_data_set_bool(video_settings, "repeat_headers", true);
	}
}

const char *WHIPService::GetConnectInfo(void *data, uint32_t type)
{
	switch ((enum obs_service_connect_info)type) {
	case OBS_SERVICE_CONNECT_INFO_SERVER_URL:
		return static_cast<WHIPService *>(data)->server.c_str();
	case OBS_SERVICE_CONNECT_INFO_BEARER_TOKEN:
		return static_cast<WHIPService *>(data)->bearer_token.c_str();
	case OBS_SERVICE_CONNECT_INFO_STREAM_ID:
	case OBS_SERVICE_CONNECT_INFO_USERNAME:
	case OBS_SERVICE_CONNECT_INFO_PASSWORD:
	case OBS_SERVICE_CONNECT_INFO_ENCRYPT_PASSPHRASE:
		return NULL;
	}

	return NULL;
}

bool WHIPService::CanTryToConnect(void *data)
{
	WHIPService *service = static_cast<WHIPService *>(data);

	return (service->server.c_str() != NULL &&
		service->server.c_str()[0] != '\0');
}

void register_whip_service()
{
	struct obs_service_info info = {};

	info.id = "whip_custom";
	info.get_name = [](void *) -> const char * {
		return obs_module_text("Service.Name");
	};
	info.create = [](obs_data_t *settings,
			 obs_service_t *service) -> void * {
		return new WHIPService(settings, service);
	};
	info.destroy = [](void *priv_data) {
		delete static_cast<WHIPService *>(priv_data);
	};
	info.update = [](void *priv_data, obs_data_t *settings) {
		static_cast<WHIPService *>(priv_data)->Update(settings);
	};
	info.get_properties = [](void *) -> obs_properties_t * {
		return WHIPService::Properties();
	};
	info.get_protocol = [](void *) -> const char * { return "WHIP"; };
	info.get_url = [](void *priv_data) -> const char * {
		return static_cast<WHIPService *>(priv_data)->server.c_str();
	};
	info.get_output_type = [](void *) -> const char * {
		return "whip_output";
	};
	info.get_connect_info = [](void *data, uint32_t type) -> const char * {
		return WHIPService::GetConnectInfo(data, type);
	};
	info.apply_encoder_settings = [](void *, obs_data_t *video_settings,
					 obs_data_t *audio_settings) {
		WHIPService::ApplyEncoderSettings(video_settings,
						  audio_settings);
	};
	info.get_supported_video_codecs = [](void *data) -> const char ** {
		return (const char **)static_cast<WHIPService *>(data)
			->video_codecs;
	};
	info.get_supported_audio_codecs = [](void *data) -> const char ** {
		return (const char **)static_cast<WHIPService *>(data)
			->audio_codecs;
	};
	info.can_try_to_connect = [](void *data) -> bool {
		return WHIPService::CanTryToConnect(data);
	};

	obs_register_service(&info);
}
