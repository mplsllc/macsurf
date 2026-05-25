/*
 * MacSurf - macos9_https_fetcher.c
 *
 * Native HTTPS fetcher backed by macTLS (BearSSL + Open Transport).
 * Drives TLS through the OSTLS_New/Start/Pump/Read/Write async API
 * and delivers decrypted HTTP body bytes into NetSurf core via the
 * fetcher_operation_table.
 *
 * V1 scope:
 *   - GET only (POST queued, see macos9_http_fetcher.c for the form
 *     encoding pattern when wired)
 *   - No keep-alive pool (every fetch opens a fresh TLS endpoint)
 *   - No auto-redirect (FETCH_REDIRECT dispatched on 3xx; llcache
 *     restarts the fetch)
 *   - Content-Length and chunked Transfer-Encoding both decoded
 *   - 4 concurrent slots per V1 cap (heap budget on a 16 MB partition,
 *     each OSTLSConnection is ~32 KB)
 *   - 15s no-progress timeout (mirrors fixes107)
 *
 * Self-frees via fetch_remove_from_queues + fetch_free at every
 * terminal callback per fixes102-105 discipline.
 */

#include "utils/errors.h"
#include "utils/nsurl.h"
#include "utils/log.h"
#include "content/fetch.h"
#include "content/fetchers.h"
#include "macsurf_debug.h"

#include <string.h>
#include <stdlib.h>

#ifdef __MACOS9__
#include <Events.h>
#endif

#include "ostls_async.h"
#include "ostls_http.h"
#include "macos9_disk_cache.h"

#define MAX_HTTPS_F        32   /* fixes226: was 16; mactrove hits ~21% NO FREE SLOTS at 16 */
#define HDR_BUF_MAX        4096
#define READ_CHUNK         1024
#define NO_PROGRESS_TICKS  900   /* 15s at 60Hz */

enum hs_state {
	HS_IDLE = 0,
	HS_QUEUED,
	HS_STARTING,
	HS_TLSING,
	HS_SEND_REQ,
	HS_HEADERS,
	HS_BODY,
	HS_CACHEHIT,    /* fixes218 — serve from on-disk cache, no TLS */
	HS_DONE,
	HS_FAIL
};

struct macos9_https_ctx {
	struct fetch    *parent;
	struct nsurl    *url;
	char             host[256];
	unsigned short   port;
	char             path[1024];

	int              state;
	int              aborted;
	int              done;
	const char      *err;
	int              status;

	OSTLSConnection *conn;
	char             req_buf[1024];
	unsigned long    req_len;
	unsigned long    req_sent;

	char            *hdr_buf;
	long             hdr_len;
	long             hdr_cap;

	int              chunked;
	long             content_length;
	long             body_bytes;

	OSTLSChunkDecoder chunk;

	char             mime[128];
	char             redirect_url[1024];

	unsigned long    progress_ticks;

	/* fixes218 — disk cache. cache_eligible flips on after parse_headers
	 * sees a 200 OK with a whitelisted MIME. cache_capture accumulates
	 * the body bytes (raw, post-chunk-decode if applicable); on
	 * FETCH_FINISHED we write to disk via macos9_cache_store. If a
	 * lookup at setup time succeeds, we route through HS_CACHEHIT and
	 * never touch the network. */
	int              cache_eligible;
	char            *cache_capture;
	long             cache_cap_len;
	long             cache_cap_cap;
	int              cache_overflow;
	char            *cache_hit_body;
	long             cache_hit_len;
	char             cache_hit_mime[128];
	int              cache_hit_status;

	/* fixes228 — auto-retry on benign peer-close. CF and Google CDN
	 * close TLS connections aggressively after handshake; one retry
	 * with a fresh connection usually succeeds. retries counts attempts
	 * BEYOND the first; capped at HTTPS_MAX_RETRIES so we don't loop
	 * forever on a genuinely-dead host. */
	int              retries;
};

#define HTTPS_MAX_RETRIES 2

static struct macos9_https_ctx https_slots[MAX_HTTPS_F];

/* ---------- helpers ---------- */

static unsigned long now_ticks(void)
{
#ifdef __MACOS9__
	return (unsigned long)TickCount();
#else
	return 0;
#endif
}

