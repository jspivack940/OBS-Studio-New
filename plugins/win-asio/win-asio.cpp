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

#include "ASIODevice.hpp"

extern os_event_t *shutting_down;
asio_data *global_output_asio_data;
obs_data_t *global_output_settings;

static int get_obs_output_2() {
  struct obs_audio_info aoi;
  obs_get_audio_info(&aoi);
  return (int)get_audio_channels(aoi.speakers);
}

static const char *asio_input_getname(void *unused) {
  UNUSED_PARAMETER(unused);
  return obs_module_text("AsioInput");
}

ASIODeviceList *list;

bool retrieve_device_list() {
  list = new ASIODeviceList();
  list->scanForDevices();
  return list->deviceNames.size() > 0;
}

void free_device_list() { delete list; }

/* This creates the device if it hasn't been created by this or another source
  or retrieves its pointer if it already exists. The source is also added as
  a client of the device. */
static void attach_device(void *vptr, obs_data_t *settings) {
  struct asio_data *data = (struct asio_data *)vptr;
  std::string name(obs_data_get_string(settings, "device_id"));
  for (int i = 0; i < list->deviceNames.size(); i++) {
    if (list->deviceNames[i] == name) {
      data->asio_device = list->attachDevice(name);
      if (!data->asio_device) {
        blog(LOG_ERROR, "[asio_source '%s']:\nFailed to create device %s", obs_source_get_name(data->source),
             name.c_str());
      } else if (!data->asio_device->asioObject) {
        blog(LOG_ERROR, "[asio_source '%s']:\nDriver could not find a connected device or device might already be in use by another app.",
             obs_source_get_name(data->source));
        data->asio_device = nullptr;
      } else {
        data->device_index = i;
        // increment the device client list if the source was never a client
        // & add source ptr as a client of asio device.
        data->asio_client_index[i] = (int)data->asio_device->obs_clients.size();
        data->asio_device->obs_clients.push_back(data);
        data->asio_device->current_nb_clients++;
        // log some info about the device
        blog(LOG_INFO,
             "[asio_source '%s']:\nsource added to device %s;\n\tcurrent sample rate: %i,"
             "\n\tcurrent buffer: %i,"
             "\n\tinput latency: %f ms\n",
             obs_source_get_name(data->source), name.c_str(), (int)data->asio_device->getCurrentSampleRate(),
             data->asio_device->readBufferSizes(0),
             1000.0f * (float)data->asio_device->getInputLatencyInSamples() /
                 data->asio_device->getCurrentSampleRate());
      }
      break;
    }
  }
}

static void detach_device(void *vptr, std::string name) {
  struct asio_data *data = (struct asio_data *)vptr;
  int prev_dev_idx = list->getIndexFromDeviceName(name);
  int prev_client_idx = data->asio_client_index[prev_dev_idx];
  data->asio_device->obs_clients[prev_client_idx] = nullptr;
  data->asio_device->current_nb_clients--;
  if (data->asio_device->current_nb_clients == 0 && !data->asio_device->obs_output_client) {
    blog(LOG_INFO,
         "[asio_source '%s']: \n\tDevice % s removed;\n"
         "\tnumber of xruns: % i;\n\t -1 means xruns are not reported by your device. Increase your buffer if you get "
         "a high count & hear cracks, pops or else !\n ",
         obs_source_get_name(data->source), name.c_str(), data->asio_device->getXRunCount());
    if (data->asio_device->getXRunCount() >= 0) {
      blog(LOG_INFO,
           "\tnumber of xruns: % i;\n\tIncrease your buffer if you get a high count & hear cracks, pops or else !\n ",
           data->asio_device->getXRunCount());
    }
    data->asio_device->close();
  }
}

