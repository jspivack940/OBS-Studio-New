#include <obs-module.h>

#include "whip-output.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-webrtc", "en-US")
MODULE_EXPORT const char *obs_module_description(void)
{
	return "WHIP output & service";
}

extern struct obs_output_info whip_output_info;
extern struct obs_service_info whip_custom_service;

bool obs_module_load()
{
	obs_register_output(&whip_output_info);
	obs_register_service(&whip_custom_service);

	return true;
}
