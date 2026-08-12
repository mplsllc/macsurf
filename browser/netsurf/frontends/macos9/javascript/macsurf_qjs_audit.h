/*
 * MacSurf  -  macsurf_qjs_audit.h
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
extern long g_js_skip_count;     /* fixes1141  -  scripts skipped (size cap) */
extern long g_js_timeout_count;  /* fixes1141  -  scripts aborted (deadline) */

/* R1.3  -  per-script census, the `LIFE SCRIPT CENSUS` lines.
 *
 * One entry per script execution, written by qjs_census_note() in
 * macsurf_qjs.c at the point the outcome is known (js_exec's compile/run
 * split, js_exec_module's single eval).  macsurf_qjs_page_js_summary()
 * emits one LIFE SCRIPT CENSUS line per entry right after the JS PAGE
 * line, then clears the array  -  the clear happens THERE and not in
 * macsurf_qjs_audit_reset(), because js_newheap (which calls audit_reset)
 * runs per browser_window AND per (i)frame, and an iframe created
 * mid-parse would wipe the main document's entries before the page
 * summary ever emitted them. */
#define SCRIPT_CENSUS_MAX    128
#define SCRIPT_CENSUS_NAME   64
#define SCRIPT_CENSUS_INLINE   0
#define SCRIPT_CENSUS_EXTERNAL 1
#define SCRIPT_CENSUS_MODULE   2

struct script_census_entry {
	char name[SCRIPT_CENSUS_NAME];  /* qjs_short_name()ed at record time */
	long size;                      /* source bytes */
	long compile_us;                /* parse + codegen */
	long run_us;                    /* bytecode execution */
	unsigned char type;             /* SCRIPT_CENSUS_* above */
	unsigned char defer_async;      /* bit0=defer, bit1=async; 0 = "-" */
	unsigned char compiled;         /* 0=fail, 1=ok */
	unsigned char completed;        /* 0=fail, 1=ok */
};
extern struct script_census_entry g_script_census[SCRIPT_CENSUS_MAX];
extern long g_script_census_count;  /* entries recorded this page */
extern long g_script_census_full;   /* executions dropped when array filled */

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
