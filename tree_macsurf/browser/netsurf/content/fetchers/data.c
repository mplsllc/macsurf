/*
 * Copyright 2008 Rob Kendrick <rjek@netsurf-browser.org>
 *
 * This file is part of NetSurf.
 *
 * NetSurf is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * NetSurf is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * \file
 * data scheme handling.  See http://tools.ietf.org/html/rfc2397
 */

#include <stdbool.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#include <libwapcaplet/libwapcaplet.h>
#include <nsutils/base64.h>

#include "netsurf/inttypes.h"
#include "utils/url.h"
#include "utils/nsurl.h"
#include "utils/corestrings.h"
#include "utils/log.h"
#include "utils/utils.h"
#include "utils/ring.h"

#include "content/fetch.h"
#include "content/fetchers.h"
#include "content/fetchers/data.h"

struct fetch_data_context {
	struct fetch *parent_fetch;
	nsurl *url;
	char *mimetype;
	char *data;
	size_t datalen;
	bool base64;

	bool aborted;
	bool locked;
	
	struct fetch_data_context *r_next, *r_prev;
};

static struct fetch_data_context *ring = NULL;

static bool fetch_data_initialise(lwc_string *scheme)
{
	NSLOG(netsurf, INFO, "fetch_data_initialise called for %s",
	      lwc_string_data(scheme));

	return true;
}

static void fetch_data_finalise(lwc_string *scheme)
{
	NSLOG(netsurf, INFO, "fetch_data_finalise called for %s",
	      lwc_string_data(scheme));
}

static bool fetch_data_can_fetch(const nsurl *url)
{
	return true;
}

static void fetch_data_send_callback(const fetch_msg *msg,
		struct fetch_data_context *c)
{
	c->locked = true;
	fetch_send_callback(msg, c->parent_fetch);
	c->locked = false;
}

static void fetch_data_send_header(struct fetch_data_context *ctx,
		const char *fmt, ...)
{
	char header[64];
	fetch_msg msg;
	va_list ap;
	int len;

	va_start(ap, fmt);
	len = vsnprintf(header, sizeof(header), fmt, ap);
	va_end(ap);

	if (len >= (int)sizeof(header) || len < 0) {
		return;
	}

	msg.type = FETCH_HEADER;
	msg.data.header_or_data.len = len;
	msg.data.header_or_data.buf = (const uint8_t *)header;
	fetch_data_send_callback(&msg, ctx);
}

static void *fetch_data_setup(struct fetch *parent_fetch, nsurl *url,
		 bool only_2xx, bool downgrade_tls, const char *post_urlenc,
		 const struct fetch_multipart_data *post_multipart,
		 const char **headers)
{
	struct fetch_data_context *ctx = calloc(1, sizeof(*ctx));
	
	if (ctx == NULL)
		return NULL;
		
	ctx->parent_fetch = parent_fetch;
	ctx->url = nsurl_ref(url);

	RING_INSERT(ring, ctx);
	
	return ctx;
}

static bool fetch_data_start(void *ctx)
{
	return true;
}

static void fetch_data_free(void *ctx)
{
	struct fetch_data_context *c = ctx;

	nsurl_unref(c->url);
	free(c->data);
	free(c->mimetype);
	free(ctx);
}

static void fetch_data_abort(void *ctx)
{
	struct fetch_data_context *c = ctx;

	/* To avoid the poll loop having to deal with the fetch context
	 * disappearing from under it, we simply flag the abort here. 
	 * The poll loop itself will perform the appropriate cleanup.
	 */
	c->aborted = true;
}