static void asio_input_update(void *vptr, obs_data_t *settings) {
  struct asio_data *data = (struct asio_data *)vptr;
  std::string err;
  bool swapping_device = false;
  const char *new_device = obs_data_get_string(settings, "device_id");
  std::string name(new_device);

  if (!new_device)
    return;

  // update the device data if we've swapped to a new one
  if (!data->device && new_device)
    data->device = bstrdup(new_device);

  if (!data->asio_device) {
    attach_device(data, settings);
  } else if (strcmp(data->asio_device->getName().c_str(), new_device) != 0) {
    detach_device(data, data->asio_device->getName());
    attach_device(data, settings);
    swapping_device = true;
  }

  ASIODevice *asio_device = data->asio_device;
  if (!asio_device)
    return;
  if (!asio_device->isOpen())
    err = asio_device->open(asio_device->getCurrentSampleRate(), asio_device->getDefaultBufferSize());

  // update the routing
  for (int i = 0; i < data->out_channels; i++) {
    std::string route_str = "route " + std::to_string(i);
    if (data->route[i] != (int)obs_data_get_int(settings, route_str.c_str())) {
      data->route[i] = (int)obs_data_get_int(settings, route_str.c_str());
    }
  }
}

static void *asio_input_create(obs_data_t *settings, obs_source_t *source) {
  struct asio_data *data = (struct asio_data *)bzalloc(sizeof(struct asio_data));
  data->source = source;
  data->asio_device = nullptr;
  data->output = nullptr;
  data->device = nullptr;
  for (int i = 0; i < maxNumASIODevices; i++)
    data->asio_client_index[i] = -1; // not a client if negative;
  speaker_layout layout = (speaker_layout)obs_data_get_int(settings, "speaker_layout");
  int recorded_channels = get_audio_channels(layout);
  data->out_channels = recorded_channels;
  data->stopping = false;
  data->active = true;
  for (int i = 0; i < MAX_AUDIO_CHANNELS; i++) {
    data->route[i] = -1;
  }
  asio_input_update(data, settings);
  blog(LOG_INFO, "[asio_source '%s']: created successfully.", obs_source_get_name(data->source));
  return data;
}

static void remove_client(void *vptr) {
  struct asio_data *data = (struct asio_data *)vptr;
  if (data->asio_device) {
    detach_device(data, data->asio_device->getName());
    data->stopping = true;
    data->asio_device = nullptr;
  }
}

static void asio_input_destroy(void *vptr) {
  struct asio_data *data = (struct asio_data *)vptr;

  if (!data)
    return;
  os_event_timedwait(shutting_down, 1000);
  ;
  /* delete the asio source from clients of asio device */
  if (data->device)
    bfree((void *)data->device);
  remove_client(data);
  bfree(data);
}

static bool fill_out_channels_modified(void *vptr, obs_properties_t *props, obs_property_t *chanlist,
                                       obs_data_t *settings) {
  UNUSED_PARAMETER(props);
  struct asio_data *data = (struct asio_data *)vptr;
  std::string name(obs_data_get_string(settings, "device_id"));

  obs_property_list_clear(chanlist);
  obs_property_list_add_int(chanlist, obs_module_text("Mute"), -1);
  if (!data->asio_device)
    return true;
  if (data->asio_client_index[data->device_index] >= 0) {
    std::vector<std::string> in_names = data->asio_device->getInputChannelNames();
    int input_channels = (int)in_names.size();
    for (int i = 0; i < input_channels; i++)
      obs_property_list_add_int(chanlist, in_names[i].c_str(), i);
    for (int i = input_channels; i < MAX_AUDIO_CHANNELS; i++) {
      std::string route_str = "route " + std::to_string(i);
      obs_data_set_int(settings, route_str.c_str(), -1);
    }

    // store the number of input channels for the device
    data->in_channels = input_channels; // not used anywhere actually ???
  }

  return true;
}

static bool asio_device_changed(void *vptr, obs_properties_t *props, obs_property_t *devlist, obs_data_t *settings) {
  struct asio_data *data = (struct asio_data *)vptr;
  int i;
  int output_channels = data->out_channels;
  int max_channels = MAX_AUDIO_CHANNELS;
  const char *curDeviceId = obs_data_get_string(settings, "device_id");
  obs_property_t *panel = obs_properties_get(props, "ctrl");
  std::vector<obs_property_t *> route(max_channels, nullptr);

  int itemCount = (int)obs_property_list_item_count(devlist);
  bool itemFound = false;

  for (i = 0; i < itemCount; i++) {
    const char *DeviceId = obs_property_list_item_string(devlist, i);
    if (strcmp(DeviceId, curDeviceId) == 0) {
      itemFound = true;
      break;
    }
  }

  if (!itemFound) {
    obs_property_list_insert_string(devlist, 0, " ", curDeviceId);
    obs_property_list_item_disable(devlist, 0, true);
  } else {
    /* update the channel names */
    for (i = 0; i < max_channels; i++) {
      std::string name = "route " + std::to_string(i);
      route[i] = obs_properties_get(props, name.c_str());
      obs_property_set_modified_callback2(route[i], fill_out_channels_modified, data);
      fill_out_channels_modified(data, props, route[i], settings);
      obs_property_set_visible(route[i], i < output_channels);
    }
  }

  if (data->asio_device) {
    obs_property_set_visible(panel, data->asio_device->hasControlPanel());
  }

  return true;
}

