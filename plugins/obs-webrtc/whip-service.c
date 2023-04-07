#include <obs-module.h>

#define MAX_CODECS 3

struct whip_custom {
	char *server;
	char *bearer_token;
	const char *video_codecs[MAX_CODECS];
	const char *audio_codecs[MAX_CODECS];
};

static const char *whip_custom_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("Service.Name");
}

static void whip_custom_update(void *data, obs_data_t *settings)
{
	struct whip_custom *service = data;

	bfree(service->server);
	bfree(service->bearer_token);

	service->server = bstrdup(obs_data_get_string(settings, "server"));
	service->bearer_token =
		bstrdup(obs_data_get_string(settings, "bearer_token"));
}

static void whip_custom_destroy(void *data)
{
	struct whip_custom *service = data;

	bfree(service->server);
	bfree(service->bearer_token);
	bfree(service);
}

static void *whip_custom_create(obs_data_t *settings, obs_service_t *service)
{
	UNUSED_PARAMETER(service);

	struct whip_custom *data = bzalloc(sizeof(struct whip_custom));
	data->video_codecs[0] = "h264";
	data->audio_codecs[0] = "opus";
	whip_custom_update(data, settings);

	return data;
}

static obs_properties_t *whip_custom_properties(void *unused)
{
	UNUSED_PARAMETER(unused);
	obs_properties_t *ppts = obs_properties_create();

	obs_properties_add_text(ppts, "server", "URL", OBS_TEXT_DEFAULT);
	obs_properties_add_text(ppts, "bearer_token",
				obs_module_text("Service.BearerToken"),
				OBS_TEXT_PASSWORD);

	return ppts;
}

static const char *whip_custom_url(void *data)
{
	struct whip_custom *service = data;
	return service->server;
}

static const char *whip_custom_get_protocol(void *data)
{
	UNUSED_PARAMETER(data);
	return "WHIP";
}

const char *whip_custom_get_output_type(void *data)
{
	UNUSED_PARAMETER(data);
	return "whip_output";
};

static void whip_custom_apply_settings(void *data, obs_data_t *video_settings,
				       obs_data_t *audio_settings)
{
	UNUSED_PARAMETER(audio_settings);
	struct whip_custom *service = data;
	// No b-frames, SPS/PPS repetition & CBR required
	if (service->server != NULL && video_settings != NULL) {
		obs_data_set_int(video_settings, "bf", 0);
		obs_data_set_string(video_settings, "rate_control", "CBR");
		obs_data_set_bool(video_settings, "repeat_headers", true);
	}
}

static const char *whip_custom_get_connect_info(void *data, uint32_t type)
{
	struct whip_custom *service = data;
	switch ((enum obs_service_connect_info)type) {
	case OBS_SERVICE_CONNECT_INFO_SERVER_URL:
		return service->server;
	case OBS_SERVICE_CONNECT_INFO_BEARER_TOKEN:
		return service->bearer_token;
	default:
		return NULL;
	}
}

static const char **whip_custom_get_supported_video_codecs(void *data)
{
	struct whip_custom *service = data;
	return service->video_codecs;
}

static const char **whip_custom_get_supported_audio_codecs(void *data)
{
	struct whip_custom *service = data;
	return service->audio_codecs;
}

static bool whip_custom_can_try_to_connect(void *data)
{
	struct whip_custom *service = data;
	return (service->server != NULL && service->server[0] != '\0');
}

struct obs_service_info whip_custom_service = {
	.id = "whip_custom",
	.get_name = whip_custom_name,
	.create = whip_custom_create,
	.destroy = whip_custom_destroy,
	.update = whip_custom_update,
	.get_properties = whip_custom_properties,
	.get_protocol = whip_custom_get_protocol,
	.get_output_type = whip_custom_get_output_type,
	.get_url = whip_custom_url,
	.get_connect_info = whip_custom_get_connect_info,
	.apply_encoder_settings = whip_custom_apply_settings,
	.get_supported_video_codecs = whip_custom_get_supported_video_codecs,
	.get_supported_audio_codecs = whip_custom_get_supported_audio_codecs,
	.can_try_to_connect = whip_custom_can_try_to_connect,
};
