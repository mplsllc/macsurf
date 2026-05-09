/*
 * Copyright 2011 Vincent Sanders <vince@netsurf-browser.org>
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
 * resource scheme URL handling. Based on the data fetcher by Rob Kendrick
 */

#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdarg.h>
#include <libwapcaplet/libwapcaplet.h>

#include "netsurf/inttypes.h"
#include "utils/nsurl.h"
#include "utils/corestrings.h"
#include "utils/log.h"
#include "utils/time.h"
#include "utils/messages.h"
#include "utils/utils.h"
#include "utils/ring.h"
#include "netsurf/fetch.h"
#include "desktop/gui_internal.h"

#include "content/fetch.h"
#include "content/fetchers.h"
#include "content/fetchers/resource.h"

#define DIRECT_ETAG_VALUE 123456

/** Valid resource paths */
static const char *fetch_resource_paths[] = {
	"adblock.css",
	"default.css",
	"internal.css",
	"quirks.css",
	"user.css",
	"credits.html",
	"licence.html",
	"welcome.html",
	"favicon.ico",
	"default.ico",
	"netsurf.png",
	"icons/arrow-l.png",
	"icons/content.png",
	"icons/directory.png",
	"icons/directory2.png",
	"icons/hotlist-add.png",
	"icons/hotlist-rmv.png",
	"icons/search.png"
};

/**
 * map of resource scheme paths to redirect urls
 */
static struct fetch_resource_map_entry {
	lwc_string *path; /**< resource scheme path */
	nsurl *redirect_url; /**< url to redirect to */
	const uint8_t *data; /**< direct pointer to data */
	size_t data_len; /**< length of direct data */
} fetch_resource_map[NOF_ELEMENTS(fetch_resource_paths)];

struct fetch_resource_context;

typedef bool (*fetch_resource_handler)(struct fetch_resource_context *);

/** Context for an resource fetch */
struct fetch_resource_context {
	struct fetch_resource_context *r_next, *r_prev;

	struct fetch *fetchh; /**< Handle for this fetch */

	bool aborted; /**< Flag indicating fetch has been aborted */
	bool locked; /**< Flag indicating entry is already entered */

	nsurl *url; /**< requested url */

	struct fetch_resource_map_entry *entry; /**< resource map entry */

	fetch_resource_handler handler;

	time_t etag;
};

static struct fetch_resource_context *ring = NULL;

static uint32_t fetch_resource_path_count;

/** issue fetch callbacks with locking */
static inline bool fetch_resource_send_callback(const fetch_msg *msg,
		struct fetch_resource_context *ctx)
{
	ctx->locked = true;
	fetch_send_callback(msg, ctx->fetchh);
	ctx->locked = false;

	return ctx->aborted;
}

static bool fetch_resource_send_header(struct fetch_resource_context *ctx,
		const char *fmt, ...)
{
	fetch_msg msg;
	char header[64];
	va_list ap;
	int len;

	va_start(ap, fmt);
	len = vsnprintf(header, sizeof header, fmt, ap);
	va_end(ap);

	if (len >= (int)sizeof(header) || len < 0) {
		return false;
	}

	msg.type = FETCH_HEADER;
	msg.data.header_or_data.buf = (const uint8_t *) header;
	msg.data.header_or_data.len = len;

	return fetch_resource_send_callback(&msg, ctx);
}



/**
 * resource handler that results in a redirect to another url.
 */
static bool fetch_resource_redirect_handler(struct fetch_resource_context *ctx)
{
	fetch_msg msg;

	/* content is going to return redirect */
	fetch_set_http_code(ctx->fetchh, 302);

	msg.type = FETCH_REDIRECT;
	msg.data.redirect = nsurl_access(ctx->entry->redirect_url);
	fetch_resource_send_callback(&msg, ctx);

	return true;
}

