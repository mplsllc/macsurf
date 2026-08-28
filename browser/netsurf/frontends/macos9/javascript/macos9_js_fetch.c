/*
 * MacSurf - macos9_js_fetch.c
 *
 * Native XMLHttpRequest/fetch backend for QuickJS (S3, #167). Before this
 * file, JS had NO real network path: XMLHttpRequest was undefined (fixes845
 * added a logging-only shim that fails safe with status 0) and fetch() was
 * a synchronous fake thenable that always resolved {ok:false,status:0}
 * (fixes843b/845, #167 S1 census -- both confirmed via WORK-gated hardware
 * logging that real Facebook JS never received a single byte of real
 * response data). This closes that gap over content/fetch.h's fetch_start(),
 * the same raw entry point macos9_webfont.c uses to bypass the hlcache/
 * content layer -- calling fetch_start() on an https:// URL transparently
 * routes through macos9_tls_fetcher.c, so cookies, per-host UA, and Sec-
 * Fetch/Origin synthesis all apply automatically with zero extra plumbing
 * (macos9_tls_fetcher.c even reserves the override hook: a caller whose
 * headers[] already contains its own "Sec-Fetch-"/"Origin:" line wins over
 * the auto-synthesis -- see qjs_xhr_native_send()'s header-array build).
 *
 * Two patterns this file mirrors deliberately, both already hardware-proven
 * elsewhere in this codebase:
 *
 *   - Fixed-size, index-addressed arena for JSValue lifetime (never a
 *     linked list) -- the same shape as macsurf_qjs.c's s_timer_arena /
 *     qjs_flush_timers. A linked list could be spliced into a cycle by a
 *     reentrant callback (an XHR onload starting another XHR, evicting the
 *     slot the outer walk still holds); bounded array iteration makes that
 *     structurally impossible. See macos9_js_fetch_flush(), the sibling of
 *     qjs_flush_timers(), called from the exact same navigation-teardown
 *     point in js_newthread().
 *
 *   - Raw fetch_start() accumulation, from macos9_webfont.c's
 *     webfont_fetch_cb(): realloc-doubling body buffer with a hard byte
 *     cap enforced by "poisoning" rather than truncating, and the
 *     "fetch=NULL means the object is freed by NetSurf right after this
 *     callback returns, never touch it again" ownership rule that applies
 *     uniformly to FETCH_FINISHED, FETCH_ERROR, AND FETCH_REDIRECT (all
 *     three sit at or above FETCH_MIN_FINISHED_MSG in the fetch_msg_type
 *     enum, and macos9_tls_fetcher.c's own redirect path tears the fetch
 *     object down with fetch_remove_from_queues()+fetch_free() right after
 *     dispatching FETCH_REDIRECT, same as the terminal messages).
 *
 * JS callbacks (onreadystatechange/onload/onerror, a resolved/rejected
 * fetch() Promise) are NEVER invoked from inside xhr_fetch_cb() itself --
 * that callback can run from deep inside the TLS state machine, and
 * calling back into JS (which can navigate, trigger GC, or start another
 * fetch) from there is not safe. Every terminal event is instead handed to
 * macos9_schedule(0, xhr_deliver, slot), which runs on the next
 * cooperative-scheduler tick, exactly like every other JS-visible
 * continuation in this engine (setTimeout, the reconvert debounce).
 *
 * FETCH_REDIRECT: fetch_start() never auto-follows a redirect (see the
 * "No auto-redirect" note in macos9_tls_fetcher.c's file banner; llcache
 * is the one caller in stock NetSurf that follows it, and this file is XHR's
 * equivalent of that). The old fetch is dead the instant our callback
 * returns (see the ownership rule above), so we null our handle immediately
 * and, if under the hop cap, resolve the (possibly relative) target via
 * nsurl_join() against the CURRENT url and fire a fresh fetch_start()
 * ourselves. Per the fetch/XHR redirect spec: a 303 always downgrades to
 * GET with no body; a 301/302 downgrades to GET+no-body only when the
 * original method was POST; 307/308 always preserve method and body.
 */

#include "macos9.h"

#ifdef WITH_QUICKJS

#include <string.h>
#include <stdlib.h>

#include "utils/ns_errors.h"
#include "utils/nsurl.h"
#include "content/fetch.h"
#include "content/macsurf_nav_seed.h"
#include "macsurf_diag.h"
#include "content/content_protected.h"

#include "macsurf_debug.h"
#include "macsurf_debug_log.h"
#include "macsurf_qjs.h"
#include "macos9_js_fetch.h"

