/*
 * MacSurf  -  macsurf_qjs_audit.c
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

	/* fixes1273 (#167) - stage-by-stage timer pipeline audit.
	 *
	 * A real facebook.com load logged 83 SECONDS of JS with timers=0 and
	 * raf=0. Since requestAnimationFrame is implemented as
	 * setTimeout(fn,16), those are one fact, not two: deferred work is not
	 * being delivered at all. That matters far beyond Facebook - it is
	 * every site whose content arrives through a timer, a promise
	 * continuation scheduled from a timer, or an animation frame.
	 *
	 * timers=0 alone only says the LAST stage never happened. These say
	 * which one did not, by naming the gap:
	 *   created=0                  setTimeout was never called (or refused)
	 *   created>0, pumps=0         the event loop never pumped at all
	 *   pumps>0, frozen~=pumps     the reconvert gate suppressed JS
	 *   owner_skip>0 with due=0    timers live in a realm nothing pumps
	 *   due=0 with pending>0       nothing ever reached its deadline
	 *   due>0 but fires=0          invocation itself is being suppressed
	 *   evicted>0                  callbacks lost to arena pressure (256)
	 *
	 * Counters are cumulative across the session; pending is instantaneous. */
	{
		long created = -1, fires = -1, due = -1, evicted = -1;
		long owner_skip = -1, pumps = -1, frozen = -1, pending = -1;
		extern void macsurf_qjs_timer_stats(long *, long *, long *,
				long *, long *, long *, long *, long *);
		macsurf_qjs_timer_stats(&created, &fires, &due, &evicted,
				&owner_skip, &pumps, &frozen, &pending);
		macsurf_debug_log_writef(
			"LIFE TIMERAUD created=%ld pending=%ld pumps=%ld "
			"frozen=%ld owner_skip=%ld due=%ld fires=%ld "
			"evicted=%ld",
			created, pending, pumps, frozen, owner_skip, due,
			fires, evicted);
	}

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

/* fixes1259 (#167) - read a zero-arg JS global function's int32 return
 * value. Same one-shot JS_Eval pattern __msRequireTraceTotal already used
 * (fixes1247) for exactly this purpose, generalized so the six FBLOADER
 * counters below don't each need their own copy of this boilerplate.
 * Returns -1 if ctx is unusable, the function is missing, or it threw. */
static long
qjs_read_int_global(JSContext *ctx, const char *fn_name)
{
	/* fn_name is interpolated twice into the template below; the longest
	 * caller ("__msFBLoader_rlTargetCalls", 27 bytes) plus the ~102
	 * bytes of fixed template text runs to ~156 - sized with headroom,
	 * not tight, since this is a fixed set of literal names we control,
	 * not user input, but sprintf here has no bounds check of its own. */
	char src[320];
	JSValue r;
	long result = -1;

	if (ctx == NULL || fn_name == NULL) return -1;
	if (strlen(fn_name) > 64) return -1;

	sprintf(src,
		"(function(){try{"
		"return (typeof globalThis.%s==='function')?"
		"globalThis.%s():-1;"
		"}catch(e){return -1;}})()",
		fn_name, fn_name);

	r = JS_Eval(ctx, src, strlen(src), "<jsfbldr>", JS_EVAL_TYPE_GLOBAL);
	if (!JS_IsException(r)) {
		int32_t n = 0;
		JS_ToInt32(ctx, &n, r);
		result = (long) n;
	} else {
		JS_FreeValue(ctx, JS_GetException(ctx));
	}
	JS_FreeValue(ctx, r);
	return result;
}

/* ---- JS profile emission ---- */
/* Emitted once per navigation from the NAV: DONE hook in browser_window.c,
 * beside the existing PERFACC / JSTIME lines. */
/* fixes1289 (#167) - one Facebook boot report used to be one ~3 KB JSON value.
 * The durable logger has a deliberate 1024-byte line bound, so hardware only
 * retained the healthy module-prefix and silently lost the decisive renderer,
 * SSR and guarded-error suffix.  Keep every answer independently bounded and
 * name the actual page contracts instead: did ServerJSPayloadListener consume
 * the data-sjs islands, what state did Facebook's SSR machine reach, and what
 * did ErrorGuard retain?  These reads do not call a page entry point or change
 * DOM state. */
static void
qjs_emit_fb_value(JSContext *ctx, const char *label, const char *src)
{
	JSValue r;
	const char *s;

	if (ctx == NULL || label == NULL || src == NULL) return;
	r = JS_Eval(ctx, src, strlen(src), "<jsfbboot>", JS_EVAL_TYPE_GLOBAL);
	if (JS_IsException(r)) {
		JS_FreeValue(ctx, JS_GetException(ctx));
		macsurf_debug_log_writef("LIFE %s eval exception", label);
		JS_FreeValue(ctx, r);
		return;
	}
	if (JS_IsNull(r)) {
		JS_FreeValue(ctx, r);
		return;
	}
	s = JS_ToCString(ctx, r);
	macsurf_debug_log_writef("LIFE %s %s", label,
			(s != NULL) ? s : "(null)");
	if (s != NULL) JS_FreeCString(ctx, s);
	JS_FreeValue(ctx, r);
}

