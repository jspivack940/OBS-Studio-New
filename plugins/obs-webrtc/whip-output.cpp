#include <sstream>
#include <cstring>

#include <obs-module.h>
#include <util/curl/curl-helper.h>
#include <util/platform.h>
#include <util/base.h>

#include "whip-output.h"

#define do_log(level, output, format, ...)                      \
	blog(level, "[obs-webrtc] [whip_output: '%s'] " format, \
	     obs_output_get_name(output), ##__VA_ARGS__)

const char *msid = "obs-studio";

const uint32_t audio_ssrc = 5002;
const char *audio_cname = "audio";
const char *audio_mid = "0";
const uint32_t audio_clockrate = 48000;
const uint8_t audio_payload_type = 111;

const uint32_t video_ssrc = 5000;
const char *video_cname = "video";
const char *video_mid = "1";
const uint32_t video_clockrate = 90000;
const uint8_t video_payload_type = 96;

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

WHIPOutput::WHIPOutput(obs_data_t *, obs_output_t *output)
	: output(output),
	  endpoint_url(),
	  bearer_token(),
	  resource_url(),
	  running(false),
	  start_stop_mutex(),
	  start_stop_thread(),
	  peer_connection(-1),
	  total_bytes_sent(0),
	  audio_track(-1),
	  video_track(-1),
	  last_audio_timestamp(0),
	  last_video_timestamp(0)
{
}

WHIPOutput::~WHIPOutput()
{
	Stop();

	{
		std::lock_guard<std::mutex> l(start_stop_mutex);
		if (start_stop_thread.joinable())
			start_stop_thread.join();
	}
}

bool WHIPOutput::Start()
{
	std::lock_guard<std::mutex> l(start_stop_mutex);

	if (!obs_output_can_begin_data_capture(output, 0))
		return false;

	if (!obs_output_initialize_encoders(output, 0))
		return false;

	if (start_stop_thread.joinable())
		start_stop_thread.join();

	start_stop_thread = std::thread(&WHIPOutput::StartThread, this);

	return true;
}

void WHIPOutput::Stop(uint64_t)
{
	if (!running)
		return;

	std::lock_guard<std::mutex> l(start_stop_mutex);
	if (start_stop_thread.joinable())
		start_stop_thread.join();

	start_stop_thread = std::thread(&WHIPOutput::StopThread, this);
}

void WHIPOutput::Data(struct encoder_packet *packet)
{
	if (!packet) {
		obs_output_signal_stop(output, OBS_OUTPUT_ENCODE_ERROR);
		return;
	}

	if (packet->type == OBS_ENCODER_AUDIO) {
		int64_t duration = packet->dts_usec - last_audio_timestamp;
		Send(packet->data, packet->size, duration, audio_track);
		last_audio_timestamp = packet->dts_usec;
	} else if (packet->type == OBS_ENCODER_VIDEO) {
		int64_t duration = packet->dts_usec - last_video_timestamp;
		Send(packet->data, packet->size, duration, video_track);
		last_video_timestamp = packet->dts_usec;
	}
}

uint64_t WHIPOutput::TotalBytes()
{
	return total_bytes_sent;
}

void WHIPOutput::ConfigureAudioTrack()
{
	rtcTrackInit track_init = {
		RTC_DIRECTION_SENDONLY,
		RTC_CODEC_OPUS,
		audio_payload_type,
		audio_ssrc,
		audio_mid,
		NULL,
		msid,
		NULL,
	};

	rtcPacketizationHandlerInit packetizer_init = {audio_ssrc,
						       audio_cname,
						       audio_payload_type,
						       audio_clockrate,
						       0,
						       0,
						       RTC_NAL_SEPARATOR_LENGTH,
						       0};

	audio_track = rtcAddTrackEx(peer_connection, &track_init);
	rtcSetOpusPacketizationHandler(audio_track, &packetizer_init);
	rtcChainRtcpSrReporter(audio_track);
	rtcChainRtcpNackResponder(audio_track, 1000);
}

void WHIPOutput::ConfigureVideoTrack()
{
	rtcTrackInit track_init = {
		RTC_DIRECTION_SENDONLY,
		RTC_CODEC_H264,
		video_payload_type,
		video_ssrc,
		video_mid,
		NULL,
		msid,
		NULL,
	};

	rtcPacketizationHandlerInit packetizer_init = {
		video_ssrc,
		video_cname,
		video_payload_type,
		video_clockrate,
		0,
		0,
		RTC_NAL_SEPARATOR_START_SEQUENCE,
		0};

	video_track = rtcAddTrackEx(peer_connection, &track_init);
	rtcSetH264PacketizationHandler(video_track, &packetizer_init);
	rtcChainRtcpSrReporter(video_track);
	rtcChainRtcpNackResponder(video_track, 1000);
}

bool WHIPOutput::Setup()
{
	rtcConfiguration config;
	memset(&config, 0, sizeof(config));

	peer_connection = rtcCreatePeerConnection(&config);
	rtcSetUserPointer(peer_connection, this);

	rtcSetStateChangeCallback(peer_connection, [](int, rtcState state,
						      void *ptr) {
		auto whipOutput = static_cast<WHIPOutput *>(ptr);
		switch (state) {
		case RTC_NEW:
			do_log(LOG_INFO, whipOutput->output,
			       "PeerConnection state is now: New");
			break;
		case RTC_CONNECTING:
			do_log(LOG_INFO, whipOutput->output,
			       "PeerConnection state is now: Connecting");
			break;
		case RTC_CONNECTED:
			do_log(LOG_INFO, whipOutput->output,
			       "PeerConnection state is now: Connected");
			break;
		case RTC_DISCONNECTED:
			do_log(LOG_INFO, whipOutput->output,
			       "PeerConnection state is now: Disconnected");
			obs_output_signal_stop(whipOutput->output,
					       OBS_OUTPUT_DISCONNECTED);
			break;
		case RTC_FAILED:
			do_log(LOG_INFO, whipOutput->output,
			       "PeerConnection state is now: Failed");
			obs_output_signal_stop(whipOutput->output,
					       OBS_OUTPUT_ERROR);
			break;
		case RTC_CLOSED:
			do_log(LOG_INFO, whipOutput->output,
			       "PeerConnection state is now: Closed");
			break;
		}
	});

	ConfigureAudioTrack();
	ConfigureVideoTrack();

	rtcSetLocalDescription(peer_connection, "offer");

	return true;
}

bool WHIPOutput::Connect()
{
	obs_service_t *service = obs_output_get_service(output);
	if (!service) {
		obs_output_signal_stop(output, OBS_OUTPUT_ERROR);
		return false;
	}

	OBSDataAutoRelease service_settings = obs_service_get_settings(service);
	if (!service_settings) {
		obs_output_signal_stop(output, OBS_OUTPUT_ERROR);
		return false;
	}

	endpoint_url = obs_service_get_connect_info(
		service, OBS_SERVICE_CONNECT_INFO_SERVER_URL);
	bearer_token = obs_service_get_connect_info(
		service, OBS_SERVICE_CONNECT_INFO_BEARER_TOKEN);

	struct curl_slist *headers = NULL;
	headers = curl_slist_append(headers, "Content-Type: application/sdp");
	if (!bearer_token.empty()) {
		auto bearer_token_header =
			std::string("Authorization: Bearer ") + bearer_token;
		headers =
			curl_slist_append(headers, bearer_token_header.c_str());
	}

	std::string read_buffer;
	std::string location_header;
	char offer_sdp[4096] = {0};
	rtcGetLocalDescription(peer_connection, offer_sdp, sizeof(offer_sdp));

	CURL *c = curl_easy_init();
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_writefunction);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, (void *)&read_buffer);
	curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, curl_headerfunction);
	curl_easy_setopt(c, CURLOPT_HEADERDATA, (void *)&location_header);
	curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(c, CURLOPT_URL, endpoint_url.c_str());
	curl_easy_setopt(c, CURLOPT_POST, 1L);
	curl_easy_setopt(c, CURLOPT_COPYPOSTFIELDS, offer_sdp);
	curl_easy_setopt(c, CURLOPT_TIMEOUT, 8L);

	auto cleanup = [&]() {
		curl_easy_cleanup(c);
		curl_slist_free_all(headers);
	};

	CURLcode res = curl_easy_perform(c);
	if (res != CURLE_OK) {
		do_log(LOG_WARNING, output,
		       "Connect failed: CURL returned result not CURLE_OK");
		cleanup();
		obs_output_signal_stop(output, OBS_OUTPUT_CONNECT_FAILED);
		return false;
	}

	long response_code;
	curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &response_code);
	if (response_code != 201) {
		do_log(LOG_WARNING, output,
		       "Connect failed: HTTP endpoint returned response code %ld",
		       response_code);
		cleanup();
		obs_output_signal_stop(output, OBS_OUTPUT_INVALID_STREAM);
		return false;
	}

	if (read_buffer.empty()) {
		do_log(LOG_WARNING, output,
		       "Connect failed: No data returned from HTTP endpoint request");
		cleanup();
		obs_output_signal_stop(output, OBS_OUTPUT_CONNECT_FAILED);
		return false;
	}

	if (location_header.empty()) {
		do_log(LOG_WARNING, output,

		       "WHIP server did not provide a resource URL via the Location header");
	} else {
		CURLU *h = curl_url();
		curl_url_set(h, CURLUPART_URL, endpoint_url.c_str(), 0);
		curl_url_set(h, CURLUPART_URL, location_header.c_str(), 0);
		char *url = nullptr;
		CURLUcode rc = curl_url_get(h, CURLUPART_URL, &url,
					    CURLU_NO_DEFAULT_PORT);
		if (!rc) {
			resource_url = url;
			curl_free(url);
			do_log(LOG_DEBUG, output, "WHIP Resource URL is: %s",
			       resource_url.c_str());
		} else {
			do_log(LOG_WARNING, output,
			       "Unable to process resource URL response");
		}
		curl_url_cleanup(h);
	}

	rtcSetRemoteDescription(peer_connection, read_buffer.c_str(), "answer");
	cleanup();
	return true;
}

