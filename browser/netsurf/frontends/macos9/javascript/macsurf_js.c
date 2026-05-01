/*
 * MacSurf -- macsurf_js.c
 *
 * Core Duktape glue.  Implements our private jscontext API and forwards
 * NetSurf's js_thread API (js_newheap / js_newthread / js_exec / ...)
 * onto it.
 *
 * In Duktape, a single duk_context is both a heap and a thread for
 * our purposes.  NetSurf's jsheap and jsthread both resolve to the same
 * struct jscontext under the hood.
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * cbrt -- C99 cube root, missing from MSL on Carbon.  Duktape's
 * Math.cbrt calls this from duk__cbrt.  Lives in this always-linked
 * TU (when WITH_DUKTAPE is on) so it is available regardless of link
 * order.
 */
double cbrt(double x);
double cbrt(double x)
{
	if (x == 0.0) return 0.0;
	if (x < 0.0)  return -pow(-x, 1.0 / 3.0);
	return pow(x, 1.0 / 3.0);
}

#include "utils/errors.h"
#include "content/handlers/javascript/js.h"
#include "duktape.h"
#include "macsurf_js.h"
#include "macsurf_debug.h"

#ifdef WITH_DUKTAPE

/*
 * NetSurf's public js.h declares jsheap / jsthread as opaque struct
 * tags.  We alias both to struct jscontext (defined in macsurf_js.h).
 * NetSurf never dereferences them -- only passes pointers around.
 */
struct jsheap {
	struct jscontext ctx;
	struct jsheap   *next;
};

struct jsthread {
	struct jscontext ctx;
};

static struct jsheap *macsurf_js_heaps = NULL;

void
macsurf_js_pump_all(void)
{
	struct jsheap *h;
	for (h = macsurf_js_heaps; h != NULL; h = h->next) {
		macsurf_js_pump(&h->ctx);
	}
}

/* ----------------------------------------------------------------- */
/* Lifecycle                                                          */
/* ----------------------------------------------------------------- */

struct jscontext *
macsurf_js_newcontext(void)
{
	struct jscontext *ctx;

	ctx = (struct jscontext *)calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		return NULL;
	}

	ctx->duk = duk_create_heap(NULL, NULL, NULL, ctx, macsurf_js_fatal);
	if (ctx->duk == NULL) {
		free(ctx);
		return NULL;
	}

	macsurf_js_setup_globals(ctx->duk);
	return ctx;
}

void
macsurf_js_destroycontext(struct jscontext *ctx)
{
	if (ctx == NULL) {
		return;
	}
	if (ctx->duk != NULL) {
		duk_destroy_heap(ctx->duk);
		ctx->duk = NULL;
	}
	free(ctx);
}

/* ----------------------------------------------------------------- */
/* Execution                                                          */
/* ----------------------------------------------------------------- */

bool
macsurf_js_exec(struct jscontext *ctx, const char *src, size_t srclen)
{
	int rc;

	if (ctx == NULL || ctx->duk == NULL || src == NULL) {
		return false;
	}
	rc = duk_peval_lstring(ctx->duk, src, srclen);
	if (rc != 0) {
		macsurf_debug_log_writef("js_exec err: %s",
				duk_safe_to_string(ctx->duk, -1));
		duk_pop(ctx->duk);
		return false;
	}
	duk_pop(ctx->duk);
	return true;
}

void
macsurf_js_pump(struct jscontext *ctx)
{
	if (ctx == NULL || ctx->duk == NULL) {
		return;
	}
	macsurf_js_run_timers(ctx);
}

