/*
 * MacSurf — macsurf_js.h
 *
 * Internal interface for the Duktape JS engine glue.  Provides:
 *   - struct jscontext: our wrapper around a duk_context
 *   - lifecycle: newcontext / destroycontext
 *   - execution: exec / pump / smoketest
 *   - date provider and fatal handler hooks shared with other TUs
 *
 * This is the private header for the MacSurf-side glue layer.  The
 * public NetSurf js_thread API (js_newheap / js_newthread / js_exec /
 * ...) is declared in content/handlers/javascript/js.h and is wired
 * onto this layer in macsurf_js.c.
 */

#ifndef MACSURF_JS_H
#define MACSURF_JS_H

#include <stdbool.h>
#include <stddef.h>

#include "duktape.h"

/*
 * jscontext — our per-page JS context.  For MacSurf every NetSurf
 * jsheap and jsthread is ultimately backed by one of these.  Kept
 * minimal; DOM wiring state lives inside the duk heap as stash keys.
 */
struct jscontext {
	duk_context *duk;
	void *win_priv;   /* browser_window opaque */
	void *doc_priv;   /* dom_document opaque */
};

/* Lifecycle */
struct jscontext *macsurf_js_newcontext(void);
void              macsurf_js_destroycontext(struct jscontext *ctx);

/* Execution */
bool macsurf_js_exec(struct jscontext *ctx, const char *src, size_t srclen);
void macsurf_js_pump(struct jscontext *ctx);
bool macsurf_js_smoketest(struct jscontext *ctx);

/* Evaluate src and write the toString'd result into `out` (truncated to
 * outlen-1 chars + NUL).  Returns true on success.  Convenience for
 * surfaces that need to display a JS-computed value. */
bool macsurf_js_eval_string(struct jscontext *ctx, const char *src,
		char *out, size_t outlen);

/* Pump every live jsheap once.  Called from the main event loop after
 * WaitNextEvent so timer callbacks fire even when no script is running. */
void macsurf_js_pump_all(void);

/* console.log capture buffer.  Every console.log/warn/info/error call
 * appends one line to this buffer (alongside the existing NSLOG path).
 * Read with macsurf_js_console_get(); reset between fetches with
 * macsurf_js_console_reset(). */
const char *macsurf_js_console_get(void);
void        macsurf_js_console_reset(void);

/* Scan src for <script>...</script> blocks and pcall each via the
 * supplied jscontext.  Output (return values + console.log lines)
 * collects in the console buffer.  Returns the number of scripts run. */
int macsurf_js_run_scripts_in_html(struct jscontext *ctx,
		const char *src, size_t srclen);

/* Fatal handler — passed to duk_create_heap; never calls exit/abort. */
void macsurf_js_fatal(void *udata, const char *msg);

/* Date provider — Unix-epoch milliseconds using GetDateTime() on OS 9. */
duk_double_t macsurf_js_get_now(void);

/* DOM bindings entry point — registers document / window / console /
 * setTimeout / setInterval / XMLHttpRequest on the Duktape global. */
void macsurf_js_setup_globals(duk_context *duk);

/* Timer subsystem — called from macsurf_js_pump(). */
void macsurf_js_run_timers(struct jscontext *ctx);

/* Event dispatch — fires an on<type> handler attached to a DOM node. */
struct dom_element;
struct dom_document;
struct dom_node;
bool macsurf_js_dispatch_event(struct jscontext *ctx,
		struct dom_element *el, const char *event_type);

#endif /* MACSURF_JS_H */
