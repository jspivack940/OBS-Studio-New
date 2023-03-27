#include <sstream>
#include <cstring>

#include <obs-module.h>
#include <util/curl/curl-helper.h>
#include <util/platform.h>
#include <util/base.h>

#include "whip-output.h"

#define do_log(level, format, ...)                              \
	blog(level, "[obs-webrtc] [whip_output: '%s'] " format, \
	     obs_output_get_name(output), ##__VA_ARGS__)

const rtc::string msid = "obs-studio";

const uint32_t audio_ssrc = 5002;
const rtc::string audio_cname = "audio";
const uint8_t audio_payload_type = 111;

const uint32_t video_ssrc = 5000;
const rtc::string video_cname = "video";
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
	  peer_connection(nullptr),
	  total_bytes_sent(0),
	  audio_track(nullptr),
	  video_track(nullptr),
	  audio_sr_reporter(nullptr),
	  video_sr_reporter(nullptr),
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
		Send(packet->data, packet->size, duration, audio_track,
		     audio_sr_reporter);
		last_audio_timestamp = packet->dts_usec;
	} else if (packet->type == OBS_ENCODER_VIDEO) {
		int64_t duration = packet->dts_usec - last_video_timestamp;
		Send(packet->data, packet->size, duration, video_track,
		     video_sr_reporter);
		last_video_timestamp = packet->dts_usec;
	}
}

uint64_t WHIPOutput::TotalBytes()
{
	return total_bytes_sent;
}

void WHIPOutput::ConfigureAudioTrack()
{
	rtc::Description::Audio audio_media(
		audio_cname, rtc::Description::Direction::SendOnly);
	audio_media.addOpusCodec(audio_payload_type);
	audio_media.addSSRC(audio_ssrc, audio_cname, msid, audio_cname);
	audio_track = peer_connection->addTrack(audio_media);

	auto rtp_config = std::make_shared<rtc::RtpPacketizationConfig>(
		audio_ssrc, audio_cname, audio_payload_type,
		rtc::OpusRtpPacketizer::defaultClockRate);
	auto packetizer = std::make_shared<rtc::OpusRtpPacketizer>(rtp_config);
	audio_sr_reporter = std::make_shared<rtc::RtcpSrReporter>(rtp_config);
	auto nack_responder = std::make_shared<rtc::RtcpNackResponder>();

	auto opus_handler =
		std::make_shared<rtc::OpusPacketizationHandler>(packetizer);
	opus_handler->addToChain(audio_sr_reporter);
	opus_handler->addToChain(nack_responder);
	audio_track->setMediaHandler(opus_handler);
}

void WHIPOutput::ConfigureVideoTrack()
{
	rtc::Description::Video video_media(
		video_cname, rtc::Description::Direction::SendOnly);
	video_media.addH264Codec(video_payload_type);
	video_media.addSSRC(video_ssrc, video_cname, msid, video_cname);
	video_track = peer_connection->addTrack(video_media);

	auto rtp_config = std::make_shared<rtc::RtpPacketizationConfig>(
		video_ssrc, video_cname, video_payload_type,
		rtc::H264RtpPacketizer::defaultClockRate);
	auto packetizer = std::make_shared<rtc::H264RtpPacketizer>(
		rtc::H264RtpPacketizer::Separator::StartSequence, rtp_config);
	video_sr_reporter = std::make_shared<rtc::RtcpSrReporter>(rtp_config);
	auto nack_responder = std::make_shared<rtc::RtcpNackResponder>();

	auto h264_handler =
		std::make_shared<rtc::H264PacketizationHandler>(packetizer);
	h264_handler->addToChain(video_sr_reporter);
	h264_handler->addToChain(nack_responder);
	video_track->setMediaHandler(h264_handler);
}

