/* Focused host regression for frozen mutation-batch trace provenance. */
#include <stdio.h>
#include <string.h>

#include "macsurf_diag.h"
#include "macsurf_trace.h"
#include "nsutils/time.h"

/* The diagnostic batch path only needs a monotonic value for progress. */
int nsu_getmonotonic_ms(nsutils_ms_t *now)
{
	if (now != (nsutils_ms_t *) 0) {
		*now = 0;
	}
	return 0;
}

static int expect_line(const char *trace, const char *line)
{
	if (strstr(trace, line) != (const char *) 0) {
		return 1;
	}
	fprintf(stderr, "missing trace line: %s\ntrace:\n%s\n", line, trace);
	return 0;
}

int main(void)
{
	struct ms_diag_scope scope;
	struct ms_diag_provenance prov;
	unsigned long batch;
	char trace[4096];
	int ok = 1;

	memset(&scope, 0, sizeof(scope));
	memset(&prov, 0, sizeof(prov));
	macsurf_trace_arm(0, 1);

	/* The original ambient API must remain a snapshot of the current task. */
	ms_diag_task_enter_external(&scope, 91, MS_TASK_SCRIPT, 92, 93, 0,
		(const char *) 0);
	macsurf_trace_emit(MS_TC_ERROR, MS_TE_NONE, 0, 0, 7, 8);
	ms_diag_task_leave(&scope);

	prov.nav = 11;
	prov.frame = 12;
	prov.doc = 13;
	prov.script = 14;
	prov.task = 15;
	batch = ms_diag_batch_open(&prov);
	ms_diag_batch_add(batch, 1, 15);
	ms_diag_batch_add(batch, 2, 16); /* mixed task ownership -> task=0 */
	ms_diag_batch_freeze(batch);
	macsurf_trace_serialize(trace, sizeof(trace));

	ok &= expect_line(trace,
		"cat=ERROR ev=0 st=0 rs=0 nav=92 frame=0 doc=0 script=93 "
		"task=91 batch=0 pass=0 paint=0 a=7 b=8");
	ok &= expect_line(trace,
		"cat=MUTATION ev=3 st=0 rs=0 nav=11 frame=12 doc=13 script=14 "
		"task=15 batch=1 pass=0 paint=0 a=1 b=13");
	ok &= expect_line(trace,
		"cat=MUTATION ev=4 st=0 rs=0 nav=11 frame=12 doc=13 script=14 "
		"task=15 batch=1 pass=0 paint=0 a=1 b=1");
	ok &= expect_line(trace,
		"cat=MUTATION ev=4 st=0 rs=0 nav=11 frame=12 doc=13 script=14 "
		"task=0 batch=1 pass=0 paint=0 a=1 b=2");
	ok &= expect_line(trace,
		"cat=MUTATION ev=5 st=0 rs=0 nav=11 frame=12 doc=13 script=14 "
		"task=0 batch=1 pass=0 paint=0 a=1 b=2");

	if (!ok) {
		return 1;
	}
	puts("trace provenance PASS");
	return 0;
}
