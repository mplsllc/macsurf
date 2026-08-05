/*
 * MacSurf — macsurf_qjs_audit.c
 *
 * Performance counters and diagnostic emitters for the QuickJS engine.
 * Extracted from macsurf_qjs.c (2026-08-05 cleanup Phase 2).
 *
 * The counters live in macsurf_qjs.c (file-scope, non-static).  This file
 * only READS and reports them; engine code in macsurf_qjs.c writes them.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "utils/ns_errors.h"
#include "macos9.h"
#include "macsurf_debug.h"
#include "macsurf_qjs.h"
#include "macsurf_qjs_audit.h"
#include "macos9_reconvert.h"

#ifdef WITH_QUICKJS

/* ---- Timer profile ---- */
void macsurf_qjs_emit_timer_profile(void)
{
	macsurf_debug_log_writef(
		"LIFE JSTIME timers=%ld timer_us=%ld", g_timer_fires, g_timer_us);
	g_timer_fires = 0;
	g_timer_us = 0;
}

/* ---- Geometry stats ---- */
void macsurf_qjs_geom_stats(long *reads, long *us);
void macsurf_qjs_geom_stats(long *reads, long *us)
{
	if (reads != NULL) *reads = g_geom_reads;
	if (us != NULL)    *us    = g_geom_us;
}

/* ---- GC note ---- */
void macsurf_qjs_gc_note(long us);
void macsurf_qjs_gc_note(long us)
{
	g_perf_gc_runs++;
	if (us > 0) g_perf_gc_us += us;
}

/* ---- Wrapper stats ---- */
void macsurf_qjs_wrap_stats(long *wraps, long *hcompiles, long *hbytes);
void macsurf_qjs_wrap_stats(long *wraps, long *hcompiles, long *hbytes)
{
	if (wraps != NULL)     *wraps     = g_wrap_installs;
	if (hcompiles != NULL) *hcompiles = g_helper_compiles;
	if (hbytes != NULL)    *hbytes    = g_helper_bytes;
}

/* ---- Perf totals ---- */
void macsurf_qjs_perf_totals(long *evals, long *compile_us, long *run_us,
		long *gc_us, long *gc_runs, int *gc_armed);
void macsurf_qjs_perf_totals(long *evals, long *compile_us, long *run_us,
		long *gc_us, long *gc_runs, int *gc_armed)
{
	if (gc_armed != NULL)   *gc_armed   = g_perf_gc_armed;
	if (evals != NULL)      *evals      = g_perf_evals;
	if (compile_us != NULL) *compile_us = g_perf_compile_us;
	if (run_us != NULL)     *run_us     = g_perf_run_us;
	if (gc_us != NULL)      *gc_us      = g_perf_gc_us;
	if (gc_runs != NULL)    *gc_runs    = g_perf_gc_runs;
}

/* ---- JS profile emission ---- */
/* Emitted once per navigation from the NAV: DONE hook in browser_window.c,
 * beside the existing PERFACC / JSTIME lines. */