static void hctx_clear(struct macos9_https_ctx *c)
{
	if (c->hdr_buf) { free(c->hdr_buf); c->hdr_buf = NULL; }
	c->hdr_len = 0;
	c->hdr_cap = 0;
	if (c->conn) {
		OSTLS_Close(c->conn);
		OSTLS_Dispose(c->conn);
		c->conn = NULL;
	}
	if (c->cache_capture) { free(c->cache_capture); c->cache_capture = NULL; }
	c->cache_cap_len = 0;
	c->cache_cap_cap = 0;
	if (c->cache_hit_body) { free(c->cache_hit_body); c->cache_hit_body = NULL; }
	c->cache_hit_len = 0;
	if (c->url) { nsurl_unref(c->url); c->url = NULL; }
	c->state = HS_IDLE;
}

/* fixes228 — tear down JUST the TLS connection so the slot can be
 * retried. Keeps c->parent, c->url, c->host, c->port, c->path so the
 * next poll-loop pass can reopen with the same target. Resets header
 * + body capture state so we don't mix data from the failed attempt. */
static void hctx_reset_for_retry(struct macos9_https_ctx *c)
{
	if (c->conn) {
		OSTLS_Close(c->conn);
		OSTLS_Dispose(c->conn);
		c->conn = NULL;
	}
	if (c->hdr_buf) { free(c->hdr_buf); c->hdr_buf = NULL; }
	c->hdr_len = 0;
	c->hdr_cap = 0;
	if (c->cache_capture) { free(c->cache_capture); c->cache_capture = NULL; }
	c->cache_cap_len = 0;
	c->cache_cap_cap = 0;
	c->cache_overflow = 0;
	c->cache_eligible = 0;
	c->req_len = 0;
	c->req_sent = 0;
	c->status = 0;
	c->body_bytes = 0;
	c->content_length = -1;
	c->chunked = 0;
	c->mime[0] = 0;
	c->redirect_url[0] = 0;
	c->state = HS_QUEUED;
	/* Do NOT clear c->aborted: if NetSurf aborted during the first
	 * attempt, we should NOT retry. The first thing the next poll
	 * pass checks is c->aborted; it'll fail cleanly. */
	c->progress_ticks = now_ticks();
}

/* fixes218 — append body bytes to the per-fetch capture buffer.
 * Same geometric-growth + overflow-latch discipline as the HTTP
 * fetcher's cache_capture_append. */
static void hctx_cache_capture(struct macos9_https_ctx *c,
		const char *buf, long len)
{
	long want;
	long cap;
	char *grown;

	if (c == NULL || buf == NULL || len <= 0) return;
	if (c->cache_overflow) return;
	if (!c->cache_eligible) return;

	want = c->cache_cap_len + len;
	if (want > MACSURF_CACHE_MAX_BYTES) {
		c->cache_overflow = 1;
		if (c->cache_capture != NULL) {
			free(c->cache_capture);
			c->cache_capture = NULL;
			c->cache_cap_len = 0;
			c->cache_cap_cap = 0;
		}
		return;
	}

	if (want > c->cache_cap_cap) {
		cap = c->cache_cap_cap == 0 ? 4096 : c->cache_cap_cap * 2;
		while (cap < want) cap *= 2;
		if (cap > MACSURF_CACHE_MAX_BYTES) cap = MACSURF_CACHE_MAX_BYTES;
		grown = (char *)realloc(c->cache_capture, cap);
		if (grown == NULL) {
			c->cache_overflow = 1;
			if (c->cache_capture != NULL) {
				free(c->cache_capture);
				c->cache_capture = NULL;
				c->cache_cap_len = 0;
				c->cache_cap_cap = 0;
			}
			return;
		}
		c->cache_capture = grown;
		c->cache_cap_cap = cap;
	}

	memcpy(c->cache_capture + c->cache_cap_len, buf, len);
	c->cache_cap_len += len;
}

