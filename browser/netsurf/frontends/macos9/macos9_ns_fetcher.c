/*
 * MacSurf — macos9_ns_fetcher.c
 *
 * HTTP/HTTPS fetcher for the NetSurf fetch system.  Wraps the
 * existing synchronous OT fetch path (proxy at 116.202.231.103:8765)
 * in the fetcher_operation_table interface so hlcache can dispatch
 * http: and https: URLs to us.
 *
 * Architecture: synchronous fetch in start(), callbacks in poll().
 * OS 9 is cooperative so blocking in start() is fine — the OT sync
 * idle notifier yields to threads while we wait for data.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <libwapcaplet/libwapcaplet.h>

#include "utils/errors.h"
#include "utils/nsurl.h"
#include "utils/log.h"
#include "content/fetch.h"
#include "content/fetchers.h"

#ifdef __MACOS9__
#include <OpenTransport.h>
#include <OpenTptInternet.h>
#include <Threads.h>
#include <MacMemory.h>
extern OTClientContextPtr macos9_ot_context;
#endif

#define MACSURF_PROXY_HOST "116.202.231.103"
#define MACSURF_PROXY_PORT 8765
#define MACSURF_FETCH_MAX  (256 * 1024)

struct macos9_fetch_ctx {
	struct fetch *parent;
	char url[512];
	char *response_buf;     /* raw HTTP response (headers + body) */
	long response_total;
	char *headers_start;    /* points into response_buf */
	long headers_len;
	char *body_start;       /* points into response_buf */
	long body_len;
	int http_status;
	bool started;
	bool finished;
	bool callbacks_sent;
	bool aborted;
	struct macos9_fetch_ctx *r_next;
	struct macos9_fetch_ctx *r_prev;
};

static struct macos9_fetch_ctx *fetch_ring = NULL;

/* ---- Ring helpers ---- */

static void
ring_insert(struct macos9_fetch_ctx *ctx)
{
	if (fetch_ring == NULL) {
		fetch_ring = ctx;
		ctx->r_next = ctx;
		ctx->r_prev = ctx;
	} else {
		ctx->r_next = fetch_ring;
		ctx->r_prev = fetch_ring->r_prev;
		fetch_ring->r_prev->r_next = ctx;
		fetch_ring->r_prev = ctx;
	}
}

static void
ring_remove(struct macos9_fetch_ctx *ctx)
{
	if (ctx->r_next == ctx) {
		fetch_ring = NULL;
	} else {
		ctx->r_prev->r_next = ctx->r_next;
		ctx->r_next->r_prev = ctx->r_prev;
		if (fetch_ring == ctx)
			fetch_ring = ctx->r_next;
	}
	ctx->r_next = NULL;
	ctx->r_prev = NULL;
}

/* ---- OT synchronous fetch ---- */

#ifdef __MACOS9__
static pascal void
ns_yield_notifier(void *ctx, OTEventCode code,
		OTResult result, void *cookie)
{
	(void)ctx; (void)result; (void)cookie;
	if (code == kOTSyncIdleEvent)
		YieldToAnyThread();
}

