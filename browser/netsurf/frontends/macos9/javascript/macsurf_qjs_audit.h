/*
 * MacSurf — macsurf_qjs_audit.h
 *
 * Performance counters and diagnostic emitters for the QuickJS engine.
 * Extracted from macsurf_qjs.c (2026-08-05 cleanup Phase 2).
 *
 * The counters are DEFINED in macsurf_qjs.c (file-scope, non-static) and
 * declared extern here so the audit TU can read them.  Engine code in
 * macsurf_qjs.c writes the counters directly; the functions in
 * macsurf_qjs_audit.c only READ and report them.
 */

#ifndef MACSURF_QJS_AUDIT_H
#define MACSURF_QJS_AUDIT_H

/* ---- Counters written by macsurf_qjs.c engine code ---- */

/* Timer subsystem */
extern long   g_timer_fires;
extern long   g_timer_us;

/* Interrupt/op counter */
extern long g_qjs_interrupts;

/* Heap pointer accessor (struct jsheap is private to macsurf_qjs.c) */
struct JSContext *macsurf_qjs_current_ctx(void);

/* Per-script performance slots */
#define QJS_PERF_SLOTS 8
#define QJS_PERF_NAME  32

struct qjs_perf_slot {
	char name[QJS_PERF_NAME];
	long bytes;
	long compile_us;
	long run_us;
	long evals;
};
extern struct qjs_perf_slot g_perf_slot[QJS_PERF_SLOTS];
extern long g_perf_evals;
extern long g_perf_bytes;
extern long g_perf_compile_us;
extern long g_perf_run_us;
extern long g_perf_gc_us;
extern long g_perf_gc_runs;
extern int  g_perf_gc_armed;
extern int  g_perf_gc_armed;

/* Wrapper-helper census */
extern long g_wrap_installs;
extern long g_helper_compiles;
extern long g_helper_bytes;
extern long g_wrap_us;

/* Geometry read census */
extern long g_geom_reads;
extern long g_geom_us;
extern long g_geom_at_ready;
extern long g_geom_at_done;
extern long g_geom_unstable;
extern long g_geom_undef;
extern long g_geom_zero;
extern long g_geom_real;

/* Misc counters (non-static in macsurf_qjs.c) */
extern long macsurf_qjs_ncalls;
extern long macsurf_qjs_native_us;
extern long macsurf_qjs_native_samp;

/* Audit budgets (written by macsurf_qjs.c and macsurf_qjs_audit_reset) */
extern long g_mut_audit_budget;
extern long g_rm_audit_budget;
extern long g_evreg_audit;
extern long g_evmiss_audit;
extern long g_evfire_audit;
extern long g_mslife_audit;
extern long g_geom_audit;
extern int  g_pn_logged;

/* JS execution census */
extern long g_js_exec_count;
extern long g_js_exec_bytes;
extern long g_js_exec_fail;

/* Listener registration count (for page_js_summary) */
extern int s_reg_n_registered;

/* ---- Audit functions (implemented in macsurf_qjs_audit.c) ---- */

void macsurf_qjs_emit_timer_profile(void);
void macsurf_qjs_geom_stats(long *reads, long *us);
void macsurf_qjs_gc_note(long us);
void macsurf_qjs_wrap_stats(long *wraps, long *hcompiles, long *hbytes);
void macsurf_qjs_perf_totals(long *evals, long *compile_us, long *run_us,
		long *gc_us, long *gc_runs, int *gc_armed);
void macsurf_qjs_emit_js_profile(void);
void macsurf_qjs_audit_reset(void);
void macsurf_qjs_page_js_summary(void);

#endif /* MACSURF_QJS_AUDIT_H */
