/*
 * MacSurf — macos9_fetcher_stubs.c
 *
 * Minimal fetcher implementations for resource:, about:, file:,
 * data:, and javascript: URL schemes.  Each one registers with the
 * fetcher system and immediately completes every request with empty
 * content.  This unblocks the HTML content handler which waits for
 * resource:internal.css (and friends) to complete before beginning
 * HTML conversion.
 *
 * Replaces the no-op stubs in macos9_extra_stubs.c which returned
 * NSERROR_OK from fetch_*_register() but never actually registered
 * a fetcher — so the fetch system had no handler for the schemes,
 * and hlcache requests hung forever.
 */

#include <stdlib.h>
#include <string.h>

#include "utils/errors.h"
#include "utils/nsurl.h"
#include "utils/log.h"
#include "utils/utils.h"
#include "content/fetch.h"
#include "content/fetchers.h"

/* ---- Shared minimal fetcher implementation ---- */

struct stub_fetch_ctx {
	struct fetch *parent;
	bool started;
	bool aborted;
	struct stub_fetch_ctx *r_next;
	struct stub_fetch_ctx *r_prev;
};

static struct stub_fetch_ctx *stub_ring = NULL;

static bool
stub_initialise(lwc_string *scheme)
{
	(void)scheme;
	return true;
}

static void
stub_finalise(lwc_string *scheme)
{
	(void)scheme;
}

static bool
stub_can_fetch(const struct nsurl *url)
{
	(void)url;
	return true;
}

static void *
stub_setup(struct fetch *parent_fetch, struct nsurl *url,
		bool only_2xx, bool downgrade_tls,
		const char *post_urlenc,
		const struct fetch_multipart_data *post_multipart,
		const char **headers)
{
	struct stub_fetch_ctx *ctx;

	(void)url; (void)only_2xx; (void)downgrade_tls;
	(void)post_urlenc; (void)post_multipart; (void)headers;

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) return NULL;
	ctx->parent = parent_fetch;
	ctx->started = false;
	ctx->aborted = false;

	/* Insert into ring so poll() can find it. */
	if (stub_ring == NULL) {
		stub_ring = ctx;
		ctx->r_next = ctx;
		ctx->r_prev = ctx;
	} else {
		ctx->r_next = stub_ring;
		ctx->r_prev = stub_ring->r_prev;
		stub_ring->r_prev->r_next = ctx;
		stub_ring->r_prev = ctx;
	}

	return ctx;
}

static bool
stub_start(void *fetch)
{
	struct stub_fetch_ctx *ctx = fetch;
	if (ctx != NULL) ctx->started = true;
	return true;
}

static void
stub_abort(void *fetch)
{
	struct stub_fetch_ctx *ctx = fetch;
	if (ctx != NULL) ctx->aborted = true;
}

static void
stub_free(void *fetch)
{
	struct stub_fetch_ctx *ctx = fetch;
	if (ctx == NULL) return;

	/* Remove from ring. */
	if (ctx->r_next == ctx) {
		stub_ring = NULL;
	} else {
		ctx->r_prev->r_next = ctx->r_next;
		ctx->r_next->r_prev = ctx->r_prev;
		if (stub_ring == ctx)
			stub_ring = ctx->r_next;
	}
	free(ctx);
}

static void
stub_poll(lwc_string *scheme)
{
	struct stub_fetch_ctx *ctx;
	struct stub_fetch_ctx *next;
	fetch_msg msg;

	(void)scheme;
	if (stub_ring == NULL) return;

	ctx = stub_ring;
	do {
		next = ctx->r_next;
		if (ctx->started && !ctx->aborted) {
			/* Send a minimal successful response with
			 * Content-Type header so the content handler
			 * knows what to do with it. */
			{
				static const char ct[] =
					"Content-Type: text/css";
				msg.type = FETCH_HEADER;
				msg.data.header_or_data.buf =
					(const uint8_t *)ct;
				msg.data.header_or_data.len =
					sizeof(ct) - 1;
				fetch_send_callback(&msg, ctx->parent);
			}
			/* Empty body. */
			{
				static const unsigned char empty = 0;
				msg.type = FETCH_DATA;
				msg.data.header_or_data.buf = &empty;
				msg.data.header_or_data.len = 0;
				fetch_send_callback(&msg, ctx->parent);
			}
			msg.type = FETCH_FINISHED;
			fetch_send_callback(&msg, ctx->parent);
			return;
		}
		ctx = next;
	} while (ctx != stub_ring);
}

static const struct fetcher_operation_table stub_ops = {
	stub_initialise,
	stub_can_fetch,
	stub_setup,
	stub_start,
	stub_abort,
	stub_free,
	stub_poll,
	NULL,
	stub_finalise
};

/* ---- Per-scheme registration ---- */

nserror fetch_resource_register(void)
{
	lwc_string *scheme;
	if (lwc_intern_string("resource", 8, &scheme) != lwc_error_ok)
		return NSERROR_NOMEM;
	return fetcher_add(scheme, &stub_ops);
}

nserror fetch_about_register(void)
{
	lwc_string *scheme;
	if (lwc_intern_string("about", 5, &scheme) != lwc_error_ok)
		return NSERROR_NOMEM;
	return fetcher_add(scheme, &stub_ops);
}

nserror fetch_file_register(void)
{
	lwc_string *scheme;
	if (lwc_intern_string("file", 4, &scheme) != lwc_error_ok)
		return NSERROR_NOMEM;
	return fetcher_add(scheme, &stub_ops);
}

nserror fetch_data_register(void)
{
	lwc_string *scheme;
	if (lwc_intern_string("data", 4, &scheme) != lwc_error_ok)
		return NSERROR_NOMEM;
	return fetcher_add(scheme, &stub_ops);
}

nserror fetch_javascript_register(void)
{
	lwc_string *scheme;
	if (lwc_intern_string("javascript", 10, &scheme) != lwc_error_ok)
		return NSERROR_NOMEM;
	return fetcher_add(scheme, &stub_ops);
}