void WHIPOutput::StartThread()
{
	if (!Setup())
		return;

	if (!Connect()) {
		rtcDeletePeerConnection(peer_connection);
		peer_connection = -1;
		return;
	}

	obs_output_begin_data_capture(output, 0);
	running = true;
}

void WHIPOutput::SendDelete()
{
	if (resource_url.empty()) {
		do_log(LOG_DEBUG, output,
		       "No resource URL available, not sending DELETE");
		return;
	}

	struct curl_slist *headers = NULL;
	if (!bearer_token.empty()) {
		auto bearer_token_header =
			std::string("Authorization: Bearer ") + bearer_token;
		headers =
			curl_slist_append(headers, bearer_token_header.c_str());
	}

	CURL *c = curl_easy_init();
	curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(c, CURLOPT_URL, resource_url.c_str());
	curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, "DELETE");
	curl_easy_setopt(c, CURLOPT_TIMEOUT, 8L);

	auto cleanup = [&]() {
		curl_easy_cleanup(c);
		curl_slist_free_all(headers);
	};

	CURLcode res = curl_easy_perform(c);
	if (res != CURLE_OK) {
		do_log(LOG_WARNING, output,
		       "DELETE request for resource URL failed. Reason: %s",
		       curl_easy_strerror(res));
		cleanup();
		return;
	}

	long response_code;
	curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &response_code);
	if (response_code != 200) {
		do_log(LOG_WARNING, output,
		       "DELETE request for resource URL failed. HTTP Code: %ld",
		       response_code);
		cleanup();
		return;
	}

	do_log(LOG_DEBUG, output,
	       "Successfully performed DELETE request for resource URL");
	cleanup();
}

