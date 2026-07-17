/*
 * MacSurf — macsurf_qjs.c
 *
 * QuickJS engine integration for the NetSurf js_thread API.
 * Parallel to macsurf_js.c (Duktape); activated by WITH_QUICKJS.
 * When WITH_QUICKJS is defined this file owns: js_initialise,
 * js_finalise, js_newheap, js_destroyheap, js_newthread,
 * js_closethread, js_destroythread, js_exec, js_fire_event,
 * js_handle_new_element, js_event_cleanup.
 *
 * Design notes:
 * - One JSRuntime per jsheap (one-heap model).
 * - One JSContext per jsthread, all sharing the same runtime so the
 *   global object is shared across threads.
 * - Timer subsystem is implemented natively with JS_Call and a JSValue array.
 * - macsurf_qjs__safe_eval logs + swallows exceptions, never throws.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "utils/ns_errors.h"
#include "macos9.h"
#include "macsurf_debug.h"
#include "macsurf_qjs.h"
#include "macsurf_timebase.h"
#include "macos9_js_fetch.h"
#include "content/handlers/html/private.h"
#include "utils/libdom.h"
/* fixes879 — document.cookie against the real jar. urldb owns NetSurf's
 * RFC-6265 cookie store; content_protected.h is what makes c->llcache visible
 * for the content_get_url() NULL guard (same include set macos9_js_fetch.c
 * uses for the same guard). */
#include "utils/nsurl.h"
#include "content/urldb.h"
#include "content/content_protected.h"

extern int macsurf_ptr_is_heap(const void *);

#ifdef WITH_QUICKJS

struct dom_event;
struct dom_document;
struct dom_node;
struct dom_element;
struct dom_string;

struct jsheap {
	JSRuntime *rt;
	JSContext *ctx;
	/* fixes875 (#304) — monotonic generation of `ctx`, bumped every time a new
	 * context is built for this heap.  A JSContext* ALONE cannot identify a
	 * realm: free one and the allocator can hand the same address straight back
	 * for the next one, so every `slot->ctx == ctx` ownership test in this file
	 * silently matches a DEAD realm's slot against a LIVE realm at the same
	 * address.  That is the ABA problem fixes550 documents for contents, and it
	 * is how a rt_OLD JSValue gets freed against rt_NEW -> free_object ->
	 * js_free_shape -> js_shape_hash_unlink walks rt_NEW's bucket chain for a
	 * shape that lives in rt_OLD's table, runs off the end, and takes an
	 * unmapped-memory exception at js_shape_hash_unlink+0004C.  The generation
	 * never repeats, so (ctx, gen) does identify a realm. */
	unsigned long ctx_gen;
	int timeout;
	/* fixes861 (#289) — every live heap, so macsurf_qjs_pump_all() can pump
	 * ALL of them.  js_newheap() runs per browser_window AND per (i)frame
	 * (browser_window.c:3373), so "the heap" has never been a real thing on a
	 * page with an iframe; g_heap is only ever the most-RECENTLY-created one.
	 * See the note on macsurf_qjs_pump_all(). */
	struct jsheap *next;
};

struct jsthread {
	struct jsheap *heap;
	JSContext *ctx;
	void *win_priv;
	void *doc_priv;
};

/* The most-recently-created heap.  NOT "the" heap: js_newheap() runs per
 * browser_window AND per (i)frame, so on any page carrying an iframe there are
 * two or more, each with its OWN JSRuntime.  Only use g_heap where "whatever
 * realm is current" is genuinely right; anything that must reach EVERY live
 * heap has to walk g_heap_list (fixes861). */
static struct jsheap *g_heap = NULL;

/* fixes861 (#289) — every live heap, newest first.  js_newheap() links,
 * js_destroyheap() unlinks.  Exists so macsurf_qjs_pump_all() can pump all of
 * them; see the note there for why pumping only g_heap froze iframes. */
static struct jsheap *g_heap_list = NULL;

/* ------------------------------------------------------------------ */
/* Interrupt handler — Cmd-. on OS 9                                   */
/* ------------------------------------------------------------------ */

/* fixes522: time-based runaway guard.  g_qjs_script_deadline is the
 * monotonic-ms time after which the currently-running top-level script is
 * aborted, so an infinite loop or pathological bundle can't hang the
 * cooperative event loop indefinitely.  0 == no deadline (init / internal
 * evals run unbounded).  Set around the top-level JS_Eval in js_exec. */
double macsurf_qjs_get_now(void);
#define QJS_SCRIPT_TIMEOUT_MS 20000
/* fixes586 — timer/event callbacks get a shorter budget: a callback that
 * burns 8s of straight CPU is pathological, and the UI is frozen while it
 * runs.  (Top-level scripts keep the 20s budget: big bundles on a G3 are
 * legitimately slow.) */
#define QJS_TIMER_TIMEOUT_MS 8000
static double g_qjs_script_deadline = 0.0;

/* fixes586 — THE tinkerdifferent hard-freeze.  The deadline was armed ONLY
 * around the top-level JS_Eval in js_exec; setTimeout/setInterval callbacks
 * (macsurf_qjs_run_timers -> JS_Call) and event dispatches (js_fire_event /
 * js_fire_dom_ready -> safe_eval) ran with deadline==0 == UNBOUNDED.  A page
 * timer that enters an infinite loop (XenForo/ThemeHouse retry-poll against
 * our partial DOM) therefore froze the machine forever with no crash: the
 * interrupt handler's WNE swallowed all events (dead UI), the deadline never
 * fired (never armed), and the log's last line was merely whatever the event
 * loop logged before the timer pass ran that tick — which is why the freeze
 * site appeared to wander between builds.  Fix: push a deadline around EVERY
 * JS entry point.  push never EXTENDS an outer deadline (nest-safe); pop
 * restores the caller's value. */
static double qjs_deadline_push(double budget_ms)
{
	double prev = g_qjs_script_deadline;
	double want = macsurf_qjs_get_now() + budget_ms;
	if (prev == 0.0 || want < prev)
		g_qjs_script_deadline = want;
	return prev;
}
static void qjs_deadline_pop(double prev)
{
	g_qjs_script_deadline = prev;
}

static int qjs_interrupt_handler(JSRuntime *rt, void *opaque)
{
	static double hb_last = 0.0;
	static double wne_last = 0.0;   /* fixes690 (#209): WNE poll throttle */
	double now;
	(void)rt; (void)opaque;

	/* fixes690: QuickJS fires this handler every JS_INTERRUPT_COUNTER_INIT
	 * (=10000) bytecode ops. macsurf_qjs_get_now() (Microseconds/mftb) is
	 * cheap; call it once per invocation and drive everything off it. */
	now = macsurf_qjs_get_now();

	/* fixes583 DIAG: heartbeat while a deadline is armed. If a script loops,
	 * these pulse every ~2s and prove JS is the freeze (and whether the
	 * deadline value is sane / the monotonic clock is advancing). Silence
	 * means JS execution is NOT where tinkerdifferent wedges. */
	if (g_qjs_script_deadline != 0.0) {
		if (now - hb_last > 2000.0) {
			hb_last = now;
			macsurf_debug_log_writef(
				"qjs: interrupt hb now=%ld deadline=%ld",
				(long)now, (long)g_qjs_script_deadline);
		}
	}

	/* Deadline check runs EVERY invocation (cheap): a runaway script is
	 * still bounded to the ~20s eval timeout regardless of the WNE throttle. */
	if (g_qjs_script_deadline != 0.0 && now > g_qjs_script_deadline) {
		macsurf_debug_log_writef("qjs: DEADLINE hit, aborting script");
		return 1;
	}

#ifdef __MACOS9__
	/* fixes690 (#209): a full WaitNextEvent Toolbox round-trip on EVERY
	 * 10k-op interrupt was thousands of WNE calls (each consuming+discarding
	 * events) during a multi-million-op jQuery/XenForo init. Poll for the
	 * cmd-period abort at most every ~200ms instead; the deadline check
	 * above still bounds runaway scripts on every call. */
	if (now - wne_last > 200.0) {
		EventRecord ev;
		wne_last = now;
		if (WaitNextEvent(everyEvent, &ev, 0, NULL)) {
			if (ev.what == keyDown &&
			    (ev.modifiers & cmdKey) &&
			    ((ev.message & charCodeMask) == '.')) {
				return 1;
			}
		}
	}
#endif
	return 0;
}

/* ------------------------------------------------------------------ */
/* Fatal handler                                                        */
/* ------------------------------------------------------------------ */

void macsurf_qjs_fatal(JSRuntime *rt, const char *msg)
{
	(void)rt;
	macsurf_debug_log_writef("qjs FATAL: %s", msg ? msg : "(null)");
}

/* ------------------------------------------------------------------ */
/* Time                                                                 */
/* ------------------------------------------------------------------ */

double macsurf_qjs_get_now(void)
{
	return (double)macsurf_monotonic_ms();
}

/* ------------------------------------------------------------------ */
/* Safe eval — logs on error, never propagates exception               */
/* ------------------------------------------------------------------ */

void macsurf_qjs__safe_eval(JSContext *qctx, const char *src)
{
	/* fixes586 — safe_eval runs PAGE event listeners (js_fire_event /
	 * js_fire_dom_ready dispatch jQuery-ready + XF init through here), so
	 * it needs the runaway deadline too.  Internal setup evals are tiny and
	 * never notice it. */
	double prevdl = qjs_deadline_push((double)QJS_SCRIPT_TIMEOUT_MS);
	JSValue val = JS_Eval(qctx, src, strlen(src),
			"<init>", JS_EVAL_TYPE_GLOBAL);
	qjs_deadline_pop(prevdl);
	if (JS_IsException(val)) {
		JSValue exc = JS_GetException(qctx);
		const char *str = JS_ToCString(qctx, exc);
		macsurf_debug_log_writef("qjs init eval failed: %s",
				str ? str : "(null)");
		if (str) JS_FreeCString(qctx, str);
		JS_FreeValue(qctx, exc);
	}
	JS_FreeValue(qctx, val);
}

/* ------------------------------------------------------------------ */
/* console.* native functions                                           */
/* ------------------------------------------------------------------ */

static void qjs_console_emit(JSContext *ctx, const char *prefix,
		int argc, JSValueConst *argv)
{
	int i;
	char buf[512];
	size_t pos = 0;
	size_t plen = strlen(prefix);

	if (plen < sizeof buf) {
		memcpy(buf, prefix, plen);
		pos = plen;
	}
	for (i = 0; i < argc; i++) {
		const char *s = JS_ToCString(ctx, argv[i]);
		size_t slen = s ? strlen(s) : 0;
		if (pos + slen + 2 < sizeof buf) {
			if (i > 0 || pos > 0) buf[pos++] = ' ';
			memcpy(buf + pos, s ? s : "", slen);
			pos += slen;
		}
		if (s) JS_FreeCString(ctx, s);
	}
	buf[pos] = '\0';
	MS_LOG(buf);
}

#define CONSOLE_FN(name, prefix) \
static JSValue qjs_console_##name(JSContext *ctx, JSValueConst this_val, \
		int argc, JSValueConst *argv) \
{ \
	(void)this_val; \
	qjs_console_emit(ctx, prefix, argc, argv); \
	return JS_UNDEFINED; \
}

CONSOLE_FN(log,   "[js]")
CONSOLE_FN(warn,  "[js:warn]")
CONSOLE_FN(error, "[js:error]")
CONSOLE_FN(info,  "[js:info]")
CONSOLE_FN(debug, "[js:debug]")

void macsurf_qjs_console_append(const char *line)
{
	MS_LOG(line ? line : "");
}

/* ------------------------------------------------------------------ */
/* alert / confirm / prompt                                             */
/* ------------------------------------------------------------------ */

static JSValue qjs_alert(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv)
{
	(void)this_val;
#ifdef __MACOS9__
	if (argc > 0) {
		const char *msg = JS_ToCString(ctx, argv[0]);
		if (msg) {
			char macroman[256];
			unsigned char pstr[256];
			short item;
			extern size_t macos9_utf8_to_macroman(const char *utf8,
					size_t len, char *mac_out, size_t max_out);
			macos9_utf8_to_macroman(msg, strlen(msg), macroman,
					sizeof macroman);
			{
				size_t len = strlen(macroman);
				if (len > 255) len = 255;
				pstr[0] = (unsigned char)len;
				{
					size_t ii;
					for (ii = 0; ii < len; ii++)
						pstr[1 + ii] = (unsigned char)macroman[ii];
				}
			}
			StandardAlert(kAlertNoteAlert, pstr, "\p", NULL, &item);
			JS_FreeCString(ctx, msg);
		}
	}
#else
	if (argc > 0) {
		const char *msg = JS_ToCString(ctx, argv[0]);
		if (msg) { MS_LOG(msg); JS_FreeCString(ctx, msg); }
	}
#endif
	return JS_UNDEFINED;
}

static JSValue qjs_confirm(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv)
{
	(void)this_val;
#ifdef __MACOS9__
	if (argc > 0) {
		const char *msg = JS_ToCString(ctx, argv[0]);
		if (msg) {
			char macroman[256];
			unsigned char pstr[256];
			short item;
			AlertStdAlertParamRec params;
			extern size_t macos9_utf8_to_macroman(const char *utf8,
					size_t len, char *mac_out, size_t max_out);
			macos9_utf8_to_macroman(msg, strlen(msg), macroman,
					sizeof macroman);
			{
				size_t len = strlen(macroman);
				if (len > 255) len = 255;
				pstr[0] = (unsigned char)len;
				{
					size_t ii;
					for (ii = 0; ii < len; ii++)
						pstr[1 + ii] = (unsigned char)macroman[ii];
				}
			}
			params.movable       = false;
			params.helpButton    = false;
			params.filterProc    = NULL;
			params.defaultText   = (StringPtr)"\pOK";
			params.cancelText    = (StringPtr)"\pCancel";
			params.otherText     = NULL;
			params.defaultButton = kAlertStdAlertOKButton;
			params.cancelButton  = kAlertStdAlertCancelButton;
			params.position      = kWindowDefaultPosition;
			StandardAlert(kAlertCautionAlert, pstr, "\p", &params, &item);
			JS_FreeCString(ctx, msg);
			return JS_NewBool(ctx, item == kAlertStdAlertOKButton ? 1 : 0);
		}
	}
	return JS_FALSE;
#else
	(void)argc; (void)argv;
	return JS_FALSE;
#endif
}

static JSValue qjs_prompt(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv)
{
	(void)this_val;
	if (argc >= 2 && !JS_IsUndefined(argv[1])) {
		return JS_DupValue(ctx, argv[1]);
	}
	(void)argc;
	return JS_NULL;
}

/* ------------------------------------------------------------------ */
/* atob / btoa — QuickJS has no built-in, provide in JS               */
/* ------------------------------------------------------------------ */

/* Provided via JS polyfill below — quickjs has atob/btoa as built-ins
 * in some builds, but our port uses CONFIG_BIGNUM only so we inject
 * pure-JS versions in the polyfill string. */

/* ------------------------------------------------------------------ */
/* monotonic clock for performance.now()                               */
/* ------------------------------------------------------------------ */

static JSValue qjs_monotonic_ms(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv)
{
	(void)this_val; (void)argc; (void)argv;
	return JS_NewFloat64(ctx, macsurf_qjs_get_now());
}

/* ------------------------------------------------------------------ */
/* window.location — backed by macos9 window list                      */
/* ------------------------------------------------------------------ */

#ifdef __MACOS9__
extern struct gui_window *macos9_window_list_head(void);
extern struct browser_window *macos9_gw_bw(struct gui_window *g);
extern void macos9_window_navigate(struct gui_window *g, const char *url);
extern void macos9_window_back(struct gui_window *g);
extern void macos9_window_forward(struct gui_window *g);
extern void macos9_window_reload(struct gui_window *g);
extern struct gui_window *initial_win;
extern void macos9_gw_set_title(struct gui_window *gw, const char *title);
#endif

static JSValue qjs_location_get(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv)
{
	(void)this_val; (void)argc; (void)argv;
#ifdef __MACOS9__
	{
		struct gui_window *win = macos9_window_list_head();
		struct browser_window *bw = win ? macos9_gw_bw(win) : NULL;
		const char *href = "about:blank";
		if (bw != NULL) {
			extern struct nsurl *browser_window_access_url(
					const struct browser_window *bw);
			struct nsurl *u = browser_window_access_url(bw);
			if (u != NULL) {
				extern const char *nsurl_access(const struct nsurl *u);
				const char *s = nsurl_access(u);
				if (s != NULL) href = s;
			}
		}
		return JS_NewString(ctx, href);
	}
#else
	return JS_NewString(ctx, "about:blank");
#endif
}

static JSValue qjs_location_set(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv)
{
	(void)this_val;
#ifdef __MACOS9__
	if (argc > 0) {
		const char *url = JS_ToCString(ctx, argv[0]);
		if (url) {
			struct gui_window *win = macos9_window_list_head();
			if (win != NULL) macos9_window_navigate(win, url);
			JS_FreeCString(ctx, url);
		}
	}
#else
	(void)argc; (void)argv;
#endif
	return JS_UNDEFINED;
}

static JSValue qjs_location_reload(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv)
{
	(void)this_val; (void)argc; (void)argv;
#ifdef __MACOS9__
	{
		struct gui_window *win = macos9_window_list_head();
		if (win != NULL) macos9_window_reload(win);
	}
#endif
	return JS_UNDEFINED;
}

static JSValue qjs_history_back(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv)
{
	(void)this_val; (void)argc; (void)argv;
#ifdef __MACOS9__
	{
		struct gui_window *win = macos9_window_list_head();
		if (win != NULL) macos9_window_back(win);
	}
#endif
	return JS_UNDEFINED;
}

static JSValue qjs_history_forward(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv)
{
	(void)this_val; (void)argc; (void)argv;
#ifdef __MACOS9__
	{
		struct gui_window *win = macos9_window_list_head();
		if (win != NULL) macos9_window_forward(win);
	}
#endif
	return JS_UNDEFINED;
}

static JSValue qjs_history_go(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv)
{
	(void)this_val;
#ifdef __MACOS9__
	{
		int delta = 0;
		struct gui_window *win;
		if (argc > 0) {
			double d;
			JS_ToFloat64(ctx, &d, argv[0]);
			delta = (int)d;
		}
		win = macos9_window_list_head();
		if (win != NULL) {
			while (delta < 0) { macos9_window_back(win); delta++; }
			while (delta > 0) { macos9_window_forward(win); delta--; }
		}
	}
#else
	(void)argc; (void)argv;
#endif
	return JS_UNDEFINED;
}

/* ------------------------------------------------------------------ */
/* Timer subsystem                                                      */
/* ------------------------------------------------------------------ */

/* fixes868 (#294) — microtask budget per pump pass.  A .then() can enqueue the
 * next job, so draining unbounded lets a promise chain (or a self-scheduling
 * loop) starve the cooperative WaitNextEvent loop and hang the Mac -- the same
 * failure class as the fixes608 timer cycle.  Leftover jobs simply run on the
 * next poll pass; a real browser's task/microtask split behaves the same way.
 * 256 clears any realistic startup chain (hackaday's loader is ~6 deep) in one
 * pass while bounding the worst case. */
#define QJS_MAX_JOBS_PER_PUMP 256

/* fixes877 — was 64, which is low for a page running several libraries at once
 * (jQuery + a carousel + an analytics shim will each hold intervals), and every
 * overflow silently destroys a callback. The arena is a fixed static array, so
 * the cost is purely memory: ~120 B/slot * 256 = ~30 KB against a ~195 MB
 * partition. Cheap insurance against a class of silent, page-visible loss.
 *
 * Also sizes run_timers' due_idx/due_id stack arrays: 2 * 256 * 4 = 2 KB per
 * frame. run_timers is called only from macsurf_qjs_pump_all and is not
 * recursive, so this is a one-off 2 KB, not a per-depth cost. */
#define QJS_MAX_TIMERS 256

/* fixes876 — how many trailing setTimeout(fn, delay, ...) arguments a slot can
 * carry.  4 covers every real use (rAF's timestamp needs 1); anything beyond is
 * logged and dropped rather than silently truncated. */
#define QJS_TIMER_MAX_ARGS 4

/* fixes854 (#283) — `ctx` is the OWNER of this slot's `fn`, captured at
 * setTimeout time, and it is load-bearing, not bookkeeping.  js_newheap()
 * runs per browser_window AND per (i)frame (browser_window.c:3373 "new
 * javascript context for each window/(i)frame"), and every heap gets its
 * OWN JSRuntime (JS_NewRuntime2 in js_newheap) — so a page with an iframe
 * has TWO runtimes, each with its own shape hash table, sharing this ONE
 * global arena.  A JSValue is only ever valid against the runtime that made
 * it: JS_FreeValue(ctx_B, fn_from_rt_A) decrefs an rt_A object, and when it
 * hits zero free_object() -> js_free_shape(rt_B, shape_A) ->
 * js_shape_hash_unlink(rt_B, shape_A) walks rt_B's bucket chain looking for
 * a shape that lives in rt_A's table, never finds it, and runs off the end
 * into unmapped memory.  That is the hackaday.com crash (PowerPC unmapped
 * memory exception at js_shape_hash_unlink+0004C, reached via
 * qjs_flush_timers -> JS_FreeValue -> free_object -> js_free_shape).
 * Every touch of `fn` MUST therefore be gated on `t->ctx == <this ctx>`.
 * This mirrors the XHR slot arena (macos9_js_fetch.c), which captures its
 * owning `ctx` and filters on it in macos9_js_fetch_flush(). */
struct qjs_timer {
	/* fixes875 (#304) — the owning realm's generation, captured next to `ctx`.
	 * `ctx` alone is a recycled address; see struct jsheap's ctx_gen note. A
	 * slot whose ctx matches but whose gen does NOT belongs to a dead realm at
	 * a reused address: its `fn` must be ABANDONED, never freed -- the runtime
	 * that owns it is gone, so there is nothing left to free it against. */
	unsigned long ctx_gen;
	int        id;
	double     expiry_ms;
	int        repeating;
	double     interval_ms;
	int        live;
	JSContext *ctx;		/* owner of `fn` AND `args` — see the note above */
	JSValue    fn;
	/* fixes876 — setTimeout(fn, delay, a, b): the extra arguments, duped at
	 * registration and replayed at every fire.  These carry EXACTLY the same
	 * cross-runtime lifetime hazard as `fn` above: they are JSValues owned by
	 * `ctx`'s runtime and may only be duped/passed/freed against it.  Every
	 * release path therefore goes through timer_slot_clear(), which handles
	 * `fn` and `args` together -- freeing one and forgetting the other is the
	 * bug this arena has already produced twice (fixes854, fixes875). */
	int        nargs;
	JSValue    args[QJS_TIMER_MAX_ARGS];
};

static struct qjs_timer s_timer_arena[QJS_MAX_TIMERS];
static int s_timer_next_id = 1;

/* fixes608 — the timer subsystem is a fixed index-addressed arena with NO
 * intrusive linked list.  The old s_timer_head list could be spliced into a
 * cycle when a timer callback reentrantly called setTimeout (timer_alloc
 * evicting/reusing a slot the run_timers walk still held), and run_timers'
 * `while (t != NULL)` then spun forever — the tinkerdifferent hard-freeze,
 * immune to the fixes586 callback deadline because the spin is in the C loop,
 * not inside JS_Call.  Index-based iteration (0..QJS_MAX_TIMERS-1) makes an
 * infinite loop structurally impossible. */
/* fixes875 (#304) — the generation currently owning `ctx`, or 0 if NO live heap
 * does.  Walks g_heap_list (fixes861), which is the only authority on which
 * realms exist.
 *
 * Zero is the important answer: it means this JSContext* is either dead or
 * belongs to a heap that is gone, so any JSValue tagged with it must be
 * abandoned rather than freed.  Freeing it is what crashes -- see struct
 * jsheap's ctx_gen note. */
static unsigned long qjs_ctx_gen(JSContext *ctx)
{
	struct jsheap *h;
	if (ctx == NULL) return 0;
	for (h = g_heap_list; h != NULL; h = h->next) {
		if (h->ctx == ctx) return h->ctx_gen;
	}
	return 0;
}

/* Does this slot really belong to (ctx, its current generation)? */
static int qjs_timer_owned_by(struct qjs_timer *t, JSContext *ctx)
{
	if (!t->live || t->ctx != ctx) return 0;
	return t->ctx_gen == qjs_ctx_gen(ctx) && t->ctx_gen != 0;
}

/* fixes875 (#304) — never-repeating realm id. Monotonic across the whole
 * process: the ONLY property required is that a value is never reused, which is
 * exactly what a JSContext* address fails to guarantee. */
static unsigned long g_ctx_gen_next = 1;

/* fixes876 — the ONE way a timer slot is released.  Every release path in this
 * file goes through here so that `fn` and `args` can never fall out of step.
 *
 * `free_vals` selects between the two disciplines this arena already needs, and
 * the distinction is load-bearing (see qjs_flush_timers' fixes875 note):
 *   1 = the slot's realm is still live at that address -> free against t->ctx.
 *   0 = ABANDON.  The owning runtime is gone, so nothing can legally free these
 *       values; blank them and leak into a runtime that no longer exists, which
 *       costs nothing real.  Freeing here is the unmapped-memory crash.
 *
 * Freeing against t->ctx (never a caller-supplied ctx) makes it structurally
 * impossible to free an rt_A value against rt_B, which is what fixes854 fixed
 * by hand at each site. */
static void timer_slot_clear(struct qjs_timer *t, int free_vals)
{
	int i;

	if (free_vals && t->ctx != NULL) {
		JS_FreeValue(t->ctx, t->fn);
		for (i = 0; i < t->nargs; i++)
			JS_FreeValue(t->ctx, t->args[i]);
	}
	t->fn = JS_UNDEFINED;
	for (i = 0; i < t->nargs; i++)
		t->args[i] = JS_UNDEFINED;
	t->nargs = 0;
	t->ctx = NULL;
	t->ctx_gen = 0;
	t->live = 0;
}

static struct qjs_timer *timer_alloc(void)
{
	int i;
	struct qjs_timer *victim;
	double victim_expiry;
	for (i = 0; i < QJS_MAX_TIMERS; i++) {
		if (!s_timer_arena[i].live) return &s_timer_arena[i];
	}
	/* All full: evict the FURTHEST-OUT slot.
	 *
	 * fixes877 — this loop used `<` on expiry_ms, i.e. it picked the MINIMUM
	 * deadline: the soonest-expiring timer, the one closest to firing and so
	 * the one most likely to be needed imminently. A page that briefly
	 * over-filled the arena would silently lose the callback that was about to
	 * run while keeping ones due much later -- a wrong answer, delivered
	 * quietly. (The old variable name `oldest` disguised it: nearest-future is
	 * not least-recently-created.) Evicting the furthest-out gives every
	 * remaining timer the most time to fire before its slot is at risk.
	 *
	 * fixes854 (#283) — free against the slot's OWN ctx, never g_heap->ctx.
	 * g_heap is just "the most recently created heap"; with an iframe on the
	 * page the evicted slot can belong to a DIFFERENT heap/runtime, and
	 * freeing an rt_A JSValue against rt_B corrupts rt_B's shape table (see
	 * the struct qjs_timer note).  A live slot always has a live ctx:
	 * qjs_flush_timers() clears this ctx's slots on navigation and
	 * js_destroyheap() clears them on teardown, both BEFORE JS_FreeContext. */
	victim = &s_timer_arena[0];
	victim_expiry = victim->expiry_ms;
	for (i = 1; i < QJS_MAX_TIMERS; i++) {
		if (s_timer_arena[i].expiry_ms > victim_expiry) {
			victim = &s_timer_arena[i];
			victim_expiry = s_timer_arena[i].expiry_ms;
		}
	}
	/* Evicting a timer is a real, page-visible loss (a callback that will now
	 * never run). It used to be silent, which reads as "everything is fine"
	 * while a page quietly misbehaves. */
	macsurf_debug_log_writef(
		"WORK timer: arena FULL (%d) -- evicting furthest-out id=%d "
		"(expiry %ld ms out); its callback will never run",
		QJS_MAX_TIMERS, victim->id,
		(long)(victim_expiry - macsurf_qjs_get_now()));
	/* fixes875 (#304) — free ONLY if the slot's realm is still the live one at
	 * that address.  A stale slot from a dead realm whose ctx address has been
	 * recycled would otherwise be freed against the NEW runtime. */
	timer_slot_clear(victim,
			 victim->ctx != NULL && victim->ctx_gen != 0 &&
			 victim->ctx_gen == qjs_ctx_gen(victim->ctx));
	return victim;
}

static JSValue qjs_settimeout_impl(JSContext *ctx,
		JSValueConst this_val, int argc, JSValueConst *argv,
		int repeating)
{
	struct qjs_timer *t;
	double delay_ms = 0.0;
	int id;
	int extra;
	int i;

	(void)this_val;
	if (argc < 1 || !JS_IsFunction(ctx, argv[0])) return JS_NewInt32(ctx, 0);
	if (argc >= 2) JS_ToFloat64(ctx, &delay_ms, argv[1]);
	if (delay_ms < 0.0) delay_ms = 0.0;

	t = timer_alloc();
	if (t == NULL) return JS_NewInt32(ctx, 0);

	id = s_timer_next_id++;
	if (s_timer_next_id <= 0) s_timer_next_id = 1;

	t->id = id;
	t->expiry_ms = macsurf_qjs_get_now() + delay_ms;
	t->repeating = repeating;
	t->interval_ms = delay_ms;
	t->live = 1;

	/* fixes876 — capture setTimeout(fn, delay, a, b, ...)'s trailing args.
	 * Dropping these is why requestAnimationFrame callbacks saw `undefined`
	 * instead of a DOMHighResTimeStamp, making the ubiquitous `t - last` idiom
	 * NaN and breaking every animation loop. */
	extra = argc - 2;
	if (extra < 0) extra = 0;
	if (extra > QJS_TIMER_MAX_ARGS) {
		macsurf_debug_log_writef(
			"WORK timer: setTimeout extra args %d > cap %d -- dropping %d",
			extra, QJS_TIMER_MAX_ARGS, extra - QJS_TIMER_MAX_ARGS);
		extra = QJS_TIMER_MAX_ARGS;
	}
	t->nargs = extra;
	/* fixes854 (#283) — capture the owning context alongside the dup.  `fn`
	 * belongs to THIS ctx's runtime and may only ever be duped/called/freed
	 * against it. */
	t->ctx = ctx;
	t->ctx_gen = qjs_ctx_gen(ctx);
	t->fn = JS_DupValue(ctx, argv[0]);
	/* Dup against the SAME ctx as `fn`, for the same reason. */
	for (i = 0; i < extra; i++)
		t->args[i] = JS_DupValue(ctx, argv[2 + i]);

	return JS_NewInt32(ctx, id);
}

static JSValue qjs_settimeout(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv)
{
	return qjs_settimeout_impl(ctx, this_val, argc, argv, 0);
}

static JSValue qjs_setinterval(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv)
{
	return qjs_settimeout_impl(ctx, this_val, argc, argv, 1);
}

static JSValue qjs_cleartimeout(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv)
{
	int target_id;
	struct qjs_timer *t;

	(void)this_val;
	if (argc < 1) return JS_UNDEFINED;
	JS_ToInt32(ctx, &target_id, argv[0]);
	{
		int i;
		for (i = 0; i < QJS_MAX_TIMERS; i++) {
			t = &s_timer_arena[i];
			/* fixes854 (#283) — `t->ctx == ctx` is a correctness gate, not
			 * an optimisation: ids come from one global counter shared by
			 * every heap, so without it a page could clearTimeout an
			 * IFRAME's id and free that runtime's JSValue against this
			 * context (see the struct qjs_timer note).  It also correctly
			 * scopes clearTimeout to the caller's own realm. */
			if (qjs_timer_owned_by(t, ctx) && t->id == target_id) {
				/* owned_by() already proved t->ctx == ctx, so the
				 * helper's free-against-t->ctx is this same ctx. */
				timer_slot_clear(t, 1);
				break;
			}
		}
	}
	return JS_UNDEFINED;
}

/* fixes532: free every pending timer's callback JSValue against the context
 * it was duped from, then empty the timer list.  Called right before a
 * context is torn down on navigation so the duped fn refs in s_timer_arena
 * cannot dangle into freed heap (run_timers would otherwise JS_Call them). */
static void qjs_flush_timers(JSContext *old_ctx)
{
	int i;
	if (old_ctx == NULL) return;
	for (i = 0; i < QJS_MAX_TIMERS; i++) {
		/* fixes854 (#283) — THE hackaday.com crash.  This used to free every
		 * live slot against old_ctx.  The arena is global but heaps are
		 * per-window/per-iframe and each has its own JSRuntime, so on any
		 * page with an iframe it freed the IFRAME's timer JSValues against
		 * the MAIN page's context: JS_FreeValue(ctx_main, fn_iframe) ->
		 * free_object(rt_main, obj_iframe) -> js_shape_hash_unlink(rt_main,
		 * shape_iframe) -> the bucket walk never finds a shape that lives in
		 * rt_iframe's table -> runs off the chain -> unmapped memory
		 * exception at js_shape_hash_unlink+0004C.  Only ever touch slots
		 * this context owns; the other heap flushes its own. */
		if (!s_timer_arena[i].live || s_timer_arena[i].ctx != old_ctx)
			continue;

		/* fixes875 (#304) — THE CRASH SITE (unmapped memory exception at
		 * js_shape_hash_unlink+0004C, reached via
		 *   js_newthread -> qjs_flush_timers -> JS_FreeValue -> free_object
		 *   -> js_free_shape -> js_shape_hash_unlink).
		 *
		 * The `ctx == old_ctx` test above is a POINTER compare, and a
		 * JSContext* is a recycled address: JS_FreeContext returns it to the
		 * allocator and the next JS_NewContext -- for a DIFFERENT heap, with a
		 * DIFFERENT JSRuntime -- can be handed the same address. A leftover
		 * slot from the dead realm then matches the live one, and freeing its
		 * `fn` here runs js_shape_hash_unlink(rt_NEW, shape_OLD): the bucket
		 * walk never finds a shape that lives in rt_OLD's table, runs off the
		 * end of the chain, and dereferences garbage. This is exactly the ABA
		 * problem fixes550 documents for contents -- pointer identity is not
		 * identity once the allocator can reuse the address.
		 *
		 * The generation settles it. A mismatch means this slot belongs to a
		 * DEAD realm, so its `fn` is ABANDONED, not freed: the runtime that
		 * allocated it is gone, and there is nothing left that can legally free
		 * it. That leaks the JSValue -- into a runtime that no longer exists,
		 * i.e. it costs nothing real -- which is the only safe move. */
		if (s_timer_arena[i].ctx_gen == 0 ||
		    s_timer_arena[i].ctx_gen != qjs_ctx_gen(old_ctx)) {
			macsurf_debug_log_writef(
				"WORK timer: ABANDON stale slot %d gen=%lu live_gen=%lu "
				"ctx=%p (dead realm at a recycled address)",
				i, s_timer_arena[i].ctx_gen, qjs_ctx_gen(old_ctx),
				(void *) old_ctx);
			/* free_vals=0: ABANDON — see the note above. */
			timer_slot_clear(&s_timer_arena[i], 0);
			continue;
		}

		timer_slot_clear(&s_timer_arena[i], 1);
	}
}

void macsurf_qjs_run_timers(struct jscontext *ctx)
{
	double now;
	JSContext *qctx;
	int due_idx[QJS_MAX_TIMERS];
	int due_id[QJS_MAX_TIMERS];
	int ndue;
	int k;
	int i;

	if (ctx == NULL || ctx->qctx == NULL) return;
	qctx = ctx->qctx;
	now = macsurf_qjs_get_now();

	/* fixes608 — snapshot the DUE timers by (slot index, id) BEFORE firing
	 * any, then fire from the snapshot.  A callback can reentrantly call
	 * setTimeout (which may evict+reuse an arena slot) or clearTimeout
	 * (which frees a slot); the index+id snapshot makes that reentrancy
	 * safe, and the bounded 0..QJS_MAX_TIMERS-1 walk can never loop forever
	 * (the old intrusive-list walk could be spliced into a cycle mid-callback
	 * -> the tinkerdifferent hard-freeze). */
	ndue = 0;
	for (i = 0; i < QJS_MAX_TIMERS; i++) {
		/* fixes854 (#283) — only fire timers belonging to THIS context.  The
		 * arena is shared by every heap (one per window/iframe, each with its
		 * own JSRuntime), so without this gate the main page's poll would
		 * JS_DupValue/JS_Call an IFRAME's callback against the main runtime —
		 * a cross-runtime call on a JSValue rt_main never allocated.  Each
		 * heap's own run_timers pass fires its own slots. */
		/* fixes875 (#304) — generation too: a stale slot at a recycled ctx
		 * address would otherwise be JS_DupValue'd + JS_Call'd against the
		 * WRONG runtime. */
		if (qjs_timer_owned_by(&s_timer_arena[i], qctx) &&
		    s_timer_arena[i].expiry_ms <= now) {
			due_idx[ndue] = i;
			due_id[ndue] = s_timer_arena[i].id;
			ndue++;
		}
	}

	for (k = 0; k < ndue; k++) {
		struct qjs_timer *t = &s_timer_arena[due_idx[k]];
		JSValue fn;
		double prevdl;
		double mydl;
		JSValue ret;
		JSValue call_args[QJS_TIMER_MAX_ARGS];
		JSValue this_obj;
		int call_nargs;
		int a;

		/* Revalidate: a prior callback may have cleared this timer, or
		 * timer_alloc may have evicted+reused this slot for a different
		 * id.  Only fire if it is still the same live timer. */
		/* fixes854 (#283) — re-check the owner too: a callback can run
		 * arbitrary JS, and an eviction may have handed this slot to a
		 * DIFFERENT heap's setTimeout since we snapshotted, which would make
		 * the JS_DupValue below cross-runtime. */
		if (!qjs_timer_owned_by(t, qctx) || t->id != due_id[k]) continue;

		/* fixes876 — snapshot fn AND the extra args BEFORE the slot can be
		 * cleared below: timer_slot_clear() blanks t->args, and the callback
		 * itself may evict/reuse this slot reentrantly. */
		fn = JS_DupValue(qctx, t->fn);
		call_nargs = t->nargs;
		for (a = 0; a < call_nargs; a++)
			call_args[a] = JS_DupValue(qctx, t->args[a]);

		if (t->repeating) {
			t->expiry_ms = macsurf_qjs_get_now() + t->interval_ms;
		} else {
			timer_slot_clear(t, 1);
		}

		/* fixes586 — bound the callback so a runaway script can't hang
		 * (the interrupt handler checks g_qjs_script_deadline). */
		prevdl = g_qjs_script_deadline;
		mydl = macsurf_qjs_get_now() + (double)QJS_TIMER_TIMEOUT_MS;
		if (prevdl == 0.0 || mydl < prevdl)
			g_qjs_script_deadline = mydl;
		/* fixes876 — HTML spec calls timer callbacks with `this` = the window.
		 * JS_UNDEFINED left strict-mode callbacks with `this === undefined`. */
		this_obj = JS_GetGlobalObject(qctx);
		ret = JS_Call(qctx, fn, this_obj, call_nargs, call_args);
		JS_FreeValue(qctx, this_obj);
		if (JS_IsException(ret)) {
			JSValue exc = JS_GetException(qctx);
			const char *str = JS_ToCString(qctx, exc);
			macsurf_debug_log_writef("qjs timer exc: %s",
					str ? str : "?");
			if (str) JS_FreeCString(qctx, str);
			JS_FreeValue(qctx, exc);
			/* Deadline-abort of a still-live (repeating) timer: kill
			 * it so the rogue interval can never re-freeze the UI. */
			if (t->live && t->id == due_id[k] && t->ctx == qctx &&
			    macsurf_qjs_get_now() >= mydl) {
				macsurf_debug_log_writef(
					"qjs: TIMER TIMEOUT -- repeating timer KILLED");
				timer_slot_clear(t, 1);
			}
		}
		JS_FreeValue(qctx, ret);
		g_qjs_script_deadline = prevdl;
		JS_FreeValue(qctx, fn);
		for (a = 0; a < call_nargs; a++)
			JS_FreeValue(qctx, call_args[a]);
	}
}

