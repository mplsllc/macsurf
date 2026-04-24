/*
 * MacSurf - macos9_http_fetcher.c
 *
 * NetSurf fetcher_operation_table implementation backed by Open Transport.
 * Replaces the v0.1 standalone macos9_fetch_url() path with a real fetcher
 * that plugs into NetSurf core via fetcher_add() and feeds bytes back through
 * fetch_send_callback().
 *
 * Pattern: Option B from docs/research/netsurf-core-wiring.md §6 — sync OT
 * with OTUseSyncIdleEvents + a notifier that yields to the Thread Manager
 * via YieldToAnyThread() on kOTSyncIdleEvent. Same OT primitives as
 * macos9_fetch.c, just split across the 9 fetcher_operation_table callbacks.
 *
 * State machine inside the fetch context:
 *   INIT      → setup() done, waiting for poll() to start work
 *   CONNECTING → endpoint open + bound, OTConnect issued
 *   SENDING   → connected, OTSnd in progress / done
 *   RECEIVING → reading the HTTP response into a buffer
 *   DONE      → buffer fully received, callbacks emitted, ready to free
 *   ERROR     → terminal error, callback emitted
 *
 * Reference structure: content/fetchers/data.c. The poll loop drains the
 * ring of active fetches and runs each one to completion (or one OT
 * step). Yielding happens inside the OT calls themselves via the sync-idle
 * notifier.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "utils/errors.h"
#include "utils/log.h"
#include "utils/nsurl.h"
#include "utils/ring.h"
#include "content/fetch.h"
#include "content/fetchers.h"

/* Open Transport headers and the per-fetch OT state are only present
 * on the actual CW8 build (gated by __MWERKS__, NOT __MACOS9__, so that
 * Linux cross-checks with -D__MACOS9__ — needed for utils/inet.h to take
 * the macos9 shim branch — don't try to include the unavailable Mac
 * Toolbox headers). */
#ifdef __MWERKS__
#include <Files.h>
#include <OpenTransport.h>
#include <OpenTptInternet.h>
#include <Threads.h>
extern OTClientContextPtr macos9_ot_context;
#endif

/* Same proxy endpoint as macos9_fetch.c. Verified working at v0.1.0
 * against frogfind.com on real OS 9.1 hardware. */
#define MACSURF_PROXY_HOST "116.202.231.103"
#define MACSURF_PROXY_PORT 8765

#define MACOS9_FETCH_BUF_SIZE 32768

enum macos9_fetch_state {
	MFS_INIT = 0,
	MFS_CONNECTING,
	MFS_SENDING,
	MFS_RECEIVING,
	MFS_DONE,
	MFS_ERROR
};

struct macos9_fetch_ctx {
	struct fetch *parent_fetch;
	struct nsurl *url;

	enum macos9_fetch_state state;
	int aborted;
	int locked;

#ifdef __MWERKS__
	EndpointRef ep;
	OTNotifyUPP notify_upp;
#endif

	char *buf;
	long buf_len;
	long buf_cap;

	const char *body;
	long body_len;
	int header_sent;

	const char *err_msg;

	struct macos9_fetch_ctx *r_next, *r_prev;
};

static struct macos9_fetch_ctx *macos9_fetch_ring = NULL;

#ifdef __MWERKS__
static pascal void macos9_fetch_yield_notifier(void *ctx, OTEventCode code,
		OTResult result, void *cookie)
{
	(void)ctx;
	(void)result;
	(void)cookie;
	if (code == kOTSyncIdleEvent)
		YieldToAnyThread();
}
#endif

/* ---- helpers ---- */

static void macos9_fetch_send(struct macos9_fetch_ctx *c, const fetch_msg *msg)
{
	c->locked = 1;
	fetch_send_callback(msg, c->parent_fetch);
	c->locked = 0;
}

static void macos9_fetch_send_error(struct macos9_fetch_ctx *c, const char *err)
{
	fetch_msg msg;
	msg.type = FETCH_ERROR;
	msg.data.error = err;
	macos9_fetch_send(c, &msg);
}

static void macos9_fetch_close_endpoint(struct macos9_fetch_ctx *c)
{
#ifdef __MWERKS__
	if (c->ep != NULL) {
		OTSndOrderlyDisconnect(c->ep);
		OTCloseProvider(c->ep);
		c->ep = NULL;
	}
	if (c->notify_upp != NULL) {
		DisposeOTNotifyUPP(c->notify_upp);
		c->notify_upp = NULL;
	}
#else
	(void)c;
#endif
}

/* ---- the OT primitives, lifted verbatim from macos9_fetch.c
 *      and split across state transitions ---- */

