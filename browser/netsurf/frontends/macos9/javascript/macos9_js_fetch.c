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
		if (nb == NULL) return;	/* OOM: keep what we have */
		*buf = nb;
		*cap = ncap;
	}
	memcpy(*buf + *len, b, (size_t) l);
	*len += l;
	(*buf)[*len] = '\0';
}

/* ---- deferred JS delivery (never called from inside xhr_fetch_cb) ---- */

static void
xhr_deliver(void *p)
{
	struct qjs_xhr_slot *s = (struct qjs_xhr_slot *) p;
	JSContext *ctx;
	JSValue fn, ret, exc;
	const char *body;
	const char *hdrs;
	const char *url_str;

	if (s == NULL || !s->used) return;
	ctx = s->ctx;
	if (ctx == NULL) { xhr_slot_release(s); return; }

	body = (s->resp_buf != NULL) ? s->resp_buf : "";
	hdrs = (s->hdr_buf != NULL) ? s->hdr_buf : "";
	url_str = (s->url != NULL) ? nsurl_access(s->url) : "";

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
			"WORK xhr deliver status=%d err=%d bytes=%ld url=%s",
			s->status, s->is_error, s->resp_len, url_str);

	fn = JS_GetPropertyStr(ctx, s->xhr_obj, "__onNativeComplete");
	if (JS_IsFunction(ctx, fn)) {
		ret = JS_Call(ctx, fn, s->xhr_obj, 0, NULL);
		if (JS_IsException(ret)) {
			exc = JS_GetException(ctx);
			JS_FreeValue(ctx, exc);
			macsurf_debug_log_writef(
					"WORK qjs xhr deliver threw url=%s",
					url_str);
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
		xhr_accum(&s->hdr_buf, &s->hdr_len, &s->hdr_cap,
				QJS_XHR_MAX_HDR_BYTES, &s->hdr_poisoned, b, l);
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
	nsurl *referer = NULL;
	struct content *c = qjs_get_content();
	nserror err;
	struct fetch *out = NULL;

	/* c->llcache can be NULL for a content that isn't (yet, or any
	 * longer) fully live -- content_get_url() -> llcache_handle_get_url()
	 * dereferences it unconditionally and crashes otherwise. Same guard
	 * as macos9_reconvert_host_allowed() (macos9_reconvert.c) uses
	 * before its own content_get_url() call, for the same reason. Caught
	 * by the S0 harness (its minimal test content has no llcache) before
	 * this ever had a chance to reach real hardware. */
	if (c != NULL && c->llcache != NULL) {
		referer = content_get_url(c);	/* borrowed; fetch_start refs it */
	}

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

	/* fixes865 (#291) — resolve the target against the DOCUMENT BASE.
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
	 * document's URL via the same qjs_get_content() + c->llcache NULL-guard
	 * xhr_start_fetch() already uses for the referer (content_get_url()
	 * dereferences llcache unconditionally and crashes on a not-fully-live
	 * content).  No base (JS with no live content) falls back to the old
	 * create, which is right: there is nothing to resolve against. */
	{
		struct content *bc = qjs_get_content();
		nsurl *base = NULL;
		if (bc != NULL && bc->llcache != NULL) {
			base = content_get_url(bc);	/* borrowed */
		}
		if (base != NULL) {
			err = nsurl_join(base, url_c, &url);
		} else {
			err = nsurl_create(url_c, &url);
		}
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
		macsurf_debug_log_writef("WORK xhr send: arena full, url=%s",
				url_c);
		return JS_NewInt32(ctx, -1);
	}

	s->ctx = ctx;
	s->xhr_obj = JS_DupValue(ctx, argv[0]);
	s->url = url;
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
			if (s->body != NULL) {
				memcpy(s->body, body_c, (size_t) s->body_len + 1);
			}
			JS_FreeCString(ctx, body_c);
		}
	}

	/* headers: argv[4] is a plain JS array of "Name: value" strings
	 * (already formatted that way by the JS-side setRequestHeader()). */
	if (argc > 4 && JS_IsArray(argv[4])) {
		JSValue lenv = JS_GetPropertyStr(ctx, argv[4], "length");
		int32_t len32 = 0;
		JS_ToInt32(ctx, &len32, lenv);
		JS_FreeValue(ctx, lenv);
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

	macsurf_debug_log_writef("WORK xhr send method=%s url=%s",
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

void
macos9_js_fetch_install(JSContext *ctx, JSValueConst global)
{
	JS_SetPropertyStr(ctx, global, "__xhrNativeSend",
			JS_NewCFunction(ctx, qjs_xhr_native_send,
					"__xhrNativeSend", 5));
	JS_SetPropertyStr(ctx, global, "__xhrNativeAbort",
			JS_NewCFunction(ctx, qjs_xhr_native_abort,
					"__xhrNativeAbort", 1));
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
