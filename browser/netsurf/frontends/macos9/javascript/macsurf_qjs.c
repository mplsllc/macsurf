/*
 * MacSurf - macsurf_qjs.c
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
#include "macsurf_qjs_audit.h"
#include "macsurf_timebase.h"
#include "macos9_js_fetch.h"
/* fixes1245 (#167) - plot_font_style_t/gui_layout_table for canvas
 * measureText's real (not fabricated) width query. Neither was pulled in
 * transitively by anything already included here -- checked by trying
 * without them first, not assumed. */
#include "netsurf/plot_style.h"
#include "netsurf/layout.h"
#include "content/handlers/html/private.h"
/* fixes1011 (Phase 3) - the box tree, for the layout metrics. box.h defines
 * struct box and the LEFT/RIGHT/TOP/BOTTOM edge indices; box_inspect.h has
 * box_coords(); box_construct.h has box_for_node(). */
#include "content/handlers/html/box.h"
#include "content/handlers/html/box_inspect.h"
#include "content/handlers/html/box_construct.h"
/* fixes1013 - corestring_dom_scroll / _resize for the scroll/resize fan-out. */
#include "utils/corestrings.h"
#include "utils/libdom.h"
/* fixes879 - document.cookie against the real jar. urldb owns NetSurf's
 * RFC-6265 cookie store; content_protected.h is what makes c->llcache visible
 * for the content_get_url() NULL guard (same include set macos9_js_fetch.c
 * uses for the same guard). */
#include "utils/nsurl.h"
#include "content/urldb.h"
#include "content/content_protected.h"
#include "content/content_factory.h"
#include "macos9_reconvert.h"

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
	/* fixes875 (#304) - monotonic generation of `ctx`, bumped every time a new
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
	/* fixes861 (#289) - every live heap, so macsurf_qjs_pump_all() can pump
	 * ALL of them.  js_newheap() runs per browser_window AND per (i)frame
	 * (browser_window.c:3373), so "the heap" has never been a real thing on a
	 * page with an iframe; g_heap is only ever the most-RECENTLY-created one.
	 * See the note on macsurf_qjs_pump_all(). */
	struct jsheap *next;
	/* fixes1117b (#265) - per-heap module source registry for ES module
	 * imports. The module loader callback checks this list before trying the
	 * disk cache.  Populated by js_exec_module for inline scripts. */
	struct module_registry *module_reg;
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
struct jsheap *g_heap = NULL;  /* exported for audit */

/* fixes861 (#289) - every live heap, newest first.  js_newheap() links,
 * js_destroyheap() unlinks.  Exists so macsurf_qjs_pump_all() can pump all of
 * them; see the note there for why pumping only g_heap froze iframes. */
static struct jsheap *g_heap_list = NULL;

/* ------------------------------------------------------------------ */
/* Interrupt handler - Cmd-. on OS 9                                   */
/* ------------------------------------------------------------------ */

/* fixes522: time-based runaway guard.  g_qjs_script_deadline is the
 * monotonic-ms time after which the currently-running top-level script is
 * aborted, so an infinite loop or pathological bundle can't hang the
 * cooperative event loop indefinitely.  0 == no deadline (init / internal
 * evals run unbounded).  Set around the top-level JS_Eval in js_exec. */
double macsurf_qjs_get_now(void);
/* fixes999 - the deadlines are OFF by default. They were runaway guards, and
 * on a 400 MHz G3 a legitimately heavy bundle can exceed a 20s budget, so the
 * guard fires on WORKING code and reports as a browser bug: the script is
 * aborted mid-init, half a framework exists, and the page fails in a way that
 * looks like a missing feature. That is the opposite of what a limit is for,
 * and it makes real sites untestable.
 *
 * Safe to remove because the ESCAPE HATCH IS REAL AND INDEPENDENT: the
 * interrupt handler polls WaitNextEvent for Cmd-. every ~200ms regardless of
 * any deadline (see qjs_interrupt_handler), so a genuinely runaway script is
 * still abortable by hand. The deadline was a second, blunter copy of a
 * mechanism the user already controls.
 *
 * Set MACSURF_JS_TIMEOUT_MS to a non-zero value to restore a budget. */
/* fixes1022 - QUIESCE SWITCHES. The fixes1011-1019 batch woke behaviours
 * real pages have never seen from this engine: real geometry answers, the
 * window load event, scroll/resize dispatch. The hardware verdict is that
 * on-load measure-then-mutate widgets (dotdotdot truncating every article,
 * slick collapsing the slider) make pages WORSE without the synchronous-
 * layout contract (Phase 3) underneath them -- partial lifecycle support is
 * worse than none, the fixes1010 lesson at page scale. Default 0 restores
 * the fixes1008-era JS-OBSERVABLE surface while keeping every crash guard
 * and correctness fix since. Re-enable ONE AT A TIME, each in the round
 * that ships the engine support it depends on. The harness builds with
 * these ON so the full surface stays tested.
 *
 * fixes1023 - THESE LIVE AT THE TOP OF THE FILE ON PURPOSE. fixes1022 put
 * them beside their first CONSUMER at ~3676, which is 160 lines BELOW two
 * use sites in macsurf_qjs_fire_scroll/_resize; the preprocessor leaves an
 * undefined macro as a bare identifier, so CW8 reported "undefined" at both
 * and the build died. Same class as the TARGET_API_MAC_CARBON prefix bug:
 * a config define is only worth what it is defined BEFORE. */
/* fixes1073 (#265) - GEOMETRY IS ON.
 *
 * It was quiesced by fixes1022 for a good reason: fixes998-1021 turned on four
 * capability classes with no hardware gate between them, and partial geometry
 * made pages WORSE than none -- a widget that measures, gets a fabricated 0 and
 * writes it back as an inline size destroys content that would have rendered
 * fine untouched.
 *
 * What has changed is that the precondition now exists. fixes1073 gives
 * geometry a forced synchronous layout (qjs_geometry_flush ->
 * macos9_reconvert_flush_now), so a measurement taken after a mutation
 * describes the page as it is rather than as it was, and every path that cannot
 * safely reflow still answers `undefined` -- never a fabricated number. That
 * was the standing condition on re-enabling this, and it is met.
 *
 * The switch stays here, and stays a switch.
 *
 * fixes1136 (Option B, js-strategic-audit-2026-08-06): OFF.  Real geometry
 * requires incremental layout -- without it the O(document) sync flush costs
 * ~1.6s per measurement burst, pages with measure-then-mutate widgets (slick,
 * dotdotdot) get 0.4% real answers, and JS consumes 96% of page-load time.
 * Return undefined (the branch-2 / pre-fixes1011 shape) until incremental
 * layout lands (MacSurf 4.0).  The entire sync-flush / settle-once / budget /
 * guard / retry infrastructure stays in the binary and is re-enabled by
 * flipping this one define back to 1. */
#ifndef MACSURF_JS_GEOMETRY
#define MACSURF_JS_GEOMETRY 0
#endif
/* fixes1141 - AUDIT ON for the hardware baseline round. Enables per-script
 * timing, failure reasons, audit budgets, and the LIFE js done ok lines.
 * The counters (g_js_skip_count, g_js_timeout_count) always increment
 * regardless of this switch; this gates only the log emissions. */
#ifndef MACSURF_JS_AUDIT
#define MACSURF_JS_AUDIT 1
#endif
/* fixes1108 (#265) - ON. Only macsurf_qjs_fire_scroll has a live call site
 * (window.c:509-510, the single scroll choke point: arrow keys, scrollbar
 * drag, core set_scroll, End, window.scrollTo all route through it). It
 * already only dispatches on a real position CHANGE (the `moved` guard at
 * window.c:504), carries its own leading+250ms-trailing debounce
 * (g_scroll_last_us/g_scroll_trailing, macsurf_qjs.c:4271-4286), and is
 * listener-gated (macsurf_qjs_event_type_live, fails OPEN when nothing is
 * registered so an empty listener table costs nothing). macsurf_qjs_fire_resize
 * has zero call sites anywhere in the tree, so this define does not newly
 * activate resize dispatch on its own -- it only unblocks the scroll path,
 * which fixes1011's real getBoundingClientRect() made load-bearing (a
 * lazy-load `rect.top < innerHeight` check now answers truly and needs a
 * real scroll event to re-fire). */
#ifndef MACSURF_JS_VIEW_EVENTS
#define MACSURF_JS_VIEW_EVENTS 1
#endif

#ifndef MACSURF_JS_TIMEOUT_MS
/* fixes1136 (Option B): 30s execution deadline.  On a 400 MHz G3, 176 KB
 * framework bundles (XenForo core-compiled.js) need ~19s; 30s gives headroom
 * while still preventing truly runaway scripts.  Set to 0 to disarm. */
#define MACSURF_JS_TIMEOUT_MS 30000
#endif
#define QJS_SCRIPT_TIMEOUT_MS MACSURF_JS_TIMEOUT_MS
/* fixes586 - timer/event callbacks get a shorter budget: a callback that
 * burns 8s of straight CPU is pathological, and the UI is frozen while it
 * runs.  (Top-level scripts keep the 20s budget: big bundles on a G3 are
 * legitimately slow.) */
#define QJS_TIMER_TIMEOUT_MS MACSURF_JS_TIMEOUT_MS
static double g_qjs_script_deadline = 0.0;

/* fixes1037 - timer-callback CPU, separate from top-level script eval. */
long   g_timer_fires = 0;  /* exported for audit */
long   g_timer_us    = 0;  /* exported for audit */
static double g_timer_t0    = 0.0;

/* fixes1236 (#167) - two more per-navigation counters, same lifecycle as
 * g_timer_fires above (read+reset by macsurf_qjs_emit_js_profile).
 *
 *   g_job_pump_cap_hits  how often macsurf_qjs_pump_all's microtask drain
 *                        (fixes868) hits QJS_MAX_JOBS_PER_PUMP and defers the
 *                        rest to the next poll. Was WORK-gated (invisible in
 *                        release) with no counter at all -- a page whose
 *                        Promise chains never advance and one whose queue is
 *                        merely deep looked identical in every prior log.
 *   g_raf_fires          requestAnimationFrame callbacks actually INVOKED
 *                        (counted at fire time, inside the callback -- see
 *                        register_browser_globals), so a healthy timer queue
 *                        that just never gets asked for rAF reads differently
 *                        from rAF itself being dead.
 */
long g_job_pump_cap_hits = 0;  /* exported for audit */
long g_raf_fires         = 0;  /* exported for audit */


/* fixes586 - THE tinkerdifferent hard-freeze.  The deadline was armed ONLY
 * around the top-level JS_Eval in js_exec; setTimeout/setInterval callbacks
 * (macsurf_qjs_run_timers -> JS_Call) and event dispatches (js_fire_event /
 * js_fire_dom_ready -> safe_eval) ran with deadline==0 == UNBOUNDED.  A page
 * timer that enters an infinite loop (XenForo/ThemeHouse retry-poll against
 * our partial DOM) therefore froze the machine forever with no crash: the
 * interrupt handler's WNE swallowed all events (dead UI), the deadline never
 * fired (never armed), and the log's last line was merely whatever the event
 * loop logged before the timer pass ran that tick - which is why the freeze
 * site appeared to wander between builds.  Fix: push a deadline around EVERY
 * JS entry point.  push never EXTENDS an outer deadline (nest-safe); pop
 * restores the caller's value. */
static double qjs_deadline_push(double budget_ms)
{
	double prev = g_qjs_script_deadline;
	double want;
	/* fixes999 - budget 0 means NO deadline: leave whatever is armed alone
	 * (normally nothing) so the script runs to completion. */
	if (budget_ms <= 0.0)
		return prev;
	want = macsurf_qjs_get_now() + budget_ms;
	if (prev == 0.0 || want < prev)
		g_qjs_script_deadline = want;
	return prev;
}
static void qjs_deadline_pop(double prev)
{
	g_qjs_script_deadline = prev;
}

double macsurf_qjs_deadline_push_ms(double budget_ms)
{
	return qjs_deadline_push(budget_ms);
}

void macsurf_qjs_deadline_pop(double prev)
{
	qjs_deadline_pop(prev);
}

double macsurf_qjs_default_timeout_ms(void)
{
	return (double)QJS_SCRIPT_TIMEOUT_MS;
}

/* fixes1071 - WHERE does a slow script actually spend its time?
 *
 * The fixes1070 hardware log found ONE 72KB script (hackaday's navigation.js
 * concat bundle) running for 24.7 SECONDS against 74ms of compile. Run-bound,
 * so bytecode caching cannot help it -- but "run" still spans two completely
 * different failure modes with different fixes:
 *
 *   - the QuickJS INTERPRETER is grinding through a genuinely huge number of
 *     bytecode ops (the script is doing real work, or looping); or
 *   - execution keeps leaving the interpreter to call NATIVE code -- our DOM
 *     bindings -- and the cost is ours, not QuickJS's.
 *
 * Two counters separate them, and both are free:
 *
 *   ops   -- QuickJS calls this interrupt handler every
 *            JS_INTERRUPT_COUNTER_INIT (=10000) bytecode ops, so counting
 *            invocations yields the op count to within 10k. The handler was
 *            already being called and already reads the clock; one increment
 *            adds nothing.
 *   ncalls -- every JS->C call passes through js_call_c_function in quickjs.c
 *            (patched there, one increment).
 *
 * Divide by run_us. A G3 interprets on the order of a million ops/sec; if the
 * measured rate is far below that, the interpreter is NOT where the time went
 * and the native side is the place to look. */
long macsurf_qjs_ncalls = 0;		/* incremented by quickjs.c */
long g_qjs_interrupts = 0;  /* exported for audit */	/* x10000 = bytecode ops    */

static int qjs_interrupt_handler(JSRuntime *rt, void *opaque)
{
	static double hb_last = 0.0;
	static double wne_last = 0.0;   /* fixes690 (#209): WNE poll throttle */
	double now;
	(void)rt; (void)opaque;

	g_qjs_interrupts++;

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
		g_js_timeout_count++;
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
/* Exception logging helper - logs message + stack, one call per site  */
/* ------------------------------------------------------------------ */

/* fixes1125 - every JS entry point that catches an exception should log
 * BOTH the message and the stack, with a LIFE prefix that survives the
 * failures-only release filter.  Before this, five paths logged only the
 * message, one silently dropped the exception, and macsurf_qjs__safe_eval
 * used no LIFE prefix at all.  One call per catch site replaces ~12
 * lines of repeated get-exception / get-message / get-stack / free
 * that had drifted apart over time. */
static void qjs_short_name(const char *name, char *out, int cap);

static void qjs_log_exc(JSContext *ctx, JSValueConst exc,
		const char *what, const char *name)
{
	const char *msg;
	JSValue stk;
	const char *ss;
	char sname[48];

	msg = JS_ToCString(ctx, exc);
	macsurf_debug_log_writef("LIFE qjs %s: %s [%s]",
			what, msg ? msg : "?",
			name ? name : "?");
	if (msg) JS_FreeCString(ctx, msg);

	stk = JS_GetPropertyStr(ctx, exc, "stack");
	if (JS_IsString(stk)) {
		ss = JS_ToCString(ctx, stk);
		if (ss != NULL) {
			qjs_short_name(name, sname, (int)sizeof(sname));
			macsurf_debug_log_writef("LIFE qjs stack: %s [%s]",
					ss, sname);
			JS_FreeCString(ctx, ss);
		}
	}
	JS_FreeValue(ctx, stk);
}

/* ------------------------------------------------------------------ */
/* Safe eval - logs on error, never propagates exception               */
/* ------------------------------------------------------------------ */

/* #265 - settle-once-per-JS-execution geometry. Defined with the
 * geometry census counters further down; forward-declared here because
 * safe_eval and the timer loop (the two earliest execution boundaries)
 * both precede it. See the definition for the full rationale. */
static void qjs_geom_settle_begin(void);

void macsurf_qjs__safe_eval(JSContext *qctx, const char *src)
{
	/* fixes586 - safe_eval runs PAGE event listeners (js_fire_event /
	 * js_fire_dom_ready dispatch jQuery-ready + XF init through here), so
	 * it needs the runaway deadline too.  Internal setup evals are tiny and
	 * never notice it. */
	double prevdl = qjs_deadline_push((double)QJS_SCRIPT_TIMEOUT_MS);
	JSValue val;

	/* #265 - every event dispatch (js_fire_event / js_fire_dom_ready /
	 * js_fire_window_load / script onload) is a fresh JS execution, so the
	 * settle-once geometry flag must not leak into it from the burst that
	 * just yielded. Cleared before ANY JS runs, not just on success: a
	 * failed dispatch still ends the previous execution. (C89: all
	 * declarations above, so this call sits after them but before the
	 * JS_Eval below.) */
	qjs_geom_settle_begin();
	val = JS_Eval(qctx, src, strlen(src),
			"<init>", JS_EVAL_TYPE_GLOBAL);
	qjs_deadline_pop(prevdl);
	if (JS_IsException(val)) {
		JSValue exc = JS_GetException(qctx);
		qjs_log_exc(qctx, exc, "init eval failed", "<init>");
		JS_FreeValue(qctx, exc);
	}
	JS_FreeValue(qctx, val);
}

/* ------------------------------------------------------------------ */
/* console.* native functions                                           */
/* ------------------------------------------------------------------ */

/* fixes1246 (#167) - console.error/warn were "[js:error]"/"[js:warn]"
 * prefixed, NOT "LIFE " -- macsurf_debug_log_write's release-build gate
 * (macsurf_log_is_crash_report) only keeps a line that either starts with
 * '=' or "NAV", or contains an exact ALL-CAPS keyword (FAIL, ERROR,
 * ASSERT, PANIC, UAF, INVALID, ABORT, NOMEM, CORRUPT, TALLOC) or the
 * literal string "LIFE ". Real framework error text ("Warning: ...",
 * "Error: ...", a React hydration-mismatch message, a component stack)
 * essentially never matches any of those case-sensitively, so EVERY
 * console.error/warn call on a shipped build has been going straight to
 * the void -- the exact same invisible-diagnostic trap as fixes1234's
 * winevt fix and fixes1237's window/document dispatchEvent fix, just one
 * level higher: this is the PAGE's OWN primary error-reporting channel,
 * silently dark for the whole history of this engine. React specifically
 * reports recoverable render/hydration errors via console.error rather
 * than an uncaught throw (by design, so one broken component does not
 * take down the whole tree) -- which is exactly the shape that would
 * explain "18 scripts execute with zero exceptions, the one registered
 * listener runs clean, nothing ever throws anywhere we can already see,
 * and yet nothing ever finishes rendering." log/info/debug stay as they
 * were: much higher call volume from ordinary pages, lower diagnostic
 * value per line. */
static void qjs_console_emit(JSContext *ctx, const char *prefix,
		int argc, JSValueConst *argv)
{
	int i;
	char buf[2048];
	size_t pos = 0;
	size_t plen = strlen(prefix);

	/* fixes1246 - budget only the two LIFE-promoted levels (error/warn);
	 * log/info/debug stay WORK-invisible in release exactly as before, so
	 * they cost nothing and need no cap. Silent drop past budget, same
	 * convention as g_mslife_audit/qjs_ms_life -- no "budget exhausted"
	 * marker line. */
	if (strncmp(prefix, "LIFE ", 5) == 0) {
		if (g_console_err_audit <= 0) return;
		g_console_err_audit--;
	}

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
	/* A React component-stack message embeds real newlines; sanitize to
	 * spaces so it can't fragment the log's one-line-per-entry format
	 * (same reason qjs_ms_life does this for __msLife strings). */
	{
		size_t k;
		for (k = 0; k < pos; k++) {
			if (buf[k] == '\r' || buf[k] == '\n') buf[k] = ' ';
		}
	}
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
CONSOLE_FN(warn,  "LIFE console.warn:")
CONSOLE_FN(error, "LIFE console.error:")
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
/* atob / btoa - QuickJS has no built-in, provide in JS               */
/* ------------------------------------------------------------------ */

/* Provided via JS polyfill below - quickjs has atob/btoa as built-ins
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
/* canvas 2D measureText - real font metrics, not a fabricated width   */
/* ------------------------------------------------------------------ */

/* fixes1245 (#167) - canvas getContext('2d') was entirely absent
 * (HTMLCanvasElement is just an empty constructor-name stub, with no
 * getContext method anywhere), so any page code calling it threw
 * "not a function". Confirmed real, non-critical-path usage in Facebook's
 * own bundles: a QR-code renderer, an avatar/photo thumbnail resizer, and
 * a font-metrics cache (CometGHLFontMetricsCache) that measures text width
 * for layout decisions.
 *
 * Real pixel compositing (fillRect/drawImage/putImageData actually
 * painting into a buffer) is a genuinely different, much larger feature --
 * no canvas element has an offscreen GWorld or any compositing pipeline,
 * and building one is layout-engine-scale work, not a JS-binding gap. The
 * JS-side context (register_browser_globals) makes every drawing method a
 * safe, honest NO-OP: a blank canvas accurately represents "nothing was
 * drawn," the same choice this codebase already made for
 * MACSURF_JS_GEOMETRY (undefined over a fabricated number) -- a script
 * that draws a chart onto an invisible canvas is a real, known gap, not a
 * silent wrong answer.
 *
 * measureText is the one piece worth doing for real rather than
 * no-op-ing: a WRONG width is exactly the "confidently wrong answer"
 * class of bug this project has hit before (fixes1031/fixes1015's
 * matchMedia/dataset lessons), and the font-metrics-cache use is
 * specifically a width computation feeding layout logic. This routes
 * through macos9_layout_table->width -- the SAME real, hardware-measured
 * function html_reformat itself uses (macos9_font.c) -- not a
 * character-count heuristic invented here. */
static JSValue qjs_canvas_measure_text_width(JSContext *ctx,
		JSValueConst this_val, int argc, JSValueConst *argv)
{
	const char *text;
	const char *font_css;
	plot_font_style_t fstyle;
	int width = 0;
	int px = 10; /* canvas spec default font is "10px sans-serif" */
	const char *p;

	(void)this_val;
	if (argc < 1) return JS_NewFloat64(ctx, 0.0);
	text = JS_ToCString(ctx, argv[0]);
	if (text == NULL) return JS_NewFloat64(ctx, 0.0);
	font_css = (argc >= 2) ? JS_ToCString(ctx, argv[1]) : NULL;

	memset(&fstyle, 0, sizeof(fstyle));
	fstyle.families = NULL;
	fstyle.family = PLOT_FONT_FAMILY_SANS_SERIF;
	fstyle.weight = 400;
	fstyle.flags = FONTF_NONE;

	if (font_css != NULL) {
		/* Crude but honest: pull the first "<digits>px" run out of the
		 * CSS font shorthand (e.g. "bold 14px Arial, sans-serif" ->
		 * 14). Not a full shorthand grammar parser -- canvas text
		 * measurement tolerates an approximate size far better than a
		 * missing/wrong one; a genuinely malformed font string just
		 * keeps the spec default above. */
		for (p = font_css; *p != '\0'; p++) {
			if (*p >= '0' && *p <= '9') {
				int val = 0;
				const char *q = p;
				while (*q >= '0' && *q <= '9') {
					val = val * 10 + (*q - '0');
					q++;
				}
				if (q[0] == 'p' && q[1] == 'x') {
					px = val;
					break;
				}
				p = q;
				if (*p == '\0') break;
				continue;
			}
		}
		if (strstr(font_css, "bold") != NULL) fstyle.weight = 700;
		if (strstr(font_css, "italic") != NULL) {
			fstyle.flags = (plot_font_flags_t)(fstyle.flags | FONTF_ITALIC);
		} else if (strstr(font_css, "oblique") != NULL) {
			fstyle.flags = (plot_font_flags_t)(fstyle.flags | FONTF_OBLIQUE);
		}
		if (strstr(font_css, "monospace") != NULL) {
			fstyle.family = PLOT_FONT_FAMILY_MONOSPACE;
		} else if (strstr(font_css, "serif") != NULL &&
				strstr(font_css, "sans-serif") == NULL) {
			fstyle.family = PLOT_FONT_FAMILY_SERIF;
		}
		JS_FreeCString(ctx, font_css);
	}
	/* plot_font_style size is in points; canvas .font sizes are px.
	 * 96px/in, 72pt/in. */
	fstyle.size = plot_style_int_to_fixed((px * 3) / 4);

	{
		extern struct gui_layout_table *macos9_layout_table;
		if (macos9_layout_table != NULL &&
				macos9_layout_table->width != NULL) {
			macos9_layout_table->width(&fstyle, text, strlen(text),
					&width);
		}
	}
	JS_FreeCString(ctx, text);
	return JS_NewFloat64(ctx, (double)width);
}

/* ------------------------------------------------------------------ */
/* window.location - backed by macos9 window list                      */
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

/* fixes1011 - defined further down, beside the rest of the Phase 3 layout
 * code; used here by the JS-facing wrappers. */
static int macsurf_qjs_scroll_x(void);
static int macsurf_qjs_scroll_y(void);
static int macsurf_qjs_viewport_w(void);
static int macsurf_qjs_viewport_h(void);

/* fixes1011 - thin JS wrappers over the viewport/scroll accessors. */
static JSValue qjs_js_viewport_w(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv)
{
	(void)this_val; (void)argc; (void)argv;
	return JS_NewInt32(ctx, macsurf_qjs_viewport_w());
}

static JSValue qjs_js_viewport_h(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv)
{
	(void)this_val; (void)argc; (void)argv;
	return JS_NewInt32(ctx, macsurf_qjs_viewport_h());
}

static JSValue qjs_js_scroll_x(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv)
{
	(void)this_val; (void)argc; (void)argv;
	return JS_NewInt32(ctx, macsurf_qjs_scroll_x());
}

static JSValue qjs_js_scroll_y(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv)
{
	(void)this_val; (void)argc; (void)argv;
	return JS_NewInt32(ctx, macsurf_qjs_scroll_y());
}

/* Really scrolls. window.scrollTo/scrollBy were no-ops, so every "back to
 * top" control and every scroll restoration silently did nothing. */
static JSValue qjs_js_scroll_to(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv)
{
	int32_t x = 0, y = 0;
	(void)this_val;
	if (argc >= 1) JS_ToInt32(ctx, &x, argv[0]);
	if (argc >= 2) JS_ToInt32(ctx, &y, argv[1]);
	if (x < 0) x = 0;
	if (y < 0) y = 0;
#ifdef __MACOS9__
	{
		struct gui_window *gw = macos9_window_list_head();
		if (gw != NULL) macos9_window_scroll_to(gw, (int)x, (int)y);
	}
#endif
	return JS_UNDEFINED;
}

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

/* history.pushState/replaceState (Phase A, fixes1198) - update the
 * displayed URL only, with no navigation/fetch. Same shape as
 * qjs_location_set, calling macos9_window_set_url_display instead of
 * macos9_window_navigate. Back/forward integration and popstate are not
 * implemented yet: this does not push a session-history entry, so
 * history.back()/forward() cannot return to a pushState URL. */
static JSValue qjs_history_set_url_display(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv)
{
	(void)this_val;
#ifdef __MACOS9__
	if (argc > 0) {
		const char *url = JS_ToCString(ctx, argv[0]);
		if (url) {
			struct gui_window *win = macos9_window_list_head();
			if (win != NULL) {
				MS_LOG("LIFE fixes1198: pushState/replaceState display-only URL update");
				MS_LOG(url);
				macos9_window_set_url_display(win, url);
			}
			JS_FreeCString(ctx, url);
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

/* fixes868 (#294) - microtask budget per pump pass.  A .then() can enqueue the
 * next job, so draining unbounded lets a promise chain (or a self-scheduling
 * loop) starve the cooperative WaitNextEvent loop and hang the Mac -- the same
 * failure class as the fixes608 timer cycle.  Leftover jobs simply run on the
 * next poll pass; a real browser's task/microtask split behaves the same way.
 * 256 clears any realistic startup chain (hackaday's loader is ~6 deep) in one
 * pass while bounding the worst case. */
#define QJS_MAX_JOBS_PER_PUMP 256

/* fixes877 - was 64, which is low for a page running several libraries at once
 * (jQuery + a carousel + an analytics shim will each hold intervals), and every
 * overflow silently destroys a callback. The arena is a fixed static array, so
 * the cost is purely memory: ~120 B/slot * 256 = ~30 KB against a ~195 MB
 * partition. Cheap insurance against a class of silent, page-visible loss.
 *
 * Also sizes run_timers' due_idx/due_id stack arrays: 2 * 256 * 4 = 2 KB per
 * frame. run_timers is called only from macsurf_qjs_pump_all and is not
 * recursive, so this is a one-off 2 KB, not a per-depth cost. */
#define QJS_MAX_TIMERS 256

/* fixes876 - how many trailing setTimeout(fn, delay, ...) arguments a slot can
 * carry.  4 covers every real use (rAF's timestamp needs 1); anything beyond is
 * logged and dropped rather than silently truncated. */
#define QJS_TIMER_MAX_ARGS 4

/* fixes854 (#283) - `ctx` is the OWNER of this slot's `fn`, captured at
 * setTimeout time, and it is load-bearing, not bookkeeping.  js_newheap()
 * runs per browser_window AND per (i)frame (browser_window.c:3373 "new
 * javascript context for each window/(i)frame"), and every heap gets its
 * OWN JSRuntime (JS_NewRuntime2 in js_newheap) - so a page with an iframe
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
	/* fixes875 (#304) - the owning realm's generation, captured next to `ctx`.
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
	JSContext *ctx;		/* owner of `fn` AND `args` - see the note above */
	JSValue    fn;
	/* fixes876 - setTimeout(fn, delay, a, b): the extra arguments, duped at
	 * registration and replayed at every fire.  These carry EXACTLY the same
	 * cross-runtime lifetime hazard as `fn` above: they are JSValues owned by
	 * `ctx`'s runtime and may only be duped/passed/freed against it.  Every
	 * release path therefore goes through timer_slot_clear(), which handles
	 * `fn` and `args` together -- freeing one and forgetting the other is the
	 * bug this arena has already produced twice (fixes854, fixes875). */
	int        nargs;
	JSValue    args[QJS_TIMER_MAX_ARGS];
	/* fixes888 (#304) - the owning JSRuntime, captured at registration.
	 *
	 * (ctx, gen) is bookkeeping ABOUT the runtime; this is the runtime. The
	 * crash is precisely "an rt_A JSValue freed against rt_B" -- free_object
	 * -> js_free_shape -> js_shape_hash_unlink walks rt_B's bucket chain for a
	 * shape living in rt_A's table, runs off the end, unmapped memory at
	 * js_shape_hash_unlink+0004C. Comparing the runtime directly tests the
	 * exact invariant JS_FreeValue requires, so it holds even when the
	 * generation bookkeeping is wrong -- which the hardware says it is, since
	 * fixes875's gate passed and the free still blew up. */
	JSRuntime *rt;
};

static struct qjs_timer s_timer_arena[QJS_MAX_TIMERS];
static int s_timer_next_id = 1;

/* fixes608 - the timer subsystem is a fixed index-addressed arena with NO
 * intrusive linked list.  The old s_timer_head list could be spliced into a
 * cycle when a timer callback reentrantly called setTimeout (timer_alloc
 * evicting/reusing a slot the run_timers walk still held), and run_timers'
 * `while (t != NULL)` then spun forever - the tinkerdifferent hard-freeze,
 * immune to the fixes586 callback deadline because the spin is in the C loop,
 * not inside JS_Call.  Index-based iteration (0..QJS_MAX_TIMERS-1) makes an
 * infinite loop structurally impossible. */
/* fixes875 (#304) - the generation currently owning `ctx`, or 0 if NO live heap
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

/* fixes888 (#304) - the LIVE runtime owning `ctx`, or NULL if no live heap does.
 *
 * Deliberately resolved through g_heap_list rather than JS_GetRuntime(ctx):
 * JS_GetRuntime dereferences the context, and the whole problem here is that
 * the pointer may be freed. The heap list is the only authority on which
 * contexts still exist, so this asks it instead of trusting the pointer. */
static JSRuntime *qjs_ctx_live_rt(JSContext *ctx)
{
	struct jsheap *h;
	if (ctx == NULL) return NULL;
	for (h = g_heap_list; h != NULL; h = h->next) {
		if (h->ctx == ctx) return h->rt;
	}
	return NULL;
}

/* Does this slot really belong to (ctx, its current generation)? */
static int qjs_timer_owned_by(struct qjs_timer *t, JSContext *ctx)
{
	if (!t->live || t->ctx != ctx) return 0;
	return t->ctx_gen == qjs_ctx_gen(ctx) && t->ctx_gen != 0;
}

/* fixes875 (#304) - never-repeating realm id. Monotonic across the whole
 * process: the ONLY property required is that a value is never reused, which is
 * exactly what a JSContext* address fails to guarantee. */
static unsigned long g_ctx_gen_next = 1;

/* fixes876 - the ONE way a timer slot is released.  Every release path in this
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

	/* fixes888 (#304) - FINAL GATE, and the one that actually matters.
	 *
	 * Whatever the caller decided from (ctx, gen), refuse to free unless the
	 * slot's ctx is STILL LIVE and STILL OWNED BY THE RUNTIME THAT MADE THESE
	 * VALUES. Every caller's own reasoning is bookkeeping that can be wrong;
	 * this is the invariant JS_FreeValue actually requires. If it does not
	 * hold, ABANDON -- leaking into a runtime that is gone (or that never
	 * owned these values) costs nothing real, and freeing is the crash.
	 *
	 * fixes875 tried to close this with (ctx, generation) alone and hardware
	 * still crashed at js_shape_hash_unlink+0004C through
	 * js_newthread -> qjs_flush_timers -> JS_FreeValue, i.e. its gate PASSED
	 * and the free was still cross-runtime. So the gen check stays as a first
	 * filter, but it is no longer what we trust. */
	if (free_vals) {
		JSRuntime *live_rt = qjs_ctx_live_rt(t->ctx);
		if (live_rt == NULL || t->rt == NULL || live_rt != t->rt) {
			macsurf_debug_log_writef(
				"WORK timer: REFUSING cross-runtime free id=%d ctx=%p "
				"slot_rt=%p live_rt=%p -- abandoning instead",
				t->id, (void *) t->ctx, (void *) t->rt,
				(void *) live_rt);
			free_vals = 0;
		}
	}

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
	 * fixes877 - this loop used `<` on expiry_ms, i.e. it picked the MINIMUM
	 * deadline: the soonest-expiring timer, the one closest to firing and so
	 * the one most likely to be needed imminently. A page that briefly
	 * over-filled the arena would silently lose the callback that was about to
	 * run while keeping ones due much later -- a wrong answer, delivered
	 * quietly. (The old variable name `oldest` disguised it: nearest-future is
	 * not least-recently-created.) Evicting the furthest-out gives every
	 * remaining timer the most time to fire before its slot is at risk.
	 *
	 * fixes854 (#283) - free against the slot's OWN ctx, never g_heap->ctx.
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
	/* fixes875 (#304) - free ONLY if the slot's realm is still the live one at
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

	/* fixes876 - capture setTimeout(fn, delay, a, b, ...)'s trailing args.
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
	/* fixes854 (#283) - capture the owning context alongside the dup.  `fn`
	 * belongs to THIS ctx's runtime and may only ever be duped/called/freed
	 * against it. */
	t->ctx = ctx;
	t->ctx_gen = qjs_ctx_gen(ctx);
	/* fixes888 (#304) - capture the owning runtime alongside the dup. Safe to
	 * dereference here: we are executing IN this context, so it is live. */
	t->rt = JS_GetRuntime(ctx);
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
			/* fixes854 (#283) - `t->ctx == ctx` is a correctness gate, not
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

	/* fixes895 (crash-B hunt) - this is the navigation/realm-teardown path
	 * that bombs at js_shape_hash_unlink+0004C when an rt_A JSValue is freed
	 * against rt_B. fixes888's runtime gate in timer_slot_clear should now
	 * refuse that free, but it is HW-unverified. Arm the durable eager flush
	 * for the whole flush so every "WORK timer" breadcrumb (including the
	 * FREE-ALLOWED identity line below and any REFUSING line) is on disk in
	 * order before the potentially-fatal JS_FreeValue -- and drop a durable
	 * position marker so a bomb here is unmistakable in MacSurf ReconvPos.txt.
	 * Disarmed at every return. */
	macsurf_debug_log_reconv_flush(1);
	macsurf_reconv_pos_set("qjs_flush_timers", 0, 0, "");
	macsurf_reconv_pos_flush();
	macsurf_debug_log_writef(
		"WORK timer: flush START old_ctx=%p live_rt=%p live_gen=%ld",
		(void *) old_ctx, (void *) qjs_ctx_live_rt(old_ctx),
		(long) qjs_ctx_gen(old_ctx));

	for (i = 0; i < QJS_MAX_TIMERS; i++) {
		/* fixes854 (#283) - THE hackaday.com crash.  This used to free every
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

		/* fixes875 (#304) - THE CRASH SITE (unmapped memory exception at
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
				"WORK timer: ABANDON stale slot %d gen=%ld live_gen=%ld "
				"ctx=%p (dead realm at a recycled address)",
				i, (long) s_timer_arena[i].ctx_gen,
				(long) qjs_ctx_gen(old_ctx),
				(void *) old_ctx);
			/* free_vals=0: ABANDON - see the note above. */
			timer_slot_clear(&s_timer_arena[i], 0);
			continue;
		}

		/* fixes895 (crash-B hunt) - the (ctx,gen) gate ALLOWED this free.
		 * timer_slot_clear's fixes888 runtime gate gets the final say, but if
		 * IT is wrong this is the last breadcrumb before the fatal JS_FreeValue.
		 * Log the full identity (captured slot_rt vs the live rt, gen vs live
		 * gen) durably, then a position marker, so a bomb pinpoints this slot
		 * and whether gen matched while the runtime differed. */
		macsurf_debug_log_writef(
			"WORK timer: FREE-ALLOWED slot=%d id=%d ctx=%p slot_rt=%p "
			"live_rt=%p gen=%ld live_gen=%ld",
			i, s_timer_arena[i].id, (void *) s_timer_arena[i].ctx,
			(void *) s_timer_arena[i].rt,
			(void *) qjs_ctx_live_rt(s_timer_arena[i].ctx),
			(long) s_timer_arena[i].ctx_gen, (long) qjs_ctx_gen(old_ctx));
		macsurf_reconv_pos_set("timer-free", (long) s_timer_arena[i].id,
				(long) i, "");
		macsurf_reconv_pos_flush();

		timer_slot_clear(&s_timer_arena[i], 1);
	}

	/* fixes895 - flush done without a bomb; disarm the eager flush. */
	macsurf_debug_log_reconv_flush(0);
	macsurf_reconv_pos_set("timer-flush-done", 0, 0, "");
	macsurf_reconv_pos_flush();
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

	/* fixes608 - snapshot the DUE timers by (slot index, id) BEFORE firing
	 * any, then fire from the snapshot.  A callback can reentrantly call
	 * setTimeout (which may evict+reuse an arena slot) or clearTimeout
	 * (which frees a slot); the index+id snapshot makes that reentrancy
	 * safe, and the bounded 0..QJS_MAX_TIMERS-1 walk can never loop forever
	 * (the old intrusive-list walk could be spliced into a cycle mid-callback
	 * -> the tinkerdifferent hard-freeze). */
	ndue = 0;
	for (i = 0; i < QJS_MAX_TIMERS; i++) {
		/* fixes854 (#283) - only fire timers belonging to THIS context.  The
		 * arena is shared by every heap (one per window/iframe, each with its
		 * own JSRuntime), so without this gate the main page's poll would
		 * JS_DupValue/JS_Call an IFRAME's callback against the main runtime -
		 * a cross-runtime call on a JSValue rt_main never allocated.  Each
		 * heap's own run_timers pass fires its own slots. */
		/* fixes875 (#304) - generation too: a stale slot at a recycled ctx
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
		/* fixes854 (#283) - re-check the owner too: a callback can run
		 * arbitrary JS, and an eviction may have handed this slot to a
		 * DIFFERENT heap's setTimeout since we snapshotted, which would make
		 * the JS_DupValue below cross-runtime. */
		if (!qjs_timer_owned_by(t, qctx) || t->id != due_id[k]) continue;

		/* #265 - a timer callback is its own JS execution burst: clear the
		 * settle-once geometry flag so its first read settles fresh. Two
		 * callbacks in one pump are two executions and may legitimately
		 * need two flushes (the DOM can change between them). */
		qjs_geom_settle_begin();

		/* fixes876 - snapshot fn AND the extra args BEFORE the slot can be
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

		/* fixes586 - bound the callback so a runaway script can't hang
		 * (the interrupt handler checks g_qjs_script_deadline). */
		/* fixes1001 - this armed the deadline DIRECTLY instead of going
		 * through qjs_deadline_push, so fixes999's "0 == no deadline"
		 * never reached it. With QJS_TIMER_TIMEOUT_MS == 0 it computed
		 * `now + 0`, i.e. a deadline ALREADY EXPIRED, and the interrupt
		 * handler aborted every timer callback on its first check --
		 * "InternalError: interrupted" twice inside 67ms on hardware,
		 * far too fast to be the Cmd-. it looked like. Every setTimeout
		 * on the page was being killed the instant it ran.
		 *
		 * Use the shared push, which is the whole point of having one. */
		/* fixes1037 - TIMER time, counted separately from script time.
		 * PERFACC says JS is 96% of a hackaday load (34.4s of 35.9s;
		 * 57.7s on a slower run) while layout+cascade+paint together
		 * are 1.4s. That is far more than executing ~250KB of bundles
		 * once, so the question is whether it is the bundles at all or
		 * timers spinning: dotdotdot installs a 500ms watch interval
		 * and slick has autoplay, and a re-firing timer is a RENDERING
		 * problem too because each pass churns the DOM. One
		 * accumulator and one counter settle it. */
		{
			extern double macos9_micros(void);
			g_timer_fires++;
			g_timer_t0 = macos9_micros();
		}
		prevdl = qjs_deadline_push((double)QJS_TIMER_TIMEOUT_MS);
		mydl = g_qjs_script_deadline;
		/* fixes876 - HTML spec calls timer callbacks with `this` = the window.
		 * JS_UNDEFINED left strict-mode callbacks with `this === undefined`. */
		this_obj = JS_GetGlobalObject(qctx);
		ret = JS_Call(qctx, fn, this_obj, call_nargs, call_args);
		{	/* fixes1037 */
			extern double macos9_micros(void);
			double dt = macos9_micros() - g_timer_t0;
			if (dt > 0.0) g_timer_us += (long)dt;
		}
		JS_FreeValue(qctx, this_obj);
		if (JS_IsException(ret)) {
			JSValue exc = JS_GetException(qctx);
			qjs_log_exc(qctx, exc, "timer exc", "setTimeout");
			JS_FreeValue(qctx, exc);
			/* Deadline-abort of a still-live (repeating) timer: kill
			 * it so the rogue interval can never re-freeze the UI. */
			/* fixes1001 - mydl == 0 means NO deadline is armed, and
			 * `now >= 0` is always true: without this guard every
			 * repeating timer that merely THREW would be killed as
			 * if it had timed out. Only a real deadline can retire
			 * a timer. */
			if (t->live && t->id == due_id[k] && t->ctx == qctx &&
			    mydl != 0.0 && macsurf_qjs_get_now() >= mydl) {
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

/* fixes1114 (#265) - document.title getter was hardcoded to return "".
 * The setter wrote the real title straight to the Mac window title bar
 * (via macos9_gw_set_title) but never cached it, so the getter had nothing
 * to return even though the page had already set it.
 *
 * fixes1114b: matchMedia is now a real evaluator (replaces hardcoded false,
 * see the JS block below). Both fixes are in this single file, no prefix
 * touch - normal incremental rebuild. */
static char g_last_title[512] = "";

static JSValue qjs_document_title_get(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv)
{
	(void)this_val; (void)argc; (void)argv;
	return JS_NewString(ctx, g_last_title);
}

static JSValue qjs_document_title_set(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv)
{
	(void)this_val;
#ifdef __MACOS9__
	if (argc > 0) {
		const char *title = JS_ToCString(ctx, argv[0]);
		if (title != NULL) {
			size_t n = strlen(title);
			if (n >= sizeof(g_last_title)) n = sizeof(g_last_title) - 1;
			memcpy(g_last_title, title, n);
			g_last_title[n] = '\0';
			if (initial_win != NULL) {
				macos9_gw_set_title(initial_win, title);
			}
			JS_FreeCString(ctx, title);
		}
	}
#else
	(void)argc; (void)argv;
#endif
	return JS_UNDEFINED;
}

/* ================================================================== */
/* DOM bridge - getElementById, querySelectorAll, getAttribute,        */
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
/* fixes878 - real cloneNode/contains (macsurf_dom_dispatch.c). `deep` and
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
/* fixes873 - shorten a script URL to something a 255-byte log line can afford.
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

/* ------------------------------------------------------------------
 * fixes1070 - JS PHASE + PER-SCRIPT PROFILE
 *
 * Measured 2026-07-25, hackaday front page: js = 25.4s of a 31s load, while
 * cascade+layout+paint together are ~1.2s. fixes1037 already split timer CPU
 * out (0.6s), which proved it is TOP-LEVEL BUNDLE EXECUTION rather than
 * intervals spinning. That is as far as one lump accumulator can take us, and
 * it is not far enough to choose a fix:
 *
 *   - if COMPILE dominates, the answer is bytecode caching -- JS_WriteObject
 *     the compiled function into the existing disk cache (macos9_disk_cache.c,
 *     which already streams and already survives relaunch) and JS_ReadObject
 *     it back. Big win, self-contained, no JS semantics touched.
 *   - if RUN dominates, caching buys nothing at all and the answer is a
 *     per-script budget, or making our own DOM bindings cheaper.
 *
 * Those are opposite pieces of work, and 25 seconds is too much to spend
 * guessing which. So: bracket compile and run separately, and keep a small
 * top-N table naming the scripts that actually cost the time. Both are
 * accumulate-into-statics and emit-once-per-navigation -- the shape CLAUDE.md
 * settled on after per-event logging repeatedly polluted the very measurement
 * it was taken for (the fixes347d var() trace was 95.5% of one load's log).
 * ------------------------------------------------------------------ */

struct qjs_perf_slot g_perf_slot[QJS_PERF_SLOTS];  /* exported for audit */
long g_perf_evals      = 0;  /* exported for audit */	/* top-level evals this navigation  */
long g_perf_bytes      = 0;  /* exported for audit */	/* source bytes compiled            */
long g_perf_compile_us = 0;  /* exported for audit */	/* parse + codegen, summed          */
long g_perf_run_us     = 0;  /* exported for audit */	/* bytecode execution, summed       */
long g_perf_gc_us      = 0;  /* exported for audit */	/* JS_RunGC, summed (see note below)*/
long g_perf_gc_runs    = 0;  /* exported for audit */

/* fixes1070 - is automatic cycle-GC ARMED? Set by js_newheap alongside the
 * JS_SetGCThreshold call, so the log can never present gc=0 as "collection is
 * free" when the truth is "collection never runs".
 *
 * It does not run: fixes593 pushed the threshold to 1GB, past the 128MB memory
 * cap, to work around a heap-corruption freeze on tinkerdifferent that was
 * suspected to be the cycle collector double-freeing an over-released ref.
 * That was a diagnostic that shipped, and the underlying refcount bug was never
 * found, so GC has been off ever since.
 *
 * That matters HERE because it is a live perf hypothesis, not just history:
 * with the collector off, every cyclic object a page creates survives the whole
 * navigation and the heap only grows toward the 128MB cap. On a bundle-heavy
 * page that is steadily worse allocator behaviour and locality across the same
 * 25s of script execution this round is trying to explain. gcarmed=0 in the
 * JSPHASE line is the flag that says so out loud. */
int g_perf_gc_armed = 0;  /* exported for audit */

/* fixes1013 - JS execution census, referenced by js_exec/js_exec_module.
 * Defined here (above their first use) and exported for the audit TU. */
long g_js_exec_count = 0;  /* exported for audit */
long g_js_exec_bytes = 0;  /* exported for audit */
long g_js_exec_fail  = 0;  /* exported for audit */
long g_js_skip_count = 0;  /* fixes1141 - scripts skipped (size cap) */
long g_js_timeout_count = 0;  /* fixes1141 - scripts aborted (deadline) */

/* R1.3 - per-script census backing the `LIFE SCRIPT CENSUS` lines.  Written
 * by qjs_census_note() (below), emitted and cleared by the page summary in
 * macsurf_qjs_audit.c. */
struct script_census_entry g_script_census[SCRIPT_CENSUS_MAX];
long g_script_census_count = 0;  /* exported for audit */
long g_script_census_full  = 0;  /* exported for audit */

/* fixes1071 - wrapper-helper compile census. Declared HERE, above the perf
 * emitters that read them, rather than beside qjs_helper_fn where they are
 * written: C89 needs the declaration before every use, and the JSWHERE emit
 * sits earlier in the file than the wrapper code. See qjs_helper_fn for what
 * these mean and why they exist. */
long g_wrap_installs   = 0;  /* exported for audit */
long g_helper_compiles = 0;  /* exported for audit */
long g_helper_bytes    = 0;  /* exported for audit */
/* fixes1078 - what the per-element wrapper install COSTS to execute.
 *
 * fixes1071 cached the helper COMPILE, but every wrapper still runs all four
 * helper functions, and they do dozens of Object.defineProperty calls with
 * closures captured over `el`. That is per-element work a real DOM does once
 * on a shared prototype. hackaday builds 566 wrappers; if this number is
 * seconds, migrating the helpers to qjs_el_install_proto is the single
 * biggest JS win available and it costs no capability at all. */
long g_wrap_us         = 0;  /* exported for audit */
/* fixes1078 - time spent INSIDE native bindings, sampled 1-in-64 by
 * js_call_c_function and scaled. ncalls x an assumed per-call cost is how the
 * 25us figure was inferred; this measures it instead. Sampled because timing
 * every call would add two Microseconds() traps to the hottest path in the
 * engine and change the thing being measured. */
long macsurf_qjs_native_us    = 0;
long macsurf_qjs_native_samp  = 0;

/* fixes1077 - geometry read census, so the cost of answering is measurable
 * rather than inferred. reads = every geometry entry; us = what they cost. */
long g_geom_reads = 0;  /* exported for audit */
long g_geom_us    = 0;  /* exported for audit */
/* fixes1087 - WHERE in the load a measurement was answered, and how often we
 * still refuse. `ready` is the whole point: before this it was structurally
 * zero, because the gate demanded DONE. If it stays zero the gate did not
 * actually open. */
long g_geom_at_ready = 0;  /* exported for audit */
long g_geom_at_done  = 0;  /* exported for audit */
long g_geom_unstable = 0;  /* exported for audit */
/* fixes1087 - WHAT the page got back. A refusal and a confidently wrong
 * number fail differently and want different fixes, so count them apart:
 *   undef  refused (unsettled, or a mutation pending with no box)
 *   zero   answered 0 -- "not rendered". True for a hidden element, and a
 *          LIE for one whose box simply has not been built yet. This is the
 *          number that collapses a carousel, so it is the one to watch.
 *   real   answered from a real box. */
long g_geom_undef = 0;  /* exported for audit */
long g_geom_zero  = 0;  /* exported for audit */
long g_geom_real  = 0;  /* exported for audit */

/* #265 (hackaday slider) - SETTLE-ONCE-PER-JS-EXECUTION geometry flag.
 *
 * Every geometry read used to call macos9_reconvert_flush_now(), a FULL
 * synchronous html_reconvert() (~1.2 s) whenever the DOM carried any
 * pending dirty marks. slick's setDimensions interleaves reads and DOM
 * writes, so of its 1280 reads, 1255 found marks: 25 flushes consumed
 * the whole 30 s sync budget and every later read was DECLINED and
 * answered undefined/0, baking width:0px/height:0px into every slide.
 * Hardware: `LIFE JSSYNC flush=25 declined=1255 us=30680059`.
 *
 * The box tree cannot change while JS is not running (cooperative
 * model), so one flush per JS execution answers every read of that
 * burst: the reads between mutations answer from the settled tree.
 * Once a flush has run (or nothing was pending), skip the flush and
 * answer from the current box tree.
 *
 * Deliberate deviation from the "clear on every DOM mutation" draft:
 * clearing here on macos9_js_mark_dom_dirty_node would re-arm the flag
 * on the FIRST write after a settle, so every subsequent read would
 * attempt a flush again -- declined=1255 proves reads and mutations
 * interleave 1:1, so that design keeps ~1280 flush ATTEMPTS and the
 * budget still breaks. Clearing only at JS-execution boundaries
 * (qjs_geom_settle_begin: top of macsurf_qjs_pump_all, top of js_exec,
 * per timer callback, per safe_eval) is the only thing that cuts the
 * flush count below the budget.
 *
 * Content-keyed (iframes have their own content): a settle in one
 * runtime must not silence flushes for another. A DECLINED flush leaves
 * the flag 0 so the next read retries -- today's retry semantics are
 * preserved. qjs_geometry_settled() still independently gates every
 * read on tree stability/liveness. */
static int g_geom_settled = 0;
static void *g_geom_settled_c = NULL;

static void qjs_geom_settle_begin(void)
{
	g_geom_settled = 0;
	g_geom_settled_c = NULL;
}


static void qjs_perf_note_script(const char *name, long bytes,
		long compile_us, long run_us)
{
	char sn[QJS_PERF_NAME];
	int i;
	int victim;
	long victim_cost;
	long cost;

	g_perf_evals++;
	if (bytes > 0)      g_perf_bytes      += bytes;
	if (compile_us > 0) g_perf_compile_us += compile_us;
	if (run_us > 0)     g_perf_run_us     += run_us;

	qjs_short_name(name, sn, (int)sizeof(sn));

	/* Same script evaluated twice (or an inline <script> sharing the
	 * document's name) merges into one row rather than consuming a second
	 * slot -- otherwise a page with many small inline scripts evicts the
	 * one big bundle we are trying to find. */
	for (i = 0; i < QJS_PERF_SLOTS; i++) {
		if (g_perf_slot[i].name[0] != '\0' &&
		    strcmp(g_perf_slot[i].name, sn) == 0) {
			g_perf_slot[i].bytes      += bytes;
			g_perf_slot[i].compile_us += compile_us;
			g_perf_slot[i].run_us     += run_us;
			g_perf_slot[i].evals++;
			return;
		}
	}
	for (i = 0; i < QJS_PERF_SLOTS; i++) {
		if (g_perf_slot[i].name[0] == '\0') {
			strcpy(g_perf_slot[i].name, sn);
			g_perf_slot[i].bytes      = bytes;
			g_perf_slot[i].compile_us = compile_us;
			g_perf_slot[i].run_us     = run_us;
			g_perf_slot[i].evals      = 1;
			return;
		}
	}
	/* Table full: evict the cheapest row, but only if this script beats
	 * it. Keeps the N most expensive scripts, which is the whole point. */
	victim = 0;
	victim_cost = g_perf_slot[0].compile_us + g_perf_slot[0].run_us;
	for (i = 1; i < QJS_PERF_SLOTS; i++) {
		long c = g_perf_slot[i].compile_us + g_perf_slot[i].run_us;
		if (c < victim_cost) { victim_cost = c; victim = i; }
	}
	cost = compile_us + run_us;
	if (cost > victim_cost) {
		strcpy(g_perf_slot[victim].name, sn);
		g_perf_slot[victim].bytes      = bytes;
		g_perf_slot[victim].compile_us = compile_us;
		g_perf_slot[victim].run_us     = run_us;
		g_perf_slot[victim].evals      = 1;
	}
}

/* R1.3 - record one script execution for the page census.
 *
 * Called from js_exec and js_exec_module once per execution, at the point
 * the outcome is known.  defer/async is NOT reachable at these call sites
 * (the core's html_script type never crosses the js_exec boundary), so it
 * stays 0 ("-") until a round threads it through.  `compiled`/`completed`
 * are 0/1 flags: a compile failure is compiled=0 completed=0 (nothing
 * ran); a run failure is compiled=1 completed=0; a clean run is 1/1.
 *
 * The array is cleared by the page summary's emit, not here - see
 * macsurf_qjs_audit.h for why (per-(i)frame audit_reset). */
static void qjs_census_note(const char *name, long bytes,
		unsigned char type, unsigned char compiled,
		unsigned char completed, long compile_us, long run_us)
{
	char sn[SCRIPT_CENSUS_NAME];
	struct script_census_entry *e;

	if (g_script_census_count >= SCRIPT_CENSUS_MAX) {
		g_script_census_full++;
		return;
	}
	e = &g_script_census[g_script_census_count++];
	qjs_short_name(name, sn, (int)sizeof(sn));
	strcpy(e->name, sn);
	e->size       = bytes;
	e->compile_us = compile_us;
	e->run_us     = run_us;
	e->type       = type;
	e->defer_async = 0;   /* not available at this call site yet */
	e->compiled  = compiled;
	e->completed = completed;
}

/* fixes1070 - read-back accessors, so the harness can assert on what this
 * instrument MEASURED rather than merely that it emitted something.
 *
 * The standing rule here is "assert counts, never booleans", and it exists
 * because a boolean check cannot see a double-fire (the libdom double-dispatch
 * hid for ~15 rounds behind "did it fire?"). The same trap applies to a
 * profiler with even less excuse: compile and run brackets that were
 * accidentally swapped would still produce two plausible non-zero numbers, and
 * every conclusion drawn from them would be backwards. Exposing the slots lets
 * harness Test 49 run one compile-heavy and one run-heavy script and check
 * that the split lands on the right side of the seam.
 *
 * Returns 1 if slot i is in use, 0 otherwise. Any out pointer may be NULL. */
int macsurf_qjs_perf_slot(int i, char *name, int cap, long *bytes,
		long *compile_us, long *run_us, long *evals);
int macsurf_qjs_perf_slot(int i, char *name, int cap, long *bytes,
		long *compile_us, long *run_us, long *evals)
{
	if (i < 0 || i >= QJS_PERF_SLOTS) return 0;
	if (g_perf_slot[i].name[0] == '\0') return 0;
	if (name != NULL && cap > 0) {
		int j = 0;
		while (j < cap - 1 && g_perf_slot[i].name[j] != '\0') {
			name[j] = g_perf_slot[i].name[j]; j++;
		}
		name[j] = '\0';
	}
	if (bytes != NULL)      *bytes      = g_perf_slot[i].bytes;
	if (compile_us != NULL) *compile_us = g_perf_slot[i].compile_us;
	if (run_us != NULL)     *run_us     = g_perf_slot[i].run_us;
	if (evals != NULL)      *evals      = g_perf_slot[i].evals;
	return 1;
}

/* fixes1070 - run a collection explicitly, and therefore measurably.
 *
 * Automatic GC is disarmed (see g_perf_gc_armed), so JS_RunGC currently has no
 * caller at all in a normal load and the timing hook in quickjs.c would be
 * dead code that nothing could verify. This gives it one, and it is the entry
 * point a future round would use if collection is ever re-armed at a safe
 * quiescent point rather than mid-allocation -- which is the shape that would
 * dodge the fixes593 freeze while getting the memory back.
 *
 * Nothing on the load path calls this today; it changes no behaviour. */
void macsurf_qjs_run_gc(struct jsheap *heap);
void macsurf_qjs_run_gc(struct jsheap *heap)
{
	if (heap == NULL || heap->rt == NULL) return;
	JS_RunGC(heap->rt);
}

/* fixes1071 - wrapper/helper-compile census, for harness Test 50. */



/* fixes870 (#297) - createElementNS, Preact's only element factory. */
extern dom_exception macsurf_dom_document_create_element_ns_s(dom_document *doc,
		const char *ns, const char *qname, dom_element **element);
/* fixes872 - declare the fixes867 owner-document accessor properly. It was being
 * called with NO prototype in scope, so C89 implicitly declared it int-returning.
 * Benign by luck here (dom_exception is an enum, i.e. int, and comes back in r3
 * either way) -- but only by luck, and the same omission on a double- or
 * pointer-returning function is a real miscompile. */
extern dom_exception macsurf_dom_node_get_owner_document(dom_node *node,
		dom_document **result);
/* fixes846 (#167 S3) - real createTextNode/createDocumentFragment/text-data. */
extern dom_exception macsurf_dom_document_create_text_node_s(dom_document *doc,
		const char *data, dom_text **text);
extern dom_exception macsurf_dom_document_create_document_fragment(
		dom_document *doc, dom_document_fragment **fragment);
extern dom_exception macsurf_dom_characterdata_get_data(dom_node *node,
		dom_string **data);
extern dom_exception macsurf_dom_characterdata_set_data_s(dom_node *node,
		const char *data);
/* fixes1168 (#262) - attribute enumeration for the innerHTML serializer
 * (macsurf_dom_dispatch.c wrappers around the static-inline libdom
 * vtable dispatchers). */
extern dom_exception macsurf_dom_node_get_attributes(dom_node *node,
		dom_namednodemap **result);
extern dom_exception macsurf_dom_attr_get_name(dom_node *attr,
		dom_string **name);
extern dom_exception macsurf_dom_attr_get_value(dom_node *attr,
		dom_string **value);

/* ---- Global document/content pointers (set in js_newthread) ---- */
static dom_document  *g_qjs_document = NULL;
static struct content *g_qjs_content = NULL;

void qjs_set_document(dom_document *doc)  { g_qjs_document = doc; }
void qjs_set_content(struct content *c)   { g_qjs_content  = c; }

/* fixes846 (#167 S3) - macos9_js_fetch.c's only need for g_qjs_content:
 * read the page URL as a fetch_start() referer at send()-time. See this
 * pointer's staleness rules two comments below; the caller must snapshot
 * whatever it needs synchronously, not hold this across an async gap. */
struct content *qjs_get_content(void) { return g_qjs_content; }

JSContext *macsurf_qjs_current_ctx(void)
{
	return (g_heap != NULL) ? g_heap->ctx : NULL;
}

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
 * keepalive are ONE mechanism - they cannot be split.  Why:
 *   - The map gives a node AT MOST ONE wrapper (lookup-then-create), so
 *     el.parentNode === el.parentNode and removeChild/contains identity hold.
 *   - "At most one wrapper" is also what guarantees the finalizer runs EXACTLY
 *     ONCE per node, which is the precondition that makes the keepalive's
 *     balanced ref/unref sound.
 *   - Single-owner / single-release: each wrapper owns exactly ONE node ref
 *     and ONE owner-document keepalive ref (g_qjs_document captured at wrap
 *     time).  Both are released SOLELY by the finalizer or the realm drain -
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
	JSValue    val;        /* map-owned wrapper root; released on drain */
	JSRuntime *rt;         /* fixes900 - the runtime that created this
	                        * wrapper; the drain is PER-RUNTIME so an
	                        * iframe heap-destroy cannot free the parent
	                        * runtime's wrappers (crash B). */
	struct qjs_wrap_entry *next;
};

/* This map is file-static and shared by EVERY runtime -- js_newheap() runs per
 * window AND per (i)frame, so on a page with an iframe there are multiple live
 * runtimes with entries here at once. It is drained on realm reset and heap
 * destroy, but ONLY for the runtime being torn down (fixes900): draining every
 * bucket unconditionally used to unref the PARENT document's node + keepalive
 * refs when an iframe closed, freeing the parent's document out from under its
 * still-live runtime -> the js_shape_hash_unlink / null-fn-ptr crash. */
static struct qjs_wrap_entry *s_wrap_buckets[QJS_WRAP_BUCKETS];

/* install is defined far below; wrap (just under here) folds it in on miss. */

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
static int qjs_wrap_insert(dom_node *node, dom_node *owner_doc, JSValue val,
		JSRuntime *rt)
{
	unsigned int h = qjs_wrap_hash(node);
	struct qjs_wrap_entry *e =
		(struct qjs_wrap_entry *)malloc(sizeof(struct qjs_wrap_entry));
	if (e == NULL) return 0;
	e->node = node;
	e->owner_doc = owner_doc;
	e->val = JS_DupValueRT(rt, val);
	e->rt = rt;             /* fixes900 - owning runtime for the per-rt drain */
	e->next = s_wrap_buckets[h];
	s_wrap_buckets[h] = e;
	return 1;
}

/* Unlink the entry for node (does NOT drop refs - the caller does). */
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

/* Realm-reset / heap-destroy drain - does BOTH halves then clears.  The
 * finalizers fired by JS_FreeContext normally empty the map first (each removes
 * its entry and drops node+owner_doc); this is the GUARANTEED, pure-C release
 * for any entry whose finalizer did not run (e.g. wrapper objects still in
 * obj->method reference cycles that JS_FreeContext leaves for JS_FreeRuntime):
 * release the wrapper root, drop the node ref AND the owner-document keepalive
 * ref, THEN clear the entry. */
/* fixes1008 - defined further down (next to qjs_dom_register_listener, which
 * is what feeds them), used here at realm teardown. */
static void qjs_reg_clear(void);
static void qjs_evgate_reset(void);

static void qjs_wrap_drain(JSRuntime *rt)
{
	unsigned int i;
	int cleaned = 0;
	int kept = 0;
	for (i = 0; i < QJS_WRAP_BUCKETS; i++) {
		struct qjs_wrap_entry *e = s_wrap_buckets[i];
		struct qjs_wrap_entry *prev = NULL;
		while (e != NULL) {
			struct qjs_wrap_entry *next = e->next;
			/* fixes900 - PER-RUNTIME drain. Only release wrappers created by
			 * the runtime being torn down. An entry belonging to a DIFFERENT,
			 * still-live runtime (the classic case: the parent document's
			 * wrappers while an IFRAME heap is being destroyed) is LEFT LINKED
			 * -- unref'ing its node + document-keepalive here would free the
			 * parent's DOM out from under its live runtime, which is crash B
			 * (fixes867 merely COUNTED these 'foreign' entries and freed them
			 * anyway). rt==NULL means "drain everything" (final process
			 * teardown), preserving the old behaviour for that one case. */
			if (rt != NULL && e->rt != rt) {
				prev = e;
				kept++;
				e = next;
				continue;
			}
			if (prev == NULL) s_wrap_buckets[i] = next;
			else prev->next = next;
			JS_SetOpaque(e->val, NULL);
			JS_FreeValueRT(e->rt, e->val);
			if (e->node)      macsurf_dom_node_unref(e->node);
			if (e->owner_doc) macsurf_dom_node_unref(e->owner_doc);
			free(e);
			cleaned++;
			e = next;
		}
	}
	/* fixes900 - `kept` (wrappers left linked because they belong to another
	 * live runtime) replaces fixes867's `foreign` counter: a non-zero `kept`
	 * on an iframe teardown is exactly the parent's wrappers we now correctly
	 * DECLINE to free. The old code counted them and freed them anyway (crash
	 * B). rt keys the ownership; g_qjs_document is no longer consulted here (it
	 * is a single stale-prone global - the reason the old owner_doc match was
	 * unreliable). */
	/* fixes1008 - the registration set and the event-type gate are both keyed
	 * to this realm's nodes, so they die with it. Leaving the set behind
	 * would let a RECYCLED node address look already-registered, and its
	 * listeners would silently never reach libdom. */
	qjs_reg_clear();
	qjs_evgate_reset();
	macsurf_debug_log_writef(
		"WORK wrapmap drain freed=%d kept-foreign=%d heap=%p",
		cleaned, kept, (void *)g_heap);
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

/* Look up a DOM constructor stub's .prototype by name and return it as an
 * owned JSValue (caller frees); JS_NULL when the constructor does not exist
 * (the caller then keeps the wrapper class proto, which still answers
 * instanceof HTMLElement/Element/Node through the class proto chain, see
 * qjs_el_install_proto). */
static JSValue qjs_ctor_proto_by_name(JSContext *ctx, const char *ctor_name)
{
	JSValue g, ctor, proto;

	g = JS_GetGlobalObject(ctx);
	if (JS_IsException(g)) return JS_NULL;
	ctor = JS_GetPropertyStr(ctx, g, ctor_name);
	JS_FreeValue(ctx, g);
	if (JS_IsException(ctor) || !JS_IsFunction(ctx, ctor)) {
		JS_FreeValue(ctx, ctor);
		return JS_NULL;
	}
	proto = JS_GetPropertyStr(ctx, ctor, "prototype");
	JS_FreeValue(ctx, ctor);
	if (JS_IsException(proto) || !JS_IsObject(proto)) {
		JS_FreeValue(ctx, proto);
		return JS_NULL;
	}
	return proto;
}

/* fixes1127 -- the per-tag DOM constructor prototype for a freshly-wrapped
 * element, so `el instanceof HTMLDivElement` / `HTMLElement` / `Element` /
 * `Node` answer truthfully.  The DOM constructors are JS stubs; each per-tag
 * stub's .prototype has its __proto__ re-pointed at the wrapper class proto
 * (qjs_el_install_proto), so setting the wrapper object's OWN proto to the
 * tag's constructor prototype puts the whole family in the wrapper's chain
 * while keeping the on* accessors (which live on the class proto) reachable.
 *
 * Live driver: XenForo core-compiled.js measureScrollBar calls
 * XF.createElement("div", {className:"scrollMeasure"}, m.body), which gates
 * its append behind `b instanceof HTMLElement`; with the stubs disconnected
 * every element answered false, the probe div was never appended, and
 * `b.parentNode.removeChild(b)` threw "cannot read property 'removeChild' of
 * null" -- blocking XF.Element registration and the editor.
 *
 * Returns an owned JSValue (caller frees) or JS_NULL when the tag has no
 * constructor (wrapper keeps the class proto; per-tag instanceof stays false
 * exactly as before, family instanceof still true). */
static JSValue qjs_dom_ctor_proto(JSContext *ctx, const char *tag_lc)
{
	char name[48];
	int i, n;

	if (tag_lc == NULL || tag_lc[0] == '\0') return JS_NULL;

	/* CamelCase exceptions the naive "HTML"+tag+"Element" build gets wrong. */
	if (strcmp(tag_lc, "svg") == 0) {
		strcpy(name, "SVGSVGElement");
	} else if (strcmp(tag_lc, "textarea") == 0) {
		strcpy(name, "HTMLTextAreaElement");
	} else if (strcmp(tag_lc, "ul") == 0) {
		strcpy(name, "HTMLUListElement");
	} else if (strcmp(tag_lc, "ol") == 0) {
		strcpy(name, "HTMLOListElement");
	} else if (strcmp(tag_lc, "dl") == 0) {
		strcpy(name, "HTMLDListElement");
	} else if (strcmp(tag_lc, "p") == 0) {
		strcpy(name, "HTMLParagraphElement");
	} else if (strcmp(tag_lc, "a") == 0) {
		strcpy(name, "HTMLAnchorElement");
	} else if (strcmp(tag_lc, "tr") == 0) {
		strcpy(name, "HTMLTableRowElement");
	} else if (strcmp(tag_lc, "td") == 0 || strcmp(tag_lc, "th") == 0) {
		strcpy(name, "HTMLTableCellElement");
	} else if (strcmp(tag_lc, "caption") == 0) {
		strcpy(name, "HTMLTableCaptionElement");
	} else if (strcmp(tag_lc, "col") == 0) {
		strcpy(name, "HTMLTableColElement");
	} else {
		n = 4;	/* "HTML" */
		strcpy(name, "HTML");
		name[n] = (tag_lc[0] >= 'a' && tag_lc[0] <= 'z')
			? (char)(tag_lc[0] - 'a' + 'A') : tag_lc[0];
		n++;
		for (i = 1; tag_lc[i] != '\0' && n < (int)sizeof(name) - 8; i++)
			name[n++] = tag_lc[i];
		strcpy(name + n, "Element");
	}
	return qjs_ctor_proto_by_name(ctx, name);
}

/* Point a freshly-wrapped object's prototype at a constructor's prototype
 * when the constructor exists; leave the wrapper class proto otherwise.
 * Node-shape-accurate for elements (per-tag), text/comment (Text /
 * CharacterData / Comment) and fragments (DocumentFragment) -- a text node
 * must NOT answer instanceof HTMLElement, which is what the shared class
 * proto alone would do after fixes1127's p.__proto__ link. */
static void qjs_wrap_set_family_proto(JSContext *ctx, JSValue obj,
		const char *ctor_name)
{
	JSValue tp = qjs_ctor_proto_by_name(ctx, ctor_name);
	if (JS_IsObject(tp)) {
		JS_SetPrototype(ctx, obj, tp);
	}
	JS_FreeValue(ctx, tp);
}

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
 * qjs_el_finalizer / qjs_wrap_drain - nothing else unrefs that node. */
static JSValue qjs_wrap_element(JSContext *ctx, dom_element *el)
{
	dom_node *node = (dom_node *)el;
	struct qjs_wrap_entry *hit;
	dom_node *owner_doc;
	JSValue obj;
	dom_string *tag_ds = NULL;
	const char *tag_str = "";
	char tag_lc[32];
	char tag_uc[32];
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

	if (qjs_wrap_insert(node, owner_doc, obj, JS_GetRuntime(ctx)) == 0) {
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
		tag_uc[i] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
	}
	tag_lc[i] = '\0';
	tag_uc[i] = '\0';
	if (tag_ds) macsurf_dom_string_unref(tag_ds);

	/* fixes1127 -- route the wrapper through its tag's constructor
	 * prototype so instanceof answers truthfully for the whole DOM family
	 * (HTMLDivElement -> HTMLElement -> Element -> Node via the class proto
	 * chain).  On a miss the wrapper keeps the class proto, which still
	 * answers instanceof HTMLElement/Element/Node. */
	{
		JSValue tp = qjs_dom_ctor_proto(ctx, tag_lc);
		if (JS_IsObject(tp)) {
			JS_SetPrototype(ctx, obj, tp);
		}
		JS_FreeValue(ctx, tp);
	}
	/* fixes1168 (#299) - tagName is UPPERCASE for HTML elements, matching
	 * every real browser (nodeName is the qualified name - also uppercase
	 * per spec - but is left as-is here to limit the blast radius to what
	 * #299 asked for). The lowercase form still feeds the constructor
	 * prototype lookup above. */
	JS_SetPropertyStr(ctx, obj, "tagName",  JS_NewString(ctx, tag_uc));
	JS_SetPropertyStr(ctx, obj, "nodeName", JS_NewString(ctx, tag_lc));
	JS_SetPropertyStr(ctx, obj, "nodeType", JS_NewInt32(ctx, 1));
	JS_SetPropertyStr(ctx, obj, "__ptr",
		JS_NewInt64(ctx, (long long)(size_t)el));

	/* fixesXXXX (#211) — the whole method surface (event bridge, attributes,
	 * textContent/innerHTML, mutation, query, classList/style/dataset,
	 * metrics, node traversal) lives on the wrapper class proto p and on
	 * Node.prototype, installed ONCE per realm by qjs_el_install_proto_surface
	 * inside qjs_el_install_proto. A wrap is now identity props + a wrap-table
	 * entry; g_wrap_installs is the audit count of the wrappers built. */
	g_wrap_installs++;
	return obj;
}

/* ---- getAttribute / setAttribute as QJS C functions registered on    */
/*      the wrapper class proto (qjs_el_install_proto_surface, once per */
/*      realm, fixesXXXX #211) — the element is `this` at call time.    */

/* getAttribute / setAttribute as CFunctionData: the element is `this`. */
static JSValue qjs_el_getAttribute_data(JSContext *ctx,
		JSValueConst this_val, int argc, JSValueConst *argv,
		int magic, JSValueConst *func_data)
{
	dom_element *el;
	const char *name_cstr;
	dom_string *name_ds, *val_ds;
	JSValue ret;

	(void)this_val; (void)magic;
	el = (dom_element *)qjs_get_node(this_val);
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

/* ====================================================================== */
/* fixes1015 - THE FULL JS/DOM AUDIT TRAIL.
 *
 * Multiple rounds have died guessing what a page did from aggregate counters
 * (mutcensus counts mutations but not WHAT mutated; the event gate counts
 * types but not targets; geometry reads were invisible). This block gives
 * every audit line an IDENTITY: which element, which value, which listener,
 * which read. All LIFE-prefixed (anything else is dropped by the failures-only
 * gate), all budgeted so a runaway page cannot flood the log forever -- the
 * budgets are sized to cover the whole first page load, which is the part
 * that has been going wrong.
 *
 * This is diagnostic instrumentation: when the current bug class is closed,
 * the budgets can be dropped, but the helpers should stay -- identity-free
 * logging is how we got here. */

/* Copy src into dst (cap includes the NUL), mapping CR/LF/TAB to spaces --
 * the log is CR-terminated, so an embedded newline would split the line. */
static void qjs_audit_copy(char *dst, int cap, const char *src)
{
	int i = 0;
	if (cap <= 0) return;
	if (src != NULL) {
		while (i < cap - 1 && src[i] != '\0') {
			char ch = src[i];
			if (ch == '\r' || ch == '\n' || ch == '\t') ch = ' ';
			dst[i] = ch;
			i++;
		}
	}
	dst[i] = '\0';
}

/* "TAG#id.class" for an element (id/class truncated), "(ntypeN)" otherwise.
 * NEVER touches a vtable on a non-element (the fixes846 lesson). */
static void qjs_node_brief(dom_node *n, char *out, int cap)
{
	dom_node_type nt;
	dom_string *ds;
	dom_string *nds;
	int pos = 0;

	if (cap <= 0) return;
	out[0] = '\0';
	if (n == NULL) { qjs_audit_copy(out, cap, "(null)"); return; }
	nt = (dom_node_type)0;
	if (macsurf_dom_node_get_node_type(n, &nt) != DOM_NO_ERR) {
		qjs_audit_copy(out, cap, "(?)");
		return;
	}
	if ((int)nt != 1) {  /* 1 == ELEMENT; 3 text, 9 document, 11 fragment */
		char tmp[16];
		sprintf(tmp, "(ntype%d)", (int)nt);
		qjs_audit_copy(out, cap, tmp);
		return;
	}
	ds = NULL;
	if (macsurf_dom_element_get_tag_name((dom_element *)n, &ds) == DOM_NO_ERR
			&& ds != NULL) {
		const char *d = dom_string_data(ds);
		int i = 0;
		while (d[i] != '\0' && i < 12 && pos < cap - 1)
			out[pos++] = d[i++];
		macsurf_dom_string_unref(ds);
	}
	nds = qjs_make_domstr("id");
	if (nds != NULL) {
		ds = NULL;
		if (macsurf_dom_element_get_attribute((dom_element *)n, nds, &ds)
				== DOM_NO_ERR && ds != NULL) {
			const char *d = dom_string_data(ds);
			int i = 0;
			if (d[0] != '\0' && pos < cap - 1) out[pos++] = '#';
			while (d[i] != '\0' && i < 24 && pos < cap - 1)
				out[pos++] = d[i++];
			macsurf_dom_string_unref(ds);
		}
		macsurf_dom_string_unref(nds);
	}
	nds = qjs_make_domstr("class");
	if (nds != NULL) {
		ds = NULL;
		if (macsurf_dom_element_get_attribute((dom_element *)n, nds, &ds)
				== DOM_NO_ERR && ds != NULL) {
			const char *d = dom_string_data(ds);
			int i = 0;
			if (d[0] != '\0' && pos < cap - 1) out[pos++] = '.';
			while (d[i] != '\0' && i < 32 && pos < cap - 1) {
				out[pos++] = (d[i] == ' ') ? '.' : d[i];
				i++;
			}
			macsurf_dom_string_unref(ds);
		}
		macsurf_dom_string_unref(nds);
	}
	out[pos] = '\0';
}

/* One line per DOM mutation: op, target identity, and the value/detail.
 * Budgeted per session; the first page load is what matters. */
long g_mut_audit_budget = 500;  /* exported for audit */
/* fixes1029 - removals get their own budget, independent of the audit
 * switch: they are the one mutation class that can DELETE page content. */
long g_rm_audit_budget = 120;  /* exported for audit */
long g_evreg_audit = 250;  /* exported for audit */
long g_evmiss_audit = 60;  /* exported for audit */
long g_evfire_audit = 300;  /* exported for audit */
long g_mslife_audit = 250;  /* exported for audit */
/* fixes1246 (#167) - console.error/console.warn just went from invisible
 * to LIFE-visible; a page's own high-frequency warning (e.g. a
 * deprecation notice logged on every scroll/render) could otherwise flood
 * the log the same way any other unconditional per-event line would.
 * Generous on purpose -- this budget exists to catch a genuine diagnostic
 * BURST (many distinct React warnings during one failed mount), not to
 * make error reporting rare. */
long g_console_err_audit = 200;  /* exported for audit */
extern long g_geom_audit; /* defined below, near the metric accessors */
/* fixes1110 -- parentNode "not attached" diagnostic budget (fixes1004,
 * tag name added fixes1109). Was a function-local `static int`, so the cap
 * was PROCESS-lifetime: hackaday's 8 fires exhausted it before 68kmla ever
 * loaded in the same session, and the 68kmla-specific tag was never seen.
 * Reset alongside the other audit budgets in macsurf_qjs_audit_reset() so
 * every navigation gets its own 8. */
int g_pn_logged = 0;  /* exported for audit */


static void qjs_mut_audit(const char *op, dom_node *target,
		const char *arg, const char *val)
{
	char tb[80];
	char vb[100];
	if (g_mut_audit_budget <= 0) return;
	g_mut_audit_budget--;
	qjs_node_brief(target, tb, (int)sizeof tb);
	qjs_audit_copy(vb, (int)sizeof vb, val);
	macsurf_debug_log_writef("LIFE mut %s %s %s %s",
			op, tb, (arg != NULL) ? arg : "", vb);
	if (g_mut_audit_budget == 0)
		macsurf_debug_log_writef("LIFE mut audit budget exhausted "
				"(further mutations counted in mutcensus only)");
}
/* ====================================================================== */

static JSValue qjs_el_setAttribute_data(JSContext *ctx,
		JSValueConst this_val, int argc, JSValueConst *argv,
		int magic, JSValueConst *func_data)
{
	dom_element *el;
	const char *name_cstr, *val_cstr;
	dom_string *name_ds, *val_ds;
	int attr_kind;	/* fixes926 */

	(void)this_val; (void)magic;
	el = (dom_element *)qjs_get_node(this_val);
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
	/* fixes926 - classify BEFORE the name is freed. Only class/style could
	 * ever be answered by a recascade rather than a box rebuild; every other
	 * attribute is baked at box construction or reaches nothing. */
	attr_kind = MACOS9_DOMMUT_SETATTRIBUTE;
	if (strcmp(name_cstr, "class") == 0) {
		attr_kind = MACOS9_DOMMUT_SETATTR_CLASS;
	} else if (strcmp(name_cstr, "style") == 0) {
		attr_kind = MACOS9_DOMMUT_SETATTR_STYLE;
	}
	/* fixes1015 - audit WHO sets WHAT to WHAT. */
	qjs_mut_audit("setattr", (dom_node *)el, name_cstr, val_cstr);
	JS_FreeCString(ctx, name_cstr);
	JS_FreeCString(ctx, val_cstr);
	if (name_ds && val_ds) {
		macsurf_dom_element_set_attribute(el, name_ds, val_ds);
		if (g_qjs_content) macos9_js_mark_dom_dirty_node(g_qjs_content,
				(void *) el, attr_kind);
	}
	if (name_ds) macsurf_dom_string_unref(name_ds);
	if (val_ds)  macsurf_dom_string_unref(val_ds);
	return JS_UNDEFINED;
}

/* ---- Forward declarations ---- */
static JSValue qjs_wrap_element_full(JSContext *ctx, dom_element *el);
static void qjs_collect_by_tag(JSContext *ctx, dom_node *node,
		const char *tag_lc, JSValue arr, int *count);

/* fixes880 - the CSS-selector matcher's types and entry points. The matcher
 * bodies live further down (near qjs_sel_parse); only the type definitions had
 * to move up here, because qjs_el_qsa_data holds a `struct qjs_sel` by value
 * and a forward declaration is not enough for that. Selector support and the
 * `approx` fallback are documented at the matcher itself. */
#define QJS_SEL_MAX_COMPOUND 4
#define QJS_SEL_MAX_CLASS    4
#define QJS_SEL_NAME         64
#define QJS_SEL_MAX_ATTR     4

/* fixes1090c - attribute selectors, e.g. img[data-lazy]. `op` is 0 for bare
 * presence ([attr]) or one of '=' '~' '^' '$' '*' matching the CSS operator
 * of the same shorthand (~= word, ^= prefix, $= suffix, *= substring). */
struct qjs_sel_attr {
	char name[QJS_SEL_NAME];
	char op;
	char val[QJS_SEL_NAME];
};

/* fixes1240 (#167) - :not(X), X a single simple selector (tag / .class /
 * #id / one [attr] clause -- the overwhelmingly common real-world shape,
 * e.g. Facebook's own `script[data-sjs]:not([data-processed])`). A
 * combined/compound or multi-selector :not() (`:not(.a.b)`,
 * `:not(a, b)`) still degrades to the existing approx/tag-only fallback,
 * same posture as every other unsupported selector shape here. Mirrors
 * qjs_sel_compound's own matchable fields exactly (not embedded by value
 * to avoid a self-referential struct) so qjs_simple_match can match
 * either one through the same code. */
struct qjs_sel_not_target {
	char tag[32];
	char cls[QJS_SEL_MAX_CLASS][QJS_SEL_NAME];
	int  ncls;
	char id[QJS_SEL_NAME];
	struct qjs_sel_attr attr[QJS_SEL_MAX_ATTR];
	int  nattr;
};

struct qjs_sel_compound {
	char tag[32];                                 /* "" = any, or lowercase */
	char cls[QJS_SEL_MAX_CLASS][QJS_SEL_NAME];
	int  ncls;
	char id[QJS_SEL_NAME];                        /* "" = none */
	struct qjs_sel_attr attr[QJS_SEL_MAX_ATTR];
	int  nattr;
	int  has_not;                                 /* fixes1240 */
	struct qjs_sel_not_target nott;               /* fixes1240 */
};

struct qjs_sel {
	struct qjs_sel_compound c[QJS_SEL_MAX_COMPOUND];
	int n;      /* compound count; c[n-1] is the SUBJECT (rightmost) */
	int approx; /* 1 = selector had syntax we ignored (tag-only fallback) */
};

/* fixes1242 (#167) - comma-separated selector LISTS, e.g.
 * `document.querySelectorAll('button, [role="button"], [tabindex="0"]')` --
 * confirmed live in Facebook's own bundles (12 call sites across the 18
 * scripts one profile-page load executes, plus React's own DOM reconciler
 * internals use the shape constantly). Previously any ',' inside a
 * selector hit qjs_sel_parse's generic "unsupported combinator" branch,
 * which set ->approx and discarded everything from the comma onward --
 * `'a, b'` silently became just `'a'`.
 *
 * Each alternative is an independent `struct qjs_sel` (its own descendant
 * chain); a node matches the list if it matches ANY alternative. Deliberately
 * a SEPARATE struct/function family layered on top of the existing
 * single-alternative qjs_sel/qjs_sel_match/qjs_collect_by_sel/
 * qjs_find_first_by_sel rather than changing their signatures -- those three
 * are also called internally (qjs_collect_by_sel recurses on itself, both
 * :not() helpers reuse qjs_simple_match) and widening them would have meant
 * auditing every recursive call site for a redundant list-of-one wrap. */
#define QJS_SEL_MAX_LIST 8
struct qjs_sel_list {
	struct qjs_sel alt[QJS_SEL_MAX_LIST];
	int n;
	int approx; /* 1 if ANY alternative degraded, or the list overflowed */
};

static void qjs_sel_parse(const char *sel, struct qjs_sel *out);
static void qjs_sel_list_parse(const char *sel, struct qjs_sel_list *out);
static void qjs_collect_by_sel(JSContext *ctx, dom_node *node,
		const struct qjs_sel *s, JSValue arr, int *count);
static void qjs_collect_by_sel_list(JSContext *ctx, dom_node *node,
		const struct qjs_sel_list *sl, JSValue arr, int *count);
/* fixes878 - node-type-dispatching wrapper, defined after the three concrete
 * wrappers.  The node-oriented traversal getters below live on
 * Node.prototype via qjs_el_install_proto_surface (fixes1170, #211), so they
 * are reachable through this declaration on every wrapper shape. */
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
	el = (dom_element *)qjs_get_node(this_val);
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
	el = (dom_element *)qjs_get_node(this_val);
	if (el == NULL || argc < 1) return JS_UNDEFINED;
	s = JS_ToCString(ctx, argv[0]);
	if (s == NULL) return JS_UNDEFINED;
	ds = qjs_make_domstr(s);
	if (g_rm_audit_budget > 0) {	/* fixes1030 */
		char pb[80], vb[64];
		g_rm_audit_budget--;
		qjs_node_brief((dom_node *)el, pb, (int)sizeof pb);
		qjs_audit_copy(vb, (int)sizeof vb, s);
		macsurf_debug_log_writef("LIFE tc %s <- \"%s\"", pb, vb);
	}
	/* fixes1031 - textContent = "" MUST LEAVE NO TEXT NODE BEHIND.
	 *
	 * DOM says: "Let node be null. If the given value is not the empty
	 * string, set node to a new Text node whose data is the given value",
	 * then replace all children with node. Empty string therefore means
	 * REMOVE EVERYTHING AND ADD NOTHING. libdom's set_text_content adds an
	 * empty Text node instead, and that one stray node is what has been
	 * destroying real pages:
	 *
	 * jQuery's buildFragment -- the path behind $(html), .append(html),
	 * .wrapInner(), .wrapAll() and much else -- clears its scratch
	 * fragment with `fragment.textContent = ""` before collecting the
	 * parsed nodes. With an empty Text node left in place, EVERY
	 * jQuery(htmlString) comes back one node longer than it should, with
	 * that text node FIRST. jQuery.wrapAll then does
	 *
	 *     wrap = jQuery( html, ... ).eq( 0 ).clone( true );
	 *
	 * so `wrap` is the stray TEXT NODE rather than the wrapper element,
	 * and the subsequent `.append( this )` moves the element's entire
	 * contents into a text node -- which cannot hold children. The content
	 * is gone.
	 *
	 * That is hackaday's article river: the dotdotdot plugin calls
	 * wrapInner() on every entry, and each entry reaches the box tree with
	 * kids=0. Locally reproduced with the real plugin (harness Test 45).
	 * The blast radius is far wider than one site -- wrapAll/wrapInner are
	 * ordinary jQuery, and a leading phantom text node also perturbs
	 * .eq(0), .first(), :first-child logic and index arithmetic anywhere a
	 * fragment is built from markup. */
	JS_FreeCString(ctx, s);
	if (ds) {
		if (dom_string_length(ds) == 0) {
			/* Empty: remove every child, add nothing. */
			dom_node *ch = NULL;
			if (macsurf_dom_node_get_first_child((dom_node *)el, &ch)
					!= DOM_NO_ERR)
				ch = NULL;
			while (ch != NULL) {
				dom_node *nx = NULL;
				dom_node *removed = NULL;
				if (macsurf_dom_node_get_next_sibling(ch, &nx)
						!= DOM_NO_ERR)
					nx = NULL;
				if (macsurf_dom_node_remove_child((dom_node *)el,
						ch, &removed) == DOM_NO_ERR &&
						removed != NULL)
					macsurf_dom_node_unref(removed);
				macsurf_dom_node_unref(ch);
				ch = nx;
			}
		} else {
			macsurf_dom_node_set_text_content((dom_node *)el, ds);
		}
		macsurf_dom_string_unref(ds);
		if (g_qjs_content) macos9_js_mark_dom_dirty_node(g_qjs_content,
				(void *) el, MACOS9_DOMMUT_TEXTCONTENT);
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
	dom_node *src_parent, *html_el, *body_el, *head_el;
	dom_hubbub_parser_params params;
	dom_hubbub_parser *parser = NULL;
	dom_hubbub_error herr;
	dom_document_fragment *frag = NULL;
	const char *html_src;
	size_t html_len = 0;

	(void) this_val; (void) magic;
	el = (dom_element *) qjs_get_node(this_val);
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
	/* fixes1015 - audit with the markup head while the string is alive. */
	if (g_rm_audit_budget > 0) {	/* fixes1030 */
		char pb[80], vb[64];
		g_rm_audit_budget--;
		qjs_node_brief((dom_node *)el, pb, (int)sizeof pb);
		qjs_audit_copy(vb, (int)sizeof vb, html_src);
		macsurf_debug_log_writef("LIFE ih %s len=%ld <- \"%s\"",
				pb, (long)html_len, vb);
	}
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
		/* fixes1007 - TAKE <head>'s CHILDREN TOO, and take them FIRST.
		 *
		 * This binding's fragment parser has no context-element support
		 * (fixes846): it wraps the markup in an implied <html><head>/<body>
		 * exactly as if parsing a whole page. So markup that STARTS with a
		 * head-only element -- <script>, <style>, <link>, <meta>, <title> --
		 * has it placed in <head>, and descending straight to <body> dropped
		 * it on the floor, silently.
		 *
		 * Measured: `div.innerHTML = '<script src=...></script>'` produced
		 * ZERO children and empty textContent. That is not a corner case --
		 * it is what document.write() hands us on hackaday (its ad script
		 * writes a <script> tag), and it is a common innerHTML pattern in its
		 * own right.
		 *
		 * Head first, then body, so source order survives the round trip.
		 * The move loop below re-fetches first_child each iteration, so it is
		 * safe to run it twice over different parents. */
		head_el = qjs_find_child_element_by_tag(html_el, "head");
		if (head_el != NULL) {
			child = NULL;
			macsurf_dom_node_get_first_child(head_el, &child);
			while (child != NULL) {
				append_result = NULL;
				macsurf_dom_node_append_child((dom_node *) el,
						child, &append_result);
				if (append_result != NULL)
					macsurf_dom_node_unref(append_result);
				macsurf_dom_node_unref(child);
				child = NULL;
				macsurf_dom_node_get_first_child(head_el, &child);
			}
			macsurf_dom_node_unref(head_el);
		}
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

	if (g_qjs_content) macos9_js_mark_dom_dirty_node(g_qjs_content,
			(void *) el, MACOS9_DOMMUT_INNERHTML);
	return JS_UNDEFINED;
}

/* ---- innerHTML read-back: real HTML serializer (fixes1168, #262) ----
 * The innerHTML GETTER previously returned el.textContent - every tag
 * stripped, so jQuery .html() and any read-modify-write pattern
 *     el.innerHTML = el.innerHTML + more
 * got plain text back and silently corrupted content on the next write.
 * __getInnerHTML mirrors __setInnerHTML: a C serializer walking the real
 * child nodes, emitting elements (lowercase tag, attributes), escaped text,
 * and comments. Void elements (br/img/input/...) get no end tag; script and
 * style children are emitted RAW like browsers do (their content is not
 * entity-decoded on re-parse, so escaping would corrupt it).
 *
 * Attributes are enumerated from the real namednodemap (not a fixed list)
 * so src/href/class/id/style/type/name/value/checked/disabled/selected and
 * every data-* attribute come through without a per-attribute whitelist to
 * rot. */

struct qjs_ih_buf {
	char *data;
	size_t len;
	size_t cap;
};

static int qjs_ih_reserve(struct qjs_ih_buf *b, size_t extra)
{
	size_t need;
	size_t ncap;
	char *nd;

	if (extra > (size_t) -1 - b->len) return -1;
	need = b->len + extra;
	if (need <= b->cap) return 0;
	ncap = b->cap ? b->cap : 256;
	while (ncap < need) {
		if (ncap > (size_t) -1 / 2) { ncap = need; break; }
		ncap *= 2;
	}
	nd = (char *) malloc(ncap);
	if (nd == NULL) return -1;
	if (b->data) {
		memcpy(nd, b->data, b->len);
		free(b->data);
	}
	b->data = nd;
	b->cap = ncap;
	return 0;
}

static int qjs_ih_append(struct qjs_ih_buf *b, const char *s, size_t n)
{
	if (qjs_ih_reserve(b, n)) return -1;
	memcpy(b->data + b->len, s, n);
	b->len += n;
	return 0;
}

static int qjs_ih_append_cstr(struct qjs_ih_buf *b, const char *s)
{
	return qjs_ih_append(b, s, strlen(s));
}

/* Escape & < > for text content (and attribute values via
 * qjs_ih_append_esc_attr, which also escapes the quote that will delimit
 * them). UTF-8 bytes pass through untouched - escaping is byte-wise and the
 * characters escaped are single ASCII bytes, so multibyte sequences can
 * never be split. */
static int qjs_ih_append_esc(struct qjs_ih_buf *b, const char *s, size_t n)
{
	size_t i;
	for (i = 0; i < n; i++) {
		char c = s[i];
		switch (c) {
		case '&':
			if (qjs_ih_append_cstr(b, "&amp;")) return -1;
			break;
		case '<':
			if (qjs_ih_append_cstr(b, "&lt;")) return -1;
			break;
		case '>':
			if (qjs_ih_append_cstr(b, "&gt;")) return -1;
			break;
		default:
			if (qjs_ih_append(b, &c, 1)) return -1;
			break;
		}
	}
	return 0;
}

static int qjs_ih_append_esc_attr(struct qjs_ih_buf *b, const char *s,
		size_t n)
{
	size_t i;
	for (i = 0; i < n; i++) {
		char c = s[i];
		switch (c) {
		case '&':
			if (qjs_ih_append_cstr(b, "&amp;")) return -1;
			break;
		case '<':
			if (qjs_ih_append_cstr(b, "&lt;")) return -1;
			break;
		case '>':
			if (qjs_ih_append_cstr(b, "&gt;")) return -1;
			break;
		case '"':
			if (qjs_ih_append_cstr(b, "&quot;")) return -1;
			break;
		default:
			if (qjs_ih_append(b, &c, 1)) return -1;
			break;
		}
	}
	return 0;
}

static int qjs_ih_is_void(const char *tag)
{
	static const char *const voids[] = {
		"area", "base", "br", "col", "embed", "hr", "img", "input",
		"link", "meta", "param", "source", "track", "wbr", NULL
	};
	int i;
	for (i = 0; voids[i] != NULL; i++) {
		if (strcmp(tag, voids[i]) == 0) return 1;
	}
	return 0;
}

static int qjs_ih_serialize_element(struct qjs_ih_buf *b, dom_element *el);
static int qjs_ih_serialize_node(struct qjs_ih_buf *b, dom_node *node)
{
	dom_node_type ntype = 0;
	dom_string *ds = NULL;
	int r = 0;

	macsurf_dom_node_get_node_type(node, &ntype);
	if (ntype == 1) {
		return qjs_ih_serialize_element(b, (dom_element *) node);
	}
	if (ntype == 3 || ntype == 4) {	/* text / CDATA */
		if (macsurf_dom_characterdata_get_data(node, &ds) == DOM_NO_ERR
				&& ds != NULL) {
			r = qjs_ih_append_esc(b, dom_string_data(ds),
					(size_t) dom_string_length(ds));
			macsurf_dom_string_unref(ds);
		}
		return r;
	}
	if (ntype == 8) {	/* comment */
		if (qjs_ih_append_cstr(b, "<!--")) return -1;
		if (macsurf_dom_characterdata_get_data(node, &ds) == DOM_NO_ERR
				&& ds != NULL) {
			r = qjs_ih_append(b, dom_string_data(ds),
					(size_t) dom_string_length(ds));
			macsurf_dom_string_unref(ds);
			if (r) return -1;
		}
		return qjs_ih_append_cstr(b, "-->");
	}
	return 0;	/* document / fragment / other: no markup of their own */
}

/* Walk every child, serializing each. Drains the sibling chain even after an
 * error so no ref is leaked. */
static int qjs_ih_serialize_children(struct qjs_ih_buf *b, dom_node *parent)
{
	dom_node *child = NULL, *next = NULL;
	int r = 0;

	macsurf_dom_node_get_first_child(parent, &child);
	while (child != NULL) {
		next = NULL;
		macsurf_dom_node_get_next_sibling(child, &next);
		if (r == 0) r = qjs_ih_serialize_node(b, child);
		macsurf_dom_node_unref(child);
		child = next;
	}
	return r;
}

/* Raw-text children (script/style): emit the data UNESCAPED. Browsers do
 * this because script/style content is raw text on re-parse - &lt; would
 * NOT decode back to <, so escaping would change what re-parsing reads. */
static int qjs_ih_serialize_raw_children(struct qjs_ih_buf *b,
		dom_node *parent)
{
	dom_node *child = NULL, *next = NULL;
	dom_node_type nt = 0;
	int r = 0;

	macsurf_dom_node_get_first_child(parent, &child);
	while (child != NULL) {
		next = NULL;
		macsurf_dom_node_get_next_sibling(child, &next);
		if (r == 0) {
			nt = 0;
			macsurf_dom_node_get_node_type(child, &nt);
			if (nt == 3 || nt == 4) {
				dom_string *ds = NULL;
				if (macsurf_dom_characterdata_get_data(child, &ds)
						== DOM_NO_ERR && ds != NULL) {
					r = qjs_ih_append(b, dom_string_data(ds),
							(size_t) dom_string_length(ds));
					macsurf_dom_string_unref(ds);
				}
			}
		}
		macsurf_dom_node_unref(child);
		child = next;
	}
	return r;
}

/* Attributes: enumerate the element's real namednodemap. The map and each
 * item node / name / value come back ref'd; everything is unref'd here. */
static int qjs_ih_serialize_attrs(struct qjs_ih_buf *b, dom_element *el)
{
	dom_namednodemap *map = NULL;
	dom_ulong len = 0;
	dom_ulong i;
	int r = 0;

	if (macsurf_dom_node_get_attributes((dom_node *) el, &map) != DOM_NO_ERR
			|| map == NULL)
		return 0;
	if (dom_namednodemap_get_length(map, &len) != DOM_NO_ERR) len = 0;
	for (i = 0; i < len && r == 0; i++) {
		dom_node *an = NULL;
		dom_string *aname = NULL;
		dom_string *aval = NULL;
		if (dom_namednodemap_item(map, i, &an) != DOM_NO_ERR || an == NULL)
			continue;
		if (macsurf_dom_attr_get_name(an, &aname) != DOM_NO_ERR
				|| aname == NULL) {
			macsurf_dom_node_unref(an);
			continue;
		}
		r = qjs_ih_append(b, " ", 1);
		if (!r) r = qjs_ih_append(b, dom_string_data(aname),
				(size_t) dom_string_length(aname));
		if (!r) r = qjs_ih_append_cstr(b, "=\"");
		if (!r) {
			if (macsurf_dom_attr_get_value(an, &aval) == DOM_NO_ERR
					&& aval != NULL) {
				r = qjs_ih_append_esc_attr(b,
						dom_string_data(aval),
						(size_t) dom_string_length(aval));
				macsurf_dom_string_unref(aval);
			}
		}
		if (!r) r = qjs_ih_append(b, "\"", 1);
		macsurf_dom_string_unref(aname);
		macsurf_dom_node_unref(an);
	}
	dom_namednodemap_unref(map);
	return r;
}

static int qjs_ih_serialize_element(struct qjs_ih_buf *b, dom_element *el)
{
	dom_string *tname = NULL;
	const char *tag = "";
	char tag_lc[64];
	int i;
	int r = 0;
	int raw = 0;

	if (macsurf_dom_element_get_tag_name(el, &tname) != DOM_NO_ERR
			|| tname == NULL)
		return 0;
	tag = dom_string_data(tname);
	for (i = 0; i < 63 && tag[i]; i++) {
		char c = tag[i];
		tag_lc[i] = (c >= 'A' && c <= 'Z') ? (char) (c + 32) : c;
	}
	tag_lc[i] = '\0';

	raw = (strcmp(tag_lc, "script") == 0 || strcmp(tag_lc, "style") == 0);

	r = qjs_ih_append(b, "<", 1);
	if (!r) r = qjs_ih_append_cstr(b, tag_lc);
	if (!r) r = qjs_ih_serialize_attrs(b, el);
	if (!r) r = qjs_ih_append(b, ">", 1);
	if (!r && qjs_ih_is_void(tag_lc) == 0) {
		if (raw) {
			r = qjs_ih_serialize_raw_children(b, (dom_node *) el);
		} else {
			r = qjs_ih_serialize_children(b, (dom_node *) el);
		}
		if (!r) {
			r = qjs_ih_append_cstr(b, "</");
			if (!r) r = qjs_ih_append_cstr(b, tag_lc);
			if (!r) r = qjs_ih_append(b, ">", 1);
		}
	}
	macsurf_dom_string_unref(tname);
	return r;
}

/* fixes1168 (#262) - __getInnerHTML: serialize the element's children to
 * markup. Mirrors __setInnerHTML (qjs_el_set_inner_html_data): a C function
 * registered on the element wrapper, resolving its node via this_val. */
static JSValue qjs_el_get_inner_html_data(JSContext *ctx,
		JSValueConst this_val, int argc, JSValueConst *argv,
		int magic, JSValueConst *func_data)
{
	dom_element *el;
	struct qjs_ih_buf b;
	int r;
	JSValue ret;

	(void) this_val; (void) argc; (void) argv; (void) magic;
	el = (dom_element *) qjs_get_node(this_val);
	if (el == NULL) return JS_NewString(ctx, "");
	memset(&b, 0, sizeof(b));
	/* innerHTML is the markup of the element's DESCENDANTS only - the
	 * element's own tag and attributes belong to outerHTML. Serialize the
	 * children, not the element itself (a harness round-trip test caught
	 * the element-once version wrapping the re-parse in a stray child). */
	r = qjs_ih_serialize_children(&b, (dom_node *) el);
	if (r != 0 || b.data == NULL) {
		if (b.data) free(b.data);
		return JS_NewString(ctx, "");
	}
	ret = JS_NewStringLen(ctx, b.data, b.len);
	free(b.data);
	return ret;
}

/* fixes1168 (#262) - __getOuterHTML: same serializer, but the element
 * ITSELF (tag + attributes + children). The old JS-side outerHTML stub
 * wrapped innerHTML in the bare tag name, silently dropping every
 * attribute - a lying answer for the clone/echo patterns that read it. */
static JSValue qjs_el_get_outer_html_data(JSContext *ctx,
		JSValueConst this_val, int argc, JSValueConst *argv,
		int magic, JSValueConst *func_data)
{
	dom_element *el;
	struct qjs_ih_buf b;
	int r;
	JSValue ret;

	(void) this_val; (void) argc; (void) argv; (void) magic;
	el = (dom_element *) qjs_get_node(this_val);
	if (el == NULL) return JS_NewString(ctx, "");
	memset(&b, 0, sizeof(b));
	r = qjs_ih_serialize_node(&b, (dom_node *) el);
	if (r != 0 || b.data == NULL) {
		if (b.data) free(b.data);
		return JS_NewString(ctx, "");
	}
	ret = JS_NewStringLen(ctx, b.data, b.len);
	free(b.data);
	return ret;
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
	/* fixes1004 - WHICH null?
	 *
	 * preamble.min.js still throws "cannot read property 'removeChild' of
	 * null" at hiddenscroll, which does appendChild(b) then
	 * b.parentNode.removeChild(b). fixes1002 fixed the MOCK-body path, and
	 * the error survived, so the real libdom path returns null too -- but
	 * this getter has THREE ways to do that and they want three different
	 * fixes:
	 *   node   the wrapper has lost its node (a lifetime bug)
	 *   parent the append genuinely did not attach (a mutation bug)
	 *   type   the parent is not an ELEMENT, e.g. a fragment or the
	 *          document itself, and the ntype != 1 filter below discards a
	 *          perfectly good parent (a filter bug)
	 * Guessing a third time is worse than asking. Capped so a page that
	 * legitimately reads parentNode on detached nodes cannot flood. */
	{
		el = (dom_element *)qjs_get_node(this_val);
		if (el == NULL) {
			if (g_pn_logged < 8) { g_pn_logged++;
				macsurf_debug_log_write(
					"LIFE parentNode NULL why=node"); }
			return JS_NULL;
		}
		macsurf_dom_node_get_parent_node((dom_node *)el, &parent);
		if (parent == NULL) {
			if (g_pn_logged < 8) {
				/* fixes1109 (#265) -- WHICH node has no parent, and what
				 * tag is it? el is the wrapper's underlying dom_element,
				 * the same pointer identity appendChild's own qjs_get_node
				 * resolves for its child argument -- if a later reconvert
				 * or GC has freed/reused it, this pointer alone will not
				 * prove that, but the tag name pins down WHAT was being
				 * asked about (hiddenscroll's probe div vs something
				 * else), which the prior bare log line could not. */
				char tagbuf[24];
				dom_string *tn = NULL;
				g_pn_logged++;
				tagbuf[0] = '\0';
				if (macsurf_dom_element_get_tag_name(el, &tn) == DOM_NO_ERR
						&& tn != NULL) {
					const char *ts = dom_string_data(tn);
					size_t i;
					for (i = 0; i < sizeof(tagbuf) - 1 && ts[i]; i++)
						tagbuf[i] = ts[i];
					tagbuf[i] = '\0';
					macsurf_dom_string_unref(tn);
				}
				macsurf_debug_log_writef(
					"LIFE parentNode NULL why=parent (not attached) "
					"el=%p tag=%s", (void *)el, tagbuf);
			}
			return JS_NULL;
		}
		macsurf_dom_node_get_node_type(parent, &ntype);
		if (ntype != 1) {
			if (g_pn_logged < 8) { g_pn_logged++;
				macsurf_debug_log_writef(
					"LIFE parentNode NULL why=type ntype=%d",
					(int)ntype); }
			macsurf_dom_node_unref(parent);
			return JS_NULL;
		}
	}
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
	el = (dom_element *)qjs_get_node(this_val);
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
	el = (dom_element *)qjs_get_node(this_val);
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

/* fixes867 (#293) - THE BLINDFOLD, removed.
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

	/* Failure path only - these are diagnostic reads, kept off the hot path.
	 * get_owner_document hands back an OWNED ref; unref both below. */
	if (parent != NULL) macsurf_dom_node_get_owner_document(parent, &pdoc);
	if (child  != NULL) macsurf_dom_node_get_owner_document(child,  &cdoc);

	macsurf_debug_log_writef(
		"LIFE dom %s FAIL exc=%d seq=%d dispatching_mutation=%d "
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
	el = (dom_element *)qjs_get_node(this_val);
	if (el == NULL || argc < 1) return JS_NULL;
	child_el = (dom_element *)qjs_get_node(argv[0]);
	if (child_el == NULL) return JS_NULL;
	exc = macsurf_dom_node_append_child((dom_node *)el, (dom_node *)child_el,
			&result);
	if (result) macsurf_dom_node_unref(result);
	err = qjs_dom_mut_check(ctx, "appendChild", exc, (dom_node *)el,
			(dom_node *)child_el);
	if (JS_IsException(err)) return err;
	{	/* fixes1015 */
		char cb[80];
		qjs_node_brief((dom_node *)child_el, cb, (int)sizeof cb);
		qjs_mut_audit("appendChild", (dom_node *)el, "<-", cb);
	}
	if (g_qjs_content) macos9_js_mark_dom_dirty_node(g_qjs_content,
			(void *) el, MACOS9_DOMMUT_APPENDCHILD);
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
	el = (dom_element *)qjs_get_node(this_val);
	if (el == NULL || argc < 1) return JS_NULL;
	child_el = (dom_element *)qjs_get_node(argv[0]);
	if (child_el == NULL) return JS_NULL;
	exc = macsurf_dom_node_remove_child((dom_node *)el, (dom_node *)child_el,
			&result);
	if (result) macsurf_dom_node_unref(result);
	/* fixes867 (#293) - same blindfold as appendChild.  removeChild is hit by
	 * the SAME mutation semaphore (node.c:989 dispatches DOMNodeRemoval, and
	 * :744's readonly check guards the removal path too), so a JS remove from
	 * inside a mutation handler fails just as silently. */
	err = qjs_dom_mut_check(ctx, "removeChild", exc, (dom_node *)el,
			(dom_node *)child_el);
	if (JS_IsException(err)) return err;
	if (g_rm_audit_budget > 0) {	/* fixes1015/1029 */
		char cb[80], pb[80];
		g_rm_audit_budget--;
		qjs_node_brief((dom_node *)child_el, cb, (int)sizeof cb);
		qjs_node_brief((dom_node *)el, pb, (int)sizeof pb);
		macsurf_debug_log_writef("LIFE rm %s -> %s", pb, cb);
	}
	if (g_qjs_content) macos9_js_mark_dom_dirty_node(g_qjs_content,
			(void *) el, MACOS9_DOMMUT_REMOVECHILD);
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
	el = (dom_element *)qjs_get_node(this_val);
	if (el == NULL || argc < 1) return JS_NULL;
	new_el = (dom_element *)qjs_get_node(argv[0]);
	if (new_el == NULL) return JS_NULL;
	ref_el = (argc >= 2 && !JS_IsNull(argv[1]))
		? (dom_element *)qjs_get_node(argv[1]) : NULL;
	exc = macsurf_dom_node_insert_before((dom_node *)el, (dom_node *)new_el,
		(dom_node *)ref_el, &result);
	if (result) macsurf_dom_node_unref(result);
	/* fixes867 (#293) - same blindfold as appendChild, and this one matters
	 * most for modern pages: insertBefore(node, null) is Preact's ONLY
	 * insertion primitive (it never calls appendChild), so a silent rejection
	 * here means a React/Preact app renders nothing, with no error. */
	err = qjs_dom_mut_check(ctx, "insertBefore", exc, (dom_node *)el,
			(dom_node *)new_el);
	if (JS_IsException(err)) return err;
	{	/* fixes1015 */
		char cb[80];
		qjs_node_brief((dom_node *)new_el, cb, (int)sizeof cb);
		qjs_mut_audit("insertBefore", (dom_node *)el, "<-", cb);
	}
	if (g_qjs_content) macos9_js_mark_dom_dirty_node(g_qjs_content,
			(void *) el, MACOS9_DOMMUT_INSERTBEFORE);
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
	int rm_kind;	/* fixes926 */
	(void)this_val; (void)magic;
	el = (dom_element *)qjs_get_node(this_val);
	if (el == NULL || argc < 1) return JS_UNDEFINED;
	name_cstr = JS_ToCString(ctx, argv[0]);
	if (name_cstr == NULL) return JS_UNDEFINED;
	name_ds = qjs_make_domstr(name_cstr);
	/* fixes926 - classify before the free (see setAttribute). */
	rm_kind = MACOS9_DOMMUT_REMOVEATTRIBUTE;
	if (strcmp(name_cstr, "class") == 0) {
		rm_kind = MACOS9_DOMMUT_SETATTR_CLASS;
	} else if (strcmp(name_cstr, "style") == 0) {
		rm_kind = MACOS9_DOMMUT_SETATTR_STYLE;
	}
	qjs_mut_audit("rmattr", (dom_node *)el, name_cstr, NULL); /* fixes1015 */
	JS_FreeCString(ctx, name_cstr);
	if (name_ds) {
		macsurf_dom_element_remove_attribute(el, name_ds);
		/* fixes843b - this real DOM mutation never marked dirty, so
		 * el.removeAttribute(...) (a common show/hide idiom) never
		 * triggered a repaint. Match setAttribute's behaviour. */
		if (g_qjs_content) macos9_js_mark_dom_dirty_node(g_qjs_content,
				(void *) el, rm_kind);
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
	el = (dom_element *)qjs_get_node(this_val);
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
	self = qjs_get_node(this_val);
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
	self = qjs_get_node(this_val);
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
	self = qjs_get_node(this_val);
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
	self = qjs_get_node(this_val);
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
	el = (dom_element *)qjs_get_node(this_val);
	arr = JS_NewArray(ctx);
	if (el == NULL) return arr;
	if (macsurf_dom_node_get_first_child((dom_node *)el, &child) !=
			DOM_NO_ERR)
		return arr;
	while (child) {
		if (macsurf_dom_node_get_node_type(child, &ntype) != DOM_NO_ERR) {
			macsurf_dom_node_unref(child);
			break;
		}
		next = NULL;
		if (macsurf_dom_node_get_next_sibling(child, &next) != DOM_NO_ERR)
			next = NULL;
		if (ntype == 1) {
			JSValue w = qjs_wrap_element_full(ctx, (dom_element *)child);
			JS_SetPropertyUint32(ctx, arr, (unsigned int)count, w);
			count++;
		} else {
			macsurf_dom_node_unref(child);
		}
		child = next;
	}
	return arr;
}

/* ---- element.querySelectorAll (scoped) ---- */
/* fixes880 - element-scoped querySelectorAll now uses the SAME matcher the
 * document level has used since fixes871 (qjs_sel_parse + qjs_collect_by_sel).
 *
 * It used to truncate the selector at the first '[', '.', ':' or ' ' and call
 * qjs_collect_by_tag on whatever prefix was left. Two silent wrong answers fell
 * out of that, in opposite directions:
 *   - `el.querySelector('.foo')` truncated to the EMPTY string and returned
 *     null. Every class-scoped lookup on the web.
 *   - `el.querySelectorAll('div.bar')` truncated to `div` and returned EVERY
 *     descendant div, ignoring the class entirely.
 * Neither threw, so a page just quietly did the wrong thing. fixes871 fixed the
 * document level and left this one behind.
 *
 * SCOPE: descendants only -- qjs_collect_by_sel matches the node it is handed,
 * which is right for the document walk (document.querySelector('html') must
 * find <html>) but wrong here: per spec the scope element itself never matches
 * its own query. So the walk starts at each CHILD rather than at `el`.
 *
 * Descendant combinators still resolve against the full ancestor chain, above
 * `el` as well -- container.querySelector('div p') matching a <p> whose <div>
 * ancestor is outside the container is correct per spec, not a leak. */
static JSValue qjs_el_qsa_data(JSContext *ctx,
		JSValueConst this_val, int argc, JSValueConst *argv,
		int magic, JSValueConst *func_data)
{
	dom_element *el;
	const char *sel;
	int count = 0;
	JSValue arr;
	struct qjs_sel_list s;
	dom_node *child = NULL;
	dom_node *next = NULL;

	(void)this_val; (void)magic;
	arr = JS_NewArray(ctx);
	el = (dom_element *)qjs_get_node(this_val);
	if (el == NULL || argc < 1) return arr;
	sel = JS_ToCString(ctx, argv[0]);
	if (sel == NULL) return arr;

	qjs_sel_list_parse(sel, &s);
	if (s.approx) {
		macsurf_debug_log_writef(
			"WORK el.qsa: APPROX selector (unsupported syntax ignored): %s",
			sel);
	}
	JS_FreeCString(ctx, sel);
	if (s.n == 0) return arr;

	if (macsurf_dom_node_get_first_child((dom_node *)el, &child) != DOM_NO_ERR)
		return arr;
	while (child != NULL) {
		qjs_collect_by_sel_list(ctx, child, &s, arr, &count);
		if (macsurf_dom_node_get_next_sibling(child, &next) != DOM_NO_ERR)
			next = NULL;
		/* collect_by_sel refs again before wrapping, so this child's own
		 * ref from get_first_child/get_next_sibling is still ours to drop --
		 * same discipline as collect_by_sel's internal loop. */
		macsurf_dom_node_unref(child);
		child = next;
		next = NULL;
	}
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
/* ------------------------------------------------------------------
 * fixes1071 - COMPILE THE WRAPPER HELPERS ONCE, NOT PER ELEMENT.
 *
 * Every element wrapper installed four JS helper blocks, and each one was a
 * JS_Eval of a fixed C string literal -- i.e. QuickJS parsed and code-generated
 * the same ~13.9 KB of JavaScript again for every single element the page
 * touched:
 *
 *     <el-helpers>  10810 bytes   classList/style/dataset/matches/closest/...
 *     <node-nav>     1123 bytes   firstChild/nextSibling/... traversal
 *     <el-props>      841 bytes   textContent property
 *     <el-layout>    1084 bytes   offsetWidth/offsetHeight/...
 *
 * The fixes1070 hardware log priced compilation on this machine at 2.17us per
 * source byte (jQuery: 101134 bytes in 219008us), so that is ~30ms of pure
 * recompilation PER WRAPPER. It is also why hackaday looked "run-bound" with a
 * trivial compile total: this compile happens inside a C binding during script
 * execution, so every microsecond of it was charged to run_us.
 *
 * hackaday's navigation.js -- ONE 72KB script measured at 24.7 SECONDS, half
 * the entire page load -- walks the nav tree touching element after element.
 * ~800 wrappers is the whole 24.7s.
 *
 * The source is a compile-time constant, so the compiled function is reusable.
 * Cache it and JS_Call the same function object per wrapper.
 *
 * KEYED PER CONTEXT, and that is not incidental: every iframe gets its own
 * JSRuntime here, and a JSValue is only valid against the runtime that created
 * it. A single file-static JSValue would hand iframe B a function object owned
 * by iframe A's runtime -- the exact cross-runtime trap this codebase has
 * already been bitten by. Stashing it on the context's own global object makes
 * the cache per-context by construction and frees it with the context, with no
 * teardown hook to forget. Defined non-enumerable so page code walking
 * `for (k in window)` cannot see it.
 * ------------------------------------------------------------------ */

static JSValue qjs_helper_fn(JSContext *ctx, const char *key,
		const char *src, const char *fname)
{
	JSValue g;
	JSValue fn;

	g = JS_GetGlobalObject(ctx);
	fn = JS_GetPropertyStr(ctx, g, key);
	if (JS_IsFunction(ctx, fn)) {
		JS_FreeValue(ctx, g);
		return fn;		/* cache hit; caller owns this ref */
	}
	JS_FreeValue(ctx, fn);

	fn = JS_Eval(ctx, src, strlen(src), fname, JS_EVAL_TYPE_GLOBAL);
	g_helper_compiles++;
	g_helper_bytes += (long)strlen(src);
	if (JS_IsException(fn)) {
		JS_FreeValue(ctx, g);
		return fn;		/* caller reports; unchanged behaviour */
	}
	/* Store a DUP: the global keeps one reference, the caller gets the
	 * other and frees it exactly as it did when this was a fresh eval. */
	JS_DefinePropertyValueStr(ctx, g, key, JS_DupValue(ctx, fn), 0);
	JS_FreeValue(ctx, g);
	return fn;
}

static void qjs_el_install_js_helpers(JSContext *ctx, JSValue proto)
{
	static const char *src =
		"(function(){var P=this;"
		/* classList -- lazy PER-INSTANCE factory behind a shared accessor:
		 * the cl object captures the element (this at factory time), so it
		 * must exist per element -- but only when first touched, and the
		 * accessor/closure machinery is shared. */
		"(function(){"
		"var mk=function(){"
		"var el=this;"
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
		"return cl;};"
		"Object.defineProperty(P,'classList',{get:function(){"
		"if(!this.__cl)this.__cl=mk.call(this);return this.__cl;},"
		"configurable:true});"
		"Object.defineProperty(P,'className',{"
		"get:function(){return this.getAttribute('class')||'';},"
		"set:function(v){this.setAttribute('class',v);},"
		"configurable:true});"
		"})();"
		/* id property */
		"Object.defineProperty(P,'id',{"
		"get:function(){return this.getAttribute('id')||'';},"
		"set:function(v){this.setAttribute('id',v);},"
		"configurable:true});"
		/* value property */
		"Object.defineProperty(P,'value',{"
		"get:function(){return this.getAttribute('value')||'';},"
		"set:function(v){this.setAttribute('value',v);},"
		"configurable:true});"
		/* fixes866 (#292) - reflect the rest of the common content
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
		"Object.defineProperty(P,p,{"
		"get:function(){return this.getAttribute(p)||'';},"
		"set:function(v){this.setAttribute(p,String(v));},"
		"configurable:true});"
		"})(_rp[_i]);}"
		"})();"
		/* name property */
		"Object.defineProperty(P,'name',{"
		"get:function(){return this.getAttribute('name')||'';},"
		"set:function(v){this.setAttribute('name',v);},"
		"configurable:true});"
		/* type property */
		"Object.defineProperty(P,'type',{"
		"get:function(){return this.getAttribute('type')||'';},"
		"set:function(v){this.setAttribute('type',v);},"
		"configurable:true});"
		/* innerHTML= (fixes846, #167 S3) - real HTML fragment parse via
		 * __setInnerHTML (dom_hubbub_fragment_parser_create), builds
		 * actual child elements instead of stripping all markup to text.
		 * Read side (fixes1168, #262) is a real serializer via
		 * __getInnerHTML (qjs_el_get_inner_html_data), so .html() and
		 * read-modify-write patterns get markup back; textContent remains
		 * the fallback if a wrapper lacks the native helper. */
		"Object.defineProperty(P,'innerHTML',{"
		"get:function(){return (typeof this.__getInnerHTML==='function')"
			"?this.__getInnerHTML():(this.textContent||'');},"
		"set:function(v){"
		"if(typeof this.__setInnerHTML==='function')"
		"this.__setInnerHTML(String(v));"
		"else this.textContent=String(v).replace(/<[^>]*>/g,'');},"
		"configurable:true});"
		/* outerHTML - real markup now (fixes1168 #262): the native
		 * serializer emits the element itself with its attributes; the
		 * old tag-wrapping stub dropped them. */
		"Object.defineProperty(P,'outerHTML',{"
		"get:function(){return (typeof this.__getOuterHTML==='function')"
			"?this.__getOuterHTML():'<'+String(this.tagName).toLowerCase()+'>'"
			"+this.innerHTML+'</'+String(this.tagName).toLowerCase()+'>';},"
		"configurable:true});"
		/* dataset proxy -- lazy per-instance factory (see classList note) */
		"(function(){"
		"var mk=function(){"
		"var el=this;"
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
		"return p;};"
		"Object.defineProperty(P,'dataset',{get:function(){"
		"if(!this.__ds)this.__ds=mk.call(this);return this.__ds;},"
		"configurable:true});"
		"})();"
		/* style proxy -- lazy per-instance factory (see classList note) */
		"(function(){"
		"var mk=function(){"
		"var el=this;"
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
		"return sp;};"
		"Object.defineProperty(P,'style',{get:function(){"
		"if(!this.__st)this.__st=mk.call(this);return this.__st;},"
		"configurable:true});"
		"})();"
		/* matches - tag, #id, .class, [attr], compound */
		"P.matches=function(sel){"
		"if(!sel||!sel.trim)return false;"
		"sel=sel.trim();"
		"var parts=sel.split(',');"
		"var i;for(i=0;i<parts.length;i++){"
		"var s=parts[i].trim();"
		"var ok=true;"
		"var rest=s;"
		"var tagM=rest.match(/^([a-zA-Z][a-zA-Z0-9]*)/);"
		"if(tagM){if(this.tagName!==tagM[1].toUpperCase())ok=false;"
		"rest=rest.substr(tagM[1].length);}"
		"var re=/([#.:]|\\[)[^#.:\\[\\]]*(\\])?/g;"
		"var m;while(ok&&(m=re.exec(rest))){"
		"var t=m[0];"
		"if(t.charAt(0)==='#'){if(this.getAttribute('id')!==t.substr(1))ok=false;}"
		"else if(t.charAt(0)==='.'){var v=this.getAttribute('class')||'';"
		"if((' '+v+' ').indexOf(' '+t.substr(1)+' ')<0)ok=false;}"
		"else if(t.charAt(0)==='['){"
		"var inner=t.slice(1,-1);"
		"var eqI=inner.indexOf('=');"
		"if(eqI<0){if(!this.hasAttribute(inner.replace(/\\s/g,'')))ok=false;}"
		"else{"
		"var op=inner.charAt(eqI-1);"
		"var an,av,ev;"
		"if(op==='*'||op==='~'||op==='|'||op==='^'||op==='$'){"
		"an=inner.substr(0,eqI-1).trim();}else{an=inner.substr(0,eqI).trim();op='=';}"
		"av=inner.substr(eqI+1).trim().replace(/^['\"]|['\"]$/g,'');"
		"ev=this.getAttribute(an)||'';"
		"if(op==='='){if(ev!==av)ok=false;}"
		"else if(op==='*'){if(ev.indexOf(av)<0)ok=false;}"
		"else if(op==='~'){if((' '+ev+' ').indexOf(' '+av+' ')<0)ok=false;}"
		"else if(op==='^'){if(ev.indexOf(av)!==0)ok=false;}"
		"else if(op==='$'){if(ev.lastIndexOf(av)!==ev.length-av.length)ok=false;}"
		"}}}"
		"if(ok)return true;}"
		"return false;};"
		/* closest - walk parentNode chain */
		"P.closest=function(sel){"
		"var n=this;"
		"while(n&&n.matches){if(n.matches(sel))return n;n=n.parentNode;}"
		"return null;};"
		/* fixes1245 (#167) - canvas 2D context. Every drawing method is a
		 * safe, honest no-op (see the native measureText comment above
		 * for why real pixel compositing is out of scope); measureText
		 * is the one real, hardware-backed value. Installed on the
		 * shared element proto like matches/closest/dataset/style above
		 * rather than gated to canvas tags specifically -- technically
		 * spec puts getContext only on HTMLCanvasElement, but nothing
		 * else in this file gates its per-tag additions that way either,
		 * and no real page calls .getContext() on a non-canvas element. */
		"P.getContext=function(type){"
		"if(type!=='2d')return null;"
		"if(!this.__ctx2d)this.__ctx2d={};"
		"if(this.__ctx2d['2d'])return this.__ctx2d['2d'];"
		"var noop=function(){};"
		"var c={"
		"canvas:this,"
		"fillStyle:'#000000',strokeStyle:'#000000',lineWidth:1,"
		"lineCap:'butt',lineJoin:'miter',miterLimit:10,"
		"font:'10px sans-serif',textAlign:'start',textBaseline:'alphabetic',"
		"direction:'ltr',"
		"globalAlpha:1,globalCompositeOperation:'source-over',"
		"shadowBlur:0,shadowColor:'rgba(0,0,0,0)',"
		"shadowOffsetX:0,shadowOffsetY:0,"
		"filter:'none',imageSmoothingEnabled:true,"
		"lineDashOffset:0,"
		"save:noop,restore:noop,"
		"scale:noop,rotate:noop,translate:noop,transform:noop,"
		"setTransform:noop,resetTransform:noop,"
		"getTransform:function(){return{a:1,b:0,c:0,d:1,e:0,f:0,"
			"is2D:true,isIdentity:true};},"
		"beginPath:noop,closePath:noop,moveTo:noop,lineTo:noop,"
		"arc:noop,arcTo:noop,ellipse:noop,rect:noop,"
		"bezierCurveTo:noop,quadraticCurveTo:noop,"
		"fill:noop,stroke:noop,clip:noop,"
		"isPointInPath:function(){return false;},"
		"isPointInStroke:function(){return false;},"
		"fillRect:noop,strokeRect:noop,clearRect:noop,"
		"fillText:noop,strokeText:noop,drawImage:noop,"
		"getLineDash:function(){return [];},setLineDash:noop,"
		"createLinearGradient:function(){"
			"return{addColorStop:noop};},"
		"createRadialGradient:function(){"
			"return{addColorStop:noop};},"
		"createConicGradient:function(){"
			"return{addColorStop:noop};},"
		"createPattern:function(){return null;},"
		/* real-shaped, correctly-sized, honestly-zeroed pixel buffers --
		 * a script that reads/writes ImageData programmatically (rather
		 * than expecting it to visually paint) still gets correct
		 * behaviour. */
		"createImageData:function(w,h){"
			"if(w&&typeof w==='object'){h=w.height;w=w.width;}"
			"w=Math.max(0,w|0);h=Math.max(0,h|0);"
			"return{width:w,height:h,"
				"data:new Uint8ClampedArray(w*h*4)};},"
		"getImageData:function(x,y,w,h){"
			"w=Math.max(0,w|0);h=Math.max(0,h|0);"
			"return{width:w,height:h,"
				"data:new Uint8ClampedArray(w*h*4)};},"
		"putImageData:noop,"
		/* the one real value: hardware-measured text width via the same
		 * layout engine html_reformat itself uses. */
		"measureText:function(s){"
			"var w=0;"
			"try{if(typeof __canvasMeasureText==='function')"
				"w=__canvasMeasureText(String(s),this.font);}"
			"catch(e){}"
			"return{width:w,"
				"actualBoundingBoxLeft:0,actualBoundingBoxRight:w,"
				"actualBoundingBoxAscent:0,actualBoundingBoxDescent:0,"
				"fontBoundingBoxAscent:0,fontBoundingBoxDescent:0,"
				"emHeightAscent:0,emHeightDescent:0,"
				"hangingBaseline:0,alphabeticBaseline:0,"
				"ideographicBaseline:0};},"
		"getContextAttributes:function(){return{};}"
		"};"
		"this.__ctx2d['2d']=c;return c;"
		"};"
		/* fixes1245 - toDataURL/toBlob must never throw or return
		 * undefined (real callers assume a string / async callback
		 * unconditionally). No real pixels exist to encode, so both
		 * honestly represent "blank": a well-formed, valid 1x1
		 * transparent PNG data URI (a real, standard placeholder image
		 * used across the web -- not a fabricated format) rather than
		 * inventing pixel content that was never drawn. */
		"P.toDataURL=function(){"
		"return 'data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAAB"
			"CAQAAAC1HAwCAAAAC0lEQVR42mNkYAAAAAYAAjCB0C8AAAAASUVORK5CYII=';"
		"};"
		"P.toBlob=function(cb,mimeType){"
		"if(typeof cb!=='function')return;"
		"try{setTimeout(function(){"
			"var t=(mimeType&&/^image\\//.test(mimeType))?mimeType:"
				"'image/png';"
			"cb(new Blob([],{type:t}));"
		"},0);}catch(e){try{cb(null);}catch(_){}}"
		"};"
		/* event handling */
		/* fixes989 - addEventListener / removeEventListener are NATIVE
		 * now (qjs_el_add_event_listener_data). They are installed on the
		 * class proto BEFORE this string is evaluated, so a JS definition
		 * here would silently shadow them and the libdom registration
		 * would never happen. dispatchEvent stays JS-free too -- the
		 * native listener callback calls it, so the _L/_H firing logic has
		 * exactly one implementation. */
		/* fixes872 (#300) - fire the addEventListener list (_L) AND the on*
		 * handler (_H, set through the prototype accessors) exactly once each.
		 * Both routes are real and pages use both; dispatchEvent firing only _L
		 * is why js_fire_script_load had to call el['on'+type] separately. */
		/* fixes1008 (1e) - this is now __msFireLocal, the LOCAL firing of
		 * one node's own listeners. `dispatchEvent` itself is a native that
		 * routes through libdom so synthetic events bubble to ancestors and
		 * the document, and returns false when cancelled; it calls back here
		 * (via the re-entrancy guard) to do the actual firing, so there is
		 * still exactly ONE place that walks _L and _H.
		 *
		 * fixes1008 also stops swallowing handler errors silently: a throwing
		 * handler must not abort the others, but it must be VISIBLE, or
		 * "site loads and does nothing" has no log line to explain it. */
		/* fixes1040 (#264) - PHASE FILTER. libdom walks capture (ancestors)
		 * -> AT_TARGET -> bubble (ancestors) and calls back here once per
		 * node it visits. Firing the node's whole _L list on every callback
		 * threw away the phase: a node carrying both a capture and a
		 * non-capture listener ran BOTH during the capture pass, so a
		 * capture+bubble ancestor beat the target itself and hardware saw
		 * [cap,bubble,target]. Fire only the listeners whose capture flag
		 * (el._LC, index-aligned with el._L) matches the current phase.
		 *
		 * eventPhase: 1=CAPTURING 2=AT_TARGET 3=BUBBLING. 0/absent means a
		 * LOCAL-ONLY fire -- the re-entrancy fallback in
		 * qjs_el_dispatch_event_data, js_fire_script_load, and the native
		 * UI fan-out all reach us that way -- and there the pre-existing
		 * fire-everything behaviour is the correct one. AT_TARGET runs both
		 * kinds in registration order, matching event_target.c:285-286. */
		"P.__msFireLocal=function(ev){"
		"var t=ev&&ev.type||'';"
		"var ph=(ev&&ev.eventPhase)||0;"
		"if(this._L&&this._L[t]){"
		"var a=this._L[t].slice();"
		"var c=(this._LC&&this._LC[t])?this._LC[t].slice():null;"
		"var i;for(i=0;i<a.length;i++){"
		/* stopImmediatePropagation, observed BETWEEN handlers -- see
		 * qjs_ev_stop_immediate_data. Checked before each call so the
		 * handler that set it still completes. */
		"if(ev&&ev.__msStopNow)return true;"
		"if(ph===1||ph===3){"
		"var cap=c?!!c[i]:false;"
		"if(ph===1?!cap:cap)continue;"
		"}"
		"try{a[i].call(this,ev);}"
		"catch(e){try{console.error('LIFE jsevent listener threw ['+t+']: '+"
		"((e&&e.message)||e));}catch(_){}}}}"
		"if(ev&&ev.__msStopNow)return true;"
		/* on* handlers are non-capture by definition, so they must never
		 * run in the capturing phase. */
		"if(ph!==1&&this._H&&this._H[t]){try{this._H[t].call(this,ev);}"
		"catch(e){try{console.error('LIFE jsevent on'+t+' threw: '+"
		"((e&&e.message)||e));}catch(_){}}}"
		"return true;};"
		/* fixes1008 (2b) - THE MISSING DOM SURFACE.
		 *
		 * Every one of these was simply absent, and each is common enough
		 * that its absence throws rather than degrades: a script calling
		 * el.remove() or el.replaceChild() gets "not a function" and dies on
		 * the spot, exactly like document.write did (fixes1007).
		 *
		 * All are built on the native primitives that already exist
		 * (appendChild / removeChild / insertBefore / __setInnerHTML /
		 * parentNode / children), so there is no second mutation path that
		 * can drift from the real one, and each still routes through
		 * qjs_dom_mut_check and the reconvert dirty-mark. */
		"P.remove=function(){"
			"var p=this.parentNode;if(p&&p.removeChild)p.removeChild(this);};"
		"P.replaceChild=function(nw,old){"
			"if(!nw||!old)return old;"
			"this.insertBefore(nw,old);this.removeChild(old);return old;};"
		"P.append=function(){var i;for(i=0;i<arguments.length;i++){"
			"var a=arguments[i];"
			"this.appendChild(typeof a==='string'?"
				"document.createTextNode(a):a);}};"
		"P.prepend=function(){var i,f=this.firstChild;"
			"for(i=0;i<arguments.length;i++){var a=arguments[i];"
			"this.insertBefore(typeof a==='string'?"
				"document.createTextNode(a):a,f);}};"
		"P.before=function(){var p=this.parentNode;if(!p)return;var i;"
			"for(i=0;i<arguments.length;i++){var a=arguments[i];"
			"p.insertBefore(typeof a==='string'?"
				"document.createTextNode(a):a,this);}};"
		"P.after=function(){var p=this.parentNode;if(!p)return;"
			"var r=this.nextSibling,i;"
			"for(i=0;i<arguments.length;i++){var a=arguments[i];"
			"var n=(typeof a==='string')?document.createTextNode(a):a;"
			"if(r)p.insertBefore(n,r);else p.appendChild(n);}};"
		"P.replaceWith=function(){"
			"this.before.apply(this,arguments);this.remove();};"
		/* insertAdjacent* -- the four spec positions, on the real fragment
		 * parser (so a written <script> is a real script element, same as
		 * document.write). */
		"P.insertAdjacentElement=function(pos,n){"
			"if(!n)return null;pos=String(pos).toLowerCase();"
			"if(pos==='beforebegin')this.before(n);"
			"else if(pos==='afterbegin')this.prepend(n);"
			"else if(pos==='beforeend')this.appendChild(n);"
			"else if(pos==='afterend')this.after(n);"
			"return n;};"
		"P.insertAdjacentHTML=function(pos,html){"
			"var h=document.createElement('div');"
			"try{h.innerHTML=String(html);}catch(e){return;}"
			"var kids=[],c=h.firstChild;"
			"while(c){kids.push(c);c=c.nextSibling;}"
			"var i;"
			"if(String(pos).toLowerCase()==='afterbegin'||"
			   "String(pos).toLowerCase()==='beforebegin'){"
				"for(i=kids.length-1;i>=0;i--)"
					"this.insertAdjacentElement(pos,kids[i]);"
			"}else{"
				"for(i=0;i<kids.length;i++)"
					"this.insertAdjacentElement(pos,kids[i]);"
			"}};"
		"P.insertAdjacentText=function(pos,t){"
			"this.insertAdjacentElement(pos,document.createTextNode(String(t)));};"
		/* attributes / getAttributeNames: reconstructed from the reflected
		 * set plus data-*. Not a live NamedNodeMap -- callers iterate it,
		 * which a static array serves. */
		"P.getAttributeNames=function(){"
			"var out=[],i,ks=['id','class','style','src','href','type',"
				"'name','rel','target','alt','title','placeholder',"
				"'action','method','width','height','media','value',"
				"'disabled','checked','readonly','required'];"
			"for(i=0;i<ks.length;i++)"
				"if(this.hasAttribute&&this.hasAttribute(ks[i]))out.push(ks[i]);"
			"return out;};"
		"Object.defineProperty(P,'attributes',{configurable:true,"
			"get:function(){var ns=this.getAttributeNames(),out=[],i;"
			"for(i=0;i<ns.length;i++)out.push({name:ns[i],"
				"value:this.getAttribute(ns[i])});"
			"out.getNamedItem=function(n){var j;"
				"for(j=0;j<out.length;j++)if(out[j].name===n)return out[j];"
				"return null;};"
			"return out;}});"
		"P.compareDocumentPosition=function(o){"
			"if(!o||o===this)return 0;"
			"if(this.contains&&this.contains(o))return 20;"
			"if(o.contains&&o.contains(this))return 10;"
			"return 4;};"
		"P.isEqualNode=function(o){return o===this;};"
		"P.isSameNode=function(o){return o===this;};"
		/* fixes1009 - ELEMENT-SCOPED getElementsBy*.
		 *
		 * These existed on `document` (fixes873) but NOT on elements, and
		 * hardware named it: with fixes1008 in, the only two JS exceptions
		 * left on hackaday.com were both
		 *   TypeError: not a function   at ...:17:41
		 * which is `container.getElementsByTagName('ul')[0]` in the WordPress
		 * navigation script. The bundle also calls .getElementsByClassName
		 * three times.
		 *
		 * Same delegation document uses: querySelectorAll is element-scoped
		 * and already native (fixes871), and a bare tag or .class IS a
		 * compound selector -- so this is the same query, and there is one
		 * matcher rather than a second subtly-different walker. Returns a
		 * static array, not a live HTMLCollection; every caller here
		 * indexes or iterates, which an array serves. */
		"P.getElementsByTagName=function(t){"
			"return this.querySelectorAll(String(t));};"
		"P.getElementsByClassName=function(c){"
			"return this.querySelectorAll('.'+String(c).split(/\\s+/)"
				".filter(function(x){return !!x;}).join('.'));};"
		"P.getElementsByName=function(n){"
			"return this.querySelectorAll('[name=\"'+String(n)+'\"]');};"
		/* Cheap neighbours, same round: each is one line and each throws
		 * rather than degrading when absent. */
		"P.toggleAttribute=function(n,f){"
			"var has=!!(this.hasAttribute&&this.hasAttribute(n));"
			"var want=(f===undefined)?!has:!!f;"
			"if(want)this.setAttribute(n,'');else this.removeAttribute(n);"
			"return want;};"
		"P.normalize=function(){};"
		/* Form-control state. `checked` and `selected` are properties in the
		 * DOM but attributes here, which is the honest approximation until
		 * they are wired to struct form_control; `disabled` reflects. */
		"(function(){var bp=['checked','disabled','readOnly','required',"
			"'selected','multiple','autofocus'];var i;"
			"for(i=0;i<bp.length;i++)(function(p){"
			"var a=p.toLowerCase();"
			"Object.defineProperty(P,p,{configurable:true,"
			"get:function(){return !!(this.hasAttribute&&this.hasAttribute(a));},"
			"set:function(v){if(v)this.setAttribute(a,a);"
				"else if(this.removeAttribute)this.removeAttribute(a);}});"
			"})(bp[i]);})();"
		/* misc -- the zero stubs. The REAL metrics (getBoundingClientRect
		 * + the 8 offset/client/scroll getters) are installed on the proto
		 * AFTER this block by qjs_el_install_proto_surface, so they win --
		 * same ordering as the old per-element install. */
		"P.getBoundingClientRect=function(){"
		"return{top:0,left:0,right:0,bottom:0,width:0,height:0,x:0,y:0};};"
		"P.getClientRects=function(){return[this.getBoundingClientRect()];};"
		"P.scrollIntoView=function(){};"
		"P.scrollIntoViewIfNeeded=function(){};"
		"P.focus=function(){};"
		"P.blur=function(){};"
		/* fixes997 - el.click() synthesises a real dispatch instead of
		 * being a no-op. Frameworks use it to trigger a control
		 * programmatically (and the hidden-file-input pattern depends
		 * on it entirely). Routed through this element's own
		 * dispatchEvent so _L and _H both fire exactly as a real click
		 * does -- one firing implementation, not a second one that can
		 * drift. Deliberately does NOT perform the default action: a
		 * synthetic click on a link navigating would be a surprising
		 * side effect to introduce here, and no page tested needs it. */
		"P.click=function(){"
		"if(this.dispatchEvent)this.dispatchEvent({type:'click',"
		"target:this,currentTarget:this,"
		"preventDefault:function(){},stopPropagation:function(){}});};"
		/* fixes878 - the node-oriented traversal surface used to be hardcoded
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
		 * cloneNode/contains/firstChild/lastChild/nextSibling/previousSibling/
		 * childNodes/hasChildNodes are NO LONGER defined here. They moved to
		 * Node.prototype in qjs_install_node_traversal, which every wrapper
		 * shape reaches through the fixes1127 family chain -- one install,
		 * zero per-element closures (fixes1170, #211). */
		"})";
	JSValue fn;

	fn = qjs_helper_fn(ctx, "__ms_h_el", src, "<el-helpers>");
	if (!JS_IsException(fn)) {
		/* `this` = the wrapper-class proto p: everything above lands on p
		 * and is found by every wrapper through the per-tag constructor
		 * chain (HTMLDivElement.prototype -> p -> HTMLElement.prototype ->
		 * Element.prototype -> Node.prototype). At call time `this` is the
		 * wrapper. */
		JS_Call(ctx, fn, proto, 0, NULL);
	} else {
		JSValue ex = JS_GetException(ctx);
		const char *msg = JS_ToCString(ctx, ex);
		macsurf_debug_log_writef("qjs el-helpers eval error: %s", msg ? msg : "?");
		if (msg) JS_FreeCString(ctx, msg);
		JS_FreeValue(ctx, ex);
	}
	JS_FreeValue(ctx, fn);
}

/* ---- fixes878/#211: the node-level traversal surface, for EVERY node wrapper ----
 *
 * Installed ONCE per realm on Node.prototype -- anchored via the DOM
 * constructor family register_browser_globals installs (Node is always one
 * of them) -- never per wrapper. Elements, text/comment nodes AND fragments
 * all reach it through the fixes1127 family chain:
 *
 *   element   : per-tag ctor proto -> class proto p -> HTMLElement.prototype
 *               -> Element.prototype -> Node.prototype
 *   text/comment/CDATA: Text/Comment/CharacterData.prototype
 *               -> CharacterData.prototype -> Node.prototype
 *   fragment  : DocumentFragment.prototype -> Node.prototype
 *
 * That breadth is the point: `box.firstChild.nextSibling` walks THROUGH a
 * text node, so if only elements carry the surface the chain dies at the
 * first gap between tags -- which is most real markup, and was the first
 * thing Test 21 caught.
 *
 * SAFE ON ALL THREE SHAPES because every function here goes through the base
 * dom_node vtable (get_first_child / get_next_sibling / clone_node /
 * contains), which element, text and fragment all implement. This is the
 * same rule fixes846 arrived at the hard way: qjs_wrap_element reads through
 * the ELEMENT vtable (dom_element_get_tag_name), and reusing it for a
 * fragment -- a different, smaller shape -- was an ASan
 * global-buffer-overflow. Nothing in this function may touch an
 * element-only operation.
 *
 * #211: the C functions read the node from `this` (qjs_get_node(this_val));
 * there are no per-instance func_data closures, so this install is a fixed
 * per-realm cost instead of ~8 closures per wrapper. */
static void qjs_install_node_traversal(JSContext *ctx, JSValue node_proto)
{
	JSValue f;
	static const char *nav_src =
		"(function(){var P=this;"
		/* fixes1010 - NODE-LEVEL UNIVERSALS, on EVERY wrapper shape.
		 *
		 * getRootNode / ownerDocument / isConnected were added to
		 * qjs_el_install_js_helpers in fixes1009, which runs for ELEMENTS
		 * ONLY -- text, CDATA, comment and fragment wrappers get this
		 * function instead. That asymmetry immediately bit, and in the
		 * nastiest way: adding a method CHANGED WHICH CODE PATH jQuery
		 * TAKES.
		 *
		 * jQuery feature-detects `J.getRootNode` on a probe element and, if
		 * present, swaps its isAttached implementation for
		 *     ce.contains(e.ownerDocument,e) || e.getRootNode(Z)===e.ownerDocument
		 * Before fixes1009 the detect was false and the simple branch ran.
		 * After it, the detect passed on an element and jQuery then called
		 * e.getRootNode() on nodes that had no such method -- a TypeError
		 * that did not exist before I added the method. Hardware caught it as
		 * the last remaining exception on hackaday.
		 *
		 * The lesson is general enough to state: a feature-detect makes a
		 * PARTIAL implementation worse than none. Anything a library probes
		 * for must exist on every shape it can then be called on, so these
		 * live here -- the one surface element, text and fragment all get.
		 *
		 * #211: on Node.prototype now, so EVERY shape gets them with one
		 * install -- the asymmetry is closed by construction. */
		"P.getRootNode=function(){var n=this;"
		"while(n&&n.parentNode)n=n.parentNode;return n||this;};"
		"Object.defineProperty(P,'ownerDocument',{configurable:true,"
		"get:function(){return typeof document!=='undefined'?document:null;}});"
		"Object.defineProperty(P,'isConnected',{configurable:true,"
		"get:function(){var n=this;while(n&&n.parentNode)n=n.parentNode;"
		"return !!(n&&(n===document||n===document.documentElement||"
		"n.nodeType===9));}});"
		"Object.defineProperty(P,'parentNode',{"
		"get:function(){return this.__getParentNode();},configurable:true});"
		"Object.defineProperty(P,'firstChild',{"
		"get:function(){return this.__getFirstChild();},configurable:true});"
		"Object.defineProperty(P,'lastChild',{"
		"get:function(){return this.__getLastChild();},configurable:true});"
		"Object.defineProperty(P,'nextSibling',{"
		"get:function(){return this.__getNextSibling();},configurable:true});"
		"Object.defineProperty(P,'previousSibling',{"
		"get:function(){return this.__getPreviousSibling();},configurable:true});"
		/* snapshot array, not a live NodeList -- see qjs_el_get_child_nodes_data */
		"Object.defineProperty(P,'childNodes',{"
		"get:function(){return this.__getChildNodes();},configurable:true});"
		"P.hasChildNodes=function(){return this.__getFirstChild()!==null;};"
		"})";
	JSValue fn;

	f = JS_NewCFunctionData(ctx, qjs_el_get_edge_data, 0, 0 /*firstChild*/, 0, NULL);
	JS_SetPropertyStr(ctx, node_proto, "__getFirstChild", f);
	f = JS_NewCFunctionData(ctx, qjs_el_get_edge_data, 0, 1 /*lastChild*/, 0, NULL);
	JS_SetPropertyStr(ctx, node_proto, "__getLastChild", f);
	f = JS_NewCFunctionData(ctx, qjs_el_get_edge_data, 0, 2 /*nextSibling*/, 0, NULL);
	JS_SetPropertyStr(ctx, node_proto, "__getNextSibling", f);
	f = JS_NewCFunctionData(ctx, qjs_el_get_edge_data, 0, 3 /*prevSibling*/, 0, NULL);
	JS_SetPropertyStr(ctx, node_proto, "__getPreviousSibling", f);
	f = JS_NewCFunctionData(ctx, qjs_el_get_child_nodes_data, 0, 0, 0, NULL);
	JS_SetPropertyStr(ctx, node_proto, "__getChildNodes", f);
	f = JS_NewCFunctionData(ctx, qjs_el_get_parent_node_data, 0, 0, 0, NULL);
	JS_SetPropertyStr(ctx, node_proto, "__getParentNode", f);
	f = JS_NewCFunctionData(ctx, qjs_el_clone_node_data, 1, 0, 0, NULL);
	JS_SetPropertyStr(ctx, node_proto, "cloneNode", f);
	f = JS_NewCFunctionData(ctx, qjs_el_contains_data, 1, 0, 0, NULL);
	JS_SetPropertyStr(ctx, node_proto, "contains", f);

	fn = qjs_helper_fn(ctx, "__ms_h_nav", nav_src, "<node-nav>");
	if (!JS_IsException(fn)) {
		/* `this` = Node.prototype: everything above lands once per realm
		 * and is found by every wrapper shape through the family chain. */
		JS_Call(ctx, fn, node_proto, 0, NULL);
	}
	JS_FreeValue(ctx, fn);
}

/* Install all native C functions and JS helpers on an element object */
/* ==================================================================== */
/* fixes989 (#264/#300) - the event bridge                              */
/* ==================================================================== */
/*
 * Before this, a real mouse click reached NOTHING. interaction.c did a real
 * libdom dispatch via fire_generic_dom_event, but nothing in the tree ever
 * called dom_event_target_add_event_listener, so it dispatched into an empty
 * listener set; and the only other route, macsurf_qjs_dispatch_dom_click, was
 * a stub returning 0. JS kept its own registries (_L for addEventListener, _H
 * for on* properties) that libdom knew nothing about. That is why pages
 * rendered correctly and ignored every click.
 *
 * THE DESIGN, and its one important property: libdom never holds a JSValue.
 *
 * addEventListener still fills the SAME _L registry as before -- JS owns the
 * callbacks -- and additionally registers a single shared marker listener with
 * libdom for (node, type, capture). That listener carries no JS state at all.
 * When libdom dispatches, the callback reads the event's currentTarget,
 * resolves that node to its ONE stable wrapper through the wrapper cache
 * (qjs_wrap_lookup -- this is what makes the whole approach possible: `_L` set
 * through any reference to a node is visible through every other), and calls
 * that wrapper's existing dispatchEvent to fire _L and _H for this node.
 * libdom therefore owns PROPAGATION; JS owns the callbacks; neither owns the
 * other's memory.
 *
 * What that buys: when the JS realm is rebuilt (navigation, js_newthread) the
 * registries vanish with it and the marker listeners simply resolve to no
 * wrapper and no-op. There is no teardown ordering to get right, nothing to
 * unregister before freeing a runtime, and no second owner of a callback --
 * which is the whole hazard class that #283/#304 came from.
 *
 * ONE listener object serves every registration: libdom keys its own storage
 * by (type, listener, capture) per node, so registering the same listener on
 * many nodes and types is correct, and removal matches on the same pointer.
 */

static dom_event_listener *g_qjs_dom_listener = NULL;

/* The callback needs a JSContext and only has the wrapper entry's runtime.
 * Resolve it through the live-heap list rather than g_heap: with an iframe
 * there are several runtimes and g_heap is merely the newest. */
static JSContext *qjs_ctx_for_runtime(JSRuntime *rt)
{
	struct jsheap *h;
	if (rt == NULL) return NULL;
	for (h = g_heap_list; h != NULL; h = h->next) {
		if (h->rt == rt) return h->ctx;
	}
	return NULL;
}

/* preventDefault / stopPropagation are wired straight to the REAL dom_event,
 * so fire_generic_dom_event's existing return value carries the answer and
 * interaction.c needs no separate channel. The event pointer travels as an
 * integer in func_data because the event outlives neither the dispatch nor
 * this closure -- both are torn down when the dispatch returns. */
static JSValue qjs_ev_prevent_default_data(JSContext *ctx,
		JSValueConst this_val, int argc, JSValueConst *argv,
		int magic, JSValueConst *func_data)
{
	int64_t p = 0;
	(void)this_val; (void)argc; (void)argv; (void)magic;
	if (JS_ToInt64(ctx, &p, func_data[0]) == 0 && p != 0) {
		dom_event_prevent_default((dom_event *)(size_t)p);
	}
	return JS_UNDEFINED;
}

static JSValue qjs_ev_stop_propagation_data(JSContext *ctx,
		JSValueConst this_val, int argc, JSValueConst *argv,
		int magic, JSValueConst *func_data)
{
	int64_t p = 0;
	(void)this_val; (void)argc; (void)argv; (void)magic;
	if (JS_ToInt64(ctx, &p, func_data[0]) == 0 && p != 0) {
		dom_event_stop_propagation((dom_event *)(size_t)p);
	}
	return JS_UNDEFINED;
}

/* fixes1008 (1d) - the REAL stopImmediatePropagation.
 *
 * It was aliased to qjs_ev_stop_propagation_data, which stops the walk moving
 * to the next NODE but lets the remaining listeners on the CURRENT node run.
 * That is the difference the method exists to express, and libdom implements
 * it properly (evt->stop_now breaks the listener loop in
 * _dom_event_target_dispatch). Code that calls this is deliberately trying to
 * suppress its siblings -- a validation handler cancelling the rest of a
 * chain -- so silently running them anyway is a wrong answer, not a missing
 * feature. */
static JSValue qjs_ev_stop_immediate_data(JSContext *ctx,
		JSValueConst this_val, int argc, JSValueConst *argv,
		int magic, JSValueConst *func_data)
{
	int64_t p = 0;
	(void)argc; (void)argv; (void)magic;
	if (JS_ToInt64(ctx, &p, func_data[0]) == 0 && p != 0) {
		dom_event_stop_immediate_propagation((dom_event *)(size_t)p);
	}
	/* libdom's stop_now breaks ITS listener loop -- but all of a node's JS
	 * handlers live inside ONE libdom entry (the shared g_qjs_dom_listener),
	 * so __msFireLocal is already running and would happily finish walking
	 * el._L. The flag is what lets it break between handlers, which is the
	 * whole observable difference from stopPropagation. */
	if (JS_IsObject(this_val)) {
		JS_SetPropertyStr(ctx, (JSValue)this_val, "__msStopNow",
				JS_NewBool(ctx, 1));
	}
	return JS_UNDEFINED;
}

/* preventDefault must also be OBSERVABLE: `e.defaultPrevented` is read by
 * plenty of code that wants to know whether an earlier handler already
 * cancelled. Sets the flag on the event object as well as on the dom_event. */
static JSValue qjs_ev_prevent_default2_data(JSContext *ctx,
		JSValueConst this_val, int argc, JSValueConst *argv,
		int magic, JSValueConst *func_data)
{
	int64_t p = 0;
	(void)argc; (void)argv; (void)magic;
	if (JS_ToInt64(ctx, &p, func_data[0]) == 0 && p != 0) {
		dom_event_prevent_default((dom_event *)(size_t)p);
	}
	if (JS_IsObject(this_val)) {
		JS_SetPropertyStr(ctx, (JSValue)this_val, "defaultPrevented",
				JS_NewBool(ctx, 1));
	}
	return JS_UNDEFINED;
}

/* fixes1008 (1d) - the UI detail for the event currently being dispatched.
 *
 * interaction.c fills these immediately before fire_generic_dom_event() and
 * clears them after, so a handler reading e.clientX gets the real pointer
 * position rather than undefined (which it computes NaN from). Globals rather
 * than a widened signature: fire_generic_dom_event is core NetSurf API called
 * from several places, and widening an exported signature is the CW8 flat-
 * namespace trap (reference_cw8_no_signature_widening).
 *
 * ZEROED between dispatches by macsurf_qjs_clear_event_detail(), so a stale
 * click's coordinates can never leak into an unrelated later event. */
static int g_qjs_ev_x = 0;
static int g_qjs_ev_y = 0;
static int g_qjs_ev_button = 0;
static int g_qjs_ev_key = 0;
static int g_qjs_ev_mods = 0;

void macsurf_qjs_set_event_detail(int x, int y, int button, int key, int mods);
void macsurf_qjs_set_event_detail(int x, int y, int button, int key, int mods)
{
	g_qjs_ev_x = x;
	g_qjs_ev_y = y;
	g_qjs_ev_button = button;
	g_qjs_ev_key = key;
	g_qjs_ev_mods = mods;
}

void macsurf_qjs_clear_event_detail(void);
/* fixes1013 - SCROLL AND RESIZE, dispatched at last.
 *
 * These were the one part of the 1f fan-out with no libdom path: they do not
 * originate in interaction.c at all, they come from the frontend's own scroll
 * and window-resize handling. Leaving them out had a consequence far past
 * "some handlers do not fire":
 *
 * fixes1011 made getBoundingClientRect return REAL geometry. Before that it
 * returned zeros, so every site's own lazy-load test -- rect.top <
 * innerHeight -- was true for EVERY image and the whole page loaded eagerly,
 * by accident. With real geometry those tests correctly answer "below the
 * fold", and the images then wait for a scroll event that never came. So
 * making layout correct BROKE lazy images, and this is the missing half.
 *
 * Dispatched at the document (scroll targets the scrolling element; both
 * bubble to window through the 1b fan-out). Gated like the rest, so a page
 * with no scroll listener pays nothing on a machine where scrolling is
 * already the most performance-sensitive thing there is. */
/* fixes1020 - THROTTLE. This fired a full libdom dispatch (Event object +
 * every jQuery scroll handler on the page) per SCROLL NOTCH, which is what
 * made scrolling crawl on real pages the moment fixes1013 wired it. 4 Hz
 * leading edge plus one TRAILING fire, so handlers that check the final
 * position ("has it scrolled into view") still see it after the throttle
 * window closes. A page with no scroll listener still pays nothing. */
static double g_scroll_last_us = 0.0;
static int g_scroll_trailing = 0;

void macsurf_qjs_fire_scroll(void);

static void qjs_scroll_trailing_cb(void *p)
{
	(void)p;
	g_scroll_trailing = 0;
	g_scroll_last_us = 0.0;   /* let the trailing fire through */
	macsurf_qjs_fire_scroll();
}

void macsurf_qjs_fire_scroll(void)
{
	extern double macos9_micros(void);
	double now;
	if (!MACSURF_JS_VIEW_EVENTS) return;   /* fixes1022 quiesce */
	if (g_qjs_document == NULL) return;
	if (!macsurf_qjs_event_type_live("scroll")) return;
	now = macos9_micros();
	if (now - g_scroll_last_us < 250000.0) {
		if (!g_scroll_trailing) {
			g_scroll_trailing = 1;
			(void) macos9_schedule(300, qjs_scroll_trailing_cb,
					NULL);
		}
		return;
	}
	g_scroll_last_us = now;
	(void) fire_generic_dom_event(corestring_dom_scroll,
			(dom_node *) g_qjs_document, true, false);
}

void macsurf_qjs_fire_resize(void);
void macsurf_qjs_fire_resize(void)
{
	if (!MACSURF_JS_VIEW_EVENTS) return;   /* fixes1022 quiesce */
	if (g_qjs_document == NULL) return;
	if (!macsurf_qjs_event_type_live("resize")) return;
	(void) fire_generic_dom_event(corestring_dom_resize,
			(dom_node *) g_qjs_document, true, false);
}

void macsurf_qjs_clear_event_detail(void)
{
	g_qjs_ev_x = g_qjs_ev_y = g_qjs_ev_button = 0;
	g_qjs_ev_key = g_qjs_ev_mods = 0;
}

/* ===================================================================
 * fixes1011 (Phase 3) - LAYOUT, VISIBLE TO JAVASCRIPT.
 *
 * getComputedStyle returned only inline `style` values, getBoundingClientRect
 * returned all zeros, and offsetWidth/clientHeight/scrollTop existed only on
 * the MOCK fallback elements -- never on real wrappers. Any script that
 * MEASURES therefore computed garbage from zeros: sticky headers, dropdown and
 * tooltip positioning, lightboxes, carousels, "is this in view" checks, and
 * every responsive-JS branch.
 *
 * This is the failure mode that does NOT throw. A missing method dies loudly
 * and gets found in one round (see fixes1007's document.write, fixes1009's
 * getElementsByTagName). Zeros propagate silently into arithmetic and the page
 * merely looks wrong, which is why this survived long after the throwing bugs
 * were gone.
 *
 * All of it reads the REAL box tree via box_for_node + box_coords and the REAL
 * cascade via css_computed_*, both of which already existed -- nothing new is
 * computed, it was simply never exposed.
 *
 * STALENESS POLICY, and it is a deliberate choice: a box can be absent (not
 * yet laid out, display:none, or mid-reconvert). We return the LAST KNOWN
 * geometry, i.e. zeros only when there has never been a box. We do NOT force a
 * synchronous layout pass here. Forcing one inside a JS handler would reflow
 * under native callers that hold `struct box *` across the dispatch --
 * html_mouse_action alone holds six -- and that is the Class 1/Class 3 crash
 * shape arriving by a new route. Measure-after-mutate therefore reads
 * pre-mutation geometry for one frame, which is wrong in the same direction a
 * real browser is wrong when you read before a reflow, and is survivable.
 * Forcing layout is its own round with its own guard flag.
 * =================================================================== */

/* Viewport scroll + size, from the front window's gui_window.
 *
 * Guarded on __MACOS9__ because macos9_window_list_head() is Carbon-side; the
 * Linux harness has no window, and 0/viewport-default is the right answer
 * there (its box tree is laid out at a fixed 800x600). */
static int macsurf_qjs_scroll_x(void)
{
#ifdef __MACOS9__
	struct gui_window *gw = macos9_window_list_head();
	if (gw != NULL) return gw->scroll_x;
#endif
	return 0;
}

static int macsurf_qjs_scroll_y(void)
{
#ifdef __MACOS9__
	struct gui_window *gw = macos9_window_list_head();
	if (gw != NULL) return gw->scroll_y;
#endif
	return 0;
}

static int macsurf_qjs_viewport_w(void)
{
#ifdef __MACOS9__
	struct gui_window *gw = macos9_window_list_head();
	if (gw != NULL) {
		int w = gw->content_rect.right - gw->content_rect.left;
		if (w > 0) return w;
	}
#endif
	return 949;   /* matches the innerWidth the shim has always reported */
}

static int macsurf_qjs_viewport_h(void)
{
#ifdef __MACOS9__
	struct gui_window *gw = macos9_window_list_head();
	if (gw != NULL) {
		int h = gw->content_rect.bottom - gw->content_rect.top;
		if (h > 0) return h;
	}
#endif
	return 613;
}

/* CSS display keyword. Only the values a script is likely to test are named;
 * anything else answers "block", which is what an unknown block-level box
 * behaves as. `none` is the one that must never be wrong -- see
 * qjs_get_computed_style. */
static const char *qjs_css_display_name(uint8_t v)
{
	switch (v) {
	case CSS_DISPLAY_NONE:         return "none";
	case CSS_DISPLAY_INLINE:       return "inline";
	case CSS_DISPLAY_INLINE_BLOCK: return "inline-block";
	case CSS_DISPLAY_FLEX:         return "flex";
	case CSS_DISPLAY_INLINE_FLEX:  return "inline-flex";
	case CSS_DISPLAY_GRID:         return "grid";
	case CSS_DISPLAY_INLINE_GRID:  return "inline-grid";
	case CSS_DISPLAY_TABLE:        return "table";
	case CSS_DISPLAY_TABLE_CELL:   return "table-cell";
	case CSS_DISPLAY_TABLE_ROW:    return "table-row";
	case CSS_DISPLAY_LIST_ITEM:    return "list-item";
	default:                       return "block";
	}
}

/* fixes1014 - is the box tree in a state where geometry answers are TRUE?
 *
 * The DONE gate (fixes1012) is crash-correct but it created a WINDOW OF LIES:
 * DOMContentLoaded fires from html_box_convert_done BEFORE content_set_ready,
 * so every script that initialises at ready -- jQuery $(function(){}), every
 * slider, every widget that sizes itself -- measured during a period where the
 * gate forced every read to 0. Zero is not a harmless degradation: scripts
 * WRITE their measurements back as inline styles, and width:0 written once at
 * init survives forever. That is how whole sections vanished from hackaday /
 * 68kmla front pages (the fixes1011 regression): pre-1011 the same reads gave
 * `undefined`, which propagates as NaN and makes the style write a NO-OP.
 *
 * So the rule is split: when this returns 0, geometry accessors must answer
 * the way the pre-1011 engine did (undefined / all-zero rect) -- the shape a
 * decade of pages demonstrably tolerates -- never a fabricated real-looking
 * number. When it returns 1, answers come from the real box tree. */
/* fixes1230 (#167) - narrow geometry exception for Facebook's Bloks
 * checkpoint pages. Original evidence (2026-08-20 hardware log,
 * recover/code): the code-entry widget is a Bloks-mounted div (DIV#<uuid>,
 * kids=0) inside a page that otherwise renders cleanly with zero JS
 * exceptions -- the whole viewport collapses to h=1 because that one
 * container never gets children.
 *
 * fixes1231 correction: a FOLLOW-UP hardware log showed `LIFE JSGEOM
 * reads=0` on every one of these pages, including ones that still failed --
 * this scope was never actually reached, because Bloks' viewport-size check
 * here goes through ResizeObserver, not getBoundingClientRect/offsetWidth
 * (fixed separately, see the ResizeObserver comment below). Left in place
 * (harmless when unreached, zero cost) and widened from the single
 * recover/code URL to the whole confirmed family -- the SAME kids=0/h=1
 * collapse was independently confirmed on two_step_verification/two_factor
 * and two_step_verification/authentication too -- in case some other Bloks
 * variant does call real geometry directly. Still not a blanket flip: every
 * other m.facebook.com page keeps the default `undefined` policy pending
 * real incremental layout (see CLAUDE.md). */
static int qjs_geometry_scope_allowed(void)
{
	struct content *c = g_qjs_content;
	const char *url;
	const char *path;

	if (c == NULL || c->llcache == NULL) return 0;
	url = nsurl_access(content_get_url(c));
	if (url == NULL) return 0;
	path = strstr(url, "://m.facebook.com/");
	if (path == NULL) return 0;
	return strstr(path, "/recover/") != NULL ||
			strstr(path, "/two_step_verification/") != NULL;
}

/* fixes1073 (#265) - force layout before answering, the measure/mutate contract.
 *
 * Called at the top of every geometry entry point. If script has mutated the
 * DOM and the box tree has not caught up, rebuild and lay out RIGHT HERE so the
 * answer describes what the page actually looks like now. No-ops when nothing
 * is dirty, and refuses (leaving the fixes1016 `undefined`) whenever a flush
 * would be unsafe -- see macos9_reconvert_flush_now for the guard stack.
 *
 * This is the whole difference between a browser that a widget can lay itself
 * out against and one that hands back nothing and gets laid out wrong. */
static void qjs_geometry_flush(void)
{
	extern int macos9_reconvert_flush_now(void *cv);
	extern int macos9_reconvert_pending_for(void *cv);
	extern double macos9_micros(void);
	double t0;

	if (!MACSURF_JS_GEOMETRY && !qjs_geometry_scope_allowed()) return;
	if (g_qjs_content == NULL) return;
	/* fixes1077 - count and time every geometry entry. This is the number
	 * that says whether answering is affordable; before it existed the
	 * 13-second cost of enabling geometry could only be inferred from the
	 * difference between two hardware logs. */
	g_geom_reads++;

	/* #265 - settle-once-per-execution (see the flag comment above the
	 * counters): one flush per JS burst is all a script needs, so once
	 * settled, answer from the current box tree without paying for
	 * another reconvert. Content-keyed so an iframe runtime's settle
	 * cannot silence the parent's flushes. */
	if (g_geom_settled && g_geom_settled_c == (void *) g_qjs_content)
		return;

	/* Nothing pending: the box tree already answers for the current DOM.
	 * Settle without even paying for the flush call. */
	if (!macos9_reconvert_pending_for(g_qjs_content)) {
		g_geom_settled = 1;
		g_geom_settled_c = (void *) g_qjs_content;
		return;
	}

	/* Pending marks: flush synchronously (the fixes1073 forced reflow).
	 * Return 1 means a flush ran and consumed this content's slots --
	 * settled. Return 0 is a DECLINED flush (budget, in-progress, ...):
	 * leave the flag 0 so the next read retries, preserving today's
	 * retry semantics. */
	t0 = macos9_micros();
	if (macos9_reconvert_flush_now((void *) g_qjs_content)) {
		g_geom_settled = 1;
		g_geom_settled_c = (void *) g_qjs_content;
	}
	g_geom_us += (long)(macos9_micros() - t0);
}

static int qjs_geometry_settled(void)
{
	extern int macsurf_reconvert_in_progress;
	/* fixes1077 - CACHE THE LIVENESS SCAN.
	 *
	 * macos9_content_is_live() walks a 256-entry table, and this predicate
	 * runs on every single geometry read a page performs. Enabling geometry
	 * (fixes1073) therefore put a 256-iteration scan in front of every
	 * measurement: hackaday's navigation.js went 5.96s -> 19.04s while the
	 * forced reflow never fired once, so the whole cost bought nothing.
	 *
	 * The answer cannot change unless the content registry changes, and the
	 * registry now bumps an epoch whenever it does. Cache against (epoch,
	 * content) and rescan only when one moves -- which is a handful of times
	 * per navigation instead of tens of thousands. */
	extern unsigned long macos9_content_registry_epoch;
	static unsigned long cached_epoch = (unsigned long)-1;
	static void *cached_c = NULL;
	static int cached_live = 0;

	if (!MACSURF_JS_GEOMETRY && !qjs_geometry_scope_allowed()) return 0;
	if (g_qjs_content == NULL) return 0;
	if (macsurf_reconvert_in_progress) return 0;

	if (cached_epoch != macos9_content_registry_epoch ||
	    cached_c != (void *)g_qjs_content) {
		cached_epoch = macos9_content_registry_epoch;
		cached_c = (void *)g_qjs_content;
		cached_live = macos9_content_is_live(g_qjs_content);
	}
	if (cached_live == 0) return 0;

	/* fixes1087 (#265) - ask whether the TREE is stable, not whether the
	 * load has finished.
	 *
	 * The old test was `status != CONTENT_STATUS_DONE -> refuse`, and
	 * hardware showed it refusing every measurement taken during page load:
	 * declined=660 with notdone at 100%, four builds running. Script init
	 * happens before DONE, so every measure-then-layout widget got nothing.
	 * hackaday's featured slider is the visible casualty -- PAGEMAP has it
	 * slick-initialized with 5 slides and the track collapsed to h=15,
	 * because slick sets .slick-list height from a measurement and its
	 * slides are floated inside an overflow:hidden box with no natural
	 * height. It asked, we refused, it never set the height.
	 *
	 * macsurf_html_tree_stable checks the thing DONE was standing in for:
	 * a tree exists, no layout pass is running, no dom_to_box walk is in
	 * flight. That is strictly more precise -- a DONE content is ALSO
	 * unsafe mid-reconvert, which the status test never caught.
	 *
	 * Called only after the liveness check above, never before: on a freed
	 * content that dereference is the use-after-free the registry exists to
	 * prevent, and the epoch cache cannot report a freed content live. */
	{
		extern int macsurf_html_tree_stable(struct content *c);
		if (!macsurf_html_tree_stable(g_qjs_content)) {
			g_geom_unstable++;
			return 0;
		}
		if (g_qjs_content->status == CONTENT_STATUS_DONE)
			g_geom_at_done++;
		else
			g_geom_at_ready++;
	}
	return 1;
}

/* The box for a wrapper, or NULL. box_for_node hands back a raw pointer stored
 * on the DOM node, which can be stale across a reconvert -- the same hazard
 * the click path guards. Callers here only ever READ scalar fields from it and
 * degrade safely (see qjs_geometry_settled), so a stale-but-mapped box yields
 * wrong numbers rather than a fault; an unmapped one is rejected by
 * macsurf_ptr_is_heap. */
static struct box *qjs_box_for(JSValueConst v)
{
	dom_node *n;
	struct box *b;

	/* fixes1012 - THE DOCUMENTED CRASH-SAFETY CHECKLIST, which fixes1011
	 * satisfied only one third of.
	 *
	 * box_special.c:1417 records the rules for walking the box tree from
	 * outside layout, each one bought with a G3 crash:
	 *   1. re-resolve via box_for_node, never hold a raw box*  (fixes674b:
	 *      a stale box* went bad via normalisation and box_coords FAULTED
	 *      walking the parent chain)
	 *   2. only walk when the content is CONTENT_STATUS_DONE  (fixes674c: a
	 *      walk mid-construction hit a renormalising parent chain -> UAF)
	 *   3. registry-guarded liveness, pointer-membership only, no deref
	 *
	 * fixes1011 did (1) and checked the box pointer was in-heap, but did
	 * NEITHER (2) NOR (3) -- and qjs_box_origin calls box_coords(), which is
	 * precisely the parent-chain walk that faulted in 674b. Every
	 * getBoundingClientRect / offsetWidth / getComputedStyle from script was
	 * therefore one mid-reconvert measurement away from the same crash.
	 *
	 * A script measuring during teardown or mid-reconvert is not exotic: it
	 * is what a resize handler, a scroll handler or an IntersectionObserver
	 * callback does, and those fire exactly when the tree is churning. */
	if (!qjs_geometry_settled()) return NULL;

	n = qjs_get_node(v);
	if (n == NULL) return NULL;
	b = box_for_node(n);
	if (b == NULL) return NULL;
	if (!macsurf_ptr_is_heap((const void *)b)) return NULL;
	return b;
}

/* box_coords walks parent pointers to the root. A stale link anywhere in that
 * chain is a fault, so validate every hop and give up rather than follow a
 * wild pointer. Bounded too: a corrupted tree can be cyclic, and an unbounded
 * walk there hangs the cooperative event loop with no crash to diagnose. */
static int qjs_box_chain_ok(struct box *b)
{
	int depth = 0;
	while (b != NULL) {
		if (!macsurf_ptr_is_heap((const void *)b)) return 0;
		if (++depth > 512) return 0;
		b = b->parent;
	}
	return 1;
}

/* fixes1011 - SANITIZE THE SENTINEL BEFORE IT REACHES JAVASCRIPT.
 *
 * Every box is born width = UNKNOWN_WIDTH (INT_MAX) as a "not laid out yet"
 * marker, and this fork's failure-tolerant layout paths zero a failed box's
 * HEIGHT but never its WIDTH -- that asymmetry is the entire split-scrollbar
 * bug (fixes625), where INT_MAX rode up the ancestor chain and made the
 * document 2147483647 wide.
 *
 * Caught here by the harness on its first run: #feed reported a border-box of
 * 2147483647x0. Without this, el.offsetWidth hands a page INT_MAX for any
 * element whose layout has not resolved, and measuring code computes nonsense
 * from it -- silently, because it is a plausible number, not an error.
 *
 * Same rule layout_get_box_bbox applies: anything negative, INT_MAX, or past
 * a sane ceiling is "unknown", and unknown is 0. Zero is honest -- it is what
 * a not-yet-laid-out element measures in a real browser too. */
#define QJS_LAYOUT_SANE_MAX 1000000

static int qjs_sane(int v)
{
	if (v < 0) return 0;
	if (v >= QJS_LAYOUT_SANE_MAX) return 0;
	return v;
}

/* Document coordinates of a box's border edge. */
static void qjs_box_origin(struct box *b, int *ox, int *oy)
{
	int x = 0, y = 0;
	*ox = 0;
	*oy = 0;
	/* fixes1012 - validate the whole chain BEFORE box_coords walks it. This
	 * is the call that faulted in fixes674b. */
	if (!qjs_box_chain_ok(b)) return;
	box_coords(b, &x, &y);
	*ox = x;
	*oy = y;
}

/* fixes1015 - defined just below, used by get_rect above its definition. */
static void qjs_geom_audit(const char *what, JSValueConst wrapper,
		const char *result);

static JSValue qjs_el_get_rect(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv, int magic, JSValueConst *func_data)
{
	struct box *b;
	JSValue r;
	int x = 0, y = 0, w = 0, h = 0;
	(void)this_val; (void)argc; (void)argv; (void)magic;

	qjs_geometry_flush();	/* fixes1073 (#265) */
	b = qjs_box_for(this_val);
	if (b != NULL) {
		qjs_box_origin(b, &x, &y);
		/* Border-box, matching getBoundingClientRect: content plus padding
		 * plus border on each side. */
		w = qjs_sane(b->width) + b->padding[LEFT] + b->padding[RIGHT] +
			b->border[LEFT].width + b->border[RIGHT].width;
		h = qjs_sane(b->height) + b->padding[TOP] + b->padding[BOTTOM] +
			b->border[TOP].width + b->border[BOTTOM].width;
		/* fixes1132 - same sentinel-height→descendant-extent
		 * fallback as qjs_el_metric's QJS_M_OFFH above. */
		if (h <= 1 && qjs_sane(b->descendant_y1) > h + 10)
			h = qjs_sane(b->descendant_y1)
				+ b->padding[TOP] + b->padding[BOTTOM]
				+ b->border[TOP].width
				+ b->border[BOTTOM].width;
		/* Viewport coordinates: document position minus scroll. */
		x -= macsurf_qjs_scroll_x();
		y -= macsurf_qjs_scroll_y();
	}
	/* fixes1014 - no box (hidden element, or geometry not settled yet):
	 * the LITERAL all-zero rect, exactly what a real browser answers for
	 * display:none and what this engine answered for everything pre-1011.
	 * Subtracting the scroll here produced top=-scroll_y, a fabricated
	 * "above the viewport" position no browser ever reports. */
	{	/* fixes1015 */
		char rb[64];
		sprintf(rb, "x=%d y=%d w=%d h=%d%s", x, y, w, h,
				(b == NULL) ? " (no box)" : "");
		qjs_geom_audit("getRect", this_val, rb);
	}

	r = JS_NewObject(ctx);
	JS_SetPropertyStr(ctx, r, "x", JS_NewInt32(ctx, x));
	JS_SetPropertyStr(ctx, r, "y", JS_NewInt32(ctx, y));
	JS_SetPropertyStr(ctx, r, "left", JS_NewInt32(ctx, x));
	JS_SetPropertyStr(ctx, r, "top", JS_NewInt32(ctx, y));
	JS_SetPropertyStr(ctx, r, "right", JS_NewInt32(ctx, x + w));
	JS_SetPropertyStr(ctx, r, "bottom", JS_NewInt32(ctx, y + h));
	JS_SetPropertyStr(ctx, r, "width", JS_NewInt32(ctx, w));
	JS_SetPropertyStr(ctx, r, "height", JS_NewInt32(ctx, h));
	return r;
}

/* fixes1015 - geometry-read audit: what measuring code actually SEES is the
 * question three rounds have argued about from theory. Budgeted. */
long g_geom_audit = 200; /* fixes1016: non-static, reset per navigation */

static const char *qjs_metric_name(int magic);

static void qjs_geom_audit(const char *what, JSValueConst wrapper,
		const char *result)
{
	char nb[80];
	if (g_geom_audit <= 0) return;
	g_geom_audit--;
	qjs_node_brief(qjs_get_node(wrapper), nb, (int)sizeof nb);
	macsurf_debug_log_writef("LIFE geom %s %s -> %s", what, nb, result);
}

/* One accessor for every box-derived metric, selected by `magic`, so there is
 * a single place that knows how a metric maps onto the box tree. */
#define QJS_M_OFFW   0
#define QJS_M_OFFH   1
#define QJS_M_CLIW   2
#define QJS_M_CLIH   3
#define QJS_M_OFFT   4
#define QJS_M_OFFL   5
#define QJS_M_SCRW   6
#define QJS_M_SCRH   7

static JSValue qjs_el_metric(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv, int magic, JSValueConst *func_data)
{
	struct box *b;
	int v = 0;
	int bx = 0, by = 0, px = 0, py = 0;
	(void)this_val; (void)argc; (void)argv;

	/* fixes1014 - before the tree is settled (DOMContentLoaded runs BEFORE
	 * content_set_ready, so this window covers every ready-time init on
	 * every page), answer `undefined`, the pre-1011 shape: it propagates as
	 * NaN and a NaN style write is a no-op. Answering 0 here is what erased
	 * whole page sections -- scripts wrote the fabricated 0 back as inline
	 * width/height and the damage outlived the measurement. Once settled, a
	 * missing box really does mean "not rendered" and 0 is the true answer
	 * (jQuery :hidden relies on it). */
	qjs_geometry_flush();	/* fixes1073 (#265) */
	if (!qjs_geometry_settled()) {
		g_geom_undef++;			/* fixes1087 */
		qjs_geom_audit(qjs_metric_name(magic), this_val,
				"undefined (unsettled)");
		return JS_UNDEFINED;
	}

	b = qjs_box_for(this_val);
	if (b == NULL) {
		/* fixes1016 - no box while a mutation awaits its reconvert is
		 * NOT "hidden", it is "not measured yet": a real browser reflows
		 * synchronously and answers truly. We cannot (Phase 3's forced
		 * pass is its own risky round), so answer undefined -- the
		 * NaN-propagating no-op -- rather than a fabricated 0. This is
		 * exactly how slick collapsed the hackaday featured carousel:
		 * it measured its just-inserted slides, got 0, and wrote it
		 * back as inline sizes. */
		extern int macos9_reconvert_pending_for(void *cv);
		if (macos9_reconvert_pending_for(g_qjs_content)) {
			g_geom_undef++;		/* fixes1087 */
			qjs_geom_audit(qjs_metric_name(magic), this_val,
					"undefined (mutation pending)");
			return JS_UNDEFINED;
		}
		/* fixes1087 - NEVER fabricate 0 before the load is DONE.
		 *
		 * Opening the gate to CONTENT_STATUS_READY lets a widget measure
		 * elements that already have boxes, which is the whole point. But
		 * it also means reaching here for an element whose box has simply
		 * not been built yet, and answering 0 for that is the fixes1014
		 * failure verbatim: the script writes the fabricated 0 back as an
		 * inline width/height and the content is erased for good.
		 *
		 * 0 is only the TRUE answer once the document is DONE -- there a
		 * missing box really does mean "not rendered", which is what
		 * jQuery's :hidden relies on. Before that, `undefined` is the
		 * honest answer and NaN-propagates into a no-op.
		 *
		 * Harness Test 43 asserts exactly this and caught the first cut of
		 * this change fabricating a value in the unsettled window. */
		if (g_qjs_content->status != CONTENT_STATUS_DONE) {
			g_geom_undef++;
			qjs_geom_audit(qjs_metric_name(magic), this_val,
					"undefined (no box, not DONE)");
			return JS_UNDEFINED;
		}
		g_geom_zero++;			/* fixes1087 - the dangerous one */
		qjs_geom_audit(qjs_metric_name(magic), this_val,
				"0 (no box)");
		return JS_NewInt32(ctx, 0);
	}

	g_geom_real++;				/* fixes1087 */
	bx = b->border[LEFT].width + b->border[RIGHT].width;
	by = b->border[TOP].width + b->border[BOTTOM].width;
	px = b->padding[LEFT] + b->padding[RIGHT];
	py = b->padding[TOP] + b->padding[BOTTOM];

	switch (magic) {
	case QJS_M_OFFW: v = qjs_sane(b->width) + px + bx; break;
	case QJS_M_OFFH:
		v = qjs_sane(b->height) + py + by;
		/* fixes1132 - slick sets height:0px as a measurement reset
		 * then reads outerHeight(). b->height is 1 (explicit 0px
		 * clamped), but descendant_y1 is the real content extent
		 * (250+). When the explicit height is a clear sentinel (≤1)
		 * and content is meaningfully taller, answer content. */
		if (v <= 1 && qjs_sane(b->descendant_y1) > v + 10)
			v = qjs_sane(b->descendant_y1) + py + by;
		break;
	/* clientWidth/Height EXCLUDE the border and include padding. */
	case QJS_M_CLIW: v = qjs_sane(b->width) + px; break;
	case QJS_M_CLIH: v = qjs_sane(b->height) + py; break;
	case QJS_M_OFFT: {
		int x = 0, y = 0, pxo = 0, pyo = 0;
		qjs_box_origin(b, &x, &y);
		if (b->parent != NULL) qjs_box_origin(b->parent, &pxo, &pyo);
		v = y - pyo;
		break;
	}
	case QJS_M_OFFL: {
		int x = 0, y = 0, pxo = 0, pyo = 0;
		qjs_box_origin(b, &x, &y);
		if (b->parent != NULL) qjs_box_origin(b->parent, &pxo, &pyo);
		v = x - pxo;
		break;
	}
	/* scrollWidth/Height are the DESCENDANT extent -- how big the content
	 * is, not how big the box is. That distinction is the entire point of
	 * the property: overflow checks compare it against clientWidth. */
	case QJS_M_SCRW:
		v = qjs_sane(b->descendant_x1) > 0 ? qjs_sane(b->descendant_x1)
			: (qjs_sane(b->width) + px);
		break;
	case QJS_M_SCRH:
		v = qjs_sane(b->descendant_y1) > 0 ? qjs_sane(b->descendant_y1)
			: (qjs_sane(b->height) + py);
		break;
	default: v = 0; break;
	}
	if (v < 0) v = 0;
	{	/* fixes1015 */
		char rb[24];
		sprintf(rb, "%d", v);
		qjs_geom_audit(qjs_metric_name(magic), this_val, rb);
	}
	return JS_NewInt32(ctx, v);
}

static const char *qjs_metric_name(int magic)
{
	switch (magic) {
	case QJS_M_OFFW: return "offsetWidth";
	case QJS_M_OFFH: return "offsetHeight";
	case QJS_M_CLIW: return "clientWidth";
	case QJS_M_CLIH: return "clientHeight";
	case QJS_M_OFFT: return "offsetTop";
	case QJS_M_OFFL: return "offsetLeft";
	case QJS_M_SCRW: return "scrollWidth";
	case QJS_M_SCRH: return "scrollHeight";
	default:         return "metric?";
	}
}

/* fixes1011 - getComputedStyle, over the REAL cascade.
 *
 * It returned only what was in the inline `style` attribute, so
 *   getComputedStyle(el).display === 'none'
 * -- one of the most-executed lines on the web -- was false for everything
 * hidden by a stylesheet, a class, or the UA sheet. Scripts that toggle
 * visibility by reading it first therefore made the wrong decision every time,
 * silently.
 *
 * Reads box->style through the css_computed_* accessors. Only the properties
 * scripts actually read are covered rather than all 155: display, visibility,
 * position, width/height, color/background-color, font-size/family/weight,
 * line-height, opacity, overflow, z-index, text-align, float. Anything else
 * falls back to the inline value, which is what the old implementation always
 * did -- so this is strictly additive, never a regression.
 *
 * SERIALIZATION MATTERS as much as the value. getComputedStyle().width is
 * "100px" (a string with units), offsetWidth is a number, and colours are
 * "rgb(r, g, b)". Handing back a bare number where a string is expected makes
 * parseInt() succeed and string comparisons fail, which is the sort of wrong
 * that produces NaN three lines later instead of an error here. */
static void qjs_cs_px(JSContext *ctx, JSValue o, const char *name, int px)
{
	char buf[32];
	sprintf(buf, "%dpx", px);
	JS_SetPropertyStr(ctx, o, name, JS_NewString(ctx, buf));
}

static void qjs_cs_colour(JSContext *ctx, JSValue o, const char *name,
		css_color c)
{
	char buf[40];
	/* libcss packs colour as 0xAARRGGBB. */
	sprintf(buf, "rgb(%d, %d, %d)",
		(int)((c >> 16) & 0xff), (int)((c >> 8) & 0xff), (int)(c & 0xff));
	JS_SetPropertyStr(ctx, o, name, JS_NewString(ctx, buf));
}

static JSValue qjs_get_computed_style(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv)
{
	JSValue out, inline_style;
	struct box *b = NULL;
	(void)this_val;

	qjs_geometry_flush();	/* fixes1073 (#265) */
	out = JS_NewObject(ctx);
	if (argc >= 1 && JS_IsObject(argv[0])) {
		b = qjs_box_for(argv[0]);
	}
	/* fixes1015 - what did getComputedStyle actually answer? */
	if (argc >= 1 && JS_IsObject(argv[0])) {
		if (b != NULL && b->style != NULL) {
			char rb[48];
			sprintf(rb, "disp=%s w=%d",
				qjs_css_display_name(
					css_computed_display_static(b->style)),
				qjs_sane(b->width));
			qjs_geom_audit("computedStyle", argv[0], rb);
		} else {
			qjs_geom_audit("computedStyle", argv[0],
				"inline-only (no box/style)");
		}
	}

	if (b != NULL && b->style != NULL) {
		css_computed_style *s = b->style;
		css_fixed len = 0;
		css_unit unit = CSS_UNIT_PX;
		css_color col = 0;
		uint8_t v;

		v = css_computed_display_static(s);
		JS_SetPropertyStr(ctx, out, "display",
			JS_NewString(ctx, qjs_css_display_name(v)));
		v = css_computed_visibility(s);
		JS_SetPropertyStr(ctx, out, "visibility",
			JS_NewString(ctx,
				v == CSS_VISIBILITY_HIDDEN ? "hidden" :
				v == CSS_VISIBILITY_COLLAPSE ? "collapse" : "visible"));
		v = css_computed_position(s);
		JS_SetPropertyStr(ctx, out, "position",
			JS_NewString(ctx,
				v == CSS_POSITION_ABSOLUTE ? "absolute" :
				v == CSS_POSITION_RELATIVE ? "relative" :
				v == CSS_POSITION_FIXED ? "fixed" : "static"));
		v = css_computed_overflow_x(s);
		JS_SetPropertyStr(ctx, out, "overflow",
			JS_NewString(ctx,
				v == CSS_OVERFLOW_HIDDEN ? "hidden" :
				v == CSS_OVERFLOW_SCROLL ? "scroll" :
				v == CSS_OVERFLOW_AUTO ? "auto" : "visible"));
		v = css_computed_float(s);
		JS_SetPropertyStr(ctx, out, "float",
			JS_NewString(ctx,
				v == CSS_FLOAT_LEFT ? "left" :
				v == CSS_FLOAT_RIGHT ? "right" : "none"));
		v = css_computed_text_align(s);
		JS_SetPropertyStr(ctx, out, "textAlign",
			JS_NewString(ctx,
				v == CSS_TEXT_ALIGN_CENTER ? "center" :
				v == CSS_TEXT_ALIGN_RIGHT ? "right" :
				v == CSS_TEXT_ALIGN_JUSTIFY ? "justify" : "left"));

		css_computed_color(s, &col);
		qjs_cs_colour(ctx, out, "color", col);
		col = 0;
		css_computed_background_color(s, &col);
		qjs_cs_colour(ctx, out, "backgroundColor", col);

		if (css_computed_font_size(s, &len, &unit) == CSS_FONT_SIZE_DIMENSION) {
			qjs_cs_px(ctx, out, "fontSize", FIXTOINT(len));
		}
		v = css_computed_font_weight(s);
		JS_SetPropertyStr(ctx, out, "fontWeight",
			JS_NewString(ctx,
				(v == CSS_FONT_WEIGHT_BOLD ||
				 v == CSS_FONT_WEIGHT_BOLDER ||
				 v >= CSS_FONT_WEIGHT_600) ? "bold" : "normal"));

		{
			int32_t z = 0;
			if (css_computed_z_index(s, &z) == CSS_Z_INDEX_SET) {
				JS_SetPropertyStr(ctx, out, "zIndex",
					JS_NewInt32(ctx, (int)z));
			} else {
				JS_SetPropertyStr(ctx, out, "zIndex",
					JS_NewString(ctx, "auto"));
			}
		}
		{
			css_fixed o = 0;
			if (css_computed_opacity(s, &o) == CSS_OPACITY_SET) {
				JS_SetPropertyStr(ctx, out, "opacity",
					JS_NewFloat64(ctx, FIXTOFLT(o)));
			} else {
				JS_SetPropertyStr(ctx, out, "opacity",
					JS_NewString(ctx, "1"));
			}
		}
		/* Used width/height come from the BOX, not the cascade -- the
		 * cascade may say `auto` while the box knows the resolved pixels,
		 * and "used value" is what getComputedStyle is specified to give. */
		qjs_cs_px(ctx, out, "width", qjs_sane(b->width));
		qjs_cs_px(ctx, out, "height", qjs_sane(b->height));
		qjs_cs_px(ctx, out, "marginTop", b->margin[TOP]);
		qjs_cs_px(ctx, out, "marginRight", b->margin[RIGHT]);
		qjs_cs_px(ctx, out, "marginBottom", b->margin[BOTTOM]);
		qjs_cs_px(ctx, out, "marginLeft", b->margin[LEFT]);
		qjs_cs_px(ctx, out, "paddingTop", b->padding[TOP]);
		qjs_cs_px(ctx, out, "paddingRight", b->padding[RIGHT]);
		qjs_cs_px(ctx, out, "paddingBottom", b->padding[BOTTOM]);
		qjs_cs_px(ctx, out, "paddingLeft", b->padding[LEFT]);
	}

	/* Inline style wins where the cascade gave us nothing, preserving the
	 * old behaviour for every property not covered above. */
	inline_style = JS_UNDEFINED;
	if (argc >= 1 && JS_IsObject(argv[0])) {
		inline_style = JS_GetPropertyStr(ctx, argv[0], "style");
	}
	JS_SetPropertyStr(ctx, out, "__inline", inline_style);
	return out;
}

/* fixes1006 - call obj.dispatchEvent(ev), swallowing (but LOGGING) a throw.
 * One bad handler must never abort propagation to the rest, and a silent
 * catch is how "site loads, does nothing, no log" happens. `what` names the
 * target in the log line. */
static void qjs_fire_dispatch(JSContext *ctx, JSValueConst obj,
		JSValue evobj, const char *what)
{
	JSValue fn, ret;
	JSValue argv[1];

	if (JS_IsUndefined(obj) || JS_IsNull(obj)) return;
	/* fixes1008 (1e) - __msFireLocal FIRST. Elements now have a NATIVE
	 * dispatchEvent that starts a fresh libdom dispatch; calling it from
	 * inside the libdom callback would recurse without end. __msFireLocal is
	 * the local _L/_H firing that the callback actually wants. document and
	 * window have no __msFireLocal (they are not element wrappers), so they
	 * fall through to their JS dispatchEvent shims, which is correct. */
	fn = JS_GetPropertyStr(ctx, obj, "__msFireLocal");
	if (!JS_IsFunction(ctx, fn)) {
		JS_FreeValue(ctx, fn);
		fn = JS_GetPropertyStr(ctx, obj, "dispatchEvent");
	}
	if (JS_IsFunction(ctx, fn)) {
		argv[0] = evobj;
		ret = JS_Call(ctx, fn, obj, 1, (JSValueConst *)argv);
		if (JS_IsException(ret)) {
			JSValue ex = JS_GetException(ctx);
			qjs_log_exc(ctx, ex, "event handler threw", what);
			JS_FreeValue(ctx, ex);
		}
		JS_FreeValue(ctx, ret);
	}
	JS_FreeValue(ctx, fn);
}

static void qjs_dom_listener_cb(dom_event *evt, void *pw)
{
	dom_event_target *ct = NULL;
	dom_event_target *tt = NULL;
	dom_node *node;
	struct qjs_wrap_entry *hit;
	JSContext *ctx;
	dom_string *type_ds = NULL;
	JSValue evobj, disp, ret, pd;
	JSValue callargv[1];

	/* #265 - a libdom-originated dispatch (fire_generic_dom_event from
	 * interaction.c, i.e. a real click) is a C->JS execution boundary:
	 * the WNE loop runs event handlers BEFORE macsurf_qjs_pump_all in the
	 * same pass, so without this clear the previous pass's settle would
	 * leak into the click handler's first geometry read. Page-initiated
	 * el.dispatchEvent does NOT reach here (it fires __msFireLocal
	 * directly), so mid-burst dispatches are untouched. */
	qjs_geom_settle_begin();
	(void)pw;
	if (evt == NULL) return;
	if (dom_event_get_current_target(evt, &ct) != DOM_NO_ERR) return;
	if (ct == NULL) return;
	node = (dom_node *)ct;          /* getter took a ref; we own it */

	hit = qjs_wrap_lookup(node);
	if (hit == NULL) {
		/* No wrapper: this node was never touched by script, or the realm
		 * has been rebuilt and its wrappers drained. Nothing to run.
		 * fixes1015 - this silent drop IS a lost handler if the node ever
		 * had one; make it visible. */
		if (g_evmiss_audit > 0) {
			char nb[80];
			g_evmiss_audit--;
			qjs_node_brief(node, nb, (int)sizeof nb);
			macsurf_debug_log_writef(
				"LIFE evfire MISS (no wrapper) target=%s", nb);
		}
		macsurf_dom_node_unref(node);
		return;
	}
	ctx = qjs_ctx_for_runtime(hit->rt);
	if (ctx == NULL) {
		macsurf_dom_node_unref(node);
		return;
	}
	if (dom_event_get_type(evt, &type_ds) != DOM_NO_ERR || type_ds == NULL) {
		macsurf_dom_node_unref(node);
		return;
	}
	/* fixes1015 - every real dispatch that reaches script, with identity. */
	{
		if (g_evfire_audit > 0) {
			char nb[80];
			g_evfire_audit--;
			qjs_node_brief(node, nb, (int)sizeof nb);
			macsurf_debug_log_writef("LIFE evfire %s at %s",
					dom_string_data(type_ds), nb);
		}
	}

	/* fixes1008 (1d) - a REAL Event instance, not an ad-hoc object.
	 *
	 * This used to be a bare JS_NewObject with `type`, `target` and three
	 * methods bolted on. Consequences that all show up in real library code:
	 * `ev instanceof Event` was false; `bubbles`, `cancelable`, `eventPhase`,
	 * `defaultPrevented`, `timeStamp` and `isTrusted` were all undefined; and
	 * a mouse handler reading `e.clientX` or a key handler reading `e.key`
	 * got undefined and computed NaN from it.
	 *
	 * Built via `new Event(type)` so the prototype chain is right, then the
	 * real values are written over the constructor's defaults from the actual
	 * dom_event. Falls back to a plain object if the constructor is somehow
	 * missing, because an event with no prototype still beats no dispatch. */
	{
		JSValue global = JS_GetGlobalObject(ctx);
		JSValue ctor = JS_GetPropertyStr(ctx, global, "Event");
		evobj = JS_UNDEFINED;
		if (JS_IsFunction(ctx, ctor)) {
			JSValue targ[1];
			targ[0] = JS_NewStringLen(ctx, dom_string_data(type_ds),
					(size_t)dom_string_length(type_ds));
			evobj = JS_CallConstructor(ctx, ctor, 1,
					(JSValueConst *)targ);
			JS_FreeValue(ctx, targ[0]);
			if (JS_IsException(evobj)) {
				JS_FreeValue(ctx, JS_GetException(ctx));
				evobj = JS_UNDEFINED;
			}
		}
		JS_FreeValue(ctx, ctor);
		JS_FreeValue(ctx, global);
		if (JS_IsUndefined(evobj)) evobj = JS_NewObject(ctx);
	}
	JS_SetPropertyStr(ctx, evobj, "type",
		JS_NewStringLen(ctx, dom_string_data(type_ds),
				(size_t)dom_string_length(type_ds)));

	/* Real flags off the real dom_event. eventPhase in particular must be
	 * AT_TARGET for the target's own visit -- before fixes1005 the spurious
	 * second visit reported BUBBLING, and a count-only test would not have
	 * noticed the phase was wrong. */
	{
		bool b = false;
		dom_event_flow_phase ph = 0;
		if (dom_event_get_bubbles(evt, &b) == DOM_NO_ERR)
			JS_SetPropertyStr(ctx, evobj, "bubbles", JS_NewBool(ctx, b));
		b = false;
		if (dom_event_get_cancelable(evt, &b) == DOM_NO_ERR)
			JS_SetPropertyStr(ctx, evobj, "cancelable", JS_NewBool(ctx, b));
		if (dom_event_get_event_phase(evt, &ph) == DOM_NO_ERR)
			JS_SetPropertyStr(ctx, evobj, "eventPhase",
					JS_NewInt32(ctx, (int)ph));
		JS_SetPropertyStr(ctx, evobj, "defaultPrevented",
				JS_NewBool(ctx, 0));
		JS_SetPropertyStr(ctx, evobj, "timeStamp",
				JS_NewFloat64(ctx, macsurf_qjs_get_now()));
		/* isTrusted is TRUE only here -- this is the native UI path.
		 * Anything from `new Event` / dispatchEvent reports false, and some
		 * libraries branch on it to reject synthetic input. */
		JS_SetPropertyStr(ctx, evobj, "isTrusted", JS_NewBool(ctx, 1));
	}

	/* Mouse and keyboard details, from the values the frontend already has.
	 * macsurf_qjs_set_event_detail() is filled in by interaction.c right
	 * before the dispatch; zeroed otherwise so a stale click's coordinates
	 * can never leak into an unrelated event. */
	{
		const char *t = dom_string_data(type_ds);
		size_t tl = (size_t)dom_string_length(type_ds);
		int is_mouse = (tl >= 5 && strncmp(t, "mouse", 5) == 0) ||
			(tl == 5 && strncmp(t, "click", 5) == 0) ||
			(tl == 8 && strncmp(t, "dblclick", 8) == 0) ||
			(tl == 11 && strncmp(t, "contextmenu", 11) == 0);
		int is_key = (tl >= 3 && strncmp(t, "key", 3) == 0);
		if (is_mouse) {
			JS_SetPropertyStr(ctx, evobj, "clientX",
					JS_NewInt32(ctx, g_qjs_ev_x));
			JS_SetPropertyStr(ctx, evobj, "clientY",
					JS_NewInt32(ctx, g_qjs_ev_y));
			JS_SetPropertyStr(ctx, evobj, "pageX",
					JS_NewInt32(ctx, g_qjs_ev_x));
			JS_SetPropertyStr(ctx, evobj, "pageY",
					JS_NewInt32(ctx, g_qjs_ev_y));
			JS_SetPropertyStr(ctx, evobj, "screenX",
					JS_NewInt32(ctx, g_qjs_ev_x));
			JS_SetPropertyStr(ctx, evobj, "screenY",
					JS_NewInt32(ctx, g_qjs_ev_y));
			JS_SetPropertyStr(ctx, evobj, "button",
					JS_NewInt32(ctx, g_qjs_ev_button));
			JS_SetPropertyStr(ctx, evobj, "buttons",
					JS_NewInt32(ctx, g_qjs_ev_button ? 1 : 0));
			JS_SetPropertyStr(ctx, evobj, "detail", JS_NewInt32(ctx, 1));
		} else if (is_key) {
			char kb[8];
			int n = 0;
			if (g_qjs_ev_key >= 32 && g_qjs_ev_key < 127) {
				kb[n++] = (char)g_qjs_ev_key;
			}
			kb[n] = '\0';
			JS_SetPropertyStr(ctx, evobj, "key", JS_NewString(ctx, kb));
			JS_SetPropertyStr(ctx, evobj, "code", JS_NewString(ctx, kb));
			JS_SetPropertyStr(ctx, evobj, "keyCode",
					JS_NewInt32(ctx, g_qjs_ev_key));
			JS_SetPropertyStr(ctx, evobj, "which",
					JS_NewInt32(ctx, g_qjs_ev_key));
			JS_SetPropertyStr(ctx, evobj, "charCode",
					JS_NewInt32(ctx, g_qjs_ev_key));
		}
		if (is_mouse || is_key) {
			JS_SetPropertyStr(ctx, evobj, "shiftKey",
				JS_NewBool(ctx, (g_qjs_ev_mods & 1) != 0));
			JS_SetPropertyStr(ctx, evobj, "ctrlKey",
				JS_NewBool(ctx, (g_qjs_ev_mods & 2) != 0));
			JS_SetPropertyStr(ctx, evobj, "altKey",
				JS_NewBool(ctx, (g_qjs_ev_mods & 4) != 0));
			JS_SetPropertyStr(ctx, evobj, "metaKey",
				JS_NewBool(ctx, (g_qjs_ev_mods & 8) != 0));
		}
	}
	JS_SetPropertyStr(ctx, evobj, "currentTarget",
		JS_DupValue(ctx, hit->val));
	/* fixes1006 (1c) - event.target, WRAPPED ON DEMAND.
	 *
	 * This used to be lookup-only: on a wrap-table miss it simply did not set
	 * the property, so `event.target` was `undefined` for any node script had
	 * never touched. Delegation handlers universally open with
	 * `e.target.matches(...)` or `e.target.closest(...)`, which throws on
	 * undefined and takes the handler down -- so delegation was dead in the
	 * field even where the listener itself was reached. It passed in t.html
	 * only because every node there had been through getElementById and so
	 * already had a wrapper.
	 *
	 * LOOKUP FIRST, wrap only on a miss. That order is load-bearing now that
	 * the document is in the table: qjs_wrap_any_node deliberately returns
	 * JS_NULL for nodeType 9 (minting a wrapper for a node shape whose vtable
	 * does not match was the fixes846 ASan overflow), so going straight to it
	 * would yield `e.target === null` for every document-targeted event --
	 * DOMContentLoaded, readystatechange, document.dispatchEvent -- and
	 * `e.target === document` is a check libraries make.
	 *
	 * REF DISCIPLINE: the getter hands us an owned ref. qjs_wrap_any_node
	 * CONSUMES one, so the miss path must NOT unref afterwards; the hit path
	 * must. Getting this backwards is a double-unref on every dispatch. */
	if (dom_event_get_target(evt, &tt) == DOM_NO_ERR && tt != NULL) {
		struct qjs_wrap_entry *th = qjs_wrap_lookup((dom_node *)tt);
		JSValue tv;
		if (th != NULL) {
			tv = JS_DupValue(ctx, th->val);
			macsurf_dom_node_unref((dom_node *)tt);
		} else {
			tv = qjs_wrap_any_node(ctx, (dom_node *)tt);
		}
		if (!JS_IsNull(tv) && !JS_IsUndefined(tv)) {
			JS_SetPropertyStr(ctx, evobj, "target",
				JS_DupValue(ctx, tv));
			JS_SetPropertyStr(ctx, evobj, "srcElement", tv);
		} else {
			JS_FreeValue(ctx, tv);
		}
	}
	pd = JS_NewInt64(ctx, (long long)(size_t)evt);
	JS_SetPropertyStr(ctx, evobj, "preventDefault",
		JS_NewCFunctionData(ctx, qjs_ev_prevent_default2_data,
				0, 0, 1, &pd));
	JS_SetPropertyStr(ctx, evobj, "stopPropagation",
		JS_NewCFunctionData(ctx, qjs_ev_stop_propagation_data,
				0, 0, 1, &pd));
	/* fixes1008 (1d) - its OWN implementation now, not the stopPropagation
	 * alias. See qjs_ev_stop_immediate_data. */
	JS_SetPropertyStr(ctx, evobj, "stopImmediatePropagation",
		JS_NewCFunctionData(ctx, qjs_ev_stop_immediate_data,
				0, 0, 1, &pd));
	JS_FreeValue(ctx, pd);   /* NewCFunctionData took its own reference */

	/* Fire this node's _L + _H through the one implementation that exists. */
	{
		/* fixes1006 (1b) - WINDOW FAN-OUT, and the order matters in BOTH
		 * directions.
		 *
		 * `window` has no DOM node, so its listeners are registered against
		 * the document node and delivered here. Per spec window is the
		 * OUTERMOST target: it runs BEFORE the document while capturing and
		 * AFTER it while bubbling. A single "always last" fan-out would be
		 * wrong for capture, which is exactly the phase jQuery's focusin
		 * workaround relies on.
		 *
		 * Only the document entry fans out -- element hits dispatch once. */
		int is_doc = (g_qjs_document != NULL &&
				node == (dom_node *)g_qjs_document);
		int capturing = 0;
		JSValue global = JS_UNDEFINED;
		if (is_doc) {
			dom_event_flow_phase ph = 0;
			if (dom_event_get_event_phase(evt, &ph) == DOM_NO_ERR &&
			    ph == DOM_CAPTURING_PHASE) {
				capturing = 1;
			}
			global = JS_GetGlobalObject(ctx);
		}

		if (is_doc && capturing) {
			qjs_fire_dispatch(ctx, global, evobj, "window");
		}

		qjs_fire_dispatch(ctx, hit->val, evobj,
				is_doc ? "document" : "element");

		if (is_doc && !capturing) {
			qjs_fire_dispatch(ctx, global, evobj, "window");
		}
		if (is_doc) JS_FreeValue(ctx, global);
	}
	JS_FreeValue(ctx, evobj);
	macsurf_dom_string_unref(type_ds);
	macsurf_dom_node_unref(node);
}

/* ===================================================================
 * fixes1008 (1a) - THE REGISTERED-TYPE GATE.
 *
 * 1f fans out mousedown/mouseup/mouseover/mouseout/dblclick/keyup/... from
 * interaction.c. Without a gate, every hover transition on a page where
 * nothing listens would build an Event object and hit the wrap table on
 * every mouse move, on a G3. That is the difference between usable and
 * molasses, so the gate lands BEFORE the fan-out, not after.
 *
 * SET-ONLY, NEVER CLEARED. A count would have to be decremented on
 * removeEventListener, and a bare bitmask cleared there cannot know whether
 * another listener of the same type survives -- it would go dark while a
 * live listener remained, which is a silent "handlers stopped working" bug.
 * Set-only is entirely adequate because realms are per-navigation: the set
 * dies with the page. The cost of a stale bit is one wasted dispatch into an
 * empty listener list, which is cheap and correct.
 *
 * FED FROM THE SINGLE CHOKEPOINT. All three registration routes --
 * addEventListener, inline on* attributes (macsurf_qjs_bind_inline_handlers),
 * and el.onclick= (__msRegEvent) -- already funnel through
 * qjs_dom_register_listener, so the increment goes there rather than at three
 * call sites. A missed one would mean markup handlers silently never firing.
 *
 * GOVERNS NATIVE UI FAN-OUT ONLY. el.dispatchEvent(new Event('foo')) must
 * always dispatch regardless of this set, or synthetic events on custom types
 * break. Do not "optimise" that by consulting the gate there.
 * =================================================================== */
#define QJS_EVGATE_SLOTS 64

static const char *s_evgate[QJS_EVGATE_SLOTS];
static int s_evgate_n = 0;

static void qjs_evgate_add(const char *type)
{
	int i;
	if (type == NULL || type[0] == '\0') return;
	for (i = 0; i < s_evgate_n; i++) {
		if (strcmp(s_evgate[i], type) == 0) return;
	}
	if (s_evgate_n >= QJS_EVGATE_SLOTS) return;   /* full: fail OPEN below */
	{
		char *dup = (char *)malloc(strlen(type) + 1);
		if (dup == NULL) return;
		strcpy(dup, type);
		s_evgate[s_evgate_n++] = dup;
	}
}

static void qjs_evgate_reset(void)
{
	int i;
	for (i = 0; i < s_evgate_n; i++) free((void *)s_evgate[i]);
	s_evgate_n = 0;
}

/* Exported for interaction.c. Returns non-zero if ANY listener of this type
 * has been registered in this realm.
 *
 * FAILS OPEN in two cases, both deliberate: before any script has run
 * (s_evgate_n == 0, e.g. a page with no JS at all -- dispatching a handful of
 * events into an empty set costs nothing and a closed gate here would be
 * indistinguishable from a broken engine), and when the table is full. A gate
 * that fails closed silently deletes user interaction, which is exactly the
 * bug class this whole batch exists to remove. */
int macsurf_qjs_event_type_live(const char *type);
int macsurf_qjs_event_type_live(const char *type)
{
	int i;
	if (type == NULL) return 1;
	if (s_evgate_n == 0) return 1;
	if (s_evgate_n >= QJS_EVGATE_SLOTS) return 1;
	for (i = 0; i < s_evgate_n; i++) {
		if (strcmp(s_evgate[i], type) == 0) return 1;
	}
	return 0;
}

/* ===================================================================
 * fixes1008 - REGISTRATION DEDUPE, at the chokepoint.
 *
 * libdom does NOT dedupe (node, type): dom_event_target_add_event_listener
 * appends another listener_entry every time, and _dom_event_target_dispatch
 * loops over ALL of them -- so N registrations replay the node's whole _L/_H
 * list N times. We pass the same shared g_qjs_dom_listener every time, so the
 * duplicates are pure loss.
 *
 * THREE routes register and they could not see each other:
 *   addEventListener   guarded by `fresh` (first listener of that type in _L)
 *   el.onclick=        __msRegEvent, NO guard
 *   inline on* markup  macsurf_qjs_bind_inline_handlers, NO guard
 * so `s.onload = fn` plus addEventListener('load') on the same node produced
 * two entries and fired onload TWICE -- caught by harness Test 13, which is
 * the dynamic-loader idiom, where a promise resolving twice is exactly the
 * hang this engine spent fixes868/869 fixing.
 *
 * The document had the same shape and got a JS-side __msRegOnce in fixes1006.
 * This replaces per-route guards with one C-side set keyed by
 * (node, type, capture), which is where it always belonged: every route funnels
 * through qjs_dom_register_listener, so one check covers all of them, elements
 * and document alike.
 *
 * FAILS OPEN on malloc failure -- a duplicate dispatch is survivable, a missing
 * registration means the handler never runs at all.
 * =================================================================== */
#define QJS_REG_BUCKETS 64
#define QJS_REG_TYPELEN 32

struct qjs_reg_entry {
	dom_node *node;
	char type[QJS_REG_TYPELEN];
	int capture;
	struct qjs_reg_entry *next;
};

static struct qjs_reg_entry *s_reg_buckets[QJS_REG_BUCKETS];
/* fixes1013 - how many distinct (node, type, capture) registrations the page
 * made. A page that ran its scripts but wired up nothing looks very different
 * from one that never ran them, and this is the number that separates them. */
int s_reg_n_registered = 0;  /* exported for audit */

/* Returns 1 if this (node, type, capture) was ALREADY registered. */
static int qjs_reg_seen(dom_node *node, const char *type, int capture)
{
	unsigned int h = (unsigned int)(((size_t)node >> 3) &
			(QJS_REG_BUCKETS - 1));
	struct qjs_reg_entry *e = s_reg_buckets[h];
	while (e != NULL) {
		if (e->node == node && e->capture == capture &&
		    strcmp(e->type, type) == 0) {
			return 1;
		}
		e = e->next;
	}
	e = (struct qjs_reg_entry *)malloc(sizeof(struct qjs_reg_entry));
	if (e == NULL) return 0;
	e->node = node;
	e->capture = capture;
	strncpy(e->type, type, QJS_REG_TYPELEN - 1);
	e->type[QJS_REG_TYPELEN - 1] = '\0';
	e->next = s_reg_buckets[h];
	s_reg_buckets[h] = e;
	s_reg_n_registered++;
	return 0;
}

/* Realm teardown: the nodes are going away, so the set must go with them or a
 * recycled node address would look pre-registered and its listeners would
 * never reach libdom. Called from qjs_wrap_drain. */
static void qjs_reg_clear(void)
{
	unsigned int i;
	for (i = 0; i < QJS_REG_BUCKETS; i++) {
		struct qjs_reg_entry *e = s_reg_buckets[i];
		while (e != NULL) {
			struct qjs_reg_entry *n = e->next;
			free(e);
			e = n;
		}
		s_reg_buckets[i] = NULL;
	}
	s_reg_n_registered = 0;
}

/* fixes996 - the single place a node is registered with libdom.
 *
 * THREE routes can put a handler on a node and every one of them must
 * register, or a real click never reaches it: addEventListener (fixes989),
 * an inline on* attribute in markup (fixes995), and `el.onclick = fn` from
 * script -- which is the one that was missed. Hardware found it: t.html test 3
 * stayed grey while 1 and 2 went green.
 *
 * The on* setter is a JS accessor (qjs_el_install_proto) that only writes _H,
 * so nothing told libdom the node cared about the type. Factoring registration
 * to here and calling it from all three closes the class rather than the
 * instance: the next route added gets it by calling one function. */
static void qjs_dom_register_listener(dom_node *node, const char *type,
		int capture)
{
	dom_string *tds;
	if (node == NULL || type == NULL || type[0] == '\0') return;
	/* fixes1008 (1a) - the gate's single feed point. All three registration
	 * routes reach here, so this one line covers addEventListener, inline
	 * on* attributes and el.onclick= alike. */
	qjs_evgate_add(type);
	/* fixes1008 - once per (node, type, capture). See qjs_reg_seen: libdom
	 * appends duplicates and replays the whole handler list per entry. */
	if (qjs_reg_seen(node, type, capture)) return;
	/* fixes1015 - audit every NEW libdom registration with its target, so
	 * "the page wired itself up" is checkable listener by listener. */
	if (g_evreg_audit > 0) {
		char nb[80];
		g_evreg_audit--;
		qjs_node_brief(node, nb, (int)sizeof nb);
		macsurf_debug_log_writef("LIFE evreg %s cap=%d on %s",
				type, capture, nb);
	}
	if (g_qjs_dom_listener == NULL) {
		(void)dom_event_listener_create(qjs_dom_listener_cb, NULL,
				&g_qjs_dom_listener);
	}
	if (g_qjs_dom_listener == NULL) return;
	tds = qjs_make_domstr(type);
	if (tds == NULL) return;
	(void)dom_event_target_add_event_listener(node, tds,
			g_qjs_dom_listener, capture ? true : false);
	macsurf_dom_string_unref(tds);
}

/* fixes996 - exposed to the on* setter, which is JS and cannot reach libdom.
 * Idempotent by libdom's own keying: registering the same (type, listener,
 * capture) twice is not additive, so re-assigning el.onclick is harmless. */
static JSValue qjs_el_reg_event_data(JSContext *ctx,
		JSValueConst this_val, int argc, JSValueConst *argv,
		int magic, JSValueConst *func_data)
{
	dom_node *node;
	const char *type_c;
	(void)this_val; (void)magic;
	node = qjs_get_node(this_val);
	if (node == NULL || argc < 1) return JS_UNDEFINED;
	type_c = JS_ToCString(ctx, argv[0]);
	if (type_c == NULL) return JS_UNDEFINED;
	qjs_dom_register_listener(node, type_c, 0);
	JS_FreeCString(ctx, type_c);
	return JS_UNDEFINED;
}

/* fixes1006 (1b) - the DOCUMENT/WINDOW registration hook.
 *
 * document.addEventListener and window.addEventListener stored their handlers
 * in JS-only registries (document._listeners / _winListeners) that libdom knew
 * nothing about. Nothing ever called dom_event_target_add_event_listener for
 * them, so a real mouse click dispatched into an empty listener set at the
 * document and never reached $(document).on('click', ...) -- the dominant
 * pattern in jQuery, XenForo (XF.activate), WordPress and every
 * delegation-based app. The registries themselves are fine and stay; only the
 * REGISTRATION was missing.
 *
 * `window` has no DOM node of its own, so its listeners register against the
 * document node too and are fanned out separately in qjs_dom_listener_cb,
 * which is where the capture/bubble ordering is applied.
 *
 * Idempotent by libdom's own keying: (node, type, listener, capture) twice is
 * not additive, so re-registering the same type is harmless. */
static JSValue qjs_doc_reg_event(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv)
{
	const char *type_c;
	int capture = 0;
	(void)this_val;
	if (g_qjs_document == NULL || argc < 1) return JS_UNDEFINED;
	type_c = JS_ToCString(ctx, argv[0]);
	if (type_c == NULL) return JS_UNDEFINED;
	if (argc >= 2) {
		if (JS_IsBool(argv[1])) {
			capture = JS_ToBool(ctx, argv[1]);
		} else if (JS_IsObject(argv[1])) {
			JSValue c = JS_GetPropertyStr(ctx, argv[1], "capture");
			capture = JS_ToBool(ctx, c);
			JS_FreeValue(ctx, c);
		}
	}
	qjs_dom_register_listener((dom_node *)g_qjs_document, type_c, capture);
	JS_FreeCString(ctx, type_c);
	return JS_UNDEFINED;
}

/* fixes1008 (1e) - el.dispatchEvent, routed through libdom so it BUBBLES.
 *
 * The JS implementation fired only that node's own _L/_H and returned true
 * unconditionally. Two things were wrong with that and both are load-bearing:
 * a synthetic event never reached ancestors or the document (so a framework
 * that triggers a control programmatically and relies on delegation to catch
 * it saw nothing), and cancellation was unobservable, because dispatchEvent
 * must return FALSE when a cancelable event was cancelled.
 *
 * Routing through dom_event_target_dispatch_event means the ONE dispatch
 * implementation serves both real input and synthetic events -- no second
 * path to drift. el.click() (fixes997) bubbles for free as a result.
 *
 * RE-ENTRANCY: qjs_dom_listener_cb calls dispatchEvent on the wrapper it
 * looked up, so dispatching through libdom from inside dispatchEvent would
 * recurse forever. g_qjs_in_dispatch is the guard -- while a native dispatch
 * is in flight, this falls back to the local _L/_H firing, which is exactly
 * what the callback wants from it.
 *
 * The type is taken from ev.type; a plain object works as well as a real
 * Event, because that is what page code passes. */
static int g_qjs_in_dispatch = 0;

static JSValue qjs_el_dispatch_event_data(JSContext *ctx,
		JSValueConst this_val, int argc, JSValueConst *argv,
		int magic, JSValueConst *func_data)
{
	dom_node *node;
	JSValue tv, local;
	const char *type_c;
	dom_string *tds;
	dom_event *evt = NULL;
	bool ok_flag = true;
	bool bubbles = true, cancelable = true;
	(void)this_val; (void)magic;

	node = qjs_get_node(this_val);
	if (node == NULL || argc < 1 || !JS_IsObject(argv[0])) {
		return JS_NewBool(ctx, 1);
	}

	/* Re-entrant (we are inside qjs_dom_listener_cb): fire locally. */
	if (g_qjs_in_dispatch) {
		local = JS_GetPropertyStr(ctx, this_val, "__msFireLocal");
		if (JS_IsFunction(ctx, local)) {
			JSValue r = JS_Call(ctx, local, this_val, 1, argv);
			JS_FreeValue(ctx, local);
			if (JS_IsException(r)) return r;
			JS_FreeValue(ctx, r);
		} else {
			JS_FreeValue(ctx, local);
		}
		return JS_NewBool(ctx, 1);
	}

	tv = JS_GetPropertyStr(ctx, argv[0], "type");
	type_c = JS_ToCString(ctx, tv);
	JS_FreeValue(ctx, tv);
	if (type_c == NULL) return JS_NewBool(ctx, 1);

	{
		JSValue b = JS_GetPropertyStr(ctx, argv[0], "bubbles");
		if (!JS_IsUndefined(b)) bubbles = JS_ToBool(ctx, b) ? true : false;
		JS_FreeValue(ctx, b);
		b = JS_GetPropertyStr(ctx, argv[0], "cancelable");
		if (!JS_IsUndefined(b)) cancelable = JS_ToBool(ctx, b) ? true : false;
		JS_FreeValue(ctx, b);
	}

	tds = qjs_make_domstr(type_c);
	JS_FreeCString(ctx, type_c);
	if (tds == NULL) return JS_NewBool(ctx, 1);

	if (dom_event_create(&evt) != DOM_NO_ERR) {
		macsurf_dom_string_unref(tds);
		return JS_NewBool(ctx, 1);
	}
	if (dom_event_init(evt, tds, bubbles, cancelable) != DOM_NO_ERR) {
		dom_event_unref(evt);
		macsurf_dom_string_unref(tds);
		return JS_NewBool(ctx, 1);
	}
	macsurf_dom_string_unref(tds);

	g_qjs_in_dispatch = 1;
	(void)dom_event_target_dispatch_event(node, evt, &ok_flag);
	g_qjs_in_dispatch = 0;
	dom_event_unref(evt);

	/* false when a cancelable event was cancelled -- the whole point of the
	 * return value, and previously always true. */
	return JS_NewBool(ctx, ok_flag ? 1 : 0);
}

static JSValue qjs_el_add_event_listener_data(JSContext *ctx,
		JSValueConst this_val, int argc, JSValueConst *argv,
		int magic, JSValueConst *func_data)
{
	dom_node *node;
	const char *type_c;
	JSValue L, arr, lenv, C, carr, R, seen;
	uint32_t len = 0;
	int capture = 0;
	int fresh = 0;
	char key[160];
	(void)this_val; (void)magic;

	node = qjs_get_node(this_val);
	if (node == NULL || argc < 2 || !JS_IsFunction(ctx, argv[1])) {
		return JS_UNDEFINED;
	}
	type_c = JS_ToCString(ctx, argv[0]);
	if (type_c == NULL) return JS_UNDEFINED;

	/* third argument is capture, or an options object carrying it. Parsed
	 * and HONOURED now; the old JS shim accepted and discarded it. */
	if (argc >= 3) {
		if (JS_IsBool(argv[2])) {
			capture = JS_ToBool(ctx, argv[2]);
		} else if (JS_IsObject(argv[2])) {
			JSValue c = JS_GetPropertyStr(ctx, argv[2], "capture");
			capture = JS_ToBool(ctx, c);
			JS_FreeValue(ctx, c);
		}
	}

	/* JS-side registry, unchanged shape: el._L[type] is an array of fns. */
	L = JS_GetPropertyStr(ctx, this_val, "_L");
	if (!JS_IsObject(L)) {
		JS_FreeValue(ctx, L);
		L = JS_NewObject(ctx);
		JS_SetPropertyStr(ctx, this_val, "_L", JS_DupValue(ctx, L));
	}
	arr = JS_GetPropertyStr(ctx, L, type_c);
	if (JS_IsUndefined(arr) || JS_IsNull(arr)) {
		JS_FreeValue(ctx, arr);
		arr = JS_NewArray(ctx);
		JS_SetPropertyStr(ctx, L, type_c, JS_DupValue(ctx, arr));
		/* fixes1040 (#264) - no longer sets `fresh` here. The libdom
		 * registration is keyed per (type, capture) via _LR below, which
		 * is now the single source of truth; setting it here as well
		 * would register a second time for the same pair. */
	}
	lenv = JS_GetPropertyStr(ctx, arr, "length");
	JS_ToUint32(ctx, &len, lenv);
	JS_FreeValue(ctx, lenv);
	JS_SetPropertyUint32(ctx, arr, len, JS_DupValue(ctx, argv[1]));
	JS_FreeValue(ctx, arr);
	JS_FreeValue(ctx, L);

	/* fixes1040 (#264) - el._LC[type] is the index-aligned array of capture
	 * flags for el._L[type]. The flag has to survive to DISPATCH time:
	 * __msFireLocal must fire only the listeners belonging to the phase
	 * libdom is currently in, and until now it had no way to tell them
	 * apart. removeEventListener splices this in lockstep with _L. */
	C = JS_GetPropertyStr(ctx, this_val, "_LC");
	if (!JS_IsObject(C)) {
		JS_FreeValue(ctx, C);
		C = JS_NewObject(ctx);
		JS_SetPropertyStr(ctx, this_val, "_LC", JS_DupValue(ctx, C));
	}
	carr = JS_GetPropertyStr(ctx, C, type_c);
	if (JS_IsUndefined(carr) || JS_IsNull(carr)) {
		JS_FreeValue(ctx, carr);
		carr = JS_NewArray(ctx);
		JS_SetPropertyStr(ctx, C, type_c, JS_DupValue(ctx, carr));
	}
	JS_SetPropertyUint32(ctx, carr, len, JS_NewBool(ctx, capture));
	JS_FreeValue(ctx, carr);
	JS_FreeValue(ctx, C);

	/* fixes1040 (#264) - register with libdom once per (node, type, CAPTURE),
	 * not once per (node, type).
	 *
	 * The old rule registered only for the FIRST listener of a type and
	 * carried whichever capture flag that one happened to have. A node
	 * holding both a capture and a non-capture listener was therefore known
	 * to libdom under ONE phase only, and because __msFireLocal then fired
	 * the whole _L list, both listeners ran in that phase. On hardware that
	 * produced [cap,bubble,target] for a capture+bubble outer with a target
	 * inner -- the bubble listener firing during the CAPTURE pass, before
	 * the target was reached. Harness Test 47 pins the ordering. */
	key[0] = '\0';
	if (strlen(type_c) < sizeof(key) - 3) {
		strcpy(key, type_c);
		/* octal escape, not \x01: a hex escape would swallow the letter */
		strcat(key, capture ? "\001C" : "\001B");
		R = JS_GetPropertyStr(ctx, this_val, "_LR");
		if (!JS_IsObject(R)) {
			JS_FreeValue(ctx, R);
			R = JS_NewObject(ctx);
			JS_SetPropertyStr(ctx, this_val, "_LR",
					JS_DupValue(ctx, R));
		}
		seen = JS_GetPropertyStr(ctx, R, key);
		if (JS_IsUndefined(seen) || JS_IsNull(seen)) {
			fresh = 1;
			JS_SetPropertyStr(ctx, R, key, JS_NewBool(ctx, 1));
		}
		JS_FreeValue(ctx, seen);
		JS_FreeValue(ctx, R);
	} else {
		/* pathological type name: register rather than silently drop */
		fresh = 1;
	}

	if (fresh) {
		qjs_dom_register_listener(node, type_c, capture);
	}
	JS_FreeCString(ctx, type_c);
	return JS_UNDEFINED;
}

/* fixes1040 (#264) - splice el._LC[type] at `idx`, keeping the capture-flag
 * array index-aligned with el._L[type] after a removeEventListener. Silent
 * no-op when the element has no _LC yet (a listener registered before this
 * change, or an element that only ever used on* handlers). */
static void qjs_lc_splice_at(JSContext *ctx, JSValueConst el,
		const char *type_c, uint32_t idx)
{
	JSValue C, carr, sp;

	C = JS_GetPropertyStr(ctx, el, "_LC");
	if (!JS_IsObject(C)) { JS_FreeValue(ctx, C); return; }

	carr = JS_GetPropertyStr(ctx, C, type_c);
	if (JS_IsObject(carr)) {
		sp = JS_GetPropertyStr(ctx, carr, "splice");
		if (JS_IsFunction(ctx, sp)) {
			JSValue sargv[2];
			JSValue r;
			sargv[0] = JS_NewUint32(ctx, idx);
			sargv[1] = JS_NewInt32(ctx, 1);
			r = JS_Call(ctx, sp, carr, 2, (JSValueConst *)sargv);
			JS_FreeValue(ctx, r);
			JS_FreeValue(ctx, sargv[0]);
			JS_FreeValue(ctx, sargv[1]);
		}
		JS_FreeValue(ctx, sp);
	}
	JS_FreeValue(ctx, carr);
	JS_FreeValue(ctx, C);
}

static JSValue qjs_el_remove_event_listener_data(JSContext *ctx,
		JSValueConst this_val, int argc, JSValueConst *argv,
		int magic, JSValueConst *func_data)
{
	const char *type_c;
	JSValue L, arr, lenv, item;
	uint32_t len = 0, i;
	(void)this_val; (void)magic;

	if (argc < 2) return JS_UNDEFINED;
	type_c = JS_ToCString(ctx, argv[0]);
	if (type_c == NULL) return JS_UNDEFINED;

	L = JS_GetPropertyStr(ctx, this_val, "_L");
	if (JS_IsObject(L)) {
		arr = JS_GetPropertyStr(ctx, L, type_c);
		if (JS_IsObject(arr)) {
			lenv = JS_GetPropertyStr(ctx, arr, "length");
			JS_ToUint32(ctx, &len, lenv);
			JS_FreeValue(ctx, lenv);
			for (i = 0; i < len; i++) {
				item = JS_GetPropertyUint32(ctx, arr, i);
				if (JS_VALUE_GET_PTR(item) ==
				    JS_VALUE_GET_PTR(argv[1])) {
					JSValue sp = JS_GetPropertyStr(ctx, arr,
							"splice");
					if (JS_IsFunction(ctx, sp)) {
						JSValue sargv[2];
						JSValue r;
						sargv[0] = JS_NewUint32(ctx, i);
						sargv[1] = JS_NewInt32(ctx, 1);
						r = JS_Call(ctx, sp, arr, 2,
							(JSValueConst *)sargv);
						JS_FreeValue(ctx, r);
						JS_FreeValue(ctx, sargv[0]);
						JS_FreeValue(ctx, sargv[1]);
					}
					JS_FreeValue(ctx, sp);
					JS_FreeValue(ctx, item);
					/* fixes1040 (#264) - splice the capture-flag
					 * array at the SAME index. _LC is index-
					 * aligned with _L; letting them drift would
					 * make every later listener dispatch in the
					 * wrong phase, which is a subtler version of
					 * the bug this whole change fixes. */
					qjs_lc_splice_at(ctx, this_val,
							type_c, i);
					break;
				}
				JS_FreeValue(ctx, item);
			}
		}
		JS_FreeValue(ctx, arr);
	}
	JS_FreeValue(ctx, L);
	JS_FreeCString(ctx, type_c);
	/* The libdom registration is deliberately LEFT in place: it is keyed by
	 * (node, type) not by callback, and with an empty _L the callback runs
	 * and finds nothing. Removing it here would break a second listener of
	 * the same type that is still registered. */
	return JS_UNDEFINED;
}

/* fixes995 (#264) - inline on* HTML attributes.
 *
 * `<a onclick="...">` in MARKUP has never done anything: the attribute was
 * never compiled, so the handler did not exist in any registry and no dispatch
 * could reach it. That is separate from `el.onclick = fn` in script, which has
 * worked for a while (the _H accessors, fixes872) -- markup and script were
 * two different worlds and only one of them was wired.
 *
 * Compiled at INSERTION, from the DOMNodeInserted hook that already exists in
 * dom_event.c, so it covers parsed markup and JS-inserted markup alike with no
 * new traversal. The compiled function lands in the SAME _H slot the script
 * accessors use, which gives correct replace-semantics for free: assigning
 * el.onclick later overwrites the markup handler, exactly as it should.
 * Registering the shared libdom listener for the type is what makes a real
 * click reach it (fixes989).
 *
 * The handler body is wrapped as a function of `event`, matching what browsers
 * do; `this` is bound to the element by dispatchEvent's `.call(el, ev)`.
 */
void macsurf_qjs_bind_inline_handlers(struct dom_node *node);
void macsurf_qjs_bind_inline_handlers(struct dom_node *node)
{
	static const char *const names[] = {
		"onclick", "onchange", "onsubmit", "oninput", "onfocus",
		"onblur", "onmouseover", "onmouseout", "onmousedown",
		"onmouseup", "onkeydown", "onkeyup", "onkeypress", "ondblclick"
	};
	const int n_names = (int)(sizeof(names) / sizeof(names[0]));
	JSContext *ctx;
	JSValue wrapper, H, fnv, src;
	dom_string *val = NULL;
	dom_string *nm = NULL;
	int i;
	int bound = 0;

	if (node == NULL || g_heap == NULL) return;
	ctx = g_heap->ctx;
	if (ctx == NULL) return;

	for (i = 0; i < n_names; i++) {
		const char *type;
		char body_buf[16];
		int has = 0;

		nm = qjs_make_domstr(names[i]);
		if (nm == NULL) continue;
		if (macsurf_dom_element_has_attribute((dom_element *)node,
				nm, &has) != DOM_NO_ERR || has == 0) {
			macsurf_dom_string_unref(nm);
			continue;
		}
		val = NULL;
		if (macsurf_dom_element_get_attribute((dom_element *)node,
				nm, &val) != DOM_NO_ERR || val == NULL) {
			macsurf_dom_string_unref(nm);
			continue;
		}
		macsurf_dom_string_unref(nm);
		(void)body_buf;

		/* Compile: (function(event){ <attr value> }) */
		{
			const char *b = dom_string_data(val);
			size_t blen = (size_t)dom_string_length(val);
			size_t need = blen + 32;
			char *srcbuf = (char *)malloc(need);
			if (srcbuf == NULL) {
				macsurf_dom_string_unref(val);
				continue;
			}
			strcpy(srcbuf, "(function(event){");
			memcpy(srcbuf + 17, b, blen);
			strcpy(srcbuf + 17 + blen, "})");
			src = JS_Eval(ctx, srcbuf, 17 + blen + 2,
					"<inline-handler>", JS_EVAL_TYPE_GLOBAL);
			free(srcbuf);
		}
		macsurf_dom_string_unref(val);
		if (JS_IsException(src)) {
			JSValue ex = JS_GetException(ctx);
			JS_FreeValue(ctx, ex);
			JS_FreeValue(ctx, src);
			continue;
		}
		if (!JS_IsFunction(ctx, src)) {
			JS_FreeValue(ctx, src);
			continue;
		}
		fnv = src;

		/* Wrapper on demand: only elements that actually carry an on*
		 * attribute get one, so this does not wrap the whole document. */
		macsurf_dom_node_ref(node);
		wrapper = qjs_wrap_element(ctx, (dom_element *)node);
		if (!JS_IsObject(wrapper)) {
			JS_FreeValue(ctx, fnv);
			JS_FreeValue(ctx, wrapper);
			continue;
		}
		H = JS_GetPropertyStr(ctx, wrapper, "_H");
		if (!JS_IsObject(H)) {
			JS_FreeValue(ctx, H);
			H = JS_NewObject(ctx);
			JS_SetPropertyStr(ctx, wrapper, "_H",
					JS_DupValue(ctx, H));
		}
		type = names[i] + 2;            /* "onclick" -> "click" */
		JS_SetPropertyStr(ctx, H, type, fnv);   /* takes fnv */
		JS_FreeValue(ctx, H);
		JS_FreeValue(ctx, wrapper);

		/* Make a REAL event reach it (fixes996 - shared helper). */
		qjs_dom_register_listener(node, type, 0);
		bound++;
	}
	if (bound > 0) {
		macsurf_debug_log_writef(
			"LIFE jsevent inline bound=%d", bound);
	}
}

/* Full wrap: identical to qjs_wrap_element now - the method surface lives on
 * the class proto / Node.prototype (qjs_el_install_proto_surface, once per
 * realm), so a wrap is identity props + a wrap-table entry and cache hits
 * skip it entirely. Kept as a named entry point for the traversal/mutation
 * call sites. */
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
	n = qjs_get_node(this_val);
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
	n = qjs_get_node(this_val);
	if (n == NULL || argc < 1) return JS_UNDEFINED;
	v = JS_ToCString(ctx, argv[0]);
	if (v == NULL) return JS_UNDEFINED;
	macsurf_dom_characterdata_set_data_s(n, v);
	JS_FreeCString(ctx, v);
	if (g_qjs_content) macos9_js_mark_dom_dirty_node(g_qjs_content,
			(void *) n, MACOS9_DOMMUT_CHARDATA);
	return JS_UNDEFINED;
}

static JSValue qjs_text_append_child_noop(JSContext *ctx,
		JSValueConst this_val, int argc, JSValueConst *argv)
{
	(void) ctx; (void) this_val; (void) argc; (void) argv;
	return JS_NULL;
}

/* fixes878 - qjs_text_clone_node_data is GONE. It was
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

	if (qjs_wrap_insert(node, owner_doc, obj, JS_GetRuntime(ctx)) == 0) {
		JS_SetOpaque(obj, NULL);
		macsurf_dom_node_unref(node);
		if (owner_doc) macsurf_dom_node_unref(owner_doc);
		return obj;
	}

	/* fixes878 - report the node's REAL type instead of hardcoding #text.
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
		/* fixes1127 -- real family prototype: text/comment/CDATA wrappers
		 * answer instanceof Text/CharacterData/Node truthfully and must NOT
		 * answer instanceof HTMLElement (the shared class proto alone would
		 * route them there after the fixes1127 p.__proto__ link -- a lying
		 * answer that sends node-skipping loops down element paths). */
		qjs_wrap_set_family_proto(ctx, obj,
			(wt == 8) ? "Comment" : (wt == 4) ? "CharacterData" : "Text");
	}
	JS_SetPropertyStr(ctx, obj, "__ptr",
		JS_NewInt64(ctx, (long long) (size_t) tn));

	/* fixes1170 (#211) - nodeValue/data/textContent live on
	 * CharacterData.prototype and parentNode + the node-level traversal
	 * surface on Node.prototype (qjs_el_install_proto_surface, once per
	 * realm), so a text/comment/CDATA wrapper is now just identity props +
	 * the appendChild no-op below -- zero per-node closures. The family
	 * proto set above chains Text/Comment/CharacterData -> CharacterData ->
	 * Node, which is what makes those reachable. */
	JS_SetPropertyStr(ctx, obj, "appendChild",
		JS_NewCFunction(ctx, qjs_text_append_child_noop,
				"appendChild", 1));

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

	if (qjs_wrap_insert(node, owner_doc, obj, JS_GetRuntime(ctx)) == 0) {
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
	JS_FreeValue(ctx, data[0]);

	{
		JSAtom atom;
		JSValue getter;

		atom = JS_NewAtom(ctx, "children");
		getter = JS_GetPropertyStr(ctx, obj, "__getChildren");
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

	/* fixes1170 (#211) - parentNode + the node-level traversal surface are on
	 * Node.prototype (qjs_el_install_proto_surface), reached through the
	 * DocumentFragment family proto set below; the per-instance closures
	 * are gone. */

	/* fixes1127 -- real family prototype: a fragment answers instanceof
	 * DocumentFragment/Node, never HTMLElement (same lying-answer reasoning
	 * as the text-node wrapper). */
	qjs_wrap_set_family_proto(ctx, obj, "DocumentFragment");

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
/* ==== fixes871 (#298) - compound selector matcher ==========================
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
/* fixes880 - the selector TYPES moved up next to the forward declarations, so
 * the element-scoped qsa (which is defined well above this point) can hold a
 * `struct qjs_sel` by value and reach the same matcher the document level uses.
 * The matcher itself stays here. */

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

/* fixes1240 - one `[...]` attribute clause, factored out of qjs_sel_parse's
 * main loop so the :not(...) inner parser below can share it instead of
 * duplicating fixes1090c's parsing rules (name/op/quoted-or-not value) a
 * second time and risking the two silently drifting apart. `*pp` must
 * point AT the '['; advances past the matching ']' unconditionally (same
 * "swallow to the close bracket" recovery as the original had) even on a
 * parse it can't represent. Returns 1 iff it recorded a usable attribute
 * constraint into attr[]/*nattr. */
static int qjs_sel_parse_attr1(const char **pp, struct qjs_sel_attr *attr,
		int *nattr, int max_attr, int *approx)
{
	const char *p = *pp;
	int added = 0;

	p++; /* skip '[' */
	while (*p == ' ' || *p == '\t') p++;
	if (*nattr >= max_attr) {
		*approx = 1;
	} else {
		struct qjs_sel_attr *a = &attr[*nattr];
		int k = 0;
		memset(a, 0, sizeof(*a));
		while (k < QJS_SEL_NAME - 1 && *p != '\0' &&
		       *p != ']' && *p != '=' && *p != '~' &&
		       *p != '^' && *p != '$' && *p != '*' &&
		       *p != ' ' && *p != '\t') {
			a->name[k++] = *p++;
		}
		a->name[k] = '\0';
		while (*p == ' ' || *p == '\t') p++;
		if (k == 0) {
			*approx = 1;
		} else if (*p == ']' || *p == '\0') {
			a->op = 0; /* bare [attr] presence */
			(*nattr)++;
			added = 1;
		} else if (*p == '~' || *p == '^' || *p == '$' || *p == '*') {
			a->op = *p++;
			if (*p == '=') p++; else *approx = 1;
		} else if (*p == '=') {
			a->op = '=';
			p++;
		} else {
			*approx = 1;
			a->op = 0;
			a->name[0] = '\0';
		}
		if (a->name[0] != '\0' && a->op != 0) {
			int vk = 0;
			char quote = 0;
			while (*p == ' ' || *p == '\t') p++;
			if (*p == '"' || *p == '\'') {
				quote = *p++;
			}
			if (quote) {
				while (vk < QJS_SEL_NAME - 1 &&
				       *p != '\0' && *p != quote) {
					a->val[vk++] = *p++;
				}
				if (*p == quote) p++;
			} else {
				while (vk < QJS_SEL_NAME - 1 &&
				       *p != '\0' && *p != ']' &&
				       *p != ' ' && *p != '\t') {
					a->val[vk++] = *p++;
				}
			}
			a->val[vk] = '\0';
			(*nattr)++;
			added = 1;
		}
	}
	/* Swallow to the closing ']' (also any trailing case-flag / stray
	 * syntax we did not parse above). */
	while (*p != '\0' && *p != ']') p++;
	if (*p == ']') p++;
	*pp = p;
	return added;
}

/* fixes1240 (#167) - :not(X) inner parser. `*pp` points AT the ':' of
 * ":not(...)"; on success advances past the matching ')' and returns 1
 * with c->has_not / c->nott filled in. On anything beyond a single
 * tag/.class/#id/[attr] target (compound target, combinators, nested
 * pseudo-classes, or a malformed/unterminated clause) leaves *pp
 * untouched and returns 0 so the caller falls back to the existing
 * approx/swallow-to-whitespace recovery -- same degrade posture as every
 * other unsupported selector shape here, not a new failure mode. */
static int qjs_sel_parse_not(const char **pp, struct qjs_sel_compound *c,
		int *approx)
{
	const char *q = *pp;
	int any = 0;

	if (strncmp(q, ":not(", 5) != 0 || c->has_not) return 0;
	q += 5;
	while (*q != '\0' && *q != ')') {
		if (*q == '.' || *q == '#') {
			char kind = *q++;
			char buf[QJS_SEL_NAME];
			int k = 0;
			while (k < QJS_SEL_NAME - 1 && *q != '\0' && *q != '.'
			       && *q != '#' && *q != '[' && *q != ')') {
				buf[k++] = *q++;
			}
			buf[k] = '\0';
			if (k == 0) return 0;
			if (kind == '.') {
				if (c->nott.ncls >= QJS_SEL_MAX_CLASS) return 0;
				strcpy(c->nott.cls[c->nott.ncls], buf);
				c->nott.ncls++;
			} else {
				strcpy(c->nott.id, buf);
			}
			any = 1;
		} else if (*q == '[') {
			if (!qjs_sel_parse_attr1(&q, c->nott.attr,
					&c->nott.nattr, QJS_SEL_MAX_ATTR, approx)) {
				return 0;
			}
			any = 1;
		} else if (*q == ' ' || *q == '\t' || *q == ':' || *q == '>' ||
			   *q == ',' || *q == '+' || *q == '~') {
			/* Compound/combinator/nested-pseudo target: beyond the
			 * single-simple-selector scope documented above. */
			return 0;
		} else {
			int k = 0;
			while (k < 31 && *q != '\0' && *q != '.' && *q != '#'
			       && *q != '[' && *q != ')') {
				char ch = *q++;
				c->nott.tag[k++] = (ch >= 'A' && ch <= 'Z')
						? (char)(ch + 32) : ch;
			}
			c->nott.tag[k] = '\0';
			if (k == 0) return 0;
			any = 1;
		}
	}
	if (!any || *q != ')') return 0;
	c->has_not = 1;
	*pp = q + 1;
	return 1;
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
			} else if (*p == '[') {
				/* fixes1090c - attribute selectors, e.g.
				 * img[data-lazy] or a[href^="https:"]. These were
				 * previously swallowed whole (falling back to
				 * tag-only), which made `img[data-lazy]` match EVERY
				 * <img>, lazy or not -- confirmed against the real
				 * hackaday slick.js bundle in the harness: it made
				 * loadImages() run jQuery's deprecated .load(fn)
				 * event shorthand (removed in jQuery 3.x) against
				 * ordinary images, throwing and aborting the theme's
				 * init script before it reached the slider at all.
				 * Parse [name], [name=val], [name~=val], [name^=val],
				 * [name$=val], [name*=val], quoted or not -- shared
				 * with the :not(...) inner parser (fixes1240) via
				 * qjs_sel_parse_attr1. */
				if (qjs_sel_parse_attr1(&p, c->attr, &c->nattr,
						QJS_SEL_MAX_ATTR, &out->approx)) {
					started = 1;
				}
			} else if (*p == ':') {
				/* fixes1240 (#167) - :not(single-simple-selector), e.g.
				 * Facebook's own script[data-sjs]:not([data-processed]).
				 * Anything beyond that scope (compound/multi-selector
				 * :not(), any other pseudo-class) falls through to the
				 * same approx/swallow recovery as before. */
				if (qjs_sel_parse_not(&p, c, &out->approx)) {
					started = 1;
				} else {
					out->approx = 1;
					while (*p != '\0' && *p != ' ' && *p != '\t') p++;
				}
			} else if (*p == '>' || *p == ',' || *p == '+' || *p == '~') {
				/* Unsupported: swallow the rest of this compound and
				 * fall back to whatever tag/class/id/attr we already
				 * have. */
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

/* fixes1240 - tag/#id/.class/[attr] matching against an ELEMENT node,
 * factored out of qjs_compound_match so the :not(...) exclusion (a second,
 * independent simple selector, struct qjs_sel_not_target -- see its
 * declaration for why it isn't the same struct) can be tested through the
 * exact same rules instead of a second, driftable copy. Caller has already
 * confirmed `node` is an ELEMENT_NODE. `cls` takes the same layout as
 * either struct's `cls[QJS_SEL_MAX_CLASS][QJS_SEL_NAME]` field decayed to
 * a pointer-to-array, which is why both call sites can pass their own
 * `c->cls` / `c->nott.cls` directly.
 *
 * fixes1241 (#167) - `cls` MUST be `const`. Both call sites are
 * qjs_compound_match's `const struct qjs_sel_compound *c`, so `c->cls` /
 * `c->nott.cls` are themselves const-qualified array types
 * (`const char[QJS_SEL_MAX_CLASS][QJS_SEL_NAME]`) -- passing that to a
 * non-const `char cls[][QJS_SEL_NAME]` parameter silently drops the
 * qualifier, which CW8 rejects outright ("illegal implicit conversion
 * from 'const char[4][64]' to 'char (*)[64]'", real build error against
 * this exact fixes1240 code) where gcc's default gnu99 mode -- what
 * check-c89/check-macdefault actually run -- stayed silent. Harness-clean
 * is not proof of CW8-clean for a qualifier mismatch like this one. */
static int qjs_simple_match(dom_node *node, const char *tag,
		const char cls[][QJS_SEL_NAME], int ncls, const char *id,
		const struct qjs_sel_attr *attr, int nattr)
{
	int i;

	if (tag[0] != '\0' && strcmp(tag, "*") != 0) {
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
		ok = (strcmp(lc, tag) == 0);
		if (!ok) return 0;
	}

	if (id[0] != '\0') {
		char buf[QJS_SEL_NAME];
		if (!qjs_attr_str((dom_element *)node, "id", buf, (int)sizeof(buf)))
			return 0;
		if (strcmp(buf, id) != 0) return 0;
	}

	if (ncls > 0) {
		char buf[512];
		if (!qjs_attr_str((dom_element *)node, "class", buf, (int)sizeof(buf)))
			return 0;
		for (i = 0; i < ncls; i++) {
			if (!qjs_class_has(buf, cls[i])) return 0;
		}
	}

	/* fixes1090c - [attr] / [attr=val] / [attr~=val] / [attr^=val] /
	 * [attr$=val] / [attr*=val]. A dropped/unparsed attribute (name
	 * cleared by the parser) imposes no constraint, same degrade as an
	 * overflowed class list. */
	for (i = 0; i < nattr; i++) {
		const struct qjs_sel_attr *a = &attr[i];
		char buf[256];
		size_t bl, vl;

		if (a->name[0] == '\0') continue;
		if (!qjs_attr_str((dom_element *)node, a->name, buf,
				(int)sizeof(buf))) {
			return 0; /* attribute not present at all */
		}
		bl = strlen(buf);
		vl = strlen(a->val);
		switch (a->op) {
		case 0: /* bare presence: qjs_attr_str already confirmed it */
			break;
		case '=':
			if (strcmp(buf, a->val) != 0) return 0;
			break;
		case '~':
			if (!qjs_class_has(buf, a->val)) return 0;
			break;
		case '^':
			if (vl == 0 || vl > bl || strncmp(buf, a->val, vl) != 0)
				return 0;
			break;
		case '$':
			if (vl == 0 || vl > bl ||
					strcmp(buf + (bl - vl), a->val) != 0)
				return 0;
			break;
		case '*':
			if (vl == 0 || strstr(buf, a->val) == NULL) return 0;
			break;
		default:
			break;
		}
	}
	return 1;
}

/* Does ONE element match ONE compound? */
static int qjs_compound_match(dom_node *node, const struct qjs_sel_compound *c)
{
	dom_node_type ntype = 0;

	if (node == NULL) return 0;
	macsurf_dom_node_get_node_type(node, &ntype);
	if (ntype != 1) return 0; /* ELEMENT_NODE only */

	if (!qjs_simple_match(node, c->tag, c->cls, c->ncls, c->id,
			c->attr, c->nattr)) {
		return 0;
	}

	/* fixes1240 (#167) - :not(X): exclude if the negated simple selector
	 * DOES match. */
	if (c->has_not && qjs_simple_match(node, c->nott.tag, c->nott.cls,
			c->nott.ncls, c->nott.id, c->nott.attr, c->nott.nattr)) {
		return 0;
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

/* fixes1242 (#167) - split on TOP-LEVEL commas only: not inside `[...]`
 * (an attribute value could itself legitimately contain a comma, e.g.
 * `[data-list="a,b"]`) and not inside a quoted attribute value even if it
 * contains an unbalanced bracket character. Each segment is trimmed and
 * parsed through the existing, unmodified qjs_sel_parse. */
static void qjs_sel_list_parse(const char *sel, struct qjs_sel_list *out)
{
	const char *p;
	const char *seg_start;
	int depth = 0;
	char quote = 0;

	memset(out, 0, sizeof(*out));
	if (sel == NULL) return;
	p = sel;
	seg_start = sel;

	for (;;) {
		char ch = *p;
		if (quote != 0) {
			if (ch == quote) quote = 0;
			else if (ch == '\0') break;
			p++;
			continue;
		}
		if (ch == '"' || ch == '\'') {
			quote = ch;
			p++;
			continue;
		}
		if (ch == '[') {
			depth++;
			p++;
			continue;
		}
		if (ch == ']') {
			if (depth > 0) depth--;
			p++;
			continue;
		}
		if ((ch == ',' && depth == 0) || ch == '\0') {
			const char *a = seg_start;
			const char *b = p;
			while (a < b && (*a == ' ' || *a == '\t')) a++;
			while (b > a && (b[-1] == ' ' || b[-1] == '\t')) b--;
			if (b > a) {
				if (out->n < QJS_SEL_MAX_LIST) {
					char buf[256];
					int len = (int)(b - a);
					if (len > (int)sizeof(buf) - 1)
						len = (int)sizeof(buf) - 1;
					memcpy(buf, a, (size_t)len);
					buf[len] = '\0';
					qjs_sel_parse(buf, &out->alt[out->n]);
					if (out->alt[out->n].approx) out->approx = 1;
					out->n++;
				} else {
					out->approx = 1; /* more alternatives than we track */
				}
			}
			seg_start = p + 1;
			if (ch == '\0') break;
		}
		p++;
	}
}

static int qjs_sel_list_match(dom_node *node, const struct qjs_sel_list *sl)
{
	int i;
	for (i = 0; i < sl->n; i++) {
		if (qjs_sel_match(node, &sl->alt[i])) return 1;
	}
	return 0;
}

/* Collect every match in document order, deduped by construction: each node
 * is visited exactly once regardless of how many alternatives it satisfies,
 * unlike collecting per-alternative and merging would be. */
static void qjs_collect_by_sel_list(JSContext *ctx, dom_node *node,
		const struct qjs_sel_list *sl, JSValue arr, int *count)
{
	dom_node *child = NULL;
	dom_node *next  = NULL;

	if (node == NULL) return;
	if (qjs_sel_list_match(node, sl)) {
		macsurf_dom_node_ref(node); /* qjs_wrap_element CONSUMES a ref */
		JS_SetPropertyUint32(ctx, arr, (uint32_t)(*count),
				qjs_wrap_element(ctx, (dom_element *)node));
		(*count)++;
	}
	macsurf_dom_node_get_first_child(node, &child);
	while (child != NULL) {
		qjs_collect_by_sel_list(ctx, child, sl, arr, count);
		macsurf_dom_node_get_next_sibling(child, &next);
		macsurf_dom_node_unref(child);
		child = next;
	}
}

/* First match in document order across every alternative. */
static dom_element *qjs_find_first_by_sel_list(dom_node *node,
		const struct qjs_sel_list *sl)
{
	dom_node *child = NULL;
	dom_node *next  = NULL;
	dom_element *found = NULL;

	if (node == NULL) return NULL;
	if (qjs_sel_list_match(node, sl)) {
		macsurf_dom_node_ref(node);
		return (dom_element *)node;
	}
	macsurf_dom_node_get_first_child(node, &child);
	while (child != NULL) {
		found = qjs_find_first_by_sel_list(child, sl);
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
	dom_element *root = NULL;
	JSValue arr;
	int count = 0;

	/* fixes886 - `char tag_lc[64]; int i;` used to be declared here, left
	 * behind when fixes871 removed the '#id' fast path that used them. They
	 * were kept alive only by a `(void)tag_lc; (void)i;` below, and reading an
	 * uninitialised int that way is what produced CW8's
	 *     Warning: variable 'i' is not initialized before being used
	 * Removed rather than silenced: a dead local kept quiet by a (void) cast is
	 * a warning suppressor with no upside. */
	(void)this_val;
	arr = JS_NewArray(ctx);
	if (g_qjs_document == NULL || argc < 1) return arr;
	sel = JS_ToCString(ctx, argv[0]);
	if (sel == NULL) return arr;

	/* fixes871 (#298) - the fixes864 '#id' fast path that used to sit here is
	 * gone: it did strchr(sel,'#') ANYWHERE in the selector, so `#a .b` returned
	 * #a rather than the .b inside it. Ids are just another compound qualifier
	 * to the matcher below, which gets `#a .b`, `div#a`, and `.x#a` all right.
	 * (qs keeps a getElementById fast path because a single-id lookup there is
	 * O(1) and by far the most common call; a qsa returning a 0-or-1 array does
	 * not justify a second, subtly-different code path.)
	 *
	 * fixes871 (#298) - real compound matching (tag/.class/#id/descendant).
	 * The old code extracted only the tag, so `.comment-form__verbum` (Preact's
	 * Verbum mount) returned EMPTY and `div.foo` matched every div. */
	{
		struct qjs_sel_list s;
		qjs_sel_list_parse(sel, &s);
		if (s.approx) {
			macsurf_debug_log_writef(
				"WORK qsa: APPROX selector (unsupported syntax "
				"ignored) sel=%s", sel);
		}
		JS_FreeCString(ctx, sel);
		if (s.n == 0) return arr;

		macsurf_dom_document_get_document_element(g_qjs_document, &root);
		if (root != NULL) {
			qjs_collect_by_sel_list(ctx, (dom_node *)root, &s, arr, &count);
			macsurf_dom_node_unref((dom_node *)root);
		}
	}

	return arr;
}

/* fixes691 (#210): first-match-only walker. Pre-order DFS that returns the
 * FIRST matching element (with one ref held for the caller to hand to
 * qjs_wrap_element, which takes ownership) instead of collecting+wrapping the
 * whole matching set. Same document order as qjs_collect_by_tag, so it returns
 * exactly what qsa[0] would have - without the O(n) walk and the expensive
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
	dom_element *root = NULL;
	dom_element *found = NULL;

	/* fixes1242 (#167) - `char tag_lc[64]; int i;` removed: dead locals,
	 * same class of leftover fixes886 already found and removed from the
	 * sibling qjs_querySelectorAll (see its comment above) -- neither is
	 * referenced anywhere in this function's body, and an uninitialised
	 * `i` read this way is exactly what produced CW8's
	 *     Warning: variable 'i' is not initialized before being used
	 * there. Noticed only because this edit already touches the function;
	 * unrelated to the comma-list change itself. */
	(void)this_val;
	if (g_qjs_document == NULL || argc < 1) return JS_NULL;
	sel = JS_ToCString(ctx, argv[0]);
	if (sel == NULL) return JS_NULL;

	/* fixes864 (#290) - '#id' had no branch at all, so "#commentform" fell
	 * through as a TAG NAME, matched nothing, and returned null while
	 * getElementById('commentform') found it fine.  Silent null, no throw --
	 * exactly how hackaday's reply box dies:
	 *     var e = document.querySelector("#commentform"); if (e) { ...all of it... }
	 * a null `e` skips the whole chain (IntersectionObserver -> loadScript ->
	 * fetch -> injected <script>) without a single error line.
	 *
	 * fixes871 (#298) - that branch did `strchr(sel, '#')` ANYWHERE in the
	 * selector, so `#a .b` ("the .b inside #a") returned #a: the wrong element,
	 * confidently. Now the fast path is taken only when the parsed selector
	 * really is a single id-bearing compound, and the result is still run
	 * through the full compound match so a tag/class qualifier can reject it.
	 *
	 * fixes1242 (#167) - comma-list aware. The #id fast path only applies
	 * when the WHOLE selector is one alternative that is itself a single
	 * id-bearing compound (`'#a'`, `'div#a'`) -- `'#a, #b'` has two
	 * alternatives and must fall through to the general walk so both are
	 * considered. */
	{
		struct qjs_sel_list s;
		qjs_sel_list_parse(sel, &s);
		if (s.approx) {
			macsurf_debug_log_writef(
				"WORK qs: APPROX selector (unsupported syntax "
				"ignored) sel=%s", sel);
		}
		JS_FreeCString(ctx, sel);
		if (s.n == 0) return JS_NULL;

		if (s.n == 1 && s.alt[0].n == 1 && s.alt[0].c[0].id[0] != '\0') {
			dom_string *id_ds = qjs_make_domstr(s.alt[0].c[0].id);
			dom_element *el = NULL;
			if (id_ds == NULL) return JS_NULL;
			macsurf_dom_document_get_element_by_id(g_qjs_document,
					id_ds, &el);
			macsurf_dom_string_unref(id_ds);
			if (el == NULL) return JS_NULL;
			if (!qjs_compound_match((dom_node *)el, &s.alt[0].c[0])) {
				macsurf_dom_node_unref((dom_node *)el);
				return JS_NULL;
			}
			/* get_element_by_id hands back a ref; wrap takes it. */
			return qjs_wrap_element(ctx, el);
		}

		macsurf_dom_document_get_document_element(g_qjs_document, &root);
		if (root == NULL) return JS_NULL;
		found = qjs_find_first_by_sel_list((dom_node *)root, &s);
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

/* ====================================================================== */
/* R1.2 -- the WANT probe: every miss on the global object, in one place  */
/* ====================================================================== */
/* A prototype-level probe above the realm global: 'X' in window, typeof  */
/* X, window.X and a bare X reference all funnel through the exotic       */
/* has/get handlers below whenever the global itself has no such own      */
/* property.  The name is logged once per page (deduped), split into      */
/* WANT (capitalised = constructor/Web-API-looking names) and WANTLOW     */
/* (the rest), minus the UMD/module denylist.  This is the                */
/* implement-next list, written by real sites instead of by hand.         */
/*                                                                        */
/* QuickJS semantics note: an exotic's has_property/get_property          */
/* SHORT-CIRCUIT the engine's prototype walk (quickjs.h: "The following   */
/* methods can be emulated with the previous ones, so they are usually    */
/* not needed" -- only Proxy implements them upstream).  Returning 0 /    */
/* undefined unconditionally would therefore cut the global's chain off   */
/* BEFORE Object.prototype: window.hasOwnProperty, window.toString,       */
/* 'x' in window would all answer lies.  The handlers instead log, then   */
/* continue the lookup on the probe's OWN prototype (the global's former  */
/* prototype) and return that answer, so the chain stays intact.          */
/*                                                                        */
/* The one divergence this causes: a BARE reference to a missing global   */
/* (no typeof, no window.X) returns undefined instead of throwing         */
/* ReferenceError -- the exotic's get_property return bypasses the        */
/* engine's throw_ref_error check, and the handler cannot tell a bare     */
/* reference from a window.X read.  Verified against a no-probe baseline  */
/* in the harness: typeof/in/window.X are unchanged; only the             */
/* try/catch-bare-read pattern of a missing name loses its throw.         */
/* ====================================================================== */

#define QJS_WANT_BUCKETS 64

struct qjs_want_ent {
	struct qjs_want_ent *next;
	char *name;
};

static struct qjs_want_ent *g_want_set[QJS_WANT_BUCKETS];
static JSClassID g_want_class_id = 0;

/* UMD/module-system probes that will never be implemented; do not log. */
static const char *const g_want_deny[] = {
	"define", "module", "exports", "require", "global", "process"
};

static JSValue want_get_property(JSContext *ctx, JSValueConst obj,
		JSAtom atom, JSValueConst receiver);
static int want_has_property(JSContext *ctx, JSValueConst obj, JSAtom atom);

static JSClassExoticMethods g_want_exotic = {
	NULL, NULL, NULL, NULL,          /* own-property trio + define */
	want_has_property,               /* has_property */
	want_get_property,               /* get_property */
	NULL                             /* set_property */
};

static JSClassDef g_want_class = {
	"MacSurfWantProbe",              /* class_name */
	NULL, NULL, NULL,                /* finalizer, gc_mark, call */
	&g_want_exotic                   /* exotic */
};

static unsigned qjs_want_hash(const char *s)
{
	unsigned h = 5381;
	while (*s != '\0') {
		h = (h << 5) + h + (unsigned)(unsigned char)*s;
		s++;
	}
	return h & (QJS_WANT_BUCKETS - 1);
}

/* First occurrence of a name per page is logged, the rest are silent.
 * Returns 1 if a line was written, 0 if filtered or already seen. */
static int qjs_want_report(const char *name)
{
	unsigned h, i;
	struct qjs_want_ent *e, *n;

	for (i = 0; i < (sizeof g_want_deny / sizeof g_want_deny[0]); i++) {
		if (strcmp(name, g_want_deny[i]) == 0) return 0;
	}
	h = qjs_want_hash(name);
	for (e = g_want_set[h]; e != NULL; e = e->next) {
		if (strcmp(e->name, name) == 0) return 0;
	}
	n = (struct qjs_want_ent *)malloc(sizeof *n);
	if (n == NULL) return 0;
	n->name = strdup(name);
	if (n->name == NULL) {
		free(n);
		return 0;
	}
	n->next = g_want_set[h];
	g_want_set[h] = n;
	/* Capitalised = constructor/Web-API-looking -> WANT, else WANTLOW.
	 * The URL slot is empty for now (no document URL handy in the
	 * handler); the per-page reset is what groups a page's entries. */
	if (name[0] >= 'A' && name[0] <= 'Z') {
		macsurf_debug_log_writef("LIFE WANT %s []", name);
	} else {
		macsurf_debug_log_writef("LIFE WANTLOW %s []", name);
	}
	return 1;
}

/* Clear the per-page dedupe set.  Called from macsurf_qjs_audit_reset()
 * (each navigation/realm build) so every page gets its own first-use log. */
void qjs_want_reset(void)
{
	unsigned i;

	for (i = 0; i < QJS_WANT_BUCKETS; i++) {
		struct qjs_want_ent *e = g_want_set[i];
		while (e != NULL) {
			struct qjs_want_ent *next = e->next;
			free(e->name);
			free(e);
			e = next;
		}
		g_want_set[i] = NULL;
	}
}

static int want_has_property(JSContext *ctx, JSValueConst obj, JSAtom atom)
{
	const char *name;
	int ret;
	JSValue proto;

	name = JS_AtomToCString(ctx, atom);
	if (name != NULL) {
		qjs_want_report(name);
		JS_FreeCString(ctx, name);
	}
	/* Answer from the probe's own prototype (the global's former
	 * prototype) so Object.prototype members stay reachable. */
	proto = JS_GetPrototype(ctx, obj);
	ret = JS_HasProperty(ctx, proto, atom);
	JS_FreeValue(ctx, proto);
	return ret;
}

static JSValue want_get_property(JSContext *ctx, JSValueConst obj,
		JSAtom atom, JSValueConst receiver)
{
	const char *name;
	JSValue proto;
	JSValue v;

	(void)receiver;
	name = JS_AtomToCString(ctx, atom);
	if (name != NULL) {
		qjs_want_report(name);
		JS_FreeCString(ctx, name);
	}
	/* Same as want_has_property: continue the walk on the probe's own
	 * prototype so the chain above the probe stays intact. */
	proto = JS_GetPrototype(ctx, obj);
	v = JS_GetProperty(ctx, proto, atom);
	JS_FreeValue(ctx, proto);
	return v;
}

/* Give the realm global a probe prototype.  Called once per realm from
 * register_browser_globals AFTER every shim block has run, so the census
 * only ever sees what page scripts ask for, never the `typeof g.X` checks
 * our own setup ran. */
static void qjs_install_want_probe(JSContext *ctx)
{
	JSRuntime *rt;
	JSValue global;
	JSValue old_proto;
	JSValue exotic;

	rt = JS_GetRuntime(ctx);
	if (g_want_class_id == 0) {
		JS_NewClassID(rt, &g_want_class_id);
	}
	/* Per-runtime registration (classes live in the runtime's class
	 * array, exactly like qjs_dom_init_class).  Re-registering the same
	 * runtime is a benign error. */
	JS_NewClass(rt, g_want_class_id, &g_want_class);

	global = JS_GetGlobalObject(ctx);
	old_proto = JS_GetPrototype(ctx, global);
	exotic = JS_NewObjectProtoClass(ctx, old_proto, g_want_class_id);
	if (!JS_IsException(exotic)) {
		JS_SetPrototype(ctx, global, exotic);
		/* Gotcha #4 -- installing a prototype above the global changes
		 * Object.getPrototypeOf(globalThis); this line is the marker
		 * that the probe (and that change) is live. */
		macsurf_debug_log_writef("LIFE WANT probe installed");
	}
	JS_FreeValue(ctx, global);
	JS_FreeValue(ctx, old_proto);
	JS_FreeValue(ctx, exotic);
}

/* ---- Wire getElementById/querySelectorAll on the document object ---- */
/* ================================================================== */
/* register_browser_globals - installs the browser runtime globals    */
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
/* ---- fixes872 (#300) - the element PROTOTYPE, carrying the on* handlers ----
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
/* ---- fixesXXXX (#211): the heavyweight element surface, ONCE per realm ----
 *
 * Everything qjs_el_install_native_attrs used to install per wrapper -- the
 * event bridge, the attribute/textContent/innerHTML backends, the mutation
 * and query natives, and the JS helper surface (classList, style proxy,
 * dataset proxy, matches, closest, ...) -- now lands on the wrapper CLASS
 * proto p (owned by JS_SetClassProto) and, for the node-level traversal
 * surface, on Node.prototype. Every wrapper reaches them through its per-tag
 * constructor prototype chain, and the C functions read the element from
 * `this` (qjs_get_node(this_val)) instead of per-instance func_data
 * closures -- zero closures per node on this path.
 *
 * ORDERING CONSTRAINT, preserved from the old per-element install:
 *   1. natives FIRST (the JS helper string must not shadow them),
 *   2. then the JS helper string (its zero stubs are fine, they are
 *      overridden by...),
 *   3. then tc_src,
 *   4. then the REAL metrics (which must beat the zero stubs the JS string
 *      defines -- the ordering is the only thing that makes them win),
 *   5. then lay_src,
 *   6. then the node surface (Node.prototype + CharacterData.prototype),
 *      last so nothing above can clobber it.
 *
 * Called from qjs_el_install_proto right after JS_SetClassProto, which runs
 * once per realm (second call bails) before anything is wrapped, so every
 * wrapper -- elements, text/comment/CDATA, fragments -- is born into a
 * complete surface.
 */
static void qjs_el_install_proto_surface(JSContext *ctx, JSValue proto)
{
	JSValue f;
	JSValue g;
	JSValue node_proto;
	JSValue cd_proto;
	JSValue fn2;
	size_t i;

	/* fixes989 — the event bridge. Installed BEFORE the JS helper string is
	 * evaluated at the end of this function, and deliberately not defined
	 * there, so nothing shadows them. */
	f = JS_NewCFunctionData(ctx, qjs_el_add_event_listener_data, 3, 0, 0, NULL);
	JS_SetPropertyStr(ctx, proto, "addEventListener", f);
	f = JS_NewCFunctionData(ctx, qjs_el_remove_event_listener_data, 2, 0, 0, NULL);
	JS_SetPropertyStr(ctx, proto, "removeEventListener", f);
	/* fixes1008 (1e) — native dispatchEvent, so synthetic events bubble.
	 * Installed BEFORE the JS helper string is evaluated; that string now
	 * defines __msFireLocal instead of dispatchEvent, so nothing shadows
	 * this. */
	f = JS_NewCFunctionData(ctx, qjs_el_dispatch_event_data, 1, 0, 0, NULL);
	JS_SetPropertyStr(ctx, proto, "dispatchEvent", f);
	/* fixes996 — the on* setter (a JS accessor) calls this to register. */
	f = JS_NewCFunctionData(ctx, qjs_el_reg_event_data, 1, 0, 0, NULL);
	JS_SetPropertyStr(ctx, proto, "__msRegEvent", f);

	/* Core DOM methods */
	f = JS_NewCFunctionData(ctx, qjs_el_getAttribute_data, 1, 0, 0, NULL);
	JS_SetPropertyStr(ctx, proto, "getAttribute", f);
	f = JS_NewCFunctionData(ctx, qjs_el_setAttribute_data, 2, 0, 0, NULL);
	JS_SetPropertyStr(ctx, proto, "setAttribute", f);
	f = JS_NewCFunctionData(ctx, qjs_el_remove_attribute_data, 1, 0, 0, NULL);
	JS_SetPropertyStr(ctx, proto, "removeAttribute", f);
	f = JS_NewCFunctionData(ctx, qjs_el_has_attribute_data, 1, 0, 0, NULL);
	JS_SetPropertyStr(ctx, proto, "hasAttribute", f);

	/* textContent */
	f = JS_NewCFunctionData(ctx, qjs_el_get_text_content_data, 0, 0, 0, NULL);
	JS_SetPropertyStr(ctx, proto, "__getTextContent", f);
	f = JS_NewCFunctionData(ctx, qjs_el_set_text_content_data, 1, 0, 0, NULL);
	JS_SetPropertyStr(ctx, proto, "__setTextContent", f);
	/* fixes846 (#167 S3) — real innerHTML= via HTML fragment parsing. */
	f = JS_NewCFunctionData(ctx, qjs_el_set_inner_html_data, 1, 0, 0, NULL);
	JS_SetPropertyStr(ctx, proto, "__setInnerHTML", f);
	/* fixes1168 (#262) — real innerHTML read-back: the JS getter calls this
	 * to serialize the child tree to markup (see qjs_el_get_inner_html_data). */
	f = JS_NewCFunctionData(ctx, qjs_el_get_inner_html_data, 0, 0, 0, NULL);
	JS_SetPropertyStr(ctx, proto, "__getInnerHTML", f);
	/* fixes1168 (#262) — real outerHTML read-back: same serializer over the
	 * element itself (tag + attributes + children). */
	f = JS_NewCFunctionData(ctx, qjs_el_get_outer_html_data, 0, 0, 0, NULL);
	JS_SetPropertyStr(ctx, proto, "__getOuterHTML", f);

	/* Traversal */
	f = JS_NewCFunctionData(ctx, qjs_el_get_parent_node_data, 0, 0, 0, NULL);
	JS_SetPropertyStr(ctx, proto, "__getParentNode", f);
	f = JS_NewCFunctionData(ctx, qjs_el_get_next_sibling_data, 0, 0, 0, NULL);
	JS_SetPropertyStr(ctx, proto, "__getNextElementSibling", f);
	f = JS_NewCFunctionData(ctx, qjs_el_get_prev_sibling_data, 0, 0, 0, NULL);
	JS_SetPropertyStr(ctx, proto, "__getPreviousElementSibling", f);
	f = JS_NewCFunctionData(ctx, qjs_el_get_children_data, 0, 0, 0, NULL);
	JS_SetPropertyStr(ctx, proto, "__getChildren", f);

	/* Mutation */
	f = JS_NewCFunctionData(ctx, qjs_el_append_child_data, 1, 0, 0, NULL);
	JS_SetPropertyStr(ctx, proto, "appendChild", f);
	f = JS_NewCFunctionData(ctx, qjs_el_remove_child_data, 1, 0, 0, NULL);
	JS_SetPropertyStr(ctx, proto, "removeChild", f);
	f = JS_NewCFunctionData(ctx, qjs_el_insert_before_data, 2, 0, 0, NULL);
	JS_SetPropertyStr(ctx, proto, "insertBefore", f);

	/* Scoped query */
	f = JS_NewCFunctionData(ctx, qjs_el_qsa_data, 1, 0, 0, NULL);
	JS_SetPropertyStr(ctx, proto, "querySelectorAll", f);
	f = JS_NewCFunctionData(ctx, qjs_el_qs_data, 1, 0, 0, NULL);
	JS_SetPropertyStr(ctx, proto, "querySelector", f);

	/* JS-side helpers: classList, style, dataset, matches, closest, etc. */
	qjs_el_install_js_helpers(ctx, proto);

	/* Wire textContent as a property using the C getter/setter helpers */
	{
		static const char *tc_src =
			"(function(){var P=this;"
			"Object.defineProperty(P,'textContent',{"
			"get:function(){return this.__getTextContent();},"
			"set:function(v){this.__setTextContent(String(v));},"
			"configurable:true});"
			"Object.defineProperty(P,'parentNode',{"
			"get:function(){return this.__getParentNode();},"
			"configurable:true});"
			"Object.defineProperty(P,'nextElementSibling',{"
			"get:function(){return this.__getNextElementSibling();},"
			"configurable:true});"
			"Object.defineProperty(P,'previousElementSibling',{"
			"get:function(){return this.__getPreviousElementSibling();},"
			"configurable:true});"
			"Object.defineProperty(P,'children',{"
			"get:function(){return this.__getChildren();},"
			"configurable:true});"
			/* fixes1031 — firstElementChild / lastElementChild /
			 * childElementCount. nextElementSibling and
			 * previousElementSibling were here; their two siblings were
			 * not, and jQuery's wrapAll walks firstElementChild:
			 *
			 *   wrap.map(function(){ var e=this;
			 *       while (e.firstElementChild) e=e.firstElementChild;
			 *       return e; }).append(this);
			 *
			 * so wrapInner() -- and therefore the dotdotdot truncation
			 * plugin hackaday runs over every article entry -- lost the
			 * content it was re-parenting. Derived from `children` so
			 * they cannot disagree with it. */
			"Object.defineProperty(P,'firstElementChild',{"
			"get:function(){var c=this.__getChildren();"
				"return (c&&c.length)?c[0]:null;},"
			"configurable:true});"
			"Object.defineProperty(P,'lastElementChild',{"
			"get:function(){var c=this.__getChildren();"
				"return (c&&c.length)?c[c.length-1]:null;},"
			"configurable:true});"
			"Object.defineProperty(P,'childElementCount',{"
			"get:function(){var c=this.__getChildren();"
				"return (c&&c.length)?c.length:0;},"
			"configurable:true});"
			"})";
		fn2 = qjs_helper_fn(ctx, "__ms_h_props", tc_src, "<el-props>");
		if (!JS_IsException(fn2)) {
			JS_Call(ctx, fn2, proto, 0, NULL);
		}
		JS_FreeValue(ctx, fn2);
	}

	/* fixes1011 (Phase 3) — REAL layout metrics, replacing the zero stubs.
	 *
	 * Installed AFTER the JS helper string, which defines the zero-returning
	 * getBoundingClientRect -- these must win, and the ordering is the only
	 * thing that makes them win. */
	g = JS_NewCFunctionData(ctx, qjs_el_get_rect, 0, 0, 0, NULL);
	JS_SetPropertyStr(ctx, proto, "getBoundingClientRect", g);

	/* Each metric is a getter, because page code reads them as
	 * properties (el.offsetWidth), never as calls. */
	{
		static const struct { const char *name; int magic; } mm[] = {
			{ "offsetWidth",  QJS_M_OFFW },
			{ "offsetHeight", QJS_M_OFFH },
			{ "clientWidth",  QJS_M_CLIW },
			{ "clientHeight", QJS_M_CLIH },
			{ "offsetTop",    QJS_M_OFFT },
			{ "offsetLeft",   QJS_M_OFFL },
			{ "scrollWidth",  QJS_M_SCRW },
			{ "scrollHeight", QJS_M_SCRH }
		};
		JSAtom a;
		JSValue fn;
		for (i = 0; i < sizeof mm / sizeof mm[0]; i++) {
			a = JS_NewAtom(ctx, mm[i].name);
			fn = JS_NewCFunctionData(ctx, qjs_el_metric,
					0, mm[i].magic, 0, NULL);
			JS_DefinePropertyGetSet(ctx, proto, a, fn, JS_UNDEFINED,
					JS_PROP_CONFIGURABLE);
			JS_FreeAtom(ctx, a);
		}
	}
	{
		/* getClientRects wraps the single rect; scrollTop/Left are the
		 * window's scroll for now (per-element scrollers are not modelled),
		 * and the SETTER really moves the view rather than recording a
		 * number, which is what a "scroll to top" button needs. */
		static const char *lay_src =
			"(function(){var P=this;"
			"P.getClientRects=function(){"
				"return [this.getBoundingClientRect()];};"
			"Object.defineProperty(P,'scrollTop',{configurable:true,"
				"get:function(){return (typeof window!=='undefined')?"
					"(window.scrollY||0):0;},"
				"set:function(v){if(typeof window!=='undefined'&&"
					"window.scrollTo)window.scrollTo(window.scrollX||0,v|0);}});"
			"Object.defineProperty(P,'scrollLeft',{configurable:true,"
				"get:function(){return (typeof window!=='undefined')?"
					"(window.scrollX||0):0;},"
				"set:function(v){if(typeof window!=='undefined'&&"
					"window.scrollTo)window.scrollTo(v|0,window.scrollY||0);}});"
			"Object.defineProperty(P,'offsetParent',{configurable:true,"
				"get:function(){var p=this.parentNode;"
				"while(p&&p.nodeType===1){"
					"if(p===document.body)return p;p=p.parentNode;}"
				"return (typeof document!=='undefined')?document.body:null;}});"
			"P.scrollIntoView=function(){"
				"var r=this.getBoundingClientRect();"
				"if(typeof window!=='undefined'&&window.scrollTo)"
					"window.scrollTo(window.scrollX||0,"
						"(window.scrollY||0)+r.top);};"
			"})";
		fn2 = qjs_helper_fn(ctx, "__ms_h_lay", lay_src, "<el-layout>");
		if (!JS_IsException(fn2)) {
			JS_Call(ctx, fn2, proto, 0, NULL);
		}
		JS_FreeValue(ctx, fn2);
	}

	/* fixes878/#211 — the node-level surface: elements, text/comment AND
	 * fragments all reach Node.prototype through the fixes1127 family chain,
	 * so a single install covers every wrapper shape. Elements still answer
	 * parentNode/textContent from p above (closer in the chain, same
	 * __get* backends), so only the truly node-level names are redefined
	 * here. */
	node_proto = qjs_ctor_proto_by_name(ctx, "Node");
	if (JS_IsObject(node_proto)) {
		qjs_install_node_traversal(ctx, node_proto);
		JS_FreeValue(ctx, node_proto);
	}

	/* CharacterData surface — nodeValue/data/textContent getter/setter pairs
	 * for text/comment/CDATA wrappers. They share the shape and all reach
	 * CharacterData.prototype through the family chain, so one install with
	 * zero closures covers every one (the old per-instance pairs in
	 * qjs_wrap_text_node were the #211 cost on this path). */
	cd_proto = qjs_ctor_proto_by_name(ctx, "CharacterData");
	if (JS_IsObject(cd_proto)) {
		JSValue getter;
		JSValue setter;
		JSAtom atom;

		getter = JS_NewCFunctionData(ctx, qjs_text_get_data_data,
				0, 0, 0, NULL);
		setter = JS_NewCFunctionData(ctx, qjs_text_set_data_data,
				1, 0, 0, NULL);

		atom = JS_NewAtom(ctx, "nodeValue");
		JS_DefinePropertyGetSet(ctx, cd_proto, atom,
				JS_DupValue(ctx, getter), JS_DupValue(ctx, setter),
				JS_PROP_CONFIGURABLE);
		JS_FreeAtom(ctx, atom);

		atom = JS_NewAtom(ctx, "data");
		JS_DefinePropertyGetSet(ctx, cd_proto, atom,
				JS_DupValue(ctx, getter), JS_DupValue(ctx, setter),
				JS_PROP_CONFIGURABLE);
		JS_FreeAtom(ctx, atom);

		atom = JS_NewAtom(ctx, "textContent");
		JS_DefinePropertyGetSet(ctx, cd_proto, atom, getter, setter,
				JS_PROP_CONFIGURABLE);
		JS_FreeAtom(ctx, atom);

		JS_FreeValue(ctx, cd_proto);
	}
}

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
		"this._H[k]=(typeof v==='function')?v:null;"
		/* fixes996 -- tell libdom this node wants the event, or a real
		 * click never dispatches here and _H is never read. */
		"if(v&&this.__msRegEvent){try{this.__msRegEvent(k);}catch(e){}}"
		"}});"
		"})(n[i]);}"
		/* fixes1127 -- the DOM constructor family chain, so
		 * `el instanceof HTMLElement` / Element / Node and the per-tag
		 * constructors answer truthfully on real wrappers.
		 *
		 * The DOM constructors (HTMLElement, HTMLDivElement, ...) are
		 * empty-function stubs installed by setup_globals; their
		 * .prototype objects are chained to each other there (Element ->
		 * Node, HTMLElement -> Element, Text -> CharacterData -> Node,
		 * DocumentFragment -> Node).  THIS class proto p is the piece
		 * that connects the wrapper world to that family:
		 *   p.__proto__ = HTMLElement.prototype  -- every wrapper whose
		 *     chain includes p (the default for an element wrapper)
		 *     answers instanceof HTMLElement/Element/Node.
		 *   X.prototype.__proto__ = p for each per-tag HTML* constructor
		 *     -- a wrapper whose own proto is HTMLDivElement.prototype
		 *     keeps p (and with it the on* accessors) in its chain while
		 *     also answering instanceof HTMLDivElement.
		 *
		 * Live driver: XenForo core-compiled.js measureScrollBar calls
		 * XF.createElement("div", {className:"scrollMeasure"}, m.body),
		 * which gates its append behind `b instanceof HTMLElement`.
		 * With the family disconnected the gate is false, the probe div
		 * is never appended, and `b.parentNode.removeChild(b)` throws
		 * "cannot read property 'removeChild' of null" -- blocking
		 * XF.Element registration and the editor. */
		"if(typeof HTMLElement!=='undefined'&&HTMLElement.prototype){"
			"try{p.__proto__=HTMLElement.prototype;}catch(e){}}"
		"var ht=['HTMLUnknownElement','HTMLHtmlElement','HTMLHeadElement',"
			"'HTMLBodyElement','HTMLDivElement','HTMLSpanElement',"
			"'HTMLParagraphElement','HTMLAnchorElement','HTMLImageElement',"
			"'HTMLCanvasElement','HTMLInputElement','HTMLButtonElement',"
			"'HTMLTextAreaElement','HTMLSelectElement','HTMLOptionElement',"
			"'HTMLFormElement','HTMLLabelElement','HTMLUListElement',"
			"'HTMLOListElement','HTMLLIElement','HTMLTableElement',"
			"'HTMLTableRowElement','HTMLTableCellElement','HTMLScriptElement',"
			"'HTMLStyleElement','HTMLLinkElement','HTMLMetaElement',"
			"'HTMLIFrameElement','HTMLVideoElement','HTMLAudioElement',"
			"'HTMLMediaElement','HTMLSourceElement','HTMLPictureElement',"
			"'HTMLTemplateElement','HTMLSlotElement','HTMLBRElement',"
			"'HTMLHRElement','HTMLPreElement','HTMLDListElement',"
			"'SVGSVGElement'];"
		"for(i=0;i<ht.length;i++){"
			"var c=(typeof globalThis!=='undefined'?globalThis:this)[ht[i]];"
			"if(c&&c.prototype){try{c.prototype.__proto__=p;}catch(e){}}}"
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

	/* fixesXXXX (#211) — the heavyweight element surface, ONCE per realm.
	 * qjs_el_install_native_attrs used to run this whole install per wrapper
	 * (~80 closures + 2 ES6 Proxies + ~90 defineProperty calls per unique DOM
	 * node); it now lives on the class proto and Node.prototype, installed
	 * exactly once here before anything is wrapped. Timed into wrapus so the
	 * JSCOST audit line shows the one-time cost instead of a per-element one. */
	{
		extern double macos9_micros(void);
		double wt0;
		wt0 = macos9_micros();
		qjs_el_install_proto_surface(ctx, proto);
		g_wrap_us += (long)(macos9_micros() - wt0);
	}
}

static void qjs_dom_install(JSContext *ctx)
{
	JSValue global = JS_GetGlobalObject(ctx);
	JSValue doc    = JS_GetPropertyStr(ctx, global, "document");

	/* fixes872 (#300) - before any element is wrapped in this realm. */
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
		/* fixes870 (#297) - createElementNS: Preact's only element factory. */
		qjs_set_func(ctx, doc, "__createElementNSNative",
				qjs_create_element_ns, 3);
		/* fixes846 (#167 S3) - real createTextNode/createDocumentFragment,
		 * same native-fast-path/JS-fallback shape as createElement above. */
		qjs_set_func(ctx, doc, "__createTextNodeNative",
				qjs_create_text_node, 1);
		qjs_set_func(ctx, doc, "__createDocumentFragmentNative",
				qjs_create_document_fragment, 0);
		/* fixes879 - document.cookie as a REAL accessor pair over the
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
		/* fixes1006 (1b) - make `document` a REAL event target.
		 *
		 * Two halves. First the registration hook the JS shims call, so
		 * document.addEventListener / window.addEventListener reach
		 * dom_event_target_add_event_listener. */
		qjs_set_func(ctx, doc, "__msRegDocEvent", qjs_doc_reg_event, 2);

		/* Second: put the JS `document` object in the wrap table, keyed by
		 * the document dom_node, so qjs_dom_listener_cb's qjs_wrap_lookup
		 * finds it and can call its dispatchEvent.
		 *
		 * DO NOT mint a wrapper for the document via qjs_wrap_element /
		 * qjs_wrap_any_node -- those read through an ELEMENT vtable and a
		 * document is a different, smaller shape; that mismatch was the
		 * fixes846 ASan global-buffer-overflow. Registering the object that
		 * already exists is safe by construction because the callback only
		 * ever calls dispatchEvent on it, never an element-only operation.
		 *
		 * The entry is deliberately WEAK: no JS_DupValue on the value. The
		 * document object is a property of the realm global, so
		 * JS_FreeContext frees it, and drain (which runs AFTER
		 * JS_FreeContext) must never touch that value. It is also the one
		 * entry with no finalizer -- JS_GetOpaque(v, s_el_class_id) is NULL
		 * for it -- so nothing removes it on its own and every table walker
		 * must tolerate a non-element entry.
		 *
		 * The NODE ref is taken, because drain unrefs e->node. owner_doc is
		 * NULL: the document cannot be its own keepalive. */
		if (g_qjs_document != NULL &&
		    qjs_wrap_lookup((dom_node *)g_qjs_document) == NULL) {
			macsurf_dom_node_ref((dom_node *)g_qjs_document);
			if (qjs_wrap_insert((dom_node *)g_qjs_document, NULL,
					doc, JS_GetRuntime(ctx)) == 0) {
				/* malloc failed: give the ref straight back so
				 * the table and the refcount stay consistent. */
				macsurf_dom_node_unref(
					(dom_node *)g_qjs_document);
			}
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
			/* fixes1112 (#265) -- THE FIX, proven on hardware first (fixes1111).
			 *
			 * appendChild/removeChild/insertBefore below used to set
			 * c.parentNode via a PLAIN ASSIGNMENT. On a real element wrapper
			 * parentNode is a getter-only accessor (~6121, no setter), so the
			 * assignment silently no-ops in sloppy mode and a later read of
			 * c.parentNode falls through to the REAL native getter, which
			 * correctly reports the child was never attached to the real DOM
			 * -- because it never was; only mkfb's own `kids` array knew
			 * about it. Hardware, 68kmla.org (fixes1111's probe):
			 * "mkfb.appendChild tag=BODY childtag=div real=1" immediately
			 * preceded the "removeChild of null" throw every single time --
			 * a REAL wrapper div appended into the fake body created before
			 * <body> exists (preamble.min.js runs mid-parse; see the
			 * "reconvert: defer" line at the identical timestamp).
			 *
			 * setPN mirrors the OTHER mock's already-working fix (fixes1002,
			 * ~9084-9089): Object.defineProperty, not assignment, so it
			 * shadows the getter-only accessor on real wrappers instead of
			 * being swallowed by it. Also correct for a fake child (another
			 * mkfb element, which has no accessor to fight -- defineProperty
			 * behaves like a normal set there). */
			"function setPN(c,v){if(!c)return;try{"
			"Object.defineProperty(c,'parentNode',"
			"{value:v,writable:true,configurable:true});"
			"}catch(e){c.parentNode=v;}}"
			/* fixes1113 (#265) -- the SECOND throw right behind fixes1112's
			 * fix, hardware-confirmed same session: "cannot read property
			 * 'fake' of undefined" at preamble.min.js's cleanup helper `c()`,
			 * which unconditionally reads `f.body.dataset.fake`. mkfb had no
			 * `dataset` at all -- real wrapper elements get one via a
			 * getter/proxy (~3680), but nothing gave mkfb's fake elements
			 * the property real code assumes every element has. A plain
			 * object is correct here (not the getter/proxy machinery real
			 * elements need): mkfb elements are short-lived JS-only mocks,
			 * nothing else in this file reads or writes their dataset, and
			 * `dataset.fake` reading `undefined` (falsy) instead of throwing
			 * is exactly right -- the fake-body-creation branch that would
			 * have set dataset.fake='true' never runs anyway (document.body
			 * already resolves truthy via mkfb itself, short-circuiting
			 * that branch, same shape as the fixes1112 root cause). */
			"var el={nodeType:1,tagName:(tag||'div').toUpperCase(),"
			"clientWidth:vw,clientHeight:vh,offsetWidth:vw,offsetHeight:vh,"
			"scrollWidth:vw,scrollHeight:vh,scrollTop:0,scrollLeft:0,"
			"offsetTop:0,offsetLeft:0,style:{},parentNode:null,dataset:{},"
			"childNodes:kids,firstChild:null,lastChild:null,"
			"getAttribute:function(n){return attrs[n]!==undefined?attrs[n]:null;},"
			"setAttribute:function(n,v){attrs[n]=String(v);},"
			"removeAttribute:function(n){delete attrs[n];},"
			"hasAttribute:function(n){return attrs[n]!==undefined;},"
			"appendChild:function(c){if(c){setPN(c,this);kids.push(c);"
			"this.firstChild=kids[0];this.lastChild=kids[kids.length-1];}return c;},"
			"removeChild:function(c){var i=kids.indexOf(c);if(i>=0){kids.splice(i,1);"
			"setPN(c,null);this.firstChild=kids[0]||null;"
			"this.lastChild=kids[kids.length-1]||null;}return c;},"
			"insertBefore:function(c,r){var i=kids.indexOf(r);"
			"if(i<0)i=kids.length;kids.splice(i,0,c);setPN(c,this);"
			"this.firstChild=kids[0]||null;this.lastChild=kids[kids.length-1]||null;return c;},"
			"contains:function(){return false;},"
			"addEventListener:function(){},removeEventListener:function(){},"
			"dispatchEvent:function(){return true;},"
			"querySelector:function(){return null;},querySelectorAll:function(){return[];},"
			"getElementsByClassName:function(){return[];},getElementsByTagName:function(){return[];},"
			"getBoundingClientRect:function(){return{top:0,left:0,right:vw,bottom:vh,width:vw,height:vh,x:0,y:0};}};"
			"el.className='';"
			"Object.defineProperty(el,'classList',{get:(function(){var c=cls(el);return function(){return c;};})(),configurable:true});"
			/* fixes1127 -- fake elements must pass the SAME instanceof gates
			 * real wrappers now do, or XF.createElement's
			 * `b instanceof HTMLElement && b.appendChild(f)` skips the append
			 * for a pre-body document.body -- the measureScrollBar
			 * removeChild-of-null throw fixes1112 fixed for the direct-append
			 * path, hit through a different gate this time.  setPrototypeOf
			 * (not __proto__ assignment) so the per-tag prototype's chain
			 * (routed through the wrapper class proto by qjs_el_install_proto)
			 * is what the fake inherits; own members are unaffected. */
			"if(typeof HTMLElement!=='undefined'){"
			"var _ct={};_ct.body=HTMLBodyElement;_ct.html=HTMLHtmlElement;"
			"_ct.head=HTMLHeadElement;_ct.div=HTMLDivElement;"
			"_ct['#fragment']=DocumentFragment;"
			"var _cp=(tag==='#fragment')?DocumentFragment:"
			"(_ct[String(tag||'').toLowerCase()]||HTMLUnknownElement);"
			"if(_cp&&_cp.prototype){try{Object.setPrototypeOf(el,_cp.prototype);}catch(e){}}}"
			"return el;}"
			"d.createElement=function(tag){"
			"var n=d.__createElementNative?d.__createElementNative(tag):null;"
			"if(n)return n;return mkfb(tag);};"
			/* fixes870 (#297) - createElementNS, Preact's only element factory.
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
			"if(!_fbBody){try{__msLife('document.body fell back to mkfb "
			"(no real body yet)');}catch(e){}_fbBody=mkfb('body');}"
			"return _fbBody;}});"
			"Object.defineProperty(d,'head',{configurable:true,"
			"get:function(){var n=d.__getHead();if(n)return n;"
			"if(!_fbHead)_fbHead=mkfb('head');return _fbHead;}});"
			/* (XF-probe round, FormData crash) -- document must answer
			 * "defaultView" (and the IE-era parentWindow) with the global
			 * object.  editor-compiled.js (Froala v4, the 68kmla reply box)
			 * does `this.win = "defaultView" in this.doc ?
			 * this.doc.defaultView : this.doc.parentWindow` then reads
			 * `a.win.FormData` in _init -- with neither getter present,
			 * "defaultView" in doc is false, parentWindow is undefined, and
			 * every editor construction dies with "cannot read property
			 * 'FormData' of undefined" (hw log: LIFE qjs timer exc at
			 * 93248 and 191241, one per nav).  window IS the global object
			 * here (register_browser_globals aliases it), and FormData is
			 * set on the global by the FormData block, so returning window
			 * is exactly what the spec's document.defaultView must be.
			 * (These getters run at CALL time, so `window` -- assigned
			 * later in register_browser_globals -- is always defined.) */
			"Object.defineProperty(d,'defaultView',{configurable:true,"
			"get:function(){return (typeof window!=='undefined')?window:globalThis;}});"
			"Object.defineProperty(d,'parentWindow',{configurable:true,"
			"get:function(){return (typeof window!=='undefined')?window:globalThis;}});"
			/* fixes1131b - XF LazyHandlerLoader is called with the DOCUMENT
			 * (nodeType=9) as its container; documents lack .matches/.closest
			 * (element-only in the DOM spec).  No-ops: a document never matches
			 * a CSS selector and has no ancestor. */
			"Object.defineProperty(d,'matches',{configurable:true,"
			"value:function(){return false;}});"
			"Object.defineProperty(d,'closest',{configurable:true,"
			"value:function(){return null;}});"
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
 * fixes717 (#207 diagnostic) - crypto.getRandomValues / crypto.randomUUID
 *
 * QuickJS ships no `crypto` global, so any script that touches it (uuid
 * libraries, cache-busting, the Cloudflare beacon captured in the
 * SuperLogger log) throws "crypto.getRandomValues() not supported" and
 * aborts the whole script. fixes717 filled them from a clock-seeded
 * xorshift so those scripts RAN instead of crashing (DIRECTIVE #2).
 *
 * fixes1069 - that generator is gone; both entry points now draw from
 * macEntropy, exactly as the fixes717 comment here prescribed ("If a page
 * ever needs real CSPRNG output, back this with macEntropy's pool
 * (OSTLS_*), which is already linked and hardware-verified").
 *
 * The upgrade matters because a weak PRNG behind crypto.* is not a missing
 * feature, it is a WRONG ANSWER - the failure mode this engine has paid for
 * repeatedly. A page minting a session token, a CSRF nonce or a v4 UUID got
 * bytes derived from the tick count and a stack address, with nothing to
 * feature-detect: crypto.getRandomValues was present and answered.
 *
 * macEntropy is the same pool that seeds every TLS handshake - SHA-256
 * based, seeded from OT packet jitter, key/mouse timing and a seed file
 * persisted across launches, with a statistical self-test. OSTLS_RandomBytes
 * (fixes1069) extracts under its own domain-separation tag, so this stream
 * is independent of the TLS seed.
 *
 * Declared locally rather than including macTLS/os9/ostls_entropy.h: that
 * header pulls the BearSSL engine types, and main.c already reaches
 * macEntropy this same way (see OSTLS_StirEntropy there).
 * ==================================================================== */
extern void OSTLS_RandomBytes(void *out, unsigned long len);

static JSValue qjs_crypto_get_random_values(JSContext *ctx,
	JSValueConst this_val, int argc, JSValueConst *argv)
{
	size_t byte_off = 0, byte_len = 0, bpe = 0, ab_size = 0;
	JSValue ab;
	uint8_t *ptr;

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
	OSTLS_RandomBytes(ptr + byte_off, (unsigned long) byte_len);
	JS_FreeValue(ctx, ab);
	return JS_DupValue(ctx, argv[0]);   /* spec: returns the same array */
}

/* fixes1015 - __msLife: a LIFE-prefixed log line callable from the JS shims.
 * console.error's WORK routing is compiled out of shipping builds, which is
 * exactly how earlier shim diagnostics went dark. This one survives the
 * failures-only gate by construction. Budgeted. */
static JSValue qjs_ms_life(JSContext *ctx,
	JSValueConst this_val, int argc, JSValueConst *argv)
{
	const char *s;
	(void)this_val;
	if (g_mslife_audit <= 0 || argc < 1) return JS_UNDEFINED;
	s = JS_ToCString(ctx, argv[0]);
	if (s != NULL) {
		char vb[160];
		int i = 0;
		while (i < (int)sizeof(vb) - 1 && s[i] != '\0') {
			vb[i] = (s[i] == '\r' || s[i] == '\n') ? ' ' : s[i];
			i++;
		}
		vb[i] = '\0';
		g_mslife_audit--;
		macsurf_debug_log_writef("LIFE js %s", vb);
		JS_FreeCString(ctx, s);
	}
	return JS_UNDEFINED;
}

/* fixes1236 (#167) - __msRafFired: called from INSIDE our own
 * requestAnimationFrame implementation at fire time (not registration time),
 * so g_raf_fires only counts callbacks that actually ran. See the counter's
 * declaration for why this is separate from the timer-fire count. */
static JSValue qjs_raf_fired(JSContext *ctx,
	JSValueConst this_val, int argc, JSValueConst *argv)
{
	(void)ctx; (void)this_val; (void)argc; (void)argv;
	g_raf_fires++;
	return JS_UNDEFINED;
}

static JSValue qjs_crypto_random_uuid(JSContext *ctx,
	JSValueConst this_val, int argc, JSValueConst *argv)
{
	static const char hex[] = "0123456789abcdef";
	unsigned char b[16];
	char out[37];
	int i, p;

	(void)this_val; (void)argc; (void)argv;
	OSTLS_RandomBytes(b, 16UL);
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

/* fixes843b (#167 S1 census) - native-side visibility into the fetch()
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
	macsurf_debug_log_writef("LIFE fetch url=%s ok=%d status=%d",
			url ? url : "(null)", ok, (int)status);
	if (url) JS_FreeCString(ctx, url);
	return JS_UNDEFINED;
}

/* fixes845 (#167 S1 census cont'd) - a census round that only instruments
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
	macsurf_debug_log_writef("LIFE xhr event=%s method=%s url=%s",
			event ? event : "?", method ? method : "",
			url ? url : "");
	if (event) JS_FreeCString(ctx, event);
	if (method) JS_FreeCString(ctx, method);
	if (url) JS_FreeCString(ctx, url);
	return JS_UNDEFINED;
}

/* ------------------------------------------------------------------ */
/* localStorage / sessionStorage persistence                            */
/* ------------------------------------------------------------------ */

/* localStorage/sessionStorage used to be pure in-memory JS objects, lost
 * on every navigation (the realm is rebuilt per page load). localStorage
 * now persists per origin to MacSurfData/LocalStorage/<l_<origin>_<hash>
 * .json, one file per origin, holding the JSON key-value map. The _Storage
 * shim (further down) calls __storageLoad when the realm is built and
 * __storageSave after every setItem/removeItem/clear. sessionStorage
 * deliberately stays in-memory: per spec it is per-tab, and with one
 * realm per navigation it already behaves as a fresh session on each
 * page load.
 *
 * Non-Mac builds (Linux harness / syntax check) compile the file I/O out:
 * __storageLoad returns null and __storageSave is a no-op, so the harness
 * keeps the old in-memory behaviour exactly. */

#define MACSURF_STORAGE_MAX_BYTES (1024L * 1024L)
#define MACSURF_STORAGE_ORIGIN_MAX 15   /* HFS filenames cap at 31 chars */

/* FNV-1a, same family as the disk cache's URL hash. */
static unsigned long
macsurf_storage_hash(const char *s)
{
	unsigned long h = 2166136261UL;
	while (*s != '\0') {
		h ^= (unsigned char) *s;
		h *= 16777619UL;
		s++;
	}
	return h;
}

/* Build the HFS-safe per-origin filename for a page URL. The origin is
 * scheme://host[:non-default-port]; characters that are not filename-safe
 * are replaced, the name part is capped at MACSURF_STORAGE_ORIGIN_MAX
 * chars, and an 8-hex hash suffix keeps distinct origins from colliding
 * (the disk cache uses the same hash-for-filename pattern). Result is a
 * Str63-style Pascal string: fname[0] = length, fname[1..] = bytes. */
static void
macsurf_storage_fname(const char *url, unsigned char *fname)
{
	const char *hex = "0123456789abcdef";
	const char *p;
	const char *sep;
	char tmp[31];
	int i = 2;
	int j;
	unsigned long h;

	tmp[0] = 'l';
	tmp[1] = '_';
	if (url != NULL) {
		p = url;
		if (strncmp(p, "https://", 8) == 0) {
			p += 8;
		} else if (strncmp(p, "http://", 7) == 0) {
			p += 7;
		} else {
			/* A URL with no "://" at all (e.g. the "about:blank"
			 * placeholder a nested realm can still carry mid-navigation)
			 * left p NULL here -- strstr's NULL result was assigned into
			 * p unconditionally, then the walk below dereferenced it.
			 * Keep p at the original url in that case instead. */
			sep = strstr(p, "://");
			if (sep != NULL) p = sep + 3;
		}
		while (*p != '\0' && *p != '/' && *p != '?' && *p != '#' &&
		       i < MACSURF_STORAGE_ORIGIN_MAX + 2) {
			char c = *p;
			if (c >= 'A' && c <= 'Z') c = (char) (c - 'A' + 'a');
			if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
			    c == '.' || c == '_' || c == '-') {
				tmp[i++] = c;
			} else {
				tmp[i++] = '_';
			}
			p++;
		}
	}
	tmp[i++] = '_';
	h = macsurf_storage_hash((url != NULL) ? url : "");
	for (j = 0; j < 8; j++) {
		tmp[i + j] = hex[(h >> (28 - j * 4)) & 0xF];
	}
	i += 8;
	tmp[i++] = '.';
	tmp[i++] = 'j';
	tmp[i++] = 's';
	tmp[i++] = 'o';
	tmp[i++] = 'n';
	tmp[i] = '\0';
	fname[0] = (unsigned char) i;
	memcpy(fname + 1, tmp, (size_t) i);
}

#ifdef __MACOS9__

/* Resolve (creating as needed) the per-origin storage file under
 * MacSurfData/LocalStorage/, via the shared macos9_data_dir_get() helper
 * (macos9_disk_cache.c) so every MacSurfData subfolder lives in the same
 * place. Same FSMakeFSSpec/FSpCreate pattern as the disk cache's store. */
static OSErr
macsurf_storage_spec(const char *url, FSSpec *out)
{
	OSErr err;
	short vRef;
	long dirID;
	unsigned char fname[32];

	err = macos9_data_dir_get("LocalStorage", &vRef, &dirID);
	if (err != noErr) {
		macsurf_debug_log_writef("DIAG LocalStorage dir FAIL err=%d",
				(int)err);
		return err;
	}
	macsurf_storage_fname(url, fname);
	err = FSMakeFSSpec(vRef, dirID, fname, out);
	if (err == fnfErr) {
		err = FSpCreate(out, '????', '????', smSystemScript);
		if (err != noErr) return err;
		err = FSMakeFSSpec(vRef, dirID, fname, out);
	}
	return err;
}

#endif /* __MACOS9__ */

/* __storageLoad() -> JSON string of the current origin's saved map, or
 * null when there is nothing persisted (or anything failed). Called by
 * the _Storage shim at realm build; returning null is the no-data case,
 * never an error. */
static JSValue
qjs_storage_load(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv)
{
#ifdef __MACOS9__
	struct content *c;
	const char *url;
	FSSpec spec;
	short ref = 0;
	long eof = 0;
	long count;
	char *buf;
	JSValue out;
	OSErr err;

	(void) this_val; (void) argc; (void) argv;

	/* fixes1199 - g_qjs_content is a raw pointer with no lifetime guarantee
	 * (same hazard qjs_geometry_settled() already guards against via
	 * macos9_content_is_live()). During realm construction for a nested
	 * document (js_newthread -> qjs_build_context -> register_browser_globals
	 * -> the _Storage shim's synchronous __storageLoad() call), the new
	 * realm's own content has not been wired via qjs_set_content() yet, so
	 * this reads whatever content was live last -- which can already be
	 * freed. c->llcache on a freed content can still read back non-NULL
	 * garbage, so the old check let a dangling c through; content_get_url()
	 * or nsurl_access() on it then hands macsurf_storage_fname() a garbage
	 * or NULL url pointer it has no way to detect. */
	c = qjs_get_content();
	if (c == NULL) return JS_NULL;
	if (!macos9_content_is_live(c)) {
		MS_LOG("LIFE fixes1199: storage load caught stale g_qjs_content");
		return JS_NULL;
	}
	if (c->llcache == NULL) return JS_NULL;
	url = nsurl_access(content_get_url(c));
	if (url == NULL) return JS_NULL;

	err = macsurf_storage_spec(url, &spec);
	if (err != noErr) return JS_NULL;
	if (FSpOpenDF(&spec, fsRdPerm, &ref) != noErr) return JS_NULL;
	if (GetEOF(ref, &eof) != noErr || eof <= 0 ||
	    eof > MACSURF_STORAGE_MAX_BYTES) {
		FSClose(ref);
		return JS_NULL;
	}
	count = eof;
	buf = (char *) malloc((size_t) count + 1);
	if (buf == NULL) {
		FSClose(ref);
		return JS_NULL;
	}
	if (FSRead(ref, &count, buf) != noErr || count != eof) {
		free(buf);
		FSClose(ref);
		return JS_NULL;
	}
	FSClose(ref);
	buf[count] = '\0';
	out = JS_NewStringLen(ctx, buf, (size_t) count);
	free(buf);
	return out;
#else
	(void) ctx; (void) this_val; (void) argc; (void) argv;
	return JS_NULL;
#endif
}

/* __storageSave(json) - rewrite the current origin's storage file with the
 * given JSON. Overwrite-from-start + SetEOF truncation, so a shorter map
 * never leaves stale trailing bytes that would break JSON.parse on load. */
static JSValue
qjs_storage_save(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv)
{
#ifdef __MACOS9__
	struct content *c;
	const char *url;
	const char *s;
	FSSpec spec;
	short ref = 0;
	long count;
	OSErr err;

	(void) this_val;

	if (argc < 1 || !JS_IsString(argv[0])) return JS_UNDEFINED;
	s = JS_ToCString(ctx, argv[0]);
	if (s == NULL) return JS_UNDEFINED;

	/* fixes1199 - see the matching guard in qjs_storage_load(). */
	c = qjs_get_content();
	if (c == NULL || !macos9_content_is_live(c) || c->llcache == NULL) {
		JS_FreeCString(ctx, s);
		return JS_UNDEFINED;
	}
	url = nsurl_access(content_get_url(c));
	if (url == NULL) {
		JS_FreeCString(ctx, s);
		return JS_UNDEFINED;
	}

	count = (long) strlen(s);
	if (count > MACSURF_STORAGE_MAX_BYTES) {
		/* over the per-file cap: drop the write, like the cache's cap. */
		JS_FreeCString(ctx, s);
		return JS_UNDEFINED;
	}

	err = macsurf_storage_spec(url, &spec);
	if (err != noErr) {
		JS_FreeCString(ctx, s);
		return JS_UNDEFINED;
	}
	if (FSpOpenDF(&spec, fsRdWrPerm, &ref) != noErr) {
		JS_FreeCString(ctx, s);
		return JS_UNDEFINED;
	}
	SetFPos(ref, fsFromStart, 0);
	if (count > 0) {
		if (FSWrite(ref, &count, s) != noErr) {
			FSClose(ref);
			JS_FreeCString(ctx, s);
			return JS_UNDEFINED;
		}
	}
	(void) SetEOF(ref, count);
	FSClose(ref);
	JS_FreeCString(ctx, s);
	return JS_UNDEFINED;
#else
	(void) ctx; (void) this_val; (void) argc; (void) argv;
	return JS_UNDEFINED;
#endif
}

static void register_browser_globals(JSContext *ctx)
{
	JSValue global = JS_GetGlobalObject(ctx);
	JSValue console;
	JSValue location_obj;
	JSValue history_obj;
	JSValue nav_obj;
	JSValue crypto_obj;

	/* window / self / globalThis aliases - scripts check 'typeof window' */
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

	/* fixes843b (#167 S1 census) - see qjs_work_log_fetch's comment. */
	qjs_set_func(ctx, global, "__workLogFetch", qjs_work_log_fetch, 3);
	/* fixes845 - see qjs_work_log_xhr's comment. */
	qjs_set_func(ctx, global, "__workLogXHR", qjs_work_log_xhr, 3);

	/* fixes846 (#167 S3) - native XHR/fetch backend over fetch_start().
	 * See macos9_js_fetch.c for the full design. */
	macos9_js_fetch_install(ctx, global);

	/* --- crypto (getRandomValues / randomUUID) - fixes717 --- */
	crypto_obj = JS_NewObject(ctx);
	qjs_set_func(ctx, crypto_obj, "getRandomValues",
		qjs_crypto_get_random_values, 1);
	qjs_set_func(ctx, crypto_obj, "randomUUID", qjs_crypto_random_uuid, 0);
	JS_SetPropertyStr(ctx, global, "crypto", crypto_obj);

	/* --- monotonic clock for performance.now() --- */
	qjs_set_func(ctx, global, "__macsurf_monotonic_ms", qjs_monotonic_ms, 0);
	qjs_set_func(ctx, global, "__canvasMeasureText",
			qjs_canvas_measure_text_width, 2); /* fixes1245 */

	/* fixes1011 (Phase 3) - the viewport/scroll natives the window geometry
	 * accessors above are built on. Installed before the eval blocks that
	 * reference them. */
	qjs_set_func(ctx, global, "__viewportW", qjs_js_viewport_w, 0);
	qjs_set_func(ctx, global, "__viewportH", qjs_js_viewport_h, 0);
	qjs_set_func(ctx, global, "__scrollX",   qjs_js_scroll_x, 0);
	qjs_set_func(ctx, global, "__scrollY",   qjs_js_scroll_y, 0);
	qjs_set_func(ctx, global, "__scrollTo",  qjs_js_scroll_to, 2);
	qjs_set_func(ctx, global, "__gcsNative", qjs_get_computed_style, 1);
	qjs_set_func(ctx, global, "__msLife",    qjs_ms_life, 1); /* fixes1015 */
	qjs_set_func(ctx, global, "__msRafFired", qjs_raf_fired, 0); /* fixes1236 */
	/* localStorage persistence backend, consumed by the _Storage shim
	 * below (register_browser_globals runs per navigation, so the saved
	 * map is reloaded on every realm build). */
	qjs_set_func(ctx, global, "__storageLoad", qjs_storage_load, 0);
	qjs_set_func(ctx, global, "__storageSave", qjs_storage_save, 1);

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
	/* fixes876 - pass the DOMHighResTimeStamp every rAF callback expects.
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
	 * does not depend on `performance` (defined in a later eval block).
	 *
	 * fixes1236 (#167) - __msRafFired() counts a callback actually FIRING
	 * (inside the timeout, not at registration), and __msRafOrig stashes
	 * this function's own identity so macsurf_qjs_emit_js_profile can later
	 * detect whether the page overwrote window.requestAnimationFrame with
	 * its own scheduler -- a silent override would otherwise look identical
	 * to "rAF never gets called" in a naive fire-count alone. The fixes1149
	 * fallback ~40 lines below this file's register_browser_globals (guarded
	 * by `typeof g.requestAnimationFrame!=='function'`) is confirmed dead
	 * under normal load: this definition runs first in the same realm build
	 * and always leaves a real function installed, so the guard never trips
	 * unless THIS eval itself failed. Left in place as a fallback for that
	 * case, not because the two compete. */
	macsurf_qjs__safe_eval(ctx,
		"function requestAnimationFrame(fn){"
			"return setTimeout(function(){"
				"try{__msRafFired();}catch(e){}"
				"fn(__macsurf_monotonic_ms());"
			"},16);"
		"}"
		"function cancelAnimationFrame(id){"
			"clearTimeout(id);"
		"}"
		"globalThis.__msRafOrig=requestAnimationFrame;");

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
			/* fixes855 (#284) - document.nodeType MUST be 9
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
			/* fixes881 (Phase 0.7) - 'loading', not 'complete'.
			 *
			 * This is realm setup: it runs while the page is still parsing, so
			 * 'complete' was simply false. It also never became anything else
			 * -- js_fire_dom_ready set 'complete' again, and 'loading' and
			 * 'interactive' appeared nowhere in the file.
			 *
			 * The cost was not cosmetic. The near-universal init guard
			 *     if (document.readyState === 'loading')
			 *         addEventListener('DOMContentLoaded', init);
			 *     else init();
			 * always took the else branch and ran init() synchronously during
			 * parse, BEFORE the box tree existed -- so scripts that carefully
			 * wait for the DOM got the one thing they were avoiding. Now:
			 * 'loading' here -> 'interactive' at js_fire_dom_ready ->
			 * 'complete' at js_fire_window_load. */
			"document.readyState='loading';"
			/* fixes879 - `document.cookie=''` used to live here as a plain
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
			/* fixes846 (#167 S3) - real native-backed nodes, same
			 * native-fast-path/pre-document-fallback shape as createElement
			 * (qjs_dom_install installs __createTextNodeNative /
			 * __createDocumentFragmentNative once a real document is
			 * wired; createDocumentFragment itself is defined there too,
			 * so it is NOT redefined here -- an earlier unconditional
			 * override at this exact spot was dead code, always clobbered
			 * by qjs_dom_install running right after this function, but
			 * misleadingly suggested a no-op fragment was live; removed). */
			/* fixes1002 (#264) - document.implementation.
			 *
			 * It did not exist AT ALL, and jQuery 3.x reads
			 * `document.implementation.createHTMLDocument` during its
			 * own init. Hardware (now that fixes1000 made JS errors
			 * visible) shows exactly that:
			 *   TypeError: cannot read property 'createHTMLDocument'
			 *   of undefined  [hackaday _static bundle]
			 * followed by ReferenceError: jQuery is not defined in
			 * every script after it. So ONE missing property takes
			 * jQuery down and every jQuery-dependent script with it --
			 * which on hackaday is the entire comment/reply chain.
			 *
			 * createHTMLDocument returns a detached document used as a
			 * scratch parser. A minimal object with the shape jQuery
			 * touches (documentElement / head / body / createElement)
			 * is enough to clear init; it is not a real second
			 * document, and it is honest about that rather than
			 * pretending -- anything that tries to PARSE into it gets
			 * an empty document rather than wrong content.
			 *
			 * hasFeature() answers true: it is the legacy probe and
			 * the spec now requires exactly that. */
			"if(!document.implementation)document.implementation={"
				"hasFeature:function(){return true;},"
				"createHTMLDocument:function(t){"
					"var d={};"
					"d.nodeType=9;"
					"d.title=String(t||'');"
					"d.createElement=function(n){"
						"return document.createElement(n);};"
					"d.createTextNode=function(x){"
						"return document.createTextNode(x);};"
					"d.documentElement=document.createElement('html');"
					"d.head=document.createElement('head');"
					"d.body=document.createElement('body');"
					"d.getElementsByTagName=function(){return [];};"
					"d.querySelectorAll=function(){return [];};"
					"d.querySelector=function(){return null;};"
					"return d;},"
				"createDocumentType:function(){return {};},"
				"createDocument:function(){"
					"return document.implementation."
					"createHTMLDocument('');}};"
			"document.createTextNode=document.createTextNode||function(t){"
				"var n=document.__createTextNodeNative?"
					"document.__createTextNodeNative(String(t)):null;"
				"if(n)return n;"
				"return {nodeValue:String(t),textContent:String(t),"
					"appendChild:function(){return null;},data:String(t)};};"
			/* fixes873 (#301) - getElementsByTagName/ClassName were stubs that
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
			/* fixes1007 - document.write / writeln.
			 *
			 * Did not exist AT ALL (zero occurrences in this file), and
			 * on hardware it is the TOP remaining JS exception on a real
			 * page: hackaday's ad script does
			 *     OA_spc+="'><"+"/script>";document.write(OA_spc);
			 * and dies with "TypeError: not a function", taking the rest
			 * of that script with it. Still everywhere in ad, analytics
			 * and legacy code.
			 *
			 * Deliberately does NOT re-enter hubbub's tokenizer. Doing
			 * that mid-parse is the classic source of parser corruption
			 * and this tree has already paid for parser re-entrancy once
			 * (the fixes512 deferred-unpause machinery). Instead the
			 * written markup goes through the REAL fragment parser that
			 * innerHTML= already uses (fixes846,
			 * dom_hubbub_fragment_parser_create) and the resulting nodes
			 * are inserted at the current insertion point -- immediately
			 * after document.currentScript, which is where a parser would
			 * have put them.
			 *
			 * A <script> written this way becomes a real script element in
			 * the document, so dom_SCRIPT_showed_up picks it up and
			 * fetches/executes it exactly like any dynamically injected
			 * script (fixes868/869). That is the whole point: the ad
			 * script above is writing a script tag.
			 *
			 * currentScript is null for anything deferred or async, which
			 * is not an edge case -- fall back to <body>. Children are
			 * collected BEFORE any move, because moving mutates the
			 * sibling chain being walked.
			 *
			 * NOT implemented: post-load document.open()'s full document
			 * replace. Real browsers blank the page there, which is almost
			 * always a site bug being punished; blanking it here on a
			 * mistaken read would be a far worse failure than ignoring it.
			 * open() returns the document and close() is a no-op, and this
			 * comment is the record that the omission is deliberate. */
			"document.write=function(){"
				"var s='',i;"
				"for(i=0;i<arguments.length;i++)s+=String(arguments[i]);"
				"if(!s)return;"
				"var ref=document.currentScript||null;"
				"var parent=(ref&&ref.parentNode)||document.body||"
					"document.documentElement;"
				"if(!parent)return;"
				"var anchor=(ref&&ref.parentNode===parent)?"
					"ref.nextSibling:null;"
				"var holder=document.createElement('div');"
				"if(!holder)return;"
				"try{holder.innerHTML=s;}catch(e){return;}"
				"var kids=[],c=holder.firstChild;"
				"while(c){kids.push(c);c=c.nextSibling;}"
				"for(i=0;i<kids.length;i++){try{"
					"if(anchor)parent.insertBefore(kids[i],anchor);"
					"else parent.appendChild(kids[i]);"
				"}catch(e){}}"
			"};"
			"document.writeln=function(){"
				"var a=[],i;"
				"for(i=0;i<arguments.length;i++)a.push(String(arguments[i]));"
				"a.push('\\n');"
				"document.write(a.join(''));"
			"};"
			"document.open=function(){return document;};"
			"document.close=function(){};"
			"document.getElementsByTagName=function(t){"
				"return document.querySelectorAll(String(t));};"
			"document.getElementsByClassName=function(c){"
				"return document.querySelectorAll('.'+String(c).split(/\\s+/)"
					".filter(function(x){return !!x;}).join('.'));};"
			/* fixes1008 (2b) - was a hardcoded [] , which is a WRONG
			 * ANSWER rather than a missing method: a caller gets an
			 * empty list and concludes the elements do not exist. Radio
			 * groups and legacy form code use it constantly. Delegates
			 * to the same compound matcher getElementsByTagName and
			 * getElementsByClassName already use (fixes873). */
			"document.getElementsByName=function(n){"
				"return document.querySelectorAll('[name=\"'+"
					"String(n)+'\"]');};"
			"document.createComment=function(t){"
				"return document.createTextNode('');};"
			/* fixes1010 - the same universals on `document`.
			 *
			 * jQuery's isAttached is ce.contains(e.ownerDocument, e), and
			 * its contains() does `a.contains ? a.contains(bup) : ...` with
			 * a = the document. The guard means a missing document.contains
			 * does not throw -- it silently answers "not attached", which is
			 * worse: jQuery then treats every element as detached and skips
			 * work it should do. Delegate to documentElement, which has the
			 * real native contains. */
			"document.contains=function(n){"
				"if(!n)return false;"
				"if(n===document||n===document.documentElement)return true;"
				"var de=document.documentElement;"
				"return !!(de&&de.contains&&de.contains(n));};"
			"document.getRootNode=function(){return document;};"
			"Object.defineProperty(document,'isConnected',{"
				"configurable:true,get:function(){return true;}});"
			"document.ownerDocument=null;"
			"document.querySelectorAll=document.querySelectorAll||"
				"function(){return [];};"
			/* fixes1006 (1b) - tell libdom, or a real click never
			 * arrives. The _listeners registry is unchanged; the
			 * missing half was the registration. __msRegDocEvent is
			 * the native hook installed by qjs_dom_install (absent
			 * for the pre-document window, hence the typeof guard). */
			/* ONCE PER TYPE. libdom does not dedupe (node, type) --
			 * every registration appends another listener_entry, and
			 * _dom_event_target_dispatch loops over ALL of them, so N
			 * registrations replay the whole _listeners array N times.
			 * Measured: 3 registrations of 'click' ran one delegation
			 * handler 4 times. Elements already guard this with
			 * `fresh` in qjs_el_add_event_listener_data; document and
			 * window share the document node, so they share one gate. */
			"document._reg={};"
			"document.__msRegOnce=function(t,opt){"
				"if(document._reg[t])return;"
				"document._reg[t]=1;"
				"if(typeof document.__msRegDocEvent==='function'){"
					"try{document.__msRegDocEvent(t,opt);}catch(e){}}};"
			"document.addEventListener=document.addEventListener||"
				"function(t,fn,opt){"
					"if(!document._listeners)document._listeners={};"
					"if(!document._listeners[t])document._listeners[t]=[];"
					"document._listeners[t].push(fn);"
					/* fixes1249 (#167) - the real Facebook bundle has
					 * AT LEAST 5-6 distinct DOMContentLoaded listener
					 * registration sites (a messenger health tracker, a
					 * generic event-ready utility, React's own Suspense-
					 * retry logic, a Comet page-load health check that
					 * warns if #has-finished-comet-page is missing --
					 * confirmed by reading the real bundles), but the
					 * census shows only ONE window/document listener
					 * ever gets registered. fixes1247 (#167) already
					 * showed require() never fires at all -- most of
					 * those candidates live inside __d-wrapped module
					 * factories that never run. This identifies WHICH
					 * ONE actually does, by fingerprinting the listener
					 * function's own source at registration time -- a
					 * grep-able match against the real bundle text
					 * already on hand answers "which listener" directly
					 * instead of guessing. Scoped to DOMContentLoaded
					 * only (the one type this whole investigation cares
					 * about) so this can't become a general, unbounded
					 * function-source log. */
					"if(t==='DOMContentLoaded'){try{"
						"if(typeof __msLife==='function')"
							"__msLife('docevt reg DOMContentLoaded: '+"
								"String(fn).slice(0,180));"
					"}catch(e){}}"
					"document.__msRegOnce(t,opt);};"
			"document.removeEventListener=document.removeEventListener||"
				"function(t,fn){"
					"var L=document._listeners&&document._listeners[t];if(!L)return;"
					"for(var i=0;i<L.length;i++)if(L[i]===fn){L.splice(i,1);return;}};"
			/* fixes1236 (#167) - had ZERO diagnostics and a fully silent
			 * catch(e){} -- a document-level listener that threw left no
			 * trace anywhere. window.dispatchEvent (below, same fix round)
			 * at least attempted one; document.dispatchEvent never did.
			 * Same __msLife pattern: type=/n= scoped to lifecycle events to
			 * bound log volume, THROW unscoped since it is inherently rare. */
			"document.dispatchEvent=document.dispatchEvent||"
				"function(ev){"
					"var t=ev&&ev.type;"
					"var L=document._listeners&&document._listeners[t];"
					"var n=L?L.length:0;"
					"if(t==='DOMContentLoaded'||t==='load'||"
					   "t==='readystatechange'||t==='pageshow'){"
						"try{if(typeof __msLife==='function')"
							"__msLife('docevt type='+t+' n='+n);}catch(_){}}"
					"if(L)L.forEach(function(f){try{f(ev);}catch(e){"
						"try{if(typeof __msLife==='function')"
							"__msLife('docevt THREW type='+t+': '+"
								"((e&&e.message)||e));}catch(_){}"
					"}});return true;};"
		"}");

	/* fixes879 - the "navigator extended shims" block that used to sit HERE was
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
	 * PerformanceObserver stays a no-op. IntersectionObserver is DIFFERENT
	 * and must actually fire: modern feeds (Facebook) gate their content
	 * load on it -- they observe the feed container and only request/
	 * reveal content when the observer reports it intersecting the
	 * viewport. A no-op observer means the "you're visible, load now"
	 * signal never arrives, so the feed JS runs (confirmed on hardware:
	 * ~550KB executed) but issues ZERO fetch/XHR and never hydrates.
	 * fixes853 (#167): give IntersectionObserver a real-enough
	 * implementation -- observe() asynchronously delivers a single
	 * isIntersecting=true entry for the target (a pragmatic "visible on
	 * layout" first cut; geometry-accurate viewport testing is a later
	 * refinement), which is the trigger that lets the feed request its
	 * data through the now-real fetch/XHR (fixes846). Fires via the real
	 * timer arena (setTimeout), asynchronously, exactly as a browser
	 * delivers observer records. */
	macsurf_qjs__safe_eval(ctx,
		/* fixes1015 - PerformanceObserver is a NO-OP; a page that waits
		 * on one waits forever. Log each observe() so that failure mode
		 * is visible instead of silent.
		 *
		 * fixes1232 - this used to be shared with MutationObserver under
		 * one constructor (_Observer), so its log line could only say the
		 * ambiguous "Mutation/ResizeObserver.observe". fixes1235 gives
		 * MutationObserver its own real implementation below; this
		 * factory now backs PerformanceObserver alone (kept as a factory,
		 * not simplified to a single class, in case a future no-op
		 * observer needs to share it again). */
		"function _mkObserver(label){"
			"function C(cb){this._cb=cb;}"
			"C.prototype.observe=function(t,opts){"
				"try{__msLife('WANT '+label+'.observe '"
				"+((t&&(t.id||t.tagName))||'?')+' (NO-OP)');}catch(_){}"
				"var self=this;"
				"try{setTimeout(function(){"
					"try{self._cb([],self);}catch(e){}"
				"},0);}catch(e){}"
			"};"
			"C.prototype.unobserve=function(){};"
			"C.prototype.disconnect=function(){};"
			"C.prototype.takeRecords=function(){return [];};"
			"return C;"
		"}"
		"this.PerformanceObserver=_mkObserver('PerformanceObserver');");
	/* fixes1235 (#167) - real-enough MutationObserver. Hardware evidence
	 * (2026-08-20): a Facebook checkpoint's approval-detection UI installs
	 * exactly one `new MutationObserver(cb).observe(document.documentElement,
	 * ...)` and then goes completely silent -- confirmed via the observer
	 * shim's own argument shape (PerformanceObserver's real API never takes
	 * a DOM node, only ours logged tagName="HTML") and via zero further log
	 * activity of any kind for ~18s after the old no-op's one empty-array
	 * callback. Root cause: the old shared stub delivered `[]`; a callback
	 * reading entries[0] got undefined and had nothing to react to, so
	 * server-driven approval never revealed itself in the DOM the app was
	 * waiting on.
	 *
	 * Design (scoped, not a full spec implementation): records are
	 * delivered from js_fire_mutation_batch (macsurf_qjs.c), called once
	 * from html_reconvert_done AFTER a reconvert completes successfully --
	 * the SAME fire point fixes1090's resize/load convergence hooks use.
	 * That means delivery cannot happen more often than reconvert's own
	 * debounce/floor already allows, and is never called from inside the
	 * box-tree rebuild -- the feedback-loop risk the old no-op comment
	 * warned about is bounded by construction, not by hoping the page
	 * behaves. Phase 1 scope: every registered observer gets ONE synthetic
	 * childList record per completed reconvert, regardless of its actual
	 * target/subtree options -- matching the one usage pattern evidence
	 * shows (a root-level, presumably subtree:true watcher) without
	 * building precise per-node/subtree target matching against the
	 * pending table's opaque dom_node pointers, which macos9_reconvert.c
	 * cannot safely dereference (see its header comment) and would need a
	 * new bridge through html.c to do properly. A future round can narrow
	 * this once a non-root .observe() target shows up in evidence. */
	macsurf_qjs__safe_eval(ctx,
		"function MutationObserver(cb){this._cb=cb;this._targets=[];}"
		"MutationObserver.prototype.observe=function(t,opts){"
			"if(!t)return;"
			"try{__msLife('WANT MutationObserver.observe '"
			"+((t.id||t.tagName)||'?')+' (real)');}catch(_){}"
			"this._targets.push(t);"
			"if(!globalThis.__msMutObservers)globalThis.__msMutObservers=[];"
			"if(globalThis.__msMutObservers.indexOf(this)<0)"
				"globalThis.__msMutObservers.push(this);"
		"};"
		"MutationObserver.prototype.unobserve=function(t){"
			"var i=this._targets.indexOf(t);if(i>=0)this._targets.splice(i,1);"
		"};"
		"MutationObserver.prototype.disconnect=function(){"
			"this._targets=[];"
			"if(globalThis.__msMutObservers){"
				"var j=globalThis.__msMutObservers.indexOf(this);"
				"if(j>=0)globalThis.__msMutObservers.splice(j,1);"
			"}"
		"};"
		"MutationObserver.prototype.takeRecords=function(){return [];};"
		"this.MutationObserver=MutationObserver;"
		"globalThis.__msDeliverMutations=function(){"
			"var list=globalThis.__msMutObservers;"
			"if(!list||list.length===0)return;"
			"for(var i=0;i<list.length;i++){"
				"var ob=list[i];"
				"if(!ob._targets||ob._targets.length===0)continue;"
				"var recs=[];"
				"for(var j=0;j<ob._targets.length;j++){"
					"recs.push({type:'childList',target:ob._targets[j],"
						"addedNodes:[],removedNodes:[],"
						"previousSibling:null,nextSibling:null,"
						"attributeName:null,attributeNamespace:null,"
						"oldValue:null});"
				"}"
				"try{ob._cb(recs,ob);}catch(e){}"
			"}"
		"};");
	/* fixes1231 (#167) - real-enough ResizeObserver. Hardware evidence
	 * (2026-08-20, Facebook 2FA checkpoint): every broken checkpoint page
	 * (recover/code, two_step_verification/two_factor,
	 * two_step_verification/authentication) logs exactly
	 * "WANT Mutation/ResizeObserver.observe HTML (NO-OP)" and nothing
	 * else geometry-related (LIFE JSGEOM reads=0 throughout -- Bloks never
	 * calls getBoundingClientRect/offsetWidth directly here). The old
	 * shared _Observer answered with an EMPTY entries array; a callback
	 * that reads entries[0].contentRect got undefined and silently did
	 * nothing, so the app never learned the viewport's size and the
	 * size-dependent mount never ran. The observed target is always
	 * document.documentElement/body in every capture -- a viewport-size
	 * watcher, not a "measure my own container" one -- so this can answer
	 * HONESTLY using window.innerWidth/innerHeight, which are real static
	 * values already (qjs_js_viewport_w/h), not gated by
	 * MACSURF_JS_GEOMETRY and with no measure-then-mutate hazard: the
	 * viewport size does not change as a side effect of a script reading
	 * it. Any OTHER target still falls back to getBoundingClientRect(),
	 * i.e. the existing honest-undefined/zero policy, unchanged. */
	macsurf_qjs__safe_eval(ctx,
		"function ResizeObserver(cb){this._cb=cb;this._targets=[];}"
		"ResizeObserver.prototype.observe=function(el){"
			"if(!el)return;"
			"try{__msLife('WANT ResizeObserver.observe '"
			"+((el.id||el.tagName)||'?')+' (real)');}catch(_){}"
			"this._targets.push(el);"
			"var self=this;"
			"setTimeout(function(){"
				"if(self._targets.indexOf(el)<0)return;"
				"var w,h,r;"
				"if(typeof document!=='undefined'&&"
					"(el===document.documentElement||el===document.body)){"
					"w=innerWidth;h=innerHeight;"
				"}else{"
					"r=(el.getBoundingClientRect&&el.getBoundingClientRect())||"
						"{width:0,height:0};"
					"w=r.width;h=r.height;"
				"}"
				"var box={inlineSize:w,blockSize:h};"
				"var entry={target:el,"
					"contentRect:{x:0,y:0,top:0,left:0,"
						"width:w,height:h,right:w,bottom:h},"
					"borderBoxSize:[box],contentBoxSize:[box],"
					"devicePixelContentBoxSize:[box]};"
				"try{self._cb([entry],self);}catch(e){}"
			"},0);"
		"};"
		"ResizeObserver.prototype.unobserve=function(el){"
			"var i=this._targets.indexOf(el);if(i>=0)this._targets.splice(i,1);"
		"};"
		"ResizeObserver.prototype.disconnect=function(){this._targets=[];};"
		"this.ResizeObserver=ResizeObserver;");
	macsurf_qjs__safe_eval(ctx,
		"function IntersectionObserver(cb,opts){"
			"this._cb=cb;this._opts=opts||{};this._targets=[];"
			"this.root=(opts&&opts.root)||null;"
			"this.rootMargin=(opts&&opts.rootMargin)||'0px';"
			"this.thresholds=[0];"
		"}"
		"IntersectionObserver.prototype.observe=function(el){"
			"if(!el)return;"
			"try{__msLife('WANT IntersectionObserver.observe '"
			"+((el.id||el.tagName)||'?')+' (always intersecting)');}"
			"catch(_){}"
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
		/* fixes1011 - these were no-ops, so every "back to top" button and
		 * every scroll restoration silently did nothing. Accepts both the
		 * (x, y) and the ({top, left, behavior}) forms; `behavior:'smooth'`
		 * is honoured as an instant jump, which is the honest degradation. */
		"this.scrollTo=function(a,b){"
			"var x,y;"
			"if(a&&typeof a==='object'){x=a.left;y=a.top;}else{x=a;y=b;}"
			"__scrollTo((x===undefined?__scrollX():x)|0,"
				"(y===undefined?__scrollY():y)|0);};"
		"this.scrollBy=function(a,b){"
			"var dx,dy;"
			"if(a&&typeof a==='object'){dx=a.left;dy=a.top;}else{dx=a;dy=b;}"
			"__scrollTo((__scrollX()+((dx|0)||0))|0,"
				"(__scrollY()+((dy|0)||0))|0);};"
		"this.scroll=this.scrollTo;"
		"this._winListeners={};"
		/* fixes1006 (1b) - window has no DOM node, so register against the
		 * DOCUMENT node and let qjs_dom_listener_cb fan out to
		 * _winListeners with the right capture/bubble ordering. Without
		 * this, window click/scroll/keydown handlers never fired from real
		 * input at all. */
		"this.addEventListener=function(t,fn,opt){"
			"if(!this._winListeners[t])this._winListeners[t]=[];"
			"this._winListeners[t].push(fn);"
			/* fixes1249 (#167) - same fingerprint as document's
			 * addEventListener (see its comment); window is the OTHER
			 * side of the exact same "which of 5-6 candidate
			 * DOMContentLoaded listeners actually registers" question. */
			"if(t==='DOMContentLoaded'){try{"
				"if(typeof __msLife==='function')"
					"__msLife('winevt reg DOMContentLoaded: '+"
						"String(fn).slice(0,180));"
			"}catch(e){}}"
			"if(typeof document!=='undefined'&&"
			   "typeof document.__msRegOnce==='function'){"
				"try{document.__msRegOnce(t,opt);}catch(e){}}};"
		"this.removeEventListener=function(t,fn){"
			"var arr=this._winListeners[t];if(!arr)return;"
			"for(var i=0;i<arr.length;i++)if(arr[i]===fn){arr.splice(i,1);return;}};"
		/* fixes863 (#289 probe) - this used to be a bare
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
		/* fixes1236 (#167) - the WORK/console.error trace below NEVER
		 * survived to a hardware log across two full Facebook sessions
		 * (grep for "winevt" in either: zero hits) -- console.error's WORK
		 * routing is compiled out of shipping builds entirely (fixes1015's
		 * own rationale for __msLife existing at all), not merely filtered
		 * by text content as the fixes863 comment above claimed. Switched
		 * to __msLife (LIFE-prefixed, budgeted, survives by construction).
		 * type=/n= scoped to the lifecycle event names this investigation
		 * cares about, so a page that dispatches custom pub/sub events on
		 * window does not burn the shared __msLife budget; a THROWN
		 * handler is logged for ANY type since that is inherently rare. */
		"this.dispatchEvent=function(ev){"
			"var t=ev&&ev.type;var arr=t&&this._winListeners[t];"
			"var n=arr?arr.length:0;"
			"if(t==='DOMContentLoaded'||t==='load'||t==='readystatechange'||"
			   "t==='pageshow'){"
				"try{if(typeof __msLife==='function')"
					"__msLife('winevt type='+t+' n='+n);}catch(_){}}"
			"if(arr)arr.forEach(function(f){try{f(ev);}catch(e){"
				"try{if(typeof __msLife==='function')"
					"__msLife('winevt THREW type='+t+': '+"
						"((e&&e.message)||e));}catch(_){}"
			"}});return true;};"
		/* fixes1011 - the REAL getComputedStyle. __gcsNative reads the
		 * cascade + box (installed in qjs_dom_install); this wrapper adds
		 * getPropertyValue with dash-to-camel mapping and falls back to the
		 * inline style for anything the native side does not cover, which is
		 * exactly what this function used to do for EVERYTHING. */
		"this.getComputedStyle=function(el){"
			"var c=(typeof __gcsNative==='function')?__gcsNative(el):null;"
			"var inl=(el&&el.style)?el.style:null;"
			"var o=c||{};"
			"o.getPropertyValue=function(p){"
				"var k=String(p).replace(/-([a-z])/g,function(m,x){"
					"return x.toUpperCase();});"
				"if(o[k]!==undefined&&typeof o[k]!=='function')return ''+o[k];"
				"if(inl&&inl.getPropertyValue)return inl.getPropertyValue(p);"
				"return '';};"
			"o.setProperty=function(p,v){if(inl&&inl.setProperty)"
				"inl.setProperty(p,v);};"
			"o.cssText=(inl&&inl.cssText)||'';"
			"return o;};"
		/* fixes1015 - ours answers matches:false unconditionally, so any
		 * rendering that branches on a media query silently takes the
		 * false path. Log which queries the page actually asked. */
		/* fixes1114b (#265) - REAL matchMedia evaluator, not hardcoded false.
		 *
		 * The old stub answered {matches:false} to EVERY query, which is a
		 * lying answer: a page's (min-width:800px) check got `false` and the
		 * page served its mobile layout on a 949px-wide window.
		 *
		 * Unknown queries still log via __msLife (matching the old WANT
		 * behavior) so the hardware log tells us which queries real pages
		 * use next. Known-query evaluation is at call time so
		 * this.innerWidth/this.innerHeight (set a few lines below) are
		 * already available. */
		"this.matchMedia=function(q){"
			"var m={matches:false,media:q||'',"
				"addListener:function(){},removeListener:function(){},"
				"addEventListener:function(){},removeEventListener:function(){}};"
			"if(!q)return m;"
			"var s=q.replace(/[\\t\\n\\r ]/g,'');"
			"try{"
				/* display-mode */
				"if(s==='(display-mode:standalone)'||s==='(display-mode:fullscreen)')return m;"
				"if(s==='(display-mode:browser)'){m.matches=true;return m;}"
				/* prefers-color-scheme (Mac OS 9 is always light) */
				"if(s==='(prefers-color-scheme:dark)')return m;"
				"if(s==='(prefers-color-scheme:light)'){m.matches=true;return m;}"
				/* max/min width (viewport is 949px) */
				"if(s.indexOf('(max-width:')===0){var n=parseInt(s.slice(11));"
					"m.matches=(n>0&&this.innerWidth<=n);return m;}"
				"if(s.indexOf('(min-width:')===0){var n=parseInt(s.slice(11));"
					"m.matches=(n>0&&this.innerWidth>=n);return m;}"
				/* max/min height (viewport is 613px) */
				"if(s.indexOf('(max-height:')===0){var n=parseInt(s.slice(12));"
					"m.matches=(n>0&&this.innerHeight<=n);return m;}"
				"if(s.indexOf('(min-height:')===0){var n=parseInt(s.slice(12));"
					"m.matches=(n>0&&this.innerHeight>=n);return m;}"
				/* pointer (desktop = fine) */
				"if(s==='(pointer:fine)'){m.matches=true;return m;}"
				/* hover (desktop = hover) */
				"if(s==='(hover:hover)'){m.matches=true;return m;}"
				/* prefers-reduced-motion */
				"if(s==='(prefers-reduced-motion:reduce)')return m;"
				"if(s==='(prefers-reduced-motion:no-preference)'){m.matches=true;return m;}"
				/* unknown - log so we can add it */
				"try{__msLife('WANT matchMedia \"'+q+'\" (unknown, answered false)');}catch(e){}"
			"}catch(e){"
				"try{__msLife('WANT matchMedia \"'+q+'\" (parse error)');}catch(e2){}"
			"}"
			"return m;};"
		"this.requestIdleCallback=function(fn){return setTimeout(fn,0);};"
		"this.cancelIdleCallback=function(id){clearTimeout(id);};"
		/* fixes1011 - LIVE viewport + scroll, not frozen constants.
		 *
		 * innerWidth/innerHeight were hardcoded 949x613 and scrollX/scrollY
		 * were permanently 0, so responsive-JS branches always took the same
		 * path regardless of the real window, and any "have we scrolled past
		 * the header" test was permanently false. These are accessors now,
		 * reading the front window each time.
		 *
		 * scrollTo/scrollBy really scroll -- they were no-ops, so every
		 * "back to top" control and every scroll-restoration did nothing. */
		"Object.defineProperty(this,'innerWidth',{configurable:true,"
			"get:function(){return __viewportW();}});"
		"Object.defineProperty(this,'innerHeight',{configurable:true,"
			"get:function(){return __viewportH();}});"
		"Object.defineProperty(this,'outerWidth',{configurable:true,"
			"get:function(){return __viewportW();}});"
		"Object.defineProperty(this,'outerHeight',{configurable:true,"
			"get:function(){return __viewportH();}});"
		"Object.defineProperty(this,'scrollX',{configurable:true,"
			"get:function(){return __scrollX();}});"
		"Object.defineProperty(this,'scrollY',{configurable:true,"
			"get:function(){return __scrollY();}});"
		"Object.defineProperty(this,'pageXOffset',{configurable:true,"
			"get:function(){return __scrollX();}});"
		"Object.defineProperty(this,'pageYOffset',{configurable:true,"
			"get:function(){return __scrollY();}});"
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

	/* --- FormData ---
	 * Ordered [key,value,filename] triples, not a spec-perfect Map --
	 * enough for XenForo's editor-compiled.js attach path and any script
	 * that just constructs/populates/reads one. value may be a string or
	 * a Blob/File-like object (see the capability-detection block further
	 * down -- Blob/File already exist there). filename is only meaningful
	 * when value is a Blob/File; append/set's 3rd arg lets a caller name
	 * it explicitly. Iteration order matches insertion order (spec). */
	macsurf_qjs__safe_eval(ctx,
		"function FormData(form){"
			"this._k=[];this._v=[];this._f=[];"
			"if(form&&form.elements){"
				"var els=form.elements;"
				"for(var i=0;i<els.length;i++){"
					"var e=els[i];"
					"if(!e||!e.name||e.disabled)continue;"
					"var t=(e.type||'').toLowerCase();"
					"if((t==='checkbox'||t==='radio')&&!e.checked)continue;"
					"if(e.value!==undefined){"
						"this._k.push(e.name);this._v.push(e.value);this._f.push(undefined);"
					"}"
				"}"
			"}"
		"}"
		"FormData.prototype.append=function(k,v,fn){"
			"this._k.push(String(k));"
			"this._v.push((v&&typeof v==='object')?v:String(v));"
			"this._f.push(fn);};"
		"FormData.prototype.get=function(k){"
			"for(var i=0;i<this._k.length;i++)"
				"if(this._k[i]==k)return this._v[i];"
			"return null;};"
		"FormData.prototype.getAll=function(k){"
			"var r=[];for(var i=0;i<this._k.length;i++)"
				"if(this._k[i]==k)r.push(this._v[i]);return r;};"
		"FormData.prototype.set=function(k,v,fn){"
			"this.delete(k);this.append(k,v,fn);};"
		"FormData.prototype.has=function(k){"
			"for(var i=0;i<this._k.length;i++)if(this._k[i]==k)return true;"
			"return false;};"
		"FormData.prototype.delete=function(k){"
			"for(var i=this._k.length-1;i>=0;i--)"
				"if(this._k[i]==k){this._k.splice(i,1);this._v.splice(i,1);this._f.splice(i,1);}};"
		"FormData.prototype.forEach=function(cb,thisArg){"
			"for(var i=0;i<this._k.length;i++)cb.call(thisArg,this._v[i],this._k[i],this);};"
		"FormData.prototype.entries=function(){"
			"var self=this,i=0;"
			"var it={next:function(){"
				"if(i>=self._k.length)return{value:undefined,done:true};"
				"var r={value:[self._k[i],self._v[i]],done:false};i++;return r;}};"
			"if(typeof Symbol!=='undefined'&&Symbol.iterator)it[Symbol.iterator]=function(){return it;};"
			"return it;};"
		"FormData.prototype.keys=function(){"
			"var self=this,i=0;"
			"var it={next:function(){"
				"if(i>=self._k.length)return{value:undefined,done:true};"
				"var r={value:self._k[i],done:false};i++;return r;}};"
			"if(typeof Symbol!=='undefined'&&Symbol.iterator)it[Symbol.iterator]=function(){return it;};"
			"return it;};"
		"FormData.prototype.values=function(){"
			"var self=this,i=0;"
			"var it={next:function(){"
				"if(i>=self._k.length)return{value:undefined,done:true};"
				"var r={value:self._v[i],done:false};i++;return r;}};"
			"if(typeof Symbol!=='undefined'&&Symbol.iterator)it[Symbol.iterator]=function(){return it;};"
			"return it;};"
		"if(typeof Symbol!=='undefined'&&Symbol.iterator)"
			"FormData.prototype[Symbol.iterator]=function(){return this.entries();};"
		"this.FormData=FormData;");

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
		/* FormData bodies are not native-fetch-layer aware -- __xhrNativeSend
		 * just JS_ToCString()s whatever it is handed. Encode multipart/form-data
		 * here in JS (spec boundary format) rather than teaching the native
		 * layer a new body type; a plain string/number/etc body passes through
		 * unchanged, same as before. */
		"XMLHttpRequest.prototype._encodeFormData=function(fd){"
			"var boundary='----MacSurfFormBoundary'+"
				"Math.random().toString(36).slice(2)+Date.now().toString(36);"
			"var parts=[];"
			"for(var i=0;i<fd._k.length;i++){"
				"var k=fd._k[i],v=fd._v[i],fn=fd._f[i];"
				"var isBlob=(v&&typeof v==='object');"
				"var name=(isBlob?(fn||v.name||'blob'):null);"
				"var head='--'+boundary+'\\r\\nContent-Disposition: form-data; name=\"'+k+'\"';"
				"if(isBlob){"
					"head+='; filename=\"'+name+'\"\\r\\nContent-Type: '+"
						"(v.type||'application/octet-stream')+'\\r\\n\\r\\n';"
				"}else{"
					"head+='\\r\\n\\r\\n';"
				"}"
				"parts.push(head+String(isBlob?'':v)+'\\r\\n');"
			"}"
			"parts.push('--'+boundary+'--\\r\\n');"
			"return{body:parts.join(''),contentType:'multipart/form-data; boundary='+boundary};"
		"};"
		"XMLHttpRequest.prototype.send=function(body){"
			"if(typeof __workLogXHR==='function')"
				"__workLogXHR('send',this._method,this._url);"
			"if(typeof __xhrNativeSend!=='function'){"
				"this.readyState=4;this.status=0;this._fire('readystatechange');"
				"this._fire('error');this._fire('loadend');return;"
			"}"
			"if(typeof FormData!=='undefined'&&body instanceof FormData){"
				"var enc=this._encodeFormData(body);"
				"var hasCT=false;"
				"for(var hi=0;hi<this._reqHeaders.length;hi++)"
					"if(/^content-type\\s*:/i.test(this._reqHeaders[hi])){hasCT=true;break;}"
				"if(!hasCT)this._reqHeaders.push('Content-Type: '+enc.contentType);"
				"body=enc.body;"
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

	/* --- Headers / Request / Response (fixes1140) --- *
	 *
	 * THE 68kmla "Post thread" BUTTON. Hardware named it exactly:
	 *
	 *   LIFE CLICK tag=A live=1 prevented=1 xfclick=overlay
	 *              href=/bb/forums/-/create-thread
	 *   LIFE WANT Request []
	 *   LIFE js unhandled rejection: TypeError: not a function
	 *
	 * The click routing was never broken -- `prevented=1` means XenForo's
	 * delegated handler matched the node and CLAIMED the click. It then did
	 * `new Request(...)` to load the overlay, `Request` did not exist, the
	 * Promise rejected, and the overlay silently never opened. One missing
	 * constructor, three rounds of blaming geometry.
	 *
	 * fetch() has existed since fixes846 but ONLY in its `fetch(url, opts)`
	 * form; the Fetch API's three classes were never added, so any caller
	 * using the spec's object form hit a wall. These are ordinary
	 * spec-shaped implementations, defined BEFORE fetch so it can accept a
	 * Request and a Headers.
	 *
	 * Header names are case-insensitive per spec, so the store is keyed on
	 * the lowercased name while `forEach` reports the name as given. */
	macsurf_qjs__safe_eval(ctx,
		"(function(){"
		"function Headers(init){"
			"this._h={};"
			"if(init){"
				"if(init instanceof Headers){"
					"var self=this;"
					"init.forEach(function(v,k){self.append(k,v);});"
				"}else if(Object.prototype.toString.call(init)==='[object Array]'){"
					"for(var i=0;i<init.length;i++)"
						"if(init[i]&&init[i].length>=2)"
							"this.append(init[i][0],init[i][1]);"
				"}else{"
					"for(var k in init)"
						"if(Object.prototype.hasOwnProperty.call(init,k))"
							"this.append(k,init[k]);"
				"}"
			"}"
		"}"
		"Headers.prototype.append=function(n,v){"
			"n=String(n).toLowerCase();"
			"if(this._h[n]===undefined)this._h[n]=String(v);"
			"else this._h[n]=this._h[n]+', '+String(v);};"
		"Headers.prototype.set=function(n,v){"
			"this._h[String(n).toLowerCase()]=String(v);};"
		"Headers.prototype.get=function(n){"
			"var v=this._h[String(n).toLowerCase()];"
			"return v===undefined?null:v;};"
		"Headers.prototype.has=function(n){"
			"return this._h[String(n).toLowerCase()]!==undefined;};"
		"Headers.prototype['delete']=function(n){"
			"delete this._h[String(n).toLowerCase()];};"
		"Headers.prototype.forEach=function(fn,thisArg){"
			"for(var k in this._h)"
				"if(Object.prototype.hasOwnProperty.call(this._h,k))"
					"fn.call(thisArg,this._h[k],k,this);};"
		"Headers.prototype.keys=function(){"
			"var a=[];this.forEach(function(v,k){a.push(k);});return a;};"
		"Headers.prototype.values=function(){"
			"var a=[];this.forEach(function(v){a.push(v);});return a;};"
		"Headers.prototype.entries=function(){"
			"var a=[];this.forEach(function(v,k){a.push([k,v]);});return a;};"
		"this.Headers=Headers;"
		"})();");

	macsurf_qjs__safe_eval(ctx,
		"(function(){"
		"function Request(input,init){"
			"init=init||{};"
			"if(input&&typeof input==='object'&&input.url!==undefined){"
				"this.url=String(input.url);"
				"this.method=init.method||input.method||'GET';"
				"this.headers=new Headers(init.headers||input.headers);"
				"this.body=init.body!==undefined?init.body:input.body;"
				"this.credentials=init.credentials||input.credentials||'same-origin';"
				"this.mode=init.mode||input.mode||'cors';"
			"}else{"
				"this.url=String(input);"
				"this.method=init.method||'GET';"
				"this.headers=new Headers(init.headers);"
				"this.body=init.body;"
				"this.credentials=init.credentials||'same-origin';"
				"this.mode=init.mode||'cors';"
			"}"
			"this.method=String(this.method).toUpperCase();"
			"this.cache=init.cache||'default';"
			"this.redirect=init.redirect||'follow';"
			"this.referrer=init.referrer||'about:client';"
			"this.signal=init.signal||null;"
			"this.bodyUsed=false;"
		"}"
		"Request.prototype.clone=function(){return new Request(this);};"
		"this.Request=Request;"
		"})();");

	macsurf_qjs__safe_eval(ctx,
		"(function(){"
		"function Response(body,init){"
			"init=init||{};"
			"this._body=body===undefined||body===null?'':String(body);"
			"this.status=init.status===undefined?200:init.status;"
			"this.statusText=init.statusText===undefined?'':String(init.statusText);"
			"this.ok=this.status>=200&&this.status<300;"
			"this.headers=new Headers(init.headers);"
			"this.url=init.url||'';"
			"this.type=init.type||'basic';"
			"this.redirected=false;"
			"this.bodyUsed=false;"
		"}"
		"Response.prototype.text=function(){"
			"this.bodyUsed=true;return Promise.resolve(this._body);};"
		"Response.prototype.json=function(){"
			"this.bodyUsed=true;"
			"try{return Promise.resolve(JSON.parse(this._body));}"
			"catch(e){return Promise.reject(e);}};"
		"Response.prototype.clone=function(){"
			"return new Response(this._body,{status:this.status,"
				"statusText:this.statusText,headers:this.headers,"
				"url:this.url});};"
		"Response.error=function(){"
			"var r=new Response('',{status:0});r.type='error';return r;};"
		"this.Response=Response;"
		"})();");

	/* --- AbortController / AbortSignal (fixes1243, #167) ---
	 *
	 * Was an empty `function(){}` stub in the generic DOM-constructor-name
	 * list (fixes1206-era): `new AbortController().signal` was undefined,
	 * `.abort()` a no-op. 17 real call sites across the 18 scripts one
	 * Facebook profile-page load executes; confirmed used with fetch(),
	 * the near-universal pattern.
	 *
	 * Real, spec-shaped behaviour: .aborted / .reason, onabort +
	 * addEventListener('abort', ...), AbortSignal.abort()/timeout()
	 * statics, and fetch() below actually wires it to a REAL cancel --
	 * XMLHttpRequest.prototype.abort() (a few lines up) already calls the
	 * native __xhrNativeAbort, so an abort here really does stop the
	 * in-flight network request, not just settle the JS promise. */
	macsurf_qjs__safe_eval(ctx,
		"(function(g){"
		"\"use strict\";"
		"function mkAbortErr(reason){"
			"if(reason!==undefined&&reason!==null)return reason;"
			"var e=new Error('The operation was aborted.');"
			"e.name='AbortError';return e;}"
		"function AbortSignal(){"
			"this.aborted=false;this.reason=undefined;"
			"this.onabort=null;this._listeners=[];}"
		"AbortSignal.prototype.addEventListener=function(t,fn){"
			"if(t==='abort'&&typeof fn==='function')"
				"this._listeners.push(fn);};"
		"AbortSignal.prototype.removeEventListener=function(t,fn){"
			"if(t!=='abort')return;"
			"var i=this._listeners.indexOf(fn);"
			"if(i>=0)this._listeners.splice(i,1);};"
		"AbortSignal.prototype.throwIfAborted=function(){"
			"if(this.aborted)throw this.reason;};"
		"AbortSignal.prototype._doAbort=function(reason){"
			"if(this.aborted)return;"
			"this.aborted=true;this.reason=mkAbortErr(reason);"
			"var ev={type:'abort',target:this};"
			"if(typeof this.onabort==='function'){"
				"try{this.onabort(ev);}catch(e){}}"
			"var L=this._listeners.slice(),i;"
			"for(i=0;i<L.length;i++){try{L[i](ev);}catch(e){}}};"
		"AbortSignal.abort=function(reason){"
			"var s=new AbortSignal();"
			"s.aborted=true;s.reason=mkAbortErr(reason);return s;};"
		"AbortSignal.timeout=function(ms){"
			"var s=new AbortSignal();"
			"setTimeout(function(){"
				"var e=new Error('The operation timed out.');"
				"e.name='TimeoutError';s._doAbort(e);"
			"},ms);return s;};"
		"function AbortController(){this.signal=new AbortSignal();}"
		"AbortController.prototype.abort=function(reason){"
			"this.signal._doAbort(reason);};"
		"g.AbortSignal=AbortSignal;"
		"g.AbortController=AbortController;"
		"})(this);");

	/* --- fetch() (fixes846, Request/Headers-aware since fixes1140) --- *
	 * A real Promise (QuickJS's native Promise is already an intrinsic --
	 * JS_AddIntrinsicPromise, see qjs_build_context) wrapping the real
	 * async XHR above. Replaces the fake synchronous thenable that always
	 * resolved {ok:false,status:0} regardless of what happened.
	 *
	 * fixes1140: accepts the spec's `fetch(new Request(...))` form as well
	 * as `fetch(url, opts)`, resolves a real Response instance, and reads
	 * headers from a Headers object (whose values are NOT enumerable via
	 * `for in`, which is why the old loop silently sent none). */
	macsurf_qjs__safe_eval(ctx,
		"this.fetch=function(url,opts){"
			"opts=opts||{};"
			"if(url&&typeof url==='object'&&url.url!==undefined){"
				"var rq=url;"
				"url=rq.url;"
				"if(opts.method===undefined)opts.method=rq.method;"
				"if(opts.headers===undefined)opts.headers=rq.headers;"
				"if(opts.body===undefined)opts.body=rq.body;"
				"if(opts.credentials===undefined)opts.credentials=rq.credentials;"
				"if(opts.signal===undefined)opts.signal=rq.signal;"
			"}"
			/* fixes1243 (#167) - already-aborted signal: never even open
			 * the XHR, matching the spec (a fetch given a pre-aborted
			 * signal rejects synchronously-ish, before any network
			 * activity starts). */
			"if(opts.signal&&opts.signal.aborted){"
				"return Promise.reject(opts.signal.reason);}"
			"return new Promise(function(resolve,reject){"
				"try{"
					"var xhr=new XMLHttpRequest();"
					"xhr.open(opts.method||'GET',url,true);"
					"if(opts.headers){"
						"if(typeof opts.headers.forEach==='function'){"
							"opts.headers.forEach(function(v,k){"
								"xhr.setRequestHeader(k,v);});"
						"}else{"
							"for(var h in opts.headers)"
								"xhr.setRequestHeader(h,opts.headers[h]);"
						"}"
					"}"
					/* fixes1243 (#167) - abort mid-flight. xhr.abort()
					 * calls the native __xhrNativeAbort, so this really
					 * stops the request, not just settles the promise. */
					"if(opts.signal){"
						"opts.signal.addEventListener('abort',function(){"
							"try{xhr.abort();}catch(ae){}"
							"reject(opts.signal.reason);"
						"});"
					"}"
					"xhr.onreadystatechange=function(){"
						"if(xhr.readyState!==4)return;"
						"var ok=xhr.status>=200&&xhr.status<300;"
						"if(typeof __workLogFetch==='function')"
							"__workLogFetch(String(url),ok,xhr.status);"
						"if(xhr.status===0){reject(new Error('Network error'));return;}"
						"var respText=xhr.responseText||'';"
						/* fixes1140 - a real Response instance, so
						 * `r instanceof Response` holds and `r.headers`
						 * is a real Headers. Response headers are parsed
						 * from getAllResponseHeaders() when available. */
						"var hdrs=new Headers();"
						"try{"
							"var raw=xhr.getAllResponseHeaders&&xhr.getAllResponseHeaders();"
							"if(raw){"
								"var lines=String(raw).split(/\\r?\\n/);"
								"for(var li=0;li<lines.length;li++){"
									"var ci=lines[li].indexOf(':');"
									"if(ci>0)hdrs.append("
										"lines[li].substr(0,ci).trim(),"
										"lines[li].substr(ci+1).trim());"
								"}"
							"}"
						"}catch(he){}"
						"var resp=new Response(respText,{"
							"status:xhr.status,"
							"statusText:xhr.statusText||'',"
							"headers:hdrs,"
							"url:xhr.responseURL||String(url)"
						"});"
						"resolve(resp);"
					"};"
					"xhr.send(opts.body===undefined?null:opts.body);"
				"}catch(e){reject(e);}"
			"});"
		"};");

	/* --- localStorage / sessionStorage ---
	 * localStorage persists per origin: the saved JSON map is loaded via
	 * __storageLoad at realm build and rewritten via __storageSave after
	 * every mutation (setItem/removeItem/clear). sessionStorage stays
	 * in-memory on purpose (per-spec it is per-tab, and with one realm per
	 * navigation it reads as a fresh session on each page load). Non-Mac
	 * builds have no __storage* natives, so the harness keeps the old
	 * in-memory behaviour exactly. */
	macsurf_qjs__safe_eval(ctx,
		"function _Storage(){this._m={};}"
		"_Storage.prototype.getItem=function(k){"
			"return k in this._m?this._m[k]:null;};"
		"_Storage.prototype.setItem=function(k,v){"
			"this._m[k]=String(v);this._save();};"
		"_Storage.prototype.removeItem=function(k){"
			"delete this._m[k];this._save();};"
		"_Storage.prototype.clear=function(){"
			"this._m={};this._save();};"
		"_Storage.prototype._save=function(){"
			"if(!this._persist||typeof __storageSave!=='function')return;"
			"try{__storageSave(JSON.stringify(this._m));}catch(e){}"
		"};"
		"_Storage.prototype.key=function(i){"
			"var ks=Object.keys(this._m);return ks[i]||null;};"
		"Object.defineProperty(_Storage.prototype,'length',{"
			"get:function(){return Object.keys(this._m).length;}});"
		"this.localStorage=new _Storage();"
		"this.localStorage._persist=true;"
		"if(typeof __storageLoad==='function'){"
			"try{"
				"var _ld=__storageLoad();"
				"if(_ld){"
					"var _o=JSON.parse(_ld);"
					"var _k;"
					"for(_k in _o){"
						"if(Object.prototype.hasOwnProperty.call(_o,_k))"
							"this.localStorage._m[_k]=_o[_k];"
					"}"
				"}"
			"}catch(e){}"
		"}"
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
				"if(Array.isArray(init)){"
					"for(var ai=0;ai<init.length;ai++)"
						"if(init[ai]&&init[ai].length>=2)"
							"this._m[String(init[ai][0])]=String(init[ai][1]);"
				"}else if(typeof init==='object'){"
					"for(var k in init)"
						"if(Object.prototype.hasOwnProperty.call(init,k))"
							"this._m[String(k)]=String(init[k]);"
				"}else{"
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
	qjs_set_func(ctx, history_obj, "__setUrlDisplay", qjs_history_set_url_display, 1);
	JS_SetPropertyStr(ctx, history_obj, "length", JS_NewInt32(ctx, 0));
	JS_SetPropertyStr(ctx, global, "history", history_obj);

	/* pushState/replaceState Phase A (fixes1198): update the URL bar and
	 * history.state without navigating. No session-history entry is
	 * pushed, so history.length stays 0 and back()/forward() cannot
	 * return to a pushState URL yet -- that (plus popstate) is Phase B. */
	macsurf_qjs__safe_eval(ctx,
		"(function(){"
		"  if(typeof history==='undefined')return;"
		"  var _state=null;"
		"  history.pushState=history.pushState||function(s,t,u){"
		"    _state=s;"
		"    if(u!==undefined&&u!==null)history.__setUrlDisplay(String(u));"
		"  };"
		"  history.replaceState=history.replaceState||function(s,t,u){"
		"    _state=s;"
		"    if(u!==undefined&&u!==null)history.__setUrlDisplay(String(u));"
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
	 * fixes879 - MUST stay after the JS_SetPropertyStr(global,"navigator")
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
			"navigator.sendBeacon=function(url,data){"
				"if(typeof url==='undefined'||url===null)return false;"
				"var d='';"
				"if(typeof data!=='undefined'&&data!==null){"
					"try{d=String(data);}catch(e){return false;}"
				"}"
				"if(typeof __beaconSend==='function'){"
					"try{return !!__beaconSend(String(url),d);}"
					"catch(e){return false;}"
				"}"
				"return false;"
			"};"
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
		"if(typeof g.navigator!=='undefined'&&typeof g.navigator.sendBeacon!=='function'){"
			"g.navigator.sendBeacon=function(url,data){"
				"if(typeof url==='undefined'||url===null)return false;"
				"var d='';"
				"if(typeof data!=='undefined'&&data!==null){"
					"try{d=String(data);}catch(e){return false;}"
				"}"
				"if(typeof g.__beaconSend==='function'){"
					"try{return !!g.__beaconSend(String(url),d);}"
					"catch(e){return false;}"
				"}"
				"return false;"
			"};"
		"}"
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
		/* fixes1002 (#182) - the mock's appendChild used to be
		 * `function(c){return c;}`: it returned the child WITHOUT
		 * attaching it, so c.parentNode stayed null. That is the whole
		 * XenForo cascade, now visible on hardware thanks to fixes1000:
		 *   TypeError: cannot read property 'removeChild' of null
		 *   at hiddenscroll (preamble.min.js:4:427)
		 * because the probe does appendChild(b) ... b.parentNode.
		 * removeChild(b) -- the universal append-measure-remove idiom.
		 * This mock is reached when a script runs BEFORE <body> exists
		 * (#182), so document.body has no real node to give.
		 *
		 * defineProperty, not assignment: on a REAL element wrapper
		 * parentNode is a getter-only accessor from the prototype, so
		 * `c.parentNode = this` silently does nothing (or throws under
		 * strict mode). An own data property shadows the accessor for
		 * this one node, which is exactly the scope wanted -- and it is
		 * reverted on removeChild, so nothing leaks a fake parent after
		 * the probe finishes. Wrapped in try/catch because a frozen or
		 * exotic object must not take the page down. */
		"appendChild:function(c){if(c){try{Object.defineProperty(c,'parentNode',"
			"{value:this,writable:true,configurable:true});}catch(e){}}return c;},"
		"removeChild:function(c){if(c){try{Object.defineProperty(c,'parentNode',"
			"{value:null,writable:true,configurable:true});}catch(e){}}return c;},"
		"insertBefore:function(c){if(c){try{Object.defineProperty(c,'parentNode',"
			"{value:this,writable:true,configurable:true});}catch(e){}}return c;},"
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
	 *     crypto.getRandomValues, caches, Blob, File, FileReader,
	 *     URL.createObjectURL) ---
	 *
	 * fixes882: MediaSource was listed here and is NOT defined by the block
	 * below -- the name appears nowhere else in this file, so `MediaSource` is
	 * simply undefined at runtime. The only media-adjacent things installed are
	 * the bare HTMLVideoElement/HTMLAudioElement/HTMLMediaElement/
	 * HTMLSourceElement constructors further down, which exist purely so
	 * `typeof` checks do not throw. Listing a stub that was never written is
	 * worse than listing nothing: it reads as "handled" to anyone grepping. */
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
		/* fixes1243 (#167) - AbortController/AbortSignal removed from this
		 * empty-stub list: they get a real implementation below, next to
		 * fetch(). Left in this list `new AbortController().signal` was
		 * undefined and `controller.abort()` a no-op -- any page code that
		 * checked `signal.aborted` or called `signal.addEventListener`
		 * threw "cannot read property of undefined". */
		"'DOMParser','XMLSerializer','TreeWalker','NodeIterator'];"
		"var i;"
		"for(i=0;i<names.length;i++){"
		"if(typeof g[names[i]]==='undefined'){"
		"g[names[i]]=function(){};"
		"}"
		"}"
		/* fixes1144 - NodeFilter constants. Froala editor 4.2.1
		 * accesses NodeFilter.SHOW_TEXT in its TreeWalker init;
		 * NodeFilter must be an object with the spec's constant
		 * values, not a constructor stub. */
		"if(typeof g.NodeFilter==='undefined'){"
		"g.NodeFilter={FILTER_ACCEPT:1,FILTER_REJECT:2,FILTER_SKIP:3,"
		"SHOW_ALL:-1,SHOW_ELEMENT:1,SHOW_ATTRIBUTE:2,SHOW_TEXT:4,"
		"SHOW_CDATA_SECTION:8,SHOW_PROCESSING_INSTRUCTION:64,"
		"SHOW_COMMENT:128,SHOW_DOCUMENT:256,"
		"SHOW_DOCUMENT_TYPE:512,SHOW_DOCUMENT_FRAGMENT:1024};"
		"}"
		/* fixes1145 - DOMPurify stub. Froala 4.2.1 requires
		 * window.DOMPurify.sanitize() for XSS sanitization before
		 * enabling rich-text mode. Without it the editor degrades
		 * to a plain textarea. On MacSurf, innerHTML already does
		 * not execute scripts, so sanitize is a safe pass-through. */
		"if(typeof g.DOMPurify==='undefined'){"
		"g.DOMPurify={sanitize:function(d,c){return d||'';},"
		"addHook:function(){},removeHook:function(){},"
		"isSupported:true,version:'macsurf',"
		"removed:[]};"
		"}"
		/* fixes1127 -- chain the family BELOW the wrapper class proto.
		 * qjs_el_install_proto re-points each per-tag HTML* constructor's
		 * .prototype.__proto__ at the wrapper class proto p (so a wrapper
		 * whose own proto is HTMLDivElement.prototype keeps p and the
		 * on* accessors in its chain); the links here are the parts the
		 * class proto itself routes through: p -> HTMLElement ->
		 * Element -> Node, and the Text/CharacterData/DocumentFragment
		 * families that are NOT elements.  Real wrappers get their own
		 * prototype per node shape in qjs_wrap_element /
		 * qjs_wrap_text_node / qjs_wrap_fragment. */
		"if(g.Node){"
		"if(g.Element&&g.Element.prototype)"
			"g.Element.prototype.__proto__=g.Node.prototype;"
		"if(g.CharacterData&&g.CharacterData.prototype)"
			"g.CharacterData.prototype.__proto__=g.Node.prototype;"
		"if(g.DocumentFragment&&g.DocumentFragment.prototype)"
			"g.DocumentFragment.prototype.__proto__=g.Node.prototype;"
		/* fixes1146 - Node type constants. XenForo core-compiled.js
		 * accesses Node.ELEMENT_NODE during initialization; without
		 * these every instanceof check and nodeType comparison that
		 * uses the named constants throws. */
		"g.Node.ELEMENT_NODE=1;"
		"g.Node.ATTRIBUTE_NODE=2;"
		"g.Node.TEXT_NODE=3;"
		"g.Node.CDATA_SECTION_NODE=4;"
		"g.Node.PROCESSING_INSTRUCTION_NODE=7;"
		"g.Node.COMMENT_NODE=8;"
		"g.Node.DOCUMENT_NODE=9;"
		"g.Node.DOCUMENT_TYPE_NODE=10;"
		"g.Node.DOCUMENT_FRAGMENT_NODE=11;"
		"}"
		"if(g.Element&&g.Element.prototype){"
		"if(g.HTMLElement&&g.HTMLElement.prototype)"
			"g.HTMLElement.prototype.__proto__=g.Element.prototype;"
		"if(g.SVGElement&&g.SVGElement.prototype)"
			"g.SVGElement.prototype.__proto__=g.Element.prototype;"
		"}"
		"if(g.CharacterData&&g.CharacterData.prototype){"
		"if(g.Text&&g.Text.prototype)"
			"g.Text.prototype.__proto__=g.CharacterData.prototype;"
		"if(g.Comment&&g.Comment.prototype)"
			"g.Comment.prototype.__proto__=g.CharacterData.prototype;"
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
		/* fixes1244 (#167) - queueMicrotask was routed through `soon`
		 * (setTimeout(fn,0)), i.e. a MACROtask -- spec requires it run at
		 * Promise-reaction priority: before any timer, interleaved with
		 * .then() callbacks, in the SAME turn of the event loop as
		 * whatever scheduled it. A boot sequence using queueMicrotask to
		 * defer init by exactly one microtask (a common React/Comet-style
		 * idiom -- e.g. `if (ready) queueMicrotask(runInit); else
		 * addEventListener('load', () => queueMicrotask(runInit))`)
		 * instead had that work pushed behind every pending timer AND
		 * reordered relative to whatever Promise chains were already
		 * queued -- a real, observable ordering bug, not just a naming
		 * nitpick. Promise.resolve().then(fn) is a genuine QuickJS
		 * microtask (JS_AddIntrinsicPromise's real job queue, the same
		 * one macsurf_qjs_pump_all drains every tick), so this now has
		 * correct relative ordering. A throwing callback is caught and
		 * logged directly rather than left to become an "unhandled
		 * rejection" on a promise nothing else references -- closer to
		 * spec (report-the-exception), and a clearer log line. */
		"if(typeof g.queueMicrotask!=='function'){"
		"g.queueMicrotask=function(fn){"
		"Promise.resolve().then(function(){"
		"try{fn();}catch(e){"
		"try{if(typeof __msLife==='function')"
			"__msLife('queueMicrotask threw: '+"
				"((e&&e.message)||e));}catch(_){}"
		"}"
		"});"
		"};}"
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
		/* fixes1147 - persistent Selection + full Range stub + contentEditable.
		 * Froala 4.2.1 needs getSelection() to return a STABLE object whose
		 * getRangeAt(0) returns a non-null Range with all the spec methods,
		 * and it needs element.contentEditable/isContentEditable to decide
		 * whether to use rich-text or fall back to plain textarea. */
		"var __macsurf_sel=null;"
		"var __macsurf_makeRange=function(){"
		"return{"
		"startContainer:null,endContainer:null,"
		"startOffset:0,endOffset:0,"
		"collapsed:true,commonAncestorContainer:null,"
		"setStart:function(sc,so){this.startContainer=sc;this.startOffset=so;},"
		"setEnd:function(ec,eo){this.endContainer=ec;this.endOffset=eo;},"
		"setStartBefore:function(n){this.startContainer=n.parentNode;this.startOffset=0;},"
		"setStartAfter:function(n){this.startContainer=n.parentNode;this.startOffset=1;},"
		"setEndBefore:function(n){this.endContainer=n.parentNode;this.endOffset=0;},"
		"setEndAfter:function(n){this.endContainer=n.parentNode;this.endOffset=1;},"
		"selectNode:function(n){this.startContainer=n.parentNode;this.startOffset=0;"
			"this.endContainer=n.parentNode;this.endOffset=1;this.collapsed=false;},"
		"selectNodeContents:function(n){this.startContainer=n;this.startOffset=0;"
			"this.endContainer=n;this.endOffset=(n.childNodes||[]).length;this.collapsed=false;},"
		"collapse:function(toStart){if(toStart){this.endContainer=this.startContainer;"
			"this.endOffset=this.startOffset;}else{this.startContainer=this.endContainer;"
			"this.startOffset=this.endOffset;}this.collapsed=true;},"
		"cloneRange:function(){var r=__macsurf_makeRange();"
			"r.startContainer=this.startContainer;r.startOffset=this.startOffset;"
			"r.endContainer=this.endContainer;r.endOffset=this.endOffset;"
			"r.collapsed=this.collapsed;return r;},"
		"deleteContents:function(){},"
		"extractContents:function(){return g.document.createDocumentFragment?g.document.createDocumentFragment():null;},"
		"insertNode:function(n){},"
		"surroundContents:function(n){},"
		"compareBoundaryPoints:function(how,src){return 0;},"
		"getBoundingClientRect:function(){return{top:0,left:0,right:0,bottom:0,width:0,height:0};},"
		"getClientRects:function(){return[];},"
		"toString:function(){return'';},"
		"detach:function(){}};};"
		"g.Range=function(){return __macsurf_makeRange();};"
		"if(typeof g.getSelection!=='function'){"
		"g.getSelection=function(){"
			"if(__macsurf_sel===null){"
				"__macsurf_sel={"
				"anchorNode:null,anchorOffset:0,"
				"focusNode:null,focusOffset:0,"
				"isCollapsed:true,rangeCount:0,type:'None',"
				"_range:__macsurf_makeRange(),"
				"toString:function(){return '';},"
				"addRange:function(r){this._range=r||__macsurf_makeRange();"
					"this.rangeCount=1;this.type='Range';this.isCollapsed=r?r.collapsed:true;},"
				"removeAllRanges:function(){this.rangeCount=0;this.type='None';},"
				"getRangeAt:function(i){return this.rangeCount>0?this._range:null;},"
				"collapse:function(n,o){this._range.setStart(n,o);this._range.setEnd(n,o);"
					"this.isCollapsed=true;},"
				"collapseToStart:function(){this.isCollapsed=true;},"
				"collapseToEnd:function(){this.isCollapsed=true;},"
				"extend:function(n,o){this.focusNode=n;this.focusOffset=o;this.isCollapsed=false;},"
				"selectAllChildren:function(n){this._range.selectNodeContents(n);"
					"this.rangeCount=1;this.type='Range';},"
				"deleteFromDocument:function(){},"
				"modify:function(){},"
				"setBaseAndExtent:function(an,ao,fn,fo){"
					"this.anchorNode=an;this.anchorOffset=ao;"
					"this.focusNode=fn;this.focusOffset=fo;},"
				"containsNode:function(){return false;},"
				"setPosition:function(n,o){this._range.setStart(n,o);"
					"this._range.setEnd(n,o);}"
				"};"
			"}"
			"return __macsurf_sel;"
		"};}"
		/* fixes1147 - element.contentEditable + isContentEditable.
		 * Froala 4.2.1 checks these to decide between rich-text (div
		 * with contenteditable) and plain textarea fallback. */
		"if(g.Element&&g.Element.prototype){"
		"if(!('contentEditable'in g.Element.prototype)){"
			"Object.defineProperty(g.Element.prototype,'contentEditable',{"
			"get:function(){return this.getAttribute?this.getAttribute('contenteditable')||'inherit':'inherit';},"
			"set:function(v){if(this.setAttribute)this.setAttribute('contenteditable',String(v));},"
			"enumerable:false,configurable:true});"
		"}"
		"if(!('isContentEditable'in g.Element.prototype)){"
			"Object.defineProperty(g.Element.prototype,'isContentEditable',{"
			"get:function(){var ce=this.contentEditable;return ce==='true'||ce==='';},"
			"enumerable:false,configurable:true});"
		"}"
		"}"
		"if(typeof g.document!=='undefined'){"
		"if(typeof g.document.createRange!=='function'){"
		"g.document.createRange=function(){return __macsurf_makeRange();};"
		"}"
		"if(typeof g.document.execCommand!=='function'){g.document.execCommand=function(){return false;};}"
		"if(typeof g.document.queryCommandSupported!=='function'){g.document.queryCommandSupported=function(cmd){"
			"return cmd==='insertUnorderedList'||cmd==='insertOrderedList'||cmd==='bold'||"
			"cmd==='italic'||cmd==='underline'||cmd==='strikethrough'||"
			"cmd==='createLink'||cmd==='unlink'||cmd==='justifyLeft'||"
			"cmd==='justifyCenter'||cmd==='justifyRight'||cmd==='justifyFull'||"
			"cmd==='indent'||cmd==='outdent'||cmd==='undo'||cmd==='redo'||"
			"cmd==='fontSize'||cmd==='fontName'||cmd==='foreColor'||"
			"cmd==='backColor'||cmd==='removeFormat'||cmd==='insertImage';};}"
		"if(typeof g.document.queryCommandEnabled!=='function'){g.document.queryCommandEnabled=function(){return true;};}"
		"if(!('activeElement'in g.document)){g.document.activeElement=null;}"
		"if(typeof g.document.getSelection!=='function'){g.document.getSelection=g.getSelection;}"
		/* fixes1149 - document methods Froala 4.2.1 requires.
		 * createTreeWalker is the critical one: Froala calls it at
		 * line 248 and throws 'not a function' without it.
		 * Return a minimal walker with nextNode() that walks all
		 * descendants in preorder, respecting the whatToShow flags. */
		"if(typeof g.document.createTreeWalker!=='function'){"
		"g.document.createTreeWalker=function(root,whatToShow,filter){"
		"var node=root, first=true;"
		"return{"
		"root:root,whatToShow:whatToShow||0,filter:filter||null,"
		"currentNode:root,"
		"nextNode:function(){"
		"var n;"
		"if(first){first=false;"
		"n=(whatToShow&4&&root.nodeType===3)?root:null;"
		"if(!n)n=this._next(root);"
		"if(n)this.currentNode=n;return n;"
		"}else{"
		"n=this._next(this.currentNode);"
		"if(n)this.currentNode=n;return n;"
		"}},"
		"_next:function(start){"
		"var n=start.firstChild;"
		"while(n||start!==root){"
		"if(n){"
		"if((whatToShow&1&&n.nodeType===1)||"
		"(whatToShow&4&&n.nodeType===3)){"
		"if(!filter||filter(n)===1)return n;"
		"}n=n.firstChild||n.nextSibling;"
		"}else{"
		"n=start.nextSibling;"
		"if(n){"
		"if((whatToShow&1&&n.nodeType===1)||"
		"(whatToShow&4&&n.nodeType===3)){"
		"if(!filter||filter(n)===1)return n;"
		"}start=n;n=n.firstChild;"
		"}else{start=start.parentNode;n=null;}"
		"}}return null;},"
		"previousNode:function(){return null;},"
		"parentNode:function(){return null;},"
		"firstChild:function(){return null;},"
		"lastChild:function(){return null;},"
		"previousSibling:function(){return null;},"
		"nextSibling:function(){return null;}};};}"
		"if(typeof g.document.createNodeIterator!=='function'){"
		"g.document.createNodeIterator=function(root,whatToShow,filter){"
		"return g.document.createTreeWalker(root,whatToShow,filter);};}"
		"if(typeof g.document.createEvent!=='function'){"
		"g.document.createEvent=function(type){"
		"var e=null;"
		"if(type==='MouseEvents'||type==='MouseEvent'){"
		"e={type:'',bubbles:false,cancelable:false,"
		"initMouseEvent:function(t,b,c){this.type=t;this.bubbles=b;this.cancelable=c;}};"
		"}else if(type==='Event'||type==='Events'||type==='HTMLEvents'){"
		"e={type:'',bubbles:false,cancelable:false,"
		"initEvent:function(t,b,c){this.type=t;this.bubbles=b;this.cancelable=c;}};"
		"}else{e={type:'',bubbles:false,cancelable:false};}"
		"if(e&&!e.preventDefault)e.preventDefault=function(){};"
		"if(e&&!e.stopPropagation)e.stopPropagation=function(){};"
		"return e;};}"
		"if(typeof g.document.hasFocus!=='function'){"
		"g.document.hasFocus=function(){return true;};}"
		"if(!('scrollingElement'in g.document)){"
		"g.document.scrollingElement=g.document.documentElement||g.document.body;}"
		"if(!('implementation'in g.document)){"
		"g.document.implementation={createHTMLDocument:function(t){return g.document;},"
		"createDocument:function(){return g.document;},"
		"hasFeature:function(){return true;}};}"
		"if(!('domain'in g.document)){g.document.domain='';}"
		"if(!('doctype'in g.document)){g.document.doctype=null;}"
		"}"
		/* fixes1149 - rAF, MouseEvent, and other globals Froala needs.
		 * fixes1236 (#167) - the rAF fallback here is confirmed DEAD under
		 * normal load: register_browser_globals installs a real
		 * requestAnimationFrame earlier in this same function (~line 9159)
		 * before this guarded block runs in the same pass, so
		 * `typeof g.requestAnimationFrame!=='function'` is always false
		 * here. Left as a genuine fallback (only fires if the earlier eval
		 * itself failed), not because the two definitions compete. */
		"if(typeof g.requestAnimationFrame!=='function'){"
		"g.requestAnimationFrame=function(fn){return g.setTimeout(function(){fn(Date.now());},16);};}"
		"if(typeof g.cancelAnimationFrame!=='function'){"
		"g.cancelAnimationFrame=function(id){g.clearTimeout(id);};}"
		"if(typeof g.MouseEvent!=='function'){"
		"g.MouseEvent=function(type,opts){opts=opts||{};var e={type:type||'',"
		"bubbles:opts.bubbles||false,cancelable:opts.cancelable||false,"
		"clientX:opts.clientX||0,clientY:opts.clientY||0,"
		"screenX:opts.screenX||0,screenY:opts.screenY||0,"
		"button:opts.button||0,which:opts.which||1,"
		"altKey:!!opts.altKey,ctrlKey:!!opts.ctrlKey,"
		"shiftKey:!!opts.shiftKey,metaKey:!!opts.metaKey};"
		"e.preventDefault=function(){};e.stopPropagation=function(){};"
		"return e;};}"
		"if(typeof g.FocusEvent!=='function'){"
		"g.FocusEvent=function(type,opts){opts=opts||{};return{type:type||'',"
		"bubbles:!!opts.bubbles,cancelable:!!opts.cancelable,"
		"relatedTarget:opts.relatedTarget||null};};}"
		"if(typeof g.KeyboardEvent!=='function'){"
		"g.KeyboardEvent=function(type,opts){opts=opts||{};return{type:type||'',"
		"key:opts.key||'',code:opts.code||'',"
		"ctrlKey:!!opts.ctrlKey,shiftKey:!!opts.shiftKey,"
		"altKey:!!opts.altKey,metaKey:!!opts.metaKey};};}"
		"if(typeof g.InputEvent!=='function'){"
		"g.InputEvent=function(type,opts){opts=opts||{};return{type:type||'',"
		"data:opts.data||null,inputType:opts.inputType||''};};}"
		"})(this);");

	/* --- XF LazyHandlerLoader diagnostic (68kmla post-thread button) ---
	 *
	 * Answers: WHAT throws "TypeError: not a function [setTimeout]" at
	 * core-compiled.js:108 (e.matches(k) inside LazyHandlerLoader's
	 * collector f) 14x/session on 68kmla.org, which blocks lazy handler
	 * init and leaves the post-thread button dead.
	 *
	 * v4, timer-free.  A setter trap on globalThis.XF wraps the real
	 * machinery the moment XF is assigned; a microtask queued from the
	 * setter drains between scripts (after the preamble's IIFE added
	 * XF.ready); XF.ready/XF.activate are wrapped so the bundle's own
	 * ready(XF.onPageLoad) call at core-compiled:216 re-arms install
	 * once the REAL LazyHandlerLoader exists (line 107); DOMContentLoaded
	 * (listener on BOTH document and window -- js_fire_dom_ready
	 * dispatches at both) is the backstop.
	 *
	 * v4 addition -- the trap is NOT the only arming path: hardware
	 * showed ZERO 'XF LAZY' lines though the harness (same bundles, same
	 * engine, same trap) arms fine, i.e. the setter appears to never fire
	 * on the Mac, and the trap's getter then shadows globalThis.XF with
	 * undefined forever.  retry() therefore ALSO reads `typeof XF` --
	 * identifier resolution reaches the preamble's `const XF={}` GLOBAL
	 * LEXICAL binding, visible regardless of the property trap -- and
	 * falls back to window.XF/globalThis.XF.  So the DOMContentLoaded
	 * backstop arms install() even on a realm where the setter never
	 * fires, and install() runs before XF's own domready handler fires
	 * loadLazyHandlers (this listener registered before any page script).
	 *
	 * NO setTimeout/setInterval anywhere: a probe timer
	 * would land in the timer arena, go gen-stale on realm teardown and
	 * abort the harness at JS_FreeRuntime (gc_obj_list leak).
	 *
	 * Logs (all "XF LAZY " via __msLife, so they ride the LIFE gate):
	 *  installed                        -- probe armed on this realm
	 *  call nodeType=.. tag=.. .cls matches=fn qsa=fn docEl=.. found=init:a,click:b regN=7 reg=a,b
	 *                                     -- per loadLazyHandlers call: the
	 *                                     container identity + every
	 *                                     data-xf-init/data-xf-click name
	 *                                     under it + handler census
	 *  THROW <same> msg=.. stack=..     -- the caught exception, rethrown
	 *  APPLY <name> ctor=<type>         -- applyHandler resolved a non-ctor
	 *  CLASSMAP <name> -> <type> NONCTOR -- getObjectFromIdentifier returned a
	 *                                     non-function (the new k(...) site)
	 *                                     -- the line-108 "not a function"
	 *                                     signature when the registered ctor
	 *                                     is bogus
	 */
	macsurf_qjs__safe_eval(ctx,
		"(function(){"
		"function life(s){try{__msLife('XF LAZY '+s);}catch(e){}}"
		"var names=[],lastN=-1;"
		"function regDump(){"
		"	if(names.length!==lastN){"
		"		lastN=names.length;"
		"		return ' regN='+names.length+(names.length?' reg='+names.slice(0,120).join(','):'');"
		"	}"
		"	return '';"
		"}"
		"var xfVal=undefined,haveXf=false;"
		"function tryWrapReady(XF){"
		"	if(typeof XF.ready!=='function'||XF.ready.__msProbed)return;"
		"	XF.ready.__msProbed=true;"
		"	var orig=XF.ready;"
		"	XF.ready=function(f){install();return orig.apply(this,arguments);};"
		"}"
		"function tryWrapActivate(XF){"
		"	if(typeof XF.activate!=='function'||XF.activate.__msProbed)return;"
		"	XF.activate.__msProbed=true;"
		"	var orig=XF.activate;"
		"	XF.activate=function(el){install();return orig.apply(this,arguments);};"
		"}"
		"function install(){"
		"	if(!haveXf||!xfVal)return false;"
		"	var XF=xfVal;"
		"	tryWrapReady(XF);"
		"	tryWrapActivate(XF);"
		"	var LL=XF.LazyHandlerLoader;"
		"	if(!LL||typeof LL.loadLazyHandlers!=='function')return false;"
		"	if(LL.__msProbed)return true;"
		"	LL.__msProbed=true;"
		"	if(XF.Element&&typeof XF.Element.register==='function'&&!XF.Element.__msRegProbed){"
		"		XF.Element.__msRegProbed=true;"
		"		var origReg=XF.Element.register;"
		"		XF.Element.register=function(n,c){"
		"			names.push(String(n));"
		"			return origReg.apply(this,arguments);"
		"		};"
		"	}"
		"	var origLL=LL.loadLazyHandlers;"
		"	LL.loadLazyHandlers=function(c){"
		"		var info='?';"
		"		try{"
		"			var t=(c&&c.nodeType!==undefined)?String(c.nodeType):typeof c;"
		"			var tag=(c&&c.tagName)?String(c.tagName):(c&&c.nodeType===9?'DOCUMENT':String(c));"
		"			var cls=(c&&c.className)?' .'+String(c.className).split(' ').join(' .'):'';"
		"			var mt=(c)?typeof c.matches:'null-container';"
		"			var qsa=(c)?typeof c.querySelectorAll:'null-container';"
		"			var docEl='-';"
		"			if(c&&c.nodeType===9)docEl=(c.documentElement&&c.documentElement.tagName)||'NULL';"
		"			var found='';"
		"			if(c&&qsa==='function'){"
		"				var els=c.querySelectorAll('[data-xf-init]');"
		"				var i,v,arr=[];"
		"				for(i=0;i<els.length;i++){"
		"					v=els[i].getAttribute('data-xf-init');"
		"					if(v)arr.push('init:'+v);"
		"				}"
		"				els=c.querySelectorAll('[data-xf-click]');"
		"				for(i=0;i<els.length;i++){"
		"					v=els[i].getAttribute('data-xf-click');"
		"					if(v)arr.push('click:'+v);"
		"				}"
		"				found=' found='+arr.join(',');"
		"			}"
		"			info='nodeType='+t+' tag='+tag+cls+' matches='+mt+' qsa='+qsa+' docEl='+docEl+found;"
		"		}catch(e){info='LOGERR';}"
		"		life('call '+info+regDump());"
		"		try{return origLL.apply(this,arguments);}"
		"		catch(e){"
		"			life('THROW '+info+' msg='+((e&&e.message)||e)"
		"				+' stack='+String((e&&e.stack)||'').split('\\n').slice(0,3).join(' | '));"
		"			throw e;"
		"		}"
		"	};"
		"	if(XF.Element&&typeof XF.Element.applyHandler==='function'&&!XF.Element.__msProbedAH){"
		"		XF.Element.__msProbedAH=true;"
		"		var origAH=XF.Element.applyHandler;"
		"		XF.Element.applyHandler=function(el,name,opts){"
		"			var kt='?';"
		"			try{kt=typeof XF.Element.getObjectFromIdentifier(String(name));}catch(e){kt='err';}"
		"			if(kt!=='function')life('APPLY '+name+' ctor='+kt);"
		"			return origAH.apply(this,arguments);"
		"		};"
		"	}"
		"	if(XF.ClassMapper&&XF.ClassMapper.prototype&&"
		"		typeof XF.ClassMapper.prototype.getObjectFromIdentifier==='function'&&"
		"		!XF.ClassMapper.prototype.__msProbed){"
		"		XF.ClassMapper.prototype.__msProbed=true;"
		"		var origG=XF.ClassMapper.prototype.getObjectFromIdentifier;"
		"		XF.ClassMapper.prototype.getObjectFromIdentifier=function(name){"
		"			var r=origG.apply(this,arguments);"
		"			if(r!=null&&typeof r!=='function')life('CLASSMAP '+name+' -> '+typeof r+' NONCTOR');"
		"			return r;"
		"		};"
		"	}"
		"	life('installed');"
		"	return true;"
		"}"
		"function retry(){"
		"	/* v4 -- never depend on the setter trap ALONE.  Observed on"
		"	 * hardware: zero 'XF LAZY' lines though the harness (same bundles,"
		"	 * same engine, same trap) arms fine -- the setter appears to never"
		"	 * fire on the Mac.  With the trap's getter shadowing globalThis.XF,"
		"	 * every read of window.XF/globalThis.XF then returns undefined"
		"	 * forever and the probe is permanently mute: haveXf is only ever"
		"	 * set by the setter.  The rescue: the preamble declares"
		"	 * `const XF={}` -- a GLOBAL LEXICAL binding, which identifier"
		"	 * resolution in this closure reaches through the lexical"
		"	 * environment REGARDLESS of the property trap.  Read THAT first;"
		"	 * the window.XF/globalThis.XF reads cover the property-assignment"
		"	 * case (they work whenever the setter fired, i.e. the harness). */"
		"	if(!haveXf||!xfVal){"
		"		try{"
		"			if(typeof XF!=='undefined'&&XF){xfVal=XF;haveXf=true;}"
		"			else if(typeof window!=='undefined'&&window.XF){xfVal=window.XF;haveXf=true;}"
		"			else if(typeof globalThis!=='undefined'&&globalThis.XF){xfVal=globalThis.XF;haveXf=true;}"
		"		}catch(e){}"
		"	}"
		"	install();"
		"}"
		"try{"
		"	Object.defineProperty(globalThis,'XF',{"
		"		configurable:true,enumerable:true,"
		"		get:function(){return xfVal;},"
		"		set:function(v){"
		"			xfVal=v;haveXf=!!v;retry();"
		"			/* retry again after the assigning script completes: the"
		"			 * preamble assigns XF EMPTY, then its IIFE adds ready/Feature;"
		"			 * a microtask queued here drains between scripts, when XF is"
		"			 * fully populated. (An eval-time microtask is NOT enough --"
		"			 * it drains before any page script runs.) */"
		"			if(v&&typeof Promise!=='undefined'&&typeof Promise.resolve==='function'){"
		"				Promise.resolve().then(function(){retry();});"
		"			}"
		"		}"
		"	});"
		"	if(typeof XF!=='undefined'&&XF){xfVal=XF;haveXf=true;retry();}"
		"	if(typeof Promise!=='undefined'&&typeof Promise.resolve==='function'){"
		"		Promise.resolve().then(function(){retry();});"
		"	}"
		"	if(typeof document!=='undefined'&&document.addEventListener){"
		"		try{document.addEventListener('DOMContentLoaded',function(){retry();});}catch(e){}"
		"	}"
		"	if(typeof window!=='undefined'&&window.addEventListener){"
		"		try{window.addEventListener('DOMContentLoaded',function(){retry();});}catch(e){}"
		"	}"
		"}catch(e){}"
		"})();");

	/* fixes1146 - ensure Node constants are set from C.  The JS-side
	 * safe_eval above sets them inside `if(g.Node)`, but Node may be
	 * undefined in timer callbacks or iframe contexts.  Setting them
	 * here with the C API guarantees they survive regardless of how
	 * the page accesses Node. */
	{
		JSValue node = JS_GetPropertyStr(ctx, global, "Node");
		if (!JS_IsUndefined(node) && !JS_IsNull(node)) {
			JS_SetPropertyStr(ctx, node,
				"ELEMENT_NODE", JS_NewInt32(ctx, 1));
			JS_SetPropertyStr(ctx, node,
				"ATTRIBUTE_NODE", JS_NewInt32(ctx, 2));
			JS_SetPropertyStr(ctx, node,
				"TEXT_NODE", JS_NewInt32(ctx, 3));
			JS_SetPropertyStr(ctx, node,
				"CDATA_SECTION_NODE", JS_NewInt32(ctx, 4));
			JS_SetPropertyStr(ctx, node,
				"PROCESSING_INSTRUCTION_NODE",
				JS_NewInt32(ctx, 7));
			JS_SetPropertyStr(ctx, node,
				"COMMENT_NODE", JS_NewInt32(ctx, 8));
			JS_SetPropertyStr(ctx, node,
				"DOCUMENT_NODE", JS_NewInt32(ctx, 9));
			JS_SetPropertyStr(ctx, node,
				"DOCUMENT_TYPE_NODE", JS_NewInt32(ctx, 10));
			JS_SetPropertyStr(ctx, node,
				"DOCUMENT_FRAGMENT_NODE",
				JS_NewInt32(ctx, 11));
		}
		JS_FreeValue(ctx, node);
	}

	/* fixes1147b - verify DOM constructors survived setup + add
	 * XF compatibility aliases.  XF's minified code accesses
	 * HTML_Element (underscore) and HTMLGElement; these are not
	 * real spec names but the code tries instanceof checks on
	 * them.  Emit a one-line census at context creation so the
	 * hardware log confirms every constructor is callable. */
	{
		static const char *const ctor_names[] = {
			"Node", "Element", "HTMLElement",
			"HTMLDivElement", "HTMLSpanElement",
			"HTMLInputElement", "HTMLTextAreaElement",
			"HTMLButtonElement", "HTMLFormElement",
			"HTMLAnchorElement", "HTMLImageElement",
			NULL
		};
		int all_ok = 1;
		const char *const *cn;
		for (cn = ctor_names; *cn != NULL; cn++) {
			JSValue cv = JS_GetPropertyStr(ctx, global, *cn);
			int is_func = JS_IsFunction(ctx, cv);
			if (!is_func) {
				macsurf_debug_log_writef(
					"LIFE CTOR MISS %s", *cn);
				all_ok = 0;
			}
			JS_FreeValue(ctx, cv);
		}
		/* XF compatibility: HTML_Element alias */
		{
			JSValue html_el = JS_GetPropertyStr(ctx, global,
				"HTMLElement");
			if (JS_IsFunction(ctx, html_el)) {
				JS_SetPropertyStr(ctx, global,
					"HTML_Element", JS_DupValue(ctx, html_el));
			}
			JS_FreeValue(ctx, html_el);
		}
		if (all_ok) {
			macsurf_debug_log_writef(
				"LIFE CTOR census: all present");
		}
	}

	/* fixes1247 (#167) - Facebook's own module loader (the Comet/"cr:"
	 * bundler runtime __d/__r bootstrap, confirmed present in the real
	 * bundles this engine executes) exposes two OFFICIAL, INTENTIONAL
	 * instrumentation hooks: window.__onBeforeModuleFactory /
	 * __onAfterModuleFactory, called with the module record (.id = the
	 * module name string) immediately before/after that module's factory
	 * function actually RUNS. Currently null (unused) -- this is Facebook's
	 * OWN extension point, not an undocumented internal being poked at.
	 *
	 * Why this exists: hours of static bundle analysis (grep across every
	 * script this exact page executes) traced the SSR-splash-reveal chain
	 * as far as ServerJSPayloadListener_NEW.process() and could not find a
	 * single literal require()/call site for it anywhere in the visible
	 * source -- strong evidence the real entry point is DATA-DRIVEN (a
	 * route manifest of module names, iterated generically) rather than a
	 * grep-able string. This hook answers the question directly, from real
	 * execution, instead of more static guessing: does the page's own
	 * module system ever actually RUN the factory for any of a small,
	 * targeted watchlist of modules identified during that investigation.
	 *
	 * SAFETY: the page's own require() dispatch (`z(r)` in the real
	 * bundle) calls `t.__onBeforeModuleFactory==null||
	 * t.__onBeforeModuleFactory(l)` with NO try/catch around it -- if our
	 * hook function throws, it aborts THAT MODULE'S OWN REQUIRE CALL,
	 * which could break the page in a new and worse way than simply not
	 * having this diagnostic. The hook body is therefore wrapped in its
	 * own try/catch that can NEVER propagate outward, no matter what `l`
	 * looks like.
	 *
	 * The page's own bootstrap does `t.__onBeforeModuleFactory=null;`
	 * unconditionally as part of ITS OWN init (confirmed in the real
	 * bundle) -- a plain data-property assignment would just overwrite
	 * whatever we install beforehand. Object.defineProperty with a
	 * get/set pair survives that: the setter silently discards whatever
	 * the page assigns, the getter always hands back our tracer. Scoped
	 * to a SMALL watchlist and deduped per-module (not every module --
	 * a 13MB+ bundle set defines/requires thousands, which would flood
	 * the log for no benefit over the specific question being asked) so
	 * this reuses the existing __msLife budget safely rather than needing
	 * a dedicated one. */
	macsurf_qjs__safe_eval(ctx,
		"(function(g){"
		"\"use strict\";"
		"try{"
		"var WATCH={"
			"ServerJSPayloadListener:1,ServerJSPayloadListener_NEW:1,"
			"ServerJS:1,GHLServerJSParse:1,CometPrelude:1,"
			"CometPreludeCritical:1,CometPreludeRunWhenReady:1,"
			"CometSSRContentRevealer:1,CometSSRClientInjector:1,"
			"CometClientRootRendererSSRUtils:1,"
			"CometClientRootRendererUtils:1"
		"};"
		"var seen={};"
		"var total=0;"
		"var tracer=function(l){"
			"try{"
				"total++;"
				"var id=(l&&l.id!=null)?String(l.id):null;"
				"if(id&&WATCH[id]&&!seen[id]){"
					"seen[id]=1;"
					"if(typeof __msLife==='function')"
						"__msLife('require: '+id);"
				"}"
			"}catch(e){}"
			"return undefined;"
		"};"
		"Object.defineProperty(g,'__onBeforeModuleFactory',{"
			"configurable:true,"
			"get:function(){return tracer;},"
			"set:function(v){}"
		"});"
		"g.__msRequireTraceTotal=function(){return total;};"
		"}catch(e){"
			/* fixes1248 (#167) - was a bare catch(e){}: if
			 * Object.defineProperty (or anything else in this block)
			 * threw, that failure was completely invisible -- no log
			 * line anywhere, not even via macsurf_qjs__safe_eval's own
			 * exception reporting, because THIS try/catch already
			 * absorbed it before that outer layer ever saw anything go
			 * wrong. Hardware evidence (fixes1247's first real
			 * multi-navigation session) showed __msRafOrig -- a
			 * COMPLETELY UNRELATED marker set earlier in this same
			 * register_browser_globals pass -- also reading as missing
			 * (rafOwn=-1) starting on the exact navigation where this
			 * block would have first run a second/third time, which is
			 * suspicious enough to need real visibility rather than a
			 * guess either way. */
			"try{if(typeof __msLife==='function')"
				"__msLife('require-trace install FAILED: '+"
					"((e&&e.message)||e));"
			"}catch(e2){}"
		"}"
		"})(this);");

	/* fixes1259 (#167) - Facebook loader observability. fixes1258 fixed
	 * the data: URI base64/MIME bug that was silently failing ~168 of
	 * Facebook's script tags (window.Env setup,
	 * requireLazy(["ServerJSPayloadListener"], m=>m.process())) with
	 * UnacceptableType. Hardware confirmed the fix: SPIPE now shows
	 * q=d, x=0 (essentially 100% script throughput, up from ~10%). But
	 * JSREQUIRE total=0 PERSISTS even so - the fetch/exec layer is now
	 * healthy, so whatever is stopping Facebook's app from booting is
	 * entirely inside its own module loader from this point on.
	 *
	 * requireLazy(deps, callback) does not call require() directly - it
	 * registers callback to fire once every module in deps has been
	 * __d()-defined, then fires it (possibly much later, possibly via a
	 * promise/microtask this engine doesn't drive the way a real browser
	 * does). fixes1247's require-trace is blind to this: it only sees
	 * the ACTUAL require() dispatch, not this earlier
	 * register-and-wait step, so it cannot tell "ServerJSPayloadListener
	 * never got defined" apart from "it got defined but the lazy waiter
	 * never released" apart from "the waiter fired but its own callback
	 * threw." This wraps BOTH __d and requireLazy (not requireLazy
	 * alone) specifically to remove that ambiguity, and wraps the
	 * CALLBACK passed to requireLazy (not just the requireLazy call
	 * itself) so FIRE/RETURN/THROW reflect whether the dependency
	 * actually got released, not just whether it was asked for.
	 *
	 * ACTIVE delegate-through, not fixes1247's discard-and-ignore
	 * pattern: __d/requireLazy must keep WORKING (real module
	 * registration, real lazy resolution), only OBSERVED. The setter
	 * stores whatever Facebook's own bootstrap assigns (its stub
	 * queueing implementation first, confirmed present in the real
	 * bundles: __d=function(e,t,n,r){__d_stub.push([e,t,n,r])} - then
	 * whatever real implementation replaces it later) into a closure
	 * variable; the getter always hands back OUR wrapper, which calls
	 * through to whatever was most recently stored. This survives
	 * Facebook reassigning window.__d/requireLazy any number of times,
	 * the same property-trap technique fixes1247 already proved
	 * effective for __onBeforeModuleFactory - installed at the same
	 * early point in this same function, before any page script runs. */
	macsurf_qjs__safe_eval(ctx,
		"(function(g){"
		"\"use strict\";"
		"try{"
		"var DWATCH={ServerJSPayloadListener:1};"
		"var dTotal=0,dTarget=0;"
		"var rlCalls=0,rlTargetCalls=0,rlFires=0,rlTargetFires=0;"
		"var rlSeq=0;"
		/* fixes1270 (#167) - independent module-graph reconstruction.
		 * fixes1260's __debug probe asks FACEBOOK'S OWN loader whether
		 * ServerJSPayloadListener's dependencies are ready - but that
		 * loader's readiness bookkeeping is exactly what's suspected of
		 * being broken, so a self-report from it is circular evidence.
		 * This instead builds an INDEPENDENT view from data __d() already
		 * hands us on every call, with zero reliance on any Facebook
		 * internal being correct: GDEFINED records every module name
		 * that WAS __d()-called (proof of registration, not of the
		 * loader's own resolution state), GDEPS records the literal
		 * deps array each one was defined with, VERBATIM - a dependency
		 * token like "cr:1234567" is stored and reported as-is, never
		 * guessed at or normalized into an ordinary name, since walking
		 * it as if it were one would silently misclassify a form this
		 * code cannot prove anything about. GMAX bounds total entries
		 * defensively (128-384MB hardware) - if a page ever defines
		 * more than this, the graph walk below still runs correctly
		 * against however much it collected, just incompletely. */
		"var GMAX=20000;"
		"var GDEFINED=Object.create(null);"
		"var GDEPS=Object.create(null);"
		"var GSEQ=0;"
		"var GTARGET='ServerJSPayloadListener';"
		"var gTargetDefineSeq=-1;"
		"var gTargetLazyFirstSeq=-1;"
		"var gTargetLazyLastSeq=-1;"
		/* fixes1271 (#167) - stop assuming the exact literal name.
		 *
		 * fixes1270's hardware round returned FBGRAPH defined:false --
		 * ServerJSPayloadListener was never __d()-called under that
		 * EXACT name -- while FBLOADER on the same navigation still
		 * showed rl_target_calls=166. Those two are not contradictory:
		 * rlTargetCalls tests the joined deps string with indexOf(),
		 * a SUBSTRING match, whereas DWATCH/GTARGET both test for
		 * string equality. So the lazy waiters are attached to a name
		 * that CONTAINS "ServerJSPayloadListener" without being equal
		 * to it. fixes1247's own watchlist already names one such
		 * variant, ServerJSPayloadListener_NEW, so a variant is the
		 * expected case, not a surprise.
		 *
		 * These two tables record, per DISTINCT NAME, every variant
		 * actually observed on each side, so the mismatch can never
		 * again hide inside a single boolean bucket:
		 *   DLIKE[name]  - times __d(name) was called, name containing
		 *                  the substring
		 *   RLLIKE[name] - times a requireLazy dependency equal to
		 *                  that name was requested
		 * plus first-sighting sequence numbers for each, so define-vs-
		 * wait ordering is answerable per variant rather than only for
		 * one hardcoded string.
		 *
		 * tlBudget is DELIBERATELY SEPARATE from `budget` above: the
		 * shared one is consumed by FBRL CALL/FIRE lines (id<=20) and
		 * could be exhausted before a late-registering variant is ever
		 * seen, which is exactly the discovery this round exists to
		 * make. Variants are few (one or two expected), so a small
		 * dedicated allowance costs nothing and cannot be starved. */
		"var TSUB='ServerJSPayloadListener';"
		"var DLIKE=Object.create(null);"
		"var RLLIKE=Object.create(null);"
		"var DLIKESEQ=Object.create(null);"
		"var RLLIKESEQ=Object.create(null);"
		"var tlBudget=40;"
		"var realD=(typeof g.__d==='function')?g.__d:null;"
		"var realRL=(typeof g.requireLazy==='function')?g.requireLazy:null;"
		"var budget=300;"
		"function wrappedD(){"
			"dTotal++;"
			"GSEQ++;"
			"try{"
				"var id=(arguments[0]!=null)?String(arguments[0]):null;"
				/* fixes1270 - unconditional, every __d() call, not
				 * gated by DWATCH: the graph walk needs the FULL
				 * dependency map to reconstruct the target's real
				 * closure, not just the one watched name. */
				"if(id&&!GDEFINED[id]&&GSEQ<=GMAX){"
					"GDEFINED[id]=true;"
					"var _rawDeps=arguments[1];"
					"var _cp=[];"
					"if(_rawDeps&&typeof _rawDeps.length==='number'){"
						"var _di;"
						"for(_di=0;_di<_rawDeps.length;_di++)"
							"_cp.push(_rawDeps[_di]);"
					"}"
					"GDEPS[id]=_cp;"
					"if(id===GTARGET&&gTargetDefineSeq===-1)"
						"gTargetDefineSeq=GSEQ;"
				"}else if(id){"
					"GDEFINED[id]=true;"
				"}"
				/* fixes1271 - SUBSTRING match, per distinct name.
				 * Logged once per variant on first sighting; the
				 * count keeps accruing silently after that. */
				"if(id&&id.indexOf(TSUB)>=0){"
					"if(!DLIKE[id]){"
						"DLIKE[id]=0;"
						"DLIKESEQ[id]=GSEQ;"
						"if(tlBudget>0&&"
								"typeof __msLife==='function'){"
							"tlBudget--;"
							"__msLife('FBMOD TARGETLIKE name='+id+"
								"' seq='+GSEQ);"
						"}"
					"}"
					"DLIKE[id]++;"
				"}"
				"if(id&&DWATCH[id]){"
					"dTarget++;"
					"if(budget>0&&typeof __msLife==='function'){"
						"budget--;"
						"__msLife('FBMOD D name='+id);"
					"}"
				"}"
			"}catch(e){}"
			"if(realD)return realD.apply(this,arguments);"
			"return undefined;"
		"}"
		"function wrappedRL(){"
			"var args=[];"
			"var k;"
			"for(k=0;k<arguments.length;k++)args.push(arguments[k]);"
			"rlCalls++;"
			"var id=++rlSeq;"
			"var isTarget=false;"
			"try{"
				"var deps=args[0];"
				"var dj=(deps&&deps.join)?deps.join(','):String(deps);"
				/* fixes1271 - walk the deps ARRAY and record each
				 * individual dependency string containing the
				 * substring, rather than only testing the joined
				 * blob. The joined test cannot tell WHICH literal
				 * name the waiter is actually attached to, which is
				 * the whole question this round answers. */
				"if(deps&&typeof deps.length==='number'){"
					"var _ri;"
					"for(_ri=0;_ri<deps.length;_ri++){"
						"var _dn=deps[_ri];"
						"if(typeof _dn!=='string')continue;"
						"if(_dn.indexOf(TSUB)<0)continue;"
						"if(!RLLIKE[_dn]){"
							"RLLIKE[_dn]=0;"
							"RLLIKESEQ[_dn]=GSEQ;"
							"if(tlBudget>0&&"
									"typeof __msLife==='function'){"
								"tlBudget--;"
								"__msLife('FBRL TARGETLIKE id='+id+"
									"' dep='+_dn+' seq='+GSEQ);"
							"}"
						"}"
						"RLLIKE[_dn]++;"
					"}"
				"}"
				"if(dj.indexOf('ServerJSPayloadListener')>=0){"
					"isTarget=true;rlTargetCalls++;"
					/* fixes1270 - cheap chronology: was the
					 * target __d()-defined before or after
					 * its lazy waiters registered? Reuses
					 * GSEQ, the same counter __d() already
					 * advances, rather than a second budget. */
					"if(gTargetLazyFirstSeq===-1)"
						"gTargetLazyFirstSeq=GSEQ;"
					"gTargetLazyLastSeq=GSEQ;"
				"}"
				"if((isTarget||id<=20)&&budget>0&&"
						"typeof __msLife==='function'){"
					"budget--;"
					"__msLife('FBRL CALL id='+id+' deps='+dj);"
				"}"
			"}catch(e){}"
			"var origCb=args[1];"
			"if(typeof origCb==='function'){"
				"args[1]=function(){"
					"rlFires++;"
					"if(isTarget)rlTargetFires++;"
					"try{"
						"if((isTarget||id<=20)&&budget>0&&"
								"typeof __msLife==='function'){"
							"budget--;"
							"__msLife('FBRL FIRE id='+id);"
						"}"
					"}catch(e){}"
					"try{"
						"var ret=origCb.apply(this,arguments);"
						"try{"
							"if((isTarget||id<=20)&&budget>0&&"
									"typeof __msLife==='function'){"
								"budget--;"
								"__msLife('FBRL RETURN id='+id);"
							"}"
						"}catch(e2){}"
						"return ret;"
					"}catch(err){"
						"try{if(typeof __msLife==='function')"
							"__msLife('FBRL THROW id='+id+' err='+"
								"((err&&err.message)||err));"
						"}catch(e3){}"
						"throw err;"
					"}"
				"};"
			"}"
			"if(realRL)return realRL.apply(this,args);"
			"return undefined;"
		"}"
		"Object.defineProperty(g,'__d',{"
			"configurable:true,"
			"get:function(){return wrappedD;},"
			"set:function(v){realD=v;}"
		"});"
		"Object.defineProperty(g,'requireLazy',{"
			"configurable:true,"
			"get:function(){return wrappedRL;},"
			"set:function(v){realRL=v;}"
		"});"
		"g.__msFBLoader_dTotal=function(){return dTotal;};"
		"g.__msFBLoader_dTarget=function(){return dTarget;};"
		"g.__msFBLoader_rlCalls=function(){return rlCalls;};"
		"g.__msFBLoader_rlTargetCalls=function(){return rlTargetCalls;};"
		"g.__msFBLoader_rlFires=function(){return rlFires;};"
		"g.__msFBLoader_rlTargetFires=function(){return rlTargetFires;};"
		/* fixes1270 (#167) - walk GDEPS starting at `target`, breadth-
		 * first, WITHOUT executing a single module factory. Classifies
		 * strictly by what was independently observed:
		 *   - target itself never __d()-called -> defined:false, stop.
		 *   - a direct dep of target never __d()-called -> direct_missing.
		 *   - a deeper dep never __d()-called -> transitive_missing.
		 *   - a dep token containing ':' (Facebook's cr:NNNN forms and
		 *     similar) is recorded verbatim in `special` and NEITHER
		 *     walked further NOR counted as missing - this code cannot
		 *     prove anything about what such a token resolves to, so it
		 *     makes no claim rather than guessing.
		 * Returns one JSON string so the C side needs one JS_Eval / one
		 * JS_ToCString, matching the FBSTATE pattern already used for
		 * fixes1260's __debug probe below. */
		/* fixes1271 - report every observed variant on both sides,
		 * with counts and first-sighting sequence, as one JSON blob.
		 * The C side logs this verbatim (LIFE FBTARGETS) so the four
		 * outcomes in the fixes1271 decision table can be read
		 * directly off one line rather than inferred across several. */
		"g.__msFBLoader_targetLike=function(){"
			"try{"
				"var out={sub:TSUB,defined:[],lazy:[]};"
				"var k;"
				"for(k in DLIKE)out.defined.push({name:k,n:DLIKE[k],"
					"seq:DLIKESEQ[k]});"
				"for(k in RLLIKE)out.lazy.push({name:k,n:RLLIKE[k],"
					"seq:RLLIKESEQ[k]});"
				"return JSON.stringify(out);"
			"}catch(e){"
				"return JSON.stringify({error:((e&&e.message)||"
					"String(e))});"
			"}"
		"};"
		/* fixes1271 - pick the name to graph from what was actually
		 * OBSERVED, instead of the hardcoded literal that returned
		 * defined:false last round. Preference order, most to least
		 * provable:
		 *   1. a variant seen on BOTH sides (defined AND waited on) -
		 *      unambiguous, graph it;
		 *   2. otherwise the most-requested lazy variant - this is the
		 *      real missing-module case, and graphing it makes
		 *      defined:false a MEANINGFUL result about the name the
		 *      waiters actually use;
		 *   3. otherwise the most-defined variant;
		 *   4. otherwise the original literal, so behaviour never
		 *      silently degrades to "no target at all".
		 * Returns the chosen name so the walk can report it. */
		"g.__msFBGraph_pick=function(){"
			"try{"
				"var k,best=null,bestN=-1;"
				"for(k in RLLIKE){"
					"if(DLIKE[k]){"
						"if(RLLIKE[k]>bestN){best=k;bestN=RLLIKE[k];}"
					"}"
				"}"
				"if(best)return best;"
				"bestN=-1;"
				"for(k in RLLIKE){"
					"if(RLLIKE[k]>bestN){best=k;bestN=RLLIKE[k];}"
				"}"
				"if(best)return best;"
				"bestN=-1;"
				"for(k in DLIKE){"
					"if(DLIKE[k]>bestN){best=k;bestN=DLIKE[k];}"
				"}"
				"if(best)return best;"
				"return GTARGET;"
			"}catch(e){return GTARGET;}"
		"};"
		"g.__msFBGraph_walk=function(target){"
			"try{"
				/* fixes1271 - no explicit target means "use whatever
				 * the page actually showed us"; see __msFBGraph_pick.
				 * picked=1 records that the name was discovered rather
				 * than supplied, so a reader never has to guess which
				 * happened. */
				"var picked=0;"
				"if(!target){"
					"target=g.__msFBGraph_pick();"
					"picked=1;"
				"}"
				"var out={target:target,picked:picked,defined:false,"
					"direct_missing:0,transitive_missing:0,"
					"closure:0,leaf:null,special:[],"
					"n_defined_variants:0,n_lazy_variants:0,"
					/* fixes1271 - the per-variant sequence if we
					 * have one, else the legacy counter ONLY when
					 * the target really is the legacy literal.
					 * Falling back unconditionally reported the
					 * define seq of a DIFFERENT name for a target
					 * that was never defined - exactly the kind of
					 * cross-name summary this round exists to stop
					 * producing. -1 means "not observed". */
					"target_define_seq:(DLIKESEQ[target]!==undefined)?"
						"DLIKESEQ[target]:"
						"((target===GTARGET)?gTargetDefineSeq:-1),"
					"target_lazy_first_seq:(RLLIKESEQ[target]!==undefined)?"
						"RLLIKESEQ[target]:"
						"((target===GTARGET)?gTargetLazyFirstSeq:-1),"
					"target_lazy_last_seq:gTargetLazyLastSeq};"
				"var _vk;"
				"for(_vk in DLIKE)out.n_defined_variants++;"
				"for(_vk in RLLIKE)out.n_lazy_variants++;"
				"if(!GDEFINED[target])return JSON.stringify(out);"
				"out.defined=true;"
				"var visited=Object.create(null);"
				"var depthOf=Object.create(null);"
				"var queue=[target];"
				"depthOf[target]=0;"
				"var closureCount=0,directMiss=0,transMiss=0;"
				"var leaf=null;"
				"var special=[];"
				"var qi=0;"
				"while(qi<queue.length&&qi<GMAX){"
					"var name=queue[qi++];"
					"if(visited[name])continue;"
					"visited[name]=true;"
					"closureCount++;"
					"var deps=GDEPS[name]||[];"
					"var depth=depthOf[name];"
					"var i;"
					"for(i=0;i<deps.length;i++){"
						"var d=deps[i];"
						"if(typeof d!=='string'){"
							"if(special.length<10)"
								"special.push(JSON.stringify(d));"
							"continue;"
						"}"
						"if(d.indexOf(':')>=0){"
							"if(special.length<10)special.push(d);"
							"continue;"
						"}"
						"if(!GDEFINED[d]){"
							"if(!leaf)leaf=d;"
							"if(depth===0)directMiss++;"
							"else transMiss++;"
							"continue;"
						"}"
						"if(depthOf[d]===undefined){"
							"depthOf[d]=depth+1;"
							"queue.push(d);"
						"}"
					"}"
				"}"
				"out.direct_missing=directMiss;"
				"out.transitive_missing=transMiss;"
				"out.closure=closureCount;"
				"out.leaf=leaf;"
				"out.special=special;"
				"return JSON.stringify(out);"
			"}catch(e){"
				"return JSON.stringify({error:((e&&e.message)||"
					"String(e))});"
			"}"
		"};"
		"}catch(e){"
			"try{if(typeof __msLife==='function')"
				"__msLife('FB loader trace install FAILED: '+"
					"((e&&e.message)||e));"
			"}catch(e2){}"
		"}"
		"})(this);");

	/* R1.2 - the WANT probe goes in LAST: every shim block above runs its
	 * own `typeof g.X` feature checks, and those would log their own
	 * stubbed names into the census if the probe were live yet.  Page
	 * scripts run after this point, so everything the probe sees from
	 * here on is a real "the page asked for X" signal. */
	qjs_install_want_probe(ctx);

	JS_FreeValue(ctx, global);
}

/* ------------------------------------------------------------------ */
/* macsurf_qjs_setup_globals - called before register_browser_globals  */
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
/* fixes1008 (1g) - see the install site in qjs_build_context.
 *
 * is_handled means a rejection that was already caught (or caught later);
 * those are normal control flow and must NOT be logged, or a page using
 * try/catch around a fetch fills the log with non-problems. Only genuinely
 * unhandled ones are reported. */
static void qjs_promise_rejection_tracker(JSContext *ctx, JSValueConst promise,
		JSValueConst reason, bool is_handled, void *opaque)
{
	const char *msg;
	(void)promise; (void)opaque;
	if (is_handled) return;
	msg = JS_ToCString(ctx, reason);
	macsurf_debug_log_writef("LIFE js unhandled rejection: %s",
			msg ? msg : "(no reason)");
	if (msg) JS_FreeCString(ctx, msg);
}

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

	/* fixes1008 (1g) - UNHANDLED PROMISE REJECTIONS, made visible.
	 *
	 * "Site loads, does nothing, no log" is the failure mode this whole batch
	 * exists to eliminate, and an unhandled rejection is its purest form: a
	 * loader chain that rejects three .then()s deep simply stops, leaving no
	 * exception, no error, and nothing on disk to explain it. QuickJS will
	 * tell us -- it just needs a tracker installed, and nothing ever
	 * installed one.
	 *
	 * LIFE-prefixed because the WORK channel is compiled out of shipping
	 * builds; a diagnostic nobody can read is the trap that has already cost
	 * this project four rounds. */
	JS_SetHostPromiseRejectionTracker(heap->rt,
			qjs_promise_rejection_tracker, NULL);

	MS_LOG("qjs: setup_globals");   macsurf_qjs_setup_globals(ctx);
	MS_LOG("qjs: browser_globals"); register_browser_globals(ctx);
	MS_LOG("qjs: dom_install");     qjs_dom_install(ctx);
	return ctx;
}

/* fixes593 - QuickJS capability self-test. Runs a battery of JS through the
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

	/* fixes597 - engine core is proven sound; now hammer the DOM BRIDGE (the
	 * layer the real scripts hit that the pure-JS tests above don't): wrapper
	 * creation, dom_string round-trips, node refcounts. If one of these FREEZES
	 * with no page loaded, the corruptor is reproduced in the bridge, minimal.
	 * These may THROW at startup if document isn't wired yet - a logged
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

	/* fixes598 - the ONE thing every real script does that the tests above
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

	/* fixes599 - the last untested bridge path: LIVE nodes attached to the
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

/* ================================================================
 * fixes1117b (#265) - MODULE SEMANTICS (JS_SetModuleLoaderFunc)
 *
 * QuickJS supports real ES modules via JS_EVAL_TYPE_MODULE +
 * JS_SetModuleLoaderFunc.  The normalize callback resolves relative
 * import specifiers against the importing module's URL; the loader
 * callback fetches and compiles the module source.  Both are registered
 * on the JSRuntime in js_newheap.
 * ================================================================ */

/* Simple linked list of pre-registered module sources (inline scripts
 * whose text is already available before JS_Eval is called). */
struct module_entry {
	struct module_entry *next;
	char   *name;    /* module URL (malloc'd) */
	char   *source;  /* source text (malloc'd, NUL-terminated) */
	size_t  srclen;
};

/* Module registry lives per heap; opaque pointer in the loader. */
struct module_registry {
	struct module_entry *head;
};

/* Normalize: resolve import specifier against base module name.
 * Returns a malloc'd string that QuickJS will free via JS_FreeCString. */
static char *qjs_module_normalize(JSContext *ctx,
	const char *base_name, const char *name, void *opaque)
{
	const char *last_slash;
	size_t base_len, name_len, dot;
	char *result;
	(void)opaque; (void)ctx;

	if (name == NULL) return NULL;

	/* Absolute URL or already fully qualified */
	if (name[0] == '/' || strncmp(name, "http://", 7) == 0 ||
			strncmp(name, "https://", 8) == 0) {
		return strdup(name);
	}

	/* Relative: resolve against base_name's directory */
	last_slash = base_name ? strrchr(base_name, '/') : NULL;
	if (last_slash == NULL) return strdup(name);

	base_len = (size_t)(last_slash - base_name) + 1; /* include the / */
	name_len = strlen(name);

	result = (char *)malloc(base_len + name_len + 1);
	if (result == NULL) return NULL;
	memcpy(result, base_name, base_len);
	memcpy(result + base_len, name, name_len + 1);

	/* Append .js if no extension (Node/bundler compat) */
	dot = name_len;
	while (dot > 0 && name[dot - 1] != '.') dot--;
	if (dot == 0 && name_len > 0) {
		char *ext = (char *)malloc(base_len + name_len + 4);
		if (ext != NULL) {
			memcpy(ext, result, base_len + name_len);
			memcpy(ext + base_len + name_len, ".js\0", 4);
			free(result);
			result = ext;
		}
	}

	return result;
}

/* Loader: fetch and compile a module given its normalized name.
 * Checks the pre-registered source list first (inline scripts),
 * then tries disk cache, then fails. Called synchronously by
 * QuickJS during JS_ResolveModule / import resolution. */
static JSModuleDef *qjs_module_loader(JSContext *ctx,
	const char *module_name, void *opaque)
{
	struct module_registry *reg;
	struct module_entry *e;
	JSValue val;

	reg = (struct module_registry *)opaque;

	/* 1. Check pre-registered sources (inline scripts) */
	if (reg != NULL && module_name != NULL) {
		for (e = reg->head; e != NULL; e = e->next) {
			if (e->name != NULL &&
					strcmp(e->name, module_name) == 0) {
				val = JS_Eval(ctx, e->source, e->srclen,
					module_name,
					JS_EVAL_TYPE_MODULE |
					JS_EVAL_FLAG_COMPILE_ONLY);
				if (!JS_IsException(val))
					return (JSModuleDef *)
						JS_VALUE_GET_PTR(val);
				JS_FreeValue(ctx, JS_GetException(ctx));
				return NULL;
			}
		}
	}

#ifdef __MACOS9__
	/* 2. Try disk cache */
	{
		extern int macos9_cache_lookup(const char *url,
			char **body, long *body_len);
		char *body = NULL;
		long blen = 0;

		if (macos9_cache_lookup(module_name, &body, &blen) == 1
				&& body != NULL && blen > 0) {
			val = JS_Eval(ctx, body, (size_t)blen,
				module_name,
				JS_EVAL_TYPE_MODULE |
				JS_EVAL_FLAG_COMPILE_ONLY);
			free(body);
			if (!JS_IsException(val))
				return (JSModuleDef *)
					JS_VALUE_GET_PTR(val);
			JS_FreeValue(ctx, JS_GetException(ctx));
		}
	}
#else
	(void)ctx;
#endif

	return NULL; /* module not found */
}

/* Pre-register a module source so the loader can find it during
 * import resolution. Called before JS_Eval for inline module scripts. */
static void qjs_module_register(struct module_registry *reg,
	const char *name, const char *source, size_t srclen)
{
	struct module_entry *e;
	if (reg == NULL || name == NULL || source == NULL) return;
	e = (struct module_entry *)calloc(1, sizeof(*e));
	if (e == NULL) return;
	e->name = strdup(name);
	e->source = (char *)malloc(srclen + 1);
	if (e->source != NULL) {
		memcpy(e->source, source, srclen);
		e->source[srclen] = '\0';
		e->srclen = srclen;
	}
	e->next = reg->head;
	reg->head = e;
}

static void qjs_module_registry_free(struct module_registry *reg)
{
	struct module_entry *e, *next;
	if (reg == NULL) return;
	for (e = reg->head; e != NULL; e = next) {
		next = e->next;
		free(e->name);
		free(e->source);
		free(e);
	}
	reg->head = NULL;
}

nserror js_newheap(int timeout, struct jsheap **out_heap)
{
	struct jsheap *heap;
	if (out_heap == NULL) return NSERROR_BAD_PARAMETER;
	*out_heap = NULL;
	macsurf_qjs_audit_reset(); /* fixes1016 - audit each page fully */
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
		 * the native stack was never the corruptor - set the guard to the real
		 * headroom (no small cap, which would wrongly RangeError legit deep JS)
		 * with a floor so basic JS still runs if the read is tiny. */
		if (qmax < 24576L) qmax = 24576L;  /* floor so basic JS still runs */
		JS_SetMaxStackSize(heap->rt, (size_t)qmax);
		macsurf_debug_log_writef(
			"qjs: stack guard=%ld (StackSpace=%ld)", qmax, sp_room);
	}
#else
	/* fixes873 - NON-MAC (the Linux ASan harness) - 4 MB, not the 256 KB that
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

	/* fixes593 - the heap-corruption freeze on heavy JS pages (tinkerdifferent:
	 * all scripts still RUN through QuickJS, but a later malloc/free spins on a
	 * smashed free-list). Prime suspect is QuickJS's automatic cycle-GC: it only
	 * fires once malloc_size crosses this threshold (default 256KB), i.e. ONLY
	 * on heavy pages - exactly the tinkerdifferent-vs-68kmla split - and if any
	 * ref is over-released, the cycle collector double-frees when it walks the
	 * graph. Push the threshold past the 128MB memory cap so auto-GC never runs
	 * mid-load. Nothing about which JS runs changes; the per-navigation runtime
	 * is torn down wholesale on nav, so uncollected cycles never accumulate.
	 * (If this proves it, the real refcount bug gets fixed and GC re-armed.) */
	JS_SetGCThreshold(heap->rt, (size_t)0x40000000UL);  /* 1GB > 128MB cap */
	/* fixes1070 - record that the collector is OFF so the perf log says so.
	 * Set beside the call it describes: a flag that can drift from the
	 * threshold it reports is worse than no flag. See g_perf_gc_armed. */
	g_perf_gc_armed = 0;

	JS_SetInterruptHandler(heap->rt, qjs_interrupt_handler, NULL);

	/* fixes1117b (#265) -- ES module loader. Registered on the
	 * JSRuntime (not per-context) so it is available for every
	 * context created from this runtime. */
	heap->module_reg = (struct module_registry *)
		calloc(1, sizeof(struct module_registry));
	if (heap->module_reg != NULL) {
		JS_SetModuleLoaderFunc(heap->rt,
			qjs_module_normalize,
			qjs_module_loader,
			(void *)heap->module_reg);
	}

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
	/* fixes875 (#304) - stamp the realm, then link. qjs_ctx_gen() answers by
	 * walking g_heap_list, so a timer registered before the link would be
	 * stamped generation 0 and abandoned forever (never fires, silently).
	 * Nothing between here and the link runs page JS, and JS cannot run any
	 * earlier either: heap->ctx does not exist until qjs_build_context returns,
	 * so linking sooner would not help -- the list keys on heap->ctx. */
	heap->ctx_gen = g_ctx_gen_next++;

	heap->timeout = timeout;

	g_heap = heap;
	/* fixes861 (#289) - link into the all-heaps list so this heap's timers
	 * actually get pumped.  Newest-first; order does not matter.
	 * fixes875 (#304) - this list is now also the authority qjs_ctx_gen()
	 * consults to decide which realm owns a ctx, so an unlinked heap means
	 * "generation 0" = every timer it registers is abandoned and never fires. */
	heap->next = g_heap_list;
	g_heap_list = heap;
	*out_heap = heap;
	MS_LOG("qjs: heap created");

	/* fixes671 (perf): the fixes593-598 capability self-test runs a heavy JS
	 * battery (100k object allocs, fib25, 20k string/array ops, 5k DOM ops,
	 * throw/backtrace stress) SYNCHRONOUSLY at first heap creation - ~17s on a
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
	/* fixes854 (#283) - drop this heap's timer + XHR slots BEFORE the context
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
	if (heap->ctx != NULL) {
		JS_FreeContext(heap->ctx);
		/* fixes888 (#304) - same rule as js_newthread: a freed context
		 * pointer must never stay visible on g_heap_list. This heap is not
		 * unlinked until the bottom of this function, and JS_FreeRuntime
		 * below runs finalizers -- so without this, every qjs_ctx_gen /
		 * qjs_ctx_live_rt scan during that window can match this dead
		 * heap's dangling ctx and report its generation as live. */
		heap->ctx = NULL;
		heap->ctx_gen = 0;
	}
	/* fixes541: release any wrappers whose finalizer JS_FreeContext did not
	 * run (obj->method reference cycles); both halves, then clear. */
	qjs_wrap_drain(heap->rt);
	/* fixes888 (#304) - unlink BEFORE JS_FreeRuntime, not after. The runtime
	 * teardown runs finalizers, and anything they touch that scans g_heap_list
	 * must not find this heap: its ctx is already gone and its rt is in the
	 * middle of being destroyed. fixes861 moved the unlink ahead of free(heap)
	 * for the same class of reason (pump_all walking freed memory); this moves
	 * it ahead of the teardown that can re-enter. */
	{
		struct jsheap **pp = &g_heap_list;
		while (*pp != NULL) {
			if (*pp == heap) { *pp = heap->next; break; }
			pp = &(*pp)->next;
		}
	}
	if (g_heap == heap) g_heap = NULL;
	qjs_module_registry_free(heap->module_reg);
	free(heap->module_reg);
	if (heap->rt  != NULL) JS_FreeRuntime(heap->rt);
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
		/* fixes846 (#167 S3) - same load-bearing ordering as the timer
		 * flush above: abort every in-flight XHR and free its dup'd
		 * JSValue against the OLD context before it's freed, or a
		 * response that arrives after navigation would JS_Call into
		 * freed heap from xhr_deliver(). */
		macos9_js_fetch_flush(heap->ctx);
		fresh = qjs_build_context(heap);
		if (fresh != NULL) {
			JS_FreeContext(heap->ctx);
			/* fixes888 (#304) - do NOT leave a freed pointer visible.
			 *
			 * heap is on g_heap_list the whole time, and qjs_ctx_gen /
			 * qjs_ctx_live_rt answer by scanning that list for h->ctx == ctx.
			 * Between JS_FreeContext above and `heap->ctx = fresh` below,
			 * heap->ctx is a DANGLING pointer still advertising this heap's
			 * old generation -- so any lookup landing on that address during
			 * the window (qjs_wrap_drain runs finalizers here) gets a
			 * confident, wrong answer about a realm that no longer exists.
			 * Worse, the allocator can hand that exact address to the next
			 * context, and then TWO heaps answer for one pointer and the scan
			 * returns whichever is first in the list.
			 * NULL it immediately: qjs_ctx_gen/qjs_ctx_live_rt both return
			 * 0/NULL for NULL, which is the correct "no live realm" answer. */
			heap->ctx = NULL;
			heap->ctx_gen = 0;
			/* fixes541: release every old-page wrapper's node ref AND
			 * owner-document keepalive, then clear the map, before the
			 * fresh context wraps anything.  Both halves, then clear. */
			qjs_wrap_drain(heap->rt);
			heap->ctx = fresh;
			/* fixes875 (#304) - a NEW realm, so a NEW generation. This is
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
		/* doc_priv is html_content* - extract dom_document*.
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
 * XenForo stub scripts - ported from macsurf_js.c (Duktape version).
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

/* fixes476/478/479: editor stub - FroalaEditor shim + XF.Editor registration */
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
 * Transpile cache - avoids re-transpiling the same versioned bundle
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

	/* fixes847 (#167 S1 census gap) - the other half of the js_exec
	 * visibility fix below: if thread/ctx is NULL, js_exec bails before
	 * even reaching the entry breadcrumb, and NOTHING about this script
	 * is ever logged. Make that visible too, so "zero js activity" in a
	 * hardware log can be told apart from "no thread was ever wired". */
	if (thread == NULL || thread->ctx == NULL) {
		macsurf_debug_log_writef("LIFE js exec: NO THREAD/CTX [%s]",
				name ? name : "(anon)");
		return 0;
	}
	if (txt == NULL || txtlen == 0) return 1;

	/* #265 - each js_exec is one JS execution burst: settle-once geometry
	 * must start fresh so the burst's FIRST read settles and the rest of
	 * its reads are answered from the settled tree (and so a previous
	 * burst's settle cannot silence THIS burst's first flush -- the harness
	 * calls js_exec directly with no pump in between). */
	qjs_geom_settle_begin();

	/* fixes587 BISECTION DIAG: short-circuit ALL script execution. With this
	 * on, no JS_Eval / thrown exception / build_backtrace / DOM-wrapper /
	 * timer work runs at all - the scripts are treated as clean no-ops so the
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

	/* fixes999 - the size cap is OFF by default (was 4 MB, and it SKIPPED the
	 * script outright rather than failing it, so a big bundle silently did
	 * not exist). Facebook-class bundles pass 4 MB, and a skipped bundle is
	 * indistinguishable from an engine that cannot run it -- which is exactly
	 * the confusion this batch has been unwinding. Set MACSURF_JS_MAX_BYTES
	 * to restore a ceiling. */
#ifndef MACSURF_JS_MAX_BYTES
/* fixes1136 (Option B): 256 KB script size cap.  Lets XenForo core-compiled.js
 * (~176 KB) and similar framework bundles through while still rejecting the
 * heaviest application bundles (>256 KB).  The 8s execution deadline provides
 * the primary safety net.  Set to 0 to disarm. */
#define MACSURF_JS_MAX_BYTES 262144UL
#endif
	/* fixes1143 - per-script size-cap bypass for essential bundles.
	 * Editor bundles that exceed the cap are let through; the 30s
	 * execution deadline bounds them instead.
	 *
	 * fixes1233 (#167) - hardware evidence (2026-08-20): the logged-in
	 * feed now renders (h=3448, 180 real sections, friend names visible)
	 * with 11 of 51 scripts skipped for size -- Facebook's real bundles
	 * hash their filenames per build (no stable name like
	 * "editor-compiled.js" to match), so they never qualify for the
	 * existing bypass and get silently dropped even though the 30s
	 * execution deadline is perfectly able to bound them, same as the
	 * editor bundle. Bypass by HOST instead of filename for Facebook's
	 * asset origins -- same fbcdn.net/facebook.net set already used for
	 * the UA table (macos9_fetch.c) -- to see what the skipped bundles
	 * actually add (deliberately broad per the maintainer's direction:
	 * "bypass limits just for facebook to see what happens"). */
	{
		static const char *const bypass[] = {
			"editor-compiled.js",
			"fbcdn.net",
			"facebook.net",
			NULL
		};
		int cap_bypass = 0;
		if (name != NULL) {
			const char *const *bp;
			for (bp = bypass; *bp != NULL; bp++) {
				if (strstr(name, *bp) != NULL) {
					cap_bypass = 1;
					break;
				}
			}
		}
		if (!cap_bypass &&
		    MACSURF_JS_MAX_BYTES != 0UL &&
		    txtlen > MACSURF_JS_MAX_BYTES) {
			g_js_skip_count++;
			macsurf_debug_log_writef(
				"LIFE js skip [%s len=%ld > %ld]",
				name ? name : "(anon)", (long)txtlen,
				(long)MACSURF_JS_MAX_BYTES);
			return 0;
		}
		if (cap_bypass &&
		    MACSURF_JS_MAX_BYTES != 0UL &&
		    txtlen > MACSURF_JS_MAX_BYTES) {
			macsurf_debug_log_writef(
				"LIFE js bypass [%s len=%ld]",
				name ? name : "(anon)", (long)txtlen);
		}
	}

	/* fixes523 DIAGNOSTIC: fingerprint the exact bytes handed to QuickJS so
	 * we can distinguish source corruption from an engine parse bug.  These
	 * bundles are valid pure-ASCII JS host-side, yet QuickJS reports
	 * SyntaxError / invalid-UTF-8 on them.  Logs len + 32-bit byte-sum (as
	 * hex via %p) + first 40 printable bytes; compare to the canonical
	 * fingerprint.  Remove once the cause is pinned.
	 * fixes847 (#167 S1 census gap) - WORK-prefixed. This is js_exec's
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
		g_js_exec_count++;
		g_js_exec_bytes += (long)txtlen;
		macsurf_debug_log_writef("LIFE js src [%s] len=%ld sum=%p head=%s",
			name ? name : "?", (long)txtlen, (void *)dsum, dhead);
	}

	/* fixes648 (regression fix): restore the per-bundle XenForo ES5 stub
	 * substitution that fixes522 removed. QuickJS ran the real preamble /
	 * core / editor bundles natively, but they CRASH on this engine - the
	 * documented cascade: preamble.min.js's `div.parentNode` hiddenscroll
	 * probe reads null and throws -> jQuery's Sizzle self-test throws ->
	 * core-compiled.js dies on jQuery.support -> XF.Element is never
	 * registered -> editor-compiled.js throws "newHandler of undefined". Net
	 * effect on hardware: the reply/post editor collapses to a bare one-line
	 * textarea (has-js never set, Froala never reveals/sizes it) - the "post
	 * box is shrunk/missing" report. The ES5 stubs still in this file sidestep
	 * all of it: s_xf_preamble_stub sets has-js + XF.ready, s_xf_core_stub
	 * defines XF.Element/XF.create, s_xf_editor_stub reveals+sizes the editor
	 * textarea (display:block, minHeight 280px). Substitute by script name;
	 * the real (crashing) bundle never runs. jQuery is left to run natively
	 * (the stubs are jQuery-independent), so its Sizzle crash is isolated to
	 * its own eval and does not block the editor. This was the fixes476-481
	 * mechanism that made real forum replies post on 68kmla under Duktape. */
	/* fixes998 - MACSURF_XF_STUBS: the master switch for all of the above.
	 *
	 * Default 0 = the REAL bundles run. The substitution above is training
	 * wheels from fixes648, fitted when the engine genuinely could not run
	 * them, and its stated cause (preamble's div.parentNode reading null)
	 * predates fixes846 (real DOM mutation), fixes878 (real libdom traversal)
	 * and fixes989-997 (the event model) -- every one of which rebuilt the
	 * surface it blames. Left on, it GUARANTEES the reply editor can never
	 * work: the real Froala bundle is skipped and s_xf_editor_stub fakes a
	 * bare textarea, which is exactly the "white box with no editor" reported
	 * on 68kmla.
	 *
	 * Kept behind a switch rather than deleted so the fallback is one line if
	 * the real bundles turn out to break something worse than they fix -- the
	 * stubs are the only thing that ever made a forum reply post here
	 * (fixes476-481, under Duktape). */
#ifndef MACSURF_XF_STUBS
#define MACSURF_XF_STUBS 0
#endif
	if (MACSURF_XF_STUBS && name != NULL) {
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
		 * property 'handle'/'extend' of undefined) - a big, pure-waste slice of
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
	 * natively (ES2023) - no per-filename stubs (preamble/core/editor/
	 * upload), no ES6->ES5 transpiler, no transpile cache.  Those were
	 * legacy crutches; in particular the old transpiler's async/await
	 * strip silently corrupted minified bundles (turning `asyncFoo()` into
	 * `     Foo()` -> "F is not defined").  Real scripts run as-is and
	 * compatibility gaps get fixed in the engine, not papered over per-site.
	 * The deadline (here) + memory limit (js_newheap) keep a misbehaving
	 * script from hanging or OOMing the machine. */
	/* fixes524: ROOT CAUSE.  QuickJS JS_Eval REQUIRES a NUL-terminated
	 * buffer - quickjs.h: "'input' must be zero terminated i.e.
	 * input[input_len] = '\0'".  The fetched script source from
	 * content_get_source_data is a raw byte buffer that is NOT
	 * NUL-terminated, so the lexer ran off the end into uninitialized heap,
	 * producing SyntaxErrors near end-of-file that varied between runs
	 * (reading heap garbage).  THIS - not ES6, corruption, or a CW8
	 * miscompile - is why real bundles never ran; the C-string-literal init
	 * evals were already NUL-terminated, so they worked.  Copy + terminate. */
	src = (char *)malloc(txtlen + 1);
	if (src == NULL) {
		macsurf_debug_log_writef("js: OOM copying src [%s len=%ld]",
			name ? name : "(anon)", (long)txtlen);
		return 0;
	}
	memcpy(src, txt, txtlen);
	src[txtlen] = '\0';

	/* fixes586 - push/pop (nest-safe) instead of set/clear-to-0, so a
	 * re-entrant exec can never erase an outer deadline. */
	{
		/* fixes640 - accumulate JS execution CPU per top-level eval. */
		/* fixes1070 - and SPLIT it into compile vs run.
		 *
		 * JS_Eval is compile-then-run in one call, so the fixes640
		 * bracket could only ever produce one number. QuickJS exposes
		 * the seam: JS_Eval with JS_EVAL_FLAG_COMPILE_ONLY returns the
		 * compiled function object, and JS_EvalFunction runs it. This
		 * is not a behaviour change -- quickjs.c's __JS_EvalInternal
		 * ends in exactly `if (COMPILE_ONLY) ret = fun_obj; else ret =
		 * JS_EvalFunctionInternal(ctx, fun_obj, this_obj, var_refs,
		 * sf)`, and for JS_EVAL_TYPE_GLOBAL (not _DIRECT) it has
		 * already set var_refs = NULL and sf = NULL, which is
		 * precisely what JS_EvalFunction passes. Same this_obj
		 * (ctx->global_obj) on both paths too. The two forms are
		 * equivalent for this call site; only the timing differs.
		 *
		 * JS_EvalFunction CONSUMES fun_obj on every path (JS_CallFree
		 * on the bytecode path, JS_FreeValue otherwise), so there is
		 * no leak and nothing to free here. */
		extern double macos9_micros(void);
		extern void macsurf_profile_accum_js(long us);
		/* R1.3 - the core names inline scripts "?inline script?"; every
		 * other name is a URL, i.e. an external script. */
		unsigned char ctype = (name != NULL && name[0] == '?')
				? SCRIPT_CENSUS_INLINE : SCRIPT_CENSUS_EXTERNAL;
		double prevdl = qjs_deadline_push((double)QJS_SCRIPT_TIMEOUT_MS);
		double t_js = macos9_micros();
		double t_mid;
		long c_us;
		long r_us = 0;
		JSValue fn;

		fn = JS_Eval(thread->ctx, src, txtlen,
				name ? name : "<script>",
				JS_EVAL_TYPE_GLOBAL |
				JS_EVAL_FLAG_COMPILE_ONLY);
		t_mid = macos9_micros();
		c_us = (long)(t_mid - t_js);
		if (JS_IsException(fn)) {
			/* Syntax error: fn IS the exception value, which is
			 * what plain JS_Eval would have returned. Propagate it
			 * unchanged so the error reporting below is identical
			 * to before -- a failed compile must not start looking
			 * like a different kind of failure. */
			val = fn;
			/* R1.3 - compile failed; nothing ran. */
			qjs_census_note(name, (long)txtlen, ctype,
					0, 0, c_us, 0);
		} else {
			val = JS_EvalFunction(thread->ctx, fn);
			r_us = (long)(macos9_micros() - t_mid);
			/* R1.3 - compiled ok; ran to completion or threw. */
			qjs_census_note(name, (long)txtlen, ctype,
					JS_IsException(val) ? 0 : 1,
					JS_IsException(val) ? 0 : 1,
					c_us, r_us);
		}
		macsurf_profile_accum_js(c_us + r_us);
		qjs_perf_note_script(name, (long)txtlen, c_us, r_us);
		qjs_deadline_pop(prevdl);
	}
	free(src);
	ok = !JS_IsException(val);
	if (!ok) {
		JSValue exc = JS_GetException(thread->ctx);
		const char *estr = JS_ToCString(thread->ctx, exc);
		/* fixes843b (#167 S1 census) - "err" (lowercase) never matched the
		 * crash-only log gate's "ERROR" (uppercase) keyword, so every JS
		 * exception on every page has been silently invisible in a normal
		 * build since fixes765. WORK-prefix it so a census build actually
		 * shows what's throwing. Remove the WORK prefix once the census
		 * round is done (this is deliberately loud -- one line per failed
		 * script, which is the whole point right now). */
		/* fixes873 - the MESSAGE goes FIRST, and the script name is shortened.
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
			g_js_exec_fail++;
		macsurf_debug_log_writef("LIFE qjs exec err: %s [%s len=%ld]",
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
					/* fixes873 - stack FIRST, short name after: same
					 * 255-byte-cap trap as the message line above, and a
					 * truncated stack is worth even less than none. */
					char sname[48];
					qjs_short_name(name, sname, (int)sizeof(sname));
					macsurf_debug_log_writef("LIFE qjs stack: %s [%s]",
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
	/* fixes1015 - pair every `LIFE js src` with an OUTCOME, so a clean run
	 * is distinguishable from a script that never finished. Failures already
	 * log above; this is the missing success half of the audit.
	 * fixes1032 - behind the audit switch: one flushed write per script,
	 * ~18 per page on a real site. */
#ifdef MACSURF_JS_AUDIT
	{
		char sname[48];
		qjs_short_name(name, sname, (int)sizeof(sname));
		macsurf_debug_log_writef("LIFE js done ok [%s len=%ld]",
				sname, (long)txtlen);
	}
#endif
	return 1;
}

/* fixes1117b (#265) - execute a script as an ES module.
 *
 * Compiles the source with JS_EVAL_TYPE_MODULE (which parses import/
 * export, enforces strict mode, and provides a separate module scope),
 * then resolves dependencies and executes.  For a module with imports,
 * the loader callback handles fetching those modules synchronously.
 *
 * Returns 1 on success, 0 on compile/resolve/execute failure.
 * Caller is responsible for NUL-terminating src. */
unsigned char js_exec_module(struct jsthread *thread,
	const unsigned char *txt, size_t txtlen, const char *name)
{
	JSValue val;
	JSContext *ctx;
	char *src;
	int ok;

	if (thread == NULL || thread->ctx == NULL) {
		macsurf_debug_log_writef(
			"LIFE js exec module: NO THREAD/CTX [%s]",
			name ? name : "(anon)");
		return 0;
	}
	if (txt == NULL || txtlen == 0) return 1;
	ctx = thread->ctx;

	/* #265 - module execution is a JS execution burst like js_exec:
	 * settle-once geometry starts fresh here too. */
	qjs_geom_settle_begin();

	/* Copy and NUL-terminate (same as js_exec) */
	src = (char *)malloc(txtlen + 1);
	if (src == NULL) {
		macsurf_debug_log_writef(
			"js: OOM copying module src [%s len=%ld]",
			name ? name : "(anon)", (long)txtlen);
		return 0;
	}
	memcpy(src, txt, txtlen);
	src[txtlen] = '\0';

	/* Pre-register this module so imports from OTHER modules
	 * can find it via the loader callback. */
	if (thread->heap != NULL && thread->heap->module_reg != NULL
			&& name != NULL) {
		qjs_module_register(thread->heap->module_reg,
			name, src, txtlen);
	}

	/* Compile and execute as a module in one call (same pattern
	 * as js_exec's JS_EVAL_TYPE_GLOBAL).  JS_EVAL_TYPE_MODULE
	 * compiles with strict mode + import/export, resolves
	 * dependencies via the loader callback, and executes. */
	{
		extern double macos9_micros(void);
		double t0 = macos9_micros();
		long mus;
		val = JS_Eval(ctx, src, txtlen,
			name ? name : "<module>",
			JS_EVAL_TYPE_MODULE);
		free(src);

		ok = !JS_IsException(val);
		mus = (long)(macos9_micros() - t0);
		/* R1.3 - a module is compiled, resolved and executed in the one
		 * JS_Eval call, so a failure cannot be attributed to a phase
		 * here; the whole time lands in run_us. */
		qjs_census_note(name, (long)txtlen, SCRIPT_CENSUS_MODULE,
				ok ? 1 : 0, ok ? 1 : 0, 0, mus);
		if (!ok) {
			JSValue exc = JS_GetException(ctx);
			qjs_log_exc(ctx, exc, "exec module err",
				name ? name : "<module>");
			JS_FreeValue(ctx, exc);
		}
		JS_FreeValue(ctx, val);
		macsurf_debug_log_writef(
			"LIFE qjs exec module done ok=%d us=%ld [%s]",
			(int)ok, mus, name ? name : "<module>");
	}

	/* Update page stats (same as js_exec) */
	g_js_exec_count++;
	g_js_exec_bytes += txtlen;

	return ok ? 1 : 0;
}

unsigned char js_fire_event(struct jsthread *thread, const char *type,
		struct dom_document *doc, struct dom_node *target)
{
	(void)doc; (void)target;
	if (thread == NULL || thread->ctx == NULL || type == NULL) return 0;
	/* Fire window.dispatchEvent(new Event(type)) */
	{
		/* fixes603 - buffer/guard mismatch overflow: bytes written are
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

/* fixes1235 (#167) - see js.h for the design. Called once from
 * html_reconvert_done, the SAME fire point fixes1090's resize/load
 * convergence hooks already use, so this cannot introduce a new trigger
 * frequency beyond what the reconvert debounce/floor already bounds, and
 * cannot re-enter the box-tree rebuild (it is not called from inside one).
 * If a delivered callback mutates the DOM, that mutation goes through the
 * ordinary macos9_js_mark_dom_dirty_node path exactly like any other
 * JS-driven mutation -- a normal follow-up debounced batch, not a new
 * recursive trigger. */
void js_fire_mutation_batch(struct jsthread *thread)
{
	static const char s_deliver_src[] =
		"(function(){try{"
		"if(typeof __msDeliverMutations==='function')"
		"__msDeliverMutations();"
		"}catch(e){}})();";
	if (thread == NULL || thread->ctx == NULL) return;
	macsurf_qjs__safe_eval(thread->ctx, s_deliver_src);
}

/* fixes652: real-build definition of interaction.c's click bridge (Gate 5).
 * The js_stub.c copy is gated `#ifndef WITH_QUICKJS`, so with QuickJS ON the
 * symbol was undefined and interaction.c failed to link the moment it was
 * rebuilt. Return 0 ("JS did not call preventDefault") so the browser's
 * navigation / form submit proceeds unchanged; per-element onclick handlers
 * still fire via fire_generic_dom_event at the call site. */
/* fixes989 - retained as a no-op ONLY for ABI: nothing calls it any more.
 * interaction.c now takes preventDefault from fire_generic_dom_event's return
 * value, because the dispatch it already performed is the real one. Delete
 * this once no build references the symbol. */

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
	/* fixes881 (Phase 0.7) - 'interactive', not 'complete', and NO load here.
	 *
	 * readyState went straight to 'complete' and this same function then fired
	 * `load` at the document, while html_finish_conversion had ALREADY fired
	 * `load` at the window ~30 lines before dom_to_box even started. Observed
	 * order: window-load -> DOMContentLoaded -> document-load. Spec order is
	 * DOMContentLoaded -> load, and `load` never reached window at all once the
	 * box tree existed.
	 *
	 * 'interactive' is the correct state at this point: the DOM (and here the
	 * initial box tree) exists, subresources have not necessarily settled.
	 * js_fire_window_load takes it to 'complete' and fires load. */
	static const char s_dom_ready_src[] =
		"(function(){try{"
		"if(typeof document==='undefined')return;"
		"if(document.__ms_ready_fired)return;"
		"document.__ms_ready_fired=true;"
		"document.readyState='interactive';"
		"try{document.dispatchEvent(new Event('DOMContentLoaded'));}catch(e){}"
		"try{if(typeof window!=='undefined')"
		"window.dispatchEvent(new Event('DOMContentLoaded'));}catch(e){}"
		"}catch(e){}})();";
	(void)doc;
	if (thread == NULL || thread->ctx == NULL) {
		return 0;
	}
	macsurf_qjs__safe_eval(thread->ctx, s_dom_ready_src);
	/* fixes862 (#289 probe) - was "qjs: DOMContentLoaded+load fired to
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
	macsurf_debug_log_writef("LIFE domready fired ctx=%p doc=%p",
			(void *)thread->ctx, (void *)doc);
	return 1;
}

/* fixes1096 - THE SLIDER PROBE, JS HALF.
 *
 * html.c's C-side probe (html_slider_probe) reads the DOM through libdom and
 * can name what exists around .featured-slides; it cannot see what the
 * PAGE'S OWN scripts see. This half runs in the page realm at the same probe
 * points ("ready" / "done" / "reconvert" -- html.c splices the label in) and
 * asks the questions only JS can: does jQuery exist, does jQuery.fn.slick,
 * and what does document.querySelector answer for the theme's featured
 * classes. A libdom walk and the engine's querySelector can disagree when
 * script rebuilt the tree, which is itself a finding.
 *
 * Emitted through __msLife so the lines carry the LIFE prefix and survive the
 * failures-only release filter; __msLife's per-navigation budget (60) covers
 * this (2 lines per probe point, <=6 probe points per navigation).
 * All reads, no mutations: safe to run at any probe point. */
void js_fire_slider_probe(struct jsthread *thread, const char *when)
{
	static const char s_fmt[] =
		"(function(){try{"
		"if(typeof document==='undefined'||!document.querySelector)return;"
		"var L='%s';"
		"var fs=document.querySelector('.featured-slides');"
		"var fg=document.querySelector('.featured-grid');"
		"var si=document.querySelector('.slick-initialized');"
		"var se=document.querySelector('section.featured');"
		"__msLife('SLIDER DOM['+L+'] fs='+(fs?'PRESENT':'MISSING')"
		" +' fg='+(fg?'PRESENT':'MISSING')"
		" +' si='+(si?'PRESENT':'MISSING')"
		" +' sec='+(se?'PRESENT kids='+se.children.length:'MISSING'));"
		"if(typeof jQuery!=='undefined'&&jQuery.fn){"
		"__msLife('SLIDER LIB['+L+'] jq='+typeof jQuery"
		" +' slick='+typeof jQuery.fn.slick);"
		"}else{"
		"__msLife('SLIDER LIB['+L+'] jq='+typeof jQuery+' (no fn)');"
		"}"
		"}catch(e){}})();";
	char src[1024];

	if (thread == NULL || thread->ctx == NULL) return;
	if (when == NULL) when = "?";
	sprintf(src, s_fmt, when);
	macsurf_qjs__safe_eval(thread->ctx, src);
}

/* fixes881 (Phase 0.7) - readyState='complete' + `load` at document AND window.
 *
 * Called from html_proceed_to_done's READY->DONE transition, i.e. once the box
 * tree exists AND base.active has fallen to 0 (every subresource settled) --
 * which is what the spec means by the load event.
 *
 * Before this, html_finish_conversion fired `load` ~30 lines BEFORE dom_to_box,
 * so it arrived before the box tree existed, and js_fire_dom_ready then fired a
 * SECOND `load` at the document afterwards. Net observed order was
 *   window load -> DOMContentLoaded -> document load
 * i.e. reversed, doubled, and window never saw a load once the page was really
 * there. Both of those fires are gone; this is the only one.
 *
 * Idempotent per realm (__ms_load_fired). html_proceed_to_done only reaches
 * content_set_done once per navigation, but object.c can call it repeatedly as
 * subresources land, so the guard is doing real work, not decoration. */
unsigned char js_fire_window_load(struct jsthread *thread, struct dom_document *doc)
{
	static const char s_window_load_src[] =
		"(function(){try{"
		"if(typeof document==='undefined')return;"
		"if(document.__ms_load_fired)return;"
		"document.__ms_load_fired=true;"
		"document.readyState='complete';"
		"try{document.dispatchEvent(new Event('load'));}catch(e){}"
		"try{if(typeof window!=='undefined')"
		"window.dispatchEvent(new Event('load'));}catch(e){}"
		"}catch(e){}})();";
	(void)doc;
	if (thread == NULL || thread->ctx == NULL) {
		return 0;
	}
	macsurf_qjs__safe_eval(thread->ctx, s_window_load_src);
	macsurf_debug_log_writef("LIFE window load fired ctx=%p doc=%p",
			(void *)thread->ctx, (void *)doc);
	/* fixes1013 - one line per page saying whether the JS actually ran and
	 * wired anything up. See macsurf_qjs_page_js_summary. */
	macsurf_qjs_page_js_summary();
	return 1;
}

/* fixes869 (#295) - fire `load` / `error` AT a <script> element.
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
/* fixes873 (#301) - document.currentScript.
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
	/* fixes872 (#300) - dispatchEvent now fires BOTH the addEventListener list
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

	/* #265 - firing a script onload/onerror handler is a C->JS event
	 * dispatch: a fresh execution burst, so settle-once geometry must not
	 * leak in from the script burst that just finished. */
	qjs_geom_settle_begin();

	fn = JS_Eval(ctx, s_fire_src, strlen(s_fire_src),
			"<script-load>", JS_EVAL_TYPE_GLOBAL);
	if (JS_IsException(fn)) {
		JSValue exc = JS_GetException(ctx);
		qjs_log_exc(ctx, exc, "script load setup",
			ok ? "load" : "error");
		JS_FreeValue(ctx, exc);
		JS_FreeValue(ctx, fn);
		return 0;
	}

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
		qjs_log_exc(ctx, exc,
			ok ? "script load handler exc" : "script error handler exc",
			ok ? "load" : "error");
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

/* fixes861 (#289) - pump EVERY live heap, not just g_heap.
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
	static int s_last_heaps = -1;
	int heaps = 0;
	/* #265 - settle-once geometry: JS has just yielded to the event loop,
	 * so the next burst (timer, microtask, event, script) starts fresh and
	 * its first read gets a real flush. MUST precede the reconvert-freeze
	 * gate below: while frozen, JS does not run, but when it unfreezes
	 * after a rebuild the old settle must not survive into the new burst.
	 * The per-callback clear in macsurf_qjs_run_timers still covers
	 * multiple timers within one pump. */
	qjs_geom_settle_begin();
	/* fixes898 - FREEZE JS while a reconvert box walk is in flight.
	 *
	 * The reconvert rebuilds the box tree from the DOM across MANY cooperative
	 * poll passes (convert_xml_to_box_inner self-reschedules every 20 nodes).
	 * This pump runs on every one of those passes and fires setTimeout /
	 * setInterval callbacks AND Promise microtasks -- all of which mutate the
	 * DOM. On a page whose JS never idles (hackaday's dirty-mark storm), that
	 * mutation frees+reuses nodes/strings the in-flight box walk is about to
	 * read -> box_construct reads recycled memory -> 0x2710 garbage-fn-ptr.
	 * HW (fixes897) proved it: the crash node VARIES run-to-run (a timing race),
	 * node pointers are valid, freemem is healthy -- not memory, not a single
	 * bad node, but JS racing the walk. fixes896's one-shot text-string pin
	 * cannot cover nodes CREATED-then-freed during the walk; the only correct
	 * model is to not interleave mutation with the rebuild (what a real browser
	 * does for synchronous layout). Suppress ALL JS execution for the reconvert
	 * window; it resumes the pass after html_reconvert_done clears the flag.
	 *
	 * FAIL-SAFE: the flag is global, and a content torn down mid-walk can bail
	 * out of convert_xml_to_box without html_reconvert_done ever running, which
	 * would leave it stuck TRUE and freeze JS forever. A reconvert completes in
	 * well under a second, so after a generous 10 s we force JS back on rather
	 * than deadlock. Edge-stamped locally so no other TU has to cooperate. */
	{
		extern int macsurf_reconvert_in_progress;
		static int s_reconv_was_active = 0;
		static double s_reconv_since = 0.0;
		if (macsurf_reconvert_in_progress) {
			double now = macsurf_qjs_get_now();
			if (s_reconv_was_active == 0) {
				s_reconv_was_active = 1;
				s_reconv_since = now;
			}
			if (now - s_reconv_since < 10000.0)
				return;   /* frozen: no JS during the active reconvert */
			/* stuck > 10 s -> fail-safe: unfreeze and pump. */
			macsurf_reconvert_in_progress = 0;
			s_reconv_was_active = 0;
		} else {
			s_reconv_was_active = 0;
		}
	}
	/* fixes862 (#289 probe) - fixes861 shipped with NO observable marker, so
	 * there was no way to tell from a log whether it was even in the build,
	 * let alone whether a second (iframe) heap exists to pump. Log the heap
	 * count, but ONLY when it changes: this runs every event-loop pass, so an
	 * unconditional line would drown the log. heaps>=2 on an iframe page also
	 * confirms browser_window_initialise_common (frames.c:232 -> js_newheap)
	 * really does give iframes their own heap -- an assumption fixes861 rests
	 * on and which I have not otherwise verified on hardware. */
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
		/* fixes868 (#294) - DRAIN THE MICROTASK QUEUE.
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
						macsurf_debug_log_writef("LIFE job exc: %s",
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
				/* fixes1236 (#167) - was WORK-gated (invisible in
				 * release) with no counter, so a page whose microtask
				 * queue is merely deep and one whose Promise chains
				 * never advance at all looked identical in every prior
				 * hardware log. First occurrence per navigation gets an
				 * immediate LIFE line; the running total is reported
				 * and reset in macsurf_qjs_emit_js_profile so a page
				 * that hits the cap every tick does not flood the log. */
				if (g_job_pump_cap_hits == 0) {
					macsurf_debug_log_writef(
						"LIFE job pump: hit cap %d, deferring rest",
						(int)QJS_MAX_JOBS_PER_PUMP);
				}
				g_job_pump_cap_hits++;
			}
		}
		h = next;
	}
}

#endif /* WITH_QUICKJS */