static void hctx_fail(struct macos9_https_ctx *c, const char *why)
{
	struct fetch *p;
	fetch_msg msg;

	if (c->state == HS_FAIL || c->state == HS_DONE) return;

	/* fixes226 — full diag dump on every fail. We need:
	 *  - host being fetched (so we know WHICH sites fail)
	 *  - BearSSL error code (br_err) on handshake fails
	 *  - OT error code (ot_err) on TCP-level fails
	 *  - cipher suite if handshake completed (0 if not)
	 *  - pump_calls + ot_recv_bytes to see how far we got
	 */
	macsurf_debug_log_writef("https: FAIL state=%d status=%d body=%ld why=%s",
		c->state, c->status, c->body_bytes, why ? why : "(null)");
	macsurf_debug_log_writef("  FAIL host=%s port=%d path=%s",
		c->host[0] ? c->host : "(unset)",
		(int)c->port,
		c->path[0] ? c->path : "(unset)");
	if (c->conn != NULL) {
		OSTLSDiagnostics diag;
		memset(&diag, 0, sizeof diag);
		OSTLS_GetDiagnostics(c->conn, &diag);
		/* fixes227 — macsurf_debug_log_writef supports only %d %ld %p %s %%
		 * (see project_macsurf_debug_log_specifiers memory). Cipher
		 * gets printed as decimal; 0xCCA9 = 52393 (ChaCha20-Poly1305),
		 * 0xC02B = 49195 (ECDHE-ECDSA-AES128-GCM-SHA256). */
		macsurf_debug_log_writef(
			"  FAIL diag os_err=%d ot_err=%ld br_err=%d state=%d cipher_dec=%d",
			(int)diag.os_err, (long)diag.ot_err, (int)diag.br_err,
			(int)diag.state, (int)diag.cipher_suite);
		macsurf_debug_log_writef(
			"  FAIL diag pumps=%ld br_state=%ld",
			(long)diag.pump_calls,
			(long)diag.br_state_last);
		macsurf_debug_log_writef(
			"  FAIL diag ot_send: calls=%ld bytes=%ld zero=%ld flow=%ld",
			(long)diag.ot_send_calls,
			(long)diag.ot_send_bytes,
			(long)diag.ot_send_zero,
			(long)diag.ot_send_flow);
		macsurf_debug_log_writef(
			"  FAIL diag ot_recv: calls=%ld bytes=%ld nodata=%ld",
			(long)diag.ot_recv_calls,
			(long)diag.ot_recv_bytes,
			(long)diag.ot_recv_nodata);
	} else {
		macsurf_debug_log_writef("  FAIL diag conn=NULL (never opened)");
	}

	c->err = why;
	c->state = HS_FAIL;

	msg.type = FETCH_ERROR;
	msg.data.error = why ? why : "https: fetch failed";
	fetch_send_callback(&msg, c->parent);

	p = c->parent;
	hctx_clear(c);
	fetch_remove_from_queues(p);
	fetch_free(p);
}

static void hctx_finish(struct macos9_https_ctx *c)
{
	struct fetch *p;
	fetch_msg msg;

	if (c->state == HS_FAIL || c->state == HS_DONE) return;

	macsurf_debug_log_writef("https: done body=%ld status=%d",
		c->body_bytes, c->status);

	/* fixes218 — write to disk before tearing down the slot. */
	if (c->cache_eligible && !c->cache_overflow &&
	    c->cache_capture != NULL && c->cache_cap_len > 0 &&
	    c->url != NULL) {
		const char *u = nsurl_access(c->url);
		if (u != NULL) {
			macos9_cache_store(u, c->status, c->mime,
				c->cache_capture, c->cache_cap_len);
		}
	}

	c->state = HS_DONE;
	msg.type = FETCH_FINISHED;
	fetch_send_callback(&msg, c->parent);

	p = c->parent;
	hctx_clear(c);
	fetch_remove_from_queues(p);
	fetch_free(p);
}

static char *find_line(char **buf, long *len)
{
	char *p = *buf;
	long  n = *len;
	long  i;
	for (i = 0; i + 1 < n; i++) {
		if (p[i] == '\r' && p[i+1] == '\n') {
			p[i] = 0;
			*buf = p + i + 2;
			*len = n - (i + 2);
			return p;
		}
	}
	return NULL;
}

/* Parse the accumulated header block. Returns 1 if headers were
 * fully parsed (\r\n\r\n found), 0 if we need more bytes. On parse
 * the function emits FETCH_HEADER callbacks, sets status / mime /
 * content_length / chunked, and on 3xx with Location: also emits
 * FETCH_REDIRECT and self-finishes. */