#define QJS_XHR_MAX          16
#define QJS_XHR_MAX_BYTES    (4L * 1024L * 1024L)
#define QJS_XHR_MAX_HDR_BYTES (256L * 1024L)
#define QJS_XHR_MAX_HOPS     6
#define QJS_XHR_MAX_REQ_HDRS 32

struct qjs_xhr_slot {
	int used;
	int id;
	struct fetch *fetch;
	int fetch_live;			/* see xhr_start_fetch's comment */
	JSContext *ctx;
	JSValue xhr_obj;		/* dup'd JS XMLHttpRequest instance */

	nsurl *url;			/* current (post-redirect) target */
	nsurl *referer;		/* owned snapshot of the invoking realm's URL */
	unsigned long nav_id;		/* MacSurf Trace: owning nav, captured at send() */
	unsigned long last_request_id;	/* previous hop's request_id, or 0 */
	unsigned long origin_script_id;	/* MacSurf Trace 1b: script that called send() */
	char method[8];			/* upper-cased, NUL-terminated */
	char *body;			/* owned copy of the send() body, or NULL */
	long body_len;
	char *req_headers[QJS_XHR_MAX_REQ_HDRS + 1]; /* NULL-terminated, owned */

	char *resp_buf;
	long resp_len;
	long resp_cap;
	int resp_poisoned;

	char *hdr_buf;
	long hdr_len;
	long hdr_cap;
	int hdr_poisoned;

	int status;
	int redirect_hops;
	int is_error;			/* network-level failure, not an HTTP status */
	int beacon;			/* sendBeacon slot: no JS delivery, no ctx */
};

static struct qjs_xhr_slot s_xhr_arena[QJS_XHR_MAX];
static int s_xhr_next_id = 1;

static void
xhr_free_req_headers(struct qjs_xhr_slot *s)
{
	int i;
	for (i = 0; i < QJS_XHR_MAX_REQ_HDRS && s->req_headers[i] != NULL; i++) {
		free(s->req_headers[i]);
		s->req_headers[i] = NULL;
	}
}

/* Frees every heap allocation on a slot and marks it free. Does NOT touch
 * s->fetch (caller must fetch_abort() first if still live) or s->xhr_obj
 * (caller must JS_FreeValue() it against the right ctx first -- this
 * function may run after the context is already gone, e.g. from a plain
 * reset path, so it must never touch the JSValue). */
static void
xhr_slot_wipe(struct qjs_xhr_slot *s)
{
	if (s->url != NULL) { nsurl_unref(s->url); s->url = NULL; }
	if (s->referer != NULL) { nsurl_unref(s->referer); s->referer = NULL; }
	if (s->body != NULL) { free(s->body); s->body = NULL; }
	xhr_free_req_headers(s);
	if (s->resp_buf != NULL) { free(s->resp_buf); s->resp_buf = NULL; }
	if (s->hdr_buf != NULL) { free(s->hdr_buf); s->hdr_buf = NULL; }
	s->resp_len = 0; s->resp_cap = 0; s->resp_poisoned = 0;
	s->hdr_len = 0; s->hdr_cap = 0; s->hdr_poisoned = 0;
	s->body_len = 0;
	s->used = 0;
	s->fetch = NULL;
}

static struct qjs_xhr_slot *
xhr_slot_alloc(void)
{
	int i;
	for (i = 0; i < QJS_XHR_MAX; i++) {
		if (!s_xhr_arena[i].used) {
			memset(&s_xhr_arena[i], 0, sizeof(struct qjs_xhr_slot));
			s_xhr_arena[i].used = 1;
			s_xhr_arena[i].id = s_xhr_next_id++;
			if (s_xhr_next_id <= 0) s_xhr_next_id = 1;
			return &s_xhr_arena[i];
		}
	}
	return NULL;
}

static struct qjs_xhr_slot *
xhr_slot_find(int id)
{
	int i;
	if (id <= 0) return NULL;
	for (i = 0; i < QJS_XHR_MAX; i++) {
		if (s_xhr_arena[i].used && s_xhr_arena[i].id == id)
			return &s_xhr_arena[i];
	}
	return NULL;
}

/* Full teardown: abort any live fetch, cancel any pending scheduled
 * delivery, free the dup'd JSValue against its OWN ctx (captured at
 * send() time -- may differ from a later flush's old_ctx during a
 * mid-request navigation, which is exactly why we store it per-slot
 * rather than trusting the caller's ctx), then wipe. */
static void
xhr_slot_release(struct qjs_xhr_slot *s)
{
	if (!s->used) return;
	if (s->fetch != NULL) {
		fetch_abort(s->fetch);
		s->fetch = NULL;
	}
	macos9_schedule_cancel_owner(s);
	if (s->ctx != NULL) {
		JS_FreeValue(s->ctx, s->xhr_obj);
	}
	xhr_slot_wipe(s);
}

