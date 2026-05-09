/*
 * Copyright 2012 Vincent Sanders <vince@kyllikki.org>
 *
 * This file is part of NetSurf, http://www.netsurf-browser.org/
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

/** \file
 * implementation for javascript scheme fetcher
 *
 * This fetcher implements http://www.whatwg.org/specs/web-apps/current-work/multipage/browsers.html#javascript-protocol
 */

#include <stdbool.h>
#include <stdlib.h>
#include <libwapcaplet/libwapcaplet.h>

#include "utils/nsurl.h"
#include "utils/corestrings.h"
#include "utils/ring.h"
#include "content/fetch.h"
#include "content/fetchers.h"

#include "javascript/fetcher.h"

/** Context for an resource fetch */
struct fetch_javascript_context {
	struct fetch_javascript_context *r_next, *r_prev;

	struct fetch *fetchh; /**< Handle for this fetch */

	bool aborted; /**< Flag indicating fetch has been aborted */
	bool locked; /**< Flag indicating entry is already entered */

	nsurl *url; /**< The URL being fetched */
};

static struct fetch_javascript_context *ring = NULL;


/** issue fetch callbacks with locking */
static inline bool fetch_javascript_send_callback(const fetch_msg *msg,
		struct fetch_javascript_context *ctx)
{
	ctx->locked = true;
	fetch_send_callback(msg, ctx->fetchh);
	ctx->locked = false;

	return ctx->aborted;
}


/**
 * called from poll to progress fetch.
 *
 * \todo This is currently completely unimplemented and just returns 204
 */
static bool fetch_javascript_handler(struct fetch_javascript_context *ctx)
{
	fetch_msg msg;
	int code = 204;

	/* content is going to return error code */
	fetch_set_http_code(ctx->fetchh, code);

	msg.type = FETCH_FINISHED;
	fetch_javascript_send_callback(&msg, ctx);

	return true;
}



/** callback to initialise the resource fetcher. */
static bool fetch_javascript_initialise(lwc_string *scheme)
{
	return true;
}

/** callback to finalise the resource fetcher. */
static void fetch_javascript_finalise(lwc_string *scheme)
{
}

static bool fetch_javascript_can_fetch(const nsurl *url)
{
	return true;
}

/** callback to set up a resource fetch context. */
static void *
fetch_javascript_setup(struct fetch *fetchh,
		 nsurl *url,
		 bool only_2xx,
		 bool downgrade_tls,
		 const char *post_urlenc,
		 const struct fetch_multipart_data *post_multipart,
		 const char **headers)
{
	struct fetch_javascript_context *ctx;

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL)
		return NULL;

	ctx->url = nsurl_ref(url);

	ctx->fetchh = fetchh;

	RING_INSERT(ring, ctx);

	return ctx;
}

/** callback to free a resource fetch */
static void fetch_javascript_free(void *ctx)
{
	struct fetch_javascript_context *c = ctx;
	if (c->url != NULL) {
		nsurl_unref(c->url);
	}
	RING_REMOVE(ring, c);
	free(ctx);
}

/** callback to start a resource fetch */
static bool fetch_javascript_start(void *ctx)
{
	return true;
}

/** callback to abort a resource fetch */
static void fetch_javascript_abort(void *ctx)
{
	struct fetch_javascript_context *c = ctx;

	/* To avoid the poll loop having to deal with the fetch context
	 * disappearing from under it, we simply flag the abort here.
	 * The poll loop itself will perform the appropriate cleanup.
	 */
	c->aborted = true;
}


/** callback to poll for additional resource fetch contents */
static void fetch_javascript_poll(lwc_string *scheme)
{
	struct fetch_javascript_context *c, *next;

	if (ring == NULL) return;

	/* Iterate over ring, processing each pending fetch */
	c = ring;
	do {
		/* Ignore fetches that have been flagged as locked.
		 * This allows safe re-entrant calls to this function.
		 * Re-entrancy can occur if, as a result of a callback,
		 * the interested party causes fetch_poll() to be called
		 * again.
		 */
		if (c->locked == true) {
			next = c->r_next;
			continue;
		}

		/* Only process non-aborted fetches */
		if (c->aborted == false) {
			/* resource fetches can be processed in one go */
			fetch_javascript_handler(c);
		}

		/* Compute next fetch item at the last possible moment
		 * as processing this item may have added to the ring
		 */
		next = c->r_next;

		fetch_remove_from_queues(c->fetchh);
		fetch_free(c->fetchh);

		/* Advance to next ring entry, exiting if we've reached
		 * the start of the ring or the ring has become empty
		 */
	} while ( (c = next) != ring && ring != NULL);
}

/**
 * Register javascript scheme fetcher with fetcher factory.
 *
 * \return NSERROR_OK on success or appropriate error code on faliure.
*/
nserror fetch_javascript_register(void)
{
	lwc_string *scheme = lwc_string_ref(corestring_lwc_javascript);
	const struct fetcher_operation_table fetcher_ops = {
		.initialise = fetch_javascript_initialise,
		.acceptable = fetch_javascript_can_fetch,
		.setup = fetch_javascript_setup,
		.start = fetch_javascript_start,
		.abort = fetch_javascript_abort,
		.free = fetch_javascript_free,
		.poll = fetch_javascript_poll,
		.finalise = fetch_javascript_finalise
	};

	return fetcher_add(scheme, &fetcher_ops);
}
