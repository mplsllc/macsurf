/*
 * MacSurf — macsurf_qjs.h
 *
 * QuickJS engine glue.  Implements the internal interface so macsurf_qjs.c
 * can satisfy the NetSurf js_thread API when WITH_QUICKJS is defined.
 */

#ifndef MACSURF_QJS_H
#define MACSURF_QJS_H

#ifdef WITH_QUICKJS

#include <stdbool.h>
#include <stddef.h>

#include "quickjs.h"

struct jscontext {
	JSContext *qctx;
	JSRuntime *qrt;
	void *win_priv;
	void *doc_priv;
};

/* Lifecycle */
struct jscontext *macsurf_qjs_newcontext(void);
void              macsurf_qjs_destroycontext(struct jscontext *ctx);

/* Execution */
bool macsurf_qjs_exec(struct jscontext *ctx, const char *src, size_t srclen);
void macsurf_qjs_pump(struct jscontext *ctx);

/* Pump every live heap once from the event loop. */
void macsurf_qjs_pump_all(void);

/* Safe eval used during global setup (logs error, never throws). */
void macsurf_qjs__safe_eval(JSContext *qctx, const char *src);

/* Timer callbacks wired to setTimeout/setInterval. */
void macsurf_qjs_run_timers(struct jscontext *ctx);

/* Fatal handler — called when the engine hits an unrecoverable error. */
void macsurf_qjs_fatal(JSRuntime *rt, const char *msg);

/* Get current time as ms since epoch (uses macsurf_monotonic_ms). */
double macsurf_qjs_get_now(void);

/* DOM bindings + browser globals entry point. */
void macsurf_qjs_setup_globals(JSContext *qctx);

/* console.log append (used by DOM bindings). */
void macsurf_qjs_console_append(const char *line);

#endif /* WITH_QUICKJS */

#endif /* MACSURF_QJS_H */