/* ---- response accumulation (webfont.c's realloc-doubling pattern) ---- */

static void
xhr_accum(char **buf, long *len, long *cap, long max_bytes, int *poisoned,
		const unsigned char *b, long l)
{
	if (*poisoned) return;
	if (b == NULL || l <= 0) return;
	if (*len + l > max_bytes) {
		*poisoned = 1;
		return;
	}
	if (*len + l > *cap) {
		long ncap = (*cap > 0) ? *cap : 8192L;
		char *nb;
		while (ncap < *len + l) ncap *= 2;
		if (ncap > max_bytes) ncap = max_bytes;
		nb = (char *) realloc(*buf, (size_t) (ncap + 1));
		if (nb == NULL) {
			*poisoned = 1;
			return;
		}
		*buf = nb;
		*cap = ncap;
	}
	memcpy(*buf + *len, b, (size_t) l);
	*len += l;
	(*buf)[*len] = '\0';
}

/* Accumulate ONE response header line, re-adding the terminator the fetchers
 * strip.
 *
 * fixes1098: every FETCH_HEADER carries a single BARE header line. Both
 * fetchers parse with find_line()/mfs_find_line(), which NUL each line's own
 * '\r' and then send len=strlen(p) -- so no '\r' and no '\n' ever reach this
 * callback. xhr_accum() is a pure byte-append, so without re-adding a
 * terminator every header fused into ONE line beginning with the
 * "HTTP/1.1 200 OK" status line. The prelude's getResponseHeader() splits on
 * /\r\n|\n/, found exactly one line, and compared each requested name against
 * "HTTP/1.1 200 OK" -- so EVERY lookup returned null and
 * getAllResponseHeaders() returned a single mashed string.
 *
 * null is not an error: it reads as "that header was not sent", so a caller
 * silently takes its no-such-header branch. That is the LYING ANSWER shape
 * from the fixes1005->1031 batch -- pages break on confident wrong answers,
 * not on missing APIs -- which is why this is worth a named function rather
 * than an inline two-liner.
 *
 * CRLF is what the spec requires getAllResponseHeaders() to join with. */
static void
xhr_accum_header_line(struct qjs_xhr_slot *s, const unsigned char *b, long l)
{
	if (s == NULL) return;
	xhr_accum(&s->hdr_buf, &s->hdr_len, &s->hdr_cap,
			QJS_XHR_MAX_HDR_BYTES, &s->hdr_poisoned, b, l);
	xhr_accum(&s->hdr_buf, &s->hdr_len, &s->hdr_cap,
			QJS_XHR_MAX_HDR_BYTES, &s->hdr_poisoned,
			(const unsigned char *) "\r\n", 2L);
}

/* Harness-only hook: drives the REAL accumulator above over a caller-supplied
 * sequence of bare header lines and returns the exact bytes JS would see in
 * __responseHeadersRaw. Exists so harness Test 61 tests the shipping code path
 * instead of a reimplementation of it -- a test that builds the header string
 * itself passes with or without this fix and therefore proves nothing. */
const char *
macos9_js_fetch_test_accum_headers(const char *const *lines, int nlines)
{
	static struct qjs_xhr_slot t;
	int i;

	if (t.hdr_buf != NULL) { free(t.hdr_buf); t.hdr_buf = NULL; }
	t.hdr_len = 0; t.hdr_cap = 0; t.hdr_poisoned = 0;

	for (i = 0; i < nlines; i++) {
		xhr_accum_header_line(&t, (const unsigned char *) lines[i],
				(long) strlen(lines[i]));
	}
	return (t.hdr_buf != NULL) ? t.hdr_buf : "";
}

/* ---- deferred JS delivery (never called from inside xhr_fetch_cb) ---- */

