/******************************************************************************
    Copyright (C) 2023 by Colin, Sean, tt, kc5rna, pkv
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

#include "whip-output.h"

static const char *whip_output_getname(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("Output.Name");
}

void whip_output_send_delete(void *data)
{
	struct whip_output *stream = data;
	CURLcode res;

	if (!stream->resource_url) {
		do_log(LOG_DEBUG, stream->output,
		       "No resource URL available, not sending DELETE");
		return;
	}

	struct curl_slist *headers = NULL;
	if (stream->bearer_token) {
		struct dstr bearer_token_header = {0};
		dstr_copy(&bearer_token_header, "Authorization: Bearer ");
		dstr_cat(&bearer_token_header, stream->bearer_token);
		headers = curl_slist_append(headers, bearer_token_header.array);
		dstr_free(&bearer_token_header);
	}

	CURL *c = curl_easy_init();
	curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(c, CURLOPT_URL, stream->resource_url);
	curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, "DELETE");
	curl_easy_setopt(c, CURLOPT_TIMEOUT, 8L);

	res = curl_easy_perform(c);
	if (res != CURLE_OK) {
		do_log(LOG_WARNING, stream->output,
		       "DELETE request for resource URL failed. Reason: %s",
		       curl_easy_strerror(res));
		curl_easy_cleanup(c);
		curl_slist_free_all(headers);
		return;
	}

	long response_code;
	curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &response_code);
	if (response_code != 200) {
		do_log(LOG_WARNING, stream->output,
		       "DELETE request for resource URL failed. HTTP Code: %ld",
		       response_code);
		curl_easy_cleanup(c);
		curl_slist_free_all(headers);
		return;
	}

	do_log(LOG_DEBUG, stream->output,
	       "Successfully performed DELETE request for resource URL");

	curl_easy_cleanup(c);
	curl_slist_free_all(headers);
}

static void whip_output_stop(void *data, uint64_t ts)
{
	UNUSED_PARAMETER(ts);
	struct whip_output *stream = data;

	if (!stream->running)
		return;
	pthread_join(stream->start_stop_thread, NULL);
	stream->start_stop_thread_active = false;
	if (stream->peer_connection != -1) {
		rtcDeletePeerConnection(stream->peer_connection);
		stream->peer_connection = -1;
		stream->audio_track = -1;
		stream->video_track = -1;
	}

	whip_output_send_delete(stream);
	// "stop_signal" exists because we have to preserve the "running" state
	// across reconnect attempts. If we don't emit a signal if
	// something calls obs_output_stop() and it's reconnecting, you'll
	// desync the UI, as the output will be "stopped" and not
	// "reconnecting", but the "stop" signal will have never been
	// emitted.
	if (stream->running && stream->stop_signal) {
		obs_output_signal_stop(stream->output, OBS_OUTPUT_SUCCESS);
		os_atomic_set_bool(&stream->running, false);
	}

	stream->total_bytes_sent = 0;
	stream->connect_time_ms = 0;
	stream->start_time_ns = 0;
	stream->last_audio_timestamp = 0;
	stream->last_video_timestamp = 0;
}

static void whip_output_destroy(void *data)
{
	struct whip_output *stream = data;
	whip_output_stop(stream, 0);
	pthread_mutex_destroy(&stream->start_stop_mutex);
	bfree(stream->resource_url);
	bfree(stream);
}

static void *whip_output_create(obs_data_t *settings, obs_output_t *output)
{
	struct whip_output *stream = bzalloc(sizeof(struct whip_output));
	stream->output = output;

	stream->audio_track = -1;
	stream->video_track = -1;
	stream->last_audio_timestamp = -1;
	stream->last_video_timestamp = -1;
	stream->total_bytes_sent = 0;
	stream->peer_connection = -1;
	stream->connect_time_ms = 0;
	stream->resource_url = NULL;
	stream->endpoint_url = NULL;
	stream->connect_time_ms = 0;
	stream->start_time_ns = 0;
	stream->start_stop_thread_active = false;

	os_atomic_set_bool(&stream->running, false);
	os_atomic_set_bool(&stream->stop_signal, true);
	if (pthread_mutex_init(&stream->start_stop_mutex, NULL) != 0)
		goto fail;
	return stream;

fail:
	whip_output_destroy(stream);
	return NULL;
}

void whip_output_configure_audio_track(void *data)
{
	struct whip_output *stream = data;
	rtcTrackInit track_init = {
		.direction = RTC_DIRECTION_SENDONLY,
		.codec = RTC_CODEC_OPUS,
		.payloadType = 111,
		.ssrc = 5002,
		.mid = 0,
		.name = "audio",
		.msid = "obs-studio",
		.trackId = NULL,
	};

	rtcPacketizationHandlerInit packetizer_init = {
		.ssrc = 5002,
		.cname = "audio",
		.payloadType = 111,
		.clockRate = 48000,
		.sequenceNumber = 0,
		.timestamp = 0,
		.nalSeparator = RTC_NAL_SEPARATOR_LENGTH,
		.maxFragmentSize = 0,
	};

	stream->audio_track =
		rtcAddTrackEx(stream->peer_connection, &track_init);
	rtcSetOpusPacketizationHandler(stream->audio_track, &packetizer_init);
	rtcChainRtcpSrReporter(stream->audio_track);
	rtcChainRtcpNackResponder(stream->audio_track, 1000);
}

void whip_output_configure_video_track(void *data)
{
	struct whip_output *stream = data;

	rtcTrackInit track_init = {
		.direction = RTC_DIRECTION_SENDONLY,
		.codec = RTC_CODEC_H264,
		.payloadType = 96,
		.ssrc = 5000,
		.mid = "1",
		.name = "video",
		.msid = "obs-studio",
		.trackId = NULL,
	};

	rtcPacketizationHandlerInit packetizer_init = {
		.ssrc = 5000,
		.cname = "video",
		.payloadType = 96,
		.clockRate = 90000,
		.sequenceNumber = 0,
		.timestamp = 0,
		.nalSeparator = RTC_NAL_SEPARATOR_START_SEQUENCE,
		.maxFragmentSize = 0,
	};
	stream->video_track =
		rtcAddTrackEx(stream->peer_connection, &track_init);
	rtcSetH264PacketizationHandler(stream->video_track, &packetizer_init);
	rtcChainRtcpSrReporter(stream->video_track);
	rtcChainRtcpNackResponder(stream->video_track, 1000);
}

void rtcCB(int pc, rtcState state, void *data)
{
	UNUSED_PARAMETER(pc);

	struct whip_output *stream = data;
	switch (state) {
	case RTC_NEW:
		do_log(LOG_INFO, stream->output,
		       "PeerConnection state is now: New");
		break;
	case RTC_CONNECTING:
		do_log(LOG_INFO, stream->output,
		       "PeerConnection state is now: Connecting");
		stream->start_time_ns = os_gettime_ns();
		break;
	case RTC_CONNECTED:
		do_log(LOG_INFO, stream->output,
		       "PeerConnection state is now: Connected");
		stream->connect_time_ms =
			(int)((os_gettime_ns() - stream->start_time_ns) /
			      1000000.0);
		do_log(LOG_INFO, stream->output, "Connect time: %f sec",
		       (float)stream->connect_time_ms / 1000.0f);
		break;
	case RTC_DISCONNECTED:
		do_log(LOG_INFO, stream->output,
		       "PeerConnection state is now: Disconnected");
		stream->stop_signal = false;
		obs_output_signal_stop(stream->output, OBS_OUTPUT_DISCONNECTED);
		break;
	case RTC_FAILED:
		do_log(LOG_INFO, stream->output,
		       "PeerConnection state is now: Failed");
		stream->stop_signal = false;
		obs_output_signal_stop(stream->output, OBS_OUTPUT_ERROR);
		break;
	case RTC_CLOSED:
		do_log(LOG_INFO, stream->output,
		       "PeerConnection state is now: Closed");
		break;
	}
}

bool whip_output_setup(void *data)
{
	struct whip_output *stream = data;
	obs_service_t *service = obs_output_get_service(stream->output);
	if (!service) {
		obs_output_signal_stop(stream->output, OBS_OUTPUT_ERROR);
		return false;
	}

	stream->endpoint_url = obs_service_get_connect_info(
		service, OBS_SERVICE_CONNECT_INFO_SERVER_URL);
	stream->bearer_token = obs_service_get_connect_info(
		service, OBS_SERVICE_CONNECT_INFO_BEARER_TOKEN);
	if (!stream->endpoint_url || !stream->endpoint_url[0]) {
		obs_output_signal_stop(stream->output, OBS_OUTPUT_BAD_PATH);
		return false;
	}

	rtcConfiguration config;
	memset(&config, 0, sizeof(config));

	stream->peer_connection = rtcCreatePeerConnection(&config);
	rtcSetUserPointer(stream->peer_connection, stream);

	rtcSetStateChangeCallback(stream->peer_connection, *rtcCB);

	whip_output_configure_audio_track(stream);
	whip_output_configure_video_track(stream);

	rtcSetLocalDescription(stream->peer_connection, "offer");

	return true;
}

bool whip_output_connect(void *data)
{
	struct whip_output *stream = data;

	struct curl_slist *headers = NULL;
	headers = curl_slist_append(headers, "Content-Type: application/sdp");
	if (stream->bearer_token[0] && stream->bearer_token) {
		struct dstr bearer_token_header = {0};
		dstr_copy(&bearer_token_header, "Authorization: Bearer ");
		dstr_cat(&bearer_token_header, stream->bearer_token);
		headers = curl_slist_append(headers, bearer_token_header.array);
		dstr_free(&bearer_token_header);
	}

	struct dstr read_buffer = {0};
	struct dstr location_header = {0};
	char offer_sdp[4096] = {0};
	rtcGetLocalDescription(stream->peer_connection, offer_sdp,
			       sizeof(offer_sdp));

	CURL *c = curl_easy_init();
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_writefunction);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, (void *)&read_buffer);
	curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, curl_headerfunction);
	curl_easy_setopt(c, CURLOPT_HEADERDATA, (void *)&location_header);
	curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(c, CURLOPT_URL, stream->endpoint_url);
	curl_easy_setopt(c, CURLOPT_POST, 1L);
	curl_easy_setopt(c, CURLOPT_COPYPOSTFIELDS, offer_sdp);
	curl_easy_setopt(c, CURLOPT_TIMEOUT, 8L);

	CURLcode res = curl_easy_perform(c);
	if (res != CURLE_OK) {
		do_log(LOG_WARNING, stream->output,
		       "Connect failed: CURL returned result not CURLE_OK");
		curl_easy_cleanup(c);
		curl_slist_free_all(headers);
		obs_output_signal_stop(stream->output,
				       OBS_OUTPUT_CONNECT_FAILED);
		return false;
	}

	long response_code;
	curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &response_code);
	if (response_code != 201) {
		do_log(LOG_WARNING, stream->output,
		       "Connect failed: HTTP endpoint returned response code %ld",
		       response_code);
		curl_easy_cleanup(c);
		curl_slist_free_all(headers);
		obs_output_signal_stop(stream->output,
				       OBS_OUTPUT_INVALID_STREAM);
		return false;
	}

	if (dstr_is_empty(&read_buffer)) {
		do_log(LOG_WARNING, stream->output,
		       "Connect failed: No data returned from HTTP endpoint request");
		curl_easy_cleanup(c);
		curl_slist_free_all(headers);
		obs_output_signal_stop(stream->output,
				       OBS_OUTPUT_CONNECT_FAILED);
		return false;
	}

	if (dstr_is_empty(&location_header)) {
		do_log(LOG_WARNING, stream->output,
		       "WHIP server did not provide a resource URL via the Location header");
	} else {
		CURLU *h = curl_url();
		curl_url_set(h, CURLUPART_URL, stream->endpoint_url, 0);
		curl_url_set(h, CURLUPART_URL, location_header.array, 0);
		char *url = NULL;
		CURLUcode rc = curl_url_get(h, CURLUPART_URL, &url,
					    CURLU_NO_DEFAULT_PORT);
		if (!rc) {
			stream->resource_url = bstrdup(url);
			curl_free(url);
			do_log(LOG_DEBUG, stream->output,
			       "WHIP Resource URL is: %s",
			       stream->resource_url);
		} else {
			do_log(LOG_WARNING, stream->output,
			       "Unable to process resource URL response");
		}
		curl_url_cleanup(h);
	}

	rtcSetRemoteDescription(stream->peer_connection, read_buffer.array,
				"answer");
	curl_easy_cleanup(c);
	curl_slist_free_all(headers);

	dstr_free(&read_buffer);
	dstr_free(&location_header);
	return true;
}

static void *start_stop_thread(void *data)
{
	struct whip_output *stream = data;
	if (!whip_output_setup(stream))
		return 0;

	if (!whip_output_connect(stream)) {
		rtcDeletePeerConnection(stream->peer_connection);
		stream->peer_connection = -1;
		stream->audio_track = -1;
		stream->video_track = -1;
		return 0;
	}

	obs_output_begin_data_capture(stream->output, 0);
	stream->running = true;
	stream->start_stop_thread_active = true;
	return 0;
}

static bool whip_output_start(void *data)
{
	struct whip_output *stream = data;

	if (!obs_output_can_begin_data_capture(stream->output, 0))
		return false;
	if (!obs_output_initialize_encoders(stream->output, 0))
		return false;
	if (stream->start_stop_thread_active)
		pthread_join(stream->start_stop_thread, NULL);

	return pthread_create(&stream->start_stop_thread, NULL,
			      start_stop_thread, stream) == 0;
}

uint64_t whip_output_total_bytes_sent(void *data)
{
	struct whip_output *stream = data;
	return stream->total_bytes_sent;
}

void whip_output_send(void *priv_data, void *data, uintptr_t size,
		      uint64_t duration, int track)
{
	UNUSED_PARAMETER(track);
	struct whip_output *stream = priv_data;

	// sample time is in us, we need to convert it to seconds
	double elapsed_seconds = (double)duration / (1000000.0);

	// get elapsed time in clock rate
	uint32_t elapsed_timestamp = 0;
	rtcTransformSecondsToTimestamp(track, elapsed_seconds,
				       &elapsed_timestamp);

	// set new timestamp
	uint32_t current_timestamp = 0;
	rtcGetCurrentTrackTimestamp(track, &current_timestamp);
	rtcSetTrackRtpTimestamp(track, current_timestamp + elapsed_timestamp);

	stream->total_bytes_sent += size;

	rtcSendMessage(track, (const char *)(data), (int)size);
}

void whip_output_data(void *data, struct encoder_packet *packet)
{
	struct whip_output *stream = data;
	if (!packet) {
		stream->stop_signal = false;
		obs_output_signal_stop(stream->output, OBS_OUTPUT_ENCODE_ERROR);
		return;
	}

	if (packet->type == OBS_ENCODER_AUDIO) {
		int64_t duration =
			packet->dts_usec - stream->last_audio_timestamp;
		whip_output_send(stream, packet->data, packet->size, duration,
				 stream->audio_track);
		stream->last_audio_timestamp = packet->dts_usec;
	} else if (packet->type == OBS_ENCODER_VIDEO) {
		int64_t duration =
			packet->dts_usec - stream->last_video_timestamp;
		whip_output_send(stream, packet->data, packet->size, duration,
				 stream->video_track);
		stream->last_video_timestamp = packet->dts_usec;
	}
}

static void whip_output_defaults(obs_data_t *defaults) {}

static obs_properties_t *whip_output_properties(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_properties_create();
}

static float whip_output_congestion(void *unused)
{
	UNUSED_PARAMETER(unused);
	return 0.0f;
}

static int whip_output_connect_time(void *data)
{
	struct whip_output *stream = data;
	return (int)stream->connect_time_ms;
}

static int whip_output_dropped_frames(void *unused)
{
	UNUSED_PARAMETER(unused);
	return 0;
}

struct obs_output_info whip_output_info = {
	.id = "whip_output",
	.flags = OBS_OUTPUT_AV | OBS_OUTPUT_ENCODED | OBS_OUTPUT_SERVICE,
	.protocols = "WHIP",
	.encoded_video_codecs = "h264",
	.encoded_audio_codecs = "opus",
	.get_name = whip_output_getname,
	.create = whip_output_create,
	.destroy = whip_output_destroy,
	.start = whip_output_start,
	.stop = whip_output_stop,
	.encoded_packet = whip_output_data,
	.get_defaults = whip_output_defaults,
	.get_properties = whip_output_properties,
	.get_total_bytes = whip_output_total_bytes_sent,
	.get_congestion = whip_output_congestion,
	.get_connect_time_ms = whip_output_connect_time,
	.get_dropped_frames = whip_output_dropped_frames,
};
