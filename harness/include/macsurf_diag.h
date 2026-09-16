/* Harness-only surface used by the real QuickJS glue. */
#ifndef HARNESS_MACSURF_DIAG_H
#define HARNESS_MACSURF_DIAG_H
enum ms_script_kind { MS_SCRIPT_CLASSIC = 0, MS_SCRIPT_MODULE };
enum ms_script_state { MS_SCR_RUNNING = 0, MS_SCR_DONE, MS_SCR_COMPILE_FAIL,
	MS_SCR_RUN_FAIL, MS_SCR_SKIPPED };
enum ms_task_kind { MS_TASK_NONE = 0, MS_TASK_SCRIPT, MS_TASK_EVENT,
	MS_TASK_TIMER, MS_TASK_MICROTASK, MS_TASK_INTERNAL_SETUP,
	MS_TASK_INTERNAL_NOTIFICATION, MS_TASK_XHR };
struct ms_diag_scope {
	unsigned long prev_script;
	unsigned long prev_task;
	unsigned long my_id;
};
struct ms_diag_provenance {
	unsigned long nav, frame, doc, script, task, batch, pass;
};
enum ms_render_kind { MS_RENDER_INITIAL = 0, MS_RENDER_RECONVERT,
	MS_RENDER_FAST_STYLE, MS_RENDER_FAST_INHERITED, MS_RENDER_POLICY };
enum ms_stage_reason { MS_SREASON_NONE = 0, MS_SREASON_CLASSIFIER_OTHER,
	MS_SREASON_BORDER_WIDTH_BITS_DIFFER, MS_SREASON_STRUCTURAL_IN_BATCH,
	MS_SREASON_NOT_READY, MS_SREASON_NO_CANDIDATE };
enum ms_render_result { MS_RRES_RUNNING = 0, MS_RRES_DONE, MS_RRES_FALLBACK,
	MS_RRES_FAIL, MS_RRES_QUEUED, MS_RRES_DECLINED,
	MS_RRES_COSMETIC_SUPPRESSED, MS_RRES_DEFER_NOT_DONE,
	MS_RRES_DEFER_JS_ACTIVE, MS_RRES_BUSY, MS_RRES_STALE_DROP,
	MS_RRES_OVERFLOW };
enum ms_render_action { MS_RACTION_NONE = 0, MS_RACTION_PAINT,
	MS_RACTION_RECASCADE, MS_RACTION_LOCAL_REFLOW, MS_RACTION_SUBTREE,
	MS_RACTION_FULL, MS_RACTION_SYNC_FULL };
enum ms_stage_kind { MS_STAGE_STYLEFAST = 0, MS_STAGE_INHERITED_COLOR };
enum ms_stage_result { MS_SRES_COMMIT = 0, MS_SRES_FALLBACK, MS_SRES_DECLINE };
struct ms_diag_render_scope {
	unsigned long prev_nav, prev_frame, prev_doc, prev_script, prev_task,
		prev_batch, prev_pass;
	unsigned long my_pass;
};
void ms_diag_script_enter(struct ms_diag_scope *, unsigned long, int,
	const char *);
void ms_diag_script_leave(struct ms_diag_scope *, int);
void ms_diag_task_enter_external(struct ms_diag_scope *, unsigned long, int,
	unsigned long, unsigned long, unsigned long, const char *);
void ms_diag_task_set_script(unsigned long, unsigned long);
void ms_diag_task_leave(struct ms_diag_scope *);
unsigned long ms_diag_cur_script(void);
unsigned long ms_diag_cur_task(void);
unsigned long ms_diag_cur_nav(void);
unsigned long ms_diag_frame_get(const void *);
unsigned long ms_diag_batch_open(const struct ms_diag_provenance *);
void ms_diag_batch_add(unsigned long, int, unsigned long);
void ms_diag_batch_freeze(unsigned long);
int ms_diag_batch_provenance(unsigned long, struct ms_diag_provenance *);
unsigned long ms_diag_render_enter(struct ms_diag_render_scope *, int,
	const struct ms_diag_provenance *);
void ms_diag_render_leave(struct ms_diag_render_scope *, int, int);
void ms_diag_render_action(int);
void ms_diag_render_stage(int, int, int, int, unsigned long, unsigned long,
	const char *, int);

enum ms_operation_reason {
	MS_OPR_NONE = 0, MS_OPR_PRE_ABORTED, MS_OPR_BAD_URL, MS_OPR_NO_BASE,
	MS_OPR_ARENA_FULL, MS_OPR_BODY_ALLOC, MS_OPR_HEADER_LIMIT,
	MS_OPR_FETCH_START_FAIL, MS_OPR_NETWORK_ERROR, MS_OPR_RESPONSE_POISONED,
	MS_OPR_REDIRECT_LIMIT, MS_OPR_REDIRECT_DOWNGRADE, MS_OPR_ABORTED,
	MS_OPR_REALM_GONE, MS_OPR_TIMEOUT, MS_OPR_AUTH, MS_OPR_CERT,
	MS_OPR_SSL_ERROR, MS_OPR_NOT_MODIFIED
};
enum ms_error_kind {
	MS_ERR_JS_EXCEPTION = 0, MS_ERR_API_DECLINE, MS_ERR_PROMISE_REJECTION,
	MS_ERR_CALLBACK_FAILURE
};
void ms_diag_error_record(unsigned long op_id, unsigned long request_id,
	int kind, int boundary, int reason, const char *name, const char *message);

#endif