/* resource handler that returns data directly */
static bool fetch_resource_data_handler(struct fetch_resource_context *ctx)
{
	fetch_msg msg;

	/* Check if we can just return not modified */
	if (ctx->etag != 0 && ctx->etag == DIRECT_ETAG_VALUE) {
		fetch_set_http_code(ctx->fetchh, 304);
		msg.type = FETCH_NOTMODIFIED;
		fetch_resource_send_callback(&msg, ctx);
		return true;
	}

	/* fetch is going to be successful */
	fetch_set_http_code(ctx->fetchh, 200);

	/* Any callback can result in the fetch being aborted.
	 * Therefore, we _must_ check for this after _every_ call to
	 * fetch_file_send_callback().
	 */

	/* content type */
	if (fetch_resource_send_header(ctx, "Content-Type: %s",
				       guit->fetch->filetype(lwc_string_data(ctx->entry->path)))) {
		goto fetch_resource_data_aborted;
	}

	/* content length */
	if (fetch_resource_send_header(ctx, "Content-Length: %" PRIsizet,
				       ctx->entry->data_len)) {
		goto fetch_resource_data_aborted;
	}

	/* create etag */
	if (fetch_resource_send_header(ctx, "ETag: \"%10" PRId64 "\"",
				       (int64_t) DIRECT_ETAG_VALUE)) {
		goto fetch_resource_data_aborted;
	}

	/* create max-age of 1 year */
	if (fetch_resource_send_header(ctx,
			"Cache-Control: max-age=31536000")) {
		goto fetch_resource_data_aborted;
	}

	msg.type = FETCH_DATA;
	msg.data.header_or_data.buf = (const uint8_t *) ctx->entry->data;
	msg.data.header_or_data.len = ctx->entry->data_len;
	fetch_resource_send_callback(&msg, ctx);

	if (ctx->aborted == false) {
		msg.type = FETCH_FINISHED;
		fetch_resource_send_callback(&msg, ctx);
	}

fetch_resource_data_aborted:

	return true;
}

static bool fetch_resource_notfound_handler(struct fetch_resource_context *ctx)
{
	fetch_msg msg;
	int code = 404;
	char buffer[1024];
	const char *title;
	char key[8];

	/* content is going to return error code */
	fetch_set_http_code(ctx->fetchh, code);

	/* content type */
	if (fetch_resource_send_header(ctx, "Content-Type: text/html; charset=utf-8"))
		goto fetch_resource_notfound_handler_aborted;

	snprintf(key, sizeof key, "HTTP%03d", code);
	title = messages_get(key);

	snprintf(buffer, sizeof buffer,
		 "<html><head>"
		 "<title>%s</title>"
		 "<link rel=\"stylesheet\" type=\"text/css\" "
		 "href=\"resource:internal.css\">\n"
		 "</head>"
		 "<body class=\"ns-even-bg ns-even-fg ns-border\" "
		 "id =\"fetcherror\">\n"
		 "<h1 class=\"ns-border ns-odd-fg-bad\">%s</h1>\n"
		 "<p>%s %d %s %s</p>\n"
		 "</body>\n</html>\n",
		 title, title,
		 messages_get("FetchErrorCode"), code,
		 messages_get("FetchFile"), nsurl_access(ctx->url));

	msg.type = FETCH_DATA;
	msg.data.header_or_data.buf = (const uint8_t *) buffer;
	msg.data.header_or_data.len = strlen(buffer);
	if (fetch_resource_send_callback(&msg, ctx))
		goto fetch_resource_notfound_handler_aborted;

	msg.type = FETCH_FINISHED;
	fetch_resource_send_callback(&msg, ctx);

fetch_resource_notfound_handler_aborted:
	return false;
}



/** callback to initialise the resource fetcher. */
static bool fetch_resource_initialise(lwc_string *scheme)
{
	struct fetch_resource_map_entry *e;
	uint32_t i;
	nserror res;

	fetch_resource_path_count = 0;

	for (i = 0; i < NOF_ELEMENTS(fetch_resource_paths); i++) {
		e = &fetch_resource_map[fetch_resource_path_count];

		if (lwc_intern_string(fetch_resource_paths[i],
				strlen(fetch_resource_paths[i]),
				&e->path) != lwc_error_ok) {
			while (i > 0) {
				i--;
				lwc_string_unref(fetch_resource_map[i].path);
				nsurl_unref(fetch_resource_map[i].redirect_url);
			}
			/** \todo should this exit with an error condition? */
		}

		e->data = NULL;
		res = guit->fetch->get_resource_data(lwc_string_data(e->path),
						     &e->data,
						     &e->data_len);
		if (res == NSERROR_OK) {
			NSLOG(netsurf, INFO, "direct data for %s",
			      fetch_resource_paths[i]);
			fetch_resource_path_count++;
		} else {
			e->redirect_url = guit->fetch->get_resource_url(fetch_resource_paths[i]);
			if (e->redirect_url == NULL) {
				lwc_string_unref(e->path);
			} else {
				NSLOG(netsurf, INFO, "redirect url for %s",
				      fetch_resource_paths[i]);
				fetch_resource_path_count++;
			}
		}
	}

	return true;
}