void macsurf_qjs_emit_js_profile(void);
void macsurf_qjs_emit_js_profile(void)
{
	int i;
	int rank;
	int order[QJS_PERF_SLOTS];
	int used = 0;

	macsurf_debug_log_writef(
		"LIFE JSPHASE evals=%ld bytes=%ld compile=%ldus run=%ldus "
		"gc=%ldus gcruns=%ld gcarmed=%d",
		g_perf_evals, g_perf_bytes, g_perf_compile_us, g_perf_run_us,
		g_perf_gc_us, g_perf_gc_runs, g_perf_gc_armed);

	/* fixes1071 — WHERE the run time went, and whether the wrapper-helper
	 * cache is holding.
	 *
	 *   ops     estimated bytecode ops (interrupts x 10000). Divide by
	 *           JSPHASE run= for ops/sec: a rate far under ~1M/s on this
	 *           hardware means the interpreter is NOT the bottleneck.
	 *   ncalls  every JS->C call (our bindings + QuickJS built-ins).
	 *   wraps   element wrappers built this navigation.
	 *   hcomp   helper-source COMPILES. Before fixes1071 this equalled
	 *           4 x wraps; it should now be a small constant per context.
	 *   hbytes  helper source bytes actually compiled. If this starts
	 *           tracking wraps again, the cache is broken -- that is the
	 *           regression this line exists to catch. */
	macsurf_debug_log_writef(
		"LIFE JSWHERE branches=%ld ncalls=%ld wraps=%ld hcomp=%ld "
		"hbytes=%ld",
		g_qjs_interrupts * 10000L, macsurf_qjs_ncalls,
		g_wrap_installs, g_helper_compiles, g_helper_bytes);
	/* fixes1078 — SPLIT the JS run time three ways: per-element wrapper
	 * install, time inside native bindings, and by subtraction whatever is
	 * left (real interpretation). nativeus is the 1-in-64 sample scaled
	 * back up, so treat it as an estimate with roughly sqrt(samples)
	 * accuracy -- ample for deciding where seconds live. */
	macsurf_debug_log_writef(
		"LIFE JSCOST wrapus=%ld nativeus=%ld nsamp=%ld",
		g_wrap_us, macsurf_qjs_native_us * 64L,
		macsurf_qjs_native_samp);
	g_wrap_us = 0;
	macsurf_qjs_native_us = 0;
	macsurf_qjs_native_samp = 0;

	/* fixes1073 (#265) — the forced-layout census.
	 *
	 *   flush     synchronous reflows script actually forced by measuring
	 *   declined  geometry reads that could NOT reflow (unsafe window, or
	 *             the per-navigation budget spent) and so answered
	 *             `undefined` -- the fixes1016 fallback
	 *   us        what the flushes cost, so the price of the measure/mutate
	 *             contract is visible rather than buried in the js total
	 *
	 * `declined` is the number that matters. Small means the contract is
	 * being honoured and widgets are getting real answers. Large means
	 * pages are thrashing or the budget is too tight, and the next round is
	 * incremental layout rather than a bigger budget. */
	{
		extern void macos9_reconvert_sync_stats(long *f, long *d,
				long *us);
		extern void macos9_reconvert_sync_reset(void);
		long sf = 0, sd = 0, su = 0;
		macos9_reconvert_sync_stats(&sf, &sd, &su);
		macsurf_debug_log_writef(
			"LIFE JSSYNC flush=%ld declined=%ld us=%ld", sf, sd, su);
		/* fixes1095 (#265 Round C1) — and WHERE that `us` went. Round B
		 * measured flush=2 us=3353022, i.e. ~1.7s per synchronous
		 * reconvert, which exhausted the 2s budget and produced 522
		 * further declines. Cost is now the blocker, and it is NOT
		 * cascade/layout (PERFACC has those at 2.15s for the WHOLE
		 * navigation while two flushes alone cost 3.35s) -- it is the
		 * reconvert's own O(document) teardown and box construction.
		 * Emitted next to JSSYNC so the two are read together. */
		{
			extern void html_reconvert_phase_report(void);
			html_reconvert_phase_report();
		}
		/* fixes1075 — and WHY the declines happened. fixes1073's first
		 * hardware log read `flush=0 declined=660` on hackaday, which
		 * says the feature never ran but not which guard stopped it.
		 * `notdone` dominating would mean geometry is gated out of the
		 * entire page-load window -- i.e. exactly when scripts
		 * initialise and measure -- and the gate, not the budget, is the
		 * next thing to fix. */
		if (sd > 0) {
			extern void macos9_reconvert_sync_reasons(long *nd,
					long *ac, long *pa, long *ip,
					long *bu, long *bz);
			long rnd = 0, rac = 0, rpa = 0, rip = 0, rbu = 0, rbz = 0;
			macos9_reconvert_sync_reasons(&rnd, &rac, &rpa, &rip,
					&rbu, &rbz);
			macsurf_debug_log_writef(
				"LIFE JSSYNCWHY notdone=%ld active=%ld paint=%ld "
				"inprog=%ld budget=%ld busy=%ld",
				rnd, rac, rpa, rip, rbu, rbz);
		}
		{	/* fixes1077 — how many measurements, and what they cost. */
			long gr = 0, gu = 0;
			macsurf_qjs_geom_stats(&gr, &gu);
			macsurf_debug_log_writef(
				"LIFE JSGEOM reads=%ld gateus=%ld ready=%ld "
				"done=%ld unstable=%ld",
				gr, gu, g_geom_at_ready, g_geom_at_done,
				g_geom_unstable);
			macsurf_debug_log_writef(
				"LIFE JSGEOMANS undef=%ld zero=%ld real=%ld",
				g_geom_undef, g_geom_zero, g_geom_real);
			g_geom_undef = 0; g_geom_zero = 0; g_geom_real = 0;
			g_geom_reads = 0;
			g_geom_us = 0;
			g_geom_at_ready = 0;
			g_geom_at_done = 0;
			g_geom_unstable = 0;
		}
		macos9_reconvert_sync_reset();
		{	/* fixes1095 — per-navigation, like every counter here. */
			extern void html_reconvert_phase_reset(void);
			html_reconvert_phase_reset();
		}
	}
	g_qjs_interrupts  = 0;
	macsurf_qjs_ncalls = 0;
	g_wrap_installs   = 0;
	g_helper_compiles = 0;
	g_helper_bytes    = 0;

	for (i = 0; i < QJS_PERF_SLOTS; i++) {
		if (g_perf_slot[i].name[0] != '\0') order[used++] = i;
	}
	/* Selection sort by total cost, descending. N is 8; a sort here costs
	 * nothing and a log read should not have to rank rows by eye. */
	for (rank = 0; rank < used; rank++) {
		int best = rank;
		long best_cost = g_perf_slot[order[rank]].compile_us +
				 g_perf_slot[order[rank]].run_us;
		int j;
		for (j = rank + 1; j < used; j++) {
			long c = g_perf_slot[order[j]].compile_us +
				 g_perf_slot[order[j]].run_us;
			if (c > best_cost) { best_cost = c; best = j; }
		}
		if (best != rank) {
			int t = order[rank]; order[rank] = order[best];
			order[best] = t;
		}
	}
	for (rank = 0; rank < used; rank++) {
		struct qjs_perf_slot *s = &g_perf_slot[order[rank]];
		macsurf_debug_log_writef(
			"LIFE JSTOP%d %s b=%ld c=%ldus r=%ldus n=%ld",
			rank + 1, s->name, s->bytes, s->compile_us,
			s->run_us, s->evals);
	}

	/* Zero for the next navigation, exactly as the PERFACC accumulators do
	 * -- every hardware log we pull covers two or more loads, and carrying
	 * one page's bundles into the next page's table would misattribute the
	 * cost to whichever site happened to be loaded second. */
	for (i = 0; i < QJS_PERF_SLOTS; i++) {
		g_perf_slot[i].name[0]    = '\0';
		g_perf_slot[i].bytes      = 0;
		g_perf_slot[i].compile_us = 0;
		g_perf_slot[i].run_us     = 0;
		g_perf_slot[i].evals      = 0;
	}
	g_perf_evals      = 0;
	g_perf_bytes      = 0;
	g_perf_compile_us = 0;
	g_perf_run_us     = 0;
	g_perf_gc_us      = 0;
	g_perf_gc_runs    = 0;
}