static bool
do_ot_fetch(struct macos9_fetch_ctx *ctx)
{
	OSStatus err;
	EndpointRef ep;
	OTConfigurationRef cfg;
	DNSAddress dns_addr;
	TCall snd_call;
	char proxy_addr[64];
	char request[1024];
	long req_len;
	long total;
	OTResult n;
	OTNotifyUPP notifyUPP;
	char *sep;
	char *buf;

	cfg = OTCreateConfiguration("tcp");
	if (cfg == NULL) return false;

	ep = OTOpenEndpointInContext(cfg, 0, NULL, &err, macos9_ot_context);
	if (err != noErr || ep == NULL) return false;

	notifyUPP = NewOTNotifyUPP(ns_yield_notifier);
	OTSetSynchronous(ep);
	OTSetBlocking(ep);
	OTInstallNotifier(ep, notifyUPP, NULL);
	OTUseSyncIdleEvents(ep, true);

	err = OTBind(ep, NULL, NULL);
	if (err != noErr) {
		OTCloseProvider(ep);
		DisposeOTNotifyUPP(notifyUPP);
		return false;
	}

	sprintf(proxy_addr, "%s:%d", MACSURF_PROXY_HOST, MACSURF_PROXY_PORT);
	OTMemzero(&snd_call, sizeof(TCall));
	snd_call.addr.buf = (UInt8 *)&dns_addr;
	snd_call.addr.len = OTInitDNSAddress(&dns_addr, proxy_addr);

	err = OTConnect(ep, &snd_call, NULL);
	if (err != noErr) {
		OTCloseProvider(ep);
		DisposeOTNotifyUPP(notifyUPP);
		return false;
	}

	req_len = 0;
	req_len += sprintf(request + req_len, "GET %s HTTP/1.0\r\n", ctx->url);
	req_len += sprintf(request + req_len, "User-Agent: MacSurf/0.3\r\n");
	req_len += sprintf(request + req_len, "Connection: close\r\n\r\n");

	n = OTSnd(ep, request, req_len, 0);
	if (n < 0) {
		OTSndOrderlyDisconnect(ep);
		OTCloseProvider(ep);
		DisposeOTNotifyUPP(notifyUPP);
		return false;
	}

	buf = (char *)NewPtr(MACSURF_FETCH_MAX + 4);
	if (buf == NULL) {
		OTSndOrderlyDisconnect(ep);
		OTCloseProvider(ep);
		DisposeOTNotifyUPP(notifyUPP);
		return false;
	}

	total = 0;
	do {
		n = OTRcv(ep, buf + total, (MACSURF_FETCH_MAX - 1) - total, NULL);
		if (n > 0) total += n;
	} while (n > 0 && total < (MACSURF_FETCH_MAX - 1));
	buf[total] = '\0';

	OTSndOrderlyDisconnect(ep);
	OTCloseProvider(ep);
	DisposeOTNotifyUPP(notifyUPP);

	ctx->response_buf = buf;
	ctx->response_total = total;

	/* Split headers from body at \r\n\r\n */
	ctx->headers_start = buf;
	ctx->body_start = buf;
	ctx->body_len = total;
	ctx->headers_len = 0;

	if (total > 0) {
		sep = strstr(buf, "\r\n\r\n");
		if (sep != NULL) {
			ctx->headers_len = (long)(sep - buf);
			ctx->body_start = sep + 4;
			ctx->body_len = total - (long)(ctx->body_start - buf);
		}
	}

	/* Parse HTTP status from first line */
	ctx->http_status = 200;
	if (total > 12 && strncmp(buf, "HTTP/", 5) == 0) {
		const char *sp = strchr(buf, ' ');
		if (sp != NULL) {
			ctx->http_status = atoi(sp + 1);
		}
	}

	return true;
}
#endif /* __MACOS9__ */

/* ---- fetcher_operation_table callbacks ---- */

static bool
fetch_macos9_initialise(lwc_string *scheme)
{
	(void)scheme;
	return true;
}

static void
fetch_macos9_finalise(lwc_string *scheme)
{
	(void)scheme;
}

static bool
fetch_macos9_can_fetch(const struct nsurl *url)
{
	(void)url;
	return true;
}

static void *
fetch_macos9_setup(struct fetch *parent_fetch, struct nsurl *url,
		bool only_2xx, bool downgrade_tls,
		const char *post_urlenc,
		const struct fetch_multipart_data *post_multipart,
		const char **headers)
{
	struct macos9_fetch_ctx *ctx;
	const char *url_str;
	size_t url_len;

	(void)only_2xx; (void)downgrade_tls;
	(void)post_urlenc; (void)post_multipart; (void)headers;

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) return NULL;

	ctx->parent = parent_fetch;
	url_str = nsurl_access(url);
	url_len = strlen(url_str);
	if (url_len >= sizeof(ctx->url))
		url_len = sizeof(ctx->url) - 1;
	memcpy(ctx->url, url_str, url_len);
	ctx->url[url_len] = '\0';

	ring_insert(ctx);
	return ctx;
}