static int parse_headers(struct macos9_https_ctx *c, long *body_off)
{
	char *sep;
	char *p, *cur;
	long  cur_len;
	fetch_msg msg;

	if (c->hdr_len < 4) return 0;
	sep = NULL;
	{
		long i;
		for (i = 0; i + 3 < c->hdr_len; i++) {
			if (c->hdr_buf[i] == '\r' && c->hdr_buf[i+1] == '\n' &&
			    c->hdr_buf[i+2] == '\r' && c->hdr_buf[i+3] == '\n') {
				sep = c->hdr_buf + i;
				break;
			}
		}
	}
	if (!sep) return 0;

	*sep = 0;
	cur = c->hdr_buf;
	cur_len = (long)(sep - c->hdr_buf) + 2;
	*body_off = (long)((sep + 4) - c->hdr_buf);

	p = find_line(&cur, &cur_len);
	if (p && strncmp(p, "HTTP/", 5) == 0) {
		char *sp = strchr(p, ' ');
		if (sp) c->status = atoi(sp + 1);
		msg.type = FETCH_HEADER;
		msg.data.header_or_data.buf = (const uint8_t*)p;
		msg.data.header_or_data.len = strlen(p);
		fetch_send_callback(&msg, c->parent);
	}
	fetch_set_http_code(c->parent, c->status);
	macsurf_debug_log_writef("https: status=%d mime='%s' clen=%ld chunked=%d",
		c->status, c->mime, c->content_length, c->chunked);

	while ((p = find_line(&cur, &cur_len)) != NULL) {
		if (p[0] == 0) break;
		if (strncasecmp(p, "Content-Type:", 13) == 0) {
			char *v = p + 13; while (*v == ' ') v++;
			strncpy(c->mime, v, 127); c->mime[127] = 0;
		}
		if (strncasecmp(p, "Content-Length:", 15) == 0) {
			char *v = p + 15; while (*v == ' ') v++;
			c->content_length = atol(v);
		}
		if (strncasecmp(p, "Transfer-Encoding:", 18) == 0) {
			char *v = p + 18; while (*v == ' ') v++;
			if (strncasecmp(v, "chunked", 7) == 0) c->chunked = 1;
		}
		if (strncasecmp(p, "Location:", 9) == 0) {
			char *v = p + 9; size_t lv;
			while (*v == ' ' || *v == '\t') v++;
			lv = strlen(v);
			if (lv >= sizeof(c->redirect_url)) lv = sizeof(c->redirect_url) - 1;
			memcpy(c->redirect_url, v, lv);
			c->redirect_url[lv] = 0;
		}
		msg.type = FETCH_HEADER;
		msg.data.header_or_data.buf = (const uint8_t*)p;
		msg.data.header_or_data.len = strlen(p);
		fetch_send_callback(&msg, c->parent);
	}

	if (c->status >= 300 && c->status < 400 && c->redirect_url[0] != 0) {
		struct fetch *parent_save;
		msg.type = FETCH_REDIRECT;
		msg.data.redirect = c->redirect_url;
		fetch_send_callback(&msg, c->parent);
		MS_LOG("https: redirect");
		parent_save = c->parent;
		hctx_clear(c);
		fetch_remove_from_queues(parent_save);
		fetch_free(parent_save);
		return 2;	/* terminal */
	}

	/* fixes218 — cache eligibility decided once mime has been parsed. */
	if (macos9_cache_mime_eligible(c->status, c->mime)) {
		c->cache_eligible = 1;
	}

	if (c->chunked) OSTLS_HTTP_ChunkDecoderInit(&c->chunk);

	return 1;
}

/* Pump body bytes — either chunked-decoded or raw. Returns 1 if body
 * is complete, 0 if more bytes expected. */