/* ---- Audit reset ---- */
/* fixes1016 -- refill every audit budget at each new JS realm (= each
 * navigation), so page 2 and the iframes are audited too; the fixes1015
 * per-session budgets died mid-page-one and 68kmla was never covered. */
void macsurf_qjs_audit_reset(void)
{
	/* fixes1022 — audit OFF by default (every line is a flushed write);
	 * define MACSURF_JS_AUDIT to get the fixes1020 skeleton back.
	 *
	 * fixes1029 — EXCEPT the removal audit, which is on for this round.
	 * The device's pagemap shows every DIV.entry-intro on hackaday with
	 * kids=0 -- the article titles, bylines and excerpts are GONE from the
	 * DOM -- while the same markup parsed in the harness has them, and the
	 * census reports a burst of remove=56. So a script is emptying the
	 * entries and we need its victims named. Removal-only keeps the volume
	 * to roughly one line per removed node instead of the full audit. */
#ifdef MACSURF_JS_AUDIT
	g_mut_audit_budget = 60;
	g_evreg_audit = 20;
	g_evmiss_audit = 10;
	g_evfire_audit = 20;
	g_mslife_audit = 60;
	g_geom_audit = 30;
#else
	g_mut_audit_budget = 0;
	g_evreg_audit = 0;
	g_evmiss_audit = 0;
	g_evfire_audit = 0;
	/* fixes1092 — geometry-only audit, ON for this round, independent of
	 * the other MACSURF_JS_AUDIT channels (which stay off — this page has
	 * enough DOM/event traffic that the full audit would drown the budget
	 * before slick's own height reads ever got logged). Answers the open
	 * question from fixes1090b/1090c: is `.featured-slides div`'s height
	 * read as settled or unsettled at the moment the theme script measures
	 * it, and what does slick's own setPosition() read on the reconverts
	 * the resize/load fire triggers. Revert to 0 once that's answered —
	 * `LIFE geom` is a flushed write per call like every other audit line. */
	g_geom_audit = 40;
#endif
	/* fixes1039 — the WANT channel stays ON, independent of the audit.
	 *
	 * The engine now runs hackaday's whole stack -- front page, article
	 * page and the Jetpack comment iframe -- with ZERO exceptions. So the
	 * remaining rendering failures are not crashes; they are the
	 * capabilities we STUB, where the page asks a question, gets a
	 * confident wrong answer, and quietly takes the other branch. That is
	 * the fixes1031 shape again (a lie, not a gap) and it is invisible by
	 * construction.
	 *
	 * __msLife carries exactly those: every matchMedia query (we answer
	 * `false` to all of them), every MutationObserver/ResizeObserver
	 * .observe (no-ops), every IntersectionObserver target (always
	 * intersecting). It fires only when a page ASKS for something stubbed,
	 * so a page that wants nothing pays nothing -- a handful of lines
	 * rather than the hundreds the full audit emits. This is the
	 * implement-next list, written by real sites instead of by me. */
	g_mslife_audit = 60;
	/* fixes1030 — the three ways script can DELETE page content:
	 * removeChild, textContent= and innerHTML=. All three now log their
	 * target's identity on this shared budget, independent of
	 * MACSURF_JS_AUDIT, because hackaday's article entries reach the box
	 * tree ALREADY EMPTY (kids=0 at the `ready` dump) while the same
	 * markup parsed without script keeps all four children -- and the
	 * removal audit cleared removeChild entirely (120 lines, every one a
	 * Typekit font probe or a jQuery feature-detect element). The census
	 * in the same window reports text=35, and the river has 7 entries. */
	/* fixes1032 — OFF with the rest of the audit. These three named the
	 * deleter (fixes1029-1031); they cost a flushed write per mutation and
	 * are not wanted in a baseline. */
#ifdef MACSURF_JS_AUDIT
	g_rm_audit_budget = 260;
#else
	g_rm_audit_budget = 0;
#endif
	g_pn_logged = 0; /* fixes1110 — per-navigation, not per-process */
	qjs_want_reset(); /* R1.2 — per-page WANT dedupe set */
}