static bool
fetch_macos9_start(void *fetch)
{
	struct macos9_fetch_ctx *ctx = fetch;
	if (ctx == NULL) return false;

	ctx->started = true;

#ifdef __MACOS9__
	if (!do_ot_fetch(ctx)) {
		ctx->finished = true;
		ctx->http_status = 0;
	}
#else
	/* Linux stub: no actual fetch. */
	ctx->finished = true;
	ctx->http_status = 0;
#endif

	return true;
}

static void
fetch_macos9_abort(void *fetch)
{
	struct macos9_fetch_ctx *ctx = fetch;
	if (ctx != NULL) ctx->aborted = true;
}

static void
fetch_macos9_free(void *fetch)
{
	struct macos9_fetch_ctx *ctx = fetch;
	if (ctx == NULL) return;

	ring_remove(ctx);
	if (ctx->response_buf != NULL) {
#ifdef __MACOS9__
		DisposePtr(ctx->response_buf);
#else
		free(ctx->response_buf);
#endif
	}
	free(ctx);
}

static void
fetch_macos9_poll(lwc_string *scheme)
{
	struct macos9_fetch_ctx *ctx;
	fetch_msg msg;

	(void)scheme;
	if (fetch_ring == NULL) return;

	ctx = fetch_ring;
	do {
		struct macos9_fetch_ctx *next = ctx->r_next;

		if (!ctx->started || ctx->callbacks_sent || ctx->aborted) {
			ctx = next;
			if (ctx == fetch_ring) break;
			continue;
		}

		if (ctx->http_status == 0) {
			/* Fetch failed — send error. */
			msg.type = FETCH_ERROR;
			msg.data.error = "OT fetch failed";
			fetch_send_callback(&msg, ctx->parent);
			ctx->callbacks_sent = true;
			return;
		}

		/* Send headers line by line. */
		if (ctx->headers_len > 0) {
			char *p = ctx->headers_start;
			char *end = ctx->headers_start + ctx->headers_len;
			char *line_end;

			while (p < end) {
				line_end = strstr(p, "\r\n");
				if (line_end == NULL) line_end = end;

				msg.type = FETCH_HEADER;
				msg.data.header_or_data.buf = (const uint8_t *)p;
				msg.data.header_or_data.len =
						(size_t)(line_end - p);
				fetch_send_callback(&msg, ctx->parent);

				if (ctx->aborted) {
					ctx->callbacks_sent = true;
					return;
				}

				p = line_end;
				if (p + 2 <= end && p[0] == '\r' && p[1] == '\n')
					p += 2;
			}
		}

		/* Send body data. */
		if (ctx->body_len > 0) {
			msg.type = FETCH_DATA;
			msg.data.header_or_data.buf =
					(const uint8_t *)ctx->body_start;
			msg.data.header_or_data.len =
					(size_t)ctx->body_len;
			fetch_send_callback(&msg, ctx->parent);

			if (ctx->aborted) {
				ctx->callbacks_sent = true;
				return;
			}
		}

		/* Done. */
		msg.type = FETCH_FINISHED;
		fetch_send_callback(&msg, ctx->parent);
		ctx->callbacks_sent = true;

		/* fetch_send_callback may have freed us via the ring,
		 * so stop iterating immediately. */
		return;

	} while (ctx != fetch_ring);
}

/* ---- Registration ---- */

static const struct fetcher_operation_table macos9_http_ops = {
	fetch_macos9_initialise,
	fetch_macos9_can_fetch,
	fetch_macos9_setup,
	fetch_macos9_start,
	fetch_macos9_abort,
	fetch_macos9_free,
	fetch_macos9_poll,
	NULL,       /* fdset — not used on OS 9 */
	fetch_macos9_finalise
};

nserror macos9_fetcher_register(void)
{
	lwc_string *http_scheme = NULL;
	lwc_string *https_scheme = NULL;
	nserror err;

	if (lwc_intern_string("http", 4, &http_scheme) != lwc_error_ok)
		return NSERROR_NOMEM;
	if (lwc_intern_string("https", 5, &https_scheme) != lwc_error_ok)
		return NSERROR_NOMEM;

	err = fetcher_add(http_scheme, &macos9_http_ops);
	if (err != NSERROR_OK) return err;

	err = fetcher_add(https_scheme, &macos9_http_ops);
	return err;
}
