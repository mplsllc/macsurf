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

/* Starts a new readiness epoch at an accepted top-level navigation.  Older
 * session contracts remain available to `pending`, but cannot make this new
 * run look stalled before it has had a chance to progress. */
void macsurf_diag_navigation_begin(void);

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
long macsurf_diag_serialize_prefs(char *buf, long cap);
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

/* ================= Browser API operation / error diagnostics =================
 * These rings describe work attempted before a struct fetch reaches the wire
 * request ring.  `op_id` is stable across a fetch/XHR setup and its eventual
 * native request; request_id remains zero when the operation was declined
 * before fetch_start(). */
enum ms_operation_kind {
	MS_OP_FETCH = 0, MS_OP_XHR, MS_OP_BEACON
};
enum ms_operation_phase {
	MS_OP_ATTEMPT = 0, MS_OP_OPEN, MS_OP_SEND_ATTEMPT, MS_OP_NATIVE_ALLOC,
	MS_OP_WIRE_START, MS_OP_ABORT, MS_OP_DELIVER, MS_OP_SETTLE,
	MS_OP_NATIVE_EVENT
};
enum ms_operation_result {
	MS_OP_PENDING = 0, MS_OP_OK, MS_OP_DECLINE, MS_OP_RESOLVE,
	MS_OP_REJECT, MS_OP_IGNORED
};
enum ms_operation_reason {
	MS_OPR_NONE = 0, MS_OPR_PRE_ABORTED, MS_OPR_BAD_URL, MS_OPR_NO_BASE,
	MS_OPR_ARENA_FULL, MS_OPR_BODY_ALLOC, MS_OPR_HEADER_LIMIT,
	MS_OPR_FETCH_START_FAIL, MS_OPR_NETWORK_ERROR, MS_OPR_RESPONSE_POISONED,
	MS_OPR_REDIRECT_LIMIT, MS_OPR_REDIRECT_DOWNGRADE, MS_OPR_ABORTED,
	MS_OPR_REALM_GONE, MS_OPR_TIMEOUT, MS_OPR_AUTH, MS_OPR_CERT,
	MS_OPR_SSL_ERROR, MS_OPR_NOT_MODIFIED
};
enum ms_answer_quality {
	MS_ANSWER_NATIVE = 0, MS_ANSWER_APPROX, MS_ANSWER_FALLBACK,
	MS_ANSWER_UNSUPPORTED
};
enum ms_error_kind {
	MS_ERR_JS_EXCEPTION = 0, MS_ERR_API_DECLINE, MS_ERR_PROMISE_REJECTION,
	MS_ERR_CALLBACK_FAILURE
};

unsigned long ms_diag_operation_begin(int kind, int quality);
void ms_diag_operation_record(unsigned long op_id, int kind, int phase,
	int result, int reason, int quality, unsigned long request_id);
void ms_diag_error_record(unsigned long op_id, unsigned long request_id,
	int kind, int boundary, int reason, const char *name, const char *message);
long macsurf_diag_serialize_operations(char *buf, long cap);
long macsurf_diag_serialize_errors(char *buf, long cap);

/* ===================== Phase 2: expected-transition contracts =====================
 * A contract is MacSurf-owned durable state, not an inferred page outcome.
 * Writers open a bounded entry when MacSurf has actually accepted responsibility
 * for a next transition, then mark an explicit terminal state.  The AppleEvent
 * serializers are read-only, so repeated `pending`/`settlement` reads are stable.
 */
enum ms_contract_kind {
	MS_CONTRACT_OPERATION = 0, MS_CONTRACT_IO, MS_CONTRACT_MODULE_WAIT
};
enum ms_contract_state {
	MS_CONTRACT_WAITING = 0, MS_CONTRACT_COMPLETED, MS_CONTRACT_FIRED,
	MS_CONTRACT_SATISFIED, MS_CONTRACT_FAILED, MS_CONTRACT_CANCELLED,
	MS_CONTRACT_DECLINED, MS_CONTRACT_EXPIRED
};
enum ms_contract_expected {
	MS_EXPECT_NONE = 0, MS_EXPECT_WIRE_START, MS_EXPECT_SETTLE,
	MS_EXPECT_CHECK, MS_EXPECT_CALLBACK, MS_EXPECT_RELEASE,
	MS_EXPECT_CALLBACK_RETURN
};

