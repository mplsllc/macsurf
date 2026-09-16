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
struct ms_diag_render_scope {
	unsigned long prev_nav, prev_frame, prev_doc, prev_batch, prev_pass;
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
#endif