static bool asio_layout_changed(void *vptr, obs_properties_t *props, obs_property_t *list, obs_data_t *settings) {
  UNUSED_PARAMETER(list);
  struct asio_data *data = (struct asio_data *)vptr;
  int max_channels = MAX_AUDIO_CHANNELS;
  speaker_layout layout = (speaker_layout)obs_data_get_int(settings, "speaker_layout");
  int recorded_channels = get_audio_channels(layout);
  data->out_channels = recorded_channels;
  int i = 0;
  for (i = 0; i < max_channels; i++) {
    std::string name = "route " + std::to_string(i);
    obs_property_t *r = obs_properties_get(props, name.c_str());
    obs_property_set_modified_callback2(r, fill_out_channels_modified, data);
    fill_out_channels_modified(data, props, r, settings);
    obs_property_set_visible(r, i < recorded_channels);
  }
  return true;
}

static bool show_panel(obs_properties_t *props, obs_property_t *property, void *vptr) {
  UNUSED_PARAMETER(props);
  UNUSED_PARAMETER(property);

  if (!vptr)
    return false;
  struct asio_data *data = (struct asio_data *)vptr;
  ASIODevice *device = data->asio_device;
  if (device && device->hasControlPanel())
    device->showControlPanel();
  return false;
}

static bool show_panel_output(obs_properties_t *props, obs_property_t *property, void *vptr) {
  UNUSED_PARAMETER(props);
  UNUSED_PARAMETER(property);
  struct asio_data *data;
  if (!vptr)
    data = global_output_asio_data;
  else
    data = (struct asio_data *)vptr;
  ASIODevice *device = data->asio_device;
  if (device && device->hasControlPanel())
    device->showControlPanel();
  return false;
}

static std::vector<speaker_layout> known_layouts = {
    SPEAKERS_MONO,    /**< Channels: MONO */
    SPEAKERS_STEREO,  /**< Channels: FL, FR */
    SPEAKERS_2POINT1, /**< Channels: FL, FR, LFE */
    SPEAKERS_4POINT0, /**< Channels: FL, FR, FC, RC */
    SPEAKERS_4POINT1, /**< Channels: FL, FR, FC, LFE, RC */
    SPEAKERS_5POINT1, /**< Channels: FL, FR, FC, LFE, RL, RR */
    SPEAKERS_7POINT1, /**< Channels: FL, FR, FC, LFE, RL, RR, SL, SR */
};

static std::vector<std::string> known_layouts_str = {"Mono", "Stereo", "2.1", "4.0", "4.1", "5.1", "7.1"};

static obs_properties_t *asio_input_properties(void *vptr) {
  struct asio_data *data = (struct asio_data *)vptr;
  obs_properties_t *props = obs_properties_create();
  obs_property_t *devices;
  obs_property_t *format;
  obs_property_t *panel;
  int max_channels = MAX_AUDIO_CHANNELS;
  std::vector<obs_property_t *> route(max_channels, nullptr);

  devices = obs_properties_add_list(props, "device_id", obs_module_text("Device"), OBS_COMBO_TYPE_LIST,
                                    OBS_COMBO_FORMAT_STRING);
  obs_property_set_modified_callback2(devices, asio_device_changed, data);

  /* list of asio devices */
  std::vector<std::string> DeviceNames = list->deviceNames;
  for (int i = 0; i < DeviceNames.size(); i++) {
    obs_property_list_add_string(devices, DeviceNames[i].c_str(), DeviceNames[i].c_str());
  }

  /* setting up the speaker layout on input */
  format = obs_properties_add_list(props, "speaker_layout", obs_module_text("Format"), OBS_COMBO_TYPE_LIST,
                                   OBS_COMBO_FORMAT_INT);
  for (size_t i = 0; i < known_layouts.size(); i++)
    obs_property_list_add_int(format, known_layouts_str[i].c_str(), known_layouts[i]);
  obs_property_set_modified_callback2(format, asio_layout_changed, data);

  /* generating the list of obs channels */
  for (size_t i = 0; i < max_channels; i++) {
    route[i] = obs_properties_add_list(props, ("route " + std::to_string(i)).c_str(),
                                       obs_module_text(("Route." + std::to_string(i)).c_str()), OBS_COMBO_TYPE_LIST,
                                       OBS_COMBO_FORMAT_INT);
    if (i >= get_obs_output_2())
      obs_property_set_visible(route[i], false);
  }

  panel = obs_properties_add_button2(props, "ctrl", obs_module_text("Control Panel"), show_panel, vptr);

  return props;
}