static void
xhr_deliver(void *p)
{
	struct qjs_xhr_slot *s = (struct qjs_xhr_slot *) p;
	JSContext *ctx;
	JSValue fn, ret, exc, stk;
	double prevdl;
	struct ms_diag_scope __xtsk;	/* MacSurf Trace 1b */
	const char *body;
	const char *hdrs;
	const char *url_str;
	const char *msg = NULL;
	const char *ss = NULL;

	if (s == NULL || !s->used) return;
	/* sendBeacon slots are fire-and-forget: nothing to deliver, and no
	 * JSValue/ctx to touch (the realm may be long gone). The fetch is
	 * over, so just release the C-side allocations. */
	if (s->beacon) { xhr_slot_wipe(s); return; }
	ctx = s->ctx;
	if (ctx == NULL) { xhr_slot_release(s); return; }

	body = (s->resp_buf != NULL) ? s->resp_buf : "";
	hdrs = (s->hdr_buf != NULL) ? s->hdr_buf : "";
	url_str = (s->url != NULL) ? nsurl_access(s->url) : "";
	if (s->resp_poisoned || s->hdr_poisoned) {
		s->is_error = 1;
		macsurf_debug_log_writef(
			"LIFE xhr poisoned response resp=%d hdr=%d bytes=%ld url=%s",
			s->resp_poisoned, s->hdr_poisoned, s->resp_len, url_str);
	}

	JS_SetPropertyStr(ctx, s->xhr_obj, "readyState", JS_NewInt32(ctx, 4));
	JS_SetPropertyStr(ctx, s->xhr_obj, "status",
			JS_NewInt32(ctx, s->is_error ? 0 : s->status));
	JS_SetPropertyStr(ctx, s->xhr_obj, "statusText", JS_NewString(ctx, ""));
	JS_SetPropertyStr(ctx, s->xhr_obj, "responseText",
			JS_NewString(ctx, body));
	JS_SetPropertyStr(ctx, s->xhr_obj, "response", JS_NewString(ctx, body));
	JS_SetPropertyStr(ctx, s->xhr_obj, "responseURL",
			JS_NewString(ctx, url_str));
	JS_SetPropertyStr(ctx, s->xhr_obj, "__responseHeadersRaw",
			JS_NewString(ctx, hdrs));

	macsurf_debug_log_writef(
			"LIFE xhr deliver status=%d err=%d bytes=%ld url=%s",
			s->status, s->is_error, s->resp_len, url_str);

	fn = JS_GetPropertyStr(ctx, s->xhr_obj, "__onNativeComplete");
	if (JS_IsFunction(ctx, fn)) {
		prevdl = macsurf_qjs_deadline_push_ms(
				macsurf_qjs_default_timeout_ms());
		ms_diag_task_enter(&__xtsk, MS_TASK_XHR, s->nav_id,
			s->origin_script_id, s->last_request_id, (const char *) 0);
		ret = JS_Call(ctx, fn, s->xhr_obj, 0, NULL);
		ms_diag_task_leave(&__xtsk);
		macsurf_qjs_deadline_pop(prevdl);
		if (JS_IsException(ret)) {
			exc = JS_GetException(ctx);
			msg = JS_ToCString(ctx, exc);
			macsurf_debug_log_writef(
					"LIFE qjs xhr deliver threw: %s url=%s",
					msg ? msg : "?", url_str);
			if (msg) JS_FreeCString(ctx, msg);
			stk = JS_GetPropertyStr(ctx, exc, "stack");
			if (JS_IsString(stk)) {
				ss = JS_ToCString(ctx, stk);
				if (ss != NULL) {
					macsurf_debug_log_writef(
						"LIFE qjs xhr deliver stack: %s url=%s",
						ss, url_str);
					JS_FreeCString(ctx, ss);
				}
			}
			JS_FreeValue(ctx, stk);
			JS_FreeValue(ctx, exc);
		}
		JS_FreeValue(ctx, ret);
	}
	JS_FreeValue(ctx, fn);

	xhr_slot_release(s);
}

/* ---- redirect target resolution + method downgrade ---- */

static void
xhr_apply_redirect_method(struct qjs_xhr_slot *s, int status)
{
	int was_post = (strcmp(s->method, "POST") == 0);
	if (status == 303 || (was_post && (status == 301 || status == 302))) {
		strcpy(s->method, "GET");
		if (s->body != NULL) { free(s->body); s->body = NULL; }
		s->body_len = 0;
	}
	/* 307/308 (and any non-matching case): preserve method + body. */
}

static int xhr_start_fetch(struct qjs_xhr_slot *s);

/* The document URL for the realm executing this native binding.  It is a
 * borrowed URL: callers that need it beyond this synchronous operation retain
 * it in their slot with nsurl_ref(). */
static nsurl *
xhr_realm_url(JSContext *ctx)
{
	struct content *content = qjs_get_content_for_ctx(ctx);
	if (content == NULL || content->llcache == NULL) return NULL;
	return content_get_url(content);
}

/* MacSurf Trace 1a: the nav owning the realm that issued this XHR. Captured
 * once at send() so redirect hops (which run later, after realm state has
 * moved on) stay attributed to the originating navigation. */
static unsigned long
xhr_realm_nav_id(JSContext *ctx)
{
	extern unsigned long content_get_nav_id(struct content *c);
	return content_get_nav_id(qjs_get_content_for_ctx(ctx));
}