enum ms_timer_diag_state {
	MS_TIMER_ARMED = 0, MS_TIMER_DUE, MS_TIMER_FIRING, MS_TIMER_FIRED,
	MS_TIMER_CANCELLED, MS_TIMER_EVICTED, MS_TIMER_REALM_TEARDOWN,
	MS_TIMER_OWNER_MISMATCH, MS_TIMER_ABANDONED
};

/* A queued IO expectation records the gap between the shim's observable
 * scheduling request and the scheduler's authoritative timer allocation. */
enum ms_io_timer_native_state {
	MS_IO_TIMER_NATIVE_NOT_ENTERED = 0, MS_IO_TIMER_NATIVE_ENTERED,
	MS_IO_TIMER_NATIVE_BAD_CALLBACK, MS_IO_TIMER_NATIVE_NO_SLOT,
	MS_IO_TIMER_NATIVE_ALLOCATED
};

long macsurf_diag_serialize_pending(char *buf, long cap);
long macsurf_diag_serialize_settlement(char *buf, long cap);
long macsurf_diag_serialize_timers(char *buf, long cap);
/* `readiness` is intentionally polling-oriented: it reports whether a
 * hardware run is still making known browser progress, is quiescing, has
 * settled, or has stopped with known work unresolved. */
long macsurf_diag_serialize_readiness(char *buf, long cap);
void ms_diag_timer_arm(unsigned long timer_id, unsigned long nav_id,
	unsigned long script_id, unsigned long task_id, unsigned long ctx_gen);
void ms_diag_timer_state(unsigned long timer_id, int state);
void ms_diag_io_timer_expect(unsigned long io_id, const char *target_name);
void ms_diag_io_timer_native_state(unsigned long io_id,
	const char *target_name, int state);
void ms_diag_io_timer_bind(unsigned long io_id, const char *target_name,
	unsigned long timer_id);


/* ==================== Milestone 1c: Causal Render Trace ====================
 * document_id   = one DOM-document lifetime (allocated at html_begin_conversion;
 *                 an encoding/parser restart that replaces c->document -> new id).
 * frame_id      = one browsing-context lifetime (allocated at browser_window
 *                 creation; unchanged across that frame's navigations).
 * mutation_batch_id = one per-content debounced pending-table batch.
 * layout_pass_id    = one render/update attempt (STYLEFAST -> INHERITEDCOLOR ->
 *                 full reconvert are STAGES of it, not separate passes).
 * paint_pass_id = one invalidation->paint transaction requested by a render pass.
 *
 * R3: before any fast path the transaction holds ONE immutable descriptor; every
 * stage record copies it via ms_diag_cur_provenance(), never piecemeal cur_*(). */

enum ms_render_kind { MS_RENDER_INITIAL = 0, MS_RENDER_RECONVERT,
		      MS_RENDER_FAST_STYLE, MS_RENDER_FAST_INHERITED };
enum ms_stage_kind   { MS_STAGE_STYLEFAST = 0, MS_STAGE_INHERITED_COLOR };
enum ms_stage_result { MS_SRES_COMMIT = 0, MS_SRES_FALLBACK, MS_SRES_DECLINE };
enum ms_stage_reason { MS_SREASON_NONE = 0,
		       MS_SREASON_CLASSIFIER_OTHER,
		       MS_SREASON_BORDER_WIDTH_BITS_DIFFER,
		       MS_SREASON_STRUCTURAL_IN_BATCH,
		       MS_SREASON_NOT_READY,
		       MS_SREASON_NO_CANDIDATE };
enum ms_render_result { MS_RRES_RUNNING = 0, MS_RRES_DONE, MS_RRES_FALLBACK,
			MS_RRES_FAIL, MS_RRES_QUEUED };

/* The frozen causal descriptor. Fill with ms_diag_cur_provenance() or build it
 * from a pending-table slot; pass by const pointer, never re-read cur_*(). */
struct ms_diag_provenance {
	unsigned long nav;
	unsigned long frame;
	unsigned long doc;
	unsigned long script;
	unsigned long task;
	unsigned long batch;
	unsigned long pass;
};

/* saved ambient render scope; scoped push/pop, NOT bare assignment */
struct ms_diag_render_scope {
	unsigned long prev_nav, prev_frame, prev_doc, prev_batch, prev_pass;
	unsigned long my_pass;
};

/* --- documents --- */
unsigned long ms_diag_document_open(unsigned long nav, unsigned long frame);
void ms_diag_document_set_frame(unsigned long doc_id, unsigned long frame);
void ms_diag_document_close(unsigned long doc_id);