static int feed_body(struct macos9_https_ctx *c, const char *buf, long n)
{
	fetch_msg msg;
	if (n <= 0) return 0;

	if (c->chunked) {
		const char *in = buf;
		UInt32 in_left = (UInt32)n;
		char decode_out[READ_CHUNK];
		while (in_left > 0 && c->chunk.state != kOSTLSChunkStateDone) {
			UInt32 out_w = 0, in_c = 0;
			OSErr e = OSTLS_HTTP_ChunkDecoderProcess(
				&c->chunk, in, in_left,
				decode_out, sizeof decode_out,
				&out_w, &in_c);
			if (e != kOSTLSAsync_OK) {
				hctx_fail(c, "chunked decode error");
				return 1;
			}
			if (out_w > 0) {
				msg.type = FETCH_DATA;
				msg.data.header_or_data.buf = (const uint8_t*)decode_out;
				msg.data.header_or_data.len = out_w;
				fetch_send_callback(&msg, c->parent);
				c->body_bytes += out_w;
				hctx_cache_capture(c, decode_out, (long)out_w);
			}
			if (in_c == 0 && out_w == 0) break;	/* would-loop guard */
			in += in_c;
			in_left -= in_c;
		}
		if (c->chunk.state == kOSTLSChunkStateDone) return 1;
		return 0;
	} else {
		long deliver = n;
		if (c->content_length >= 0 &&
		    c->body_bytes + deliver > c->content_length) {
			deliver = c->content_length - c->body_bytes;
		}
		if (deliver > 0) {
			msg.type = FETCH_DATA;
			msg.data.header_or_data.buf = (const uint8_t*)buf;
			msg.data.header_or_data.len = (size_t)deliver;
			fetch_send_callback(&msg, c->parent);
			c->body_bytes += deliver;
			hctx_cache_capture(c, buf, deliver);
		}
		if (c->content_length >= 0 &&
		    c->body_bytes >= c->content_length) return 1;
		return 0;
	}
}

/* Append bytes into hdr_buf growing as needed. */
static int hdr_append(struct macos9_https_ctx *c, const char *buf, long n)
{
	if (c->hdr_len + n > HDR_BUF_MAX) {
		hctx_fail(c, "https: header buffer overflow");
		return -1;
	}
	if (c->hdr_buf == NULL) {
		c->hdr_cap = 1024;
		c->hdr_buf = (char *)malloc(c->hdr_cap);
		if (!c->hdr_buf) { hctx_fail(c, "https: out of memory"); return -1; }
	}
	while (c->hdr_len + n > c->hdr_cap) {
		long new_cap = c->hdr_cap * 2;
		char *nb;
		if (new_cap > HDR_BUF_MAX) new_cap = HDR_BUF_MAX;
		nb = (char *)realloc(c->hdr_buf, new_cap);
		if (!nb) { hctx_fail(c, "https: out of memory"); return -1; }
		c->hdr_buf = nb;
		c->hdr_cap = new_cap;
	}
	memcpy(c->hdr_buf + c->hdr_len, buf, n);
	c->hdr_len += n;
	return 0;
}

/* ---------- per-slot pump ---------- */

