/*
 * MacSurf — macsurf_js.c
 *
 * Core Duktape glue.  Implements our private struct jscontext API
 * (newcontext / destroycontext / exec / pump / smoketest) and forwards
 * NetSurf's js_thread API (js_newheap / js_newthread / js_exec / ...)
 * onto it.
 *
 * In Duktape, a single duk_context is both a heap and a thread for
 * our purposes — we don't use coroutines.  So NetSurf's jsheap and
 * jsthread both resolve to the same struct jscontext under the hood.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "utils/errors.h"
#include "utils/log.h"
#include "content/handlers/javascript/js.h"

#include "duktape.h"
#include "macsurf_js.h"

/*
 * NetSurf's public js.h declares jsheap / jsthread as opaque struct
 * tags.  We alias both to our own struct.  NetSurf never dereferences
 * them — only passes pointers around — so this is safe.
 */
struct jsheap   { struct jscontext ctx; };
struct jsthread { struct jscontext ctx; };

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

	ctx->duk = duk_create_heap(
			NULL,                 /* alloc (use default) */
			NULL,                 /* realloc */
			NULL,                 /* free */
			ctx,                  /* udata */
			macsurf_js_fatal);    /* fatal handler */
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
		nslog_log(__FILE__, "", __LINE__,
				"js_exec error: %s",
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
	/* TODO stage 9: drain queued DOM events here. */
}

/*
 * macsurf_js_smoketest — Milestone 1 proof of life.  Evaluates "1+1"
 * and returns true iff the result is 2.  Also evaluates "typeof window"
 * as a secondary check (must be "object").
 */
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
		nslog_log(__FILE__, "", __LINE__,
				"smoketest: peval 1+1 failed: %s",
				duk_safe_to_string(ctx->duk, -1));
		ok = false;
	} else {
		ival = duk_get_int(ctx->duk, -1);
		if (ival != 2) {
			nslog_log(__FILE__, "", __LINE__,
					"smoketest: 1+1 returned %d, expected 2",
					ival);
			ok = false;
		} else {
			nslog_log(__FILE__, "", __LINE__,
					"smoketest: 1+1 = 2 OK");
		}
	}
	duk_pop(ctx->duk);

	duk_push_string(ctx->duk, "typeof window");
	if (duk_peval(ctx->duk) != 0) {
		nslog_log(__FILE__, "", __LINE__,
				"smoketest: typeof window peval failed: %s",
				duk_safe_to_string(ctx->duk, -1));
		ok = false;
	} else {
		sval = duk_get_string(ctx->duk, -1);
		if (sval == NULL || strcmp(sval, "object") != 0) {
			nslog_log(__FILE__, "", __LINE__,
					"smoketest: typeof window = %s, expected object",
					sval != NULL ? sval : "(null)");
			ok = false;
		} else {
			nslog_log(__FILE__, "", __LINE__,
					"smoketest: typeof window = object OK");
		}
	}
	duk_pop(ctx->duk);

	return ok;
}

/* ----------------------------------------------------------------- */
/* NetSurf js_thread API shim                                         */
/* ----------------------------------------------------------------- */

void
js_initialise(void)
{
	/* Duktape has no global init; contexts are per-thread heaps. */
}

void
js_finalise(void)
{
	/* Nothing to tear down globally. */
}

nserror
js_newheap(int timeout, jsheap **heap)
{
	struct jscontext *ctx;
	jsheap *h;

	(void)timeout;

	if (heap == NULL) {
		return NSERROR_BAD_PARAMETER;
	}

	h = (jsheap *)calloc(1, sizeof(*h));
	if (h == NULL) {
		return NSERROR_NOMEM;
	}

	ctx = macsurf_js_newcontext();
	if (ctx == NULL) {
		free(h);
		return NSERROR_NOMEM;
	}
	/* Copy the jscontext fields into the heap's embedded context. */
	h->ctx = *ctx;
	/* Free the temporary jscontext shell; heap owns the duk. */
	free(ctx);

	/* Run the smoke test once at heap creation so the proof of life
	 * lands in the debug log on every JS-enabled page load. */
	(void)macsurf_js_smoketest(&h->ctx);

	*heap = h;
	return NSERROR_OK;
}

void
js_destroyheap(jsheap *heap)
{
	if (heap == NULL) {
		return;
	}
	if (heap->ctx.duk != NULL) {
		duk_destroy_heap(heap->ctx.duk);
		heap->ctx.duk = NULL;
	}
	free(heap);
}

nserror
js_newthread(jsheap *heap, void *win_priv, void *doc_priv, jsthread **thread)
{
	jsthread *t;

	if (heap == NULL || thread == NULL) {
		return NSERROR_BAD_PARAMETER;
	}

	t = (jsthread *)calloc(1, sizeof(*t));
	if (t == NULL) {
		return NSERROR_NOMEM;
	}
	/* Threads share the heap's duk context in our single-thread model. */
	t->ctx.duk = heap->ctx.duk;
	t->ctx.win_priv = win_priv;
	t->ctx.doc_priv = doc_priv;

	*thread = t;
	return NSERROR_OK;
}

nserror
js_closethread(jsthread *thread)
{
	(void)thread;
	/* Nothing to shut down in the single-thread model — the duk
	 * context is owned by the heap and will outlive the thread. */
	return NSERROR_OK;
}

void
js_destroythread(jsthread *thread)
{
	if (thread != NULL) {
		free(thread);
	}
}

bool
js_exec(jsthread *thread, const uint8_t *txt, size_t txtlen, const char *name)
{
	(void)name;
	if (thread == NULL || txt == NULL) {
		return false;
	}
	return macsurf_js_exec(&thread->ctx, (const char *)txt, txtlen);
}

bool
js_fire_event(jsthread *thread, const char *type,
		struct dom_document *doc, struct dom_node *target)
{
	(void)doc;
	if (thread == NULL || type == NULL || target == NULL) {
		return false;
	}
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
	/* TODO stage 9: register listener in the element's JS wrapper. */
	(void)thread; (void)document; (void)node;
	(void)event_type_dom; (void)js_funcval;
	return false;
}

void
js_handle_new_element(jsthread *thread, struct dom_element *node)
{
	/* TODO stage 9: scan element attributes for on* handlers and
	 * register them against the element's JS wrapper. */
	(void)thread; (void)node;
}

void
js_event_cleanup(jsthread *thread, struct dom_event *evt)
{
	(void)thread; (void)evt;
}
