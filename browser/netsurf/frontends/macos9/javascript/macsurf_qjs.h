/*
 * MacSurf  -  macsurf_qjs.h
 *
 * QuickJS engine glue.  Implements the internal interface so macsurf_qjs.c
 * can satisfy the NetSurf js_thread API when WITH_QUICKJS is defined.
 */

#ifndef MACSURF_QJS_H
#define MACSURF_QJS_H

#include "quickjs.h"

#ifdef WITH_QUICKJS

#include <stdbool.h>
#include <stddef.h>

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

/* Bound a native C->JS callback with the same deadline stack used by timers. */
double macsurf_qjs_deadline_push_ms(double budget_ms);
void   macsurf_qjs_deadline_pop(double prev);
double macsurf_qjs_default_timeout_ms(void);

/* Timer callbacks wired to setTimeout/setInterval. */
void macsurf_qjs_run_timers(struct jscontext *ctx);

/* Fatal handler  -  called when the engine hits an unrecoverable error. */
void macsurf_qjs_fatal(JSRuntime *rt, const char *msg);

/* Get current time as ms since epoch (uses macsurf_monotonic_ms). */
double macsurf_qjs_get_now(void);

/* DOM bindings + browser globals entry point. */
void macsurf_qjs_setup_globals(JSContext *qctx);

/* R1.2 - WANT probe: clear the per-page global-miss dedupe set.  Called
 * from macsurf_qjs_audit_reset() on each navigation/realm build so every
 * page gets its own first-use `LIFE WANT` lines. */
void qjs_want_reset(void);

/* The content owned by this exact invoking realm, or NULL while it has no
 * document.  JS-visible native work must pass its JSContext; there is no
 * process-global current-page fallback. */
struct content *qjs_get_content_for_ctx(JSContext *ctx);

/* console.log append (used by DOM bindings). */
void macsurf_qjs_console_append(const char *line);

/* Realm diagnostic inventory declarations are below the engine guard: the
 * MSdg serializer is compiled as frontend glue, not as the QJS translation
 * unit, and still needs this read-only interface. */
#endif /* WITH_QUICKJS */

/* Realm diagnostic inventory (for MSdg GET realms). */
#define QJS_REALM_DIAG_UNAVAILABLE (~0UL)
enum qjs_realm_diag_state {
	QJS_REALM_LIVE = 0,
	QJS_REALM_TEARING_DOWN,
	QJS_REALM_RETIRED
};

struct qjs_realm_diag {
	unsigned long realm_id;
	unsigned long frame_id;
	unsigned long document_id;
	unsigned long nav_id;
	unsigned long heap_id;
	unsigned long ctx_gen;
	JSContext *ctx;
	JSRuntime *rt;
	struct content *content;
	void *document;	/* opaque on non-Mac builds */
	unsigned char state;	/* enum qjs_realm_diag_state */
	unsigned long timers_owned;
	unsigned long xhr_owned;
	unsigned long microtasks_pending;
	unsigned long modules_waiting;
	unsigned long event_listeners;
	unsigned long wrappers;
	unsigned long deferred_notifications;
};

/* Immutable identity captured when native work is queued.  It deliberately
 * contains no borrowed pointers: validation can compare it to the registered
 * live owner without ever dereferencing an old context. */
struct qjs_realm_identity {
	unsigned long realm_id, frame_id, document_id, nav_id, heap_id, ctx_gen;
	JSRuntime *rt;
};
enum qjs_realm_identity_check {
	QJS_REALM_IDENTITY_OK = 0,
	QJS_REALM_IDENTITY_CTX_NOT_REGISTERED,
	QJS_REALM_IDENTITY_DOCUMENT_MISMATCH,
	QJS_REALM_IDENTITY_NAV_MISMATCH,
	QJS_REALM_IDENTITY_GENERATION_MISMATCH,
	QJS_REALM_IDENTITY_RUNTIME_MISMATCH,
	QJS_REALM_IDENTITY_RETIRED
};
int macsurf_qjs_realm_identity(JSContext *ctx, struct qjs_realm_identity *out);
int macsurf_qjs_realm_identity_check(JSContext *ctx,
	const struct qjs_realm_identity *queued, struct qjs_realm_identity *live);

int macsurf_qjs_realm_count(void);
int macsurf_qjs_realm_get(int index, struct qjs_realm_diag *out);
unsigned long macsurf_qjs_realm_retired_total(void);
unsigned long macsurf_qjs_realm_retired_capacity(void);

/* Called when a navigation starts replacing this realm's context.
 * Transitions state from LIVE to TEARING_DOWN. */
void macsurf_qjs_realm_tearing_down(JSContext *ctx);

#endif /* MACSURF_QJS_H */
