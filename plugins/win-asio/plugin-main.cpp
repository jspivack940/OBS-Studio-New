/*  Copyright (c) 2022 pkv <pkv@obsproject.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program (see COPYING); if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301 USA.
 */

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/threading.h>

const char *PLUGIN_VERSION = "1.0.0";
OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("win-asio", "en-US")
MODULE_EXPORT const char *obs_module_description(void)
{
	return "ASIO audio plugin";
}

extern os_event_t *shutting_down;
bool retrieve_device_list();
void free_device_list();
void OBSEvent(enum obs_frontend_event event, void *);
void register_asio_source();
void register_asio_output();

bool obs_module_load(void)
{
	if (!retrieve_device_list())
		return false;
	register_asio_source();
	blog(LOG_INFO, "ASIO plugin loaded successfully (version %s)", PLUGIN_VERSION);
	if (os_event_init(&shutting_down, OS_EVENT_TYPE_AUTO))
		return false;
	obs_frontend_add_event_callback(OBSEvent, NULL);
	register_asio_output();
	return true;
}

void obs_module_unload()
{
	free_device_list();
	os_event_destroy(shutting_down);
	obs_frontend_remove_event_callback(OBSEvent, NULL);
}