/* fixes1287 (#167) - emit at the window-load edge as well as the optional
 * final-profile edge, using the owning context rather than a process-global
 * realm guess. */
void macsurf_qjs_emit_fb_boot(struct JSContext *ctx)
{
	static const char fbpayload_src[] =
		"(function(){try{var g=globalThis;"
		"if(!g.location||String(g.location.hostname||'').indexOf('facebook.com')<0)"
			"return null;"
		"var a=document.querySelectorAll('script[data-sjs]'),p=0,d=0,ins=0,"
			"lm=0,lb=0,first='';"
		"for(var i=0;i<a.length;i++){var e=a[i],want=e.dataset&&e.dataset.contentLen,"
			"got=String(e.textContent.length);"
			"if(e instanceof HTMLScriptElement)ins++;"
			"if(e.dataset&&e.dataset.processed)d++;else p++;"
			"if(want==null||want===got)lm++;else{lb++;if(!first)first=want+'/'+got;}}"
		"return 'total='+a.length+' processed='+d+' pending='+p+"
			"' scriptInstance='+ins+' lenMatch='+lm+' lenBad='+lb+"
			"(first?' firstBad='+first:'');"
		"}catch(e){return 'error='+String((e&&e.message)||e).slice(0,700);}})()";
	static const char fbroot_src[] =
		"(function(){try{var g=globalThis;"
		"if(!g.location||String(g.location.hostname||'').indexOf('facebook.com')<0)"
			"return null;"
		"if(typeof g.require!=='function')return 'require=missing';"
		"var ru=g.require('CometClientRootRendererUtils'),"
			"si=g.require('CometSSRClientInjector'),sd=si.getSSRData?si.getSSRData():null,"
			"ps=si.getArrivedPayloads?si.getArrivedPayloads():[],"
			"eid=sd&&sd.eid,root=eid?document.getElementById(eid):null,env=g.Env||{};"
		"return 'clientRendered='+(ru.getIsClientSideRendered?"
			"!!ru.getIsClientSideRendered():'na')+"
			"' payloads='+ps.length+' ssrData='+(sd?'yes':'no')+"
			"' enabled='+(sd&&sd.enabled)+' eid='+(eid||'')+"
			"' root='+(!!root)+' rootKids='+(root&&root.childNodes?root.childNodes.length:-1)+"
			"' stateManager='+(env.use_ssr_state_manager)+"
			"' splash='+(!!document.getElementById('splash-screen'))+"
			"' marker='+(!!document.getElementById('has-finished-comet-page'));"
		"}catch(e){return 'error='+String((e&&e.message)||e).slice(0,700);}})()";
	static const char fbstate_src[] =
		"(function(){try{var g=globalThis;"
		"if(!g.location||String(g.location.hostname||'').indexOf('facebook.com')<0)"
			"return null;"
		"if(typeof g.require!=='function')return 'require=missing';"
		/* fixes1290 (#167) - the old audit read sm.default.debug(), but the
		 * current Facebook module does not expose its live state there.  That
		 * made a perfectly populated LastPayloadReceived state print as three
		 * blank fields and sent the investigation in the wrong direction.
		 * This is Facebook's own read-only devtools store, used by its SSR
		 * diagnostics; report the state names rather than its multi-KB JSON. */
		"var ds=g.require('CometDevToolsSSRStateManagerDebugStore'),"
			"j=ds&&ds.getState?ds.getState():{},a=j.arrivedPayloads||[],"
			"sh=j.stateHistory||[],names=[];"
		"for(var i=0;i<sh.length;i++)names.push(sh[i]&&sh[i].state||'?');"
		"var h=names.join('>');if(h.length>500)h=h.slice(h.length-500);"
		"return 'current='+(j.currentState||'')+' terminal='+(j.isTerminal)+"
			"' outcome='+(j.ssrOutcome||'')+' arrived='+a.length+"
			"' request='+(j.clientRequestID||'')+' history='+h;"
		"}catch(e){return 'error='+String((e&&e.message)||e).slice(0,700);}})()";
	static const char fberror_src[] =
		"(function(){try{var g=globalThis;"
		"if(!g.location||String(g.location.hostname||'').indexOf('facebook.com')<0)"
			"return null;"
		"if(typeof g.require!=='function')return 'require=missing';"
		"var ep=g.require('ErrorPubSub'),h=ep&&ep.history||[],out='count='+h.length;"
		"for(var i=Math.max(0,h.length-3);i<h.length;i++){var e=h[i]||{},m=String("
			"e.message||e.messageFormat||'').replace(/[\\r\\n]+/g,' ');"
			"if(m.length>220)m=m.slice(0,220);out+=' | '+i+':'+(e.project||'')+':'"
			"+(e.name||e.type||'')+':'+m;}"
		"return out;"
		"}catch(e){return 'error='+String((e&&e.message)||e).slice(0,700);}})()";
	/* Remembered-account diagnostic: identify the actual actionable Continue
	 * element independently of native hit-testing. Values are structural only;
	 * no form field values or cookie values are read. */
	static const char fbcontinue_src[] =
		"(function(){try{var g=globalThis;"
		"if(!g.location||String(g.location.hostname||'').indexOf('facebook.com')<0)"
			"return null;"
		"var a=document.querySelectorAll('a,button,input,[role=button]'),out=[],n=0;"
		"function q(v){return String(v||'').replace(/[\\r\\n\\t ]+/g,' ').slice(0,120)}"
		"for(var i=0;i<a.length&&n<4;i++){var e=a[i],"
			"t=q(e.value||e.textContent);if(t.toLowerCase().indexOf('continue')<0)continue;"
			"var f=e.form||null,p=e.parentNode;"
			"out.push('tag='+e.tagName+' text='+t+' type='+q(e.type)+"
			"' role='+q(e.getAttribute&&e.getAttribute('role'))+"
			"' href='+q(e.href)+' onclick='+(typeof e.onclick)+"
			"' parent='+(p&&p.tagName||'')+' form='+(!!f)+"
			"' method='+(f&&q(f.method)||'')+' action='+(f&&q(f.action)||''));n++;}"
		"return 'matches='+n+(out.length?' | '+out.join(' | '):'');"
		"}catch(e){return 'error='+String((e&&e.message)||e).slice(0,700);}})()";
	/* fixes1310 (#167, A3) - the real loader's Me() chooses its execution
	 * wrapper at every __d()/requireLazy call:
	 *
	 *   t.TimeSlice && t.TimeSlice.guard ? t.TimeSlice.guard : Ne
	 *
	 * Test 81 proved the synchronous Ne path against the real recovered
	 * loader bundle, but it deliberately never required the TimeSlice module.
	 * That leaves the real-page scheduling branch untested, which matters for
	 * the still-open repeat-navigation regression.  Report the state that
	 * selects that branch at both existing FB boot audit edges (window load
	 * and NAV:DONE), without requiring TimeSlice or calling TimeSlice.guard()
	 * or ScheduleJSWork: this is an observation, not another source of page
	 * behaviour.  __debug.getModules() is Facebook's own existing diagnostic
	 * view and lets the hardware read distinguish "global missing" from
	 * "module absent/not yet ready". */
	static const char fbtasks_src[] =
		"(function(){try{var g=globalThis;"
		"if(!g.location||String(g.location.hostname||'').indexOf('facebook.com')<0)"
			"return null;"
		"var ts=g.TimeSlice,sj=g.ScheduleJSWork,mods=null,m=null,d=null,w='na';"
		"try{if(typeof g.require==='function'){"
			"d=g.require('__debug');"
			"mods=d&&d.getModules?d.getModules():null;"
			"m=mods&&mods.TimeSlice;"
			"if(d&&d.debugUnresolvedDependencies)"
				"w=String(d.debugUnresolvedDependencies(['TimeSlice']));}}catch(e){}"
		"return 'global='+typeof ts+' guard='+typeof(ts&&ts.guard)+"
			"' schedule='+typeof sj+' module='+(m?'yes':'no')+"
			"' ready='+(m?!!m.factoryFinished:'na')+"
			"' deps='+(m&&m.dependencies?m.dependencies.length:'na')+"
			"' wait='+w.replace(/[\\r\\n]+/g,' ').slice(0,360);"
		"}catch(e){return 'error='+String((e&&e.message)||e).slice(0,700);}})()";

	if (ctx == NULL) return;
	qjs_emit_fb_value(ctx, "FBPAYLOAD", fbpayload_src);
	qjs_emit_fb_value(ctx, "FBROOT", fbroot_src);
	qjs_emit_fb_value(ctx, "FBSTATE", fbstate_src);
	qjs_emit_fb_value(ctx, "FBERROR", fberror_src);
	qjs_emit_fb_value(ctx, "FBCONTINUE", fbcontinue_src);
	qjs_emit_fb_value(ctx, "FBTASK", fbtasks_src);
}

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

	/* fixes1071  -  WHERE the run time went, and whether the wrapper-helper
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
	 *           tracking wraps again, the cache is broken - that is the
	 *           regression this line exists to catch. */
	macsurf_debug_log_writef(
		"LIFE JSWHERE branches=%ld ncalls=%ld wraps=%ld hcomp=%ld "
		"hbytes=%ld",
		g_qjs_interrupts * 10000L, macsurf_qjs_ncalls,
		g_wrap_installs, g_helper_compiles, g_helper_bytes);
	/* fixes1078  -  SPLIT the JS run time three ways: per-element wrapper
	 * install, time inside native bindings, and by subtraction whatever is
	 * left (real interpretation). nativeus is the 1-in-64 sample scaled
	 * back up, so treat it as an estimate with roughly sqrt(samples)
	 * accuracy - ample for deciding where seconds live. */
	macsurf_debug_log_writef(
		"LIFE JSCOST wrapus=%ld nativeus=%ld nsamp=%ld",
		g_wrap_us, macsurf_qjs_native_us * 64L,
		macsurf_qjs_native_samp);
	g_wrap_us = 0;
	macsurf_qjs_native_us = 0;
	macsurf_qjs_native_samp = 0;

	/* fixes1073 (#265)  -  the forced-layout census.
	 *
	 *   flush     synchronous reflows script actually forced by measuring
	 *   declined  geometry reads that could NOT reflow (unsafe window, or
	 *             the per-navigation budget spent) and so answered
	 *             `undefined` - the fixes1016 fallback
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
		/* fixes1095 (#265 Round C1)  -  and WHERE that `us` went. Round B
		 * measured flush=2 us=3353022, i.e. ~1.7s per synchronous
		 * reconvert, which exhausted the 2s budget and produced 522
		 * further declines. Cost is now the blocker, and it is NOT
		 * cascade/layout (PERFACC has those at 2.15s for the WHOLE
		 * navigation while two flushes alone cost 3.35s) - it is the
		 * reconvert's own O(document) teardown and box construction.
		 * Emitted next to JSSYNC so the two are read together. */
		{
			extern void html_reconvert_phase_report(void);
			html_reconvert_phase_report();
		}
		/* fixes1075  -  and WHY the declines happened. fixes1073's first
		 * hardware log read `flush=0 declined=660` on hackaday, which
		 * says the feature never ran but not which guard stopped it.
		 * `notdone` dominating would mean geometry is gated out of the
		 * entire page-load window - i.e. exactly when scripts
		 * initialise and measure - and the gate, not the budget, is the
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
		{	/* fixes1077  -  how many measurements, and what they cost. */
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
		{	/* fixes1095  -  per-navigation, like every counter here. */
			extern void html_reconvert_phase_reset(void);
			html_reconvert_phase_reset();
		}
	}

	/* fixes1236 (#167)  -  is the event loop actually DELIVERING deferred
	 * work on this page, not just executing synchronously during parse.
	 *
	 *   cap_hits  macsurf_qjs_pump_all's microtask drain hit
	 *             QJS_MAX_JOBS_PER_PUMP and deferred the rest -- a page
	 *             whose Promise chains are simply DEEP looks different from
	 *             one where they never advance at all (that would show
	 *             JSTIME timers=0 AND cap_hits=0 AND raf=0: nothing queued,
	 *             not something queued and starved).
	 *   raf       requestAnimationFrame callbacks actually invoked.
	 *   rafOwn    1 if window.requestAnimationFrame is still OUR function,
	 *             0 if some page script overwrote it with its own scheduler
	 *             (a real Comet/lazy-load pattern) -- in that case `raf`
	 *             undercounts by construction and should not be read as
	 *             "rAF is dead" on its own. -1 means the identity marker
	 *             itself was missing (register_browser_globals did not run
	 *             for this ctx, or ran before this counter existed). */
	{
		JSContext *ctx = macsurf_qjs_current_ctx();
		int raf_own = -1;
		if (ctx != NULL) {
			static const char raf_own_src[] =
				"(function(){try{"
				"return (typeof requestAnimationFrame==='function'"
				"&&requestAnimationFrame===globalThis.__msRafOrig)"
				"?1:0;"
				"}catch(e){return -1;}})()";
			JSValue r = JS_Eval(ctx, raf_own_src, strlen(raf_own_src),
					"<jsraf>", JS_EVAL_TYPE_GLOBAL);
			if (!JS_IsException(r)) {
				int32_t n = 0;
				JS_ToInt32(ctx, &n, r);
				raf_own = (int)n;
			} else {
				JS_FreeValue(ctx, JS_GetException(ctx));
			}
			JS_FreeValue(ctx, r);
		}
		macsurf_debug_log_writef(
			"LIFE JSPUMP cap_hits=%ld raf=%ld rafOwn=%d",
			g_job_pump_cap_hits, g_raf_fires, raf_own);
		g_job_pump_cap_hits = 0;
		g_raf_fires = 0;
	}

	/* fixes1247 (#167) - total count from the __onBeforeModuleFactory
	 * require-trace (macsurf_qjs.c, register_browser_globals). Read
	 * SEPARATELY from the individual "LIFE js require: <name>" lines the
	 * hook itself emits (via __msLife, only for a small watched-module
	 * list) specifically to distinguish two very different failure
	 * shapes: total=0 means the page's own module loader never called
	 * OUR hook at all (either it does not use this require mechanism, or
	 * the hook install itself did not take -- worth knowing on its own);
	 * total>0 with none of the watched names ever appearing means
	 * requiring is happening normally but never reaches any module on
	 * this specific list. */
	{
		JSContext *ctx = macsurf_qjs_current_ctx();
		long req_total = -1;
		if (ctx != NULL) {
			static const char req_total_src[] =
				"(function(){try{"
				"return (typeof globalThis.__msRequireTraceTotal"
				"==='function')?globalThis.__msRequireTraceTotal():-1;"
				"}catch(e){return -1;}})()";
			JSValue r = JS_Eval(ctx, req_total_src, strlen(req_total_src),
					"<jsreq>", JS_EVAL_TYPE_GLOBAL);
			if (!JS_IsException(r)) {
				int32_t n = 0;
				JS_ToInt32(ctx, &n, r);
				req_total = (long)n;
			} else {
				JS_FreeValue(ctx, JS_GetException(ctx));
			}
			JS_FreeValue(ctx, r);
		}
		macsurf_debug_log_writef("LIFE JSREQUIRE total=%ld", req_total);
	}

	/* fixes1259 (#167) - Facebook loader observability. Six counters
	 * from the __d/requireLazy wrapper installed in macsurf_qjs.c's
	 * register_browser_globals. Read the same way as __msRequireTraceTotal
	 * above (a zero-arg JS function, called once per counter to keep each
	 * read isolated - if one throws or is missing, the others still
	 * report). See the decision table in the fixes1259 commit message:
	 * d_target=0 means ServerJSPayloadListener never gets __d()-defined;
	 * rl_target_calls>0 with rl_target_fires=0 means the lazy waiter
	 * registered but never released - Facebook's own dependency-release
	 * mechanism is the target from there. */
	{
		JSContext *ctx = macsurf_qjs_current_ctx();
		long d_total = -1, d_target = -1;
		long rl_calls = -1, rl_target_calls = -1;
		long rl_fires = -1, rl_target_fires = -1;
		if (ctx != NULL) {
			d_total = qjs_read_int_global(ctx, "__msFBLoader_dTotal");
			d_target = qjs_read_int_global(ctx, "__msFBLoader_dTarget");
			rl_calls = qjs_read_int_global(ctx, "__msFBLoader_rlCalls");
			rl_target_calls = qjs_read_int_global(ctx,
				"__msFBLoader_rlTargetCalls");
			rl_fires = qjs_read_int_global(ctx, "__msFBLoader_rlFires");
			rl_target_fires = qjs_read_int_global(ctx,
				"__msFBLoader_rlTargetFires");
		}
		macsurf_debug_log_writef(
			"LIFE FBLOADER d_total=%ld d_target=%ld rl_calls=%ld "
			"rl_target_calls=%ld rl_fires=%ld rl_target_fires=%ld",
			d_total, d_target, rl_calls, rl_target_calls,
			rl_fires, rl_target_fires);
	}

	/* fixes1272 (#167) - the waiter-release transition, and whether
	 * Facebook's registry even received the define.
	 *
	 * fixes1271 proved the target's whole 32-module closure is
	 * registered and rl_target_fires is still 0, and the FBRL trace
	 * showed the split precisely: requireLazy fires SYNCHRONOUSLY when
	 * the dependency is already defined (__debug, Env - both CALL and
	 * FIRE in the same millisecond) and never fires when the dependency
	 * arrives later (target: 157 calls at GSEQ 0, defined at GSEQ 227,
	 * zero fires). So the fast path works and the deferred path is dead.
	 *
	 * Read the fields as:
	 *   rl_real_null > 0        - STOP: our own wrapper dropped
	 *                              registrations while realRL was null,
	 *                              so rl_target_fires=0 is instrumentation,
	 *                              not a finding. Expected 0.
	 *   released_on_define=1    - defining the target DID release waiters;
	 *                              the failure is later than suspected.
	 *   released_on_define=0
	 *     with probe_sync_fire=1 - the registry HAS the module (a fresh
	 *                              requireLazy resolves it synchronously);
	 *                              only the deferred release step is
	 *                              broken. Fix belongs in whatever the
	 *                              real define does to walk pending
	 *                              waiters.
	 *   released_on_define=0
	 *     with probe_sync_fire=0 - the registry never received the define
	 *                              at all despite __d being called; the
	 *                              __d delegation path itself is the
	 *                              suspect.
	 *
	 * probe_sync_fire runs the target's factory once (see __msFBWait);
	 * bounded, guarded, and after all other reporting. */
	{
		JSContext *ctx = macsurf_qjs_current_ctx();
		if (ctx != NULL) {
			static const char fbwait_src[] =
				"(function(){try{"
				"if(typeof globalThis.__msFBWait!=='function')"
					"return '{\"error\":\"probe not "
						"installed\"}';"
				"return globalThis.__msFBWait();"
				"}catch(e){"
					"return JSON.stringify({error:"
						"((e&&e.message)||String(e))});"
				"}"
				"})()";
			JSValue r = JS_Eval(ctx, fbwait_src,
					strlen(fbwait_src), "<jsfbwait>",
					JS_EVAL_TYPE_GLOBAL);
			if (!JS_IsException(r)) {
				const char *s = JS_ToCString(ctx, r);
				macsurf_debug_log_writef("LIFE FBWAIT %s",
					(s != NULL) ? s : "(null)");
				if (s != NULL) JS_FreeCString(ctx, s);
			} else {
				JS_FreeValue(ctx, JS_GetException(ctx));
				macsurf_debug_log_writef(
					"LIFE FBWAIT eval exception");
			}
			JS_FreeValue(ctx, r);
		}
	}

	/* fixes1271 (#167) - which literal module name is the dead waiter
	 * actually attached to?
	 *
	 * fixes1270's hardware round returned FBGRAPH defined:false while
	 * FBLOADER on the SAME navigation reported rl_target_calls=166. Not
	 * a contradiction: rlTargetCalls tests the joined dependency string
	 * with indexOf() (substring), while the graph target was compared by
	 * string EQUALITY. So the waiters are attached to a name CONTAINING
	 * "ServerJSPayloadListener" that is not equal to it - and fixes1247's
	 * watchlist already knows one such variant, ServerJSPayloadListener_NEW.
	 *
	 * This line reports every variant observed on each side, with counts
	 * and first-sighting sequence numbers, so the four cases are readable
	 * directly rather than inferred:
	 *   same variant in defined[] and lazy[]  - graph it (FBGRAPH below
	 *                                            now picks it automatically)
	 *   variant in lazy[] but not defined[]   - real missing module, and
	 *                                            FBGRAPH's defined:false
	 *                                            now means it about the
	 *                                            name actually waited on
	 *   different variants on each side       - naming/alias/conditional
	 *                                            loader problem
	 *   several variants                      - counts and seq order say
	 *                                            which to chase first */
	{
		JSContext *ctx = macsurf_qjs_current_ctx();
		if (ctx != NULL) {
			static const char fbtargets_src[] =
				"(function(){try{"
				"if(typeof globalThis.__msFBLoader_targetLike!=="
					"'function')"
					"return '{\"error\":\"probe not "
						"installed\"}';"
				"return globalThis.__msFBLoader_targetLike();"
				"}catch(e){"
					"return JSON.stringify({error:"
						"((e&&e.message)||String(e))});"
				"}"
				"})()";
			JSValue r = JS_Eval(ctx, fbtargets_src,
					strlen(fbtargets_src), "<jsfbtargets>",
					JS_EVAL_TYPE_GLOBAL);
			if (!JS_IsException(r)) {
				const char *s = JS_ToCString(ctx, r);
				macsurf_debug_log_writef("LIFE FBTARGETS %s",
					(s != NULL) ? s : "(null)");
				if (s != NULL) JS_FreeCString(ctx, s);
			} else {
				JS_FreeValue(ctx, JS_GetException(ctx));
				macsurf_debug_log_writef(
					"LIFE FBTARGETS eval exception");
			}
			JS_FreeValue(ctx, r);
		}
	}

	/* fixes1270 (#167) - independent module-graph reconstruction, as a
	 * check against fixes1260's __debug probe rather than a replacement
	 * for it. debugUnresolvedDependencies() is Facebook's OWN loader
	 * self-reporting on whether ServerJSPayloadListener's deps are
	 * ready - but that loader's readiness bookkeeping is exactly what's
	 * suspected of being broken here, which makes asking it a slightly
	 * circular first diagnostic. This instead walks a dependency graph
	 * built ENTIRELY from __d() call data (macsurf_qjs.c's
	 * __msFBGraph_walk, installed alongside the existing __d/requireLazy
	 * wrapper), executing zero module factories and trusting zero
	 * Facebook bookkeeping.
	 *
	 * Decision table for the result:
	 *   defined=false                       - contradicts fixes1259's
	 *                                          d_target=1; instrumentation
	 *                                          itself would be suspect.
	 *   direct_missing>0                    - a dependency literally
	 *                                          named in the target's own
	 *                                          __d() call never got
	 *                                          defined; leaf names it.
	 *   transitive_missing>0                - same, but deeper in the
	 *                                          graph; leaf names the
	 *                                          first one found.
	 *   direct_missing=0 transitive_missing=0
	 *     with rl_target_fires=0 (FBLOADER, above) -
	 *          the ENTIRE observed closure was __d()-registered and the
	 *          lazy waiter STILL never released - not a missing module
	 *          at all, but Facebook's internal waiter/readiness
	 *          bookkeeping failing on this engine. That reframes where
	 *          fixes1271+ needs to look: the define/resolve transition
	 *          itself (waiter-count decrement, dependency Map/Set
	 *          membership, property-key equality), not fetch/exec or
	 *          module discovery, both already proven healthy.
	 *
	 * `special` carries any dependency token this code found but chose
	 * NOT to walk or classify - Facebook's loader uses forms other than
	 * plain module-name strings (seen in the real bundle: "cr:NNNNNNN"),
	 * and guessing at what those resolve to would be exactly the kind
	 * of unproven claim this probe exists to avoid making. Logged
	 * verbatim so a human can judge them instead. */
	{
		JSContext *ctx = macsurf_qjs_current_ctx();
		if (ctx != NULL) {
			static const char fbgraph_src[] =
				"(function(){try{"
				"if(typeof globalThis.__msFBGraph_walk!=="
					"'function')"
					"return '{\"error\":\"probe not "
						"installed\"}';"
				/* fixes1271 - no argument: the probe picks the
				 * variant actually observed this navigation (see
				 * __msFBGraph_pick). Passing the literal is what
				 * produced last round's uninformative
				 * defined:false. */
				"return globalThis.__msFBGraph_walk(null);"
				"}catch(e){"
					"return JSON.stringify({error:"
						"((e&&e.message)||String(e))});"
				"}"
				"})()";
			JSValue r = JS_Eval(ctx, fbgraph_src,
					strlen(fbgraph_src), "<jsfbgraph>",
					JS_EVAL_TYPE_GLOBAL);
			if (!JS_IsException(r)) {
				const char *s = JS_ToCString(ctx, r);
				macsurf_debug_log_writef("LIFE FBGRAPH %s",
					(s != NULL) ? s : "(null)");
				if (s != NULL) JS_FreeCString(ctx, s);
			} else {
				JS_FreeValue(ctx, JS_GetException(ctx));
				macsurf_debug_log_writef(
					"LIFE FBGRAPH eval exception");
			}
			JS_FreeValue(ctx, r);
		}
	}

	/* fixes1287 - normally emitted at window load; repeat at a true DONE
	 * edge when one exists so a later guarded failure is still visible. */
	macsurf_qjs_emit_fb_boot(macsurf_qjs_current_ctx());

	/* fixes1261 (#167) - CSS custom-property scoping: causal confirmation
	 * only, not more proof of the engine bug itself. A local harness
	 * test (--layout mode, two sibling divs each setting the SAME
	 * custom property name to a different value under a different
	 * class) already proved libcss's custom-property table is
	 * per-stylesheet/last-writer-wins, not per-selector-scoped: both
	 * divs came back with the LAST-declared value regardless of which
	 * class actually matched. What's still unconfirmed is whether THAT
	 * specific engine limitation is what makes Facebook's text
	 * invisible, or something else is. Facebook defines
	 * --fds-primary-text under both `.__fb-light-mode` (#1C1E21, a dark
	 * near-black colour meant for a light background) and
	 * `.__fb-dark-mode` (white) in the SAME inline <style> block - the
	 * exact shape the harness test reproduces. This reads the REAL
	 * computed (post-cascade, post-var()) color/backgroundColor via the
	 * same getComputedStyle path real rendering uses (fixes1011,
	 * qjs_get_computed_style -> css_computed_color /
	 * css_computed_background_color), not a synthetic re-derivation, so
	 * it reflects whatever the actual cascade decided for the real
	 * page. Near-black text against a dark background here, with
	 * __fb-dark-mode present in the class list, closes the case; if
	 * body_color already reads white/light, the invisible text is
	 * happening somewhere more specific than html/body and needs a
	 * follow-up, not a broader claim from this line alone. */
	{
		JSContext *ctx = macsurf_qjs_current_ctx();
		if (ctx != NULL) {
			static const char fbcss_src[] =
				"(function(){try{"
				"var h=document.documentElement;"
				"var b=document.body;"
				"var cls=(h&&h.getAttribute)?"
					"(h.getAttribute('class')||''):'';"
				"var hcs=(typeof getComputedStyle==='function'&&h)?"
					"getComputedStyle(h):null;"
				"var bcs=(typeof getComputedStyle==='function'&&b)?"
					"getComputedStyle(b):null;"
				"var hbg=hcs?String(hcs.backgroundColor||''):'';"
				"var bbg=bcs?String(bcs.backgroundColor||''):'';"
				"var bcol=bcs?String(bcs.color||''):'';"
				"return 'html_class=['+cls+'] html_bg='+hbg+"
					"' body_bg='+bbg+' body_color='+bcol;"
				"}catch(e){"
					"return 'FBCSS eval threw: '+"
						"((e&&e.message)||e);"
				"}"
				"})()";
			JSValue r = JS_Eval(ctx, fbcss_src, strlen(fbcss_src),
					"<jsfbcss>", JS_EVAL_TYPE_GLOBAL);
			if (!JS_IsException(r)) {
				const char *s = JS_ToCString(ctx, r);
				macsurf_debug_log_writef("LIFE FBCSS %s",
					(s != NULL) ? s : "(null)");
				if (s != NULL) JS_FreeCString(ctx, s);
			} else {
				JS_FreeValue(ctx, JS_GetException(ctx));
				macsurf_debug_log_writef(
					"LIFE FBCSS eval exception");
			}
			JS_FreeValue(ctx, r);
		}
	}

	/* fixes1239 (#167) - <script> tags the PARSER found (script.c,
	 * html_process_script), split inline/external, independent of
	 * whether js_exec ever ran one. Same local-extern pattern as
	 * html_reconvert_phase_report above -- script.c is content/html/,
	 * not this TU's own subsystem. */
	{
		extern void html_script_tag_census_report(void);
		html_script_tag_census_report();
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
	 * - every hardware log we pull covers two or more loads, and carrying
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
/* fixes1016 - refill every audit budget at each new JS realm (= each
 * navigation), so page 2 and the iframes are audited too; the fixes1015
 * per-session budgets died mid-page-one and 68kmla was never covered. */
void macsurf_qjs_audit_reset(void)
{
	/* fixes1022  -  audit OFF by default (every line is a flushed write);
	 * define MACSURF_JS_AUDIT to get the fixes1020 skeleton back.
	 *
	 * fixes1029  -  EXCEPT the removal audit, which is on for this round.
	 * The device's pagemap shows every DIV.entry-intro on hackaday with
	 * kids=0 - the article titles, bylines and excerpts are GONE from the
	 * DOM - while the same markup parsed in the harness has them, and the
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
	/* fixes1092/fixes1136  -  geometry audit OFF.  MACSURF_JS_GEOMETRY is
	 * disabled (Option B) pending incremental layout; geometry reads return
	 * undefined and the sync-flush infrastructure is dormant.  The audit
	 * channel is harmless (it would emit zeros) but there is nothing to
	 * audit.  Restore to 40+ when geometry is re-enabled. */
	g_geom_audit = 0;
#endif
	/* fixes1039  -  the WANT channel stays ON, independent of the audit.
	 *
	 * The engine now runs hackaday's whole stack - front page, article
	 * page and the Jetpack comment iframe - with ZERO exceptions. So the
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
	 * so a page that wants nothing pays nothing - a handful of lines
	 * rather than the hundreds the full audit emits. This is the
	 * implement-next list, written by real sites instead of by me. */
	g_mslife_audit = 60;
	/* fixes1246 (#167) - console.error/console.warn just went LIFE-visible
	 * (previously "[js:error]"/"[js:warn]", invisible in release). Same
	 * always-on, independent-of-MACSURF_JS_AUDIT posture as g_mslife_audit
	 * above: this is a page's OWN primary error-reporting channel, not a
	 * debug-only audit feature, and React specifically uses it to report
	 * recoverable render/hydration failures instead of an uncaught throw. */
	g_console_err_audit = 200;
	/* fixes1030  -  the three ways script can DELETE page content:
	 * removeChild, textContent= and innerHTML=. All three now log their
	 * target's identity on this shared budget, independent of
	 * MACSURF_JS_AUDIT, because hackaday's article entries reach the box
	 * tree ALREADY EMPTY (kids=0 at the `ready` dump) while the same
	 * markup parsed without script keeps all four children - and the
	 * removal audit cleared removeChild entirely (120 lines, every one a
	 * Typekit font probe or a jQuery feature-detect element). The census
	 * in the same window reports text=35, and the river has 7 entries. */
	/* fixes1032  -  OFF with the rest of the audit. These three named the
	 * deleter (fixes1029-1031); they cost a flushed write per mutation and
	 * are not wanted in a baseline. */
#ifdef MACSURF_JS_AUDIT
	g_rm_audit_budget = 260;
#else
	g_rm_audit_budget = 0;
#endif
	g_pn_logged = 0; /* fixes1110  -  per-navigation, not per-process */
	qjs_want_reset(); /* R1.2  -  per-page WANT dedupe set */
}

/* ---- Page JS summary ---- */
/* fixes1013  -  COUNT WHAT ACTUALLY RAN.
 *
 * "Zero JS exceptions" was being read as "JavaScript works", and it is not
 * evidence of that at all: it is equally consistent with no script executing.
 * The failure paths were LIFE-prefixed in fixes1000 but every SUCCESS
 * breadcrumb stayed WORK-prefixed, and WORK is compiled out of shipping
 * builds - so a hardware log could show errors and could NOT show whether a
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
			/* fixes1236 (#167) - a count alone can't distinguish "one
			 * bootstrap listener that never fires" from "one listener
			 * that IS the whole app" -- name the event types too, on the
			 * __msLife budget, only when there's something to name (a
			 * page with zero listeners costs nothing extra). */
			static const char *cnt_src =
				"(function(){var d=0,w=0;try{"
				"var L=document._listeners||{},dk=[];for(var k in L){"
				"var n=(L[k]&&L[k].length)||0;d+=n;"
				"if(n)dk.push(k+':'+n);}"
				"var W=this._winListeners||{},wk=[];for(var j in W){"
				"var m=(W[j]&&W[j].length)||0;w+=m;"
				"if(m)wk.push(j+':'+m);}"
				"if((dk.length||wk.length)&&typeof __msLife==='function')"
				"__msLife('LISTENERS doc=['+dk.join(',')+"
				"'] win=['+wk.join(',')+']');"
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
		"LIFE JS PAGE: scripts=%ld bytes=%ld failed=%ld skipped=%ld timed_out=%ld "
		"jquery=%d xf=%d doclisten=%d winlisten=%d nodelisten=%d",
		g_js_exec_count, g_js_exec_bytes, g_js_exec_fail,
		g_js_skip_count, g_js_timeout_count,
		has_jq, has_xf, doc_l, win_l, node_l);

	/* R1.3  -  the per-script census: one LIFE SCRIPT CENSUS line per
	 * execution, in record order, then clear so the next emit starts
	 * fresh.  Cleared HERE rather than in macsurf_qjs_audit_reset()  - 
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