static void asio_input_defaults(obs_data_t *settings) {
  struct obs_audio_info aoi;
  obs_get_audio_info(&aoi);
  std::vector<std::string> DeviceNames = list->deviceNames;
  obs_data_set_default_string(settings, "device_id", DeviceNames[0].c_str());
  obs_data_set_default_int(settings, "speaker_layout", aoi.speakers);

  for (int i = 0; i < MAX_AUDIO_CHANNELS; i++) {
    std::string name = "route " + std::to_string(i);
    obs_data_set_default_int(settings, name.c_str(), -1);
  }
}

static void asio_input_activate(void *vptr) {
  struct asio_data *data = (struct asio_data *)vptr;
  data->active = true;
}

static void asio_input_deactivate(void *vptr) {
  struct asio_data *data = (struct asio_data *)vptr;
  data->active = false;
}

void register_asio_source() {
  struct obs_source_info asio_input_capture = {};
  asio_input_capture.id = "asio_input_capture";
  asio_input_capture.type = OBS_SOURCE_TYPE_INPUT;
  asio_input_capture.output_flags = OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE;
  asio_input_capture.get_name = asio_input_getname;
  asio_input_capture.create = asio_input_create;
  asio_input_capture.destroy = asio_input_destroy;
  asio_input_capture.update = asio_input_update;
  asio_input_capture.get_defaults = asio_input_defaults;
  asio_input_capture.get_properties = asio_input_properties;
  asio_input_capture.icon_type = OBS_ICON_TYPE_AUDIO_INPUT;
  asio_input_capture.activate = asio_input_activate;
  asio_input_capture.deactivate = asio_input_deactivate;
  obs_register_source(&asio_input_capture);
}

#include <obs.hpp>

static const char *asio_output_getname(void *unused) {
  UNUSED_PARAMETER(unused);
  return obs_module_text("AsioOutput");
}
/* This creates the device if it hasn't been created by this or another source
  or retrieves its pointer if it already exists. The source is also added as
  a client of the device. */
static void attach_output_device(void *vptr, obs_data_t *settings) {
  struct asio_data *data = (struct asio_data *)vptr;
  std::string name(obs_data_get_string(settings, "device_id"));
  for (int i = 0; i < list->deviceNames.size(); i++) {
    if (list->deviceNames[i] == name) {
      data->asio_device = list->attachDevice(name);
      if (!data->asio_device) {
        blog(LOG_ERROR, "[asio_output]:\nFailed to create device %s", name.c_str());
      } else if (!data->asio_device->asioObject) {
        blog(LOG_ERROR, "[asio_output]:\nDriver could not find any connected device  or device might already be in use by another app.");
      } else {
        data->device_index = i;
        data->asio_device->obs_output_client = data;
        // log some info about the device
        blog(LOG_INFO,
             "[asio_output]:\nOutput added to device %s;\n\tcurrent sample rate: %i,"
             "\n\tcurrent buffer: %i,"
             "\n\toutput latency: %f ms\n",
             name.c_str(), (int)data->asio_device->getCurrentSampleRate(), data->asio_device->readBufferSizes(0),
             1000.0f * (float)data->asio_device->getOutputLatencyInSamples() /
                 data->asio_device->getCurrentSampleRate());
      }
      break;
    }
  }
}

