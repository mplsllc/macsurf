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
void ms_diag_script_enter(struct ms_diag_scope *, unsigned long, int,
	const char *);
void ms_diag_script_leave(struct ms_diag_scope *, int);
void ms_diag_task_enter_external(struct ms_diag_scope *, unsigned long, int,
	unsigned long, unsigned long, unsigned long, const char *);
void ms_diag_task_set_script(unsigned long, unsigned long);
void ms_diag_task_leave(struct ms_diag_scope *);
unsigned long ms_diag_cur_script(void);
#endif