bool
macsurf_js_eval_string(struct jscontext *ctx, const char *src,
		char *out, size_t outlen)
{
	const char *s;
	size_t n;

	if (out == NULL || outlen == 0) return false;
	out[0] = 0;
	if (ctx == NULL || ctx->duk == NULL || src == NULL) return false;

	if (duk_peval_string(ctx->duk, src) != 0) {
		s = duk_safe_to_string(ctx->duk, -1);
		if (s != NULL) {
			n = strlen(s);
			if (n >= outlen) n = outlen - 1;
			memcpy(out, s, n);
			out[n] = 0;
		}
		duk_pop(ctx->duk);
		return false;
	}
	s = duk_safe_to_string(ctx->duk, -1);
	if (s != NULL) {
		n = strlen(s);
		if (n >= outlen) n = outlen - 1;
		memcpy(out, s, n);
		out[n] = 0;
	}
	duk_pop(ctx->duk);
	return true;
}

bool
macsurf_js_smoketest(struct jscontext *ctx)
{
	bool ok = true;
	int ival;
	const char *sval;

	if (ctx == NULL || ctx->duk == NULL) {
		return false;
	}

	duk_push_string(ctx->duk, "1+1");
	if (duk_peval(ctx->duk) != 0) {
		ok = false;
	} else {
		ival = duk_get_int(ctx->duk, -1);
		if (ival != 2) ok = false;
	}
	duk_pop(ctx->duk);

	duk_push_string(ctx->duk, "typeof window");
	if (duk_peval(ctx->duk) != 0) {
		ok = false;
	} else {
		sval = duk_get_string(ctx->duk, -1);
		if (sval == NULL || strcmp(sval, "object") != 0) ok = false;
	}
	duk_pop(ctx->duk);

	return ok;
}

/* ----------------------------------------------------------------- */
/* console.log capture buffer                                         */
/* ----------------------------------------------------------------- */

#define MACSURF_JS_CONSOLE_CAP 8192
static char   macsurf_js_console_buf[MACSURF_JS_CONSOLE_CAP];
static size_t macsurf_js_console_len = 0;

const char *
macsurf_js_console_get(void)
{
	macsurf_js_console_buf[macsurf_js_console_len] = 0;
	return macsurf_js_console_buf;
}

void
macsurf_js_console_reset(void)
{
	macsurf_js_console_len = 0;
	macsurf_js_console_buf[0] = 0;
}

void
macsurf_js_console_append(const char *line)
{
	size_t n;
	if (line == NULL) return;
	n = strlen(line);
	if (n == 0) return;
	if (macsurf_js_console_len + n + 2 >= MACSURF_JS_CONSOLE_CAP) {
		n = MACSURF_JS_CONSOLE_CAP - macsurf_js_console_len - 2;
		if (n == 0) return;
	}
	memcpy(macsurf_js_console_buf + macsurf_js_console_len, line, n);
	macsurf_js_console_len += n;
	macsurf_js_console_buf[macsurf_js_console_len++] = '\n';
	macsurf_js_console_buf[macsurf_js_console_len] = 0;
}

/* ----------------------------------------------------------------- */
/* <script> extractor                                                 */
/* ----------------------------------------------------------------- */

static int
ci_match(const char *p, const char *end, const char *needle)
{
	size_t i;
	for (i = 0; needle[i] != 0; i++) {
		char c;
		if (p + i >= end) return 0;
		c = p[i];
		if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
		if (c != needle[i]) return 0;
	}
	return 1;
}

int
macsurf_js_run_scripts_in_html(struct jscontext *ctx,
		const char *src, size_t srclen)
{
	const char *p   = src;
	const char *end = src + srclen;
	int count = 0;

	if (ctx == NULL || ctx->duk == NULL || src == NULL || srclen == 0) {
		return 0;
	}

	while (p < end) {
		const char *body_start;
		const char *body_end;

		while (p < end && *p != '<') p++;
		if (p >= end) break;
		if (!ci_match(p + 1, end, "script")) { p++; continue; }
		while (p < end && *p != '>') p++;
		if (p >= end) break;
		body_start = ++p;
		body_end = NULL;
		while (p + 8 < end) {
			if (p[0] == '<' && p[1] == '/' &&
			    ci_match(p + 2, end, "script")) {
				body_end = p;
				break;
			}
			p++;
		}
		if (body_end == NULL) break;

		if (duk_peval_lstring(ctx->duk, body_start,
				(duk_size_t)(body_end - body_start)) != 0) {
			const char *err = duk_safe_to_string(ctx->duk, -1);
			char line[256];
			line[0] = 0;
			if (err != NULL) {
				size_t en = strlen(err);
				if (en > 240) en = 240;
				memcpy(line, "ERR: ", 5);
				memcpy(line + 5, err, en);
				line[5 + en] = 0;
			}
			macsurf_js_console_append(line);
		}
		duk_pop(ctx->duk);
		count++;

		while (p < end && *p != '>') p++;
		if (p < end) p++;
	}
	return count;
}