/* ------------------------------------------------------------------ */
/* document.title getter/setter                                         */
/* ------------------------------------------------------------------ */

static JSValue qjs_document_title_get(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv)
{
	(void)this_val; (void)argc; (void)argv;
	return JS_NewString(ctx, "");
}

static JSValue qjs_document_title_set(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv)
{
	(void)this_val;
#ifdef __MACOS9__
	if (argc > 0) {
		const char *title = JS_ToCString(ctx, argv[0]);
		if (title && initial_win != NULL) {
			macos9_gw_set_title(initial_win, title);
		}
		if (title) JS_FreeCString(ctx, title);
	}
#else
	(void)argc; (void)argv;
#endif
	return JS_UNDEFINED;
}

/* ================================================================== */
/* DOM bridge — getElementById, querySelectorAll, getAttribute,        */
/*              setAttribute with real libdom write-through.           */
/* Mirrors macsurf_js_dom.c (Duktape) but in QuickJS idioms.          */
/* ================================================================== */

/* DOM types come from content/handlers/html/private.h → dom/dom.h */
#define DOM_NO_ERR 0

/* libdom string helpers */
extern dom_exception dom_string_create(const uint8_t *ptr, size_t len,
		dom_string **str);
extern const char   *dom_string_data(const dom_string *str);
extern uint32_t      dom_string_length(dom_string *str);

/* macsurf_dom_dispatch.c wrappers */
extern void          macsurf_dom_node_ref(dom_node *node);
extern void          macsurf_dom_node_unref(dom_node *node);
extern void          macsurf_dom_string_unref(dom_string *str);
extern dom_exception macsurf_dom_document_get_element_by_id(dom_document *doc,
		dom_string *id, dom_element **element);
extern dom_exception macsurf_dom_element_get_tag_name(dom_element *el,
		dom_string **name);
extern dom_exception macsurf_dom_element_get_attribute(dom_element *el,
		dom_string *name, dom_string **value);
extern dom_exception macsurf_dom_element_set_attribute(dom_element *el,
		dom_string *name, dom_string *value);
extern dom_exception macsurf_dom_document_get_document_element(
		dom_document *doc, dom_element **result);
extern dom_exception macsurf_dom_node_get_first_child(dom_node *node,
		dom_node **result);
extern dom_exception macsurf_dom_node_get_last_child(dom_node *node,
		dom_node **result);
extern dom_exception macsurf_dom_node_get_next_sibling(dom_node *node,
		dom_node **result);
extern dom_exception macsurf_dom_node_get_previous_sibling(dom_node *node,
		dom_node **result);
extern dom_exception macsurf_dom_node_get_parent_node(dom_node *node,
		dom_node **result);
extern dom_exception macsurf_dom_node_get_node_type(dom_node *node,
		dom_node_type *result);
extern dom_exception macsurf_dom_node_get_text_content(dom_node *node,
		dom_string **result);
extern dom_exception macsurf_dom_node_set_text_content(dom_node *node,
		dom_string *content);
/* fixes878 — real cloneNode/contains (macsurf_dom_dispatch.c). `deep` and
 * `contains` are int, not bool: this file's C89 build maps bool to Apple's
 * Boolean (see macos9.h), so keeping the shim boundary int-only avoids
 * depending on which bool won in a given TU. */
extern dom_exception macsurf_dom_node_clone_node(dom_node *node, int deep,
		dom_node **result);
extern dom_exception macsurf_dom_node_contains(dom_node *node,
		dom_node *other, int *contains);
extern dom_exception macsurf_dom_node_append_child(dom_node *parent,
		dom_node *new_child, dom_node **result);
extern dom_exception macsurf_dom_node_remove_child(dom_node *parent,
		dom_node *old_child, dom_node **result);
extern dom_exception macsurf_dom_node_insert_before(dom_node *parent,
		dom_node *new_child, dom_node *ref_child, dom_node **result);
extern dom_exception macsurf_dom_element_has_attribute(dom_element *el,
		dom_string *name, int *result);
extern dom_exception macsurf_dom_element_remove_attribute(dom_element *el,
		dom_string *name);
extern dom_exception macsurf_dom_document_create_element_s(dom_document *doc,
		const char *tag, dom_element **element);
/* fixes873 — shorten a script URL to something a 255-byte log line can afford.
 * Keeps the FILENAME (the identifying part) and drops the leading path and the
 * query string, so
 *   https://jetpack.wordpress.com/wp-content/mu-plugins/jetpack-mu-wpcom-plugin/
 *   sun/jetpack_vendor/automattic/jetpack-mu-wpcom/src/build/verbum-comments/
 *   verbum-comments.js?m=1783962184i&minify=false&ver=2af6b658a7893b8bad68
 * becomes "verbum-comments.js". Truncating from the LEFT (the naive `%.40s`)
 * would keep "https://jetpack.wordpress.com/wp-content/" for every script on the
 * page -- i.e. the part that is identical everywhere and identifies nothing. */
static void qjs_short_name(const char *name, char *out, int cap)
{
	const char *base;
	const char *p;
	int i = 0;

	if (out == NULL || cap <= 0) return;
	out[0] = '\0';
	if (name == NULL || name[0] == '\0') {
		if (cap > 6) strcpy(out, "(anon)");
		return;
	}
	base = name;
	for (p = name; *p != '\0'; p++) {
		if (*p == '/' && p[1] != '\0') base = p + 1;
	}
	while (i < cap - 1 && base[i] != '\0' && base[i] != '?' && base[i] != '#') {
		out[i] = base[i];
		i++;
	}
	out[i] = '\0';
	if (out[0] == '\0' && cap > 6) strcpy(out, "(anon)");
}

/* fixes870 (#297) — createElementNS, Preact's only element factory. */
extern dom_exception macsurf_dom_document_create_element_ns_s(dom_document *doc,
		const char *ns, const char *qname, dom_element **element);
/* fixes872 — declare the fixes867 owner-document accessor properly. It was being
 * called with NO prototype in scope, so C89 implicitly declared it int-returning.
 * Benign by luck here (dom_exception is an enum, i.e. int, and comes back in r3
 * either way) -- but only by luck, and the same omission on a double- or
 * pointer-returning function is a real miscompile. */
extern dom_exception macsurf_dom_node_get_owner_document(dom_node *node,
		dom_document **result);
/* fixes846 (#167 S3) — real createTextNode/createDocumentFragment/text-data. */
extern dom_exception macsurf_dom_document_create_text_node_s(dom_document *doc,
		const char *data, dom_text **text);
extern dom_exception macsurf_dom_document_create_document_fragment(
		dom_document *doc, dom_document_fragment **fragment);
extern dom_exception macsurf_dom_characterdata_get_data(dom_node *node,
		dom_string **data);
extern dom_exception macsurf_dom_characterdata_set_data_s(dom_node *node,
		const char *data);

/* macos9_reconvert.c — deferred re-convert after DOM mutation */
extern void macos9_js_mark_dom_dirty(struct content *c);

/* ---- Global document/content pointers (set in js_newthread) ---- */
static dom_document  *g_qjs_document = NULL;
static struct content *g_qjs_content = NULL;

void qjs_set_document(dom_document *doc)  { g_qjs_document = doc; }
void qjs_set_content(struct content *c)   { g_qjs_content  = c; }

/* fixes846 (#167 S3) — macos9_js_fetch.c's only need for g_qjs_content:
 * read the page URL as a fetch_start() referer at send()-time. See this
 * pointer's staleness rules two comments below; the caller must snapshot
 * whatever it needs synchronously, not hold this across an async gap. */
struct content *qjs_get_content(void) { return g_qjs_content; }

/* ---- fixes879: document.cookie, against the REAL jar ----
 *
 * Was the data property `document.cookie=''`: a plain string that a page write
 * stuck to for the session and that reached no jar, persisted nothing, and
 * started every navigation empty. The jar itself is real, RFC-6265 and
 * disk-persistent (urldb.c), and both fetchers have read and written it since
 * fixes367. Only the JS exposure was fake -- so session-detection code, which
 * is the thing that reads document.cookie constantly, concluded "logged out" on
 * every page even while the very request that fetched it carried the session
 * cookie.
 *
 * The URL comes from THIS realm's own content (g_qjs_content), NOT from
 * macos9_window_list_head() the way location does: that returns the FIRST
 * window, which for an iframe is a different document entirely -- and cookies
 * are precisely where reading the wrong document's URL would be a security bug
 * rather than a cosmetic one.
 *
 * The c->llcache NULL guard is not defensive noise: content_get_url() ->
 * llcache_handle_get_url() dereferences it unconditionally and crashes on a
 * content that is not (yet, or any longer) fully live. Same guard as
 * xhr_start_fetch() and macos9_reconvert_host_allowed(), for the same reason;
 * the S0 harness's minimal test content has no llcache and caught it there. */
static nsurl *qjs_cookie_doc_url(void)
{
	struct content *c = qjs_get_content();
	if (c == NULL || c->llcache == NULL) return NULL;
	return content_get_url(c);	/* borrowed */
}

static JSValue qjs_document_cookie_get(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv)
{
	nsurl *url;
	char *cookies;
	JSValue ret;

	(void)this_val; (void)argc; (void)argv;
	url = qjs_cookie_doc_url();
	if (url == NULL) return JS_NewString(ctx, "");

	/* include_http_only = FALSE. HttpOnly exists precisely to be invisible to
	 * script; the fetchers pass true because the wire is allowed to see those
	 * cookies, and this path is not. Passing true here would hand every
	 * session cookie the server marked HttpOnly to any script on the page. */
	cookies = urldb_get_cookie(url, false);
	if (cookies == NULL) return JS_NewString(ctx, "");
	ret = JS_NewString(ctx, cookies);
	free(cookies);
	return ret;
}

static JSValue qjs_document_cookie_set(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv)
{
	nsurl *url;
	const char *hdr;

	(void)this_val;
	if (argc < 1) return JS_UNDEFINED;
	url = qjs_cookie_doc_url();
	if (url == NULL) return JS_UNDEFINED;

	hdr = JS_ToCString(ctx, argv[0]);
	if (hdr == NULL) return JS_UNDEFINED;
	/* A document.cookie write is one Set-Cookie-shaped declaration; urldb
	 * parses attributes (path/domain/expires/secure) itself. referrer NULL =>
	 * a verifiable, first-party transaction, which is what a script setting a
	 * cookie on its own document is (see urldb_set_cookie's RFC 2109 4.3.5
	 * unverifiable-transaction check). */
	urldb_set_cookie(hdr, url, NULL);
	JS_FreeCString(ctx, hdr);
	return JS_UNDEFINED;
}

/* Stage 1 death-row hook (fixes565, QuickJS port of the Duktape
 * macsurf_js_notify_content_freed): g_qjs_content is a raw content pointer,
 * independent of content_list and not reachable via any scheduled
 * continuation, so the death-row pinned-check cannot see it. The drain calls
 * this just before it frees a content; NULL our cached pointers if the content
 * going away is the one we hold, so the DOM bindings (macos9_js_mark_dom_dirty
 * and friends) never deref a freed content or a document living inside it. */
void
macsurf_js_notify_content_freed(struct content *c)
{
	if (g_qjs_content == c) {
		g_qjs_content = NULL;
		g_qjs_document = NULL;
	}
}

/* ---- dom_string helper ---- */
static dom_string *qjs_make_domstr(const char *s)
{
	dom_string *ds = NULL;
	dom_string_create((const uint8_t *)s, strlen(s), &ds);
	return ds;
}

/* ---- QuickJS class for element wrappers ---- */
static JSClassID s_el_class_id;

/* ===================================================================
 * fixes541: node-identity map + owner-document keepalive (DOM port phase one)
 *
 * MacSurf reached this design (fixes541) largely on its own, but the same
 * node-identity map + finalizer-unref + per-wrapper owner-document keepalive
 * discipline was worked out in parallel by sempaisquad in ClassicNetSurf's
 * hand-written quickjs.c -- a concurrent NetSurf-on-OS 9 effort at a different
 * scope.  Credit sempaisquad <https://github.com/sempaisquad> as a contributor
 * for the convergent lifecycle pattern.
 *
 * Atomic unit (see docs/research/quickjs-dom-port-phase1.md and
 * teardown-ordering-audit.md): the map, the wrapper, the finalizer and the
 * keepalive are ONE mechanism — they cannot be split.  Why:
 *   - The map gives a node AT MOST ONE wrapper (lookup-then-create), so
 *     el.parentNode === el.parentNode and removeChild/contains identity hold.
 *   - "At most one wrapper" is also what guarantees the finalizer runs EXACTLY
 *     ONCE per node, which is the precondition that makes the keepalive's
 *     balanced ref/unref sound.
 *   - Single-owner / single-release: each wrapper owns exactly ONE node ref
 *     and ONE owner-document keepalive ref (g_qjs_document captured at wrap
 *     time).  Both are released SOLELY by the finalizer or the realm drain —
 *     nothing else unrefs that node.  The keepalive holds the document alive
 *     for as long as ANY wrapper references it, so the document outlives its
 *     wrappers regardless of teardown order (the audit proved ordering alone
 *     cannot make the three teardown paths safe).
 *   - The map entry's EXISTENCE is the "refs still held / not yet cleaned"
 *     flag, so the finalizer and the realm drain are idempotent against each
 *     other: whoever removes the entry drops the two refs; the other no-ops.
 * =================================================================== */

#define QJS_WRAP_BUCKETS 1024u

struct qjs_wrap_entry {
	dom_node  *node;       /* key; wrapper's single owned node ref     */
	dom_node  *owner_doc;  /* keepalive; wrapper's single owned doc ref */
	JSValue    val;        /* WEAK handle to the wrapper JS object      */
	struct qjs_wrap_entry *next;
};

/* One context per window (tabs disabled by default), so a file-static map is
 * sufficient; it is fully drained on realm reset and heap destroy. */
static struct qjs_wrap_entry *s_wrap_buckets[QJS_WRAP_BUCKETS];

/* install is defined far below; wrap (just under here) folds it in on miss. */
static void qjs_el_install_native_attrs(JSContext *ctx, JSValue obj);

static unsigned int qjs_wrap_hash(dom_node *node)
{
	return (unsigned int)(((size_t)node >> 4) & (QJS_WRAP_BUCKETS - 1u));
}

static struct qjs_wrap_entry *qjs_wrap_lookup(dom_node *node)
{
	struct qjs_wrap_entry *e = s_wrap_buckets[qjs_wrap_hash(node)];
	while (e != NULL) {
		if (e->node == node) return e;
		e = e->next;
	}
	return NULL;
}

/* Returns 1 on success, 0 on malloc failure (caller degrades gracefully). */
static int qjs_wrap_insert(dom_node *node, dom_node *owner_doc, JSValue val)
{
	unsigned int h = qjs_wrap_hash(node);
	struct qjs_wrap_entry *e =
		(struct qjs_wrap_entry *)malloc(sizeof(struct qjs_wrap_entry));
	if (e == NULL) return 0;
	e->node = node;
	e->owner_doc = owner_doc;
	e->val = val;
	e->next = s_wrap_buckets[h];
	s_wrap_buckets[h] = e;
	return 1;
}

/* Unlink the entry for node (does NOT drop refs — the caller does). */
static void qjs_wrap_remove(dom_node *node)
{
	unsigned int h = qjs_wrap_hash(node);
	struct qjs_wrap_entry *e = s_wrap_buckets[h];
	struct qjs_wrap_entry *prev = NULL;
	while (e != NULL) {
		if (e->node == node) {
			if (prev == NULL) s_wrap_buckets[h] = e->next;
			else prev->next = e->next;
			free(e);
			return;
		}
		prev = e;
		e = e->next;
	}
}

/* Realm-reset / heap-destroy drain — does BOTH halves then clears.  The
 * finalizers fired by JS_FreeContext normally empty the map first (each removes
 * its entry and drops node+owner_doc); this is the GUARANTEED, pure-C release
 * for any entry whose finalizer did not run (e.g. wrapper objects still in
 * obj->method reference cycles that JS_FreeContext leaves for JS_FreeRuntime):
 * drop the node ref AND the owner-document keepalive ref, THEN clear the entry.
 * Never touches e->val — the JS object may already be gone after JS_FreeContext;
 * the matching finalizer (if it runs later) finds no map entry and no-ops. */
static void qjs_wrap_drain(void)
{
	unsigned int i;
	int cleaned = 0;
	int foreign = 0;
	dom_document *cur = g_qjs_document;
	for (i = 0; i < QJS_WRAP_BUCKETS; i++) {
		struct qjs_wrap_entry *e = s_wrap_buckets[i];
		while (e != NULL) {
			struct qjs_wrap_entry *next = e->next;
			/* fixes867 (#293) — count wrappers we are releasing that belong
			 * to a DIFFERENT document than the realm being reset. */
			if (e->owner_doc != (dom_node *)cur) foreign++;
			if (e->node)      macsurf_dom_node_unref(e->node);
			if (e->owner_doc) macsurf_dom_node_unref(e->owner_doc);
			free(e);
			cleaned++;
			e = next;
		}
		s_wrap_buckets[i] = NULL;
	}
	/* fixes867 (#293) — was "qjs wrap_drain: %d wrapper(s) released", which the
	 * failures-only gate DROPS, so this has never reached disk.
	 *
	 * `foreign` is the whole point, and it answers a live architectural
	 * question with a number instead of an argument. This map is file-static
	 * and shared by EVERY runtime (its own comment at the declaration says
	 * "one context per window ... so a file-static map is sufficient" — which
	 * js_newheap() running per-(i)frame contradicts), and this drain walks
	 * every bucket unconditionally on realm-reset and heap-destroy. So when an
	 * IFRAME resets its realm, it may be releasing the PARENT's live wrappers'
	 * node refs and doc keepalives.
	 *   foreign == 0 always  -> the bug is theoretical; partitioning the map
	 *                           can wait for the window.parent work.
	 *   foreign  > 0         -> it is REAL and biting today, and the map MUST
	 *                           be partitioned before wrapper count grows
	 *                           (i.e. before real DOM traversal lands).
	 * That is the Phase 2-blocks-Phase-3-or-Phase-4 decision, settled by
	 * observation rather than by my reading. */
	macsurf_debug_log_writef(
		"WORK wrapmap drain freed=%d foreign=%d curdoc=%p heap=%p",
		cleaned, foreign, (void *)cur, (void *)g_heap);
}

/* Pointer-validate guard (ON from phase one).  Single chokepoint every accessor
 * reads the node through: rejects NULL, misaligned, and out-of-heap pointers so
 * a foreign/garbage JS object handed to a DOM method no-ops instead of
 * dereferencing wild memory.  (No generation token in phase one: the keepalive
 * makes a wrapped node un-freeable while wrapped, and a realm reset frees the
 * whole context, so there is no stale-wrapper-survives-its-node window on this
 * surface.  Revisit if coverage widens.) */
static dom_node *qjs_get_node(JSValueConst val)
{
	dom_node *n = (dom_node *)JS_GetOpaque(val, s_el_class_id);
	if (n == NULL) return NULL;
	if (((size_t)n & 3u) != 0) return NULL;
	if (!macsurf_ptr_is_heap((const void *)(n)))
		return NULL;
	return n;
}

static void qjs_el_finalizer(JSRuntime *rt, JSValue val)
{
	dom_node *node = (dom_node *)JS_GetOpaque(val, s_el_class_id);
	(void)rt;
	if (node == NULL) return;
	/* Single-release: drop the wrapper's one node ref + one owner-doc ref,
	 * exactly once.  The map entry is the guard: if the realm drain already
	 * cleaned this node the entry is gone and we must NOT unref again. */
	{
		struct qjs_wrap_entry *e = qjs_wrap_lookup(node);
		if (e != NULL) {
			dom_node *owner_doc = e->owner_doc;
			qjs_wrap_remove(node);
			macsurf_dom_node_unref(node);
			if (owner_doc) macsurf_dom_node_unref(owner_doc);
		}
	}
}

static JSClassDef s_el_class = { "MacSurfElement", qjs_el_finalizer };

/* Build (or reuse) the ONE JS wrapper object for a dom_element*.
 *
 * Ref contract (consume / single-owner / single-release): the caller passes an
 * OWNED node ref (every libdom getter returns one).
 *   - MISS: the new wrapper ADOPTS that ref as its single node ref, and takes
 *     one keepalive ref on g_qjs_document.  Methods are installed once here.
 *   - HIT:  the wrapper already owns its one node ref, so the caller's
 *     redundant ref is released here, and a NEW JS reference to the SAME object
 *     is returned (node identity holds).  The wrapper's own ref is untouched.
 * In both cases the wrapper's node ref and doc keepalive are released SOLELY by
 * qjs_el_finalizer / qjs_wrap_drain — nothing else unrefs that node. */
