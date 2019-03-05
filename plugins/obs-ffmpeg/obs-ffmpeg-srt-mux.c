/******************************************************************************
    Copyright (C) 2015 by Hugh Bailey <obs.jim@gmail.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

#include <obs-module.h>
#include <obs-hotkey.h>
#include <obs-avc.h>
#include <util/dstr.h>
#include <util/pipe.h>
#include <util/darray.h>
#include <util/platform.h>
#include <util/circlebuf.h>
#include <util/threading.h>
#include "ffmpeg-mux/ffmpeg-mux.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#include "util/windows/win-version.h"
#include <io.h>
#include <fcntl.h>
#include <windows.h>
#define inline __inline
#endif

#include <libavformat/avformat.h>

#define do_log(level, format, ...) \
	blog(level, "[ffmpeg srt muxer: '%s'] " format, \
			obs_output_get_name(stream->output), ##__VA_ARGS__)

#define warn(format, ...)  do_log(LOG_WARNING, format, ##__VA_ARGS__)
#define info(format, ...)  do_log(LOG_INFO,    format, ##__VA_ARGS__)

struct ffmpeg_muxer {
	obs_output_t      *output;
	os_process_pipe_t *pipe;
	int64_t           stop_ts;
	uint64_t          total_bytes;
	struct dstr       path;
	bool              sent_headers;
	volatile bool     active;
	volatile bool     stopping;
	volatile bool     capturing;

	DARRAY(struct encoder_packet) mux_packets;
	pthread_t                     mux_thread;
	bool                          mux_thread_joinable;
	volatile bool                 muxing;
};

static const char *srt_getname(void *type)
{
	UNUSED_PARAMETER(type);
	return obs_module_text("FFmpegSrtMuxer");
}

static void srt_destroy(void *data)
{
	struct ffmpeg_muxer *stream = data;

	if (stream->mux_thread_joinable)
		pthread_join(stream->mux_thread, NULL);
	da_free(stream->mux_packets);

	os_process_pipe_destroy(stream->pipe);
	dstr_free(&stream->path);
	bfree(stream);
}

static void *srt_create(obs_data_t *settings, obs_output_t *output)
{
	struct ffmpeg_muxer *stream = bzalloc(sizeof(*stream));
	stream->output = output;

	UNUSED_PARAMETER(settings);
	return stream;
}

#ifdef _WIN32
#ifdef _WIN64
#define FFMPEG_MUX "ffmpeg-mux64.exe"
#else
#define FFMPEG_MUX "ffmpeg-mux32.exe"
#endif
#else
#define FFMPEG_MUX "ffmpeg-mux"
#endif

static inline bool capturing(struct ffmpeg_muxer *stream)
{
	return os_atomic_load_bool(&stream->capturing);
}

static inline bool stopping(struct ffmpeg_muxer *stream)
{
	return os_atomic_load_bool(&stream->stopping);
}

static inline bool active(struct ffmpeg_muxer *stream)
{
	return os_atomic_load_bool(&stream->active);
}

/* TODO: allow codecs other than h264 whenever we start using them */

static void add_video_encoder_params(struct ffmpeg_muxer *stream,
		struct dstr *cmd, obs_encoder_t *vencoder)
{
	obs_data_t *settings = obs_encoder_get_settings(vencoder);
	int bitrate = (int)obs_data_get_int(settings, "bitrate");
	video_t *video = obs_get_video();
	const struct video_output_info *info = video_output_get_info(video);

	obs_data_release(settings);

	dstr_catf(cmd, "%s %d %d %d %d %d ",
			obs_encoder_get_codec(vencoder),
			bitrate,
			obs_output_get_width(stream->output),
			obs_output_get_height(stream->output),
			(int)info->fps_num,
			(int)info->fps_den);
}

static void add_audio_encoder_params(struct dstr *cmd, obs_encoder_t *aencoder)
{
	obs_data_t *settings = obs_encoder_get_settings(aencoder);
	int bitrate = (int)obs_data_get_int(settings, "bitrate");
	audio_t *audio = obs_get_audio();
	struct dstr name = {0};

	obs_data_release(settings);

	dstr_copy(&name, obs_encoder_get_name(aencoder));
	dstr_replace(&name, "\"", "\"\"");

	dstr_catf(cmd, "\"%s\" %d %d %d ",
			name.array,
			bitrate,
			(int)obs_encoder_get_sample_rate(aencoder),
			(int)audio_output_get_channels(audio));

	dstr_free(&name);
}