/* ----------------------------------------------------------------- */
/* NetSurf js_thread API shim                                         */
/* ----------------------------------------------------------------- */

void
js_initialise(void)
{
}

void
js_finalise(void)
{
}

nserror
js_newheap(int timeout, jsheap **heap)
{
	struct jscontext *ctx;
	jsheap *h;

	(void)timeout;
	if (heap == NULL) return NSERROR_BAD_PARAMETER;

	h = (jsheap *)calloc(1, sizeof(*h));
	if (h == NULL) return NSERROR_NOMEM;

	ctx = macsurf_js_newcontext();
	if (ctx == NULL) { free(h); return NSERROR_NOMEM; }

	h->ctx    = *ctx;
	free(ctx);

	h->next          = macsurf_js_heaps;
	macsurf_js_heaps = h;

	*heap = h;
	return NSERROR_OK;
}

void
js_destroyheap(jsheap *heap)
{
	struct jsheap **pp;

	if (heap == NULL) return;

	pp = &macsurf_js_heaps;
	while (*pp != NULL) {
		if (*pp == heap) { *pp = heap->next; break; }
		pp = &(*pp)->next;
	}

	if (heap->ctx.duk != NULL) {
		duk_destroy_heap(heap->ctx.duk);
		heap->ctx.duk = NULL;
	}
	free(heap);
}

nserror
js_newthread(jsheap *heap, void *win_priv, void *doc_priv,
		jsthread **thread)
{
	jsthread *t;

	if (heap == NULL || thread == NULL) return NSERROR_BAD_PARAMETER;

	t = (jsthread *)calloc(1, sizeof(*t));
	if (t == NULL) return NSERROR_NOMEM;

	t->ctx.duk      = heap->ctx.duk;
	t->ctx.win_priv = win_priv;
	t->ctx.doc_priv = doc_priv;

	*thread = t;
	return NSERROR_OK;
}

nserror
js_closethread(jsthread *thread)
{
	(void)thread;
	return NSERROR_OK;
}

void
js_destroythread(jsthread *thread)
{
	if (thread != NULL) free(thread);
}

bool
js_exec(jsthread *thread, const uint8_t *txt, size_t txtlen,
		const char *name)
{
	(void)name;
	if (thread == NULL || txt == NULL) return false;
	return macsurf_js_exec(&thread->ctx, (const char *)txt, txtlen);
}

bool
js_fire_event(jsthread *thread, const char *type,
		struct dom_document *doc, struct dom_node *target)
{
	(void)doc;
	if (thread == NULL || type == NULL || target == NULL) return false;
	return macsurf_js_dispatch_event(&thread->ctx,
			(struct dom_element *)target, type);
}

bool
js_dom_event_add_listener(jsthread *thread,
		struct dom_document *document,
		struct dom_node *node,
		struct dom_string *event_type_dom,
		void *js_funcval)
{
	(void)thread; (void)document; (void)node;
	(void)event_type_dom; (void)js_funcval;
	return false;
}

void
js_handle_new_element(jsthread *thread, struct dom_element *node)
{
	(void)thread; (void)node;
}

void
js_event_cleanup(jsthread *thread, struct dom_event *evt)
{
	(void)thread; (void)evt;
}

#endif /* WITH_DUKTAPE */