static int macos9_fetch_open(struct macos9_fetch_ctx *c)
{
#ifdef __MWERKS__
	OSStatus err;
	OTConfigurationRef cfg;

	cfg = OTCreateConfiguration("tcp");
	if (cfg == NULL) {
		c->err_msg = "OT config failed";
		return 0;
	}

	c->ep = OTOpenEndpointInContext(cfg, 0, NULL, &err, macos9_ot_context);
	if (err != noErr || c->ep == NULL) {
		c->err_msg = "OT open failed";
		return 0;
	}

	c->notify_upp = NewOTNotifyUPP(macos9_fetch_yield_notifier);
	OTSetSynchronous(c->ep);
	OTSetBlocking(c->ep);
	OTInstallNotifier(c->ep, c->notify_upp, NULL);
	OTUseSyncIdleEvents(c->ep, true);

	err = OTBind(c->ep, NULL, NULL);
	if (err != noErr) {
		c->err_msg = "OT bind failed";
		return 0;
	}
	return 1;
#else
	(void)c;
	return 0;
#endif
}

static int macos9_fetch_connect(struct macos9_fetch_ctx *c)
{
#ifdef __MWERKS__
	OSStatus err;
	DNSAddress dns_addr;
	TCall snd_call;
	char proxy_addr[64];

	sprintf(proxy_addr, "%s:%d", MACSURF_PROXY_HOST, MACSURF_PROXY_PORT);
	OTMemzero(&snd_call, sizeof(TCall));
	snd_call.addr.buf = (UInt8 *)&dns_addr;
	snd_call.addr.len = OTInitDNSAddress(&dns_addr, proxy_addr);

	err = OTConnect(c->ep, &snd_call, NULL);
	if (err != noErr) {
		c->err_msg = "OT connect failed";
		return 0;
	}
	return 1;
#else
	(void)c;
	return 0;
#endif
}

static int macos9_fetch_send_request(struct macos9_fetch_ctx *c)
{
#ifdef __MWERKS__
	char request[1024];
	const char *url_text;
	long req_len;
	OTResult n;

	url_text = nsurl_access(c->url);
	if (url_text == NULL) {
		c->err_msg = "nsurl_access NULL";
		return 0;
	}

	req_len = 0;
	req_len += sprintf(request + req_len, "GET %s HTTP/1.0\r\n", url_text);
	req_len += sprintf(request + req_len, "User-Agent: MacSurf/0.2\r\n");
	req_len += sprintf(request + req_len, "Connection: close\r\n\r\n");

	n = OTSnd(c->ep, request, req_len, 0);
	if (n < 0) {
		c->err_msg = "OT send failed";
		return 0;
	}
	return 1;
#else
	(void)c;
	return 0;
#endif
}

static int macos9_fetch_recv_all(struct macos9_fetch_ctx *c)
{
#ifdef __MWERKS__
	OTResult n;
	long total = 0;

	c->buf = (char *)NewPtr(MACOS9_FETCH_BUF_SIZE + 4);
	if (c->buf == NULL) {
		c->err_msg = "NewPtr failed";
		return 0;
	}
	c->buf_cap = MACOS9_FETCH_BUF_SIZE;

	do {
		n = OTRcv(c->ep, c->buf + total,
				(MACOS9_FETCH_BUF_SIZE - 1) - total, NULL);
		if (n > 0) total += n;
	} while (n > 0 && total < (MACOS9_FETCH_BUF_SIZE - 1));

	c->buf[total] = '\0';
	c->buf_len = total;
	return 1;
#else
	(void)c;
	return 0;
#endif
}

static void macos9_fetch_split_headers(struct macos9_fetch_ctx *c)
{
	char *sep;
	c->body = c->buf;
	c->body_len = c->buf_len;
	if (c->buf_len > 0) {
		sep = strstr(c->buf, "\r\n\r\n");
		if (sep != NULL) {
			c->body = sep + 4;
			c->body_len = c->buf_len - (long)(c->body - c->buf);
		}
	}
}

/* ---- the 9 fetcher_operation_table callbacks ---- */

static bool macos9_http_initialise(lwc_string *scheme)
{
	(void)scheme;
	return true;
}

static void macos9_http_finalise(lwc_string *scheme)
{
	(void)scheme;
}

static bool macos9_http_acceptable(const struct nsurl *url)
{
	(void)url;
	return true;
}

static void *macos9_http_setup(struct fetch *parent_fetch, struct nsurl *url,
		bool only_2xx, bool downgrade_tls,
		const char *post_urlenc,
		const struct fetch_multipart_data *post_multipart,
		const char **headers)
{
	struct macos9_fetch_ctx *c;

	(void)only_2xx;
	(void)downgrade_tls;
	(void)post_urlenc;
	(void)post_multipart;
	(void)headers;

	c = (struct macos9_fetch_ctx *)calloc(1, sizeof(*c));
	if (c == NULL)
		return NULL;

	c->parent_fetch = parent_fetch;
	c->url = nsurl_ref(url);
	c->state = MFS_INIT;

	RING_INSERT(macos9_fetch_ring, c);
	return c;
}

static bool macos9_http_start(void *ctx)
{
	(void)ctx;
	/* Nothing to do — poll() will pick it up from the ring. */
	return true;
}

static void macos9_http_abort(void *ctx)
{
	struct macos9_fetch_ctx *c = (struct macos9_fetch_ctx *)ctx;
	c->aborted = 1;
}

