/*
 * MacSurf  -  macsurf_diag.h
 *
 * MacSurf Trace: the one MacSurf-owned diagnostic state boundary that the
 * `MSdg`/`GET ` AppleEvent class serialises on demand.
 *
 * Model:
 *   hot path        -> integer counters only (macsurf_diag_request_seen,
 *                      macsurf_gap_hit)
 *   NAV: DONE        -> freeze in-progress counters into a last-nav snapshot
 *                      (macsurf_diag_nav_done, macsurf_gap_emit_summary)
 *   AppleEvent query -> serialise the FROZEN snapshot into text, later, with
 *                      no traversal of hlcache / the fetch ring / windows and
 *                      no formatting on any request hot path
 *
 * Reads are non-destructive: repeated `MSdg GET summary` return the same block
 * until the next navigation completes.
 */

#ifndef MACSURF_DIAG_H
#define MACSURF_DIAG_H

/* Wire-request lifecycle state (small ints; netsummary maps to text). */
enum ms_req_state {
	MS_REQ_UNKNOWN = 0,
	MS_REQ_DONE,
	MS_REQ_FAIL,
	MS_REQ_REDIRECT,
	MS_REQ_NOTMODIFIED
};
enum ms_req_scheme { MS_SCHEME_OTHER = 0, MS_SCHEME_HTTPS, MS_SCHEME_HTTP };

/* Record one wire request at its terminal state. Called once per struct fetch
 * by the macos9 fetchers. Appends to a bounded session ring keyed by the
 * request's IMMUTABLE ids (read from the fetch via fetch_get_*), and bumps the
 * per-nav summary tally. No URL is copied -- host/path stay in the existing
 * RECON MIME / gzip / FETCHCONC log lines that already carry nav=/req=, and
 * netsummary.py joins on req. `f` is a `struct fetch *` (opaque here). */
void macsurf_diag_request_record(void *f, int state, int status,
	int scheme, unsigned long bytes_in, unsigned long bytes_out);

/* Called at NAV: DONE, AFTER macsurf_gap_emit_summary(). Freezes the
 * in-progress request tally into the last-nav snapshot and clears it. */
void macsurf_diag_nav_done(unsigned long nav_id);

/* On-demand serialisers for `MSdg GET summary` / `MSdg GET gaps`. Write a
 * versioned machine-oriented block (NUL-terminated) into buf and return its
 * length excluding the NUL, or 0 on bad args. Never mutate state.
 *
 *   MSDIAG 1 summary            MSDIAG 1 gaps
 *   nav=<n>                     nav=<n>
 *   requests=<n>               <slug>=<count>        (only non-zero, high first)
 *   failures=<n>               ...
 *   gaps_unique=<n>
 *   gaps_total=<n>
 */
long macsurf_diag_serialize_summary(char *buf, long cap);
long macsurf_diag_serialize_gaps(char *buf, long cap);

/* `MSdg GET network`: the bounded session request ring, NOT a last-nav
 * snapshot -- late requests cross navigation boundaries, so every ring entry
 * carries its own immutable nav_id and the serialiser just lists them. Header
 * nav= is the last completed navigation for orientation only.
 *
 *   MSDIAG 1 network
 *   nav=<last>
 *   req=21 nav=2 state=done status=200 redirect_from=0 in=812 out=0 scheme=1
 *   req=20 nav=1 state=fail status=0  redirect_from=0 in=0   out=0 scheme=1
 */
long macsurf_diag_serialize_network(char *buf, long cap);


/* ============================ Phase 1b ==============================
 * script_id  = one source EXECUTION attempt (not a source-file id).
 * task_id    = one event-loop ENTRY into JS as a new causal turn.
 * Synchronous nested work inherits the current task; ambiguous script
 * ownership is script=0, never guessed.
 */

enum ms_script_kind  { MS_SCRIPT_CLASSIC = 0, MS_SCRIPT_MODULE };
enum ms_script_state { MS_SCR_RUNNING = 0, MS_SCR_DONE, MS_SCR_COMPILE_FAIL,
		       MS_SCR_RUN_FAIL, MS_SCR_SKIPPED };
enum ms_task_kind    { MS_TASK_NONE = 0, MS_TASK_TIMER, MS_TASK_EVENT,
		       MS_TASK_XHR, MS_TASK_MICROTASK };

/* Saved outer scope; scoped push/pop, NOT bare assignment. */
struct ms_diag_scope {
	unsigned long prev_script;
	unsigned long prev_task;
	unsigned long my_id;		/* this scope's script or task id, 0 if inherited */
};

/* --- script scope: wrap the top-level eval in js_exec / js_exec_module --- */
void ms_diag_script_enter(struct ms_diag_scope *s, unsigned long nav_id,
	int kind, const char *name);
void ms_diag_script_leave(struct ms_diag_scope *s, int state);

/* --- task scope: wrap the JS_Call at a timer/event/xhr/microtask boundary ---
 * Returns the allocated task id, or 0 when it INHERITED the current task
 * (kind==MS_TASK_EVENT while a task is already live) -- caller still must call
 * ms_diag_task_leave with the same scope. `extra` is req_id for XHR, job count
 * for microtask, 0 otherwise; `name` is the event type for EVENT, else NULL. */
unsigned long ms_diag_task_enter(struct ms_diag_scope *s, int kind,
	unsigned long nav_id, unsigned long origin_script,
	unsigned long extra, const char *name);
void ms_diag_task_leave(struct ms_diag_scope *s);

/* microtask only: after the drain, record how many jobs ran and whether the
 * per-pump cap was hit (remainder deferred to the next task). */
void ms_diag_task_set_jobs(struct ms_diag_scope *s, unsigned long jobs,
	int capped);

/* currently-executing causal scope (for later DOM/CSS/layout instrumentation) */
unsigned long ms_diag_cur_script(void);
unsigned long ms_diag_cur_task(void);
unsigned long ms_diag_cur_nav(void);

long macsurf_diag_serialize_scripts(char *buf, long cap);
long macsurf_diag_serialize_tasks(char *buf, long cap);

#endif /* MACSURF_DIAG_H */