static void hctx_poll(struct macos9_https_ctx *c)
{
	OSTLSEvent ev;
	OSErr      e;
	UInt32     written, got;
	char       rd[READ_CHUNK];

	if (c->state == HS_IDLE || c->state == HS_DONE || c->state == HS_FAIL)
		return;

	if (c->aborted) {
		hctx_fail(c, "https: aborted");
		return;
	}

	/* fixes218 — serve from disk: synthesise FETCH_HEADER /
	 * FETCH_DATA / FETCH_FINISHED and tear down. No TLS opened. */
	if (c->state == HS_CACHEHIT) {
		fetch_msg msg;
		struct fetch *p;
		char status_line[64];
		char ct_line[160];
		int rn;

		c->status = c->cache_hit_status;
		fetch_set_http_code(c->parent, c->status);

		rn = sprintf(status_line, "HTTP/1.1 %d", c->status);
		if (rn > 0) {
			msg.type = FETCH_HEADER;
			msg.data.header_or_data.buf = (const uint8_t *)status_line;
			msg.data.header_or_data.len = (size_t)rn;
			fetch_send_callback(&msg, c->parent);
		}
		if (c->cache_hit_mime[0] != 0) {
			rn = sprintf(ct_line, "Content-Type: %s", c->cache_hit_mime);
			if (rn > 0) {
				msg.type = FETCH_HEADER;
				msg.data.header_or_data.buf = (const uint8_t *)ct_line;
				msg.data.header_or_data.len = (size_t)rn;
				fetch_send_callback(&msg, c->parent);
			}
		}

		if (c->cache_hit_body != NULL && c->cache_hit_len > 0) {
			msg.type = FETCH_DATA;
			msg.data.header_or_data.buf = (const uint8_t *)c->cache_hit_body;
			msg.data.header_or_data.len = (size_t)c->cache_hit_len;
			fetch_send_callback(&msg, c->parent);
			c->body_bytes = c->cache_hit_len;
		}

		macsurf_debug_log_writef("https: CACHE served body=%ld status=%d",
			c->body_bytes, c->status);

		c->state = HS_DONE;
		msg.type = FETCH_FINISHED;
		fetch_send_callback(&msg, c->parent);

		p = c->parent;
		hctx_clear(c);
		fetch_remove_from_queues(p);
		fetch_free(p);
		return;
	}

	if (c->state == HS_QUEUED) {
		OSTLSConfig cfg;
		memset(&cfg, 0, sizeof cfg);
		cfg.host = c->host;
		cfg.port = c->port;
		cfg.server_name = c->host;
		e = OSTLS_New(&c->conn, &cfg);
		if (e != kOSTLSAsync_OK || c->conn == NULL) {
			hctx_fail(c, "https: OSTLS_New failed");
			return;
		}
		e = OSTLS_Start(c->conn);
		if (e != kOSTLSAsync_OK) {
			hctx_fail(c, "https: OSTLS_Start failed");
			return;
		}
		c->progress_ticks = now_ticks();
		c->state = HS_TLSING;
		MS_LOG("https: started");
		return;
	}

	/* Pump up to 8 atomic steps per poll tick. */
	ev = kOSTLSEventNone;
	e = OSTLS_Pump(c->conn, 8, &ev);
	if (e != kOSTLSAsync_OK) {
		hctx_fail(c, "https: pump error");
		return;
	}

	if (ev == kOSTLSEventFailed) {
		/* fixes228 — retry once on early-stage handshake failure.
		 * CF / Google CDN sometimes drop the first connection but
		 * accept the second cleanly. Only retry if no app data yet. */
		if (c->retries < HTTPS_MAX_RETRIES &&
		    (c->state == HS_TLSING || c->state == HS_STARTING ||
		     c->state == HS_QUEUED)) {
			c->retries++;
			macsurf_debug_log_writef(
				"https: RETRY %d after Failed state=%d host=%s",
				c->retries, c->state,
				c->host[0] ? c->host : "(unset)");
			hctx_reset_for_retry(c);
			return;
		}
		hctx_fail(c, "https: handshake/transport failed");
		return;
	}
	if (ev == kOSTLSEventClosed && c->state != HS_BODY) {
		/* fixes228 — retry on "peer closed before complete" which
		 * is what CF/Google CDN does when they close a freshly
		 * handshaked connection before we get the response.
		 *
		 * fixes229 — allow retry while in HS_HEADERS too. fixes228's
		 * condition `state != HS_HEADERS` was based on a stale enum
		 * mental model: HS_CACHEHIT shifted HS_HEADERS to value 5
		 * (= the state we saw in every Closed-before-body fail).
		 * Since body_bytes is always 0 in HS_HEADERS by definition
		 * (we only count body bytes after parse_headers succeeds),
		 * retrying from HS_HEADERS can't duplicate data. */
		if (c->retries < HTTPS_MAX_RETRIES) {
			c->retries++;
			macsurf_debug_log_writef(
				"https: RETRY %d after Closed state=%d host=%s",
				c->retries, c->state,
				c->host[0] ? c->host : "(unset)");
			hctx_reset_for_retry(c);
			return;
		}
		hctx_fail(c, "https: peer closed before complete");
		return;
	}

	if (c->state == HS_TLSING) {
		if (ev == kOSTLSEventHandshakeDone ||
		    OSTLS_GetState(c->conn) == kOSTLSStateOpen) {
			/* Build the GET with a richer browser-shape header set.
			 * UA mirrors the http path's "MacSurf/0.2" string so the
			 * site's analytics / bot heuristics see a single client. */
			int rn = sprintf(c->req_buf,
				"GET %s HTTP/1.1\r\n"
				"Host: %s\r\n"
				"User-Agent: MacSurf/0.2 (Macintosh; PPC Mac OS 9)\r\n"
				"Accept: text/html,application/xhtml+xml,*/*;q=0.8\r\n"
				"Accept-Language: en-US,en;q=0.5\r\n"
				"Accept-Encoding: identity\r\n"
				"Connection: close\r\n"
				"\r\n",
				c->path, c->host);
			if (rn <= 0 || (unsigned long)rn >= sizeof c->req_buf) {
				hctx_fail(c, "https: request too large");
				return;
			}
			c->req_len = (unsigned long)rn;
			c->req_sent = 0;
			c->state = HS_SEND_REQ;
			c->progress_ticks = now_ticks();
			MS_LOG("https: handshake done");
		}
	}

	if (c->state == HS_SEND_REQ) {
		if (c->req_sent < c->req_len) {
			written = 0;
			e = OSTLS_Write(c->conn,
				c->req_buf + c->req_sent,
				(UInt32)(c->req_len - c->req_sent),
				&written);
			if (e != kOSTLSAsync_OK) {
				hctx_fail(c, "https: write failed");
				return;
			}
			if (written > 0) {
				c->req_sent += written;
				c->progress_ticks = now_ticks();
			}
		}
		if (c->req_sent >= c->req_len) {
			c->state = HS_HEADERS;
			MS_LOG("https: request sent");
		}
	}

	if (c->state == HS_HEADERS || c->state == HS_BODY) {
		got = 0;
		e = OSTLS_Read(c->conn, rd, sizeof rd, &got);
		if (e != kOSTLSAsync_OK) {
			hctx_fail(c, "https: read failed");
			return;
		}
		if (got > 0) {
			c->progress_ticks = now_ticks();

			if (c->state == HS_HEADERS) {
				long body_off = 0;
				int  r;
				if (hdr_append(c, rd, (long)got) < 0) return;
				r = parse_headers(c, &body_off);
				if (r == 2) return;	/* redirect terminal */
				if (r == 1) {
					long leftover = c->hdr_len - body_off;
					c->state = HS_BODY;
					if (leftover > 0) {
						if (feed_body(c,
						    c->hdr_buf + body_off,
						    leftover)) {
							hctx_finish(c);
							return;
						}
					}
				}
			} else {
				if (feed_body(c, rd, (long)got)) {
					hctx_finish(c);
					return;
				}
			}
		} else if (ev == kOSTLSEventClosed && c->state == HS_BODY) {
			/* peer closed mid-body — only OK if no content-length
			 * (HTTP/1.0 style). chunked must have seen final 0. */
			if (c->content_length < 0 && !c->chunked) {
				hctx_finish(c);
				return;
			}
			hctx_fail(c, "https: truncated body");
			return;
		}
	}

	/* No-progress timeout. */
	if (c->state != HS_IDLE && c->state != HS_DONE && c->state != HS_FAIL) {
		unsigned long now = now_ticks();
		if (now - c->progress_ticks > NO_PROGRESS_TICKS) {
			hctx_fail(c, "https: connection timed out");
			return;
		}
	}
}