static void macos9_http_free(void *ctx)
{
	struct macos9_fetch_ctx *c = (struct macos9_fetch_ctx *)ctx;

	macos9_fetch_close_endpoint(c);

	if (c->buf != NULL) {
#ifdef __MWERKS__
		DisposePtr(c->buf);
#else
		free(c->buf);
#endif
		c->buf = NULL;
	}

	if (c->url != NULL) {
		nsurl_unref(c->url);
		c->url = NULL;
	}

	free(c);
}

/* Run one fetch from INIT all the way to DONE/ERROR. Because we're using
 * sync OT with OTUseSyncIdleEvents, each OT call internally yields to the
 * Thread Manager, so the cooperative event loop stays alive even though
 * this function blocks. */
static void macos9_http_run(struct macos9_fetch_ctx *c)
{
	fetch_msg msg;

	if (c->aborted) {
		c->state = MFS_DONE;
		return;
	}

	/* INIT → CONNECTING */
	if (!macos9_fetch_open(c)) {
		c->state = MFS_ERROR;
		return;
	}
	c->state = MFS_CONNECTING;
	if (c->aborted) { c->state = MFS_DONE; return; }

	if (!macos9_fetch_connect(c)) {
		c->state = MFS_ERROR;
		return;
	}
	c->state = MFS_SENDING;
	if (c->aborted) { c->state = MFS_DONE; return; }

	if (!macos9_fetch_send_request(c)) {
		c->state = MFS_ERROR;
		return;
	}
	c->state = MFS_RECEIVING;
	if (c->aborted) { c->state = MFS_DONE; return; }

	if (!macos9_fetch_recv_all(c)) {
		c->state = MFS_ERROR;
		return;
	}

	macos9_fetch_split_headers(c);

	fetch_set_http_code(c->parent_fetch, 200);

	/* Synthesize one Content-Type header. The proxy doesn't reliably
	 * send one in plain HTTP and NetSurf core needs at least a
	 * mimetype to dispatch the content handler. v0.2 hardcodes
	 * text/html — when we add real header parsing this becomes the
	 * value parsed out of buf[]. */
	{
		const char *hdr = "Content-Type: text/html";
		msg.type = FETCH_HEADER;
		msg.data.header_or_data.buf = (const uint8_t *)hdr;
		msg.data.header_or_data.len = (size_t)strlen(hdr);
		macos9_fetch_send(c, &msg);
		if (c->aborted) { c->state = MFS_DONE; return; }
	}

	/* Body bytes. The split_headers helper has already pointed body
	 * past the HTTP header block. */
	if (c->body_len > 0) {
		msg.type = FETCH_DATA;
		msg.data.header_or_data.buf = (const uint8_t *)c->body;
		msg.data.header_or_data.len = (size_t)c->body_len;
		macos9_fetch_send(c, &msg);
		if (c->aborted) { c->state = MFS_DONE; return; }
	}

	msg.type = FETCH_FINISHED;
	macos9_fetch_send(c, &msg);

	c->state = MFS_DONE;
}

static void macos9_http_poll(lwc_string *scheme)
{
	struct macos9_fetch_ctx *c, *save_ring = NULL;

	(void)scheme;

	while (macos9_fetch_ring != NULL) {
		c = macos9_fetch_ring;
		RING_REMOVE(macos9_fetch_ring, c);

		if (c->locked) {
			RING_INSERT(save_ring, c);
			continue;
		}

		if (c->state == MFS_INIT) {
			macos9_http_run(c);
		}

		if (c->state == MFS_ERROR) {
			macos9_fetch_send_error(c,
				c->err_msg != NULL ? c->err_msg
						   : "fetch failed");
		}

		fetch_remove_from_queues(c->parent_fetch);
		fetch_free(c->parent_fetch);
	}

	macos9_fetch_ring = save_ring;
}

static int macos9_http_fdset(lwc_string *scheme,
		fd_set *read_set, fd_set *write_set, fd_set *error_set)
{
	(void)scheme;
	(void)read_set;
	(void)write_set;
	(void)error_set;
	return -1;
}

/* ---- registration entry point ---- */

nserror macos9_http_fetcher_register(void)
{
	/* Positional initializer (no designated init for CW8 strict C89).
	 * Field order from content/fetchers.h: initialise, acceptable,
	 * setup, start, abort, free, poll, fdset, finalise. */
	struct fetcher_operation_table fetcher_ops;
	lwc_string *scheme;
	lwc_error lerr;

	fetcher_ops.initialise = macos9_http_initialise;
	fetcher_ops.acceptable = macos9_http_acceptable;
	fetcher_ops.setup      = macos9_http_setup;
	fetcher_ops.start      = macos9_http_start;
	fetcher_ops.abort      = macos9_http_abort;
	fetcher_ops.free       = macos9_http_free;
	fetcher_ops.poll       = macos9_http_poll;
	fetcher_ops.fdset      = macos9_http_fdset;
	fetcher_ops.finalise   = macos9_http_finalise;

	lerr = lwc_intern_string("http", 4, &scheme);
	if (lerr != lwc_error_ok)
		return NSERROR_NOMEM;

	return fetcher_add(scheme, &fetcher_ops);
}