static void
xhr_follow_redirect(struct qjs_xhr_slot *s, const char *target)
{
	nsurl *joined = NULL;
	nserror err;

	if (s->redirect_hops >= QJS_XHR_MAX_HOPS) {
		s->is_error = 1;
		macos9_schedule(0, xhr_deliver, s);
		return;
	}
	err = nsurl_join(s->url, target, &joined);
	if (err != NSERROR_OK || joined == NULL) {
		s->is_error = 1;
		macos9_schedule(0, xhr_deliver, s);
		return;
	}

	/* fixes961 - never follow an https -> http redirect from JS.
	 *
	 * This is not a hypothetical. When an HTTPS fetch fails for a
	 * non-certificate reason, hctx_fail synthesises a REAL FETCH_REDIRECT
	 * to the http:// equivalent with http code 301 (the retro HTTP-only
	 * fallback, fixes249b/317). That is a considered trade for a top-level
	 * page the user typed, but it is the wrong answer for an XHR: this
	 * function only checked the hop cap, so a
	 * fetch('https://site/api/session') that hit a transient TLS failure
	 * was silently reissued over port 80 -- and the fetcher attaches
	 * cookies via urldb_get_cookie on the way out, so the session token
	 * goes across in the clear. The page's own JS never sees that it
	 * happened.
	 *
	 * Downgrades are refused here rather than in the fetcher because the
	 * fetcher's fallback is legitimate for the navigation case; it is
	 * following it from script, with credentials, that is not. */
	{
		/* Compared as strings rather than via corestring_lwc_https:
		 * corestrings.h is not on this TU's include list, and pulling
		 * it in for two comparisons is not worth the header churn on a
		 * flat-namespace CW8 build. */
		const char *cur = nsurl_access(s->url);
		const char *nxt = nsurl_access(joined);
		int downgrade = (cur != NULL && nxt != NULL &&
				 strncmp(cur, "https://", 8) == 0 &&
				 strncmp(nxt, "http://", 7) == 0);

		if (downgrade) {
			macsurf_debug_log_writef(
				"LIFE xhr INVALID https->http redirect refused: %s",
				nsurl_access(joined));
			nsurl_unref(joined);
			s->is_error = 1;
			macos9_schedule(0, xhr_deliver, s);
			return;
		}
	}

	nsurl_unref(s->url);
	s->url = joined;
	s->redirect_hops++;
	xhr_apply_redirect_method(s, s->status);

	s->resp_len = 0; s->resp_poisoned = 0;
	s->hdr_len = 0; s->hdr_poisoned = 0;

	if (xhr_start_fetch(s) != 0) {
		s->is_error = 1;
		macos9_schedule(0, xhr_deliver, s);
	}
}

/* ---- fetch_start() callback ---- */

static void
xhr_fetch_cb(const fetch_msg *msg, void *pw)
{
	struct qjs_xhr_slot *s = (struct qjs_xhr_slot *) pw;

	if (s == NULL || !s->used) return;

	switch (msg->type) {
	case FETCH_HEADER: {
		const unsigned char *b = msg->data.header_or_data.buf;
		long l = (long) msg->data.header_or_data.len;
		xhr_accum_header_line(s, b, l);
		break;
	}
	case FETCH_DATA: {
		const unsigned char *b = msg->data.header_or_data.buf;
		long l = (long) msg->data.header_or_data.len;
		xhr_accum(&s->resp_buf, &s->resp_len, &s->resp_cap,
				QJS_XHR_MAX_BYTES, &s->resp_poisoned, b, l);
		break;
	}
	case FETCH_REDIRECT: {
		const char *target = msg->data.redirect;
		if (s->fetch != NULL) s->status = (int) fetch_http_code(s->fetch);
		s->fetch = NULL;
		s->fetch_live = 0;
		if (target != NULL) {
			xhr_follow_redirect(s, target);
		} else {
			s->is_error = 1;
			macos9_schedule(0, xhr_deliver, s);
		}
		break;
	}
	case FETCH_FINISHED:
		if (s->fetch != NULL) s->status = (int) fetch_http_code(s->fetch);
		s->fetch = NULL;
		s->fetch_live = 0;
		macos9_schedule(0, xhr_deliver, s);
		break;
	case FETCH_ERROR:
		/* Salvage (mirrors webfont's close-delimited-response case): a
		 * connection-close-delimited body can arrive complete and only
		 * THEN report the close as FETCH_ERROR. If we already saw a
		 * status line and accumulated a body, treat it as finished
		 * rather than a hard network error. */
		if (s->fetch != NULL) {
			if (s->status == 0)
				s->status = (int) fetch_http_code(s->fetch);
		}
		s->fetch = NULL;
		s->fetch_live = 0;
		if (s->status == 0 || s->resp_len == 0)
			s->is_error = 1;
		macos9_schedule(0, xhr_deliver, s);
		break;
	default:
		/* FETCH_PROGRESS / FETCH_TIMEDOUT / FETCH_NOTMODIFIED / FETCH_AUTH
		 * / FETCH_CERT* / FETCH_SSL_ERR: no XHR-visible equivalent yet. */
		break;
	}
}