/* ---------- fetcher_operation_table impl ---------- */

static bool macos9_https_initialise(lwc_string *s) { (void)s; return true; }
static void macos9_https_finalise(lwc_string *s)   { (void)s; }
static bool macos9_https_acceptable(const struct nsurl *u) { (void)u; return true; }

static void *macos9_https_setup(struct fetch *p, struct nsurl *u,
	bool o, bool d, const char *pu,
	const struct fetch_multipart_data *pm, const char **h)
{
	int i, slot = -1;
	lwc_string *host_lwc, *path_lwc, *port_lwc, *query_lwc;
	const char *hs, *ps;
	size_t hs_n, ps_n;
	struct macos9_https_ctx *c;

	(void)o; (void)d; (void)pu; (void)pm; (void)h;

	for (i = 0; i < MAX_HTTPS_F; i++) {
		if (https_slots[i].state == HS_IDLE) { slot = i; break; }
	}
	if (slot < 0) {
		MS_LOG("https_setup: NO FREE SLOTS");
		return NULL;
	}
	c = &https_slots[slot];
	memset(c, 0, sizeof *c);
	c->parent = p;
	c->url = nsurl_ref(u);
	c->state = HS_QUEUED;
	c->content_length = -1;
	c->status = 0;
	c->port = 443;

	host_lwc = nsurl_get_component(u, NSURL_HOST);
	path_lwc = nsurl_get_component(u, NSURL_PATH);
	port_lwc = nsurl_get_component(u, NSURL_PORT);
	if (host_lwc == NULL) {
		MS_LOG("https_setup: no host");
		c->state = HS_IDLE;
		nsurl_unref(c->url); c->url = NULL;
		return NULL;
	}
	hs = lwc_string_data(host_lwc);
	hs_n = lwc_string_length(host_lwc);
	if (hs_n >= sizeof c->host) hs_n = sizeof c->host - 1;
	memcpy(c->host, hs, hs_n);
	c->host[hs_n] = 0;
	lwc_string_unref(host_lwc);

	if (port_lwc) {
		c->port = (unsigned short)atoi(lwc_string_data(port_lwc));
		if (c->port == 0) c->port = 443;
		lwc_string_unref(port_lwc);
	}

	if (path_lwc) {
		ps = lwc_string_data(path_lwc);
		ps_n = lwc_string_length(path_lwc);
		if (ps_n == 0) { c->path[0] = '/'; c->path[1] = 0; }
		else {
			if (ps_n >= sizeof c->path) ps_n = sizeof c->path - 1;
			memcpy(c->path, ps, ps_n);
			c->path[ps_n] = 0;
		}
		lwc_string_unref(path_lwc);
	} else {
		c->path[0] = '/'; c->path[1] = 0;
	}

	/* Append ?query if present — Drupal et al. cache-bust assets with
	 * query strings and respond 400 / 404 if the query is stripped. */
	query_lwc = nsurl_get_component(u, NSURL_QUERY);
	if (query_lwc) {
		const char *qs = lwc_string_data(query_lwc);
		size_t qs_n = lwc_string_length(query_lwc);
		size_t cur = strlen(c->path);
		/* nsurl_get_component(QUERY) returns the query WITHOUT the
		 * leading '?'. Add it ourselves. */
		if (qs_n > 0 && cur + 1 + qs_n < sizeof c->path) {
			c->path[cur] = '?';
			memcpy(c->path + cur + 1, qs, qs_n);
			c->path[cur + 1 + qs_n] = 0;
		}
		lwc_string_unref(query_lwc);
	}

	/* fixes222 — disabled: the cache-hit serving path in fixes218
	 * sent NetSurf into about:query/fetcherror because the synthetic
	 * FETCH_HEADER / FETCH_DATA / FETCH_FINISHED sequence didn't
	 * match what html_create expects. Re-enable once the header
	 * dispatch is reworked to look exactly like the live path. The
	 * cache STORE side stays on (cached bodies just go unused).
	 */

	macsurf_debug_log_writef("https_setup OK host=%s port=%d path=%.40s",
		c->host, (int)c->port, c->path);
	return c;
}