bool WHIPOutput::Setup()
{
	peer_connection = std::make_unique<rtc::PeerConnection>();
	peer_connection->onStateChange([&](rtc::PeerConnection::State state) {
		std::ostringstream state_stream;
		state_stream << state;
		do_log(LOG_INFO, "PeerConnection state is now: %s",
		       state_stream.str().c_str());
		switch (state) {
		case rtc::PeerConnection::State::Disconnected:
			obs_output_signal_stop(output, OBS_OUTPUT_DISCONNECTED);
			break;
		case rtc::PeerConnection::State::Failed:
			obs_output_signal_stop(output, OBS_OUTPUT_ERROR);
			break;
		default:
			break;
		}
	});

	ConfigureAudioTrack();
	ConfigureVideoTrack();

	peer_connection->setLocalDescription();

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

	endpoint_url = obs_service_get_url(service);
	bearer_token = obs_data_get_string(service_settings, "bearer_token");

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
	auto offer = peer_connection->localDescription();
	auto offer_sdp = std::string(offer->generateSdp());

	CURL *c = curl_easy_init();
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_writefunction);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, (void *)&read_buffer);
	curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, curl_headerfunction);
	curl_easy_setopt(c, CURLOPT_HEADERDATA, (void *)&location_header);
	curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(c, CURLOPT_URL, endpoint_url.c_str());
	curl_easy_setopt(c, CURLOPT_POST, 1L);
	curl_easy_setopt(c, CURLOPT_COPYPOSTFIELDS, offer_sdp.c_str());
	curl_easy_setopt(c, CURLOPT_TIMEOUT, 8L);

	auto cleanup = [&]() {
		curl_easy_cleanup(c);
		curl_slist_free_all(headers);
	};

	CURLcode res = curl_easy_perform(c);
	if (res != CURLE_OK) {
		do_log(LOG_WARNING,
		       "Connect failed: CURL returned result not CURLE_OK");
		cleanup();
		obs_output_signal_stop(output, OBS_OUTPUT_CONNECT_FAILED);
		return false;
	}

	long response_code;
	curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &response_code);
	if (response_code != 201) {
		do_log(LOG_WARNING,
		       "Connect failed: HTTP endpoint returned response code %ld",
		       response_code);
		cleanup();
		obs_output_signal_stop(output, OBS_OUTPUT_INVALID_STREAM);
		return false;
	}

	if (read_buffer.empty()) {
		do_log(LOG_WARNING,
		       "Connect failed: No data returned from HTTP endpoint request");
		cleanup();
		obs_output_signal_stop(output, OBS_OUTPUT_CONNECT_FAILED);
		return false;
	}

	if (location_header.empty()) {
		do_log(LOG_WARNING,
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
			do_log(LOG_DEBUG, "WHIP Resource URL is: %s",
			       resource_url.c_str());
		} else {
			do_log(LOG_WARNING,
			       "Unable to process resource URL response");
		}
		curl_url_cleanup(h);
	}

	rtc::Description answer(read_buffer, rtc::Description::Type::Answer);
	peer_connection->setRemoteDescription(answer);
	cleanup();
	return true;
}

void WHIPOutput::StartThread()
{
	if (!Setup())
		return;

	if (!Connect()) {
		peer_connection->close();
		peer_connection = nullptr;
		return;
	}

	obs_output_begin_data_capture(output, 0);
	running = true;
}

void WHIPOutput::SendDelete()
{
	if (resource_url.empty()) {
		do_log(LOG_DEBUG,
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
		do_log(LOG_WARNING,
		       "DELETE request for resource URL failed. Reason: %s",
		       curl_easy_strerror(res));
		cleanup();
		return;
	}

	long response_code;
	curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &response_code);
	if (response_code != 200) {
		do_log(LOG_WARNING,
		       "DELETE request for resource URL failed. HTTP Code: %ld",
		       response_code);
		cleanup();
		return;
	}

	do_log(LOG_DEBUG,
	       "Successfully performed DELETE request for resource URL");
	cleanup();
}

void WHIPOutput::StopThread()
{
	if (peer_connection) {
		peer_connection->close();
		peer_connection = nullptr;
	}

	SendDelete();

	if (running) {
		obs_output_signal_stop(output, OBS_OUTPUT_SUCCESS);
		running = false;
	}

	total_bytes_sent = 0;
}

void WHIPOutput::Send(void *data, uintptr_t size, uint64_t duration,
		      std::shared_ptr<rtc::Track> track,
		      std::shared_ptr<rtc::RtcpSrReporter> rtcp_sr_reporter)
{
	if (!track->isOpen())
		return;

	std::vector<rtc::byte> sample{(rtc::byte *)data,
				      (rtc::byte *)data + size};

	auto rtp_config = rtcp_sr_reporter->rtpConfig;

	// sample time is in us, we need to convert it to seconds
	auto elapsed_seconds = double(duration) / (1000.0 * 1000.0);

	// get elapsed time in clock rate
	uint32_t elapsed_timestamp =
		rtp_config->secondsToTimestamp(elapsed_seconds);

	// set new timestamp
	rtp_config->timestamp = rtp_config->timestamp + elapsed_timestamp;

	// get elapsed time in clock rate from last RTCP sender report
	auto report_elapsed_timestamp =
		rtp_config->timestamp -
		rtcp_sr_reporter->lastReportedTimestamp();

	// check if last report was at least 1 second ago
	if (rtp_config->timestampToSeconds(report_elapsed_timestamp) > 1)
		rtcp_sr_reporter->setNeedsToReport();

	try {
		track->send(sample);
		total_bytes_sent += sample.size();
	} catch (const std::exception &e) {
		// TODO: Emit output failure
	}
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

	obs_register_output(&info);
}