/* ---- issuing the actual fetch_start() call (shared by send() + redirect) --- */

static int
xhr_start_fetch(struct qjs_xhr_slot *s)
{
	nsurl *referer;
	nserror err;
	struct fetch *out = NULL;

	/* The owner was captured at JS call time.  This function also runs after
	 * redirects and after beacon navigation, when consulting a current global
	 * realm would send the wrong referrer or dereference a dead one. */
	referer = s->referer;

	/* Every real macos9 fetcher is poll-driven and never completes inside
	 * its own start() callback, so in production xhr_fetch_cb can't fire
	 * before fetch_start() returns. Guard anyway: if some fetcher DID
	 * resolve synchronously, xhr_fetch_cb's terminal cases already set
	 * fetch_live=0 and s->fetch=NULL (and may have re-entered this very
	 * function via a redirect). Blindly assigning fetch_start()'s output
	 * afterwards would resurrect a handle that's already been freed by
	 * the fetcher -- so only store it if nothing terminal happened while
	 * the call was in flight. */
	s->fetch_live = 1;
	/* MacSurf Trace 1a: seed the fetch boundary from the slot's captured
	 * owner (never a current global); each hop gets a fresh request_id and
	 * points redirect_from at the previous one. */
	macsurf_fetch_seed(s->nav_id, s->last_request_id);
	err = fetch_start(s->url, referer, xhr_fetch_cb, s,
			false /* only_2xx: XHR must see 4xx/5xx bodies too */,
			(s->body != NULL && strcmp(s->method, "GET") != 0) ?
				s->body : NULL,
			NULL, false, false,
			(const char **) s->req_headers, &out);
	if (err != NSERROR_OK || out == NULL) {
		s->fetch_live = 0;
		return -1;
	}
	s->last_request_id = fetch_get_request_id(out);
	if (s->fetch_live) {
		s->fetch = out;
	}
	return 0;
}

/* ---- JS-callable entry points ---- */