static void log_muxer_params(struct ffmpeg_muxer *stream, const char *settings)
{
	int ret;

	AVDictionary *dict = NULL;
	if ((ret = av_dict_parse_string(&dict, settings, "=", " ", 0))) {
		warn("Failed to parse muxer settings: %s\n%s",
				av_err2str(ret), settings);

		av_dict_free(&dict);
		return;
	}

	if (av_dict_count(dict) > 0) {
		struct dstr str = {0};

		AVDictionaryEntry *entry = NULL;
		while ((entry = av_dict_get(dict, "", entry,
						AV_DICT_IGNORE_SUFFIX)))
			dstr_catf(&str, "\n\t%s=%s", entry->key, entry->value);

		info("Using muxer settings:%s", str.array);
		dstr_free(&str);
	}

	av_dict_free(&dict);
}

static void add_muxer_params(struct dstr *cmd, struct ffmpeg_muxer *stream)
{
	obs_data_t *settings = obs_output_get_settings(stream->output);
	struct dstr mux = {0};

	dstr_copy(&mux, obs_data_get_string(settings, "muxer_settings"));

	log_muxer_params(stream, mux.array);

	dstr_replace(&mux, "\"", "\\\"");
	obs_data_release(settings);

	dstr_catf(cmd, "\"%s\" ", mux.array ? mux.array : "");

	dstr_free(&mux);
}

static void build_command_line(struct ffmpeg_muxer *stream, struct dstr *cmd,
		const char *path)
{
	obs_encoder_t *vencoder = obs_output_get_video_encoder(stream->output);
	obs_encoder_t *aencoders[MAX_AUDIO_MIXES];
	int num_tracks = 0;

	for (;;) {
		obs_encoder_t *aencoder = obs_output_get_audio_encoder(
				stream->output, num_tracks);
		if (!aencoder)
			break;

		aencoders[num_tracks] = aencoder;
		num_tracks++;
	}

	dstr_init_move_array(cmd, obs_module_file(FFMPEG_MUX));
	dstr_insert_ch(cmd, 0, '\"');
	dstr_cat(cmd, "\" \"");

	dstr_copy(&stream->path, path);
	dstr_replace(&stream->path, "\"", "\"\"");
	dstr_cat_dstr(cmd, &stream->path);

	dstr_catf(cmd, "\" %d %d ", vencoder ? 1 : 0, num_tracks);

	if (vencoder)
		add_video_encoder_params(stream, cmd, vencoder);

	if (num_tracks) {
		dstr_cat(cmd, "aac ");

		for (int i = 0; i < num_tracks; i++) {
			add_audio_encoder_params(cmd, aencoders[i]);
		}
	}

	add_muxer_params(cmd, stream);
}

static inline void start_pipe(struct ffmpeg_muxer *stream, const char *path)
{
	struct dstr cmd;
	build_command_line(stream, &cmd, path);
	stream->pipe = os_process_pipe_create(cmd.array, "w");
	dstr_free(&cmd);
}

static bool srt_start(void *data)
{
	struct ffmpeg_muxer *stream = data;
	obs_data_t *settings;
	obs_service_t *service;
	const char *path;

	service = obs_output_get_service(stream->output);
	const char *name = obs_service_get_id(service);
	const char *array = obs_service_get_url(service);
	if (!service)
		return false;

	if (!obs_output_can_begin_data_capture(stream->output, 0))
		return false;
	if (!obs_output_initialize_encoders(stream->output, 0))
		return false;

	settings = obs_output_get_settings(stream->output);
	dstr_copy(&stream->path, array);
	dstr_depad(&stream->path);
	path = stream->path.array;

	start_pipe(stream, path);
	obs_data_release(settings);

	if (!stream->pipe) {
		obs_output_set_last_error(stream->output,
			obs_module_text("HelperProcessFailed"));
		warn("Failed to create process pipe");
		return false;
	}

	/* write headers and start capture */
	os_atomic_set_bool(&stream->active, true);
	os_atomic_set_bool(&stream->capturing, true);
	stream->total_bytes = 0;
	obs_output_begin_data_capture(stream->output, 0);

	info("Streaming to URL '%s'...", path/*stream->path.array*/);
	return true;
}

static int deactivate(struct ffmpeg_muxer *stream)
{
	int ret = -1;

	if (active(stream)) {
		ret = os_process_pipe_destroy(stream->pipe);
		stream->pipe = NULL;

		os_atomic_set_bool(&stream->active, false);
		os_atomic_set_bool(&stream->sent_headers, false);

		info("Streaming to srt '%s' stopped", stream->path.array);
	}

	if (stopping(stream))
		obs_output_end_data_capture(stream->output);

	os_atomic_set_bool(&stream->stopping, false);
	return ret;
}

