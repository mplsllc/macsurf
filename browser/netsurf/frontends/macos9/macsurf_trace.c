/*
 * MacSurf  -  macsurf_trace.c   (universal event ring; see macsurf_trace.h)
 */

#include <stdio.h>	/* snprintf (real MSL on the Mac build) */
#include <string.h>

#include "macsurf_trace.h"
#include "macsurf_diag.h"	/* ms_diag_cur_provenance / ms_diag_cur_paint */

/* All writers run on the cooperative main / notifier context; no locking. */

#define MS_TRACE_RING_N 256

struct ms_trace_entry {
	unsigned long ts;
	unsigned long nav, doc, frame, script, task, batch, pass, paint;
	unsigned short category, event, state, reason;
	unsigned long a, b;
};

static struct ms_trace_entry g_trace_ring[MS_TRACE_RING_N];
static int g_trace_head;
static unsigned long g_trace_total;
static int g_trace_armed_flag;
static unsigned long g_trace_mask;	/* 0 == all */
static int g_trace_level;

#ifdef __MACOS9__
static unsigned long ms_trace_now(void) { return (unsigned long) TickCount(); }
#else
static unsigned long ms_trace_now(void) { return g_trace_total; }
#endif

void macsurf_trace_arm(unsigned long cat_mask, int level)
{
	g_trace_mask = cat_mask;
	g_trace_level = level;
	g_trace_armed_flag = 1;
}

void macsurf_trace_disarm(void)
{
	g_trace_armed_flag = 0;
}

int macsurf_trace_armed(void)
{
	return g_trace_armed_flag;
}

void macsurf_trace_emit(int cat, int event, int state, int reason,
	unsigned long a, unsigned long b)
{
	struct ms_trace_entry *e;
	struct ms_diag_provenance prov;

	if (!g_trace_armed_flag) {
		return;
	}
	if (cat < 0 || cat >= MS_TC__N) {
		return;
	}
	if (g_trace_mask != 0 &&
	    (g_trace_mask & (1UL << (unsigned) cat)) == 0) {
		return;
	}

	ms_diag_cur_provenance(&prov);

	e = &g_trace_ring[g_trace_head];
	g_trace_head = (g_trace_head + 1) % MS_TRACE_RING_N;
	g_trace_total++;

	e->ts = ms_trace_now();
	e->nav = prov.nav;
	e->doc = prov.doc;
	e->frame = prov.frame;
	e->script = prov.script;
	e->task = prov.task;
	e->batch = prov.batch;
	e->pass = prov.pass;
	e->paint = ms_diag_cur_paint();
	e->category = (unsigned short) cat;
	e->event = (unsigned short) event;
	e->state = (unsigned short) state;
	e->reason = (unsigned short) reason;
	e->a = a;
	e->b = b;
}

static const char *ms_trace_cat_s(int c)
{
	switch (c) {
	case MS_TC_NAV:      return "NAV";
	case MS_TC_DOC:      return "DOC";
	case MS_TC_SCRIPT:   return "SCRIPT";
	case MS_TC_TASK:     return "TASK";
	case MS_TC_MUTATION: return "MUTATION";
	case MS_TC_STYLE:    return "STYLE";
	case MS_TC_LAYOUT:   return "LAYOUT";
	case MS_TC_PAINT:    return "PAINT";
	case MS_TC_ERROR:    return "ERROR";
	default:             return "?";
	}
}

static long ms_trace_cat_append(char *buf, long cap, long n, const char *s)
{
	long slen;
	if (buf == NULL || cap <= 0 || n < 0 || n >= cap) {
		return (n < 0) ? 0 : n;
	}
	slen = (long) strlen(s);
	if (n + slen >= cap) {
		slen = cap - 1 - n;
	}
	if (slen > 0) {
		memcpy(buf + n, s, (size_t) slen);
		n += slen;
	}
	buf[n] = '\0';
	return n;
}

long macsurf_trace_serialize(char *buf, long cap)
{
	char line[224];
	long n = 0;
	int i;
	int emitted = 0;
	int scanned = 0;

	if (buf == NULL || cap < 2) {
		return 0;
	}
	buf[0] = '\0';
	n = ms_trace_cat_append(buf, cap, n, "MSDIAG 1 trace\n");
	snprintf(line, sizeof line, "armed=%d mask=%lu level=%d ring_total=%lu\n",
		g_trace_armed_flag, (unsigned long) g_trace_mask,
		g_trace_level, (unsigned long) g_trace_total);
	n = ms_trace_cat_append(buf, cap, n, line);

	for (i = 0; i < MS_TRACE_RING_N; i++) {
		int idx = (g_trace_head - 1 - i + 2 * MS_TRACE_RING_N)
			% MS_TRACE_RING_N;
		struct ms_trace_entry *e = &g_trace_ring[idx];
		if (e->ts == 0 && e->category == 0 && e->event == 0) {
			continue;	/* never written */
		}
		scanned++;
		if (n >= cap - 1) {
			continue;
		}
		snprintf(line, sizeof line,
			"ts=%lu cat=%s ev=%d st=%d rs=%d nav=%lu frame=%lu "
			"doc=%lu script=%lu task=%lu batch=%lu pass=%lu "
			"paint=%lu a=%lu b=%lu\n",
			(unsigned long) e->ts, ms_trace_cat_s((int) e->category),
			(int) e->event, (int) e->state, (int) e->reason,
			(unsigned long) e->nav, (unsigned long) e->frame,
			(unsigned long) e->doc, (unsigned long) e->script,
			(unsigned long) e->task, (unsigned long) e->batch,
			(unsigned long) e->pass, (unsigned long) e->paint,
			(unsigned long) e->a, (unsigned long) e->b);
		{
			long before = n;
			n = ms_trace_cat_append(buf, cap, n, line);
			if (n > before) {
				emitted++;
			}
		}
	}

	if (scanned > emitted) {
		snprintf(line, sizeof line, "more=%d\n", scanned - emitted);
		n = ms_trace_cat_append(buf, cap, n, line);
	}
	return n;
}