static void detach_output_device(void *vptr, std::string name) {
  struct asio_data *data = (struct asio_data *)vptr;
  data->device_index = -1;
  data->asio_device->obs_output_client = nullptr;
  blog(LOG_INFO,
       "[asio_output]:\n\tDevice % s removed;\n"
       "\tnumber of xruns: % i;\n\tincrease your buffer if you get a high count & hear cracks, pops or else !\n -1 "
       "means your device doesn't report xruns.",
       name.c_str(), data->asio_device->getXRunCount());
  if (data->asio_device->current_nb_clients == 0)
    data->asio_device->close();
}

static void asio_output_update(void *vptr, obs_data_t *settings) {
  struct asio_data *data = (struct asio_data *)vptr;
  if (!global_output_settings)
    global_output_settings = settings;
  std::string err;
  bool swapping_device = false;
  const char *new_device = obs_data_get_string(global_output_settings, "device_id");
  std::string name(new_device);

  if (!new_device)
    return;

  // update the device data if we've swapped to a new one
  if (!data->device && new_device)
    data->device = bstrdup(new_device);

  if (!data->asio_device) {
    attach_output_device(data, global_output_settings);
  } else if (strcmp(data->asio_device->getName().c_str(), new_device) != 0) {
    detach_output_device(data, data->asio_device->getName());
    attach_output_device(data, global_output_settings);
    swapping_device = true;
  }

  ASIODevice *asio_device = data->asio_device;
  if (!asio_device)
    return;
  if (!asio_device->isOpen())
    err = asio_device->open(asio_device->getCurrentSampleRate(), asio_device->getDefaultBufferSize());

  // update the routing data for each output device channels & pass the info to the device
  data->out_channels = (uint8_t)data->asio_device->getOutputChannelNames().size();
  for (int i = 0; i < data->out_channels; i++) {
    std::string out_route_str = "device_ch" + std::to_string(i);
    if (data->out_route[i] != (int)obs_data_get_int(global_output_settings, out_route_str.c_str())) {
      data->out_route[i] = (int)obs_data_get_int(global_output_settings, out_route_str.c_str());
    }
    data->asio_device->obs_track[i] = -1;         // device does not use track i
    data->asio_device->obs_track_channel[i] = -1; // device does not use any channel from track i
    for (int j = 0; j < MAX_AUDIO_MIXES; j++) {
      for (int k = 0; k < data->obs_track_channels; k++) {
        if (data->out_route[i] >= 0 && data->out_route[i] & (1ULL << (j + 4))) {
          if (k == (data->out_route[i] - (1ULL << (j + 4)))) {
            blog(LOG_DEBUG, "[asio_output]:\nDevice output channel n° %i: Track %i, Channel %i", i, j + 1, k + 1);
            data->asio_device->obs_track[i] = j;
            data->asio_device->obs_track_channel[i] = k;
          }
        }
      }
    }
  }
}

static void remove_output_client(void *vptr) {
  struct asio_data *data = (struct asio_data *)vptr;
  detach_output_device(data, data->asio_device->getName());
  if (data->asio_device) {
    data->stopping = true;
    data->asio_device = nullptr;
  }
}

static void asio_output_stop(void *vptr, uint64_t ts) {
  struct asio_data *data = (struct asio_data *)vptr;
  data->asio_device->capture_started = false;
  obs_output_end_data_capture(data->output);
  for (int j = 0; j < MAX_DEVICE_CHANNELS; j++) {
    deque_free(&data->asio_device->excess_frames[j]);
  }
}

static void asio_output_destroy(void *vptr) {
  // the output is destroyed each time the output is stopped.
  struct asio_data *data = (struct asio_data *)vptr;

  if (!data)
    return;

  os_event_timedwait(shutting_down, 1000);

  if (data->asio_device) {
    bfree((void *)data->device);
    data->asio_device = nullptr;
    // the device class is deleted separately through the shutting_down os_event
  }

  bfree(global_output_asio_data);
  obs_data_release(global_output_settings);
  global_output_asio_data = nullptr;
  global_output_settings = nullptr;
  data = nullptr;
}

