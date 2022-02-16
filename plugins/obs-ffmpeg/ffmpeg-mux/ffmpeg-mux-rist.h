/*
 * The following code is a port of FFmpeg/avformat/librist.c for obs-studio.
 * Port by pkv@obsproject.com.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#pragma once
#include "ffmpeg-mux-url.h"
#include <librist/librist.h>
#include <librist/version.h>

// RIST_MAX_PACKET_SIZE - 28 minimum protocol overhead
#define RIST_MAX_PAYLOAD_SIZE (10000 - 28)

#define FF_LIBRIST_MAKE_VERSION(major, minor, patch) \
	((patch) + ((minor)*0x100) + ((major)*0x10000))
#define FF_LIBRIST_VERSION                                 \
	FF_LIBRIST_MAKE_VERSION(LIBRIST_API_VERSION_MAJOR, \
				LIBRIST_API_VERSION_MINOR, \
				LIBRIST_API_VERSION_PATCH)
#define FF_LIBRIST_VERSION_41 FF_LIBRIST_MAKE_VERSION(4, 1, 0)
#define FF_LIBRIST_VERSION_42 FF_LIBRIST_MAKE_VERSION(4, 2, 0)

#define FF_LIBRIST_FIFO_DEFAULT_SHIFT 13

typedef struct RISTContext {
	int profile;
	int buffer_size;
	int packet_size;
	int log_level;
	int encryption;
	int fifo_shift;
	bool overrun_nonfatal;
	char *secret;

	struct rist_logging_settings logging_settings;
	struct rist_peer_config peer_config;

	struct rist_peer *peer;
	struct rist_ctx *ctx;
} RISTContext;

static int risterr2ret(int err)
{
	switch (err) {
	case RIST_ERR_MALLOC:
		return AVERROR(ENOMEM);
	default:
		return AVERROR_EXTERNAL;
	}
}

static int log_cb(void *arg, enum rist_log_level log_level, const char *msg)
{
	int level;

	switch (log_level) {
	case RIST_LOG_ERROR:
		level = AV_LOG_ERROR;
		break;
	case RIST_LOG_WARN:
		level = AV_LOG_WARNING;
		break;
	case RIST_LOG_NOTICE:
		level = AV_LOG_INFO;
		break;
	case RIST_LOG_INFO:
		level = AV_LOG_VERBOSE;
		break;
	case RIST_LOG_DEBUG:
		level = AV_LOG_DEBUG;
		break;
	case RIST_LOG_DISABLE:
		level = AV_LOG_QUIET;
		break;
	default:
		level = AV_LOG_WARNING;
	}

	av_log(arg, level, "%s", msg);

	return 0;
}

static int librist_close(URLContext *h)
{
	RISTContext *s = h->priv_data;
	int ret = 0;

	s->peer = NULL;

	if (s->ctx)
		ret = rist_destroy(s->ctx);
	s->ctx = NULL;

	return risterr2ret(ret);
}

static int librist_open(URLContext *h, const char *uri)
{
	RISTContext *s = h->priv_data;
	struct rist_logging_settings *logging_settings = &s->logging_settings;
	struct rist_peer_config *peer_config = &s->peer_config;
	int ret;
	s->buffer_size = 3000;
	s->profile = RIST_PROFILE_MAIN;
	s->packet_size = 1316;
	s->log_level = RIST_LOG_INFO;
	s->encryption = 0;
	s->secret = NULL;
	s->overrun_nonfatal = 0;
	s->fifo_shift = FF_LIBRIST_FIFO_DEFAULT_SHIFT;
	s->logging_settings =
		(struct rist_logging_settings)LOGGING_SETTINGS_INITIALIZER;
	ret = rist_logging_set(&logging_settings, s->log_level, log_cb, h, NULL,
			       NULL);
	if (ret < 0)
		return risterr2ret(ret);

	h->max_packet_size = s->packet_size;
	ret = rist_sender_create(&s->ctx, s->profile, 0, logging_settings);

	if (ret < 0)
		goto err;

	ret = rist_peer_config_defaults_set(peer_config);
	if (ret < 0)
		goto err;

#if FF_LIBRIST_VERSION < FF_LIBRIST_VERSION_41
	ret = rist_parse_address(
		uri, (const struct rist_peer_config **)&peer_config);
#else
	ret = rist_parse_address2(uri, &peer_config);
#endif
	if (ret < 0)
		goto err;

	if (((s->encryption == 128 || s->encryption == 256) && !s->secret) ||
	    ((peer_config->key_size == 128 || peer_config->key_size == 256) &&
	     !peer_config->secret[0])) {
		av_log(h, AV_LOG_ERROR,
		       "secret is mandatory if encryption is enabled\n");
		librist_close(h);
		return AVERROR(EINVAL);
	}

	if (s->secret && peer_config->secret[0] == 0)
		av_strlcpy(peer_config->secret, s->secret,
			   RIST_MAX_STRING_SHORT);

	if (s->secret && (s->encryption == 128 || s->encryption == 256))
		peer_config->key_size = s->encryption;

	if (s->buffer_size) {
		peer_config->recovery_length_min = s->buffer_size;
		peer_config->recovery_length_max = s->buffer_size;
	}

	ret = rist_peer_create(s->ctx, &s->peer, &s->peer_config);
	if (ret < 0)
		goto err;

	ret = rist_start(s->ctx);
	if (ret < 0)
		goto err;

	return 0;

err:
	librist_close(h);

	return risterr2ret(ret);
}

static int librist_write(URLContext *h, const uint8_t *buf, int size)
{
	RISTContext *s = h->priv_data;
	struct rist_data_block data_block = {0};
	int ret;

	data_block.ts_ntp = 0;
	data_block.payload = buf;
	data_block.payload_len = size;

	ret = rist_sender_data_write(s->ctx, &data_block);
	if (ret < 0)
		return risterr2ret(ret);

	return ret;
}