/* --- mutation batches (R1: keyed by document, opened per pending slot) --- */
unsigned long ms_diag_batch_open(const struct ms_diag_provenance *prov);
void ms_diag_batch_add(unsigned long batch_id, int mut_kind, unsigned long task);
void ms_diag_batch_freeze(unsigned long batch_id);

/* --- render transaction: synchronous reconvert path (open+push / close+pop) --- */
unsigned long ms_diag_render_enter(struct ms_diag_render_scope *s, int kind,
	const struct ms_diag_provenance *prov);
void ms_diag_render_leave(struct ms_diag_render_scope *s, int result, int reason);

/* --- render transaction: async initial layout (open once; push/pop per slice;
 *     close once). prov->pass is filled in by _open. --- */
unsigned long ms_diag_render_open(struct ms_diag_provenance *prov, int kind);
void ms_diag_render_slice_push(struct ms_diag_render_scope *s,
	const struct ms_diag_provenance *prov);
void ms_diag_render_slice_pop(struct ms_diag_render_scope *s);
void ms_diag_render_close(unsigned long pass_id, int result, int reason);

/* --- one structured stage record from inside a fast path (copies cur prov) ---
 * `detail` is a stage-specific sub-code (libcss color_diff_detail for the
 * inherited-colour classifier); 0 when not applicable. */
void ms_diag_render_stage(int stage_kind, int result, int reason, int detail,
	unsigned long bits_old, unsigned long bits_new,
	const char *tag, int candidate_count);

/* --- paint: lazily bind a paint id to the live pass; bump invalidations on
 *     repeat. No-op when no render pass is on the stack (caret blink etc.). --- */
void ms_diag_paint_note(void);

/* --- currently-executing causal scope --- */
void ms_diag_cur_provenance(struct ms_diag_provenance *out);
unsigned long ms_diag_cur_doc(void);
unsigned long ms_diag_cur_frame(void);
unsigned long ms_diag_cur_batch(void);
unsigned long ms_diag_cur_pass(void);
unsigned long ms_diag_cur_paint(void);

/* Pointer-keyed frame identity.  This deliberately lives outside
 * struct browser_window because the core owns contiguous arrays of browser
 * windows for frames and iframes; trace state must not change that ABI. */
void ms_diag_frame_open(void *bw);
unsigned long ms_diag_frame_get(const void *bw);
void ms_diag_frame_close(void *bw);

long macsurf_diag_serialize_documents(char *buf, long cap);
long macsurf_diag_serialize_mutations(char *buf, long cap);
long macsurf_diag_serialize_layout(char *buf, long cap);

/* ===================== Module Trace v1 ===================== */
enum ms_mod_event_type {
	MS_MOD_DEFINE = 0,
	MS_MOD_REQUEST,
	MS_MOD_RESOLVE,
	MS_MOD_EXECUTE,
	MS_MOD_FAIL,
	MS_MOD_DECLARE_DEP,
	MS_MOD_WAIT_REGISTERED,
	MS_MOD_CALLBACK_BEGIN,
	MS_MOD_CALLBACK_RETURN
};

enum ms_mod_reason {
	MS_MOD_REASON_NONE = 0,
	MS_MOD_REASON_MISSING,
	MS_MOD_REASON_DEP_MISSING,
	MS_MOD_REASON_FACTORY_THROW,
	MS_MOD_REASON_CYCLE
};

unsigned long ms_diag_module_id(const char *name);
void ms_diag_module_record(unsigned long mod_id, unsigned long dep_mod_id,
	int event_type, int reason, int depth, unsigned long wait_id);
void ms_diag_module_record_by_name(const char *name, const char *dep_name,
	int event_type, int reason, int depth, unsigned long wait_id);
long macsurf_diag_serialize_modules(char *buf, long cap);

/* ================== IntersectionObserver Trace v1 ================== */
enum ms_io_event_type {
	MS_IO_CONSTRUCT = 0,
	MS_IO_OBSERVE,
	MS_IO_QUERY,
	MS_IO_CHECK,
	MS_IO_CALLBACK,
	MS_IO_SKIP,
	MS_IO_UNOBSERVE,
	MS_IO_DISCONNECT
};

void ms_diag_io_record(unsigned long io_id, int ev_type, const char *target_name,
	long x, long y, long w, long h, int intersecting, int ratio_pct, int entries);
long macsurf_diag_serialize_io(char *buf, long cap);

#endif /* MACSURF_DIAG_H */