static int get_output_device_channel_count(std::string name) {
  for (int i = 0; i < list->deviceNames.size(); i++) {
    if (list->deviceNames[i] == name) {
      ASIODevice *asio_device = list->attachDevice(name);
      return asio_device->totalNumOutputChans;
    }
  }
  return 0;
}

static std::vector<std::string> get_device_channel_names(std::string name) {
  for (int i = 0; i < list->deviceNames.size(); i++) {
    if (list->deviceNames[i] == name) {
      ASIODevice *asio_device = list->attachDevice(name);
      return asio_device->getOutputChannelNames();
    }
  }
  std::vector<std::string> empty;
  return empty;
}

static bool asio_output_update_cb(obs_properties_t *props, obs_property_t *devlist, obs_data_t *settings) {
  asio_output_update(global_output_asio_data, global_output_settings);
  return true;
}

static bool asio_output_device_changed(obs_properties_t *props, obs_property_t *devlist, obs_data_t *settings) {
  if (!global_output_settings)
    global_output_settings = settings;
  int i;
  int output_channels = get_obs_output_channels();
  int max_channels = MAX_AUDIO_CHANNELS;
  const char *curDeviceId = obs_data_get_string(global_output_settings, "device_id");
  obs_property_t *panel = obs_properties_get(props, "ctrl");
  int obs_output_channels_count = get_obs_output_channels();
  std::vector<obs_property_t *> device_channel(MAX_DEVICE_CHANNELS, nullptr);
  std::vector<obs_property_t *> route(max_channels, nullptr);

  int itemCount = (int)obs_property_list_item_count(devlist);
  bool itemFound = false;

  for (i = 0; i < itemCount; i++) {
    const char *DeviceId = obs_property_list_item_string(devlist, i);
    if (strcmp(DeviceId, curDeviceId) == 0) {
      itemFound = true;
      break;
    }
  }

  if (!itemFound) {
    obs_property_list_insert_string(devlist, 0, " ", curDeviceId);
    obs_property_list_item_disable(devlist, 0, true);
  } else {
    /* hide the channels beyond those supported by the device */
    std::string devName(curDeviceId);
    int output_channels_device = get_output_device_channel_count(devName);
    std::vector<std::string> output_ch_names = get_device_channel_names(devName);
    for (i = 0; i < MAX_DEVICE_CHANNELS; i++) {
      std::string name = "device_ch" + std::to_string(i);
      obs_properties_remove_by_name(props, name.c_str());
    }
    for (i = 0; i < output_channels_device; i++) {
      std::string name = "device_ch" + std::to_string(i);
      device_channel[i] = obs_properties_add_list(props, name.c_str(), output_ch_names[i].c_str(), OBS_COMBO_TYPE_LIST,
                                                  OBS_COMBO_FORMAT_INT);
      obs_property_set_visible(device_channel[i], i < output_channels_device);
      obs_property_list_add_int(device_channel[i], obs_module_text("Mute"), -1);
      obs_property_set_modified_callback(device_channel[i], asio_output_update_cb);
      for (size_t j = 0; j < MAX_AUDIO_MIXES; j++) {
        for (size_t k = 0; k < obs_output_channels_count; k++) {
          // We store track and track channel in the following way:
          // 3 bits are reserved for the track channel (0-7);
          // track_index is stored as 1 << track_index + 4
          // so: track 0 = 16, track 1 = 32, etc.
          // We could have started at 2^3 = 1 << 3 but in this way
          // we allow for up to 16 track channels.
          long long idx = k + (1ULL << (j + 4));
          std::string track_chan_name = "Track" + std::to_string(j) + "." + std::to_string(k);
          obs_property_list_add_int(device_channel[i], obs_module_text(track_chan_name.c_str()), idx);
        }
      }
    }
  }
  return true;
}

static bool asio_output_start(void *vptr) {
  struct asio_data *data = (struct asio_data *)vptr;
  if (!obs_output_can_begin_data_capture(data->output, 0))
    return false;
  struct obs_audio_info aoi;
  obs_get_audio_info(&aoi);
  struct audio_convert_info aci = {};
  // Audio is always planar for asio so we ask obs to convert to planar format.
  aci.format = AUDIO_FORMAT_FLOAT_PLANAR;
  aci.speakers = aoi.speakers;
  aci.samples_per_sec = (uint32_t)data->asio_device->getCurrentSampleRate();
  obs_output_set_audio_conversion(data->output, &aci);

  if (!obs_output_begin_data_capture(data->output, 0))
    return false;

  return true;
}