void WHIPOutput::StopThread()
{
	if (peer_connection) {
		rtcDeletePeerConnection(peer_connection);
		peer_connection = -1;
	}

	SendDelete();

	if (running) {
		obs_output_signal_stop(output, OBS_OUTPUT_SUCCESS);
		running = false;
	}

	total_bytes_sent = 0;
}

void WHIPOutput::Send(void *data, uintptr_t size, uint64_t duration, int track)
{
	// sample time is in us, we need to convert it to seconds
	auto elapsed_seconds = double(duration) / (1000.0 * 1000.0);

	// get elapsed time in clock rate
	uint32_t elapsed_timestamp = 0;
	rtcTransformSecondsToTimestamp(track, elapsed_seconds,
				       &elapsed_timestamp);

	// set new timestamp
	uint32_t current_timestamp = 0;
	rtcGetCurrentTrackTimestamp(track, &current_timestamp);
	rtcSetTrackRtpTimestamp(track, current_timestamp + elapsed_timestamp);

	total_bytes_sent += size;
	rtcSendMessage(track, reinterpret_cast<const char *>(data), (int) size);
}

void register_whip_output()
{
	struct obs_output_info info = {};

	// TODO: dropped frames, congestion, connect time

	info.id = "whip_output";
	info.flags = OBS_OUTPUT_AV | OBS_OUTPUT_ENCODED | OBS_OUTPUT_SERVICE;
	info.get_name = [](void *) -> const char * {
		return obs_module_text("Output.Name");
	};
	info.create = [](obs_data_t *settings, obs_output_t *output) -> void * {
		return new WHIPOutput(settings, output);
	};
	info.destroy = [](void *priv_data) {
		delete static_cast<WHIPOutput *>(priv_data);
	};
	info.start = [](void *priv_data) -> bool {
		return static_cast<WHIPOutput *>(priv_data)->Start();
	};
	info.stop = [](void *priv_data, uint64_t ts) {
		static_cast<WHIPOutput *>(priv_data)->Stop(ts);
	};
	info.encoded_packet = [](void *priv_data,
				 struct encoder_packet *packet) {
		static_cast<WHIPOutput *>(priv_data)->Data(packet);
	};
	info.get_defaults = [](obs_data_t *) {};
	info.get_properties = [](void *) -> obs_properties_t * {
		return obs_properties_create();
	};
	info.get_total_bytes = [](void *priv_data) -> uint64_t {
		return static_cast<WHIPOutput *>(priv_data)->TotalBytes();
	};
	info.encoded_video_codecs = "h264";
	info.encoded_audio_codecs = "opus";
	info.protocols = "WHIP";
	obs_register_output(&info);
}