/** callback to finalise the resource fetcher. */
static void fetch_resource_finalise(lwc_string *scheme)
{
	uint32_t i;

	for (i = 0; i < fetch_resource_path_count; i++) {
		lwc_string_unref(fetch_resource_map[i].path);
		if (fetch_resource_map[i].data != NULL) {
			guit->fetch->release_resource_data(fetch_resource_map[i].data);
		} else {
			nsurl_unref(fetch_resource_map[i].redirect_url);
		}
	}
}

static bool fetch_resource_can_fetch(const nsurl *url)
{
	return true;
}

/**
 * set up a resource fetch context.
 */
static void *
fetch_resource_setup(struct fetch *fetchh,
		     nsurl *url,
		     bool only_2xx,
		     bool downgrade_tls,
		     const char *post_urlenc,
		     const struct fetch_multipart_data *post_multipart,
		     const char **headers)
{
	struct fetch_resource_context *ctx;
	lwc_string *path;
	nserror ret;
	uint32_t i;

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		return NULL;
	}

	ctx->handler = fetch_resource_notfound_handler;

	if ((path = nsurl_get_component(url, NSURL_PATH)) != NULL) {
		bool match;

		/* Ensure requested path is valid */
		for (i = 0; i < fetch_resource_path_count; i++) {
			if (lwc_string_isequal(path,
					fetch_resource_map[i].path,
					&match) == lwc_error_ok && match) {
				/* found a url match, select handler */
				ctx->entry = &fetch_resource_map[i];
				if (ctx->entry->data != NULL) {
					ctx->handler = fetch_resource_data_handler;
				} else {
					ctx->handler = fetch_resource_redirect_handler;
				}
				break;
			}
		}

		lwc_string_unref(path);
	}

	ctx->url = nsurl_ref(url);

	/* Scan request headers looking for If-None-Match */
	for (i = 0; headers[i] != NULL; i++) {
		if (strncasecmp(headers[i], "If-None-Match:",
				SLEN("If-None-Match:")) != 0) {
			continue;
		}

		/* If-None-Match: "12345678" */
		const char *d; d = headers[i] + SLEN(\"If-None-Match:\");

		/* Scan to first digit, if any */
		while (*d != '\0' && (*d < '0' || '9' < *d))
			d++;

		/* Convert to time_t */
		if (*d != '\0') {
			ret = nsc_snptimet(d, strlen(d), &ctx->etag);
			if (ret != NSERROR_OK) {
				NSLOG(fetch, WARNING,
						"Bad If-None-Match value");
			}
		}
	}

	ctx->fetchh = fetchh;

	RING_INSERT(ring, ctx);

	return ctx;
}

/** callback to free a resource fetch */
static void fetch_resource_free(void *ctx)
{
	struct fetch_resource_context *c = ctx;
	if (c->url != NULL)
		nsurl_unref(c->url);
	free(ctx);
}

/** callback to start a resource fetch */
static bool fetch_resource_start(void *ctx)
{
	return true;
}

/** callback to abort a resource fetch */
static void fetch_resource_abort(void *ctx)
{
	struct fetch_resource_context *c = ctx;

	/* To avoid the poll loop having to deal with the fetch context
	 * disappearing from under it, we simply flag the abort here.
	 * The poll loop itself will perform the appropriate cleanup.
	 */
	c->aborted = true;
}


/** callback to poll for additional resource fetch contents */
static void fetch_resource_poll(lwc_string *scheme)
{
	struct fetch_resource_context *c, *save_ring = NULL;

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
		if (c->aborted == false) {
			/* resource fetches can be processed in one go */
			c->handler(c);
		}

		/* And now finish */
		fetch_remove_from_queues(c->fetchh);
		fetch_free(c->fetchh);
	}

	/* Finally, if we saved any fetches which were locked, put them back
	 * into the ring for next time
	 */
	ring = save_ring;
}

nserror fetch_resource_register(void)
{
	lwc_string *scheme = lwc_string_ref(corestring_lwc_resource);
	const struct fetcher_operation_table fetcher_ops = {
		.initialise = fetch_resource_initialise,
		.acceptable = fetch_resource_can_fetch,
		.setup = fetch_resource_setup,
		.start = fetch_resource_start,
		.abort = fetch_resource_abort,
		.free = fetch_resource_free,
		.poll = fetch_resource_poll,
		.finalise = fetch_resource_finalise
	};

	return fetcher_add(scheme, &fetcher_ops);
}