static bool macos9_https_start(void *ctx)
{
	struct macos9_https_ctx *c = (struct macos9_https_ctx *)ctx;
	if (c && c->state == HS_QUEUED) {
		c->progress_ticks = now_ticks();
	}
	return true;
}

static void macos9_https_abort(void *ctx)
{
	struct macos9_https_ctx *c = (struct macos9_https_ctx *)ctx;
	if (c) c->aborted = 1;
}

static void macos9_https_free(void *ctx)
{
	struct macos9_https_ctx *c = (struct macos9_https_ctx *)ctx;
	if (!c) return;
	if (c->state != HS_IDLE) hctx_clear(c);
}

static void macos9_https_poll(lwc_string *s)
{
	int i;
	(void)s;
	for (i = 0; i < MAX_HTTPS_F; i++) {
		if (https_slots[i].state == HS_IDLE) continue;
		if (https_slots[i].aborted &&
		    (https_slots[i].state == HS_QUEUED)) {
			hctx_fail(&https_slots[i], "https: aborted-queued");
			continue;
		}
		hctx_poll(&https_slots[i]);
	}
}

int macos9_https_fetcher_active(void)
{
	int i, n = 0;
	for (i = 0; i < MAX_HTTPS_F; i++) {
		if (https_slots[i].state != HS_IDLE &&
		    https_slots[i].state != HS_DONE &&
		    https_slots[i].state != HS_FAIL) n++;
	}
	return n;
}

nserror macos9_https_fetcher_register(void)
{
	struct fetcher_operation_table ops;
	lwc_string *ss;
	ops.initialise = macos9_https_initialise;
	ops.acceptable = macos9_https_acceptable;
	ops.setup      = macos9_https_setup;
	ops.start      = macos9_https_start;
	ops.abort      = macos9_https_abort;
	ops.free       = macos9_https_free;
	ops.poll       = macos9_https_poll;
	ops.fdset      = NULL;
	ops.finalise   = macos9_https_finalise;
	lwc_intern_string("https", 5, &ss);
	fetcher_add(ss, &ops);
	return NSERROR_OK;
}