static JSValue
qjs_xhr_native_send(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv)
{
	struct qjs_xhr_slot *s;
	const char *method_c;
	const char *url_c;
	const char *body_c = NULL;
	nsurl *url = NULL;
	nsurl *base;
	nserror err;
	int i;

	(void) this_val;

	if (argc < 3) return JS_NewInt32(ctx, -1);

	method_c = JS_ToCString(ctx, argv[1]);
	url_c = JS_ToCString(ctx, argv[2]);
	if (method_c == NULL || url_c == NULL) {
		if (method_c) JS_FreeCString(ctx, method_c);
		if (url_c) JS_FreeCString(ctx, url_c);
		return JS_NewInt32(ctx, -1);
	}

	/* fixes865 (#291) - resolve the target against the DOCUMENT BASE.
	 *
	 * This was a bare nsurl_create(url_c), which only works for an absolute
	 * URL.  Real pages overwhelmingly pass root-relative ones, and those came
	 * out with no scheme and no host, so the fetch died instantly:
	 *   WORK xhr event=send GET /wp-includes/js/dist/vendor/wp-polyfill.min.js
	 *   WORK fetch url=/wp-includes/js/dist/vendor/wp-polyfill.min.js ok=0 status=0
	 * (hackaday's comment iframe: its verbum loader's urls{} are all
	 * root-relative, so BOTH wp-polyfill and verbum-comments.js failed and the
	 * form never loaded.)
	 *
	 * Note the redirect path in this same file already does it right --
	 * `nsurl_join(s->url, target, &joined)` -- so relative handling existed;
	 * only the INITIAL send lacked it.  Same shape as the timer-vs-XHR arena
	 * asymmetry (fixes854): the newer/second path learned the lesson, the
	 * first one never had it.
	 *
	 * nsurl_join is RFC-3986 and passes an absolute target straight through,
	 * so this is a strict superset of the old behaviour.  Base is the current
	 * document's URL from the invoking realm (content_get_url()
	 * dereferences llcache unconditionally and crashes on a not-fully-live
	 * content).  No base (JS with no live content) falls back to the old
	 * create, which is right: there is nothing to resolve against. */
	base = xhr_realm_url(ctx);
	if (base != NULL) {
		err = nsurl_join(base, url_c, &url);
	} else {
		err = nsurl_create(url_c, &url);
	}
	if (err != NSERROR_OK || url == NULL) {
		JS_FreeCString(ctx, method_c);
		JS_FreeCString(ctx, url_c);
		return JS_NewInt32(ctx, -1);
	}

	s = xhr_slot_alloc();
	if (s == NULL) {
		nsurl_unref(url);
		JS_FreeCString(ctx, method_c);
		JS_FreeCString(ctx, url_c);
		macsurf_debug_log_writef("LIFE xhr send: arena full, url=%s",
				url_c);
		return JS_NewInt32(ctx, -1);
	}

	s->ctx = ctx;
	s->xhr_obj = JS_DupValue(ctx, argv[0]);
	s->url = url;
	s->referer = (base != NULL) ? nsurl_ref(base) : NULL;
	s->nav_id = xhr_realm_nav_id(ctx);	/* MacSurf Trace 1a */
	s->last_request_id = 0;
	s->origin_script_id = ms_diag_cur_script();	/* MacSurf Trace 1b */
	strncpy(s->method, method_c, sizeof(s->method) - 1);
	s->method[sizeof(s->method) - 1] = '\0';
	for (i = 0; s->method[i]; i++) {
		if (s->method[i] >= 'a' && s->method[i] <= 'z')
			s->method[i] = (char) (s->method[i] - 32);
	}
	JS_FreeCString(ctx, method_c);

	if (argc > 3 && !JS_IsNull(argv[3]) && !JS_IsUndefined(argv[3])) {
		body_c = JS_ToCString(ctx, argv[3]);
		if (body_c != NULL) {
			s->body_len = (long) strlen(body_c);
			s->body = (char *) malloc((size_t) s->body_len + 1);
			if (s->body == NULL) {
				JS_FreeCString(ctx, body_c);
				JS_FreeCString(ctx, url_c);
				xhr_slot_release(s);
				return JS_NewInt32(ctx, -1);
			}
			memcpy(s->body, body_c, (size_t) s->body_len + 1);
			JS_FreeCString(ctx, body_c);
		}
	}

	/* headers: argv[4] is a plain JS array of "Name: value" strings
	 * (already formatted that way by the JS-side setRequestHeader()). */
	if (argc > 4 && JS_IsArray(argv[4])) {
		JSValue lenv = JS_GetPropertyStr(ctx, argv[4], "length");
		int32_t len32 = 0;
		if (JS_ToInt32(ctx, &len32, lenv) < 0) len32 = 0;
		JS_FreeValue(ctx, lenv);
		if (len32 < 0) len32 = 0;
		if (len32 > QJS_XHR_MAX_REQ_HDRS) len32 = QJS_XHR_MAX_REQ_HDRS;
		for (i = 0; i < len32; i++) {
			JSValue hv = JS_GetPropertyUint32(ctx, argv[4], (uint32_t) i);
			const char *hs = JS_ToCString(ctx, hv);
			if (hs != NULL) {
				s->req_headers[i] = (char *) malloc(strlen(hs) + 1);
				if (s->req_headers[i] != NULL)
					strcpy(s->req_headers[i], hs);
				JS_FreeCString(ctx, hs);
			}
			JS_FreeValue(ctx, hv);
		}
		s->req_headers[len32] = NULL;
	}

	macsurf_debug_log_writef("LIFE xhr send method=%s url=%s",
			s->method, url_c);
	JS_FreeCString(ctx, url_c);

	if (xhr_start_fetch(s) != 0) {
		xhr_slot_release(s);
		return JS_NewInt32(ctx, -1);
	}

	return JS_NewInt32(ctx, s->id);
}

static JSValue
qjs_xhr_native_abort(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv)
{
	struct qjs_xhr_slot *s;
	int32_t id = 0;

	(void) ctx; (void) this_val;

	if (argc < 1) return JS_UNDEFINED;
	JS_ToInt32(ctx, &id, argv[0]);
	s = xhr_slot_find(id);
	if (s != NULL) xhr_slot_release(s);
	return JS_UNDEFINED;
}

/* ---- navigator.sendBeacon: fire-and-forget POST ----
 *
 * Analytics (Google Analytics, gtag, etc.) call sendBeacon on page unload
 * and the data was silently lost while it returned false. This is a real
 * POST over the SAME slot arena + fetch_start() path as XHR, but with the
 * delivery half removed: no JS callback ever fires, and the slot carries
 * no ctx affinity so a navigation (macos9_js_fetch_flush) does NOT abort
 * an in-flight beacon -- surviving unload is the entire point of the API.
 * The slot simply wipes itself when the fetch terminates (xhr_deliver's
 * beacon branch), including the redirect case (redirects ARE followed for
 * beacons; a 301/302/303 downgrades POST to GET per xhr_apply_redirect_
 * method, which is the fetch spec's behaviour). */