static bool fetch_data_process(struct fetch_data_context *c)
{
	nserror res;
	fetch_msg msg;
	const char *params;
	const char *comma;
	char *unescaped;
	size_t unescaped_len;
	
	/* format of a data: URL is:
	 *   data:[<mimetype>][;base64],<data>
	 * The mimetype is optional.  If it is missing, the , before the
	 * data must still be there.
	 */
	
	NSLOG(netsurf, DEEPDEBUG, "url: %.140s", nsurl_access(c->url));
	
	if (nsurl_length(c->url) < 6) {
		/* 6 is the minimum possible length (data:,) */
		msg.type = FETCH_ERROR;
		msg.data.error = "Malformed data: URL";
		fetch_data_send_callback(&msg, c);
		return false;
	}
	
	/* skip the data: part */
	params = nsurl_access(c->url) + SLEN("data:");
	
	/* find the comma */
	if ( (comma = strchr(params, ',')) == NULL) {
		msg.type = FETCH_ERROR;
		msg.data.error = "Malformed data: URL";
		fetch_data_send_callback(&msg, c);
		return false;
	}
	
	if (params[0] == ',') {
		/* there is no mimetype here, assume text/plain */
		c->mimetype = strdup("text/plain;charset=US-ASCII");
	} else {	
		/* make a copy of everything between data: and the comma */
		c->mimetype = strndup(params, comma - params);
	}
	
	if (c->mimetype == NULL) {
		msg.type = FETCH_ERROR;
		msg.data.error = 
			"Unable to allocate memory for mimetype in data: URL";
		fetch_data_send_callback(&msg, c);
		return false;
	}
	
	if (strcmp(c->mimetype + strlen(c->mimetype) - 7, ";base64") == 0) {
		c->base64 = true;
		c->mimetype[strlen(c->mimetype) - 7] = '\0';
	} else {
		c->base64 = false;
	}
	
	/* URL unescape the data first, just incase some insane page
	 * decides to nest URL and base64 encoding.  Like, say, Acid2.
	 */
	res = url_unescape(comma + 1, 0, &unescaped_len, &unescaped);
	if (res != NSERROR_OK) {
		msg.type = FETCH_ERROR;
		msg.data.error = "Unable to URL decode data: URL";
		fetch_data_send_callback(&msg, c);
		return false;
	}
	
	if (c->base64) {
		if ((nsu_base64_decode_alloc((uint8_t *)unescaped,
					     unescaped_len,
					     (uint8_t **)&c->data,
					     &c->datalen) != NSUERROR_OK) ||
		    (c->data == NULL)) {
			msg.type = FETCH_ERROR;
			msg.data.error = "Unable to Base64 decode data: URL";
			fetch_data_send_callback(&msg, c);
			free(unescaped);
			return false;
		}
		free(unescaped);
	} else {
		c->datalen = unescaped_len;
		c->data = unescaped;
	}
	
	
	return true;
}

static void fetch_data_poll(lwc_string *scheme)
{
	fetch_msg msg;
	struct fetch_data_context *c, *save_ring = NULL;
	
	/* Iterate over ring, processing each pending fetch */
	while (ring != NULL) {
		/* Take the first entry from the ring */
		c = ring;
		RING_REMOVE(ring, c);

		/* Ignore fetches that have been flagged as locked.
		 * This allows safe re-entrant calls to this function.
		 * Re-entrancy can occur if, as a result of a callback,
		 * the interested party causes fetch_poll() to be called 
		 * again.
		 */
		if (c->locked == true) {
			RING_INSERT(save_ring, c);
			continue;
		}

		/* Only process non-aborted fetches */
		if (c->aborted == false && fetch_data_process(c) == true) {
			fetch_set_http_code(c->parent_fetch, 200);
			NSLOG(netsurf, INFO,
			      "setting data: MIME type to %s, length to %"PRIsizet,
			      c->mimetype,
			      c->datalen);
			/* Any callback can result in the fetch being aborted.
			 * Therefore, we _must_ check for this after _every_
			 * call to fetch_data_send_callback().
			 */
			fetch_data_send_header(c, "Content-Type: %s",
					c->mimetype);

			if (c->aborted == false) {
				fetch_data_send_header(c, "Content-Length: %"
						PRIsizet, c->datalen);
			}

			if (c->aborted == false) {
				/* Set max-age to 1 year. */
				fetch_data_send_header(c, "Cache-Control: "
						"max-age=31536000");
			}

			if (c->aborted == false) {
				msg.type = FETCH_DATA;
				msg.data.header_or_data.buf = 
						(const uint8_t *) c->data;
				msg.data.header_or_data.len = c->datalen;
				fetch_data_send_callback(&msg, c);
			}

			if (c->aborted == false) {
				msg.type = FETCH_FINISHED;
				fetch_data_send_callback(&msg, c);
			}
		} else {
			NSLOG(netsurf, INFO, "Processing of %.140s failed!",
			      nsurl_access(c->url));

			/* Ensure that we're unlocked here. If we aren't, 
			 * then fetch_data_process() is broken.
			 */
			assert(c->locked == false);
		}

		/* And now finish */
		fetch_remove_from_queues(c->parent_fetch);
		fetch_free(c->parent_fetch);
	}

	/* Finally, if we saved any fetches which were locked, put them back
	 * into the ring for next time
	 */
	ring = save_ring;
}

nserror fetch_data_register(void)
{
	lwc_string *scheme = lwc_string_ref(corestring_lwc_data);
	const struct fetcher_operation_table fetcher_ops = {
		.initialise = fetch_data_initialise,
		.acceptable = fetch_data_can_fetch,
		.setup = fetch_data_setup,
		.start = fetch_data_start,
		.abort = fetch_data_abort,
		.free = fetch_data_free,
		.poll = fetch_data_poll,
		.finalise = fetch_data_finalise
	};

	return fetcher_add(scheme, &fetcher_ops);
}