static void srt_stop(void *data, uint64_t ts)
{
	struct ffmpeg_muxer *stream = data;

	if (capturing(stream) || ts == 0) {
		stream->stop_ts = (int64_t)ts / 1000LL;
		os_atomic_set_bool(&stream->stopping, true);
		os_atomic_set_bool(&stream->capturing, false);
	}
}

static void signal_failure(struct ffmpeg_muxer *stream)
{
	int ret = deactivate(stream);
	int code;

	switch (ret) {
	case FFM_UNSUPPORTED:          code = OBS_OUTPUT_UNSUPPORTED; break;
	default:                       code = OBS_OUTPUT_ERROR;
	}

	obs_output_signal_stop(stream->output, code);
	os_atomic_set_bool(&stream->capturing, false);
}

static bool write_packet(struct ffmpeg_muxer *stream,
		struct encoder_packet *packet)
{
	bool is_video = packet->type == OBS_ENCODER_VIDEO;
	size_t ret;

	struct ffm_packet_info info = {
		.pts = packet->pts,
		.dts = packet->dts,
		.size = (uint32_t)packet->size,
		.index = (int)packet->track_idx,
		.type = is_video ? FFM_PACKET_VIDEO : FFM_PACKET_AUDIO,
		.keyframe = packet->keyframe
	};

	ret = os_process_pipe_write(stream->pipe, (const uint8_t*)&info,
			sizeof(info));
	if (ret != sizeof(info)) {
		warn("os_process_pipe_write for info structure failed");
		signal_failure(stream);
		return false;
	}

	ret = os_process_pipe_write(stream->pipe, packet->data, packet->size);
	if (ret != packet->size) {
		warn("os_process_pipe_write for packet data failed");
		signal_failure(stream);
		return false;
	}

	stream->total_bytes += packet->size;
	return true;
}

static bool send_audio_headers(struct ffmpeg_muxer *stream,
		obs_encoder_t *aencoder, size_t idx)
{
	struct encoder_packet packet = {
		.type         = OBS_ENCODER_AUDIO,
		.timebase_den = 1,
		.track_idx    = idx
	};

	obs_encoder_get_extra_data(aencoder, &packet.data, &packet.size);
	return write_packet(stream, &packet);
}

static bool send_video_headers(struct ffmpeg_muxer *stream)
{
	obs_encoder_t *vencoder = obs_output_get_video_encoder(stream->output);

	struct encoder_packet packet = {
		.type         = OBS_ENCODER_VIDEO,
		.timebase_den = 1
	};

	obs_encoder_get_extra_data(vencoder, &packet.data, &packet.size);
	return write_packet(stream, &packet);
}

static bool send_headers(struct ffmpeg_muxer *stream)
{
	obs_encoder_t *aencoder;
	size_t idx = 0;

	if (!send_video_headers(stream))
		return false;

	do {
		aencoder = obs_output_get_audio_encoder(stream->output, idx);
		if (aencoder) {
			if (!send_audio_headers(stream, aencoder, idx)) {
				return false;
			}
			idx++;
		}
	} while (aencoder);

	return true;
}

static void srt_data(void *data, struct encoder_packet *packet)
{
	struct ffmpeg_muxer *stream = data;

	if (!active(stream))
		return;

	if (!stream->sent_headers) {
		if (!send_headers(stream))
			return;

		stream->sent_headers = true;
	}

	if (stopping(stream)) {
		if (packet->sys_dts_usec >= stream->stop_ts) {
			deactivate(stream);
			return;
		}
	}

	write_packet(stream, packet);
}

static obs_properties_t *srt_properties(void *unused)
{
	UNUSED_PARAMETER(unused);

	obs_properties_t *props = obs_properties_create();

	obs_properties_add_text(props, "path",
			obs_module_text("URL"),
			OBS_TEXT_DEFAULT);
	return props;
}

static uint64_t srt_total_bytes(void *data)
{
	struct ffmpeg_muxer *stream = data;
	return stream->total_bytes;
}

struct obs_output_info srt_muxer = {
	.id             = "srt_output",
	.flags          = OBS_OUTPUT_AV |
	                  OBS_OUTPUT_ENCODED |
	                  OBS_OUTPUT_MULTI_TRACK,
	.encoded_video_codecs = "h264",
	.encoded_audio_codecs = "aac",
	.get_name       = srt_getname,
	.create         = srt_create,
	.destroy        = srt_destroy,
	.start          = srt_start,
	.stop           = srt_stop,
	.encoded_packet = srt_data,
	.get_total_bytes= srt_total_bytes,
	.get_properties = srt_properties
};