static void *asio_output_create(obs_data_t *settings, obs_output_t *output) {
  struct asio_data *data = (struct asio_data *)bzalloc(sizeof(struct asio_data));
  data->output = output;
  // allow all tracks for asio output including track 7 (aka asio monitoring)
  obs_output_set_mixers(data->output, (1 << 5) + (1 << 4) + (1 << 3) + (1 << 2) + (1 << 1) + (1 << 0));
  data->asio_device = nullptr;
  data->source = nullptr;
  data->device = nullptr;
  for (int i = 0; i < maxNumASIODevices; i++)
    data->asio_client_index[i] = -1; // not a client if negative;
  data->obs_track_channels = (uint8_t)audio_output_get_channels(obs_get_audio());
  data->stopping = false;
  data->active = true;
  for (int i = 0; i < MAX_DEVICE_CHANNELS; i++) {
    data->out_route[i] = -1;
  }
  if (global_output_asio_data)
    blog(LOG_INFO, "issue");
  global_output_asio_data = data;
  asio_output_update(data, settings);
  return data;
}

static void asio_receive_audio(void *vptr, size_t mix_idx, struct audio_data *frame) {
  struct asio_data *data = (struct asio_data *)vptr;
  struct audio_data in = *frame;
  ASIODevice *device = data->asio_device;

  for (int i = 0; i < device->totalNumOutputChans; i++) {
    for (int j = 0; j < data->obs_track_channels; j++) {
      if (device->obs_track[i] == mix_idx && device->obs_track_channel[i] == j) {
        deque_push_back(&device->excess_frames[i], in.data[j], in.frames * sizeof(int32_t));
      }
    }
  }
  data->asio_device->capture_started = true;
}

static uint64_t asio_output_total_bytes(void *data) { return 0; }

static void asio_output_defaults(obs_data_t *settings) {
  std::vector<std::string> DeviceNames = list->deviceNames;
  obs_data_set_default_string(settings, "device_id", DeviceNames[0].c_str());

  for (int i = 0; i < MAX_DEVICE_CHANNELS; i++) {
    std::string name = "device_ch" + std::to_string(i);
    obs_data_set_default_int(settings, name.c_str(), -1);
  }
}

static obs_properties_t *asio_output_properties(void *vptr) {
  struct asio_data *data = vptr ? (struct asio_data *)vptr : global_output_asio_data;
  obs_properties_t *props = obs_properties_create();
  obs_property_t *devices;
  obs_property_t *panel;
  int max_channels = MAX_DEVICE_CHANNELS;

  std::vector<obs_property_t *> device_channel(max_channels, nullptr);

  devices = obs_properties_add_list(props, "device_id", obs_module_text("Device"), OBS_COMBO_TYPE_LIST,
                                    OBS_COMBO_FORMAT_STRING);

  /* list of asio devices */
  std::vector<std::string> DeviceNames = list->deviceNames;
  for (int i = 0; i < DeviceNames.size(); i++) {
    obs_property_list_add_string(devices, DeviceNames[i].c_str(), DeviceNames[i].c_str());
  }

  panel = obs_properties_add_button2(props, "ctrl", obs_module_text("Control Panel"), show_panel_output, vptr);
  obs_property_set_modified_callback(devices, asio_output_device_changed);
  return props;
}
void register_asio_output() {
  struct obs_output_info asio_output = {};
  asio_output.id = "asio_output";
  asio_output.flags = OBS_OUTPUT_AUDIO | OBS_OUTPUT_MULTI_TRACK;
  asio_output.get_name = asio_output_getname;
  asio_output.create = asio_output_create;
  asio_output.destroy = asio_output_destroy;
  asio_output.start = asio_output_start;
  asio_output.stop = asio_output_stop;
  asio_output.update = asio_output_update;
  asio_output.raw_audio2 = asio_receive_audio;
  asio_output.get_total_bytes = asio_output_total_bytes;
  asio_output.get_properties = asio_output_properties;
  asio_output.get_defaults = asio_output_defaults;
  obs_register_output(&asio_output);
}