static JSValue
qjs_beacon_send(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv)
{
	struct qjs_xhr_slot *s;
	const char *url_c;
	const char *body_c = NULL;
	nsurl *url = NULL;
	nsurl *base;
	nserror err;

	(void) this_val;

	if (argc < 1) return JS_FALSE;

	url_c = JS_ToCString(ctx, argv[0]);
	if (url_c == NULL) return JS_FALSE;

	/* Resolve against the document base, same as xhr's initial send
	 * (fixes865): analytics targets are commonly root-relative. */
	base = xhr_realm_url(ctx);
	if (base != NULL) {
		err = nsurl_join(base, url_c, &url);
	} else {
		err = nsurl_create(url_c, &url);
	}
	if (err != NSERROR_OK || url == NULL) {
		JS_FreeCString(ctx, url_c);
		return JS_FALSE;
	}
	/* sendBeacon only transports over http(s); any other scheme is an
	 * invalid URL for it. */
	{
		const char *surl = nsurl_access(url);
		if (surl == NULL ||
		    (strncmp(surl, "http://", 7) != 0 &&
		     strncmp(surl, "https://", 8) != 0)) {
			nsurl_unref(url);
			JS_FreeCString(ctx, url_c);
			return JS_FALSE;
		}
	}

	s = xhr_slot_alloc();
	if (s == NULL) {
		nsurl_unref(url);
		JS_FreeCString(ctx, url_c);
		macsurf_debug_log_writef("WORK beacon send: arena full, url=%s",
				url_c);
		return JS_FALSE;
	}

	s->beacon = 1;
	s->ctx = NULL;			/* flush() skips beacons: keep flying */
	s->xhr_obj = JS_UNDEFINED;
	s->url = url;
	s->referer = (base != NULL) ? nsurl_ref(base) : NULL;
	s->nav_id = xhr_realm_nav_id(ctx);	/* MacSurf Trace 1a */
	s->last_request_id = 0;
	s->origin_script_id = ms_diag_cur_script();	/* MacSurf Trace 1b */
	strcpy(s->method, "POST");

	if (argc > 1 && !JS_IsNull(argv[1]) && !JS_IsUndefined(argv[1])) {
		body_c = JS_ToCString(ctx, argv[1]);
		if (body_c != NULL) {
			s->body_len = (long) strlen(body_c);
			s->body = (char *) malloc((size_t) s->body_len + 1);
			if (s->body == NULL) {
				JS_FreeCString(ctx, body_c);
				JS_FreeCString(ctx, url_c);
				xhr_slot_release(s);
				return JS_FALSE;
			}
			memcpy(s->body, body_c, (size_t) s->body_len + 1);
			JS_FreeCString(ctx, body_c);
		}
	}

	/* sendBeacon's default media type for a string body is text/plain.
	 * strlen+1, not a hardcoded 24: "Content-Type: text/plain" is 25
	 * chars, and a 24-byte malloc let strcpy write 2 bytes past the end
	 * (caught by the harness ASan build). */
	s->req_headers[0] = (char *) malloc(
			strlen("Content-Type: text/plain") + 1);
	if (s->req_headers[0] != NULL)
		strcpy(s->req_headers[0], "Content-Type: text/plain");
	s->req_headers[1] = NULL;

	macsurf_debug_log_writef("WORK beacon send url=%s bytes=%ld",
			url_c, (long) s->body_len);
	JS_FreeCString(ctx, url_c);

	if (xhr_start_fetch(s) != 0) {
		xhr_slot_release(s);
		return JS_FALSE;
	}
	return JS_TRUE;
}

void
macos9_js_fetch_install(JSContext *ctx, JSValueConst global)
{
	JS_SetPropertyStr(ctx, global, "__xhrNativeSend",
			JS_NewCFunction(ctx, qjs_xhr_native_send,
					"__xhrNativeSend", 5));
	JS_SetPropertyStr(ctx, global, "__xhrNativeAbort",
			JS_NewCFunction(ctx, qjs_xhr_native_abort,
					"__xhrNativeAbort", 1));
	/* navigator.sendBeacon backend. */
	JS_SetPropertyStr(ctx, global, "__beaconSend",
			JS_NewCFunction(ctx, qjs_beacon_send,
					"__beaconSend", 2));
}

void
macos9_js_fetch_flush(JSContext *old_ctx)
{
	int i;
	if (old_ctx == NULL) return;
	for (i = 0; i < QJS_XHR_MAX; i++) {
		if (s_xhr_arena[i].used && s_xhr_arena[i].ctx == old_ctx) {
			xhr_slot_release(&s_xhr_arena[i]);
		}
	}
}

#endif /* WITH_QUICKJS */