static JSValue qjs_wrap_element(JSContext *ctx, dom_element *el)
{
	dom_node *node = (dom_node *)el;
	struct qjs_wrap_entry *hit;
	dom_node *owner_doc;
	JSValue obj;
	dom_string *tag_ds = NULL;
	const char *tag_str = "";
	char tag_lc[32];
	int i;

	if (el == NULL) return JS_NULL;

	hit = qjs_wrap_lookup(node);
	if (hit != NULL) {
		/* HIT: release the caller's transferred ref; the existing wrapper
		 * already owns the node's single ref.  Hand back a new reference
		 * to the same object so identity holds. */
		macsurf_dom_node_unref(node);
		return JS_DupValue(ctx, hit->val);
	}

	/* MISS: build the single wrapper for this node. */
	obj = JS_NewObjectClass(ctx, (int)s_el_class_id);
	if (JS_IsException(obj)) {
		macsurf_dom_node_unref(node);   /* consume transferred ref */
		return JS_NULL;
	}
	JS_SetOpaque(obj, el);                  /* adopt the transferred node ref */

	/* Keepalive: own one ref on the current document so it outlives this
	 * wrapper no matter which scope (content vs heap) tears down first. */
	owner_doc = (dom_node *)g_qjs_document;
	if (owner_doc) macsurf_dom_node_ref(owner_doc);

	if (qjs_wrap_insert(node, owner_doc, obj) == 0) {
		/* malloc failed: degrade to a dead wrapper rather than leak or
		 * crash.  Drop BOTH refs now and null the opaque so the finalizer
		 * and the accessor guard both no-op on this object. */
		JS_SetOpaque(obj, NULL);
		macsurf_dom_node_unref(node);
		if (owner_doc) macsurf_dom_node_unref(owner_doc);
		return obj;
	}

	/* tagName / nodeName */
	if (macsurf_dom_element_get_tag_name(el, &tag_ds) == DOM_NO_ERR
	    && tag_ds != NULL) {
		tag_str = dom_string_data(tag_ds);
	}
	for (i = 0; i < 31 && tag_str[i]; i++) {
		char c = tag_str[i];
		tag_lc[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
	}
	tag_lc[i] = '\0';
	if (tag_ds) macsurf_dom_string_unref(tag_ds);

	JS_SetPropertyStr(ctx, obj, "tagName",  JS_NewString(ctx, tag_lc));
	JS_SetPropertyStr(ctx, obj, "nodeName", JS_NewString(ctx, tag_lc));
	JS_SetPropertyStr(ctx, obj, "nodeType", JS_NewInt32(ctx, 1));
	JS_SetPropertyStr(ctx, obj, "__ptr",
		JS_NewInt64(ctx, (long long)(size_t)el));

	/* Install methods ONCE per node (folded in from qjs_wrap_element_full so
	 * cache hits skip the heavy re-install). */
	qjs_el_install_native_attrs(ctx, obj);
	return obj;
}

/* ---- getAttribute / setAttribute as QJS C functions registered on    */
/*      the element class (called via __getAttribute / __setAttribute   */
/*      properties set at class registration time).                     */
/*      These retrieve the element pointer via JS_GetOpaque.            */

/* getAttribute / setAttribute as CFunctionData: func_data[0] is the elem obj */
static JSValue qjs_el_getAttribute_data(JSContext *ctx,
		JSValueConst this_val, int argc, JSValueConst *argv,
		int magic, JSValueConst *func_data)
{
	dom_element *el;
	const char *name_cstr;
	dom_string *name_ds, *val_ds;
	JSValue ret;

	(void)this_val; (void)magic;
	el = (dom_element *)qjs_get_node(func_data[0]);
	if (el == NULL || argc < 1) return JS_NULL;
	name_cstr = JS_ToCString(ctx, argv[0]);
	if (name_cstr == NULL) return JS_NULL;
	name_ds = qjs_make_domstr(name_cstr);
	JS_FreeCString(ctx, name_cstr);
	if (name_ds == NULL) return JS_NULL;
	val_ds = NULL;
	if (macsurf_dom_element_get_attribute(el, name_ds, &val_ds) != DOM_NO_ERR
	    || val_ds == NULL) {
		macsurf_dom_string_unref(name_ds);
		return JS_NULL;
	}
	ret = JS_NewString(ctx, dom_string_data(val_ds));
	macsurf_dom_string_unref(val_ds);
	macsurf_dom_string_unref(name_ds);
	return ret;
}

static JSValue qjs_el_setAttribute_data(JSContext *ctx,
		JSValueConst this_val, int argc, JSValueConst *argv,
		int magic, JSValueConst *func_data)
{
	dom_element *el;
	const char *name_cstr, *val_cstr;
	dom_string *name_ds, *val_ds;

	(void)this_val; (void)magic;
	el = (dom_element *)qjs_get_node(func_data[0]);
	if (el == NULL || argc < 2) return JS_UNDEFINED;
	name_cstr = JS_ToCString(ctx, argv[0]);
	val_cstr  = JS_ToCString(ctx, argv[1]);
	if (name_cstr == NULL || val_cstr == NULL) {
		if (name_cstr) JS_FreeCString(ctx, name_cstr);
		if (val_cstr)  JS_FreeCString(ctx, val_cstr);
		return JS_UNDEFINED;
	}
	name_ds = qjs_make_domstr(name_cstr);
	val_ds  = qjs_make_domstr(val_cstr);
	JS_FreeCString(ctx, name_cstr);
	JS_FreeCString(ctx, val_cstr);
	if (name_ds && val_ds) {
		macsurf_dom_element_set_attribute(el, name_ds, val_ds);
		if (g_qjs_content) macos9_js_mark_dom_dirty(g_qjs_content);
	}
	if (name_ds) macsurf_dom_string_unref(name_ds);
	if (val_ds)  macsurf_dom_string_unref(val_ds);
	return JS_UNDEFINED;
}

/* ---- Forward declarations ---- */
static JSValue qjs_wrap_element_full(JSContext *ctx, dom_element *el);
static void qjs_collect_by_tag(JSContext *ctx, dom_node *node,
		const char *tag_lc, JSValue arr, int *count);
/* fixes878 — node-type-dispatching wrapper, defined after the three concrete
 * wrappers.  The node-oriented traversal getters below are installed by
 * qjs_el_install_native_attrs, which sits ABOVE qjs_wrap_text_node /
 * qjs_wrap_fragment, so they reach it through this declaration. */
static JSValue qjs_wrap_any_node(JSContext *ctx, dom_node *node);

/* ---- textContent read ---- */
static JSValue qjs_el_get_text_content_data(JSContext *ctx,
		JSValueConst this_val, int argc, JSValueConst *argv,
		int magic, JSValueConst *func_data)
{
	dom_element *el;
	dom_string *ds = NULL;
	JSValue ret;
	(void)this_val; (void)argc; (void)argv; (void)magic;
	el = (dom_element *)qjs_get_node(func_data[0]);
	if (el == NULL) return JS_NewString(ctx, "");
	if (macsurf_dom_node_get_text_content((dom_node *)el, &ds) != DOM_NO_ERR
	    || ds == NULL) return JS_NewString(ctx, "");
	ret = JS_NewString(ctx, dom_string_data(ds));
	macsurf_dom_string_unref(ds);
	return ret;
}

/* ---- textContent write ---- */
static JSValue qjs_el_set_text_content_data(JSContext *ctx,
		JSValueConst this_val, int argc, JSValueConst *argv,
		int magic, JSValueConst *func_data)
{
	dom_element *el;
	const char *s;
	dom_string *ds;
	(void)this_val; (void)magic;
	el = (dom_element *)qjs_get_node(func_data[0]);
	if (el == NULL || argc < 1) return JS_UNDEFINED;
	s = JS_ToCString(ctx, argv[0]);
	if (s == NULL) return JS_UNDEFINED;
	ds = qjs_make_domstr(s);
	JS_FreeCString(ctx, s);
	if (ds) {
		macsurf_dom_node_set_text_content((dom_node *)el, ds);
		macsurf_dom_string_unref(ds);
		if (g_qjs_content) macos9_js_mark_dom_dirty(g_qjs_content);
	}
	return JS_UNDEFINED;
}

/* Find the first element child of `parent` whose tag name (case-
 * insensitive) matches `want`. Returns a REF'D dom_node, or NULL. Mirrors
 * the body/head lookup already used by qjs_wrap_doc_section. */
static dom_node *qjs_find_child_element_by_tag(dom_node *parent,
		const char *want)
{
	dom_node *child = NULL, *next = NULL;
	dom_node_type ntype;

	macsurf_dom_node_get_first_child(parent, &child);
	while (child != NULL) {
		ntype = 0;
		macsurf_dom_node_get_node_type(child, &ntype);
		if (ntype == 1) {
			dom_string *tname = NULL;
			if (macsurf_dom_element_get_tag_name(
					(dom_element *) child, &tname) == DOM_NO_ERR
			    && tname != NULL) {
				const char *ts = dom_string_data(tname);
				char lc[16];
				int i;
				for (i = 0; i < 15 && ts[i]; i++) {
					char c = ts[i];
					lc[i] = (c >= 'A' && c <= 'Z')
						? (char) (c + 32) : c;
				}
				lc[i] = '\0';
				macsurf_dom_string_unref(tname);
				if (strcmp(lc, want) == 0) return child;
			}
		}
		next = NULL;
		macsurf_dom_node_get_next_sibling(child, &next);
		macsurf_dom_node_unref(child);
		child = next;
	}
	return NULL;
}

/* ---- innerHTML= via real HTML fragment parsing (fixes846, #167 S3) ----
 * Previously: el.textContent = html.replace(/<[^>]*>/g,''), i.e. every tag
 * was stripped and only the text carcass assigned, so any markup-injecting
 * JS (template-string HTML injection, dangerouslySetInnerHTML-style
 * rendering) never built real elements -- confirmed via repo-wide grep that
 * dom_hubbub_fragment_parser_create had ZERO callers anywhere in this
 * codebase before this.
 *
 * This libdom binding's dom_hubbub_fragment_parser_create() has NO context-
 * element parameter (confirmed by reading bindings/hubbub/parser.c) -- it
 * just runs a normal from-scratch parse with the fragment standing in for
 * the document node. Per the HTML5 tree-construction algorithm's "before
 * html" / "before head" insertion modes, that means hubbub IMPLICITLY
 * wraps the input in html>head+body, same as parsing a full page. Naively
 * appending the fragment's own children puts a SINGLE implied <html>
 * element into el instead of the real content one level down inside its
 * <body> -- caught by the S0 harness's Test 4 (children.length was 1, not
 * the 2 real elements the test HTML actually contains). Fix: descend
 * fragment -> <html> -> <body> and move THAT element's children, not the
 * fragment's. Falls back to the fragment's own children if no <html>/
 * <body> is found (e.g. a future context-aware parser is swapped in). */
static JSValue qjs_el_set_inner_html_data(JSContext *ctx,
		JSValueConst this_val, int argc, JSValueConst *argv,
		int magic, JSValueConst *func_data)
{
	dom_element *el;
	dom_node *child, *next, *removed, *append_result;
	dom_node *src_parent, *html_el, *body_el;
	dom_hubbub_parser_params params;
	dom_hubbub_parser *parser = NULL;
	dom_hubbub_error herr;
	dom_document_fragment *frag = NULL;
	const char *html_src;
	size_t html_len = 0;

	(void) this_val; (void) magic;
	el = (dom_element *) qjs_get_node(func_data[0]);
	if (el == NULL || argc < 1 || g_qjs_document == NULL)
		return JS_UNDEFINED;
	html_src = JS_ToCStringLen(ctx, &html_len, argv[0]);
	if (html_src == NULL) return JS_UNDEFINED;

	memset(&params, 0, sizeof(params));
	params.enc = "UTF-8";
	params.fix_enc = true;
	params.enable_script = false;

	herr = dom_hubbub_fragment_parser_create(&params, g_qjs_document,
			&parser, &frag);
	if (herr != DOM_HUBBUB_OK || parser == NULL || frag == NULL) {
		JS_FreeCString(ctx, html_src);
		return JS_UNDEFINED;
	}
	dom_hubbub_parser_parse_chunk(parser,
			(const uint8_t *) html_src, html_len);
	dom_hubbub_parser_completed(parser);
	dom_hubbub_parser_destroy(parser);
	JS_FreeCString(ctx, html_src);

	/* Clear existing children before inserting the parsed content --
	 * innerHTML= REPLACES content, it does not append. */
	child = NULL;
	macsurf_dom_node_get_first_child((dom_node *) el, &child);
	while (child != NULL) {
		next = NULL;
		macsurf_dom_node_get_next_sibling(child, &next);
		removed = NULL;
		macsurf_dom_node_remove_child((dom_node *) el, child, &removed);
		if (removed != NULL) macsurf_dom_node_unref(removed);
		macsurf_dom_node_unref(child);
		child = next;
	}

	src_parent = (dom_node *) frag;
	html_el = qjs_find_child_element_by_tag((dom_node *) frag, "html");
	if (html_el != NULL) {
		body_el = qjs_find_child_element_by_tag(html_el, "body");
		if (body_el != NULL) {
			src_parent = body_el;
		} else {
			src_parent = html_el;
		}
	}

	/* Move src_parent's children into el one at a time (dom_node_
	 * append_child MOVES a node already attached elsewhere -- no clone
	 * needed). Re-fetch first_child each iteration since removing/
	 * appending mutates src_parent's child list. */
	child = NULL;
	macsurf_dom_node_get_first_child(src_parent, &child);
	while (child != NULL) {
		append_result = NULL;
		macsurf_dom_node_append_child((dom_node *) el, child,
				&append_result);
		if (append_result != NULL) macsurf_dom_node_unref(append_result);
		macsurf_dom_node_unref(child);
		child = NULL;
		macsurf_dom_node_get_first_child(src_parent, &child);
	}
	if (html_el != NULL) macsurf_dom_node_unref(html_el);
	if (src_parent != html_el && src_parent != (dom_node *) frag)
		macsurf_dom_node_unref(src_parent);
	macsurf_dom_node_unref((dom_node *) frag);

	if (g_qjs_content) macos9_js_mark_dom_dirty(g_qjs_content);
	return JS_UNDEFINED;
}

/* ---- parentNode ---- */
static JSValue qjs_el_get_parent_node_data(JSContext *ctx,
		JSValueConst this_val, int argc, JSValueConst *argv,
		int magic, JSValueConst *func_data)
{
	dom_element *el;
	dom_node *parent = NULL;
	dom_node_type ntype = 0;
	(void)this_val; (void)argc; (void)argv; (void)magic;
	el = (dom_element *)qjs_get_node(func_data[0]);
	if (el == NULL) return JS_NULL;
	macsurf_dom_node_get_parent_node((dom_node *)el, &parent);
	if (parent == NULL) return JS_NULL;
	macsurf_dom_node_get_node_type(parent, &ntype);
	if (ntype != 1) { macsurf_dom_node_unref(parent); return JS_NULL; }
	return qjs_wrap_element_full(ctx, (dom_element *)parent);
}

/* ---- nextElementSibling ---- */
static JSValue qjs_el_get_next_sibling_data(JSContext *ctx,
		JSValueConst this_val, int argc, JSValueConst *argv,
		int magic, JSValueConst *func_data)
{
	dom_element *el;
	dom_node *sib = NULL, *next = NULL;
	dom_node_type ntype = 0;
	(void)this_val; (void)argc; (void)argv; (void)magic;
	el = (dom_element *)qjs_get_node(func_data[0]);
	if (el == NULL) return JS_NULL;
	macsurf_dom_node_get_next_sibling((dom_node *)el, &sib);
	while (sib) {
		macsurf_dom_node_get_node_type(sib, &ntype);
		if (ntype == 1) return qjs_wrap_element_full(ctx, (dom_element *)sib);
		macsurf_dom_node_get_next_sibling(sib, &next);
		macsurf_dom_node_unref(sib);
		sib = next;
	}
	return JS_NULL;
}

/* ---- previousElementSibling ---- */
static JSValue qjs_el_get_prev_sibling_data(JSContext *ctx,
		JSValueConst this_val, int argc, JSValueConst *argv,
		int magic, JSValueConst *func_data)
{
	dom_element *el;
	dom_node *sib = NULL, *prev = NULL;
	dom_node_type ntype = 0;
	(void)this_val; (void)argc; (void)argv; (void)magic;
	el = (dom_element *)qjs_get_node(func_data[0]);
	if (el == NULL) return JS_NULL;
	macsurf_dom_node_get_previous_sibling((dom_node *)el, &sib);
	while (sib) {
		macsurf_dom_node_get_node_type(sib, &ntype);
		if (ntype == 1) return qjs_wrap_element_full(ctx, (dom_element *)sib);
		macsurf_dom_node_get_previous_sibling(sib, &prev);
		macsurf_dom_node_unref(sib);
		sib = prev;
	}
	return JS_NULL;
}

/* fixes867 (#293) — THE BLINDFOLD, removed.
 *
 * appendChild/removeChild/insertBefore each called their libdom op and DROPPED
 * the dom_exception on the floor, then returned the child as if it had been
 * inserted.  A rejected insert was indistinguishable from a successful one --
 * to JS, to the harness, to the log.  Six rounds of hackaday debugging were
 * spent above a failure this hid, and harness Test 11's `appended=1` assert was
 * a tautology that could not fail.
 *
 * _dom_node_insert_before has THREE silent rejections that return before any
 * attach or DOMNodeInserted dispatch (node.c:726-760), and they demand
 * completely different fixes:
 *   NO_MODIFICATION_ALLOWED_ERR + dispatching_mutation>0
 *       -> the mutation semaphore (node.c:2045): libdom marks the document
 *          read-only while dispatching a mutation event, on the assumption that
 *          "nothing should be listening" -- but NetSurf runs its whole script
 *          engine from that default action.  Architectural fix.
 *   WRONG_DOCUMENT_ERR + childOwner != nodeOwner
 *       -> document identity: g_qjs_document is a process-global set per
 *          js_newthread, so the last thread created wins for every realm.
 *          State fix.
 *   HIERARCHY_REQUEST_ERR
 *       -> neither.
 * Logging only the exception CODE would still need the cause inferred from a
 * statistical pattern; logging the OPERANDS makes one line decisive.
 *
 * Also THROWS on failure, which is what the DOM spec requires and what lets a
 * page's own error handling see the truth. `seq` is a monotonic insert counter
 * so execution ORDER is visible -- needed before choosing between deferring
 * script execution (which reorders) and dropping the semaphore (which doesn't).
 */
extern uint32_t _dom_document_dispatching_mutation(dom_document *doc);

static int s_dom_mut_seq = 0;

static JSValue qjs_dom_mut_check(JSContext *ctx, const char *op,
		dom_exception exc, dom_node *parent, dom_node *child)
{
	dom_document *pdoc = NULL;
	dom_document *cdoc = NULL;
	int seq = ++s_dom_mut_seq;

	if (exc == DOM_NO_ERR) return JS_UNDEFINED;

	/* Failure path only — these are diagnostic reads, kept off the hot path.
	 * get_owner_document hands back an OWNED ref; unref both below. */
	if (parent != NULL) macsurf_dom_node_get_owner_document(parent, &pdoc);
	if (child  != NULL) macsurf_dom_node_get_owner_document(child,  &cdoc);

	macsurf_debug_log_writef(
		"WORK dom %s FAIL exc=%d seq=%d dispatching_mutation=%d "
		"childOwner=%p nodeOwner=%p parent=%p child=%p",
		op, (int)exc, seq,
		(int)_dom_document_dispatching_mutation(pdoc),
		(void *)cdoc, (void *)pdoc, (void *)parent, (void *)child);

	if (pdoc != NULL) macsurf_dom_node_unref((dom_node *)pdoc);
	if (cdoc != NULL) macsurf_dom_node_unref((dom_node *)cdoc);

	return JS_ThrowTypeError(ctx, "%s failed: DOM exception %d", op, (int)exc);
}

/* ---- appendChild ---- */
static JSValue qjs_el_append_child_data(JSContext *ctx,
		JSValueConst this_val, int argc, JSValueConst *argv,
		int magic, JSValueConst *func_data)
{
	dom_element *el;
	dom_element *child_el;
	dom_node *result = NULL;
	dom_exception exc;
	JSValue err;
	(void)this_val; (void)magic;
	el = (dom_element *)qjs_get_node(func_data[0]);
	if (el == NULL || argc < 1) return JS_NULL;
	child_el = (dom_element *)qjs_get_node(argv[0]);
	if (child_el == NULL) return JS_NULL;
	exc = macsurf_dom_node_append_child((dom_node *)el, (dom_node *)child_el,
			&result);
	if (result) macsurf_dom_node_unref(result);
	err = qjs_dom_mut_check(ctx, "appendChild", exc, (dom_node *)el,
			(dom_node *)child_el);
	if (JS_IsException(err)) return err;
	if (g_qjs_content) macos9_js_mark_dom_dirty(g_qjs_content);
	return JS_DupValue(ctx, argv[0]);
}

/* ---- removeChild ---- */
static JSValue qjs_el_remove_child_data(JSContext *ctx,
		JSValueConst this_val, int argc, JSValueConst *argv,
		int magic, JSValueConst *func_data)
{
	dom_element *el;
	dom_element *child_el;
	dom_node *result = NULL;
	dom_exception exc;
	JSValue err;
	(void)this_val; (void)magic;
	el = (dom_element *)qjs_get_node(func_data[0]);
	if (el == NULL || argc < 1) return JS_NULL;
	child_el = (dom_element *)qjs_get_node(argv[0]);
	if (child_el == NULL) return JS_NULL;
	exc = macsurf_dom_node_remove_child((dom_node *)el, (dom_node *)child_el,
			&result);
	if (result) macsurf_dom_node_unref(result);
	/* fixes867 (#293) — same blindfold as appendChild.  removeChild is hit by
	 * the SAME mutation semaphore (node.c:989 dispatches DOMNodeRemoval, and
	 * :744's readonly check guards the removal path too), so a JS remove from
	 * inside a mutation handler fails just as silently. */
	err = qjs_dom_mut_check(ctx, "removeChild", exc, (dom_node *)el,
			(dom_node *)child_el);
	if (JS_IsException(err)) return err;
	if (g_qjs_content) macos9_js_mark_dom_dirty(g_qjs_content);
	return JS_DupValue(ctx, argv[0]);
}

/* ---- insertBefore ---- */
static JSValue qjs_el_insert_before_data(JSContext *ctx,
		JSValueConst this_val, int argc, JSValueConst *argv,
		int magic, JSValueConst *func_data)
{
	dom_element *el;
	dom_element *new_el;
	dom_element *ref_el;
	dom_node *result = NULL;
	dom_exception exc;
	JSValue err;
	(void)this_val; (void)magic;
	el = (dom_element *)qjs_get_node(func_data[0]);
	if (el == NULL || argc < 1) return JS_NULL;
	new_el = (dom_element *)qjs_get_node(argv[0]);
	if (new_el == NULL) return JS_NULL;
	ref_el = (argc >= 2 && !JS_IsNull(argv[1]))
		? (dom_element *)qjs_get_node(argv[1]) : NULL;
	exc = macsurf_dom_node_insert_before((dom_node *)el, (dom_node *)new_el,
		(dom_node *)ref_el, &result);
	if (result) macsurf_dom_node_unref(result);
	/* fixes867 (#293) — same blindfold as appendChild, and this one matters
	 * most for modern pages: insertBefore(node, null) is Preact's ONLY
	 * insertion primitive (it never calls appendChild), so a silent rejection
	 * here means a React/Preact app renders nothing, with no error. */
	err = qjs_dom_mut_check(ctx, "insertBefore", exc, (dom_node *)el,
			(dom_node *)new_el);
	if (JS_IsException(err)) return err;
	if (g_qjs_content) macos9_js_mark_dom_dirty(g_qjs_content);
	return JS_DupValue(ctx, argv[0]);
}

/* ---- removeAttribute ---- */
static JSValue qjs_el_remove_attribute_data(JSContext *ctx,
		JSValueConst this_val, int argc, JSValueConst *argv,
		int magic, JSValueConst *func_data)
{
	dom_element *el;
	const char *name_cstr;
	dom_string *name_ds;
	(void)this_val; (void)magic;
	el = (dom_element *)qjs_get_node(func_data[0]);
	if (el == NULL || argc < 1) return JS_UNDEFINED;
	name_cstr = JS_ToCString(ctx, argv[0]);
	if (name_cstr == NULL) return JS_UNDEFINED;
	name_ds = qjs_make_domstr(name_cstr);
	JS_FreeCString(ctx, name_cstr);
	if (name_ds) {
		macsurf_dom_element_remove_attribute(el, name_ds);
		/* fixes843b — this real DOM mutation never marked dirty, so
		 * el.removeAttribute(...) (a common show/hide idiom) never
		 * triggered a repaint. Match setAttribute's behaviour. */
		if (g_qjs_content) macos9_js_mark_dom_dirty(g_qjs_content);
		macsurf_dom_string_unref(name_ds);
	}
	return JS_UNDEFINED;
}

/* ---- hasAttribute ---- */
static JSValue qjs_el_has_attribute_data(JSContext *ctx,
		JSValueConst this_val, int argc, JSValueConst *argv,
		int magic, JSValueConst *func_data)
{
	dom_element *el;
	const char *name_cstr;
	dom_string *name_ds;
	int has = 0;
	(void)this_val; (void)magic;
	el = (dom_element *)qjs_get_node(func_data[0]);
	if (el == NULL || argc < 1) return JS_FALSE;
	name_cstr = JS_ToCString(ctx, argv[0]);
	if (name_cstr == NULL) return JS_FALSE;
	name_ds = qjs_make_domstr(name_cstr);
	JS_FreeCString(ctx, name_cstr);
	if (name_ds) {
		macsurf_dom_element_has_attribute(el, name_ds, &has);
		macsurf_dom_string_unref(name_ds);
	}
	return JS_NewBool(ctx, has);
}

/* ---- children (element-only child list) ---- */
/* ---- fixes878: node-oriented traversal (magic selects which edge) ----
 *
 * These replace hardcoded `null` data properties frozen onto every element at
 * wrap time. The old surface never consulted libdom and never updated as the
 * tree mutated, so the canonical clear-children idiom
 *     while (node.firstChild) node.removeChild(node.firstChild);
 * saw an empty element and no-opped, and every hand-rolled node walk visited
 * nothing. The C plumbing was already here and already used by this file's own
 * tree walkers -- only the JS exposure was fake.
 *
 * magic: 0=firstChild 1=lastChild 2=nextSibling 3=previousSibling
 */
static JSValue qjs_el_get_edge_data(JSContext *ctx,
		JSValueConst this_val, int argc, JSValueConst *argv,
		int magic, JSValueConst *func_data)
{
	dom_node *self;
	dom_node *out = NULL;
	dom_exception exc;

	(void) this_val; (void) argc; (void) argv;
	self = qjs_get_node(func_data[0]);
	if (self == NULL) return JS_NULL;

	switch (magic) {
	case 0:  exc = macsurf_dom_node_get_first_child(self, &out); break;
	case 1:  exc = macsurf_dom_node_get_last_child(self, &out); break;
	case 2:  exc = macsurf_dom_node_get_next_sibling(self, &out); break;
	default: exc = macsurf_dom_node_get_previous_sibling(self, &out); break;
	}
	if (exc != DOM_NO_ERR || out == NULL) return JS_NULL;
	/* `out` carries a ref; qjs_wrap_any_node adopts it. */
	return qjs_wrap_any_node(ctx, out);
}

/* ---- fixes878: node.childNodes ----
 * ALL children, unlike `children` which is elements-only. Returns a plain
 * snapshot array, not a live NodeList: length/indexing/forEach work, but it
 * does not update as the tree changes. That is a real (documented) gap, not a
 * silent one -- a live NodeList needs an invalidation hook this binding has no
 * home for yet. */
static JSValue qjs_el_get_child_nodes_data(JSContext *ctx,
		JSValueConst this_val, int argc, JSValueConst *argv,
		int magic, JSValueConst *func_data)
{
	dom_node *self;
	dom_node *child = NULL, *next = NULL;
	JSValue arr;
	int count = 0;

	(void) this_val; (void) argc; (void) argv; (void) magic;
	arr = JS_NewArray(ctx);
	self = qjs_get_node(func_data[0]);
	if (self == NULL) return arr;

	if (macsurf_dom_node_get_first_child(self, &child) != DOM_NO_ERR)
		return arr;
	while (child != NULL) {
		JSValue w;
		/* Read the sibling link BEFORE wrapping: the wrapper adopts
		 * child's ref, so child must not be touched afterwards. */
		if (macsurf_dom_node_get_next_sibling(child, &next) != DOM_NO_ERR)
			next = NULL;
		w = qjs_wrap_any_node(ctx, child);   /* consumes child's ref */
		if (JS_IsNull(w)) {
			/* unwrappable type (already unref'd by the wrapper) */
		} else {
			JS_SetPropertyUint32(ctx, arr, (unsigned int) count, w);
			count++;
		}
		child = next;
		next = NULL;
	}
	return arr;
}

/* ---- fixes878: node.cloneNode(deep) ----
 * Was `function(){return el;}` -- it handed back the element ITSELF, so
 * parent.appendChild(node.cloneNode(true)) MOVED the original instead of
 * copying it. Pages rendered one relocated node where they meant N copies,
 * with no error anywhere. */
static JSValue qjs_el_clone_node_data(JSContext *ctx,
		JSValueConst this_val, int argc, JSValueConst *argv,
		int magic, JSValueConst *func_data)
{
	dom_node *self;
	dom_node *copy = NULL;
	int deep = 0;

	(void) this_val; (void) magic;
	self = qjs_get_node(func_data[0]);
	if (self == NULL) return JS_NULL;
	if (argc >= 1) deep = JS_ToBool(ctx, argv[0]) ? 1 : 0;

	if (macsurf_dom_node_clone_node(self, deep, &copy) != DOM_NO_ERR ||
	    copy == NULL)
		return JS_NULL;
	/* The clone is parentless and carries a ref, which the wrapper adopts. */
	return qjs_wrap_any_node(ctx, copy);
}

/* ---- fixes878: node.contains(other) ----
 * Was `function(){return false;}`. libdom's is non-virtual and correctly
 * reports true for the node itself, per spec. */
static JSValue qjs_el_contains_data(JSContext *ctx,
		JSValueConst this_val, int argc, JSValueConst *argv,
		int magic, JSValueConst *func_data)
{
	dom_node *self;
	dom_node *other;
	int contains = 0;

	(void) this_val; (void) magic;
	self = qjs_get_node(func_data[0]);
	if (self == NULL || argc < 1) return JS_FALSE;
	other = qjs_get_node(argv[0]);
	if (other == NULL) return JS_FALSE;
	if (macsurf_dom_node_contains(self, other, &contains) != DOM_NO_ERR)
		return JS_FALSE;
	return contains ? JS_TRUE : JS_FALSE;
}

static JSValue qjs_el_get_children_data(JSContext *ctx,
		JSValueConst this_val, int argc, JSValueConst *argv,
		int magic, JSValueConst *func_data)
{
	dom_element *el;
	JSValue arr;
	dom_node *child = NULL, *next = NULL;
	dom_node_type ntype = 0;
	int count = 0;
	(void)this_val; (void)argc; (void)argv; (void)magic;
	el = (dom_element *)qjs_get_node(func_data[0]);
	arr = JS_NewArray(ctx);
	if (el == NULL) return arr;
	macsurf_dom_node_get_first_child((dom_node *)el, &child);
	while (child) {
		macsurf_dom_node_get_node_type(child, &ntype);
		if (ntype == 1) {
			JSValue w = qjs_wrap_element_full(ctx, (dom_element *)child);
			JS_SetPropertyUint32(ctx, arr, (unsigned int)count, w);
			count++;
			macsurf_dom_node_get_next_sibling(child, &next);
		} else {
			macsurf_dom_node_get_next_sibling(child, &next);
			macsurf_dom_node_unref(child);
		}
		child = next;
	}
	return arr;
}

/* ---- element.querySelectorAll (scoped) ---- */
static JSValue qjs_el_qsa_data(JSContext *ctx,
		JSValueConst this_val, int argc, JSValueConst *argv,
		int magic, JSValueConst *func_data)
{
	dom_element *el;
	const char *sel;
	char tag_lc[64];
	int i, count = 0;
	JSValue arr;
	(void)this_val; (void)magic;
	arr = JS_NewArray(ctx);
	el = (dom_element *)qjs_get_node(func_data[0]);
	if (el == NULL || argc < 1) return arr;
	sel = JS_ToCString(ctx, argv[0]);
	if (sel == NULL) return arr;
	for (i = 0; i < 63 && sel[i] && sel[i] != '[' && sel[i] != '.'
	     && sel[i] != ':' && sel[i] != ' '; i++) {
		char c = sel[i];
		tag_lc[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
	}
	tag_lc[i] = '\0';
	JS_FreeCString(ctx, sel);
	if (tag_lc[0] == '\0') return arr;
	qjs_collect_by_tag(ctx, (dom_node *)el, tag_lc, arr, &count);
	return arr;
}

/* ---- element.querySelector (scoped) ---- */
static JSValue qjs_el_qs_data(JSContext *ctx,
		JSValueConst this_val, int argc, JSValueConst *argv,
		int magic, JSValueConst *func_data)
{
	JSValue arr, result;
	uint32_t len = 0;
	arr = qjs_el_qsa_data(ctx, this_val, argc, argv, magic, func_data);
	JS_ToUint32(ctx, &len, JS_GetPropertyStr(ctx, arr, "length"));
	if (len > 0) {
		result = JS_GetPropertyUint32(ctx, arr, 0);
	} else {
		result = JS_NULL;
	}
	JS_FreeValue(ctx, arr);
	return result;
}

/* Install JS-side helpers on element object (classList, style proxy, misc) */
static void qjs_el_install_js_helpers(JSContext *ctx, JSValue obj)
{
	static const char *src =
		"(function(el){"
		/* classList */
		"(function(){"
		"var cl={"
		"contains:function(c){"
		"var v=el.getAttribute('class')||'';"
		"return(' '+v+' ').indexOf(' '+c+' ')>=0;},"
		"add:function(){"
		"var i;for(i=0;i<arguments.length;i++){"
		"var c=arguments[i];"
		"if(!this.contains(c)){"
		"var v=el.getAttribute('class')||'';"
		"el.setAttribute('class',(v?v+' ':'')+c);}}},"
		"remove:function(){"
		"var i;for(i=0;i<arguments.length;i++){"
		"var c=arguments[i];"
		"var v=el.getAttribute('class')||'';"
		"v=v.replace(new RegExp('(^|\\\\s)'+c+'(\\\\s|$)','g'),' ').trim();"
		"el.setAttribute('class',v);}},"
		"toggle:function(c,f){"
		"if(f===true){this.add(c);}else if(f===false){this.remove(c);}"
		"else if(this.contains(c)){this.remove(c);}else{this.add(c);}"
		"return this.contains(c);},"
		"replace:function(o,n){this.remove(o);this.add(n);},"
		"toString:function(){return el.getAttribute('class')||'';}"
		"};"
		"Object.defineProperty(el,'classList',{get:function(){return cl;},"
		"configurable:true});"
		"Object.defineProperty(el,'className',{"
		"get:function(){return el.getAttribute('class')||'';},"
		"set:function(v){el.setAttribute('class',v);},"
		"configurable:true});"
		"})();"
		/* id property */
		"Object.defineProperty(el,'id',{"
		"get:function(){return el.getAttribute('id')||'';},"
		"set:function(v){el.setAttribute('id',v);},"
		"configurable:true});"
		/* value property */
		"Object.defineProperty(el,'value',{"
		"get:function(){return el.getAttribute('value')||'';},"
		"set:function(v){el.setAttribute('value',v);},"
		"configurable:true});"
		/* fixes866 (#292) — reflect the rest of the common content
		 * attributes.  className/id/value above were the ONLY reflected
		 * properties, so `el.src = url` (and .href/.type/...) merely created a
		 * plain JS property on the wrapper and the DOM attribute was never
		 * set.  That is fatal for dynamically-injected scripts, which is how
		 * every modern site loads code:
		 *     const s = document.createElement('script');
		 *     s.src = url; document.body.appendChild(s);
		 * html_process_script() then does
		 *     dom_element_get_attribute(node, corestring_dom_src, &src);
		 *     if (src == NULL) exec_inline_script(...); else exec_src_script(...);
		 * so with no src attribute the injected script is run as an INLINE
		 * script with empty content -- it silently does nothing, no error.
		 * That is why hackaday's reply box never loads: its verbum loader
		 * fetches verbum-comments.js fine (fixes865, ok=1 status=200) and then
		 * injects it exactly this way.  Harness Test 11 caught it:
		 *   made=true isNative=true tag=script srcSet=  body=native appended=1
		 * -- every link works except the src.
		 * `value` is deliberately left as-is above: for form controls the
		 * property and the attribute legitimately diverge once the user types,
		 * so it is not a plain reflection and is not touched here. */
		"(function(){"
		"var _rp=['src','href','type','name','rel','target','alt','title',"
			"'placeholder','action','method','width','height','media'];"
		"var _i;for(_i=0;_i<_rp.length;_i++){(function(p){"
		"Object.defineProperty(el,p,{"
		"get:function(){return el.getAttribute(p)||'';},"
		"set:function(v){el.setAttribute(p,String(v));},"
		"configurable:true});"
		"})(_rp[_i]);}"
		"})();"
		/* name property */
		"Object.defineProperty(el,'name',{"
		"get:function(){return el.getAttribute('name')||'';},"
		"set:function(v){el.setAttribute('name',v);},"
		"configurable:true});"
		/* type property */
		"Object.defineProperty(el,'type',{"
		"get:function(){return el.getAttribute('type')||'';},"
		"set:function(v){el.setAttribute('type',v);},"
		"configurable:true});"
		/* innerHTML= (fixes846, #167 S3) — real HTML fragment parse via
		 * __setInnerHTML (dom_hubbub_fragment_parser_create), builds
		 * actual child elements instead of stripping all markup to text.
		 * Read side is still textContent-shaped (no serializer back to
		 * markup exists in this engine); good enough for the
		 * write-then-read-back-as-text patterns that exist, wrong for
		 * code that expects its own markup echoed back verbatim. */
		"Object.defineProperty(el,'innerHTML',{"
		"get:function(){return el.textContent||'';},"
		"set:function(v){"
		"if(typeof el.__setInnerHTML==='function')"
		"el.__setInnerHTML(String(v));"
		"else el.textContent=String(v).replace(/<[^>]*>/g,'');},"
		"configurable:true});"
		/* outerHTML stub */
		"Object.defineProperty(el,'outerHTML',{"
		"get:function(){return '<'+el.tagName+'>'+el.innerHTML+'</'+el.tagName+'>';},"
		"configurable:true});"
		/* dataset proxy */
		"(function(){"
		"var ds={};"
		"var p=new Proxy(ds,{"
		"get:function(t,k){"
		"if(typeof k==='string'){"
		"var attr='data-'+k.replace(/([A-Z])/g,'-$1').toLowerCase();"
		"return el.getAttribute(attr)||undefined;}"
		"return t[k];},"
		"set:function(t,k,v){"
		"if(typeof k==='string'){"
		"var attr='data-'+k.replace(/([A-Z])/g,'-$1').toLowerCase();"
		"el.setAttribute(attr,v);}"
		"t[k]=v;return true;}});"
		"try{Object.defineProperty(el,'dataset',{get:function(){return p;},"
		"configurable:true});}catch(e){"
		"el.dataset=ds;}"
		"})();"
		/* style proxy */
		"(function(){"
		"var sc={};"
		"function cc(n){return n.replace(/([A-Z])/g,'-$1').toLowerCase();}"
		"function gu(n){return n.replace(/-([a-z])/g,function(m,c){return c.toUpperCase();});}"
		"var sp={setProperty:function(p,v,pri){"
		"var s=el.getAttribute('style')||'';"
		"var re=new RegExp('(?:^|;)\\\\s*'+cc(p)+'\\\\s*:[^;]*','g');"
		"s=s.replace(re,'').replace(/;+/g,';').replace(/^;|;$/g,'').trim();"
		"if(v!==null&&v!=='')s=s+(s?';':'')+cc(p)+':'+v;"
		"el.setAttribute('style',s);sc[p]=v||'';},"
		"removeProperty:function(p){this.setProperty(p,'');},"
		"getPropertyValue:function(p){return sc[gu(p)]||sc[p]||'';},"
		"get cssText(){return el.getAttribute('style')||'';},"
		"set cssText(v){el.setAttribute('style',v);}};"
		"var PS=['display','visibility','opacity','position','overflow',"
		"'overflowX','overflowY','width','height','minWidth','maxWidth',"
		"'minHeight','maxHeight','top','left','right','bottom','margin',"
		"'marginTop','marginRight','marginBottom','marginLeft','padding',"
		"'paddingTop','paddingRight','paddingBottom','paddingLeft',"
		"'border','borderTop','borderRight','borderBottom','borderLeft',"
		"'borderRadius','boxSizing','boxShadow','outline','outlineOffset',"
		"'resize','backgroundColor','background','color','fontFamily',"
		"'fontSize','fontWeight','fontStyle','lineHeight','textAlign',"
		"'textDecoration','textOverflow','whiteSpace','verticalAlign',"
		"'cursor','pointerEvents','userSelect','zIndex','flex','flexDirection',"
		"'flexWrap','alignItems','justifyContent','transform','transition',"
		"'animation','content','listStyle','tableLayout','borderCollapse',"
		"'borderSpacing','captionSide','emptyCells','direction','float',"
		"'clear','columns'];"
		"var i;for(i=0;i<PS.length;i++)(function(p){"
		"Object.defineProperty(sp,p,{"
		"get:function(){return sc[p]||'';},"
		"set:function(v){sc[p]=v;sp.setProperty(p,v);},"
		"configurable:true,enumerable:true});})(PS[i]);"
		"Object.defineProperty(el,'style',{get:function(){return sp;},"
		"configurable:true});"
		"})();"
		/* matches — tag, #id, .class, [attr], compound */
		"el.matches=function(sel){"
		"if(!sel||!sel.trim)return false;"
		"sel=sel.trim();"
		"var parts=sel.split(',');"
		"var i;for(i=0;i<parts.length;i++){"
		"var s=parts[i].trim();"
		"var ok=true;"
		"var rest=s;"
		"var tagM=rest.match(/^([a-zA-Z][a-zA-Z0-9]*)/);"
		"if(tagM){if(el.tagName!==tagM[1].toLowerCase())ok=false;"
		"rest=rest.substr(tagM[1].length);}"
		"var re=/([#.:]|\\[)[^#.:\\[\\]]*(\\])?/g;"
		"var m;while(ok&&(m=re.exec(rest))){"
		"var t=m[0];"
		"if(t.charAt(0)==='#'){if(el.getAttribute('id')!==t.substr(1))ok=false;}"
		"else if(t.charAt(0)==='.'){var v=el.getAttribute('class')||'';"
		"if((' '+v+' ').indexOf(' '+t.substr(1)+' ')<0)ok=false;}"
		"else if(t.charAt(0)==='['){"
		"var inner=t.slice(1,-1);"
		"var eqI=inner.indexOf('=');"
		"if(eqI<0){if(!el.hasAttribute(inner.replace(/\\s/g,'')))ok=false;}"
		"else{"
		"var op=inner.charAt(eqI-1);"
		"var an,av,ev;"
		"if(op==='*'||op==='~'||op==='|'||op==='^'||op==='$'){"
		"an=inner.substr(0,eqI-1).trim();}else{an=inner.substr(0,eqI).trim();op='=';}"
		"av=inner.substr(eqI+1).trim().replace(/^['\"]|['\"]$/g,'');"
		"ev=el.getAttribute(an)||'';"
		"if(op==='='){if(ev!==av)ok=false;}"
		"else if(op==='*'){if(ev.indexOf(av)<0)ok=false;}"
		"else if(op==='~'){if((' '+ev+' ').indexOf(' '+av+' ')<0)ok=false;}"
		"else if(op==='^'){if(ev.indexOf(av)!==0)ok=false;}"
		"else if(op==='$'){if(ev.lastIndexOf(av)!==ev.length-av.length)ok=false;}"
		"}}}"
		"if(ok)return true;}"
		"return false;};"
		/* closest — walk parentNode chain */
		"el.closest=function(sel){"
		"var n=el;"
		"while(n&&n.matches){if(n.matches(sel))return n;n=n.parentNode;}"
		"return null;};"
		/* event handling */
		"el.addEventListener=function(t,fn,opts){"
		"if(!el._L)el._L={};"
		"if(!el._L[t])el._L[t]=[];"
		"el._L[t].push(fn);};"
		"el.removeEventListener=function(t,fn){"
		"if(!el._L||!el._L[t])return;"
		"var a=el._L[t];"
		"var i;for(i=0;i<a.length;i++){if(a[i]===fn){a.splice(i,1);return;}}};"
		/* fixes872 (#300) — fire the addEventListener list (_L) AND the on*
		 * handler (_H, set through the prototype accessors) exactly once each.
		 * Both routes are real and pages use both; dispatchEvent firing only _L
		 * is why js_fire_script_load had to call el['on'+type] separately. */
		"el.dispatchEvent=function(ev){"
		"var t=ev&&ev.type||'';"
		"if(el._L&&el._L[t]){"
		"var a=el._L[t].slice();"
		"var i;for(i=0;i<a.length;i++){try{a[i].call(el,ev);}catch(e){}}}"
		"if(el._H&&el._H[t]){try{el._H[t].call(el,ev);}catch(e){}}"
		"return true;};"
		/* misc */
		"el.getBoundingClientRect=function(){"
		"return{top:0,left:0,right:0,bottom:0,width:0,height:0,x:0,y:0};};"
		"el.getClientRects=function(){return[this.getBoundingClientRect()];};"
		"el.scrollIntoView=function(){};"
		"el.scrollIntoViewIfNeeded=function(){};"
		"el.focus=function(){};"
		"el.blur=function(){};"
		"el.click=function(){};"
		/* fixes878 — the node-oriented traversal surface used to be hardcoded
		 * HERE, as five lines of constants:
		 *     el.cloneNode=function(){return el;};
		 *     el.contains=function(n){return false;};
		 *     el.childNodes=[];
		 *     el.firstChild=null;el.lastChild=null;
		 *     el.nextSibling=null;el.previousSibling=null;
		 * They were not shadowing real natives -- no native of any of those
		 * names existed, so these WERE the implementation, and every one of
		 * them was a wrong answer rather than a missing one. cloneNode handing
		 * back `el` itself was the worst: it made the universal
		 * clone-and-append idiom MOVE the original.
		 *
		 * cloneNode/contains are now real C natives installed by
		 * qjs_el_install_native_attrs (which calls this function, so anything
		 * defined here would overwrite them -- hence their removal, not just
		 * their replacement). firstChild/lastChild/nextSibling/previousSibling/
		 * childNodes are live accessors installed in the tc_src block, which
		 * runs AFTER this one and so has the last word. */
		"})";
	JSValue fn, args[1];
	fn = JS_Eval(ctx, src, strlen(src), "<el-helpers>", JS_EVAL_TYPE_GLOBAL);
	if (!JS_IsException(fn)) {
		args[0] = JS_DupValue(ctx, obj);
		JS_Call(ctx, fn, JS_UNDEFINED, 1, args);
		JS_FreeValue(ctx, args[0]);
	} else {
		JSValue ex = JS_GetException(ctx);
		const char *msg = JS_ToCString(ctx, ex);
		macsurf_debug_log_writef("qjs el-helpers eval error: %s", msg ? msg : "?");
		if (msg) JS_FreeCString(ctx, msg);
		JS_FreeValue(ctx, ex);
	}
	JS_FreeValue(ctx, fn);
}

/* ---- fixes878: the node-level traversal surface, for EVERY node wrapper ----
 *
 * Installed on elements, text/comment nodes AND fragments. That breadth is the
 * point: `box.firstChild.nextSibling` walks THROUGH a text node, so if only
 * elements carry the surface the chain dies at the first gap between tags --
 * which is most real markup, and was the first thing Test 21 caught.
 *
 * SAFE ON ALL THREE SHAPES because every function here goes through the base
 * dom_node vtable (get_first_child / get_next_sibling / clone_node / contains),
 * which element, text and fragment all implement. This is the same rule
 * fixes846 arrived at the hard way: qjs_wrap_element reads through the ELEMENT
 * vtable (dom_element_get_tag_name), and reusing it for a fragment -- a
 * different, smaller shape -- was an ASan global-buffer-overflow. Nothing in
 * this function may touch an element-only operation.
 *
 * `data[0]` holds one ref that JS_NewCFunctionData dups per closure, so it is
 * released once at the end, mirroring qjs_el_install_native_attrs. */
static void qjs_install_node_traversal(JSContext *ctx, JSValue obj)
{
	JSValue data[1];
	JSValue f;
	static const char *nav_src =
		"(function(el){"
		"Object.defineProperty(el,'firstChild',{"
		"get:function(){return el.__getFirstChild();},configurable:true});"
		"Object.defineProperty(el,'lastChild',{"
		"get:function(){return el.__getLastChild();},configurable:true});"
		"Object.defineProperty(el,'nextSibling',{"
		"get:function(){return el.__getNextSibling();},configurable:true});"
		"Object.defineProperty(el,'previousSibling',{"
		"get:function(){return el.__getPreviousSibling();},configurable:true});"
		/* snapshot array, not a live NodeList -- see qjs_el_get_child_nodes_data */
		"Object.defineProperty(el,'childNodes',{"
		"get:function(){return el.__getChildNodes();},configurable:true});"
		"el.hasChildNodes=function(){return el.__getFirstChild()!==null;};"
		"})";
	JSValue fn, args[1];

	data[0] = JS_DupValue(ctx, obj);

	f = JS_NewCFunctionData(ctx, qjs_el_get_edge_data, 0, 0 /*firstChild*/, 1, data);
	JS_SetPropertyStr(ctx, obj, "__getFirstChild", f);
	f = JS_NewCFunctionData(ctx, qjs_el_get_edge_data, 0, 1 /*lastChild*/, 1, data);
	JS_SetPropertyStr(ctx, obj, "__getLastChild", f);
	f = JS_NewCFunctionData(ctx, qjs_el_get_edge_data, 0, 2 /*nextSibling*/, 1, data);
	JS_SetPropertyStr(ctx, obj, "__getNextSibling", f);
	f = JS_NewCFunctionData(ctx, qjs_el_get_edge_data, 0, 3 /*prevSibling*/, 1, data);
	JS_SetPropertyStr(ctx, obj, "__getPreviousSibling", f);
	f = JS_NewCFunctionData(ctx, qjs_el_get_child_nodes_data, 0, 0, 1, data);
	JS_SetPropertyStr(ctx, obj, "__getChildNodes", f);
	f = JS_NewCFunctionData(ctx, qjs_el_clone_node_data, 1, 0, 1, data);
	JS_SetPropertyStr(ctx, obj, "cloneNode", f);
	f = JS_NewCFunctionData(ctx, qjs_el_contains_data, 1, 0, 1, data);
	JS_SetPropertyStr(ctx, obj, "contains", f);

	JS_FreeValue(ctx, data[0]);

	fn = JS_Eval(ctx, nav_src, strlen(nav_src), "<node-nav>", JS_EVAL_TYPE_GLOBAL);
	if (!JS_IsException(fn)) {
		args[0] = JS_DupValue(ctx, obj);
		JS_Call(ctx, fn, JS_UNDEFINED, 1, args);
		JS_FreeValue(ctx, args[0]);
	}
	JS_FreeValue(ctx, fn);
}

/* Install all native C functions and JS helpers on an element object */
static void qjs_el_install_native_attrs(JSContext *ctx, JSValue obj)
{
	JSValue data[1];
	JSValue f;
	data[0] = JS_DupValue(ctx, obj);

	/* Core DOM methods */
	f = JS_NewCFunctionData(ctx, qjs_el_getAttribute_data, 1, 0, 1, data);
	JS_SetPropertyStr(ctx, obj, "getAttribute", f);
	f = JS_NewCFunctionData(ctx, qjs_el_setAttribute_data, 2, 0, 1, data);
	JS_SetPropertyStr(ctx, obj, "setAttribute", f);
	f = JS_NewCFunctionData(ctx, qjs_el_remove_attribute_data, 1, 0, 1, data);
	JS_SetPropertyStr(ctx, obj, "removeAttribute", f);
	f = JS_NewCFunctionData(ctx, qjs_el_has_attribute_data, 1, 0, 1, data);
	JS_SetPropertyStr(ctx, obj, "hasAttribute", f);

	/* textContent */
	f = JS_NewCFunctionData(ctx, qjs_el_get_text_content_data, 0, 0, 1, data);
	JS_SetPropertyStr(ctx, obj, "__getTextContent", f);
	f = JS_NewCFunctionData(ctx, qjs_el_set_text_content_data, 1, 0, 1, data);
	JS_SetPropertyStr(ctx, obj, "__setTextContent", f);
	/* fixes846 (#167 S3) — real innerHTML= via HTML fragment parsing. */
	f = JS_NewCFunctionData(ctx, qjs_el_set_inner_html_data, 1, 0, 1, data);
	JS_SetPropertyStr(ctx, obj, "__setInnerHTML", f);

	/* Traversal */
	f = JS_NewCFunctionData(ctx, qjs_el_get_parent_node_data, 0, 0, 1, data);
	JS_SetPropertyStr(ctx, obj, "__getParentNode", f);
	f = JS_NewCFunctionData(ctx, qjs_el_get_next_sibling_data, 0, 0, 1, data);
	JS_SetPropertyStr(ctx, obj, "__getNextElementSibling", f);
	f = JS_NewCFunctionData(ctx, qjs_el_get_prev_sibling_data, 0, 0, 1, data);
	JS_SetPropertyStr(ctx, obj, "__getPreviousElementSibling", f);
	f = JS_NewCFunctionData(ctx, qjs_el_get_children_data, 0, 0, 1, data);
	JS_SetPropertyStr(ctx, obj, "__getChildren", f);

	/* Mutation */
	f = JS_NewCFunctionData(ctx, qjs_el_append_child_data, 1, 0, 1, data);
	JS_SetPropertyStr(ctx, obj, "appendChild", f);
	f = JS_NewCFunctionData(ctx, qjs_el_remove_child_data, 1, 0, 1, data);
	JS_SetPropertyStr(ctx, obj, "removeChild", f);
	f = JS_NewCFunctionData(ctx, qjs_el_insert_before_data, 2, 0, 1, data);
	JS_SetPropertyStr(ctx, obj, "insertBefore", f);

	/* Scoped query */
	f = JS_NewCFunctionData(ctx, qjs_el_qsa_data, 1, 0, 1, data);
	JS_SetPropertyStr(ctx, obj, "querySelectorAll", f);
	f = JS_NewCFunctionData(ctx, qjs_el_qs_data, 1, 0, 1, data);
	JS_SetPropertyStr(ctx, obj, "querySelector", f);

	JS_FreeValue(ctx, data[0]);

	/* JS-side helpers: classList, style, dataset, matches, closest, etc. */
	qjs_el_install_js_helpers(ctx, obj);

	/* Wire textContent as a property using the C getter/setter helpers */
	{
		static const char *tc_src =
			"(function(el){"
			"Object.defineProperty(el,'textContent',{"
			"get:function(){return el.__getTextContent();},"
			"set:function(v){el.__setTextContent(String(v));},"
			"configurable:true});"
			"Object.defineProperty(el,'parentNode',{"
			"get:function(){return el.__getParentNode();},"
			"configurable:true});"
			"Object.defineProperty(el,'nextElementSibling',{"
			"get:function(){return el.__getNextElementSibling();},"
			"configurable:true});"
			"Object.defineProperty(el,'previousElementSibling',{"
			"get:function(){return el.__getPreviousElementSibling();},"
			"configurable:true});"
			"Object.defineProperty(el,'children',{"
			"get:function(){return el.__getChildren();},"
			"configurable:true});"
			"})";
		JSValue fn2, args2[1];
		fn2 = JS_Eval(ctx, tc_src, strlen(tc_src), "<el-props>",
			JS_EVAL_TYPE_GLOBAL);
		if (!JS_IsException(fn2)) {
			args2[0] = JS_DupValue(ctx, obj);
			JS_Call(ctx, fn2, JS_UNDEFINED, 1, args2);
			JS_FreeValue(ctx, args2[0]);
		}
		JS_FreeValue(ctx, fn2);
	}

	/* fixes878 — last, so nothing above can clobber it. */
	qjs_install_node_traversal(ctx, obj);
}

/* Full wrap: identical to qjs_wrap_element now — method install is folded into
 * wrap's miss path so it runs exactly once per node and cache hits skip it.
 * Kept as a named entry point for the traversal/mutation call sites. */
static JSValue qjs_wrap_element_full(JSContext *ctx, dom_element *el)
{
	return qjs_wrap_element(ctx, el);
}

/* ---- real text-node wrapper (fixes846, #167 S3) ----
 * document.createTextNode() previously returned a fake plain JS object with
 * no native opaque tag, so parent.appendChild(textNode) silently no-opped
 * (qjs_get_node() returned NULL for it, per the census). qjs_get_node() /
 * the wrap table / the finalizer are generic over ANY dom_node* stored
 * under s_el_class_id, so they're reused verbatim here -- what must NOT be
 * reused is qjs_wrap_element's property-install body, which bakes in
 * element-only semantics (tagName read, hardcoded nodeType=1, the full
 * attribute API). A text node gets its own minimal surface instead:
 * nodeType=3, nodeName='#text', and nodeValue/data/textContent backed by
 * the real characterdata (NOT a children walk -- a text node's data IS its
 * content, it has no children). */
static JSValue qjs_text_get_data_data(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv, int magic, JSValueConst *func_data)
{
	dom_node *n;
	dom_string *ds = NULL;
	const char *s = "";
	JSValue ret;
	(void) this_val; (void) argc; (void) argv; (void) magic;
	n = qjs_get_node(func_data[0]);
	if (n == NULL) return JS_NewString(ctx, "");
	if (macsurf_dom_characterdata_get_data(n, &ds) == DOM_NO_ERR
	    && ds != NULL) {
		s = dom_string_data(ds);
	}
	ret = JS_NewString(ctx, s);
	if (ds != NULL) macsurf_dom_string_unref(ds);
	return ret;
}

static JSValue qjs_text_set_data_data(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv, int magic, JSValueConst *func_data)
{
	dom_node *n;
	const char *v;
	(void) this_val; (void) magic;
	n = qjs_get_node(func_data[0]);
	if (n == NULL || argc < 1) return JS_UNDEFINED;
	v = JS_ToCString(ctx, argv[0]);
	if (v == NULL) return JS_UNDEFINED;
	macsurf_dom_characterdata_set_data_s(n, v);
	JS_FreeCString(ctx, v);
	if (g_qjs_content) macos9_js_mark_dom_dirty(g_qjs_content);
	return JS_UNDEFINED;
}

static JSValue qjs_text_append_child_noop(JSContext *ctx,
		JSValueConst this_val, int argc, JSValueConst *argv)
{
	(void) ctx; (void) this_val; (void) argc; (void) argv;
	return JS_NULL;
}

/* fixes878 — qjs_text_clone_node_data is GONE. It was
 *     return JS_DupValue(ctx, func_data[0]);
 * i.e. it handed back the SAME text node, exactly the self-returning bug the
 * element-side cloneNode had; a text node "cloned" into a second parent was
 * really moved out of the first. Both now use the real libdom virtual clone via
 * qjs_el_clone_node_data, installed by qjs_install_node_traversal. */
static JSValue qjs_wrap_text_node(JSContext *ctx, dom_text *tn)
{
	dom_node *node = (dom_node *) tn;
	struct qjs_wrap_entry *hit;
	dom_node *owner_doc;
	JSValue obj;
	JSValue data[1];
	JSValue f;

	if (tn == NULL) return JS_NULL;

	hit = qjs_wrap_lookup(node);
	if (hit != NULL) {
		macsurf_dom_node_unref(node);
		return JS_DupValue(ctx, hit->val);
	}

	obj = JS_NewObjectClass(ctx, (int) s_el_class_id);
	if (JS_IsException(obj)) {
		macsurf_dom_node_unref(node);
		return JS_NULL;
	}
	JS_SetOpaque(obj, tn);

	owner_doc = (dom_node *) g_qjs_document;
	if (owner_doc) macsurf_dom_node_ref(owner_doc);

	if (qjs_wrap_insert(node, owner_doc, obj) == 0) {
		JS_SetOpaque(obj, NULL);
		macsurf_dom_node_unref(node);
		if (owner_doc) macsurf_dom_node_unref(owner_doc);
		return obj;
	}

	/* fixes878 — report the node's REAL type instead of hardcoding #text.
	 * This wrapper is now also the landing place for the other CharacterData
	 * types reachable through firstChild/nextSibling (comment = 8,
	 * CDATASection = 4), which share CharacterData's data/nodeValue surface and
	 * so are safe here -- but calling a comment a "#text" of nodeType 3 would
	 * be a wrong answer, and comment nodes are load-bearing markers for Preact
	 * and React. createTextNode still lands on 3/#text exactly as before. */
	{
		dom_node_type wt = 0;
		const char *wname = "#text";
		macsurf_dom_node_get_node_type(node, &wt);
		if (wt == 0) wt = 3;
		if (wt == 8) wname = "#comment";
		else if (wt == 4) wname = "#cdata-section";
		JS_SetPropertyStr(ctx, obj, "nodeType", JS_NewInt32(ctx, (int) wt));
		JS_SetPropertyStr(ctx, obj, "nodeName", JS_NewString(ctx, wname));
	}
	JS_SetPropertyStr(ctx, obj, "__ptr",
		JS_NewInt64(ctx, (long long) (size_t) tn));

	/* fixes846 perf — nodeValue/data/textContent/parentNode are wired as
	 * REAL C getter/setter pairs (JS_DefinePropertyGetSet), not an
	 * Object.defineProperty(...) block run through JS_Eval on every
	 * single wrap. A reconciler-heavy page (React) calls createTextNode
	 * per leaf text update; re-lexing/parsing a JS source string on every
	 * one of those calls is a real, avoidable per-node cost that the
	 * element-wrapper path (qjs_el_install_native_attrs) also pays today
	 * -- fixed here for the new text-node path since it's freshly
	 * written; that pre-existing element-side cost is unchanged by this
	 * fix and is a good target for its own round if profiling confirms
	 * it matters. */
	data[0] = JS_DupValue(ctx, obj);
	{
		JSAtom atom;
		JSValue getter, setter;

		getter = JS_NewCFunctionData(ctx, qjs_text_get_data_data,
				0, 0, 1, data);
		setter = JS_NewCFunctionData(ctx, qjs_text_set_data_data,
				1, 0, 1, data);

		atom = JS_NewAtom(ctx, "nodeValue");
		JS_DefinePropertyGetSet(ctx, obj, atom,
				JS_DupValue(ctx, getter), JS_DupValue(ctx, setter),
				JS_PROP_CONFIGURABLE);
		JS_FreeAtom(ctx, atom);

		atom = JS_NewAtom(ctx, "data");
		JS_DefinePropertyGetSet(ctx, obj, atom,
				JS_DupValue(ctx, getter), JS_DupValue(ctx, setter),
				JS_PROP_CONFIGURABLE);
		JS_FreeAtom(ctx, atom);

		atom = JS_NewAtom(ctx, "textContent");
		JS_DefinePropertyGetSet(ctx, obj, atom, getter, setter,
				JS_PROP_CONFIGURABLE);
		JS_FreeAtom(ctx, atom);
	}
	{
		JSAtom atom = JS_NewAtom(ctx, "parentNode");
		JSValue getter = JS_NewCFunctionData(ctx,
				qjs_el_get_parent_node_data, 0, 0, 1, data);
		JS_DefinePropertyGetSet(ctx, obj, atom, getter, JS_UNDEFINED,
				JS_PROP_CONFIGURABLE);
		JS_FreeAtom(ctx, atom);
	}
	JS_SetPropertyStr(ctx, obj, "appendChild",
		JS_NewCFunction(ctx, qjs_text_append_child_noop,
				"appendChild", 1));
	JS_FreeValue(ctx, data[0]);

	/* fixes878 — text/comment nodes get the SAME node-level traversal surface
	 * as elements. This is not optional decoration: firstChild lands on the
	 * text between tags, so `box.firstChild.nextSibling` walks THROUGH a text
	 * node. Without it that chain dies at the first gap in real markup -- the
	 * first thing Test 21 caught. Base dom_node vtable ops only, so it is safe
	 * on this shape (see the qjs_install_node_traversal note). */
	qjs_install_node_traversal(ctx, obj);

	return obj;
}

/* ---- document.createTextNode (native) ---- */
static JSValue qjs_create_text_node(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv)
{
	const char *text;
	dom_text *tn = NULL;
	JSValue obj;

	(void) this_val;
	if (g_qjs_document == NULL || argc < 1) return JS_NULL;
	text = JS_ToCString(ctx, argv[0]);
	if (text == NULL) return JS_NULL;
	if (macsurf_dom_document_create_text_node_s(g_qjs_document, text, &tn)
	    != DOM_NO_ERR || tn == NULL) {
		JS_FreeCString(ctx, text);
		return JS_NULL;
	}
	JS_FreeCString(ctx, text);
	obj = qjs_wrap_text_node(ctx, tn);
	return obj;
}

/* ---- real document-fragment wrapper (fixes846, #167 S3) ----
 * A DocumentFragment needs appendChild/removeChild/insertBefore (dom_node_
 * append_child on a fragment target unwraps its children per DOM spec when
 * the fragment is later appended elsewhere), so it's tempting to just reuse
 * qjs_wrap_element wholesale -- DON'T: the S0 harness caught this as a real
 * global-buffer-overflow (ASan, ELEMENT_get_tag_name read through a
 * document_fragment's SMALLER vtable, dom_element_get_tag_name expects the
 * element vtable shape and a fragment's df_vtable doesn't have it -- this
 * is exactly the "accidental safety" the design research flagged as
 * unverified, and it turned out not to be safe at all). Same fix pattern
 * as qjs_wrap_text_node: reuse the class id / wrap-table / finalizer
 * (generic over any dom_node*), but only install the operations that are
 * genuinely defined on the BASE dom_node_vtable (append/remove/insertBefore/
 * parentNode/textContent/children -- confirmed via dom/core/node.h, every
 * node subtype's vtable starts with that base) -- never the element-only
 * ones (getAttribute et al, tagName) that live on a DIFFERENT, narrower
 * vtable a fragment doesn't have. */
static JSValue qjs_wrap_fragment(JSContext *ctx, dom_document_fragment *frag)
{
	dom_node *node = (dom_node *) frag;
	struct qjs_wrap_entry *hit;
	dom_node *owner_doc;
	JSValue obj;
	JSValue data[1];
	JSValue f;

	if (frag == NULL) return JS_NULL;

	hit = qjs_wrap_lookup(node);
	if (hit != NULL) {
		macsurf_dom_node_unref(node);
		return JS_DupValue(ctx, hit->val);
	}

	obj = JS_NewObjectClass(ctx, (int) s_el_class_id);
	if (JS_IsException(obj)) {
		macsurf_dom_node_unref(node);
		return JS_NULL;
	}
	JS_SetOpaque(obj, frag);

	owner_doc = (dom_node *) g_qjs_document;
	if (owner_doc) macsurf_dom_node_ref(owner_doc);

	if (qjs_wrap_insert(node, owner_doc, obj) == 0) {
		JS_SetOpaque(obj, NULL);
		macsurf_dom_node_unref(node);
		if (owner_doc) macsurf_dom_node_unref(owner_doc);
		return obj;
	}

	JS_SetPropertyStr(ctx, obj, "nodeType", JS_NewInt32(ctx, 11));
	JS_SetPropertyStr(ctx, obj, "nodeName",
			JS_NewString(ctx, "#document-fragment"));
	JS_SetPropertyStr(ctx, obj, "__ptr",
			JS_NewInt64(ctx, (long long) (size_t) frag));

	data[0] = JS_DupValue(ctx, obj);
	f = JS_NewCFunctionData(ctx, qjs_el_append_child_data, 1, 0, 1, data);
	JS_SetPropertyStr(ctx, obj, "appendChild", f);
	f = JS_NewCFunctionData(ctx, qjs_el_remove_child_data, 1, 0, 1, data);
	JS_SetPropertyStr(ctx, obj, "removeChild", f);
	f = JS_NewCFunctionData(ctx, qjs_el_insert_before_data, 2, 0, 1, data);
	JS_SetPropertyStr(ctx, obj, "insertBefore", f);
	f = JS_NewCFunctionData(ctx, qjs_el_get_children_data, 0, 0, 1, data);
	JS_SetPropertyStr(ctx, obj, "__getChildren", f);
	f = JS_NewCFunctionData(ctx, qjs_el_get_text_content_data, 0, 0, 1, data);
	JS_SetPropertyStr(ctx, obj, "__getTextContent", f);
	f = JS_NewCFunctionData(ctx, qjs_el_set_text_content_data, 1, 0, 1, data);
	JS_SetPropertyStr(ctx, obj, "__setTextContent", f);
	f = JS_NewCFunctionData(ctx, qjs_el_get_parent_node_data, 0, 0, 1, data);
	JS_SetPropertyStr(ctx, obj, "__getParentNode", f);
	JS_FreeValue(ctx, data[0]);

	{
		JSAtom atom;
		JSValue getter;

		atom = JS_NewAtom(ctx, "children");
		getter = JS_GetPropertyStr(ctx, obj, "__getChildren");
		JS_DefinePropertyGetSet(ctx, obj, atom, getter, JS_UNDEFINED,
				JS_PROP_CONFIGURABLE);
		JS_FreeAtom(ctx, atom);

		atom = JS_NewAtom(ctx, "parentNode");
		getter = JS_GetPropertyStr(ctx, obj, "__getParentNode");
		JS_DefinePropertyGetSet(ctx, obj, atom, getter, JS_UNDEFINED,
				JS_PROP_CONFIGURABLE);
		JS_FreeAtom(ctx, atom);

		atom = JS_NewAtom(ctx, "textContent");
		JS_DefinePropertyGetSet(ctx, obj, atom,
				JS_GetPropertyStr(ctx, obj, "__getTextContent"),
				JS_GetPropertyStr(ctx, obj, "__setTextContent"),
				JS_PROP_CONFIGURABLE);
		JS_FreeAtom(ctx, atom);
	}

	/* fixes878 — fragments need the node surface too: the whole point of a
	 * DocumentFragment is to build a subtree and then walk or move its
	 * children. Base dom_node vtable ops only, which is exactly why this is
	 * safe on the fragment's smaller shape (fixes846). */
	qjs_install_node_traversal(ctx, obj);

	return obj;
}

/* ---- fixes878: wrap a node of ANY type ----
 *
 * The node-oriented traversal surface (firstChild / lastChild / nextSibling /
 * previousSibling / childNodes / cloneNode) reaches nodes that are NOT
 * elements -- text between tags, and the comment markers Preact and React
 * depend on. `children` and nextElementSibling can filter to elements and use
 * qjs_wrap_element_full directly; these cannot.
 *
 * Dispatch on the real nodeType and use the wrapper built for that shape.
 * Getting this wrong is not theoretical: fixes846 hit an ASan
 * global-buffer-overflow by reusing the ELEMENT wrapper for a DocumentFragment,
 * whose vtable is a different, smaller shape -- qjs_wrap_element reads through
 * the element vtable (dom_element_get_tag_name), which a fragment does not
 * have. Unknown types get NULL rather than a guessed wrapper, for the same
 * reason.
 *
 * REF CONTRACT: takes the caller's transferred ref (libdom's get_* / clone
 * return a ref'd node) and hands it to the chosen wrapper, every one of which
 * adopts it -- on a wrap-map hit they unref it and return a dup, so identity
 * holds. The unknown-type path must therefore unref, or the node leaks. */
static JSValue qjs_wrap_any_node(JSContext *ctx, dom_node *node)
{
	dom_node_type ntype = 0;

	if (node == NULL) return JS_NULL;
	if (macsurf_dom_node_get_node_type(node, &ntype) != DOM_NO_ERR) {
		macsurf_dom_node_unref(node);
		return JS_NULL;
	}

	switch ((int) ntype) {
	case 1:		/* element */
		return qjs_wrap_element_full(ctx, (dom_element *) node);
	case 3:		/* text */
	case 4:		/* CDATASection  */
	case 8:		/* comment -- CharacterData, same data/nodeValue surface */
		return qjs_wrap_text_node(ctx, (dom_text *) node);
	case 11:	/* DocumentFragment */
		return qjs_wrap_fragment(ctx, (dom_document_fragment *) node);
	default:
		/* document (9), doctype (10), attr (2), PI (7): no wrapper of the
		 * right shape exists, and guessing one is the fixes846 crash. */
		macsurf_dom_node_unref(node);
		return JS_NULL;
	}
}

/* ---- document.createDocumentFragment (native) ----
 * Replaces the old JS-only mkfb('#fragment') fake (qjs_dom_install) whose
 * children were plain JS objects invisible to qjs_get_node(), so appending
 * it to a real element silently dropped every child -- see the S1 census
 * and the removed no-op override in register_browser_globals. */
static JSValue qjs_create_document_fragment(JSContext *ctx,
		JSValueConst this_val, int argc, JSValueConst *argv)
{
	dom_document_fragment *frag = NULL;

	(void) this_val; (void) argc; (void) argv;
	if (g_qjs_document == NULL) return JS_NULL;
	if (macsurf_dom_document_create_document_fragment(g_qjs_document, &frag)
	    != DOM_NO_ERR || frag == NULL) {
		return JS_NULL;
	}
	return qjs_wrap_fragment(ctx, frag);
}

/* ---- DOM tree walker: collect all elements matching tag name ---- */
static void qjs_collect_by_tag(JSContext *ctx, dom_node *node,
		const char *tag_lc, JSValue arr, int *count)
{
	dom_node *child = NULL;
	dom_node *next  = NULL;
	dom_node_type ntype = 0;

	if (node == NULL) return;
	macsurf_dom_node_get_node_type(node, &ntype);
	if (ntype == 1) { /* ELEMENT_NODE */
		dom_string *tname = NULL;
		if (macsurf_dom_element_get_tag_name((dom_element *)node, &tname)
		    == DOM_NO_ERR && tname != NULL) {
			const char *ts = dom_string_data(tname);
			char lc[32];
			int i;
			for (i = 0; i < 31 && ts[i]; i++) {
				char c = ts[i];
				lc[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
			}
			lc[i] = '\0';
			macsurf_dom_string_unref(tname);
			if (strcmp(tag_lc, "*") == 0 || strcmp(lc, tag_lc) == 0) {
				dom_element *el = (dom_element *)node;
				JSValue wrapper;
				macsurf_dom_node_ref(node);
				wrapper = qjs_wrap_element(ctx, el);
				JS_SetPropertyUint32(ctx, arr, (unsigned int)*count, wrapper);
				(*count)++;
			}
		}
	}
	/* Recurse into children */
	macsurf_dom_node_get_first_child(node, &child);
	while (child) {
		qjs_collect_by_tag(ctx, child, tag_lc, arr, count);
		macsurf_dom_node_get_next_sibling(child, &next);
		macsurf_dom_node_unref(child);
		child = next;
	}
}

/* ---- document.getElementById ---- */
static JSValue qjs_getElementById(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv)
{
	const char *id_cstr;
	dom_string *id_ds;
	dom_element *el = NULL;

	(void)this_val;
	if (g_qjs_document == NULL || argc < 1) return JS_NULL;
	id_cstr = JS_ToCString(ctx, argv[0]);
	if (id_cstr == NULL) return JS_NULL;
	id_ds = qjs_make_domstr(id_cstr);
	JS_FreeCString(ctx, id_cstr);
	if (id_ds == NULL) return JS_NULL;
	macsurf_dom_document_get_element_by_id(g_qjs_document, id_ds, &el);
	macsurf_dom_string_unref(id_ds);
	if (el == NULL) return JS_NULL;
	return qjs_wrap_element(ctx, el);
}

/* ---- document.querySelectorAll (tag-name only for now) ---- */
/* ==== fixes871 (#298) — compound selector matcher ==========================
 *
 * Before this, the selector code extracted the TAG and nothing else, then called
 * qjs_collect_by_tag(). So `div.foo` matched EVERY div and `.foo` alone matched
 * nothing at all ("class-only sel unsupported"). The comment above the parse
 * loop claimed "support bare tag, tag[attr*=val], tag.class, .class" -- none of
 * that was true beyond the bare tag.
 *
 * A class-only selector is not a nice-to-have here: Preact's Verbum mount is
 *     document.querySelectorAll(".comment-form__verbum").forEach(...)
 * which yielded 0 iterations, so the comment form never rendered even with a
 * working loader and a working element factory.
 *
 * Grepping the whole 86,970 B bundle, its ENTIRE selector surface is four
 * literals -- `#comment_parent`, `img`, `.comment-form__verbum`, and
 * `.wp-die-message p` -- and there are no non-literal selector arguments. So
 * tag / .class / #id / descendant is complete for this bundle, not a guess.
 *
 * Supported: `tag`, `*`, `.class`, `#id`, any combination (`div.a.b#c`), and the
 * descendant combinator (`.a b c`). Matching is right-to-left from the subject,
 * as every real engine does.
 *
 * NOT supported: `[attr]`, `:pseudo`, `>`, `+`, `~`, `,`. Those keep the OLD
 * tag-only approximation rather than returning empty, so nothing that relies on
 * today's sloppy behaviour regresses -- but the approximation is now explicit
 * and logged instead of being an unmarked lie in a comment.
 */
#define QJS_SEL_MAX_COMPOUND 4
#define QJS_SEL_MAX_CLASS    4
#define QJS_SEL_NAME         64

struct qjs_sel_compound {
	char tag[32];                                 /* "" = any, or lowercase */
	char cls[QJS_SEL_MAX_CLASS][QJS_SEL_NAME];
	int  ncls;
	char id[QJS_SEL_NAME];                        /* "" = none */
};

struct qjs_sel {
	struct qjs_sel_compound c[QJS_SEL_MAX_COMPOUND];
	int n;      /* compound count; c[n-1] is the SUBJECT (rightmost) */
	int approx; /* 1 = selector had syntax we ignored (tag-only fallback) */
};

/* Whitespace-delimited token test over a class attribute value. `strstr` would
 * be wrong: class="foobar" must NOT match `.foo`. */
static int qjs_class_has(const char *list, const char *want)
{
	size_t wl;
	const char *p = list;

	if (list == NULL || want == NULL || want[0] == '\0') return 0;
	wl = strlen(want);
	while (*p != '\0') {
		const char *s;
		while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'
		       || *p == '\f') {
			p++;
		}
		if (*p == '\0') break;
		s = p;
		while (*p != '\0' && *p != ' ' && *p != '\t' && *p != '\n'
		       && *p != '\r' && *p != '\f') {
			p++;
		}
		if ((size_t)(p - s) == wl && strncmp(s, want, wl) == 0) return 1;
	}
	return 0;
}

/* Read an attribute into a caller buffer. Returns 1 when present. */
static int qjs_attr_str(dom_element *el, const char *name, char *out, int cap)
{
	dom_string *nds;
	dom_string *vds = NULL;
	int got = 0;

	out[0] = '\0';
	nds = qjs_make_domstr(name);
	if (nds == NULL) return 0;
	if (macsurf_dom_element_get_attribute(el, nds, &vds) == DOM_NO_ERR
	    && vds != NULL) {
		const char *v = dom_string_data(vds);
		int i = 0;
		while (i < cap - 1 && v[i] != '\0') { out[i] = v[i]; i++; }
		out[i] = '\0';
		macsurf_dom_string_unref(vds);
		got = 1;
	}
	macsurf_dom_string_unref(nds);
	return got;
}

/* Parse a selector into compounds. Never fails; unsupported syntax degrades to
 * the tag-only approximation and sets ->approx. */
static void qjs_sel_parse(const char *sel, struct qjs_sel *out)
{
	int ci = 0;
	const char *p = sel;

	memset(out, 0, sizeof(*out));
	out->n = 0;

	while (*p != '\0' && ci < QJS_SEL_MAX_COMPOUND) {
		struct qjs_sel_compound *c;
		int started = 0;

		while (*p == ' ' || *p == '\t') p++;
		if (*p == '\0') break;

		c = &out->c[ci];

		while (*p != '\0' && *p != ' ' && *p != '\t') {
			if (*p == '.' || *p == '#') {
				char kind = *p++;
				char buf[QJS_SEL_NAME];
				int k = 0;
				while (k < QJS_SEL_NAME - 1 && *p != '\0' && *p != '.'
				       && *p != '#' && *p != ' ' && *p != '\t'
				       && *p != '[' && *p != ':' && *p != '>'
				       && *p != ',' && *p != '+' && *p != '~') {
					buf[k++] = *p++;
				}
				buf[k] = '\0';
				if (k == 0) { out->approx = 1; continue; }
				if (kind == '.') {
					if (c->ncls < QJS_SEL_MAX_CLASS) {
						strcpy(c->cls[c->ncls], buf);
						c->ncls++;
						started = 1;
					} else {
						out->approx = 1;
					}
				} else {
					strcpy(c->id, buf);
					started = 1;
				}
			} else if (*p == '[' || *p == ':' || *p == '>' || *p == ','
				   || *p == '+' || *p == '~') {
				/* Unsupported: swallow the rest of this compound and
				 * fall back to whatever tag/class/id we already have. */
				out->approx = 1;
				while (*p != '\0' && *p != ' ' && *p != '\t') p++;
			} else {
				int k = 0;
				while (k < 31 && *p != '\0' && *p != '.' && *p != '#'
				       && *p != ' ' && *p != '\t' && *p != '['
				       && *p != ':' && *p != '>' && *p != ','
				       && *p != '+' && *p != '~') {
					char ch = *p++;
					c->tag[k++] = (ch >= 'A' && ch <= 'Z')
							? (char)(ch + 32) : ch;
				}
				c->tag[k] = '\0';
				if (k > 0) started = 1;
			}
		}
		if (started) ci++;
	}
	out->n = ci;
	if (*p != '\0') out->approx = 1; /* ran out of compound slots */
}

/* Does ONE element match ONE compound? */
static int qjs_compound_match(dom_node *node, const struct qjs_sel_compound *c)
{
	dom_node_type ntype = 0;
	int i;

	if (node == NULL) return 0;
	macsurf_dom_node_get_node_type(node, &ntype);
	if (ntype != 1) return 0; /* ELEMENT_NODE only */

	if (c->tag[0] != '\0' && strcmp(c->tag, "*") != 0) {
		dom_string *tname = NULL;
		char lc[32];
		int k;
		int ok;
		if (macsurf_dom_element_get_tag_name((dom_element *)node, &tname)
		    != DOM_NO_ERR || tname == NULL) {
			return 0;
		}
		{
			const char *ts = dom_string_data(tname);
			for (k = 0; k < 31 && ts[k] != '\0'; k++) {
				char ch = ts[k];
				lc[k] = (ch >= 'A' && ch <= 'Z') ? (char)(ch + 32) : ch;
			}
			lc[k] = '\0';
		}
		macsurf_dom_string_unref(tname);
		ok = (strcmp(lc, c->tag) == 0);
		if (!ok) return 0;
	}

	if (c->id[0] != '\0') {
		char buf[QJS_SEL_NAME];
		if (!qjs_attr_str((dom_element *)node, "id", buf, (int)sizeof(buf)))
			return 0;
		if (strcmp(buf, c->id) != 0) return 0;
	}

	if (c->ncls > 0) {
		char buf[512];
		if (!qjs_attr_str((dom_element *)node, "class", buf, (int)sizeof(buf)))
			return 0;
		for (i = 0; i < c->ncls; i++) {
			if (!qjs_class_has(buf, c->cls[i])) return 0;
		}
	}
	return 1;
}

/* Full match: subject compound against `node`, then each preceding compound
 * against some ancestor, right-to-left (what every real engine does -- it lets
 * a non-matching subject bail before any ancestor walk). */
static int qjs_sel_match(dom_node *node, const struct qjs_sel *s)
{
	int ci;
	dom_node *cur = NULL;

	if (s->n <= 0) return 0;
	ci = s->n - 1;
	if (!qjs_compound_match(node, &s->c[ci])) return 0;
	ci--;
	if (ci < 0) return 1;

	macsurf_dom_node_get_parent_node(node, &cur);
	while (cur != NULL && ci >= 0) {
		dom_node *par = NULL;
		if (qjs_compound_match(cur, &s->c[ci])) ci--;
		macsurf_dom_node_get_parent_node(cur, &par);
		macsurf_dom_node_unref(cur);
		cur = par;
	}
	if (cur != NULL) macsurf_dom_node_unref(cur);
	return (ci < 0) ? 1 : 0;
}

/* Collect every match in document order. */
static void qjs_collect_by_sel(JSContext *ctx, dom_node *node,
		const struct qjs_sel *s, JSValue arr, int *count)
{
	dom_node *child = NULL;
	dom_node *next  = NULL;

	if (node == NULL) return;
	if (qjs_sel_match(node, s)) {
		macsurf_dom_node_ref(node); /* qjs_wrap_element CONSUMES a ref */
		JS_SetPropertyUint32(ctx, arr, (uint32_t)(*count),
				qjs_wrap_element(ctx, (dom_element *)node));
		(*count)++;
	}
	macsurf_dom_node_get_first_child(node, &child);
	while (child != NULL) {
		qjs_collect_by_sel(ctx, child, s, arr, count);
		macsurf_dom_node_get_next_sibling(child, &next);
		macsurf_dom_node_unref(child);
		child = next;
	}
}

/* First match in document order; returns an owned ref for qjs_wrap_element. */
static dom_element *qjs_find_first_by_sel(dom_node *node, const struct qjs_sel *s)
{
	dom_node *child = NULL;
	dom_node *next  = NULL;
	dom_element *found = NULL;

	if (node == NULL) return NULL;
	if (qjs_sel_match(node, s)) {
		macsurf_dom_node_ref(node);
		return (dom_element *)node;
	}
	macsurf_dom_node_get_first_child(node, &child);
	while (child != NULL) {
		found = qjs_find_first_by_sel(child, s);
		if (found != NULL) {
			macsurf_dom_node_unref(child);
			return found;
		}
		macsurf_dom_node_get_next_sibling(child, &next);
		macsurf_dom_node_unref(child);
		child = next;
	}
	return NULL;
}

static JSValue qjs_querySelectorAll(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv)
{
	const char *sel;
	char tag_lc[64];
	int i;
	dom_element *root = NULL;
	JSValue arr;
	int count = 0;

	(void)this_val;
	arr = JS_NewArray(ctx);
	if (g_qjs_document == NULL || argc < 1) return arr;
	sel = JS_ToCString(ctx, argv[0]);
	if (sel == NULL) return arr;

	/* fixes871 (#298) — the fixes864 '#id' fast path that used to sit here is
	 * gone: it did strchr(sel,'#') ANYWHERE in the selector, so `#a .b` returned
	 * #a rather than the .b inside it. Ids are just another compound qualifier
	 * to the matcher below, which gets `#a .b`, `div#a`, and `.x#a` all right.
	 * (qs keeps a getElementById fast path because a single-id lookup there is
	 * O(1) and by far the most common call; a qsa returning a 0-or-1 array does
	 * not justify a second, subtly-different code path.)
	 *
	 * fixes871 (#298) — real compound matching (tag/.class/#id/descendant).
	 * The old code extracted only the tag, so `.comment-form__verbum` (Preact's
	 * Verbum mount) returned EMPTY and `div.foo` matched every div. */
	{
		struct qjs_sel s;
		qjs_sel_parse(sel, &s);
		if (s.approx) {
			macsurf_debug_log_writef(
				"WORK qsa: APPROX selector (unsupported syntax "
				"ignored) sel=%s", sel);
		}
		JS_FreeCString(ctx, sel);
		if (s.n == 0) return arr;

		macsurf_dom_document_get_document_element(g_qjs_document, &root);
		if (root != NULL) {
			qjs_collect_by_sel(ctx, (dom_node *)root, &s, arr, &count);
			macsurf_dom_node_unref((dom_node *)root);
		}
	}
	(void)tag_lc; (void)i;
	return arr;
}

/* fixes691 (#210): first-match-only walker. Pre-order DFS that returns the
 * FIRST matching element (with one ref held for the caller to hand to
 * qjs_wrap_element, which takes ownership) instead of collecting+wrapping the
 * whole matching set. Same document order as qjs_collect_by_tag, so it returns
 * exactly what qsa[0] would have — without the O(n) walk and the expensive
 * per-node wrapper install on every non-first match. */
static dom_element *qjs_find_first_by_tag(dom_node *node, const char *tag_lc)
{
	dom_node *child = NULL;
	dom_node *next  = NULL;
	dom_node_type ntype = 0;
	dom_element *found = NULL;

	if (node == NULL) return NULL;
	macsurf_dom_node_get_node_type(node, &ntype);
	if (ntype == 1) { /* ELEMENT_NODE */
		dom_string *tname = NULL;
		if (macsurf_dom_element_get_tag_name((dom_element *)node, &tname)
		    == DOM_NO_ERR && tname != NULL) {
			const char *ts = dom_string_data(tname);
			char lc[32];
			int i;
			for (i = 0; i < 31 && ts[i]; i++) {
				char c = ts[i];
				lc[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
			}
			lc[i] = '\0';
			macsurf_dom_string_unref(tname);
			if (strcmp(tag_lc, "*") == 0 || strcmp(lc, tag_lc) == 0) {
				macsurf_dom_node_ref(node);
				return (dom_element *)node;
			}
		}
	}
	macsurf_dom_node_get_first_child(node, &child);
	while (child) {
		found = qjs_find_first_by_tag(child, tag_lc);
		if (found != NULL) {
			macsurf_dom_node_unref(child);
			return found;
		}
		macsurf_dom_node_get_next_sibling(child, &next);
		macsurf_dom_node_unref(child);
		child = next;
	}
	return NULL;
}

/* ---- document.querySelector ---- */
static JSValue qjs_querySelector(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv)
{
	const char *sel;
	char tag_lc[64];
	int i;
	dom_element *root = NULL;
	dom_element *found = NULL;

	(void)this_val;
	if (g_qjs_document == NULL || argc < 1) return JS_NULL;
	sel = JS_ToCString(ctx, argv[0]);
	if (sel == NULL) return JS_NULL;

	/* fixes864 (#290) — '#id' had no branch at all, so "#commentform" fell
	 * through as a TAG NAME, matched nothing, and returned null while
	 * getElementById('commentform') found it fine.  Silent null, no throw --
	 * exactly how hackaday's reply box dies:
	 *     var e = document.querySelector("#commentform"); if (e) { ...all of it... }
	 * a null `e` skips the whole chain (IntersectionObserver -> loadScript ->
	 * fetch -> injected <script>) without a single error line.
	 *
	 * fixes871 (#298) — that branch did `strchr(sel, '#')` ANYWHERE in the
	 * selector, so `#a .b` ("the .b inside #a") returned #a: the wrong element,
	 * confidently. Now the fast path is taken only when the parsed selector
	 * really is a single id-bearing compound, and the result is still run
	 * through the full compound match so a tag/class qualifier can reject it. */
	{
		struct qjs_sel s;
		qjs_sel_parse(sel, &s);
		if (s.approx) {
			macsurf_debug_log_writef(
				"WORK qs: APPROX selector (unsupported syntax "
				"ignored) sel=%s", sel);
		}
		JS_FreeCString(ctx, sel);
		if (s.n == 0) return JS_NULL;

		if (s.n == 1 && s.c[0].id[0] != '\0') {
			dom_string *id_ds = qjs_make_domstr(s.c[0].id);
			dom_element *el = NULL;
			if (id_ds == NULL) return JS_NULL;
			macsurf_dom_document_get_element_by_id(g_qjs_document,
					id_ds, &el);
			macsurf_dom_string_unref(id_ds);
			if (el == NULL) return JS_NULL;
			if (!qjs_compound_match((dom_node *)el, &s.c[0])) {
				macsurf_dom_node_unref((dom_node *)el);
				return JS_NULL;
			}
			/* get_element_by_id hands back a ref; wrap takes it. */
			return qjs_wrap_element(ctx, el);
		}

		macsurf_dom_document_get_document_element(g_qjs_document, &root);
		if (root == NULL) return JS_NULL;
		found = qjs_find_first_by_sel((dom_node *)root, &s);
		macsurf_dom_node_unref((dom_node *)root);
		if (found == NULL) return JS_NULL;
		return qjs_wrap_element(ctx, found);
	}
}

/* ---- Init class ID (call once at startup) ---- */
static void qjs_dom_init_class(JSRuntime *rt)
{
	s_el_class_id = 0;
	JS_NewClassID(rt, &s_el_class_id);
	JS_NewClass(rt, s_el_class_id, &s_el_class);
}

/* ---- Wire getElementById/querySelectorAll on the document object ---- */
/* ================================================================== */
/* register_browser_globals — installs the browser runtime globals    */
/* ================================================================== */

static void qjs_set_func(JSContext *ctx, JSValue obj,
		const char *name, JSCFunction *fn, int nargs)
{
	JSValue f = JS_NewCFunction(ctx, fn, name, nargs);
	JS_SetPropertyStr(ctx, obj, name, f);
}

/* ---- document.createElement (real libdom-backed element) ----
 * Returns a fully-wired element wrapper whose appendChild / removeChild /
 * insertBefore / parentNode / classList / style all operate on the live
 * libdom node, so a detached element appended to document.body and then
 * removed via b.parentNode.removeChild(b) (XenForo preamble hiddenscroll)
 * works correctly.  Falls back to JS_NULL if no document is wired yet; the
 * JS layer then supplies a parentNode-tracking fallback element. */
static JSValue qjs_create_element(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv)
{
	const char *tag;
	dom_element *el = NULL;
	JSValue obj;

	(void)this_val;
	if (g_qjs_document == NULL || argc < 1) return JS_NULL;
	tag = JS_ToCString(ctx, argv[0]);
	if (tag == NULL) return JS_NULL;
	if (macsurf_dom_document_create_element_s(g_qjs_document, tag, &el)
	    != DOM_NO_ERR || el == NULL) {
		JS_FreeCString(ctx, tag);
		return JS_NULL;
	}
	JS_FreeCString(ctx, tag);
	obj = qjs_wrap_element_full(ctx, el);
	return obj;
}

/* ---- document.createElementNS(namespaceURI, qualifiedName, options) ----
 * Preact's ONLY element factory. Its renderer is, verbatim:
 *     if ("svg" == k) a = "http://www.w3.org/2000/svg";
 *     else if ("math" == k) a = "http://www.w3.org/1998/Math/MathML";
 *     else if (!a) a = "http://www.w3.org/1999/xhtml";
 *     ...
 *     e = document.createElementNS(a, k, w.is && w)
 * -- createElement is never reached from the reconciler at all, so without this
 * a Preact app (hackaday's Verbum comment form) renders literally nothing.
 *
 * The THIRD ARGUMENT IS A TRAP: `w.is && w` is `undefined` when props.is is
 * unset, but the ENTIRE vnode props object when it is set. It must be tolerated
 * without choking. We deliberately never read argv[2] -- custom elements ("is")
 * are not implemented, and per spec an unrecognised `is` is simply ignored.
 * Reading it (e.g. JS_ToCString on an object) is what would break.
 *
 * argv[0] may legitimately be null: createElementNS(null, 'div') is valid and
 * means "no namespace", so null/undefined is passed through as NULL rather than
 * treated as an error. */
static JSValue qjs_create_element_ns(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv)
{
	const char *ns = NULL;
	const char *tag = NULL;
	dom_element *el = NULL;
	JSValue obj;

	(void)this_val;
	if (g_qjs_document == NULL || argc < 2) return JS_NULL;

	if (!JS_IsNull(argv[0]) && !JS_IsUndefined(argv[0])) {
		ns = JS_ToCString(ctx, argv[0]);
	}
	tag = JS_ToCString(ctx, argv[1]);
	if (tag == NULL) {
		if (ns != NULL) JS_FreeCString(ctx, ns);
		return JS_NULL;
	}

	if (macsurf_dom_document_create_element_ns_s(g_qjs_document, ns, tag, &el)
	    != DOM_NO_ERR || el == NULL) {
		JS_FreeCString(ctx, tag);
		if (ns != NULL) JS_FreeCString(ctx, ns);
		return JS_NULL;
	}
	JS_FreeCString(ctx, tag);
	if (ns != NULL) JS_FreeCString(ctx, ns);

	obj = qjs_wrap_element_full(ctx, el);
	return obj;
}

/* ---- Wrap the live <html>/<body>/<head> element ----
 * which : 0 = documentElement (<html>), 1 = <body>, 2 = <head>.
 * Returns JS_NULL when the document is not parsed/wired (JS fallback path). */
static JSValue qjs_wrap_doc_section(JSContext *ctx, int which)
{
	dom_element *root = NULL;
	JSValue result = JS_NULL;

	if (g_qjs_document == NULL) return JS_NULL;
	macsurf_dom_document_get_document_element(g_qjs_document, &root);
	if (root == NULL) return JS_NULL;

	if (which == 0) {
		/* documentElement: wrap <html> directly (ref owned by wrapper) */
		result = qjs_wrap_element_full(ctx, root);
		return result;
	}

	/* body / head: first matching element child of <html> */
	{
		const char *want = (which == 1) ? "body" : "head";
		dom_node *child = NULL;
		dom_node *next = NULL;
		dom_node_type ntype = 0;

		macsurf_dom_node_get_first_child((dom_node *)root, &child);
		while (child != NULL) {
			ntype = 0;
			macsurf_dom_node_get_node_type(child, &ntype);
			if (ntype == 1) {
				dom_string *tname = NULL;
				if (macsurf_dom_element_get_tag_name(
					(dom_element *)child, &tname) == DOM_NO_ERR
				    && tname != NULL) {
					const char *ts = dom_string_data(tname);
					char lc[16];
					int i;
					for (i = 0; i < 15 && ts[i]; i++) {
						char c = ts[i];
						lc[i] = (c >= 'A' && c <= 'Z')
							? (char)(c + 32) : c;
					}
					lc[i] = '\0';
					macsurf_dom_string_unref(tname);
					if (strcmp(lc, want) == 0) {
						macsurf_dom_node_ref(child);
						result = qjs_wrap_element_full(ctx,
							(dom_element *)child);
						macsurf_dom_node_unref(child);
						break;
					}
				}
			}
			macsurf_dom_node_get_next_sibling(child, &next);
			macsurf_dom_node_unref(child);
			child = next;
		}
	}
	macsurf_dom_node_unref((dom_node *)root);
	return result;
}

static JSValue qjs_get_document_element(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv)
{
	(void)this_val; (void)argc; (void)argv;
	return qjs_wrap_doc_section(ctx, 0);
}

static JSValue qjs_get_body(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv)
{
	(void)this_val; (void)argc; (void)argv;
	return qjs_wrap_doc_section(ctx, 1);
}

static JSValue qjs_get_head(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv)
{
	(void)this_val; (void)argc; (void)argv;
	return qjs_wrap_doc_section(ctx, 2);
}

/* Wire getElementById/querySelectorAll onto the document object */
/* ---- fixes872 (#300) — the element PROTOTYPE, carrying the on* handlers ----
 *
 * Preact decides an event's name from whether the property EXISTS:
 *     i = t != (t = t.replace(d,"$1")),
 *     a = t.toLowerCase(),
 *     t = a in e || "onFocusOut"==t || "onFocusIn"==t ? a.slice(2) : t.slice(2),
 *     e.l || (e.l = {}), e.l[t+i] = n,
 *     n ? (o ? n.u = o.u : (n.u = _, e.addEventListener(t, i?p:m, i)))
 *       : e.removeEventListener(t, i?p:m, i)
 * (verbatim from verbum-comments.js). For onClick: a = "onclick". If
 * `"onclick" in e` is TRUE it registers addEventListener("click") -- correct. If
 * FALSE it falls to `t.slice(2)` and registers addEventListener("Click"), capital
 * C, which NOTHING ever dispatches. The form then renders perfectly and silently
 * ignores every click, which is about the worst failure shape available: it looks
 * finished.
 *
 * So the entire requirement for Verbum is that `"onclick" in e` be true.
 * `el.onclick =` appears ZERO times in the whole bundle -- Preact keeps handlers
 * in its own `e.l` map and registers ONE dispatcher per type. Proper replace
 * semantics are implemented anyway, for the many sites that DO assign on*.
 *
 * On the PROTOTYPE, not per element:
 *   - `in` walks the prototype chain, so this satisfies Preact at zero
 *     per-element cost. The alternative (~27 defineProperty calls inside the
 *     per-element install eval) would add real G3 time to EVERY wrapped node,
 *     and that install is already the expensive part of wrapping.
 *   - The proto also gives elements Object.prototype (hasOwnProperty/toString),
 *     which a null-proto object does not have. Closer to a real browser.
 * Accessors are non-enumerable so `for (k in el)` does not start listing 27 new
 * keys on code that never asked for them.
 *
 * Handlers live in the element's own `_H` map, deliberately SEPARATE from the
 * `_L` addEventListener array, because the two have different semantics:
 * assigning on* REPLACES, addEventListener ACCUMULATES. Keeping them apart makes
 * replace semantics fall out of `_H[t] = v` for free, and lets dispatchEvent
 * fire both exactly once.
 *
 * Set per CONTEXT (class protos are per-context in QuickJS), from
 * qjs_dom_install, which already runs once per realm before anything is wrapped.
 */
static void qjs_el_install_proto(JSContext *ctx)
{
	static const char s_proto_src[] =
		"(function(){"
		"var n=['click','dblclick','mousedown','mouseup','mousemove',"
			"'mouseover','mouseout','mouseenter','mouseleave',"
			"'keydown','keyup','keypress',"
			"'input','change','submit','reset','focus','blur','select',"
			"'load','error','scroll','resize','contextmenu',"
			"'touchstart','touchend','touchmove'];"
		"var p={};var i;"
		"for(i=0;i<n.length;i++){(function(k){"
		"Object.defineProperty(p,'on'+k,{configurable:true,enumerable:false,"
		"get:function(){return (this._H&&this._H[k])||null;},"
		"set:function(v){if(!this._H)this._H={};"
		"this._H[k]=(typeof v==='function')?v:null;}});"
		"})(n[i]);}"
		"return p;})()";
	JSValue proto;

	/* qjs_dom_install() runs TWICE per context (once at build, once when the
	 * thread's real document is wired), so bail if the proto is already in
	 * place. Re-running is not corrupting -- the second proto is identical and
	 * per-element _H maps are unaffected -- but it would orphan the first proto
	 * and leave elements wrapped in between pointing at a different (equivalent)
	 * object, which is a confusing thing to leave lying around for no gain. */
	proto = JS_GetClassProto(ctx, s_el_class_id);
	if (JS_IsObject(proto)) {
		JS_FreeValue(ctx, proto);
		return;
	}
	JS_FreeValue(ctx, proto);

	proto = JS_Eval(ctx, s_proto_src, strlen(s_proto_src),
			"<el-proto>", JS_EVAL_TYPE_GLOBAL);
	if (JS_IsException(proto)) {
		JS_FreeValue(ctx, proto);
		return;
	}
	/* JS_SetClassProto takes ownership; do not free proto after this. */
	JS_SetClassProto(ctx, s_el_class_id, proto);
}

static void qjs_dom_install(JSContext *ctx)
{
	JSValue global = JS_GetGlobalObject(ctx);
	JSValue doc    = JS_GetPropertyStr(ctx, global, "document");

	/* fixes872 (#300) — before any element is wrapped in this realm. */
	qjs_el_install_proto(ctx);
	if (!JS_IsUndefined(doc) && !JS_IsNull(doc)) {
		qjs_set_func(ctx, doc, "getElementById",
				qjs_getElementById, 1);
		qjs_set_func(ctx, doc, "querySelector",
				qjs_querySelector, 1);
		qjs_set_func(ctx, doc, "querySelectorAll",
				qjs_querySelectorAll, 1);
		/* Real libdom-backed createElement (native fast path; JS wrapper
		 * below adds a parentNode-tracking fallback for the pre-document
		 * window). */
		qjs_set_func(ctx, doc, "__createElementNative",
				qjs_create_element, 1);
		/* fixes870 (#297) — createElementNS: Preact's only element factory. */
		qjs_set_func(ctx, doc, "__createElementNSNative",
				qjs_create_element_ns, 3);
		/* fixes846 (#167 S3) — real createTextNode/createDocumentFragment,
		 * same native-fast-path/JS-fallback shape as createElement above. */
		qjs_set_func(ctx, doc, "__createTextNodeNative",
				qjs_create_text_node, 1);
		qjs_set_func(ctx, doc, "__createDocumentFragmentNative",
				qjs_create_document_fragment, 0);
		/* fixes879 — document.cookie as a REAL accessor pair over the
		 * urldb jar, replacing the `document.cookie=''` data property
		 * installed in register_browser_globals. Defined here (rather
		 * than in the JS block) because qjs_dom_install runs once a real
		 * document is wired, which is exactly when a URL exists to key
		 * the jar by. defineProperty overrides the plain data property
		 * whichever order the two run in. */
		{
			JSAtom atom = JS_NewAtom(ctx, "cookie");
			JSValue getter = JS_NewCFunction(ctx,
					qjs_document_cookie_get, "get", 0);
			JSValue setter = JS_NewCFunction(ctx,
					qjs_document_cookie_set, "set", 1);
			JS_DefinePropertyGetSet(ctx, doc, atom, getter, setter,
					JS_PROP_CONFIGURABLE);
			JS_FreeAtom(ctx, atom);
		}
		/* Native section accessors used by the body/head/documentElement
		 * getters installed in JS below. */
		qjs_set_func(ctx, doc, "__getDocumentElement",
				qjs_get_document_element, 0);
		qjs_set_func(ctx, doc, "__getBody", qjs_get_body, 0);
		qjs_set_func(ctx, doc, "__getHead", qjs_get_head, 0);

		/* Define body / head / documentElement as getters that return the
		 * live libdom-wrapped element, falling back to a parentNode-tracking
		 * JS element so XenForo's hiddenscroll probe (append a detached div
		 * to body, then b.parentNode.removeChild(b)) works in both states. */
		{
			static const char *sec_src =
			"(function(d){"
			"function mkfb(tag){"
			"var vw=(typeof innerWidth==='number'&&innerWidth)||980;"
			"var vh=(typeof innerHeight==='number'&&innerHeight)||600;"
			"var attrs={};var kids=[];"
			"var cls=function(e){return{"
			"contains:function(c){return(' '+(attrs['class']||'')+' ').indexOf(' '+c+' ')>=0;},"
			"add:function(){var i;for(i=0;i<arguments.length;i++){if(!this.contains(arguments[i]))attrs['class']=(attrs['class']?attrs['class']+' ':'')+arguments[i];}},"
			"remove:function(){var i;for(i=0;i<arguments.length;i++){attrs['class']=(' '+(attrs['class']||'')+' ').replace(' '+arguments[i]+' ',' ').replace(/^\\s+|\\s+$/g,'');}},"
			"toggle:function(c,f){if(f===true)this.add(c);else if(f===false)this.remove(c);else if(this.contains(c))this.remove(c);else this.add(c);return this.contains(c);},"
			"replace:function(o,n){this.remove(o);this.add(n);},"
			"toString:function(){return attrs['class']||'';}};};"
			"var el={nodeType:1,tagName:(tag||'div').toUpperCase(),"
			"clientWidth:vw,clientHeight:vh,offsetWidth:vw,offsetHeight:vh,"
			"scrollWidth:vw,scrollHeight:vh,scrollTop:0,scrollLeft:0,"
			"offsetTop:0,offsetLeft:0,style:{},parentNode:null,"
			"childNodes:kids,firstChild:null,lastChild:null,"
			"getAttribute:function(n){return attrs[n]!==undefined?attrs[n]:null;},"
			"setAttribute:function(n,v){attrs[n]=String(v);},"
			"removeAttribute:function(n){delete attrs[n];},"
			"hasAttribute:function(n){return attrs[n]!==undefined;},"
			"appendChild:function(c){if(c){c.parentNode=this;kids.push(c);"
			"this.firstChild=kids[0];this.lastChild=kids[kids.length-1];}return c;},"
			"removeChild:function(c){var i=kids.indexOf(c);if(i>=0){kids.splice(i,1);"
			"if(c)c.parentNode=null;this.firstChild=kids[0]||null;"
			"this.lastChild=kids[kids.length-1]||null;}return c;},"
			"insertBefore:function(c,r){var i=kids.indexOf(r);"
			"if(i<0)i=kids.length;kids.splice(i,0,c);if(c)c.parentNode=this;"
			"this.firstChild=kids[0]||null;this.lastChild=kids[kids.length-1]||null;return c;},"
			"contains:function(){return false;},"
			"addEventListener:function(){},removeEventListener:function(){},"
			"dispatchEvent:function(){return true;},"
			"querySelector:function(){return null;},querySelectorAll:function(){return[];},"
			"getElementsByClassName:function(){return[];},getElementsByTagName:function(){return[];},"
			"getBoundingClientRect:function(){return{top:0,left:0,right:vw,bottom:vh,width:vw,height:vh,x:0,y:0};}};"
			"el.className='';"
			"Object.defineProperty(el,'classList',{get:(function(){var c=cls(el);return function(){return c;};})(),configurable:true});"
			"return el;}"
			"d.createElement=function(tag){"
			"var n=d.__createElementNative?d.__createElementNative(tag):null;"
			"if(n)return n;return mkfb(tag);};"
			/* fixes870 (#297) — createElementNS, Preact's only element factory.
			 * Same native-then-fallback shape as createElement above. `opt` is
			 * accepted and deliberately NEVER forwarded: Preact passes
			 * `props.is && props`, i.e. undefined OR the entire vnode props
			 * object, so swallowing it here guarantees the native side can
			 * never be handed an object it might try to read as a string. */
			"d.createElementNS=function(ns,tag,opt){"
			"var n=d.__createElementNSNative?"
				"d.__createElementNSNative(ns,tag):null;"
			"if(n)return n;return mkfb(tag);};"
			"if(typeof d.createDocumentFragment!=='function'||!d.__hasFrag){"
			"d.__hasFrag=true;"
			"d.createDocumentFragment=function(){"
			"var n=d.__createDocumentFragmentNative?"
			"d.__createDocumentFragmentNative():null;"
			"if(n)return n;return mkfb('#fragment');};"
			"}"
			"var _fbHtml=null,_fbBody=null,_fbHead=null;"
			"Object.defineProperty(d,'documentElement',{configurable:true,"
			"get:function(){var n=d.__getDocumentElement();if(n)return n;"
			"if(!_fbHtml)_fbHtml=mkfb('html');return _fbHtml;}});"
			"Object.defineProperty(d,'body',{configurable:true,"
			"get:function(){var n=d.__getBody();if(n)return n;"
			"if(!_fbBody)_fbBody=mkfb('body');return _fbBody;}});"
			"Object.defineProperty(d,'head',{configurable:true,"
			"get:function(){var n=d.__getHead();if(n)return n;"
			"if(!_fbHead)_fbHead=mkfb('head');return _fbHead;}});"
			"})";
			JSValue fn, args[1];
			fn = JS_Eval(ctx, sec_src, strlen(sec_src),
				"<doc-sections>", JS_EVAL_TYPE_GLOBAL);
			if (!JS_IsException(fn)) {
				args[0] = JS_DupValue(ctx, doc);
				JS_Call(ctx, fn, JS_UNDEFINED, 1, args);
				JS_FreeValue(ctx, args[0]);
			} else {
				JSValue ex = JS_GetException(ctx);
				const char *m = JS_ToCString(ctx, ex);
				macsurf_debug_log_writef(
					"qjs doc-sections eval error: %s",
					m ? m : "?");
				if (m) JS_FreeCString(ctx, m);
				JS_FreeValue(ctx, ex);
			}
			JS_FreeValue(ctx, fn);
		}
	}
	JS_FreeValue(ctx, doc);
	JS_FreeValue(ctx, global);
}

/* ====================================================================
 * fixes717 (#207 diagnostic) — crypto.getRandomValues / crypto.randomUUID
 *
 * QuickJS ships no `crypto` global, so any script that touches it (uuid
 * libraries, cache-busting, the Cloudflare beacon captured in the
 * SuperLogger log) throws "crypto.getRandomValues() not supported" and
 * aborts the whole script. Fill requests from a clock-seeded xorshift
 * generator, re-stirred with a fresh high-resolution timestamp (and
 * stack-address noise) on every call. This is NOT cryptographic-grade;
 * the goal is that these scripts RUN instead of crashing (DIRECTIVE #2).
 * If a page ever needs real CSPRNG output, back this with macEntropy's
 * pool (OSTLS_*), which is already linked and hardware-verified.
 * ==================================================================== */
static unsigned long qjs_rng_s[2] = { 0UL, 0UL };

static void qjs_rng_stir(void)
{
	unsigned long t = (unsigned long)macsurf_monotonic_ms();
	unsigned long a = (unsigned long)(size_t)&t;   /* stack-address noise */
	static unsigned long ctr = 0UL;
	ctr += 0x9E3779B9UL;
	if (qjs_rng_s[0] == 0UL && qjs_rng_s[1] == 0UL) {
		qjs_rng_s[0] = t ^ 0x85EBCA6BUL ^ ctr;
		qjs_rng_s[1] = a ^ 0xC2B2AE35UL;
		if (qjs_rng_s[0] == 0UL) qjs_rng_s[0] = 1UL;
		if (qjs_rng_s[1] == 0UL) qjs_rng_s[1] = 1UL;
	} else {
		qjs_rng_s[0] ^= (t + ctr) & 0xFFFFFFFFUL;
		qjs_rng_s[1] ^= ((a << 1) + 0x27D4EB2FUL) & 0xFFFFFFFFUL;
	}
}

static unsigned long qjs_rng_next(void)
{
	unsigned long s1 = qjs_rng_s[0];
	unsigned long s0 = qjs_rng_s[1];
	qjs_rng_s[0] = s0;
	s1 ^= (s1 << 13) & 0xFFFFFFFFUL;
	s1 ^= s1 >> 17;
	s1 ^= s0 ^ (s0 >> 5);
	qjs_rng_s[1] = s1 & 0xFFFFFFFFUL;
	return (s1 + s0) & 0xFFFFFFFFUL;
}

static JSValue qjs_crypto_get_random_values(JSContext *ctx,
	JSValueConst this_val, int argc, JSValueConst *argv)
{
	size_t byte_off = 0, byte_len = 0, bpe = 0, ab_size = 0;
	JSValue ab;
	uint8_t *ptr;
	size_t i;
	unsigned long r = 0UL;
	int rb = 0;

	(void)this_val;
	if (argc < 1)
		return JS_ThrowTypeError(ctx,
			"crypto.getRandomValues requires a TypedArray");
	ab = JS_GetTypedArrayBuffer(ctx, argv[0], &byte_off, &byte_len, &bpe);
	if (JS_IsException(ab))
		return ab;
	ptr = JS_GetArrayBuffer(ctx, &ab_size, ab);
	if (ptr == NULL) {
		JS_FreeValue(ctx, ab);
		return JS_ThrowTypeError(ctx,
			"crypto.getRandomValues: not a typed array");
	}
	if (byte_len > 65536) {
		JS_FreeValue(ctx, ab);
		return JS_ThrowRangeError(ctx,
			"crypto.getRandomValues: array exceeds 65536 bytes");
	}
	qjs_rng_stir();
	for (i = 0; i < byte_len; i++) {
		if (rb == 0) { r = qjs_rng_next(); rb = 4; }
		ptr[byte_off + i] = (uint8_t)(r & 0xFFUL);
		r >>= 8;
		rb--;
	}
	JS_FreeValue(ctx, ab);
	return JS_DupValue(ctx, argv[0]);   /* spec: returns the same array */
}

static JSValue qjs_crypto_random_uuid(JSContext *ctx,
	JSValueConst this_val, int argc, JSValueConst *argv)
{
	static const char hex[] = "0123456789abcdef";
	unsigned char b[16];
	char out[37];
	unsigned long r = 0UL;
	int rb = 0, i, p;

	(void)this_val; (void)argc; (void)argv;
	qjs_rng_stir();
	for (i = 0; i < 16; i++) {
		if (rb == 0) { r = qjs_rng_next(); rb = 4; }
		b[i] = (unsigned char)(r & 0xFFUL);
		r >>= 8;
		rb--;
	}
	b[6] = (unsigned char)((b[6] & 0x0F) | 0x40);   /* version 4 */
	b[8] = (unsigned char)((b[8] & 0x3F) | 0x80);   /* RFC 4122 variant */
	p = 0;
	for (i = 0; i < 16; i++) {
		if (i == 4 || i == 6 || i == 8 || i == 10) out[p++] = '-';
		out[p++] = hex[(b[i] >> 4) & 0x0F];
		out[p++] = hex[b[i] & 0x0F];
	}
	out[p] = '\0';
	return JS_NewString(ctx, out);
}

/* fixes843b (#167 S1 census) — native-side visibility into the fetch()
 * shim (macsurf_qjs.c has no real XMLHttpRequest, so fetch() always
 * synchronously resolves to {ok:false,status:0} -- see the shim below).
 * Called from JS right after the try/catch so we can see, per call, the
 * URL requested and what the shim actually returned, WITHOUT needing a
 * real network path to exist yet. WORK-gated so it survives the
 * failures-only release filter; remove once S3 (native XHR) lands and
 * this census question is answered for good. */
static JSValue qjs_work_log_fetch(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv)
{
	const char *url = (argc > 0) ? JS_ToCString(ctx, argv[0]) : NULL;
	int ok = (argc > 1) ? JS_ToBool(ctx, argv[1]) : 0;
	int32_t status = 0;
	(void)this_val;
	if (argc > 2) JS_ToInt32(ctx, &status, argv[2]);
	macsurf_debug_log_writef("WORK fetch url=%s ok=%d status=%d",
			url ? url : "(null)", ok, (int)status);
	if (url) JS_FreeCString(ctx, url);
	return JS_UNDEFINED;
}

/* fixes845 (#167 S1 census cont'd) — a census round that only instruments
 * the fetch() shim produced ZERO "WORK fetch" lines against real Facebook
 * hardware traffic, home feed included. The fetch() shim's own internal
 * "new XMLHttpRequest()" throws (no real XHR global exists), silently
 * caught -- but that means production JS calling XMLHttpRequest DIRECTLY
 * (a very common pattern, often preferred over fetch() for compatibility)
 * never touches the fetch shim OR its logging at all; the throw happens
 * wherever the caller's own code is, invisible to the fetch-level census.
 * This is XHR-level visibility instead: every construction/open/send is
 * logged regardless of which API surface the calling JS actually uses. */
static JSValue qjs_work_log_xhr(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv)
{
	const char *event = (argc > 0) ? JS_ToCString(ctx, argv[0]) : NULL;
	const char *method = (argc > 1) ? JS_ToCString(ctx, argv[1]) : NULL;
	const char *url = (argc > 2) ? JS_ToCString(ctx, argv[2]) : NULL;
	(void)this_val;
	macsurf_debug_log_writef("WORK xhr event=%s method=%s url=%s",
			event ? event : "?", method ? method : "",
			url ? url : "");
	if (event) JS_FreeCString(ctx, event);
	if (method) JS_FreeCString(ctx, method);
	if (url) JS_FreeCString(ctx, url);
	return JS_UNDEFINED;
}

static void register_browser_globals(JSContext *ctx)
{
	JSValue global = JS_GetGlobalObject(ctx);
	JSValue console;
	JSValue location_obj;
	JSValue history_obj;
	JSValue nav_obj;
	JSValue crypto_obj;

	/* window / self / globalThis aliases — scripts check 'typeof window' */
	JS_SetPropertyStr(ctx, global, "window",     JS_DupValue(ctx, global));
	JS_SetPropertyStr(ctx, global, "self",       JS_DupValue(ctx, global));
	JS_SetPropertyStr(ctx, global, "globalThis", JS_DupValue(ctx, global));

	/* --- console --- */
	console = JS_NewObject(ctx);
	qjs_set_func(ctx, console, "log",   qjs_console_log,   1);
	qjs_set_func(ctx, console, "warn",  qjs_console_warn,  1);
	qjs_set_func(ctx, console, "error", qjs_console_error, 1);
	qjs_set_func(ctx, console, "info",  qjs_console_info,  1);
	qjs_set_func(ctx, console, "debug", qjs_console_debug, 1);
	JS_SetPropertyStr(ctx, global, "console", console);

	/* fixes843b (#167 S1 census) — see qjs_work_log_fetch's comment. */
	qjs_set_func(ctx, global, "__workLogFetch", qjs_work_log_fetch, 3);
	/* fixes845 — see qjs_work_log_xhr's comment. */
	qjs_set_func(ctx, global, "__workLogXHR", qjs_work_log_xhr, 3);

	/* fixes846 (#167 S3) — native XHR/fetch backend over fetch_start().
	 * See macos9_js_fetch.c for the full design. */
	macos9_js_fetch_install(ctx, global);

	/* --- crypto (getRandomValues / randomUUID) — fixes717 --- */
	crypto_obj = JS_NewObject(ctx);
	qjs_set_func(ctx, crypto_obj, "getRandomValues",
		qjs_crypto_get_random_values, 1);
	qjs_set_func(ctx, crypto_obj, "randomUUID", qjs_crypto_random_uuid, 0);
	JS_SetPropertyStr(ctx, global, "crypto", crypto_obj);

	/* --- monotonic clock for performance.now() --- */
	qjs_set_func(ctx, global, "__macsurf_monotonic_ms", qjs_monotonic_ms, 0);

	/* --- alert / confirm / prompt --- */
	qjs_set_func(ctx, global, "alert",   qjs_alert,   1);
	qjs_set_func(ctx, global, "confirm", qjs_confirm, 1);
	qjs_set_func(ctx, global, "prompt",  qjs_prompt,  2);

	/* --- timers --- */
	qjs_set_func(ctx, global, "setTimeout",    qjs_settimeout,  2);
	qjs_set_func(ctx, global, "setInterval",   qjs_setinterval, 2);
	qjs_set_func(ctx, global, "clearTimeout",  qjs_cleartimeout, 1);
	qjs_set_func(ctx, global, "clearInterval", qjs_cleartimeout, 1);

	/* --- requestAnimationFrame via setTimeout(fn, 16) --- */
	/* fixes876 — pass the DOMHighResTimeStamp every rAF callback expects.
	 * Previously `setTimeout(fn,16)` fired fn with NO arguments, so the
	 * near-universal `function(t){ var dt = t - last; ... }` idiom saw
	 * `undefined`, made `dt` NaN, and every animation loop driven off a delta
	 * silently did nothing (or jumped).
	 *
	 * The timestamp MUST be read inside the callback, at FIRE time. Passing it
	 * as a setTimeout extra arg would freeze it at REGISTRATION time -- each
	 * frame would report ~16ms stale, which is exactly the sort of
	 * plausible-but-wrong value that is worse than the missing one.
	 *
	 * __macsurf_monotonic_ms is the native binding installed above, so this
	 * does not depend on `performance` (defined in a later eval block). */
	macsurf_qjs__safe_eval(ctx,
		"function requestAnimationFrame(fn){"
			"return setTimeout(function(){"
				"fn(__macsurf_monotonic_ms());"
			"},16);"
		"}"
		"function cancelAnimationFrame(id){"
			"clearTimeout(id);"
		"}");

	/* --- window.location --- */
	location_obj = JS_NewObject(ctx);
	{
		JSValue getter = JS_NewCFunction(ctx, qjs_location_get, "get", 0);
		JSValue setter = JS_NewCFunction(ctx, qjs_location_set, "set", 1);
		JSAtom href_atom = JS_NewAtom(ctx, "href");
		JS_DefinePropertyGetSet(ctx, location_obj, href_atom,
				getter, setter, JS_PROP_CONFIGURABLE);
		JS_FreeAtom(ctx, href_atom);
	}
	qjs_set_func(ctx, location_obj, "reload", qjs_location_reload, 0);
	JS_SetPropertyStr(ctx, global, "location", location_obj);

	/* Extended location properties in JS */
	macsurf_qjs__safe_eval(ctx,
		"(function(){"
		"if(typeof location==='undefined')return;"
		"function parse(){"
		"var s=location.href||'';"
		"var p={protocol:'',host:'',hostname:'',port:'',"
		"       pathname:'',search:'',hash:'',origin:''};"
		"var hi=s.indexOf('#');"
		"if(hi>=0){p.hash=s.substring(hi);s=s.substring(0,hi);}"
		"var qi=s.indexOf('?');"
		"if(qi>=0){p.search=s.substring(qi);s=s.substring(0,qi);}"
		"var ci=s.indexOf('://');"
		"var rest=s;"
		"if(ci>=0){p.protocol=s.substring(0,ci)+':';rest=s.substring(ci+3);}"
		"var pi=rest.indexOf('/');"
		"var authority=pi>=0?rest.substring(0,pi):rest;"
		"p.pathname=pi>=0?rest.substring(pi):'/';"
		"p.host=authority;"
		"var coli=authority.indexOf(':');"
		"if(coli>=0){p.hostname=authority.substring(0,coli);"
		"  p.port=authority.substring(coli+1);}"
		"else{p.hostname=authority;}"
		"p.origin=p.protocol+'//'+p.host;"
		"return p;"
		"}"
		"['protocol','host','hostname','port','pathname','search',"
		" 'hash','origin'].forEach(function(k){"
		"  Object.defineProperty(location,k,{"
		"    get:function(){return parse()[k];},"
		"    configurable:true"
		"  });"
		"});"
		"location.assign=location.assign||function(u){location.href=u;};"
		"location.replace=location.replace||function(u){location.href=u;};"
		"location.toString=location.toString||function(){return location.href;};"
		"})();");

	/* --- Event constructors --- */
	macsurf_qjs__safe_eval(ctx,
		"function Event(type,opts){"
			"opts=opts||{};"
			"this.type=String(type);"
			"this.bubbles=!!opts.bubbles;"
			"this.cancelable=!!opts.cancelable;"
			"this.composed=!!opts.composed;"
			"this.defaultPrevented=false;"
			"this.target=null;this.currentTarget=null;"
			"this.preventDefault=function(){this.defaultPrevented=true;};"
			"this.stopPropagation=function(){};"
			"this.stopImmediatePropagation=function(){};"
		"}"
		"this.Event=Event;"
		"function CustomEvent(type,opts){"
			"Event.call(this,type,opts);"
			"this.detail=opts&&opts.detail!==undefined?opts.detail:null;"
		"}"
		"CustomEvent.prototype=Object.create(Event.prototype);"
		"this.CustomEvent=CustomEvent;"
		"function MouseEvent(type,opts){Event.call(this,type,opts);"
			"opts=opts||{};"
			"this.clientX=opts.clientX||0;this.clientY=opts.clientY||0;"
			"this.screenX=opts.screenX||0;this.screenY=opts.screenY||0;"
			"this.button=opts.button||0;"
			"this.shiftKey=!!opts.shiftKey;"
			"this.ctrlKey=!!opts.ctrlKey;this.altKey=!!opts.altKey;"
			"this.metaKey=!!opts.metaKey;"
		"}"
		"MouseEvent.prototype=Object.create(Event.prototype);"
		"this.MouseEvent=MouseEvent;"
		"function KeyboardEvent(type,opts){Event.call(this,type,opts);"
			"opts=opts||{};"
			"this.key=opts.key||'';this.code=opts.code||'';"
			"this.keyCode=opts.keyCode||0;this.which=opts.which||0;"
			"this.shiftKey=!!opts.shiftKey;this.ctrlKey=!!opts.ctrlKey;"
			"this.altKey=!!opts.altKey;this.metaKey=!!opts.metaKey;"
		"}"
		"KeyboardEvent.prototype=Object.create(Event.prototype);"
		"this.KeyboardEvent=KeyboardEvent;");

	/* --- document shims --- */
	macsurf_qjs__safe_eval(ctx,
		"if(typeof document!=='undefined'){"
			"document.body=document.body||null;"
			"document.head=document.head||null;"
			"document.documentElement=document.documentElement||null;"
			/* fixes855 (#284) — document.nodeType MUST be 9
			 * (DOCUMENT_NODE).  Elements get nodeType 1, text 3 and
			 * fragments 11 (qjs_wrap_*), but the document itself never got
			 * one, so `document.nodeType` read `undefined`.  jQuery's
			 * setDocument is:
			 *   function V(e){var t,n=e?e.ownerDocument||e:ye;
			 *     return n!=T && 9===n.nodeType && n.documentElement &&
			 *            (r=(T=n).documentElement, ...);}
			 * `9===undefined` is false, so it short-circuits and T -- the
			 * document handle every later `T.createElement(...)` support
			 * probe uses -- is NEVER assigned.  jQuery then dies on its own
			 * first assert with "TypeError: cannot read property
			 * 'createElement' of undefined", taking every dependent bundle
			 * with it ("ReferenceError: jQuery is not defined").
			 * HW-observed on hackaday.com (the jquery.min.js+jquery-migrate
			 * _static bundle) and it is the same first link in the XenForo
			 * preamble->Sizzle->core-compiled cascade on 68kmla /
			 * tinkerdifferent.  documentElement is already a getter that
			 * always returns a node, so nodeType was the only miss. */
			"document.nodeType=9;"
			"document.nodeName='#document';"
			"document.readyState='complete';"
			/* fixes879 — `document.cookie=''` used to live here as a plain
			 * data property: writes stuck to the string for the session,
			 * reached no jar, and every navigation started empty. It is now a
			 * real accessor pair over urldb, installed by qjs_dom_install once
			 * a document (and therefore a URL to key the jar by) exists. This
			 * fallback keeps the property defined for the pre-document window,
			 * where there is no URL and no correct answer but '' -- but it must
			 * NOT clobber the real accessor, so only define it if absent. */
			"if(!('cookie' in document))document.cookie='';"
			"document.URL=document.URL||(typeof location!=='undefined'?location.href:'');"
			"document.referrer='';"
			"document.domain='';"
			/* fixes846 (#167 S3) — real native-backed nodes, same
			 * native-fast-path/pre-document-fallback shape as createElement
			 * (qjs_dom_install installs __createTextNodeNative /
			 * __createDocumentFragmentNative once a real document is
			 * wired; createDocumentFragment itself is defined there too,
			 * so it is NOT redefined here -- an earlier unconditional
			 * override at this exact spot was dead code, always clobbered
			 * by qjs_dom_install running right after this function, but
			 * misleadingly suggested a no-op fragment was live; removed). */
			"document.createTextNode=document.createTextNode||function(t){"
				"var n=document.__createTextNodeNative?"
					"document.__createTextNodeNative(String(t)):null;"
				"if(n)return n;"
				"return {nodeValue:String(t),textContent:String(t),"
					"appendChild:function(){return null;},data:String(t)};};"
			/* fixes873 (#301) — getElementsByTagName/ClassName were stubs that
			 * returned [] FOREVER. That is what makes webpack's runtime throw
			 * "Automatic publicPath is not supported in this browser" and take
			 * the whole bundle down with it:
			 *     if (!e && t && (t.currentScript && ... && (e=t.currentScript.src), !e)) {
			 *       var n = t.getElementsByTagName("script");
			 *       if (n.length) for (var o=n.length-1; o>-1 && (!e||!/^http(s?):/.test(e));)
			 *         e = n[o--].src
			 *     }
			 *     if (!e) throw new Error("Automatic publicPath is not supported...")
			 * With n.length===0 the loop never runs, e stays undefined, and every
			 * webpack-built bundle dies before its first line of real code.
			 *
			 * querySelectorAll now does real compound matching (fixes871), and a
			 * bare tag / bare .class IS a compound selector -- so these are the
			 * same query. Delegating keeps ONE matcher instead of a second
			 * subtly-different walker ('*' included, since the matcher handles
			 * it). The live-HTMLCollection semantics of the real DOM are not
			 * reproduced; a static array is what every caller here actually
			 * uses. */
			"document.getElementsByTagName=function(t){"
				"return document.querySelectorAll(String(t));};"
			"document.getElementsByClassName=function(c){"
				"return document.querySelectorAll('.'+String(c).split(/\\s+/)"
					".filter(function(x){return !!x;}).join('.'));};"
			"document.getElementsByName=document.getElementsByName||"
				"function(){return [];};"
			"document.querySelectorAll=document.querySelectorAll||"
				"function(){return [];};"
			"document.addEventListener=document.addEventListener||"
				"function(t,fn){"
					"if(!document._listeners)document._listeners={};"
					"if(!document._listeners[t])document._listeners[t]=[];"
					"document._listeners[t].push(fn);};"
			"document.removeEventListener=document.removeEventListener||"
				"function(t,fn){"
					"var L=document._listeners&&document._listeners[t];if(!L)return;"
					"for(var i=0;i<L.length;i++)if(L[i]===fn){L.splice(i,1);return;}};"
			"document.dispatchEvent=document.dispatchEvent||"
				"function(ev){"
					"var L=document._listeners&&document._listeners[ev&&ev.type];"
					"if(L)L.forEach(function(f){try{f(ev);}catch(e){}});return true;};"
		"}");

	/* fixes879 — the "navigator extended shims" block that used to sit HERE was
	 * DEAD CODE, every line of it. It is guarded by
	 *     if (typeof navigator !== 'undefined') { ... }
	 * but `navigator` is not created until the JS_NewObject/JS_SetPropertyStr
	 * pair further down this same function, so the guard was always false and
	 * the whole block was skipped -- silently, since a skipped if is not an
	 * error. Probed at runtime under the harness: cookieEnabled, onLine and
	 * vendor all read back `undefined`, not the values written here.
	 *
	 * That is also why cookieEnabled was never actually `false` as the audit
	 * recorded -- it simply never existed. Same practical result (undefined is
	 * falsy, so sites concluded "cookies disabled"), different cause, and it
	 * would have defeated the fix if taken at face value.
	 *
	 * Moved verbatim to just after navigator is installed. See below. */

	/* --- Observers --- *
	 * MutationObserver / ResizeObserver / PerformanceObserver stay no-ops
	 * (firing them risks feedback loops with our own reconvert/relayout).
	 * IntersectionObserver is DIFFERENT and must actually fire: modern
	 * feeds (Facebook) gate their content load on it -- they observe the
	 * feed container and only request/reveal content when the observer
	 * reports it intersecting the viewport. A no-op observer means the
	 * "you're visible, load now" signal never arrives, so the feed JS runs
	 * (confirmed on hardware: ~550KB executed) but issues ZERO fetch/XHR
	 * and never hydrates. fixes853 (#167): give IntersectionObserver a
	 * real-enough implementation -- observe() asynchronously delivers a
	 * single isIntersecting=true entry for the target (a pragmatic
	 * "visible on layout" first cut; geometry-accurate viewport testing is
	 * a later refinement), which is the trigger that lets the feed request
	 * its data through the now-real fetch/XHR (fixes846). Fires via the
	 * real timer arena (setTimeout), asynchronously, exactly as a browser
	 * delivers observer records. */
	macsurf_qjs__safe_eval(ctx,
		"function _Observer(cb){this._cb=cb;}"
		"_Observer.prototype.observe=function(){};"
		"_Observer.prototype.unobserve=function(){};"
		"_Observer.prototype.disconnect=function(){};"
		"_Observer.prototype.takeRecords=function(){return [];};"
		"this.MutationObserver=_Observer;"
		"this.ResizeObserver=_Observer;"
		"this.PerformanceObserver=_Observer;");
	macsurf_qjs__safe_eval(ctx,
		"function IntersectionObserver(cb,opts){"
			"this._cb=cb;this._opts=opts||{};this._targets=[];"
			"this.root=(opts&&opts.root)||null;"
			"this.rootMargin=(opts&&opts.rootMargin)||'0px';"
			"this.thresholds=[0];"
		"}"
		"IntersectionObserver.prototype.observe=function(el){"
			"if(!el)return;"
			"this._targets.push(el);"
			"var self=this;"
			"setTimeout(function(){"
				"if(self._targets.indexOf(el)<0)return;"
				"var r=(el.getBoundingClientRect&&el.getBoundingClientRect())||"
					"{top:0,left:0,bottom:0,right:0,width:0,height:0,x:0,y:0};"
				"var entry={target:el,isIntersecting:true,"
					"intersectionRatio:1,boundingClientRect:r,"
					"intersectionRect:r,rootBounds:null,"
					"time:(typeof performance!=='undefined'&&performance.now)?"
						"performance.now():0};"
				"try{self._cb([entry],self);}catch(e){}"
			"},0);"
		"};"
		"IntersectionObserver.prototype.unobserve=function(el){"
			"var i=this._targets.indexOf(el);if(i>=0)this._targets.splice(i,1);"
		"};"
		"IntersectionObserver.prototype.disconnect=function(){this._targets=[];};"
		"IntersectionObserver.prototype.takeRecords=function(){return [];};"
		"this.IntersectionObserver=IntersectionObserver;");

	/* --- window event helpers, scroll, getComputedStyle, matchMedia --- */
	macsurf_qjs__safe_eval(ctx,
		"this.scrollTo=function(){};"
		"this.scrollBy=function(){};"
		"this.scroll=function(){};"
		"this._winListeners={};"
		"this.addEventListener=function(t,fn){"
			"if(!this._winListeners[t])this._winListeners[t]=[];"
			"this._winListeners[t].push(fn);};"
		"this.removeEventListener=function(t,fn){"
			"var arr=this._winListeners[t];if(!arr)return;"
			"for(var i=0;i<arr.length;i++)if(arr[i]===fn){arr.splice(i,1);return;}};"
		/* fixes863 (#289 probe) — this used to be a bare
		 *     if(arr)arr.forEach(function(f){try{f(ev);}catch(e){}});
		 * so a window listener that THREW was swallowed in total silence,
		 * and js_fire_dom_ready wraps the whole dispatch in a second bare
		 * catch(e){} on top.  Two layers of silence over the exact spot
		 * hackaday's comment iframe lives: its dynamic-loader.js puts its
		 * ENTIRE body inside window.addEventListener('DOMContentLoaded',...)
		 * -> querySelector('#commentform') -> IntersectionObserver ->
		 * loadScript -> fetch.  We have proven the listener is registered
		 * BEFORE the event fires ([11121] loader runs, [11123] domready), that
		 * the iframe has its own heap (pump heaps=2) and that it IS pumped
		 * (fixes861) -- yet fetch never runs.  So either the handler throws
		 * (invisible until now) or it runs and silently does nothing.
		 * `n=` separates those: n=0 means the loader never registered here at
		 * all; n>=1 with no THREW line means it ran clean and bailed on its
		 * own `if(e)` -- i.e. querySelector('#commentform') returned null.
		 * console.error routes to MS_LOG, and the "WORK " in the text is what
		 * gets it past the failures-only gate. */
		"this.dispatchEvent=function(ev){"
			"var t=ev&&ev.type;var arr=t&&this._winListeners[t];"
			"var n=arr?arr.length:0;"
			"try{console.error('WORK winevt type='+t+' n='+n);}catch(_){}"
			"if(arr)arr.forEach(function(f){try{f(ev);}catch(e){"
				"try{console.error('WORK winevt THREW type='+t+': '+"
					"((e&&e.message)||e));}catch(_){}"
			"}});return true;};"
		"this.getComputedStyle=function(el){"
			"return {"
				"getPropertyValue:function(p){"
					"if(el&&el.style&&el.style.getPropertyValue)"
						"return el.style.getPropertyValue(p);"
					"return '';},"
				"cssText:''"
			"};};"
		"this.matchMedia=function(q){"
			"return {matches:false,media:q||'',"
				"addListener:function(){},removeListener:function(){},"
				"addEventListener:function(){},removeEventListener:function(){}};};"
		"this.requestIdleCallback=function(fn){return setTimeout(fn,0);};"
		"this.cancelIdleCallback=function(id){clearTimeout(id);};"
		"this.innerWidth=949;this.innerHeight=613;"
		"this.outerWidth=949;this.outerHeight=613;"
		"this.scrollY=0;this.scrollX=0;"
		"this.pageYOffset=0;this.pageXOffset=0;"
		"this.devicePixelRatio=1;"
		"this.screen={width:1024,height:768,availWidth:1024,availHeight:740,colorDepth:24};"
		"this.performance={"
			"now:function(){return __macsurf_monotonic_ms();},"
			"getEntriesByType:function(){return [];},"
			"getEntries:function(){return [];},"
			"getEntriesByName:function(){return [];},"
			"mark:function(){},measure:function(){},"
			"clearMarks:function(){},clearMeasures:function(){},"
			"clearResourceTimings:function(){},"
			"setResourceTimingBufferSize:function(){},"
			"timeOrigin:__macsurf_monotonic_ms(),"
			"timing:{navigationStart:__macsurf_monotonic_ms()},"
			"navigation:{type:0,redirectCount:0}};"
		"this.Promise=this.Promise||function(executor){"
			"var self=this;self._then=[];self._catch=[];"
			"self.then=function(cb){self._then.push(cb);return self;};"
			"self.catch=function(cb){self._catch.push(cb);return self;};"
			"function resolve(v){self._then.forEach(function(f){try{f(v);}catch(e){}});}"
			"function reject(e){self._catch.forEach(function(f){try{f(e);}catch(_){}});}"
			"try{executor(resolve,reject);}catch(e){reject(e);}"
		"};");

	/* --- atob / btoa (pure JS, QuickJS port may not have them) --- */
	macsurf_qjs__safe_eval(ctx,
		"(function(g){"
		"var B64='ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';"
		"if(typeof g.btoa!=='function'){"
		"g.btoa=function(s){"
		"var b='',i,c0,c1,c2,bl=s.length;"
		"for(i=0;i<bl;i+=3){"
		"c0=s.charCodeAt(i)&0xff;"
		"c1=i+1<bl?s.charCodeAt(i+1)&0xff:0;"
		"c2=i+2<bl?s.charCodeAt(i+2)&0xff:0;"
		"b+=B64[c0>>2]+B64[((c0&3)<<4)|(c1>>4)]+B64[((c1&15)<<2)|(c2>>6)]+B64[c2&63];"
		"}"
		"if(bl%3===1)b=b.slice(0,-2)+'==';"
		"else if(bl%3===2)b=b.slice(0,-1)+'=';"
		"return b;"
		"};"
		"}"
		"if(typeof g.atob!=='function'){"
		"g.atob=function(s){"
		"s=s.replace(/=/g,'').replace(/\\s/g,'');"
		"var r='',i,n=s.length,b,t;"
		"for(i=0;i<n;i+=4){"
		"b=B64.indexOf(s[i])<<18|B64.indexOf(s[i+1]||'A')<<12|"
		"  B64.indexOf(s[i+2]||'A')<<6|B64.indexOf(s[i+3]||'A');"
		"t=(b>>16)&0xff;r+=String.fromCharCode(t);"
		"if(i+2<n)r+=String.fromCharCode((b>>8)&0xff);"
		"if(i+3<n)r+=String.fromCharCode(b&0xff);"
		"}"
		"return r;"
		"};"
		"}"
		"})(this);");

	/* --- DOMParser (fixes873, #302) ---
	 * This used to hand back a FAKE document with `documentElement: null` and
	 * querySelector hardcoded to null. Any caller doing the ordinary thing --
	 *     const t = new DOMParser().parseFromString(s, "text/html");
	 *     return "" === t.documentElement.textContent.trim() && !t.querySelector("img")
	 * (verbum-comments.js:1:54379, its "is this comment empty" check) -- throws
	 * "cannot read property 'textContent' of null" instead. That one runs at
	 * MODULE INIT, inside a Preact computed signal, so it kills the bundle on
	 * every load, not just on submit.
	 *
	 * A real parse is cheap now: `innerHTML =` became genuinely real in fixes846
	 * (dom_hubbub_fragment_parser_create), so parsing into a detached <html>
	 * element gives a real subtree with real textContent and real element-scoped
	 * querySelector -- no new native code, no second parser.
	 *
	 * Not a full Document: no getElementById index, no live collections. It is a
	 * DOM subtree wearing a document-shaped hat, which is what parseFromString
	 * callers actually touch. Everything is delegated to the real root element so
	 * there is nothing here that can drift from the real DOM's behaviour. */
	macsurf_qjs__safe_eval(ctx,
		"function DOMParser(){}"
		"DOMParser.prototype.parseFromString=function(s,m){"
			"var root=null;"
			"try{root=document.createElement('html');"
			"root.innerHTML=String(s==null?'':s);}catch(e){}"
			"if(!root)return {body:{innerHTML:s||''},documentElement:null,"
				"querySelector:function(){return null;},"
				"querySelectorAll:function(){return [];},"
				"getElementById:function(){return null;},"
				"getElementsByTagName:function(){return [];}};"
			"return {documentElement:root,body:root,head:null,nodeType:9,"
				"querySelector:function(x){"
					"try{return root.querySelector?root.querySelector(x):null;}"
					"catch(e){return null;}},"
				"querySelectorAll:function(x){"
					"try{return root.querySelectorAll?root.querySelectorAll(x):[];}"
					"catch(e){return [];}},"
				"getElementById:function(x){"
					"try{return root.querySelector?root.querySelector('#'+x):null;}"
					"catch(e){return null;}},"
				"getElementsByTagName:function(x){"
					"try{return root.querySelectorAll?"
						"root.querySelectorAll(String(x)):[];}"
					"catch(e){return [];}}};"
		"};");

	/* --- FormData --- */
	macsurf_qjs__safe_eval(ctx,
		"function FormData(form){"
			"this._k=[];this._v=[];"
			"if(form&&form.elements){"
				"var els=form.elements;"
				"for(var i=0;i<els.length;i++){"
					"var e=els[i];"
					"if(e.name&&e.value!==undefined){"
						"this._k.push(e.name);"
						"this._v.push(e.value);"
					"}"
				"}"
			"}"
		"}"
		"FormData.prototype.append=function(k,v){"
			"this._k.push(String(k));this._v.push(String(v));};"
		"FormData.prototype.get=function(k){"
			"for(var i=0;i<this._k.length;i++)"
				"if(this._k[i]==k)return this._v[i];"
			"return null;};"
		"FormData.prototype.getAll=function(k){"
			"var r=[];for(var i=0;i<this._k.length;i++)"
				"if(this._k[i]==k)r.push(this._v[i]);return r;};"
		"FormData.prototype.set=function(k,v){"
			"this.delete(k);this.append(k,v);};"
		"FormData.prototype.has=function(k){return this.get(k)!==null;};"
		"FormData.prototype.delete=function(k){"
			"for(var i=this._k.length-1;i>=0;i--)"
				"if(this._k[i]==k){this._k.splice(i,1);this._v.splice(i,1);}};"
		"FormData.prototype.forEach=function(cb){"
			"for(var i=0;i<this._k.length;i++)cb(this._v[i],this._k[i],this);};");

	/* --- XMLHttpRequest (fixes846, #167 S3) --- *
	 * REAL, async, backed by macos9_js_fetch.c's native slot arena over
	 * fetch_start() -- the S1 census (fixes843b/845) proved real Facebook
	 * JS never received a single byte of real response data through the
	 * old shim (fixes845), which only ever logged the attempt and failed
	 * safe with status 0. send() now hands off to __xhrNativeSend(), which
	 * starts a real fetch and returns a slot id; the C side calls
	 * __onNativeComplete() (below) once the response is in, from a
	 * macos9_schedule()-deferred tick -- never synchronously from send()
	 * itself, so this matches every other async completion in the engine
	 * (setTimeout, the reconvert debounce). A synchronous open(...,false)
	 * is accepted (per spec) but still delivered asynchronously -- true
	 * blocking XHR would need a nested pump loop this cooperative
	 * scheduler doesn't have, and no real site actually requires it work
	 * to receive data, only that it doesn't hang or throw. */
	macsurf_qjs__safe_eval(ctx,
		"function XMLHttpRequest(){"
			"this.readyState=0;this.status=0;this.statusText='';"
			"this.responseText='';this.response='';this.responseType='';"
			"this.responseURL='';"
			"this._method='GET';this._url='';this._reqHeaders=[];"
			"this._slotId=-1;this._listeners={};"
			"if(typeof __workLogXHR==='function')__workLogXHR('new','','');"
		"}"
		"XMLHttpRequest.UNSENT=0;XMLHttpRequest.OPENED=1;"
		"XMLHttpRequest.HEADERS_RECEIVED=2;XMLHttpRequest.LOADING=3;"
		"XMLHttpRequest.DONE=4;"
		"XMLHttpRequest.prototype.open=function(method,url,async){"
			"this._method=String(method||'GET');this._url=String(url||'');"
			"this._async=(async===false)?false:true;"
			"this.readyState=1;"
			"if(typeof __workLogXHR==='function')"
				"__workLogXHR('open',this._method,this._url);"
		"};"
		"XMLHttpRequest.prototype.setRequestHeader=function(k,v){"
			"this._reqHeaders.push(String(k)+': '+String(v));};"
		"XMLHttpRequest.prototype.getAllResponseHeaders=function(){"
			"return this.__responseHeadersRaw||'';};"
		"XMLHttpRequest.prototype.getResponseHeader=function(name){"
			"var raw=this.__responseHeadersRaw||'',lines=raw.split(/\\r\\n|\\n/),i;"
			"name=String(name).toLowerCase();"
			"for(i=0;i<lines.length;i++){"
				"var c=lines[i].indexOf(':');if(c<0)continue;"
				"if(lines[i].slice(0,c).trim().toLowerCase()===name)"
					"return lines[i].slice(c+1).trim();"
			"}"
			"return null;"
		"};"
		"XMLHttpRequest.prototype.overrideMimeType=function(){};"
		"XMLHttpRequest.prototype.abort=function(){"
			"if(this._slotId>=0&&typeof __xhrNativeAbort==='function')"
				"__xhrNativeAbort(this._slotId);"
			"this._slotId=-1;this.readyState=0;this.status=0;"
		"};"
		"XMLHttpRequest.prototype.addEventListener=function(type,fn){"
			"if(!this._listeners[type])this._listeners[type]=[];"
			"this._listeners[type].push(fn);"
		"};"
		"XMLHttpRequest.prototype.removeEventListener=function(type,fn){"
			"var a=this._listeners[type];if(!a)return;"
			"var i=a.indexOf(fn);if(i>=0)a.splice(i,1);"
		"};"
		"XMLHttpRequest.prototype._fire=function(type){"
			"var t='on'+type;"
			"if(typeof this[t]==='function'){try{this[t]();}catch(e){}}"
			"var a=this._listeners[type];"
			"if(a)for(var i=0;i<a.length;i++){try{a[i]();}catch(e){}}"
		"};"
		"XMLHttpRequest.prototype.__onNativeComplete=function(){"
			"var ok=this.status>=200&&this.status<300;"
			"if(typeof __workLogXHR==='function')"
				"__workLogXHR('complete',this._method,this._url);"
			"this._fire('readystatechange');"
			"if(this.status===0){this._fire('error');}"
			"else{this._fire('load');}"
			"this._fire('loadend');"
		"};"
		"XMLHttpRequest.prototype.send=function(body){"
			"if(typeof __workLogXHR==='function')"
				"__workLogXHR('send',this._method,this._url);"
			"if(typeof __xhrNativeSend!=='function'){"
				"this.readyState=4;this.status=0;this._fire('readystatechange');"
				"this._fire('error');this._fire('loadend');return;"
			"}"
			"this._slotId=__xhrNativeSend(this,this._method,this._url,"
				"(body===undefined)?null:body,this._reqHeaders,this._async);"
			"if(this._slotId<0){"
				"var self=this;"
				"setTimeout(function(){"
					"self.readyState=4;self.status=0;"
					"self._fire('readystatechange');self._fire('error');"
					"self._fire('loadend');"
				"},0);"
			"}"
		"};"
		"this.XMLHttpRequest=XMLHttpRequest;");

	/* --- fetch() (fixes846) --- *
	 * A real Promise (QuickJS's native Promise is already an intrinsic --
	 * JS_AddIntrinsicPromise, see qjs_build_context) wrapping the real
	 * async XHR above. Replaces the fake synchronous thenable that always
	 * resolved {ok:false,status:0} regardless of what happened. */
	macsurf_qjs__safe_eval(ctx,
		"this.fetch=function(url,opts){"
			"opts=opts||{};"
			"return new Promise(function(resolve,reject){"
				"try{"
					"var xhr=new XMLHttpRequest();"
					"xhr.open(opts.method||'GET',url,true);"
					"if(opts.headers){"
						"for(var h in opts.headers)"
							"xhr.setRequestHeader(h,opts.headers[h]);"
					"}"
					"xhr.onreadystatechange=function(){"
						"if(xhr.readyState!==4)return;"
						"var ok=xhr.status>=200&&xhr.status<300;"
						"if(typeof __workLogFetch==='function')"
							"__workLogFetch(String(url),ok,xhr.status);"
						"if(xhr.status===0){reject(new Error('Network error'));return;}"
						"var respText=xhr.responseText||'';"
						"var resp={"
							"ok:ok,status:xhr.status,statusText:xhr.statusText||'',"
							"url:xhr.responseURL||String(url),"
							"headers:{"
								"get:function(n){return xhr.getResponseHeader(n);}"
							"},"
							"text:function(){return Promise.resolve(respText);},"
							"json:function(){"
								"try{return Promise.resolve(JSON.parse(respText));}"
								"catch(e){return Promise.reject(e);}"
							"}"
						"};"
						"resolve(resp);"
					"};"
					"xhr.send(opts.body===undefined?null:opts.body);"
				"}catch(e){reject(e);}"
			"});"
		"};");

	/* --- localStorage / sessionStorage --- */
	macsurf_qjs__safe_eval(ctx,
		"function _Storage(){this._m={};}"
		"_Storage.prototype.getItem=function(k){"
			"return k in this._m?this._m[k]:null;};"
		"_Storage.prototype.setItem=function(k,v){this._m[k]=String(v);};"
		"_Storage.prototype.removeItem=function(k){delete this._m[k];};"
		"_Storage.prototype.clear=function(){this._m={};};"
		"_Storage.prototype.key=function(i){"
			"var ks=Object.keys(this._m);return ks[i]||null;};"
		"Object.defineProperty(_Storage.prototype,'length',{"
			"get:function(){return Object.keys(this._m).length;}});"
		"this.localStorage=new _Storage();"
		"this.sessionStorage=new _Storage();");

	/* --- URL / URLSearchParams --- */
	macsurf_qjs__safe_eval(ctx,
		"(function(){"
		"function _parseURL(u){"
			"var s=String(u);"
			"var m=s.match(/^([a-z][a-z0-9+\\-.]*):/i);"
			"var proto=m?m[1]+':':'';"
			"var rest=m?s.substr(m[0].length):s;"
			"var hash='';var search='';var host='';var path='';"
			"var h=rest.indexOf('#');"
			"if(h>=0){hash=rest.substr(h);rest=rest.substr(0,h);}"
			"var q=rest.indexOf('?');"
			"if(q>=0){search=rest.substr(q);rest=rest.substr(0,q);}"
			"if(rest.indexOf('//')==0){"
				"rest=rest.substr(2);"
				"var p=rest.indexOf('/');"
				"if(p>=0){host=rest.substr(0,p);path=rest.substr(p);}"
				"else{host=rest;path='';}"
			"}else{path=rest;}"
			"return {protocol:proto,host:host,pathname:path,"
				"search:search,hash:hash};"
		"}"
		"function URL(u,base){"
			"var s=String(u);"
			"if(base){"
				"if(s.indexOf('://')<0&&s.charAt(0)!='/'){"
					"var b=_parseURL(base);"
					"s=b.protocol+'//'+b.host+'/'+s;"
				"}else if(s.charAt(0)=='/'){"
					"var b2=_parseURL(base);"
					"s=b2.protocol+'//'+b2.host+s;"
				"}"
			"}"
			"var p=_parseURL(s);"
			"this.href=s;this.protocol=p.protocol;this.host=p.host;"
			"this.hostname=p.host.split(':')[0];"
			"this.pathname=p.pathname;this.search=p.search;"
			"this.hash=p.hash;"
		"}"
		"URL.prototype.toString=function(){return this.href;};"
		"this.URL=URL;"
		"function URLSearchParams(init){"
			"this._m={};"
			"if(init){"
				"var s=String(init);"
				"if(s.charAt(0)=='?')s=s.substr(1);"
				"var parts=s.split('&');"
				"for(var i=0;i<parts.length;i++){"
					"if(!parts[i])continue;"
					"var eq=parts[i].indexOf('=');"
					"var k=eq>=0?parts[i].substr(0,eq):parts[i];"
					"var v=eq>=0?parts[i].substr(eq+1):'';"
					"k=decodeURIComponent(k);"
					"v=decodeURIComponent(v);"
					"this._m[k]=v;"
				"}"
			"}"
		"}"
		"URLSearchParams.prototype.get=function(k){"
			"return this._m[k]!==undefined?this._m[k]:null;};"
		"URLSearchParams.prototype.set=function(k,v){this._m[k]=String(v);};"
		"URLSearchParams.prototype.has=function(k){return k in this._m;};"
		"URLSearchParams.prototype.delete=function(k){delete this._m[k];};"
		"URLSearchParams.prototype.toString=function(){"
			"var out=[];"
			"for(var k in this._m){"
				"out.push(encodeURIComponent(k)+'='+encodeURIComponent(this._m[k]));"
			"}return out.join('&');};"
		"this.URLSearchParams=URLSearchParams;"
		"}).call(this);");

	/* --- history --- */
	history_obj = JS_NewObject(ctx);
	qjs_set_func(ctx, history_obj, "back",    qjs_history_back,    0);
	qjs_set_func(ctx, history_obj, "forward", qjs_history_forward, 0);
	qjs_set_func(ctx, history_obj, "go",      qjs_history_go,      1);
	JS_SetPropertyStr(ctx, history_obj, "length", JS_NewInt32(ctx, 0));
	JS_SetPropertyStr(ctx, global, "history", history_obj);

	macsurf_qjs__safe_eval(ctx,
		"(function(){"
		"  if(typeof history==='undefined')return;"
		"  var _state=null;"
		"  history.pushState=history.pushState||function(s,t,u){"
		"    _state=s;"
		"    if(u&&typeof location!=='undefined')location.href=u;"
		"  };"
		"  history.replaceState=history.replaceState||function(s,t,u){"
		"    _state=s;"
		"    if(u&&typeof location!=='undefined')location.href=u;"
		"  };"
		"  Object.defineProperty(history,'state',{"
		"    get:function(){return _state;},"
		"    configurable:true"
		"  });"
		"  history.scrollRestoration=history.scrollRestoration||'auto';"
		"})();");

	/* --- navigator --- */
	nav_obj = JS_NewObject(ctx);
	JS_SetPropertyStr(ctx, nav_obj, "userAgent",
		JS_NewString(ctx, "MacSurf/2.0.5 (Macintosh; PPC Mac OS 9)"));
	JS_SetPropertyStr(ctx, nav_obj, "appVersion",
		JS_NewString(ctx, "5.0 (Macintosh; PPC Mac OS 9)"));
	JS_SetPropertyStr(ctx, nav_obj, "platform",
		JS_NewString(ctx, "MacPPC"));
	JS_SetPropertyStr(ctx, nav_obj, "appName",
		JS_NewString(ctx, "Netscape"));
	JS_SetPropertyStr(ctx, nav_obj, "language",
		JS_NewString(ctx, "en-US"));
	JS_SetPropertyStr(ctx, global, "navigator", nav_obj);

	/* --- navigator extended shims ---
	 * fixes879 — MUST stay after the JS_SetPropertyStr(global,"navigator")
	 * above. This block lived earlier in the function, where its own
	 * `typeof navigator !== 'undefined'` guard was always false because
	 * navigator did not exist yet, so none of it ever ran.
	 *
	 * cookieEnabled is now TRUE. It read `undefined` (falsy) before, which was
	 * accurate while document.cookie was a dead string, but the jar is real,
	 * persistent and now reachable from script -- and sites gate their login
	 * flow on this exact flag, showing "please enable cookies" instead of the
	 * page. Leaving it falsy would waste most of the value of wiring
	 * document.cookie up at all. */
	macsurf_qjs__safe_eval(ctx,
		"if(typeof navigator!=='undefined'){"
			"navigator.cookieEnabled=true;"
			"navigator.onLine=true;"
			"navigator.languages=navigator.languages||[navigator.language||'en-US'];"
			"navigator.doNotTrack='1';"
			"navigator.connection={effectiveType:'3g',downlink:1.5,rtt:300};"
			"navigator.hardwareConcurrency=1;"
			"navigator.deviceMemory=0.5;"
			"navigator.vendor='Anthropic/MPLS';"
			"navigator.product='MacSurf';"
			"navigator.productSub='20260531';"
			"navigator.javaEnabled=function(){return false;};"
			"navigator.sendBeacon=function(){return false;};"
		"}");

	/* --- ES6+ polyfills (Array.from, Set, Map, Image, FB module system) --- */
	macsurf_qjs__safe_eval(ctx,
		"(function(g){"
		"if(!Array.from){"
		"Array.from=function(src,mapFn,thisArg){"
		"var out=[],i,n,v,idx;"
		"if(src==null)return out;"
		"if(typeof src.length==='number'){"
		"n=src.length>>>0;"
		"for(i=0;i<n;i++){v=src[i];out.push(mapFn?mapFn.call(thisArg,v,i):v);}"
		"}else if(typeof src.forEach==='function'){"
		"idx=0;src.forEach(function(v){out.push(mapFn?mapFn.call(thisArg,v,idx):v);idx++;});"
		"}"
		"return out;"
		"};"
		"}"
		"if(!Array.of){Array.of=function(){return Array.prototype.slice.call(arguments);};}"
		"if(!Array.prototype.includes){Array.prototype.includes=function(x){return this.indexOf(x)>=0;};}"
		"if(!Array.prototype.find){Array.prototype.find=function(f,t){var i;for(i=0;i<this.length;i++){if(f.call(t,this[i],i,this))return this[i];}return undefined;};}"
		"if(!Array.prototype.findIndex){Array.prototype.findIndex=function(f,t){var i;for(i=0;i<this.length;i++){if(f.call(t,this[i],i,this))return i;}return -1;};}"
		"if(!Object.assign){"
		"Object.assign=function(target){"
		"var i,k,s;"
		"for(i=1;i<arguments.length;i++){s=arguments[i];if(s==null)continue;for(k in s){if(Object.prototype.hasOwnProperty.call(s,k))target[k]=s[k];}}"
		"return target;"
		"};"
		"}"
		/* QuickJS natively has Set/Map, so guard with typeof */
		"if(typeof g.Set==='undefined'){"
		"var MSet=function(it){this._k=[];this.size=0;var self=this;if(it){if(typeof it.forEach==='function')it.forEach(function(v){self.add(v);});else if(typeof it.length==='number'){var i;for(i=0;i<it.length;i++)self.add(it[i]);}}};"
		"MSet.prototype.add=function(v){if(this._k.indexOf(v)<0){this._k.push(v);this.size=this._k.length;}return this;};"
		"MSet.prototype.has=function(v){return this._k.indexOf(v)>=0;};"
		"MSet.prototype['delete']=function(v){var i=this._k.indexOf(v);if(i<0)return false;this._k.splice(i,1);this.size=this._k.length;return true;};"
		"MSet.prototype.clear=function(){this._k=[];this.size=0;};"
		"MSet.prototype.forEach=function(cb,t){var i;for(i=0;i<this._k.length;i++)cb.call(t,this._k[i],this._k[i],this);};"
		"MSet.prototype.values=function(){return this._k.slice();};"
		"MSet.prototype.keys=MSet.prototype.values;"
		"g.Set=MSet;"
		"if(typeof g.WeakSet==='undefined')g.WeakSet=MSet;"
		"}"
		"if(typeof g.Map==='undefined'){"
		"var MMap=function(it){this._k=[];this._v=[];this.size=0;var self=this;if(it){if(typeof it.forEach==='function')it.forEach(function(p){self.set(p[0],p[1]);});else if(typeof it.length==='number'){var i;for(i=0;i<it.length;i++)self.set(it[i][0],it[i][1]);}}};"
		"MMap.prototype.set=function(k,v){var i=this._k.indexOf(k);if(i<0){this._k.push(k);this._v.push(v);this.size=this._k.length;}else{this._v[i]=v;}return this;};"
		"MMap.prototype.get=function(k){var i=this._k.indexOf(k);return i<0?undefined:this._v[i];};"
		"MMap.prototype.has=function(k){return this._k.indexOf(k)>=0;};"
		"MMap.prototype['delete']=function(k){var i=this._k.indexOf(k);if(i<0)return false;this._k.splice(i,1);this._v.splice(i,1);this.size=this._k.length;return true;};"
		"MMap.prototype.clear=function(){this._k=[];this._v=[];this.size=0;};"
		"MMap.prototype.forEach=function(cb,t){var i;for(i=0;i<this._k.length;i++)cb.call(t,this._v[i],this._k[i],this);};"
		"MMap.prototype.keys=function(){return this._k.slice();};"
		"MMap.prototype.values=function(){return this._v.slice();};"
		"g.Map=MMap;"
		"if(typeof g.WeakMap==='undefined')g.WeakMap=MMap;"
		"}"
		"if(typeof g.Image==='undefined'){"
		"var MImage=function(w,h){this.width=w||0;this.height=h||0;this.naturalWidth=0;this.naturalHeight=0;this.complete=false;this.src='';this.onload=null;this.onerror=null;this.style={};};"
		"MImage.prototype.setAttribute=function(){};"
		"MImage.prototype.addEventListener=function(){};"
		"g.Image=MImage;"
		"}"
		"if(typeof g.navigator!=='undefined'&&typeof g.navigator.sendBeacon!=='function'){g.navigator.sendBeacon=function(){return false;};}"
		"if(typeof g.__d==='undefined'){"
		"var registry={},cache={};"
		"g.__d=function(name,deps,factory){"
		"if(typeof name==='function'){var f=name;name=deps;deps=factory;factory=f;}"
		"registry[name]={deps:(deps&&deps.length)?deps:[],factory:factory};"
		"};"
		"var _require=function(name){"
		"if(cache.hasOwnProperty(name))return cache[name].exports;"
		"var mod=registry[name];"
		"if(!mod){var stub={exports:{}};cache[name]=stub;return stub.exports;}"
		"var module={exports:{}};cache[name]=module;"
		"try{"
		"if(typeof mod.factory==='function'){"
		"var args=[g,_require,_require,g.requireLazy,module,module.exports],i;"
		"for(i=0;i<mod.deps.length;i++){try{args.push(_require(mod.deps[i]));}catch(e){args.push(undefined);}}"
		"mod.factory.apply(g,args);"
		"}"
		"}catch(e){}"
		"return module.exports;"
		"};"
		"g.require=g.require||_require;"
		"g.requireDynamic=_require;"
		"g.__r=_require;"
		"g.requireLazy=function(names,cb){"
		"var r=[],i;"
		"if(names&&names.length){for(i=0;i<names.length;i++){try{r.push(_require(names[i]));}catch(e){r.push(undefined);}}}"
		"if(typeof cb==='function'){try{cb.apply(null,r);}catch(e){}}"
		"return r;"
		"};"
		"}"
		"})(this);");

	/* --- Promise combinators + documentElement/body/head fix --- */
	macsurf_qjs__safe_eval(ctx,
		"(function(g){"
		"var P=g.Promise;"
		"if(typeof P==='function'){"
		"if(!P.resolve)P.resolve=function(v){return new P(function(res){res(v);});};"
		"if(!P.reject)P.reject=function(e){return new P(function(a,rej){rej(e);});};"
		"if(!P.all)P.all=function(arr){var out=[];if(arr&&arr.forEach)arr.forEach(function(p){if(p&&typeof p.then==='function')p.then(function(v){out.push(v);});else out.push(p);});return P.resolve(out);};"
		"if(!P.race)P.race=function(arr){return new P(function(res,rej){if(arr&&arr.forEach)arr.forEach(function(p){if(p&&typeof p.then==='function')p.then(res,rej);else res(p);});});};"
		"if(!P.allSettled)P.allSettled=function(arr){var out=[];if(arr&&arr.forEach)arr.forEach(function(p){out.push({status:'fulfilled',value:p});});return P.resolve(out);};"
		"}"
		"if(typeof g.document!=='undefined'){"
		"var vw=(typeof g.innerWidth==='number'&&g.innerWidth)||980;"
		"var vh=(typeof g.innerHeight==='number'&&g.innerHeight)||600;"
		"var mkEl=function(){return {clientWidth:vw,clientHeight:vh,offsetWidth:vw,offsetHeight:vh,scrollWidth:vw,scrollHeight:vh,scrollTop:0,scrollLeft:0,offsetTop:0,offsetLeft:0,style:{},className:'',nodeType:1,"
		"getBoundingClientRect:function(){return {top:0,left:0,right:vw,bottom:vh,width:vw,height:vh,x:0,y:0};},"
		"appendChild:function(c){return c;},removeChild:function(c){return c;},insertBefore:function(c){return c;},"
		"setAttribute:function(){},getAttribute:function(){return null;},removeAttribute:function(){},hasAttribute:function(){return false;},"
		"addEventListener:function(){},removeEventListener:function(){},contains:function(){return false;},"
		"querySelector:function(){return null;},querySelectorAll:function(){return [];},"
		"getElementsByClassName:function(){return [];},getElementsByTagName:function(){return [];},"
		"classList:{add:function(){},remove:function(){},toggle:function(){},contains:function(){return false;}}};};"
		/* documentElement / body / head are installed as live getters in
		 * qjs_dom_install (real libdom-backed, with a parentNode-tracking
		 * JS fallback), so do not overwrite them with mkEl mocks here.
		 * Only fill them if dom_install has not run for some reason. */
		"var _od=Object.getOwnPropertyDescriptor(g.document,'documentElement');"
		"if(!(_od&&_od.get)){"
		"if(!g.document.documentElement)g.document.documentElement=mkEl();"
		"if(!g.document.body)g.document.body=mkEl();"
		"if(!g.document.head)g.document.head=mkEl();"
		"}"
		"}"
		"})(this);");

	/* --- capability-detection stubs (WebSocket, indexedDB, Notification,
	 *     MediaSource, crypto.getRandomValues, caches, Blob, File,
	 *     FileReader, URL.createObjectURL) --- */
	macsurf_qjs__safe_eval(ctx,
		"(function(g){"
		"var rp=function(v){return g.Promise?g.Promise.resolve(v)"
		":{then:function(cb){try{cb(v);}catch(e){}return this;},"
		"catch:function(){return this;}};};"
		"var soon=function(fn){if(typeof g.setTimeout==='function')g.setTimeout(fn,0);else{try{fn();}catch(e){}}};"
		"if(typeof g.WebSocket==='undefined'){"
		"var WS=function(url,protocols){"
		"var self=this;self.url=url;self.readyState=0;self.protocol='';"
		"self.binaryType='blob';self.bufferedAmount=0;self.extensions='';"
		"self.onopen=null;self.onmessage=null;self.onclose=null;self.onerror=null;"
		"self.send=function(){return false;};"
		"self.close=function(){self.readyState=3;if(self.onclose){try{self.onclose({code:1000,reason:'',wasClean:true});}catch(e){}}};"
		"self.addEventListener=function(t,f){if(t==='open')self.onopen=f;else if(t==='message')self.onmessage=f;else if(t==='close')self.onclose=f;else if(t==='error')self.onerror=f;};"
		"self.removeEventListener=function(){};"
		"soon(function(){self.readyState=3;if(self.onerror){try{self.onerror({type:'error'});}catch(e){}};if(self.onclose){try{self.onclose({code:1006,reason:'',wasClean:false});}catch(e){}}});"
		"};"
		"WS.CONNECTING=0;WS.OPEN=1;WS.CLOSING=2;WS.CLOSED=3;"
		"g.WebSocket=WS;"
		"}"
		"if(typeof g.indexedDB==='undefined'){"
		"var mkreq=function(){return{result:undefined,error:{name:'UnknownError',message:'unsupported'},onsuccess:null,onerror:null,onupgradeneeded:null,readyState:'pending'};};"
		"g.indexedDB={"
		"open:function(){var r=mkreq();soon(function(){r.readyState='done';if(r.onerror){try{r.onerror({target:r});}catch(e){}}});return r;},"
		"deleteDatabase:function(){var r=mkreq();soon(function(){if(r.onsuccess){try{r.onsuccess({target:r});}catch(e){}}});return r;},"
		"databases:function(){return rp([]);},"
		"cmp:function(a,b){return a<b?-1:(a>b?1:0);}"
		"};"
		"g.IDBKeyRange={bound:function(){return{};},only:function(){return{};},lowerBound:function(){return{};},upperBound:function(){return{};}};}"
		"if(typeof g.Notification==='undefined'){"
		"var N=function(title,opts){this.title=title;this.body=(opts&&opts.body)||'';this.onclick=null;this.onclose=null;this.close=function(){};};"
		"N.permission='denied';"
		"N.requestPermission=function(cb){if(cb){try{cb('denied');}catch(e){}}return rp('denied');};"
		"g.Notification=N;"
		"}"
		"if(typeof g.caches==='undefined'){"
		"var emptyCache={match:function(){return rp(undefined);},matchAll:function(){return rp([]);},add:function(){return rp(undefined);},addAll:function(){return rp(undefined);},put:function(){return rp(undefined);},delete:function(){return rp(false);},keys:function(){return rp([]);}};"
		"g.caches={open:function(){return rp(emptyCache);},match:function(){return rp(undefined);},has:function(){return rp(false);},delete:function(){return rp(false);},keys:function(){return rp([]);}};"
		"}"
		"if(typeof g.Blob==='undefined'){g.Blob=function(parts,opts){this.size=0;this.type=(opts&&opts.type)||'';this.slice=function(){return new g.Blob([]);};};}"
		"if(typeof g.File==='undefined'){g.File=function(parts,name,opts){this.name=name||'';this.size=0;this.type=(opts&&opts.type)||'';this.lastModified=0;};}"
		"if(typeof g.FileReader==='undefined'){"
		"var FR=function(){var s=this;s.result=null;s.error=null;s.readyState=0;s.onload=null;s.onerror=null;s.onloadend=null;s.onprogress=null;"
		"s.readAsText=function(){s.readyState=2;if(s.onload){try{s.onload({target:s});}catch(e){}}if(s.onloadend){try{s.onloadend({target:s});}catch(e){}}};"
		"s.readAsDataURL=s.readAsText;s.readAsArrayBuffer=s.readAsText;s.readAsBinaryString=s.readAsText;s.abort=function(){};"
		"s.addEventListener=function(){};s.removeEventListener=function(){};};"
		"g.FileReader=FR;"
		"}"
		"if(typeof g.URL!=='undefined'&&typeof g.URL.createObjectURL!=='function'){g.URL.createObjectURL=function(){return 'blob:macsurf/0';};g.URL.revokeObjectURL=function(){};}"
		"})(this);");

	/* --- DOM constructor family stubs --- */
	macsurf_qjs__safe_eval(ctx,
		"(function(g){"
		"var names=['Node','Element','CharacterData','Text','Comment','DocumentFragment',"
		"'HTMLElement','HTMLUnknownElement','HTMLHtmlElement','HTMLHeadElement','HTMLBodyElement',"
		"'HTMLDivElement','HTMLSpanElement','HTMLParagraphElement','HTMLAnchorElement',"
		"'HTMLImageElement','HTMLCanvasElement','HTMLInputElement','HTMLButtonElement',"
		"'HTMLTextAreaElement','HTMLSelectElement','HTMLOptionElement','HTMLFormElement',"
		"'HTMLLabelElement','HTMLUListElement','HTMLOListElement','HTMLLIElement',"
		"'HTMLTableElement','HTMLTableRowElement','HTMLTableCellElement',"
		"'HTMLScriptElement','HTMLStyleElement','HTMLLinkElement','HTMLMetaElement',"
		"'HTMLIFrameElement','HTMLVideoElement','HTMLAudioElement','HTMLMediaElement',"
		"'HTMLSourceElement','HTMLPictureElement','HTMLTemplateElement','HTMLSlotElement',"
		"'HTMLBRElement','HTMLHRElement','HTMLPreElement','HTMLDListElement',"
		"'SVGElement','SVGSVGElement','HTMLCollection','NodeList','DOMException',"
		"'CSSStyleDeclaration','CSSStyleSheet','CSSRule','MediaQueryList','DOMTokenList',"
		"'NamedNodeMap','Attr','Range','XMLDocument','ShadowRoot','MutationRecord',"
		"'DOMParser','XMLSerializer','TreeWalker','NodeIterator','AbortController','AbortSignal'];"
		"var i;"
		"for(i=0;i<names.length;i++){"
		"if(typeof g[names[i]]==='undefined'){"
		"g[names[i]]=function(){};"
		"}"
		"}"
		"})(this);");

	/* --- CSS / ResizeObserver / PerformanceObserver / queueMicrotask /
	 *     structuredClone / TextEncoder / TextDecoder --- */
	macsurf_qjs__safe_eval(ctx,
		"(function(g){"
		"var soon=function(fn){if(typeof g.setTimeout==='function')g.setTimeout(fn,0);else{try{fn();}catch(e){}}};"
		"if(typeof g.CSS==='undefined'){"
		"g.CSS={supports:function(){return false;},escape:function(s){return String(s);},"
		"registerProperty:function(){},paintWorklet:undefined};"
		"}"
		"if(typeof g.queueMicrotask!=='function'){g.queueMicrotask=function(fn){soon(fn);};}"
		"if(typeof g.structuredClone!=='function'){"
		"g.structuredClone=function(x){try{return JSON.parse(JSON.stringify(x));}catch(e){return x;}};"
		"}"
		"if(typeof g.TextEncoder==='undefined'){"
		"g.TextEncoder=function(){this.encoding='utf-8';this.encode=function(s){s=String(s==null?'':s);var a=[],i;for(i=0;i<s.length;i++)a.push(s.charCodeAt(i)&0xff);return a;};};"
		"}"
		"if(typeof g.TextDecoder==='undefined'){"
		"g.TextDecoder=function(){this.encoding='utf-8';this.decode=function(buf){if(!buf)return '';var s='',i,n=buf.length||0;for(i=0;i<n;i++)s+=String.fromCharCode(buf[i]);return s;};};"
		"}"
		"})(this);");

	/* --- Selection / Range / getSelection / execCommand stubs --- */
	macsurf_qjs__safe_eval(ctx,
		"(function(g){"
		"if(typeof g.getSelection!=='function'){"
		"g.getSelection=function(){"
		"return{rangeCount:0,isCollapsed:true,type:'None',"
		"toString:function(){return'';},"
		"addRange:function(){},"
		"removeAllRanges:function(){},"
		"getRangeAt:function(){return null;},"
		"collapse:function(){},"
		"selectAllChildren:function(){},"
		"deleteFromDocument:function(){}};"
		"};"
		"}"
		"if(typeof g.document!=='undefined'){"
		"if(typeof g.document.createRange!=='function'){"
		"g.document.createRange=function(){return{startContainer:null,endContainer:null,startOffset:0,endOffset:0,collapsed:true,commonAncestorContainer:null,setStart:function(){},setEnd:function(){},collapse:function(){},cloneRange:function(){return this;},deleteContents:function(){},getBoundingClientRect:function(){return{top:0,left:0,right:0,bottom:0,width:0,height:0};},getClientRects:function(){return[];},toString:function(){return'';},detach:function(){}};};"
		"}"
		"if(typeof g.document.execCommand!=='function'){g.document.execCommand=function(){return false;};}"
		"if(typeof g.document.queryCommandSupported!=='function'){g.document.queryCommandSupported=function(){return false;};}"
		"if(typeof g.document.queryCommandEnabled!=='function'){g.document.queryCommandEnabled=function(){return false;};}"
		"if(!('activeElement'in g.document)){g.document.activeElement=null;}"
		"if(typeof g.document.getSelection!=='function'){g.document.getSelection=g.getSelection;}"
		"}"
		"})(this);");

	JS_FreeValue(ctx, global);
}

/* ------------------------------------------------------------------ */
/* macsurf_qjs_setup_globals — called before register_browser_globals  */
/* to install document / getElementById / createElement / querySelector */
/* These are thin stubs; real DOM wiring deferred to a later round.    */
/* ------------------------------------------------------------------ */

void macsurf_qjs_setup_globals(JSContext *qctx)
{
	/* Install a minimal document object with the same shape as
	 * the Duktape macsurf_js_dom.c provides, so page scripts that
	 * call document.getElementById etc. get a no-op stub rather
	 * than a ReferenceError.  Real libdom wiring follows in a later
	 * round once QuickJS is confirmed working on hardware. */
	macsurf_qjs__safe_eval(qctx,
		"(function(){"
		"if(typeof document!=='undefined')return;"
		"var doc={"
			"_elements:{},"
			"getElementById:function(id){return this._elements[id]||null;},"
			"createElement:function(tag){return{tagName:tag.toUpperCase(),"
				"children:[],style:{},className:'',"
				"getAttribute:function(){return null;},"
				"setAttribute:function(){},"
				"addEventListener:function(){},"
				"removeEventListener:function(){},"
				"appendChild:function(c){this.children.push(c);return c;},"
				"querySelector:function(){return null;},"
				"querySelectorAll:function(){return[];}};},"
			"querySelector:function(sel){return null;},"
			"querySelectorAll:function(sel){return[];},"
			"title:'',"
			"readyState:'complete'"
		"};"
		"this.document=doc;"
		"})();");
}

/* ------------------------------------------------------------------ */
/* Content-handler registration (mirrors macsurf_js.c)                 */
/* ------------------------------------------------------------------ */

#include "content/content_factory.h"
#include "content/content_protected.h"

static nserror macsurf_qjs__content_create(
		const struct content_handler *handler,
		lwc_string *imime_type,
		const struct http_parameter *params,
		struct llcache_handle *llcache,
		const char *fallback_charset,
		bool quirks,
		struct content **c)
{
	struct content *content;
	nserror error;
	(void)params;
	content = (struct content *)calloc(1, sizeof(struct content));
	if (content == NULL) return NSERROR_NOMEM;
	error = content__init(content, handler, imime_type, NULL, llcache,
			fallback_charset, quirks);
	if (error != NSERROR_OK) { free(content); return error; }
	*c = content;
	return NSERROR_OK;
}

static bool macsurf_qjs__content_convert(struct content *c)
{
	content_set_ready(c);
	content_set_done(c);
	return true;
}

static void macsurf_qjs__content_destroy(struct content *c) { (void)c; }

static nserror macsurf_qjs__content_clone(const struct content *old,
		struct content **newc)
{
	(void)old; (void)newc;
	return NSERROR_CLONE_FAILED;
}

static content_type macsurf_qjs__content_type(void)
{
	return CONTENT_JS;
}

/* no_share field is in content_protected.h: content_handler.no_share is a
 * bool after the type() function ptr. */

static const struct content_handler macsurf_qjs__content_handler = {
	.create        = macsurf_qjs__content_create,
	.data_complete = macsurf_qjs__content_convert,
	.destroy       = macsurf_qjs__content_destroy,
	.clone         = macsurf_qjs__content_clone,
	.type          = macsurf_qjs__content_type,
	.no_share      = false
};

static const char * const macsurf_qjs__content_types[] = {
	"application/javascript",
	"application/ecmascript",
	"application/x-javascript",
	"text/javascript",
	"text/ecmascript"
};

/* ------------------------------------------------------------------ */
/* NetSurf js_thread API                                                */
/* ------------------------------------------------------------------ */

void js_initialise(void)
{
	size_t i;
	MS_LOG("qjs: initialise");
	for (i = 0;
	     i < sizeof macsurf_qjs__content_types /
	         sizeof macsurf_qjs__content_types[0];
	     i++) {
		nserror e = content_factory_register_handler(
				macsurf_qjs__content_types[i],
				&macsurf_qjs__content_handler);
		if (e != NSERROR_OK) {
			macsurf_debug_log_writef("qjs: register %s failed err=%d",
					macsurf_qjs__content_types[i], (int)e);
		}
	}
	MS_LOG("qjs: content types registered");
}

void js_finalise(void)
{
	MS_LOG("qjs: finalise");
}

/* fixes532: build a fresh JSContext (realm + global) on an existing runtime,
 * install all intrinsics and the full browser/DOM global surface.  Factored
 * out of js_newheap so js_newthread can recreate the realm per navigation,
 * giving each page a clean global lexical environment (no top-level
 * let/const/class redeclaration collisions across page loads).  Returns the
 * new context, or NULL on failure (runtime left intact).  Mirrors the former
 * inline chain in js_newheap byte-for-byte. */
static JSContext *qjs_build_context(struct jsheap *heap)
{
	JSContext *ctx;
	ctx = JS_NewContextRaw(heap->rt);
	if (ctx == NULL) return NULL;
	MS_LOG("qjs intr: raw ctx ok");
	MS_LOG("qjs intr: BaseObjects"); JS_AddIntrinsicBaseObjects(ctx);
	MS_LOG("qjs intr: Date");        JS_AddIntrinsicDate(ctx);
	MS_LOG("qjs intr: Eval");        JS_AddIntrinsicEval(ctx);
	MS_LOG("qjs intr: RegExp");      JS_AddIntrinsicRegExp(ctx);
	MS_LOG("qjs intr: JSON");        JS_AddIntrinsicJSON(ctx);
	MS_LOG("qjs intr: Proxy");       JS_AddIntrinsicProxy(ctx);
	MS_LOG("qjs intr: MapSet");      JS_AddIntrinsicMapSet(ctx);
	MS_LOG("qjs intr: TypedArrays"); JS_AddIntrinsicTypedArrays(ctx);
	MS_LOG("qjs intr: Promise");     JS_AddIntrinsicPromise(ctx);
	MS_LOG("qjs intr: WeakRef");     JS_AddIntrinsicWeakRef(ctx);
	MS_LOG("qjs intr: AToB");        JS_AddIntrinsicAToB(ctx);
	MS_LOG("qjs intr: Performance"); JS_AddPerformance(ctx);
	MS_LOG("qjs intr: all done");

	MS_LOG("qjs: setup_globals");   macsurf_qjs_setup_globals(ctx);
	MS_LOG("qjs: browser_globals"); register_browser_globals(ctx);
	MS_LOG("qjs: dom_install");     qjs_dom_install(ctx);
	return ctx;
}

/* fixes593 — QuickJS capability self-test. Runs a battery of JS through the
 * engine at first heap creation, BEFORE any page loads, and logs PASS/FAIL +
 * the actual value. Purpose: prove the CW8 QuickJS port is fundamentally sound
 * (correct results) and can survive heavy allocation (the 100k-object test
 * mimics what jquery/webpack bundles do). If a test returns a WRONG value, a
 * code path is miscompiled; if the engine FREEZES here with no page loaded,
 * the heap corruptor is in the engine itself, reproduced in isolation. */
#ifdef MACSURF_QJS_SELFTEST
static int qjs_selftest_i(JSContext *ctx, const char *name,
		const char *src, int want)
{
	JSValue v;
	int32_t got = -987654;
	int ok;
	v = JS_Eval(ctx, src, strlen(src), "<selftest>", JS_EVAL_TYPE_GLOBAL);
	if (JS_IsException(v)) {
		JSValue e = JS_GetException(ctx);
		const char *s = JS_ToCString(ctx, e);
		macsurf_debug_log_writef("qjs selftest %s: EXCEPTION %s",
			name, s ? s : "?");
		if (s) JS_FreeCString(ctx, s);
		JS_FreeValue(ctx, e);
		JS_FreeValue(ctx, v);
		return 0;
	}
	JS_ToInt32(ctx, &got, v);
	JS_FreeValue(ctx, v);
	ok = (got == want);
	macsurf_debug_log_writef("qjs selftest %s: %s got=%ld want=%d",
		name, ok ? "PASS" : "FAIL", (long)got, want);
	return ok;
}

static void qjs_selftest(JSContext *ctx)
{
	macsurf_debug_log_writef("qjs selftest: BEGIN");
	qjs_selftest_i(ctx, "arith", "123456789 % 1000", 789);
	qjs_selftest_i(ctx, "bigmul", "40000*40000/1000", 1600000);
	qjs_selftest_i(ctx, "str20k",
		"(function(){var s='';var i;for(i=0;i<20000;i++)s+='ab';return s.length;})()",
		40000);
	qjs_selftest_i(ctx, "arr20k",
		"(function(){var a=[];var i;for(i=0;i<20000;i++)a.push(i*2);return a[19999];})()",
		39998);
	qjs_selftest_i(ctx, "props2k",
		"(function(){var o={};var i;for(i=0;i<2000;i++)o['k'+i]=i;return o.k1999;})()",
		1999);
	qjs_selftest_i(ctx, "fib25",
		"(function f(n){return n<2?n:f(n-1)+f(n-2);})(25)", 75025);
	qjs_selftest_i(ctx, "json",
		"JSON.parse('{\"a\":[1,2,3],\"b\":{\"c\":42}}').b.c", 42);
	qjs_selftest_i(ctx, "mapreduce",
		"[1,2,3,4,5].map(function(x){return x*x;}).reduce(function(a,b){return a+b;},0)",
		55);
	qjs_selftest_i(ctx, "regex",
		"('a1b22c333'.match(/[0-9]+/g)).length", 3);
	macsurf_debug_log_writef("qjs selftest: heavy 100k objects (mimics bundles)...");
	qjs_selftest_i(ctx, "obj100k",
		"(function(){var a=[];var i;for(i=0;i<100000;i++)a.push({v:i});return a.length;})()",
		100000);

	/* fixes597 — engine core is proven sound; now hammer the DOM BRIDGE (the
	 * layer the real scripts hit that the pure-JS tests above don't): wrapper
	 * creation, dom_string round-trips, node refcounts. If one of these FREEZES
	 * with no page loaded, the corruptor is reproduced in the bridge, minimal.
	 * These may THROW at startup if document isn't wired yet — a logged
	 * exception is fine (tells us the bridge needs a live doc); a FREEZE is the
	 * prize. */
	macsurf_debug_log_writef("qjs selftest: DOM bridge stress...");
	qjs_selftest_i(ctx, "dom_create",
		"(function(){var e=document.createElement('div');e.setAttribute('id','q');return e.getAttribute('id')==='q'?1:0;})()",
		1);
	qjs_selftest_i(ctx, "dom_attr5k",
		"(function(){var i,e;for(i=0;i<5000;i++){e=document.createElement('span');e.setAttribute('class','c'+i);e.setAttribute('data-n',''+i);}return 1;})()",
		1);
	qjs_selftest_i(ctx, "dom_tree3k",
		"(function(){var r=document.createElement('div');var i,c;for(i=0;i<3000;i++){c=document.createElement('p');r.appendChild(c);}return r.childNodes.length;})()",
		3000);
	qjs_selftest_i(ctx, "dom_churn",
		"(function(){var r=document.createElement('div');var i,c;for(i=0;i<3000;i++){c=document.createElement('b');r.appendChild(c);r.removeChild(c);}return 1;})()",
		1);
	qjs_selftest_i(ctx, "dom_query",
		"(function(){var i;for(i=0;i<2000;i++){document.getElementById('nope');}return 1;})()",
		1);

	/* fixes598 — the ONE thing every real script does that the tests above
	 * don't: THROW. All of tinkerdifferent's scripts throw TypeErrors, and
	 * every freeze window this session sat next to exception handling
	 * (JS_FreeCString of the message/.stack, build_backtrace). Hammer
	 * throw+catch, then throw+catch+.stack (which runs build_backtrace). If one
	 * of these FREEZES with no page, the exception machinery is the corruptor,
	 * reproduced minimally. */
	macsurf_debug_log_writef("qjs selftest: exception stress...");
	qjs_selftest_i(ctx, "throw5k",
		"(function(){var i,n=0;for(i=0;i<5000;i++){try{var z=null;z.x;}catch(e){if(e.message.length>0)n++;}}return n;})()",
		5000);
	qjs_selftest_i(ctx, "throwstack2k",
		"(function(){var i,n=0;for(i=0;i<2000;i++){try{var z;z.foo();}catch(e){if(e.stack&&e.stack.length>0)n++;}}return n;})()",
		2000);

	/* fixes599 — the last untested bridge path: LIVE nodes attached to the
	 * document tree (all tests above used DETACHED createElement'd nodes; the
	 * real scripts wrap live parsed nodes). Attach 2k nodes to the live root and
	 * query them back through the wrapper/refcount path. Returns -1 if no root
	 * exists at startup (then this must move post-navigation); 1 if live-node
	 * wrapping is sound; a FREEZE = found it. */
	macsurf_debug_log_writef("qjs selftest: live-tree stress...");
	qjs_selftest_i(ctx, "dom_live2k",
		"(function(){var r=document.documentElement||document.body;if(!r)return -1;var i,c;for(i=0;i<2000;i++){c=document.createElement('div');c.setAttribute('id','n'+i);r.appendChild(c);}var q=document.getElementById('n1500');return q?1:0;})()",
		1);
	macsurf_debug_log_writef("qjs selftest: END");
}
#endif /* MACSURF_QJS_SELFTEST */

/* Bulletproof allocator wrappers for QuickJS JSMallocFunctions.
 * Call macsurf_safe_* directly -- do NOT route through the prefix
 * malloc macro to avoid any recursion risk. */
static void *qjs_safe_malloc(void *opaque, size_t size)
{
	(void)opaque;
	return macsurf_safe_alloc(size);
}

static void qjs_safe_free(void *opaque, void *ptr)
{
	(void)opaque;
	free(ptr);
}

static void *qjs_safe_realloc(void *opaque, void *ptr, size_t size)
{
	(void)opaque;
	return macsurf_safe_realloc(ptr, size);
}

static void *qjs_safe_calloc(void *opaque, size_t count, size_t size)
{
	(void)opaque;
	return macsurf_safe_calloc(count, size);
}

static size_t qjs_safe_usable_size(const void *ptr)
{
	(void)ptr;
	return 0;
}

/* Field order must match JSMallocFunctions (quickjs.h:470):
 * calloc, malloc, free, realloc, malloc_usable_size */
static JSMallocFunctions macsurf_qjs_mf = {
	qjs_safe_calloc,
	qjs_safe_malloc,
	qjs_safe_free,
	qjs_safe_realloc,
	qjs_safe_usable_size
};

nserror js_newheap(int timeout, struct jsheap **out_heap)
{
	struct jsheap *heap;
	if (out_heap == NULL) return NSERROR_BAD_PARAMETER;
	*out_heap = NULL;
	heap = (struct jsheap *)calloc(1, sizeof(*heap));
	if (heap == NULL) return NSERROR_NOMEM;

	heap->rt = JS_NewRuntime2(&macsurf_qjs_mf, NULL);
	if (heap->rt == NULL) { free(heap); return NSERROR_NOMEM; }

	/* fixes590 -- ROOT CAUSE of the tinkerdifferent hard-freeze.
	 *
	 * QuickJS guards native-stack overflow (deep JS recursion => deep
	 * recursive JS_CallInternal C frames, each with its own alloca) via
	 * rt->stack_limit = stack_top - stack_size.  The default stack_size is
	 * JS_DEFAULT_STACK_SIZE = 1 MB (quickjs.h), calibrated for a desktop
	 * process.  js_newheap never called JS_SetMaxStackSize, so that 1 MB
	 * default stood on OS 9 -- where the native stack is FAR smaller than
	 * 1 MB and grows DOWN toward the very same application partition that
	 * holds the MSL malloc pool.  A jQuery/Sizzle/webpack deep recursion
	 * therefore overruns the real native stack and overwrites the MSL heap
	 * free-list metadata LONG before QuickJS's 1 MB guard fires: the guard
	 * is effectively dead.  Because it clobbers MAPPED heap (not an unmapped
	 * page) there is no crash -- instead the free-list goes cyclic and the
	 * next malloc()/free() spins forever (the deterministic freeze; JS-off
	 * loads fine; disabling build_backtrace only shifted the collision
	 * threshold, all consistent with a stack-into-heap overrun).
	 *
	 * Fix: size the guard to the REAL native-stack headroom.  StackSpace()
	 * is (current SP - ApplLimit); ApplLimit is the fixed ceiling the heap
	 * cannot grow past, so setting stack_size = StackSpace() - margin puts
	 * rt->stack_limit at (ApplLimit + margin) -- a fixed address safely above
	 * the heap, independent of how deep JS later runs.  QuickJS then throws a
	 * catchable "RangeError: stack overflow" instead of smashing the heap. */
#ifdef __MACOS9__
	{
		/* Explicit prototype: StackSpace() returns long; without a
		 * declaration CW8 assumes int and truncates. CarbonLib-safe. */
		extern long StackSpace(void);
		long sp_room = StackSpace();       /* SP - ApplLimit, bytes */
		long qmax = sp_room - (96L * 1024L); /* margin: non-JS frames + slop */
		/* fixes593: StackSpace() reads ~107MB on real hw (big partition), so
		 * the native stack was never the corruptor — set the guard to the real
		 * headroom (no small cap, which would wrongly RangeError legit deep JS)
		 * with a floor so basic JS still runs if the read is tiny. */
		if (qmax < 24576L) qmax = 24576L;  /* floor so basic JS still runs */
		JS_SetMaxStackSize(heap->rt, (size_t)qmax);
		macsurf_debug_log_writef(
			"qjs: stack guard=%ld (StackSpace=%ld)", qmax, sp_room);
	}
#else
	/* fixes873 — NON-MAC (the Linux ASan harness) — 4 MB, not the 256 KB that
	 * used to be here.
	 *
	 * This branch must MODEL the Mac branch above, and 256 KB modelled nothing:
	 * the Mac sets the guard to real native headroom, which reads ~107 MB on
	 * hardware (fixes593), and even QuickJS's own default is 1 MB. At 256 KB the
	 * harness invented a "RangeError: Maximum call stack size exceeded" inside
	 * Preact's recursive reconciler for the real verbum bundle -- a failure
	 * hardware cannot have. Same trap as the timebase stub: a harness that
	 * diverges from the shipping config reports fiction. Too small manufactures
	 * phantom bugs; too large hides real ones. 4 MB sits well under the 8 MB
	 * Linux main-thread stack while giving deep-but-legitimate JS recursion the
	 * headroom the Mac actually has. */
	JS_SetMaxStackSize(heap->rt, 4UL * 1024UL * 1024UL);
#endif

	/* fixes522: bound the JS heap so a heavy/runaway script throws an OOM
	 * exception instead of exhausting the OS partition and crashing the
	 * machine.  Generous (partition free is ~300MB) but capped. */
	JS_SetMemoryLimit(heap->rt, 128UL * 1024UL * 1024UL);

	/* fixes593 — the heap-corruption freeze on heavy JS pages (tinkerdifferent:
	 * all scripts still RUN through QuickJS, but a later malloc/free spins on a
	 * smashed free-list). Prime suspect is QuickJS's automatic cycle-GC: it only
	 * fires once malloc_size crosses this threshold (default 256KB), i.e. ONLY
	 * on heavy pages — exactly the tinkerdifferent-vs-68kmla split — and if any
	 * ref is over-released, the cycle collector double-frees when it walks the
	 * graph. Push the threshold past the 128MB memory cap so auto-GC never runs
	 * mid-load. Nothing about which JS runs changes; the per-navigation runtime
	 * is torn down wholesale on nav, so uncollected cycles never accumulate.
	 * (If this proves it, the real refcount bug gets fixed and GC re-armed.) */
	JS_SetGCThreshold(heap->rt, (size_t)0x40000000UL);  /* 1GB > 128MB cap */

	JS_SetInterruptHandler(heap->rt, qjs_interrupt_handler, NULL);

	/* Register element class before any context is created */
	qjs_dom_init_class(heap->rt);

	/* fixes530/532: the JS_NewContext intrinsic chain is expanded in
	 * qjs_build_context with an MS_LOG breadcrumb before each intrinsic, so
	 * the debug log's LAST "qjs intr: X" line before a crash names the exact
	 * intrinsic that NULL-calls.  Equivalent to JS_NewContext (which is
	 * literally JS_NewContextRaw followed by that same chain). */
	heap->ctx = qjs_build_context(heap);
	if (heap->ctx == NULL) {
		JS_FreeRuntime(heap->rt);
		free(heap);
		return NSERROR_NOMEM;
	}
	/* fixes875 (#304) — stamp the realm, then link. qjs_ctx_gen() answers by
	 * walking g_heap_list, so a timer registered before the link would be
	 * stamped generation 0 and abandoned forever (never fires, silently).
	 * Nothing between here and the link runs page JS, and JS cannot run any
	 * earlier either: heap->ctx does not exist until qjs_build_context returns,
	 * so linking sooner would not help -- the list keys on heap->ctx. */
	heap->ctx_gen = g_ctx_gen_next++;

	heap->timeout = timeout;

	g_heap = heap;
	/* fixes861 (#289) — link into the all-heaps list so this heap's timers
	 * actually get pumped.  Newest-first; order does not matter.
	 * fixes875 (#304) — this list is now also the authority qjs_ctx_gen()
	 * consults to decide which realm owns a ctx, so an unlinked heap means
	 * "generation 0" = every timer it registers is abandoned and never fires. */
	heap->next = g_heap_list;
	g_heap_list = heap;
	*out_heap = heap;
	MS_LOG("qjs: heap created");

	/* fixes671 (perf): the fixes593-598 capability self-test runs a heavy JS
	 * battery (100k object allocs, fib25, 20k string/array ops, 5k DOM ops,
	 * throw/backtrace stress) SYNCHRONOUSLY at first heap creation — ~17s on a
	 * real G3 BEFORE the event loop starts, i.e. the entire 'pause on browser
	 * open'. It was a diagnostic to hunt a QuickJS freeze (long closed) and is
	 * not needed in normal operation. Gated OFF by default; define
	 * MACSURF_QJS_SELFTEST to re-run it when debugging the engine. */
#ifdef MACSURF_QJS_SELFTEST
	{
		static int selftest_done = 0;
		if (!selftest_done) {
			selftest_done = 1;
			qjs_selftest(heap->ctx);
		}
	}
#endif
	return NSERROR_OK;
}

void js_destroyheap(struct jsheap *heap)
{
	if (heap == NULL) return;
	/* fixes854 (#283) — drop this heap's timer + XHR slots BEFORE the context
	 * dies.  Both arenas are global while heaps are per-window/per-iframe, so
	 * a closed iframe used to leave `live` slots behind holding JSValues into
	 * a freed runtime: the next run_timers pass would JS_Call them, and the
	 * next navigation's qjs_flush_timers would free them against an unrelated
	 * context.  js_newthread() already does exactly this pair on the
	 * navigation path; teardown needs it too.  Order is load-bearing (free
	 * the refs against the still-live ctx, then free the ctx). */
	if (heap->ctx != NULL) {
		qjs_flush_timers(heap->ctx);
		macos9_js_fetch_flush(heap->ctx);
	}
	if (heap->ctx != NULL) JS_FreeContext(heap->ctx);
	/* fixes541: release any wrappers whose finalizer JS_FreeContext did not
	 * run (obj->method reference cycles); both halves, then clear. */
	qjs_wrap_drain();
	if (heap->rt  != NULL) JS_FreeRuntime(heap->rt);
	if (g_heap == heap) g_heap = NULL;
	/* fixes861 (#289) — unlink before the free, or pump_all walks freed
	 * memory on the very next event-loop pass. */
	{
		struct jsheap **pp = &g_heap_list;
		while (*pp != NULL) {
			if (*pp == heap) { *pp = heap->next; break; }
			pp = &(*pp)->next;
		}
	}
	free(heap);
}

nserror js_newthread(struct jsheap *heap, void *win_priv, void *doc_priv,
		struct jsthread **out_thread)
{
	struct jsthread *thread;
	if (out_thread == NULL) return NSERROR_BAD_PARAMETER;
	*out_thread = NULL;
	if (heap == NULL || heap->ctx == NULL) return NSERROR_BAD_PARAMETER;
	thread = (struct jsthread *)calloc(1, sizeof(*thread));
	if (thread == NULL) return NSERROR_NOMEM;

	/* fixes532: each navigation gets a FRESH realm/global so a page's
	 * top-level let/const/class (e.g. XenForo preamble.min.js declaring XF)
	 * does not collide with the previous page's lexical bindings in the
	 * persistent heap (SyntaxError: redeclaration of 'XF' on the 2nd load).
	 * Tear the old context down and build a clean one on the SAME runtime.
	 * Only for a real document thread (doc_priv != NULL); the bare init
	 * thread keeps the heap's context.  Order is load-bearing: flush the old
	 * page's timers (frees their duped callback JSValues against the OLD
	 * context) BEFORE freeing it, or those refs dangle and run_timers
	 * JS_Calls into freed heap. */
	if (doc_priv != NULL && heap->ctx != NULL) {
		JSContext *fresh;
		qjs_flush_timers(heap->ctx);
		/* fixes846 (#167 S3) — same load-bearing ordering as the timer
		 * flush above: abort every in-flight XHR and free its dup'd
		 * JSValue against the OLD context before it's freed, or a
		 * response that arrives after navigation would JS_Call into
		 * freed heap from xhr_deliver(). */
		macos9_js_fetch_flush(heap->ctx);
		fresh = qjs_build_context(heap);
		if (fresh != NULL) {
			JS_FreeContext(heap->ctx);
			/* fixes541: release every old-page wrapper's node ref AND
			 * owner-document keepalive, then clear the map, before the
			 * fresh context wraps anything.  Both halves, then clear. */
			qjs_wrap_drain();
			heap->ctx = fresh;
			/* fixes875 (#304) — a NEW realm, so a NEW generation. This is
			 * the reuse that bites: JS_FreeContext just above returned the
			 * old ctx's memory to the allocator, so `fresh` (or some other
			 * heap's next context) can legitimately land on that exact
			 * address. Without a fresh generation, any leftover slot tagged
			 * with the old pointer would now "own" this realm. */
			heap->ctx_gen = g_ctx_gen_next++;
			MS_LOG("qjs: realm reset for navigation");
		} else {
			MS_LOG("qjs: realm reset FAILED, reusing old ctx");
		}
	}

	thread->heap = heap;
	thread->ctx  = heap->ctx;
	thread->win_priv = win_priv;
	thread->doc_priv = doc_priv;
	*out_thread = thread;
	if (doc_priv != NULL) {
		/* doc_priv is html_content* — extract dom_document*.
		 * html_content is defined in content/handlers/html/private.h;
		 * same access pattern as macsurf_js.c (Duktape). */
		html_content *htmlc = (html_content *)doc_priv;
		qjs_set_document(htmlc->document);
		qjs_set_content((struct content *)htmlc);
		/* Re-wire getElementById/querySelectorAll with real document now */
		qjs_dom_install(heap->ctx);
		MS_LOG("qjs: thread document wired");
	}
	MS_LOG("qjs: thread created");
	return NSERROR_OK;
}

nserror js_closethread(struct jsthread *thread)
{
	(void)thread;
	return NSERROR_OK;
}

void js_destroythread(struct jsthread *thread)
{
	free(thread);
}

/* -----------------------------------------------------------------------
 * XenForo stub scripts — ported from macsurf_js.c (Duktape version).
 * These inject minimal ES5 shims in place of ES6-heavy XF bundles.
 * ----------------------------------------------------------------------- */

/* fixes481: extend window.XF instead of replacing it */
static const char s_xf_preamble_stub[] =
	"(function(){"
	"window.XF=window.XF||{};"
	"var XF=window.XF;"
	"if(!XF.ready){"
	"var _q=false,_r=[];"
	"XF.ready=function(a){_q?setTimeout(a,0):_r.push(a);};"
	"document.addEventListener('DOMContentLoaded',function(){"
	"_q=true;var i;for(i=0;i<_r.length;i++)setTimeout(_r[i],0);"
	"});"
	"}"
	"XF.browser=XF.browser||{browser:'',version:0,os:'',osVersion:null};"
	"XF.Feature=XF.Feature||{runTests:function(){},has:function(){return false;}};"
	"(function(){"
	"var de=document.documentElement;"
	"if(de&&de.getAttribute){"
	"var cls=de.getAttribute('class')||'';"
	"cls=cls.replace(/\\bhas-no-js\\b/g,'has-js');"
	"if(cls.indexOf('has-js')<0)cls='has-js '+cls;"
	"de.setAttribute('class',cls);"
	"}"
	"})();"
	"})();";

/* fixes480: ES5 XF framework stub replacing core-compiled.js */
static const char s_xf_core_stub[] =
	"(function(){"
	"var XF=window.XF||{};"
	"window.XF=XF;"
	"if(typeof console!=='undefined')console.log('[ms] core stub start XF.ready='+typeof XF.ready);"
	"XF.DataStore=(function(){"
	"var nid=1,map={};"
	"function gid(el){if(!el.__xfDsId)el.__xfDsId=nid++;return el.__xfDsId;}"
	"return{"
	"get:function(el,k){var d=map[gid(el)];return d?d[k]:void 0;},"
	"set:function(el,k,v){var i=gid(el);if(!map[i])map[i]={};map[i][k]=v;},"
	"remove:function(el,k){var d=map[gid(el)];if(d)delete d[k];}"
	"};"
	"})();"
	"XF.hasOwn=function(o,k){return Object.prototype.hasOwnProperty.call(o,k);};"
	"XF.extendObject=function(){"
	"var a=arguments,deep=false,out=a[0]||{},s=1,i,k,src;"
	"if(typeof out==='boolean'){deep=out;out=a[1]||{};s=2;}"
	"for(i=s;i<a.length;i++){src=a[i];if(!src)continue;"
	"for(k in src){if(XF.hasOwn(src,k))out[k]=src[k];}}"
	"return out;"
	"};"
	"XF.applyDataOptions=function(def,ds,prov){"
	"var out={},k,v,dt;"
	"for(k in def){"
	"if(!XF.hasOwn(def,k))continue;"
	"out[k]=def[k];"
	"if(ds&&XF.hasOwn(ds,k)){"
	"v=ds[k];dt=typeof v;"
	"switch(typeof def[k]){"
	"case 'object':if(dt==='string')try{v=JSON.parse(v);}catch(e){}break;"
	"case 'string':if(dt!=='string')v=String(v);break;"
	"case 'number':if(dt!=='number')v=Number(v);break;"
	"case 'boolean':if(dt!=='boolean')v=(v===true||v==='true');break;"
	"}"
	"out[k]=v;"
	"}}"
	"if(prov){for(k in prov){if(XF.hasOwn(prov,k))out[k]=prov[k];}}"
	"return out;"
	"};"
	"XF.create=function(proto){"
	"var Ctor=function(el,opts){"
	"var k,o=proto.options||{},inst={};"
	"for(k in o){if(XF.hasOwn(o,k))inst[k]=o[k];}"
	"this.options=XF.applyDataOptions(inst,el?el.dataset:{},opts||{});"
	"this.target=el||null;"
	"if(proto.__construct)proto.__construct.call(this,el,opts||{});"
	"};"
	"var k;"
	"for(k in proto){if(XF.hasOwn(proto,k))Ctor.prototype[k]=proto[k];}"
	"Ctor.prototype.constructor=Ctor;"
	"Ctor.extend=function(mixin){"
	"var merged={},p=Ctor.prototype;"
	"for(k in p){if(XF.hasOwn(p,k))merged[k]=p[k];}"
	"for(k in mixin){if(XF.hasOwn(mixin,k))merged[k]=mixin[k];}"
	"return XF.create(merged);"
	"};"
	"return Ctor;"
	"};"
	"XF.extend=function(base,mixin){return base.extend(mixin);};"
	"function ClassMapper(){this._map={};}"
	"ClassMapper.prototype.add=function(n,c){this._map[n]=c;};"
	"ClassMapper.prototype.getObjectFromIdentifier=function(n){"
	"var v=this._map[n];"
	"if(v){if(typeof v==='function')return v;n=v;}"
	"var parts=n.split('.'),obj=window,i;"
	"for(i=0;i<parts.length;i++){if(!obj)return null;obj=obj[parts[i]];}"
	"return obj||null;"
	"};"
	"ClassMapper.prototype.extend=function(n,m){"
	"var b=this.getObjectFromIdentifier(n);"
	"if(b&&b.extend)this._map[n]=b.extend(m);"
	"};"
	"XF.ClassMapper=ClassMapper;"
	"XF.Element=(function(){"
	"var mapper=new ClassMapper();"
	"function applyHandler(el,name,opts){"
	"var handlers=XF.DataStore.get(el,'xf-element-handlers')||{};"
	"if(handlers[name])return handlers[name];"
	"var K=mapper.getObjectFromIdentifier(name);"
	"if(!K||typeof K!=='function')return null;"
	"var h;try{h=new K(el,opts||{});}catch(e){return null;}"
	"handlers[name]=h;"
	"XF.DataStore.set(el,'xf-element-handlers',handlers);"
	"try{if(h.init)h.init();}catch(e){}"
	"return h;"
	"}"
	"function initEl(el){"
	"if(!el||!el.getAttribute)return;"
	"var attr=el.getAttribute('data-xf-init');"
	"if(!attr)return;"
	"var names=attr.split(' '),i,name,raw,opts;"
	"for(i=0;i<names.length;i++){"
	"name=names[i];if(!name)continue;"
	"raw=el.getAttribute('data-xf-'+name);"
	"opts=raw?JSON.parse(raw):{};"
	"applyHandler(el,name,opts);"
	"}}"
	"return{"
	"register:function(n,c){mapper.add(n,c);},"
	"extend:function(n,m){mapper.extend(n,m);},"
	"getObjectFromIdentifier:function(n){return mapper.getObjectFromIdentifier(n);},"
	"newHandler:function(proto){return XF.create(proto);},"
	"initialize:function(root){"
	"var els,i;"
	"try{if(root&&root.nodeType===1&&root.matches&&root.matches('[data-xf-init]'))initEl(root);}catch(e){}"
	"els=root?root.querySelectorAll('[data-xf-init]'):[];"
	"for(i=0;i<els.length;i++)initEl(els[i]);"
	"},"
	"initializeElement:initEl,"
	"applyHandler:applyHandler"
	"};"
	"})();"
	"XF.activate=function(el){XF.Element.initialize(el);};"
	"XF.on=function(el,ev,fn){if(el&&el.addEventListener)el.addEventListener(ev,fn,false);};"
	"XF.trigger=function(){};"
	"if(!XF.onPageLoad)XF.onPageLoad=function(){XF.activate(document);};"
	"var noop={initialize:function(){}};"
	"XF.NavDeviceWatcher=noop;XF.ActionIndicator=noop;"
	"XF.DynamicDate=noop;XF.KeepAlive=noop;"
	"XF.ScrollButtons=noop;XF.NavButtons=noop;"
	"XF.KeyboardShortcuts=noop;"
	"XF.LinkWatcher={initLinkProxy:function(){},initExternalWatcher:function(){}};"
	"XF.ExpandableContent={watch:function(){}};"
	"XF.LazyHandlerLoader={checkLazyRegistration:function(){},loadLazyHandlers:function(){}};"
	"XF.StickyHeader={cache:[]};"
	"XF.FormInput={initialize:function(){}};"
	"XF.display=function(){};"
	"XF.canonicalizeUrl=function(u){return'/'+u;};"
	"XF.config={url:{js:''}};"
	"XF.pageDisplayTime=Date.now?Date.now():0;"
	"XF.Event=(function(){"
	"return{"
	"newHandler:function(proto){return XF.create(proto);},"
	"extend:function(n,m){},"
	"register:function(n,h){},"
	"on:function(n,h){},"
	"off:function(n,h){},"
	"trigger:function(n,d){}"
	"};"
	"})();"
	"XF.setupHtmlInsert=function(h,cb){if(cb){try{cb(null,document,false);}catch(e){}}};"
	"XF.Animate={fadeUp:function(){},fadeDown:function(){},addClassTransitioned:function(){},removeClassTransitioned:function(el,cls,cb){if(cb)cb();}};"
	"XF.Transition=XF.Animate;"
	"XF.isCreatedContainer=function(){return false;};"
	"XF.ajax=function(m,u,d,ok,opts){"
	"var p={finally:function(fn){return p;},then:function(fn){return p;},catch:function(fn){return p;}};"
	"return p;"
	"};"
	"XF.redirect=function(){};"
	"XF.flashMessage=function(){};"
	"XF.customEvent=function(n,d){try{return new CustomEvent(n,{detail:d||{}});}catch(e){return {};}};"
	"XF.findRelativeIf=function(sel,el){"
	"try{if(sel&&el&&el.querySelector)return el.querySelector(sel);}catch(e){}"
	"return null;"
	"};"
	"XF.onDelegated=function(){};"
	"XF.isElementWithinDraftForm=function(){return false;};"
	"XF.phrase=function(k){return String(k);};"
	"XF.toCamelCase=function(s){return s.replace(/-([a-z])/g,function(m,c){return c.toUpperCase();});};"
	"XF.Push={isSupported:function(){return false;},registerWorker:function(){}};"
	"XF.smoothScroll=function(){};"
	"XF.Message=XF.Message||{};"
	"XF.MultiQuote=XF.MultiQuote||{init:function(){}};"
	"if(XF.ready)XF.ready(XF.onPageLoad);"
	"if(typeof console!=='undefined')console.log('[ms] core stub end XF.Element='+typeof XF.Element+' XF.Event='+typeof XF.Event);"
	"})();";

/* fixes476/478/479: editor stub — FroalaEditor shim + XF.Editor registration */
static const char s_xf_editor_stub[] =
	"(function(){"
	"function FroalaEditor(el,opts,cb){"
	"this._el=el;"
	"if(el&&el.style){"
	"el.style.display='block';"
	"el.style.visibility='visible';"
	"el.style.opacity='1';"
	"el.style.width='100%';"
	"el.style.minHeight='280px';"
	"el.style.height='320px';"
	"el.style.maxHeight='600px';"
	"el.style.padding='8px';"
	"el.style.border='2px solid #888';"
	"el.style.boxSizing='border-box';"
	"el.style.resize='vertical';"
	"el.style.backgroundColor='#fff';"
	"el.style.color='#000';"
	"el.style.fontFamily='monospace';"
	"el.style.fontSize='12px';"
	"}"
	"this.html={get:function(){return el?el.value:'';},set:function(v){if(el)el.value=v;}};"
	"this.events={on:function(){}};"
	"this.core={isEmpty:function(){return !el||!el.value;},injectStyle:function(){}};"
	"this.selection={save:function(){},restore:function(){},get:function(){return null;}};"
	"this.toolbar={show:function(){},hide:function(){}};"
	"this.opts=opts||{};"
	"if(cb){try{cb();}catch(e){}}"
	"}"
	"FroalaEditor.prototype.destroy=function(){};"
	"FroalaEditor.prototype.isInitialized=function(){return true;};"
	"FroalaEditor.extend=function(){};"
	"FroalaEditor.prototype.extend=function(){};"
	"FroalaEditor.LANGUAGE={xf:{direction:'ltr',translation:{}}};"
	"FroalaEditor.DefineIconTemplate=function(){};"
	"FroalaEditor.DefineIcon=function(){};"
	"FroalaEditor.RegisterPlugin=function(){};"
	"FroalaEditor.POPUP_TEMPLATES={};"
	"FroalaEditor.PLUGINS={};"
	"window.FroalaEditor=FroalaEditor;"
	"if(typeof XF!=='undefined'&&XF.Element&&XF.Element.newHandler&&XF.Element.register&&!XF.Editor){"
	"XF.Editor=XF.Element.newHandler({"
	"options:{maxHeight:0.7,minHeight:250,buttonsRemove:'',attachmentTarget:false,deferred:false},"
	"edMinHeight:63,form:null,ed:null,"
	"init:function(){"
	"try{"
	"var el=this.target;"
	"if(el&&el.tagName==='TEXTAREA'){"
	"this.form=(el.closest&&el.closest('form'))||null;"
	"this.startInit();"
	"}"
	"}catch(e){}"
	"},"
	"startInit:function(){"
	"var el=this.target,self=this;"
	"try{"
	"this.ed=new FroalaEditor(el,{},function(){if(self.editorInit)self.editorInit();});"
	"}catch(e){"
	"if(el&&el.style){el.style.display='block';el.style.visibility='visible';}"
	"}"
	"},"
	"editorInit:function(){},"
	"reInit:function(a){this.startInit(a);},"
	"isInitialized:function(){return this.ed!==null;},"
	"getValue:function(){return this.target?this.target.value:'';},"
	"destroy:function(){if(this.ed){this.ed.destroy();this.ed=null;}}"
	"});"
	"XF.Element.register('editor','XF.Editor');"
	"}"
	"try{"
	"var taAll=document.querySelectorAll('textarea');"
	"console.log('[ms] editor-stub textareas='+taAll.length);"
	"var i,t,cl,xi;"
	"for(i=0;i<taAll.length;i++){"
	"t=taAll[i];"
	"xi=(t.getAttribute&&t.getAttribute('data-xf-init'))||'';"
	"cl=(t.getAttribute&&t.getAttribute('class'))||'';"
	"if(xi.indexOf('editor')>=0||cl.indexOf('js-editor')>=0){"
	"if(cl.indexOf('u-jsOnly')>=0){"
	"cl=cl.replace(/(^|\\s)u-jsOnly(\\s|$)/g,' ');"
	"t.setAttribute('class',cl);"
	"}"
	"t.setAttribute('style',"
	"'display:block!important;visibility:visible!important;"
	"width:100%!important;min-height:280px!important;"
	"height:320px!important;padding:8px!important;"
	"border:2px solid #888!important;box-sizing:border-box!important;"
	"background:#fff!important;color:#000!important;"
	"font-family:monospace!important;font-size:12px!important');"
	"var p=t.parentNode,d=0,pc;"
	"while(p&&d<8){"
	"if(p.getAttribute){"
	"pc=p.getAttribute('class')||'';"
	"if(pc.indexOf('u-jsOnly')>=0||pc.indexOf('is-hidden')>=0){"
	"pc=pc.replace(/(^|\\s)(u-jsOnly|is-hidden)(\\s|$)/g,' ');"
	"p.setAttribute('class',pc);"
	"}"
	"var ps=p.getAttribute('style')||'';"
	"if(ps.indexOf('display')>=0&&ps.indexOf('none')>=0){"
	"p.setAttribute('style',ps.replace(/display\\s*:\\s*none[^;]*;?/g,'display:block!important;'));"
	"}"
	"}"
	"p=p.parentNode;d++;"
	"}"
	"console.log('[ms] editor textarea revealed name='+(t.name||'-'));"
	"}"
	"}"
	"}catch(e){console.log('[ms] reveal err '+e);}"
	"})();";

/* -----------------------------------------------------------------------
 * Transpile cache — avoids re-transpiling the same versioned bundle
 * on every page navigation (editor-compiled.js is 733 KB and takes ~7s).
 * ----------------------------------------------------------------------- */
#define QJS_ES6_CACHE_MAX 16

struct qjs_es6_cache_entry {
	char         *name;
	unsigned long orig_len;
	char         *out;
	size_t        out_len;
};

static struct qjs_es6_cache_entry g_qjs_cache[QJS_ES6_CACHE_MAX];
static int g_qjs_cache_count = 0;

static const char *
qjs_cache_lookup(const char *name, unsigned long orig_len, size_t *out_len_p)
{
	int i;
	if (name == NULL) return NULL;
	for (i = 0; i < g_qjs_cache_count; i++) {
		if (g_qjs_cache[i].orig_len == orig_len &&
		    g_qjs_cache[i].name != NULL &&
		    strcmp(g_qjs_cache[i].name, name) == 0) {
			if (out_len_p) *out_len_p = g_qjs_cache[i].out_len;
			return g_qjs_cache[i].out;
		}
	}
	return NULL;
}

static void
qjs_cache_store(const char *name, unsigned long orig_len,
		const char *out, size_t out_len)
{
	struct qjs_es6_cache_entry *e;
	char *name_copy;
	char *out_copy;
	if (g_qjs_cache_count >= QJS_ES6_CACHE_MAX) return;
	if (name == NULL || out == NULL) return;
	name_copy = (char *)malloc(strlen(name) + 1);
	out_copy  = (char *)malloc(out_len + 1);
	if (name_copy == NULL || out_copy == NULL) { free(name_copy); free(out_copy); return; }
	strcpy(name_copy, name);
	memcpy(out_copy, out, out_len);
	out_copy[out_len] = '\0';
	e = &g_qjs_cache[g_qjs_cache_count++];
	e->name     = name_copy;
	e->orig_len = orig_len;
	e->out      = out_copy;
	e->out_len  = out_len;
}

/* Helper: eval a C string in ctx, log errors, return 1 on success */
static int
qjs_eval_cstr(JSContext *ctx, const char *src, const char *tag)
{
	JSValue v = JS_Eval(ctx, src, strlen(src), tag, JS_EVAL_TYPE_GLOBAL);
	int ok = !JS_IsException(v);
	if (!ok) {
		JSValue exc = JS_GetException(ctx);
		const char *str = JS_ToCString(ctx, exc);
		macsurf_debug_log_writef("qjs stub err [%s]: %s", tag, str ? str : "?");
		if (str) JS_FreeCString(ctx, str);
		JS_FreeValue(ctx, exc);
	}
	JS_FreeValue(ctx, v);
	return ok;
}

/* Transpile budget (legacy): QuickJS handles ES6+ natively, so this path
 * is effectively unused. */
#define QJS_TRANSPILE_CAP (256 * 1024)

unsigned char js_exec(struct jsthread *thread,
		const unsigned char *txt, size_t txtlen,
		const char *name)
{
	JSValue val;
	int ok;
	char *src;

	/* fixes847 (#167 S1 census gap) — the other half of the js_exec
	 * visibility fix below: if thread/ctx is NULL, js_exec bails before
	 * even reaching the entry breadcrumb, and NOTHING about this script
	 * is ever logged. Make that visible too, so "zero js activity" in a
	 * hardware log can be told apart from "no thread was ever wired". */
	if (thread == NULL || thread->ctx == NULL) {
		macsurf_debug_log_writef("WORK js exec: no thread/ctx [%s]",
				name ? name : "(anon)");
		return 0;
	}
	if (txt == NULL || txtlen == 0) return 1;

	/* fixes587 BISECTION DIAG: short-circuit ALL script execution. With this
	 * on, no JS_Eval / thrown exception / build_backtrace / DOM-wrapper /
	 * timer work runs at all — the scripts are treated as clean no-ops so the
	 * parser resumes normally. Purpose: split a JS-side heap corruptor from a
	 * fetch/parse/content-side one on the 100%-deterministic tinkerdifferent
	 * freeze. If it STILL hard-freezes with this on, the corruptor is NOT in
	 * the JS path (look at fetch/parse/schedule). If it LOADS, the corruptor
	 * IS in JS exec/exception/wrapper/timer. Flip to 0 to restore JS. */
	{
		static int g_diag_disable_js = 0;   /* diagnostics off on branch; bisection ships separately */
		if (g_diag_disable_js) {
			macsurf_debug_log_writef(
				"js: EXEC DISABLED (bisect diag) [%s len=%ld]",
				name ? name : "(anon)", (long)txtlen);
			return 1;
		}
	}

	/* Hard size cap */
	if (txtlen > 4194304UL) {
		/* fixes847 (#167 S1 census gap) — WORK-prefixed: this line was
		 * plain macsurf_debug_log_writef, silently dropped by the
		 * failures-only release filter (only WORK/NAV/FAIL/etc. survive
		 * it — see the log-visibility-traps gotcha). A hardware log
		 * showing zero js activity of ANY kind on facebook.com could not
		 * be told apart from "a bundle silently hit this cap" without
		 * this being visible. */
		macsurf_debug_log_writef("WORK js skip [%s len=%ld > 4MB]",
			name ? name : "(anon)", (long)txtlen);
		return 0;
	}

	/* fixes523 DIAGNOSTIC: fingerprint the exact bytes handed to QuickJS so
	 * we can distinguish source corruption from an engine parse bug.  These
	 * bundles are valid pure-ASCII JS host-side, yet QuickJS reports
	 * SyntaxError / invalid-UTF-8 on them.  Logs len + 32-bit byte-sum (as
	 * hex via %p) + first 40 printable bytes; compare to the canonical
	 * fingerprint.  Remove once the cause is pinned.
	 * fixes847 (#167 S1 census gap) — WORK-prefixed. This is js_exec's
	 * OWN entry breadcrumb, the single line that answers "did any script
	 * on this page get to QuickJS at all" -- it was plain
	 * macsurf_debug_log_writef, silently dropped by the failures-only
	 * release filter. A hardware log against real Facebook (2026-07-16,
	 * fixes846 build) showed real .js bundles downloading (fbresp
	 * status=200) but ZERO qjs/reconvert/xhr/fetch activity of any kind
	 * -- with this line gated, that log is genuinely ambiguous between
	 * "no script ever executed" and "scripts ran cleanly but never
	 * touched fetch/XHR/DOM". This makes the next log unambiguous. */
	{
		const unsigned char *dp = (const unsigned char *)txt;
		unsigned long dsum = 0;
		size_t di;
		char dhead[41];
		int dn = (int)(txtlen < 40 ? txtlen : 40);
		for (di = 0; di < txtlen; di++) dsum += (unsigned long)dp[di];
		for (di = 0; di < (size_t)dn; di++) {
			unsigned char b = dp[di];
			dhead[di] = (b >= 32 && b < 127) ? (char)b : '.';
		}
		dhead[dn] = '\0';
		macsurf_debug_log_writef("WORK js src [%s] len=%ld sum=%p head=%s",
			name ? name : "?", (long)txtlen, (void *)dsum, dhead);
	}

	/* fixes648 (regression fix): restore the per-bundle XenForo ES5 stub
	 * substitution that fixes522 removed. QuickJS ran the real preamble /
	 * core / editor bundles natively, but they CRASH on this engine — the
	 * documented cascade: preamble.min.js's `div.parentNode` hiddenscroll
	 * probe reads null and throws -> jQuery's Sizzle self-test throws ->
	 * core-compiled.js dies on jQuery.support -> XF.Element is never
	 * registered -> editor-compiled.js throws "newHandler of undefined". Net
	 * effect on hardware: the reply/post editor collapses to a bare one-line
	 * textarea (has-js never set, Froala never reveals/sizes it) — the "post
	 * box is shrunk/missing" report. The ES5 stubs still in this file sidestep
	 * all of it: s_xf_preamble_stub sets has-js + XF.ready, s_xf_core_stub
	 * defines XF.Element/XF.create, s_xf_editor_stub reveals+sizes the editor
	 * textarea (display:block, minHeight 280px). Substitute by script name;
	 * the real (crashing) bundle never runs. jQuery is left to run natively
	 * (the stubs are jQuery-independent), so its Sizzle crash is isolated to
	 * its own eval and does not block the editor. This was the fixes476-481
	 * mechanism that made real forum replies post on 68kmla under Duktape. */
	if (name != NULL) {
		const char *stub = NULL;
		const char *tag = NULL;
		if (strstr(name, "editor-compiled") != NULL) {
			stub = s_xf_editor_stub; tag = "editor";
		} else if (strstr(name, "preamble.min.js") != NULL) {
			stub = s_xf_preamble_stub; tag = "preamble";
		} else if (strstr(name, "core-compiled.js") != NULL) {
			stub = s_xf_core_stub; tag = "core";
		}
		/* fixes670 (perf): these XenForo feature bundles depend on XF internals
		 * we don't fully provide, so on hardware they PARSE (lightbox-compiled.js
		 * alone is ~155 KB) and then throw immediately (TypeError: cannot read
		 * property 'handle'/'extend' of undefined) — a big, pure-waste slice of
		 * the per-page js= time. Substitute an empty no-op so the parse+exec is
		 * skipped entirely; the features (image lightbox, media gallery, upload,
		 * token input, prefix menu) don't work either way, so nothing is lost.
		 * Only applied AFTER the real-stub checks above so preamble/core/editor
		 * still get their functional stubs. Add doomed bundles here as they
		 * surface in the qjs-exec-err log. */
		else if (strstr(name, "lightbox-compiled") != NULL ||
			 strstr(name, "/xfmg/") != NULL ||
			 strstr(name, "attachment_manager") != NULL ||
			 strstr(name, "token_input") != NULL ||
			 strstr(name, "prefix_menu") != NULL) {
			stub = ""; tag = "skip-doomed";
		}
		if (stub != NULL) {
			JSValue sv;
			macsurf_debug_log_writef(
				"js XF stub INJECT: %s for [%s] (real bundle skipped)",
				tag, name);
			sv = JS_Eval(thread->ctx, stub, strlen(stub), name,
					JS_EVAL_TYPE_GLOBAL);
			if (JS_IsException(sv)) {
				JSValue exc = JS_GetException(thread->ctx);
				const char *estr = JS_ToCString(thread->ctx, exc);
				macsurf_debug_log_writef("js XF stub %s ERR: %s",
					tag, estr ? estr : "?");
				if (estr != NULL) JS_FreeCString(thread->ctx, estr);
				JS_FreeValue(thread->ctx, exc);
			} else {
				macsurf_debug_log_writef("js XF stub %s OK", tag);
			}
			JS_FreeValue(thread->ctx, sv);
			return 1;
		}
	}

	/* fixes522: accept ALL JavaScript.  Run every script through QuickJS
	 * natively (ES2023) — no per-filename stubs (preamble/core/editor/
	 * upload), no ES6->ES5 transpiler, no transpile cache.  Those were
	 * legacy crutches; in particular the old transpiler's async/await
	 * strip silently corrupted minified bundles (turning `asyncFoo()` into
	 * `     Foo()` -> "F is not defined").  Real scripts run as-is and
	 * compatibility gaps get fixed in the engine, not papered over per-site.
	 * The deadline (here) + memory limit (js_newheap) keep a misbehaving
	 * script from hanging or OOMing the machine. */
	/* fixes524: ROOT CAUSE.  QuickJS JS_Eval REQUIRES a NUL-terminated
	 * buffer — quickjs.h: "'input' must be zero terminated i.e.
	 * input[input_len] = '\0'".  The fetched script source from
	 * content_get_source_data is a raw byte buffer that is NOT
	 * NUL-terminated, so the lexer ran off the end into uninitialized heap,
	 * producing SyntaxErrors near end-of-file that varied between runs
	 * (reading heap garbage).  THIS — not ES6, corruption, or a CW8
	 * miscompile — is why real bundles never ran; the C-string-literal init
	 * evals were already NUL-terminated, so they worked.  Copy + terminate. */
	src = (char *)malloc(txtlen + 1);
	if (src == NULL) {
		macsurf_debug_log_writef("js: OOM copying src [%s len=%ld]",
			name ? name : "(anon)", (long)txtlen);
		return 0;
	}
	memcpy(src, txt, txtlen);
	src[txtlen] = '\0';

	/* fixes586 — push/pop (nest-safe) instead of set/clear-to-0, so a
	 * re-entrant exec can never erase an outer deadline. */
	{
		/* fixes640 — accumulate JS execution CPU per top-level eval. */
		extern double macos9_micros(void);
		extern void macsurf_profile_accum_js(long us);
		double prevdl = qjs_deadline_push((double)QJS_SCRIPT_TIMEOUT_MS);
		double t_js = macos9_micros();
		val = JS_Eval(thread->ctx, src, txtlen,
				name ? name : "<script>", JS_EVAL_TYPE_GLOBAL);
		macsurf_profile_accum_js((long)(macos9_micros() - t_js));
		qjs_deadline_pop(prevdl);
	}
	free(src);
	ok = !JS_IsException(val);
	if (!ok) {
		JSValue exc = JS_GetException(thread->ctx);
		const char *estr = JS_ToCString(thread->ctx, exc);
		/* fixes843b (#167 S1 census) — "err" (lowercase) never matched the
		 * crash-only log gate's "ERROR" (uppercase) keyword, so every JS
		 * exception on every page has been silently invisible in a normal
		 * build since fixes765. WORK-prefix it so a census build actually
		 * shows what's throwing. Remove the WORK prefix once the census
		 * round is done (this is deliberately loud -- one line per failed
		 * script, which is the whole point right now). */
		/* fixes873 — the MESSAGE goes FIRST, and the script name is shortened.
		 * macsurf_debug_log_writef hard-caps output at 255 bytes, and a modern
		 * script URL is far longer than that on its own:
		 *   .../jetpack_vendor/automattic/jetpack-mu-wpcom/src/build/
		 *   verbum-comments/verbum-comments.js?m=1783962184i&minify=false&ver=...
		 * so with the name first, the line spent its whole budget on the URL and
		 * the actual exception was truncated to "Erro". The one thing this line
		 * exists to say was the one thing it could never say -- and on the
		 * biggest script on the page, which is exactly where it's needed. */
		{
			char sname[48];
			qjs_short_name(name, sname, (int)sizeof(sname));
			macsurf_debug_log_writef("WORK qjs exec err: %s [%s len=%ld]",
				estr ? estr : "?", sname, (long)txtlen);
		}
		if (estr) JS_FreeCString(thread->ctx, estr);

		/* fixes581 DIAG: bracket the stack-extraction block. tinkerdifferent
		 * freezes right after 'qjs exec err' for ripple.min.js (the only script
		 * whose 'qjs stack' line never prints), so this block is the prime
		 * suspect. If the log ends after 'qjs: pre-stack' with no 'qjs: post-
		 * stack', the hang is inside JS_GetPropertyStr/JS_ToCString on the
		 * exception's .stack. */
		macsurf_debug_log_writef("qjs: pre-stack [%s]", name ? name : "?");

		/* fixes522: surface the JS stack so compatibility holes (missing
		 * DOM/BOM API, etc.) are easy to pinpoint and fix in the engine. */
		{
			JSValue stk = JS_GetPropertyStr(thread->ctx, exc, "stack");
			if (JS_IsString(stk)) {
				const char *ss = JS_ToCString(thread->ctx, stk);
				if (ss != NULL) {
					/* fixes873 — stack FIRST, short name after: same
					 * 255-byte-cap trap as the message line above, and a
					 * truncated stack is worth even less than none. */
					char sname[48];
					qjs_short_name(name, sname, (int)sizeof(sname));
					macsurf_debug_log_writef("WORK qjs stack: %s [%s]",
						ss, sname);
					JS_FreeCString(thread->ctx, ss);
				}
			}
			JS_FreeValue(thread->ctx, stk);
		}

		macsurf_debug_log_writef("qjs: post-stack [%s]", name ? name : "?");

		JS_FreeValue(thread->ctx, exc);
		JS_FreeValue(thread->ctx, val);
		macsurf_debug_log_writef("qjs: exec-return0 [%s]", name ? name : "?");
		return 0;
	}
	JS_FreeValue(thread->ctx, val);
	return 1;
}

unsigned char js_fire_event(struct jsthread *thread, const char *type,
		struct dom_document *doc, struct dom_node *target)
{
	(void)doc; (void)target;
	if (thread == NULL || thread->ctx == NULL || type == NULL) return 0;
	/* Fire window.dispatchEvent(new Event(type)) */
	{
		/* fixes603 — buffer/guard mismatch overflow: bytes written are
		 * 48(prefix) + tlen + 20(suffix) + 1(NUL) = 69 + tlen, but the guard
		 * was tlen<80 against a 128-byte buffer, so a 60-79 char event type
		 * wrote up to 20 bytes past script[]. Enlarged buffer + correct guard. */
		char script[256];
		size_t tlen = strlen(type);
		if (tlen < 180) {
			memcpy(script,
				"(function(){try{window.dispatchEvent(new Event('", 48);
			memcpy(script + 48, type, tlen);
			memcpy(script + 48 + tlen, "'));}catch(e){}})();", 20);
			script[48 + tlen + 20] = '\0';
			macsurf_qjs__safe_eval(thread->ctx, script);
		}
	}
	return 1;
}

/* fixes652: real-build definition of interaction.c's click bridge (Gate 5).
 * The js_stub.c copy is gated `#ifndef WITH_QUICKJS`, so with QuickJS ON the
 * symbol was undefined and interaction.c failed to link the moment it was
 * rebuilt. Return 0 ("JS did not call preventDefault") so the browser's
 * navigation / form submit proceeds unchanged; per-element onclick handlers
 * still fire via fire_generic_dom_event at the call site. */
int macsurf_qjs_dispatch_dom_click(struct dom_node *target)
{
	(void)target;
	return 0;
}

/* GATE 3: dispatch DOMContentLoaded then load into the JS *document*'s
 * registered listeners (document._listeners, installed by the shim at
 * register_browser_globals).  js_fire_event only ever reaches window
 * listeners, so without this XenForo's preamble.min.js DOMContentLoaded
 * handler never runs, XF.ready()'s queue never drains, and XF.activate
 * (document) is never called.  Call this once, after the initial box tree
 * exists.  Idempotent per realm via document.__ms_ready_fired; the realm is
 * rebuilt per navigation (js_newthread) so the flag resets automatically. */
unsigned char js_fire_dom_ready(struct jsthread *thread, struct dom_document *doc)
{
	static const char s_dom_ready_src[] =
		"(function(){try{"
		"if(typeof document==='undefined')return;"
		"if(document.__ms_ready_fired)return;"
		"document.__ms_ready_fired=true;"
		"document.readyState='complete';"
		"try{document.dispatchEvent(new Event('DOMContentLoaded'));}catch(e){}"
		"try{if(typeof window!=='undefined')"
		"window.dispatchEvent(new Event('DOMContentLoaded'));}catch(e){}"
		"try{document.dispatchEvent(new Event('load'));}catch(e){}"
		"}catch(e){}})();";
	(void)doc;
	if (thread == NULL || thread->ctx == NULL) {
		return 0;
	}
	macsurf_qjs__safe_eval(thread->ctx, s_dom_ready_src);
	/* fixes862 (#289 probe) — was "qjs: DOMContentLoaded+load fired to
	 * document", which the failures-only gate DROPS (macsurf_debug_log.c:
	 * only "WORK " and genuine failures survive), so this has been invisible
	 * on every default build. That matters now: hackaday's comment iframe
	 * loads its form from inside
	 *     window.addEventListener("DOMContentLoaded", ...)
	 * so if this never fires for the IFRAME's realm, its entire loader body
	 * -- querySelector, IntersectionObserver, loadScript, fetch -- never runs,
	 * which is exactly what the log shows (WORK xhr = 0, no injected script).
	 * ctx distinguishes the realms: the main page and the iframe are separate
	 * heaps, so two different ctx values must appear here. Only one = the
	 * iframe never gets DOMContentLoaded. */
	macsurf_debug_log_writef("WORK domready fired ctx=%p doc=%p",
			(void *)thread->ctx, (void *)doc);
	return 1;
}

/* fixes869 (#295) — fire `load` / `error` AT a <script> element.
 *
 * The universal dynamic-loader idiom is:
 *     const s = document.createElement('script');
 *     s.onload  = () => resolve();      // resolves the caller's Promise
 *     s.onerror = (e) => reject(e);
 *     s.src = url; document.body.appendChild(s);
 * Nothing ever fired those events, so the caller's promise never settled and
 * its chain stalled forever.  HW-confirmed on hackaday after fixes868: the
 * injected wp-polyfill reaches dom_SCRIPT_showed_up (flags=4), runs ASYNC and
 * EXECUTES -- and then `loadWPScript('wp-polyfill').then(() => loadWPScript('verbum'))`
 * never advances, so verbum-comments.js is never requested.
 *
 * Fires BOTH shapes because they are different registries: `s.onload = fn` is a
 * plain property on the wrapper, while addEventListener('load') lands in the
 * element's own `_L` map that el.dispatchEvent walks.  A loader may use either.
 *
 * Realm: `thread` is the script's OWNING content's js_thread (html_script_exec
 * passes c->js_thread), so this always runs in the right runtime -- passing a
 * JSValue across runtimes is the fixes854 crash.
 *
 * Ref discipline: qjs_wrap_element CONSUMES an owned ref on both the hit and
 * miss paths, and our caller (struct html_script.node) keeps its own ref for
 * later teardown -- so take a fresh ref FOR the wrap, or the wrapper's adopt
 * and html_script_free's unref would both claim the same one. */
/* fixes873 (#301) — document.currentScript.
 *
 * The FIRST thing webpack's publicPath runtime reaches for:
 *     t.currentScript && "SCRIPT" === t.currentScript.tagName.toUpperCase()
 *         && (e = t.currentScript.src)
 * and if that yields nothing it falls to getElementsByTagName("script") and then
 * throws outright. We had no currentScript at all (0 references anywhere), so
 * every webpack bundle -- which is most of the modern web -- threw on its own
 * runtime prologue before reaching a single line of application code.
 *
 * Per spec this is set for the duration of a script's execution and restored
 * afterwards (it nests: a script that synchronously runs another must get its
 * own value back), hence set/clear around script_handler in html_script_exec
 * rather than a write-once.
 *
 * `node` NULL clears it to null, which is also the correct value outside script
 * execution and for a script running from a callback.
 *
 * (The tagName.toUpperCase() above is why #299 -- our lowercase tagName -- does
 * not bite here. It would bite a bundler that compared tagName directly.)
 */
void js_set_current_script(struct jsthread *thread, struct dom_node *node)
{
	JSContext *ctx;
	JSValue global;
	JSValue doc;
	JSValue el;

	if (thread == NULL || thread->ctx == NULL) return;
	ctx = thread->ctx;

	global = JS_GetGlobalObject(ctx);
	doc = JS_GetPropertyStr(ctx, global, "document");
	if (JS_IsUndefined(doc) || JS_IsNull(doc)) {
		JS_FreeValue(ctx, doc);
		JS_FreeValue(ctx, global);
		return;
	}

	if (node == NULL) {
		el = JS_NULL;
	} else {
		macsurf_dom_node_ref(node);	/* the wrap CONSUMES a ref */
		el = qjs_wrap_element(ctx, (dom_element *)node);
	}
	JS_SetPropertyStr(ctx, doc, "currentScript", el);

	JS_FreeValue(ctx, doc);
	JS_FreeValue(ctx, global);
}

unsigned char js_fire_script_load(struct jsthread *thread,
		struct dom_node *node, int ok)
{
	/* fixes872 (#300) — dispatchEvent now fires BOTH the addEventListener list
	 * and the on* handler, so this must NOT also call el['on'+type] itself: that
	 * would run an onload handler TWICE, and a loader whose promise resolves
	 * there would resolve twice. Only fall back to the direct property call when
	 * the target has no dispatchEvent at all -- i.e. a JS FALLBACK element (mkfb,
	 * used before a document is wired), which is a plain object with no
	 * prototype accessors and no _H map, so on* is a bare expando on it. */
	static const char s_fire_src[] =
		"(function(el,type){try{"
		"var ev={type:type,target:el,currentTarget:el,"
			"bubbles:false,cancelable:false,defaultPrevented:false,"
			"preventDefault:function(){},stopPropagation:function(){}};"
		"if(typeof el.dispatchEvent==='function'){"
			"try{el.dispatchEvent(ev);}catch(e){}"
		"}else{"
			"var h=el['on'+type];"
			"if(typeof h==='function'){try{h.call(el,ev);}catch(e){}}"
		"}"
		"}catch(e){}})";
	JSContext *ctx;
	JSValue fn, el, args[2], ret;

	if (thread == NULL || thread->ctx == NULL || node == NULL) return 0;
	ctx = thread->ctx;

	fn = JS_Eval(ctx, s_fire_src, strlen(s_fire_src),
			"<script-load>", JS_EVAL_TYPE_GLOBAL);
	if (JS_IsException(fn)) { JS_FreeValue(ctx, fn); return 0; }

	macsurf_dom_node_ref(node);	/* the wrap consumes this one */
	el = qjs_wrap_element(ctx, (dom_element *)node);
	if (JS_IsNull(el) || JS_IsUndefined(el)) {
		JS_FreeValue(ctx, el);
		JS_FreeValue(ctx, fn);
		return 0;
	}

	args[0] = el;
	args[1] = JS_NewString(ctx, ok ? "load" : "error");
	ret = JS_Call(ctx, fn, JS_UNDEFINED, 2, (JSValueConst *)args);
	if (JS_IsException(ret)) {
		JSValue exc = JS_GetException(ctx);
		const char *s = JS_ToCString(ctx, exc);
		macsurf_debug_log_writef("WORK script %s handler exc: %s",
				ok ? "load" : "error", s ? s : "?");
		if (s) JS_FreeCString(ctx, s);
		JS_FreeValue(ctx, exc);
	}
	JS_FreeValue(ctx, ret);
	JS_FreeValue(ctx, args[1]);
	JS_FreeValue(ctx, el);
	JS_FreeValue(ctx, fn);

	macsurf_debug_log_writef("WORK script fired %s node=%p",
			ok ? "load" : "error", (void *)node);
	return 1;
}

void js_handle_new_element(struct jsthread *thread, struct dom_element *node)
{
	(void)thread; (void)node;
}

void js_event_cleanup(struct jsthread *thread, struct dom_event *evt)
{
	(void)thread; (void)evt;
}

/* fixes861 (#289) — pump EVERY live heap, not just g_heap.
 *
 * This is the one timer pump in the browser (main.c's event loop calls it).  It
 * used to do `tmp.qctx = g_heap->ctx; run_timers(&tmp);` -- a single heap.  That
 * was survivable only while run_timers fired every slot in the global arena
 * regardless of owner: the "wrong" ctx still dragged the other heap's timers
 * along.  fixes854 stopped that (it was freeing/JS_Calling JSValues against a
 * foreign runtime -- the js_shape_hash_unlink crash) and correctly gated every
 * slot on `t->ctx == qctx`.  Correct, but it left this pump single-heap: with
 * g_heap only ever the most-RECENTLY-created heap, and js_newheap() running per
 * window AND per (i)frame (browser_window.c:3373), exactly ONE heap's timers
 * could still fire.  Any page with an iframe had a frozen realm -- and which
 * one flipped depending on creation order.
 *
 * HW-observed on hackaday.com: the Jetpack comment iframe's dynamic-loader.js
 * calls IntersectionObserver.observe(#commentform), whose delivery is a
 * setTimeout(...,0) (see the IntersectionObserver shim).  The iframe's heap was
 * not g_heap, so that timer never ran, the IO callback never delivered,
 * WP_Enqueue_Dynamic_Script.loadScript('verbum') was never called, and
 * verbum-comments.js was never even fetched -- the reply box rendered as an
 * empty Preact mount (iframe box tree = 6 boxes).  Confirmed by the fixes860
 * probe: every script reaching dom_SCRIPT_showed_up was flags=6
 * (PARSER_INSERTED|NON_BLOCKING) i.e. parser-created and correctly skipped, with
 * ZERO flags=4 JS-injected ones, and by `WORK xhr` = 0 in the log (fetch() never
 * ran, and macos9_js_fetch.c logs every send).
 *
 * Walking the list is safe against a callback creating a heap (new heaps are
 * prepended, so `h` and its tail stay valid) but NOT against one destroying a
 * heap mid-walk, so read `next` BEFORE running the timers. */
void macsurf_qjs_pump_all(void)
{
	struct jsheap *h = g_heap_list;
	/* fixes862 (#289 probe) — fixes861 shipped with NO observable marker, so
	 * there was no way to tell from a log whether it was even in the build,
	 * let alone whether a second (iframe) heap exists to pump. Log the heap
	 * count, but ONLY when it changes: this runs every event-loop pass, so an
	 * unconditional line would drown the log. heaps>=2 on an iframe page also
	 * confirms browser_window_initialise_common (frames.c:232 -> js_newheap)
	 * really does give iframes their own heap -- an assumption fixes861 rests
	 * on and which I have not otherwise verified on hardware. */
	static int s_last_heaps = -1;
	int heaps = 0;
	{
		struct jsheap *c = g_heap_list;
		while (c != NULL) { heaps++; c = c->next; }
	}
	if (heaps != s_last_heaps) {
		s_last_heaps = heaps;
		macsurf_debug_log_writef("WORK pump heaps=%d", heaps);
	}
	while (h != NULL) {
		struct jsheap *next = h->next;
		if (h->ctx != NULL) {
			struct jscontext tmp;
			tmp.qctx = h->ctx;
			tmp.qrt  = h->rt;
			tmp.win_priv = NULL;
			tmp.doc_priv = NULL;
			macsurf_qjs_run_timers(&tmp);
		}
		/* fixes868 (#294) — DRAIN THE MICROTASK QUEUE.
		 *
		 * QuickJS does not run Promise reactions itself: `resolve(v)` only
		 * ENQUEUES the .then() callbacks as pending jobs, and the host must
		 * pump them with JS_ExecutePendingJob().  Nothing in MacSurf ever
		 * called it -- so since the day Promises were enabled, EVERY promise
		 * chain in the browser has been dead past its first resolve().  Not a
		 * hackaday bug: it is every modern site, because ~all modern JS is
		 * promise-driven.
		 *
		 * It hid because the shapes that DO work look like promises but are
		 * not: xhr.onreadystatechange and setTimeout callbacks are direct
		 * JS_Calls (which is why the HW log shows `WORK fetch ok=1 status=200`
		 * -- that line is emitted from onreadystatechange -- while the .then()
		 * on the very same fetch never fires), and a .then() whose promise is
		 * already settled *inside* the same JS_Eval still needs a job cycle it
		 * never gets.  On hackaday: fetch resolves, then
		 *   loadExtra(h,'translations').then(...).then(() => loadExternalScript(h))
		 * never advances, so createElement/appendChild are never reached --
		 * which is exactly why the log shows ZERO appends AND zero append
		 * failures (fixes867 proved the failure probe is live).
		 *
		 * PER-RUNTIME: jobs belong to a JSRuntime, and every heap has its own
		 * (fixes854/861), so each is drained separately -- passing one runtime's
		 * jobs to another is the cross-runtime crash all over again.
		 *
		 * Bounded: a job may enqueue another (a .then() chain), so an unbounded
		 * `while (JS_IsJobPending)` lets a promise loop starve the cooperative
		 * event loop -- the same class of hang as the fixes608 timer cycle.
		 * QJS_MAX_JOBS_PER_PUMP caps one pass; leftovers run on the next poll,
		 * which is what a real browser's task/microtask split does anyway. */
		if (h->rt != NULL) {
			int jobs = 0;
			while (JS_IsJobPending(h->rt) && jobs < QJS_MAX_JOBS_PER_PUMP) {
				JSContext *jctx = NULL;
				int r = JS_ExecutePendingJob(h->rt, &jctx);
				if (r < 0) {
					/* Uncaught rejection / job threw: surface it, keep
					 * draining -- one bad job must not stall the queue. */
					if (jctx != NULL) {
						JSValue exc = JS_GetException(jctx);
						const char *s = JS_ToCString(jctx, exc);
						macsurf_debug_log_writef("WORK job exc: %s",
								s ? s : "?");
						if (s) JS_FreeCString(jctx, s);
						JS_FreeValue(jctx, exc);
					}
				} else if (r == 0) {
					break;		/* queue empty */
				}
				jobs++;
			}
			if (jobs >= QJS_MAX_JOBS_PER_PUMP) {
				macsurf_debug_log_writef(
					"WORK job pump: hit cap %d, deferring rest",
					(int)QJS_MAX_JOBS_PER_PUMP);
			}
		}
		h = next;
	}
}

#endif /* WITH_QUICKJS */