/* ---- Page JS summary ---- */
/* fixes1013 — COUNT WHAT ACTUALLY RAN.
 *
 * "Zero JS exceptions" was being read as "JavaScript works", and it is not
 * evidence of that at all: it is equally consistent with no script executing.
 * The failure paths were LIFE-prefixed in fixes1000 but every SUCCESS
 * breadcrumb stayed WORK-prefixed, and WORK is compiled out of shipping
 * builds -- so a hardware log could show errors and could NOT show whether a
 * single script ever reached QuickJS. That asymmetry made the log actively
 * misleading rather than merely incomplete.
 *
 * These are session-cumulative and emitted once per page in the summary
 * below, so the cost is one line per navigation, not one per script.
 *
 * The counters are DEFINED in macsurf_qjs.c (beside qjs_perf_note_script)
 * and declared extern in macsurf_qjs_audit.h; this TU only reads them.
 * The Phase 2 extraction briefly left static copies here, which shadowed
 * the real counters and made the summary read zeroes forever. */

/* One LIFE line per page answering "did the page's JavaScript run, and did it
 * do anything" without needing a verbose build. Called from
 * js_fire_window_load. */
void macsurf_qjs_page_js_summary(void);
void macsurf_qjs_page_js_summary(void)
{
	JSContext *ctx = macsurf_qjs_current_ctx();
	int has_jq = 0, has_xf = 0, doc_l = 0, win_l = 0, node_l = 0;

	if (ctx != NULL) {
		JSValue g = JS_GetGlobalObject(ctx);
		JSValue v;

		v = JS_GetPropertyStr(ctx, g, "jQuery");
		has_jq = !JS_IsUndefined(v) && !JS_IsNull(v);
		JS_FreeValue(ctx, v);
		v = JS_GetPropertyStr(ctx, g, "XF");
		has_xf = !JS_IsUndefined(v) && !JS_IsNull(v);
		JS_FreeValue(ctx, v);

		/* Listener counts say whether the page WIRED ITSELF UP, which is
		 * the thing that actually distinguishes "scripts ran" from
		 * "scripts ran and the page is now interactive". */
		{
			static const char *cnt_src =
				"(function(){var d=0,w=0;try{"
				"var L=document._listeners||{};for(var k in L)"
				"d+=(L[k]&&L[k].length)||0;"
				"var W=this._winListeners||{};for(var j in W)"
				"w+=(W[j]&&W[j].length)||0;"
				"}catch(e){}return d*10000+w;})()";
			JSValue r = JS_Eval(ctx, cnt_src, strlen(cnt_src),
					"<jssum>", JS_EVAL_TYPE_GLOBAL);
			if (!JS_IsException(r)) {
				int32_t n = 0;
				JS_ToInt32(ctx, &n, r);
				doc_l = n / 10000;
				win_l = n % 10000;
			} else {
				JS_FreeValue(ctx, JS_GetException(ctx));
			}
			JS_FreeValue(ctx, r);
		}
		JS_FreeValue(ctx, g);
	}
	node_l = s_reg_n_registered;

	macsurf_debug_log_writef(
		"LIFE JS PAGE: scripts=%ld bytes=%ld failed=%ld "
		"jquery=%d xf=%d doclisten=%d winlisten=%d nodelisten=%d",
		g_js_exec_count, g_js_exec_bytes, g_js_exec_fail,
		has_jq, has_xf, doc_l, win_l, node_l);

	/* R1.3 — the per-script census: one LIFE SCRIPT CENSUS line per
	 * execution, in record order, then clear so the next emit starts
	 * fresh.  Cleared HERE rather than in macsurf_qjs_audit_reset() —
	 * js_newheap (which calls audit_reset) runs per browser_window AND
	 * per (i)frame, so an iframe created mid-parse would wipe the main
	 * document's entries before this summary ever emitted them. */
	{
		static const char *census_type[3] = { "inline", "external",
				"module" };
		long i;
		for (i = 0; i < g_script_census_count; i++) {
			struct script_census_entry *e = &g_script_census[i];
			const char *da = "-";
			if (e->defer_async == 3) da = "DA";
			else if (e->defer_async == 2) da = "A";
			else if (e->defer_async == 1) da = "D";
			macsurf_debug_log_writef(
				"LIFE SCRIPT CENSUS %s %ld %s %s %s %ld %s",
				(e->type < 3) ? census_type[e->type] : "?",
				e->size, da,
				e->compiled ? "ok" : "FAIL",
				e->completed ? "ok" : "FAIL",
				e->compile_us + e->run_us, e->name);
		}
		if (g_script_census_full > 0) {
			macsurf_debug_log_writef(
				"LIFE SCRIPT CENSUS (overflow: %ld more "
				"executions not listed)", g_script_census_full);
		}
		g_script_census_count = 0;
		g_script_census_full  = 0;
	}
}

#endif /* WITH_QUICKJS */
