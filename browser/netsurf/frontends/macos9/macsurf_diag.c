/*
 * MacSurf  -  macsurf_diag.c   (MacSurf Trace diagnostic state boundary)
 * See macsurf_diag.h.
 */

#include <stdio.h>	/* snprintf -- real MSL on the Mac build, not the
			 * hand-rolled macsurf_debug_log_writef formatter */
#include <string.h>

#include "content/fetch.h"	/* fetch_get_{nav_id,request_id,redirect_from} */

#include "macsurf_diag.h"
#include "macsurf_gap.h"

/* All writers run on the cooperative main / notifier context; no locking. */

/* --- per-nav summary tally (frozen at NAV: DONE) --- */
static unsigned long g_req_cur;
static unsigned long g_req_cur_fail;
static unsigned long g_req_last;
static unsigned long g_req_last_fail;
static unsigned long g_diag_last_nav;

/* --- bounded session request ring (NOT a last-nav snapshot) --- */
#define MS_DIAG_RING_N 256

struct ms_diag_request {
	unsigned long request_id;	/* 0 == empty slot */
	unsigned long nav_id;
	unsigned long redirect_from;
	unsigned long bytes_in;
	unsigned long bytes_out;
	short state;			/* enum ms_req_state */
	short status;			/* HTTP code, 0 if none */
	short scheme;			/* enum ms_req_scheme */
};

static struct ms_diag_request g_req_ring[MS_DIAG_RING_N];
static int g_req_ring_head;		/* next slot to write */
static unsigned long g_req_ring_total;	/* lifetime records (for "N more") */

void macsurf_diag_request_record(void *f, int state, int status,
	int scheme, unsigned long bytes_in, unsigned long bytes_out)
{
	struct ms_diag_request *e;
	unsigned long nav = fetch_get_nav_id((const struct fetch *) f);
	int failed = (state == MS_REQ_FAIL) || (status >= 400);

	e = &g_req_ring[g_req_ring_head];
	e->request_id = fetch_get_request_id((const struct fetch *) f);
	e->nav_id = nav;
	e->redirect_from = fetch_get_redirect_from((const struct fetch *) f);
	e->bytes_in = bytes_in;
	e->bytes_out = bytes_out;
	e->state = (short) state;
	e->status = (short) status;
	e->scheme = (short) scheme;

	g_req_ring_head = (g_req_ring_head + 1) % MS_DIAG_RING_N;
	g_req_ring_total++;

	/* per-nav summary tally: redirects are hops, not completions */
	if (state != MS_REQ_REDIRECT) {
		g_req_cur++;
		if (failed) {
			g_req_cur_fail++;
		}
	}
}

void macsurf_diag_nav_done(unsigned long nav_id)
{
	g_req_last = g_req_cur;
	g_req_last_fail = g_req_cur_fail;
	g_diag_last_nav = nav_id;
	g_req_cur = 0;
	g_req_cur_fail = 0;
}

/* Bounded append: returns the new length, never writes past cap-1, always
 * leaves buf NUL-terminated. */
static long diag_cat(char *buf, long cap, long n, const char *s)
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

long macsurf_diag_serialize_summary(char *buf, long cap)
{
	char line[96];
	long n = 0;

	if (buf == NULL || cap < 2) {
		return 0;
	}
	buf[0] = '\0';

	n = diag_cat(buf, cap, n, "MSDIAG 1 summary\n");
	snprintf(line, sizeof line, "nav=%lu\n",
		(unsigned long) (g_diag_last_nav ?
			g_diag_last_nav : macsurf_gap_last_nav()));
	n = diag_cat(buf, cap, n, line);
	snprintf(line, sizeof line, "requests=%lu\n", (unsigned long) g_req_last);
	n = diag_cat(buf, cap, n, line);
	snprintf(line, sizeof line, "failures=%lu\n", (unsigned long) g_req_last_fail);
	n = diag_cat(buf, cap, n, line);
	snprintf(line, sizeof line, "gaps_unique=%d\n", macsurf_gap_last_unique());
	n = diag_cat(buf, cap, n, line);
	snprintf(line, sizeof line, "gaps_total=%lu\n",
		(unsigned long) macsurf_gap_last_total());
	n = diag_cat(buf, cap, n, line);

	return n;
}

long macsurf_diag_serialize_gaps(char *buf, long cap)
{
	char line[160];
	long n = 0;
	int kinds;
	int i;

	if (buf == NULL || cap < 2) {
		return 0;
	}
	buf[0] = '\0';

	n = diag_cat(buf, cap, n, "MSDIAG 1 gaps\n");
	snprintf(line, sizeof line, "nav=%lu\n",
		(unsigned long) macsurf_gap_last_nav());
	n = diag_cat(buf, cap, n, line);

	kinds = macsurf_gap_kind_count();
	for (i = 0; i < kinds; i++) {
		unsigned long c = macsurf_gap_last_count(i);
		if (c == 0) {
			continue;
		}
		snprintf(line, sizeof line, "%s=%lu\n",
			macsurf_gap_slug(i), c);
		n = diag_cat(buf, cap, n, line);
	}

	return n;
}

long macsurf_diag_serialize_network(char *buf, long cap)
{
	char line[160];
	long n = 0;
	int emitted = 0;
	int scanned = 0;
	int idx;
	int i;

	if (buf == NULL || cap < 2) {
		return 0;
	}
	buf[0] = '\0';

	n = diag_cat(buf, cap, n, "MSDIAG 1 network\n");
	snprintf(line, sizeof line, "nav=%lu\n",
		(unsigned long) (g_diag_last_nav ?
			g_diag_last_nav : macsurf_gap_last_nav()));
	n = diag_cat(buf, cap, n, line);

	/* newest first: walk back from head */
	for (i = 0; i < MS_DIAG_RING_N; i++) {
		struct ms_diag_request *e;
		idx = (g_req_ring_head - 1 - i + 2 * MS_DIAG_RING_N)
			% MS_DIAG_RING_N;
		e = &g_req_ring[idx];
		if (e->request_id == 0) {
			continue;
		}
		scanned++;
		if (n >= cap - 1) {
			continue;	/* keep scanning only to count "more" */
		}
		snprintf(line, sizeof line,
			"req=%lu nav=%lu state=%d status=%d redirect_from=%lu "
			"in=%lu out=%lu scheme=%d\n",
			(unsigned long) e->request_id,
			(unsigned long) e->nav_id,
			(int) e->state, (int) e->status,
			(unsigned long) e->redirect_from,
			(unsigned long) e->bytes_in,
			(unsigned long) e->bytes_out,
			(int) e->scheme);
		{
			long before = n;
			n = diag_cat(buf, cap, n, line);
			if (n > before) {
				emitted++;
			}
		}
	}

	if (scanned > emitted) {
		snprintf(line, sizeof line, "more=%d\n", scanned - emitted);
		n = diag_cat(buf, cap, n, line);
	}
	snprintf(line, sizeof line, "ring_total=%lu\n",
		(unsigned long) g_req_ring_total);
	n = diag_cat(buf, cap, n, line);

	return n;
}

/* ======================= Phase 1b: script / task ======================= */

static unsigned long g_cur_script;
static unsigned long g_cur_task;
static unsigned long g_cur_nav;
static unsigned long g_script_seq;
static unsigned long g_task_seq;

#define MS_SCRIPT_RING_N 64
#define MS_TASK_RING_N   128
#define MS_NAME_MAX      40

struct ms_diag_script {
	unsigned long id;		/* 0 == empty */
	unsigned long nav_id;
	short kind;			/* enum ms_script_kind */
	short state;			/* enum ms_script_state */
	char name[MS_NAME_MAX];
};
struct ms_diag_task {
	unsigned long id;		/* 0 == empty */
	unsigned long nav_id;
	unsigned long script_id;
	unsigned long extra;		/* req_id (xhr) | job count (microtask) */
	short kind;			/* enum ms_task_kind */
	short capped;			/* microtask: cap hit */
	char name[MS_NAME_MAX];		/* event type, else "" */
};

static struct ms_diag_script g_script_ring[MS_SCRIPT_RING_N];
static int g_script_ring_head;
static struct ms_diag_task g_task_ring[MS_TASK_RING_N];
static int g_task_ring_head;

static void ms_name_copy(char *dst, const char *src)
{
	int i = 0;
	if (src == NULL) {
		dst[0] = '\0';
		return;
	}
	while (src[i] != '\0' && i < MS_NAME_MAX - 1) {
		char c = src[i];
		/* keep the serialised block one-token-per-field parseable */
		dst[i] = (c == ' ' || c == '\n' || c == '\r' || c == '=') ? '_' : c;
		i++;
	}
	dst[i] = '\0';
}

void ms_diag_script_enter(struct ms_diag_scope *s, unsigned long nav_id,
	int kind, const char *name)
{
	struct ms_diag_script *e;

	s->prev_script = g_cur_script;
	s->prev_task = g_cur_task;
	s->my_id = ++g_script_seq;
	if (g_script_seq == 0) {
		s->my_id = g_script_seq = 1;
	}

	e = &g_script_ring[g_script_ring_head];
	g_script_ring_head = (g_script_ring_head + 1) % MS_SCRIPT_RING_N;
	e->id = s->my_id;
	e->nav_id = nav_id;
	e->kind = (short) kind;
	e->state = (short) MS_SCR_RUNNING;
	ms_name_copy(e->name, name);

	g_cur_script = s->my_id;
	if (nav_id != 0) {
		g_cur_nav = nav_id;
	}
}

void ms_diag_script_leave(struct ms_diag_scope *s, int state)
{
	int i;
	for (i = 0; i < MS_SCRIPT_RING_N; i++) {
		if (g_script_ring[i].id == s->my_id) {
			g_script_ring[i].state = (short) state;
			break;
		}
	}
	g_cur_script = s->prev_script;
	g_cur_task = s->prev_task;
}

unsigned long ms_diag_task_enter(struct ms_diag_scope *s, int kind,
	unsigned long nav_id, unsigned long origin_script,
	unsigned long extra, const char *name)
{
	struct ms_diag_task *e;

	s->prev_script = g_cur_script;
	s->prev_task = g_cur_task;

	/* Nested rule: a dispatch from inside a live task is NOT a new turn. */
	if (kind == MS_TASK_EVENT && g_cur_task != 0) {
		s->my_id = 0;
		return 0;
	}

	s->my_id = ++g_task_seq;
	if (g_task_seq == 0) {
		s->my_id = g_task_seq = 1;
	}

	e = &g_task_ring[g_task_ring_head];
	g_task_ring_head = (g_task_ring_head + 1) % MS_TASK_RING_N;
	e->id = s->my_id;
	e->nav_id = nav_id;
	e->script_id = origin_script;
	e->extra = extra;
	e->kind = (short) kind;
	e->capped = 0;
	ms_name_copy(e->name, name);

	g_cur_task = s->my_id;
	if (nav_id != 0) {
		g_cur_nav = nav_id;
	}
	if (origin_script != 0) {
		g_cur_script = origin_script;
	}
	return s->my_id;
}

void ms_diag_task_leave(struct ms_diag_scope *s)
{
	g_cur_script = s->prev_script;
	g_cur_task = s->prev_task;
}

void ms_diag_task_set_jobs(struct ms_diag_scope *s, unsigned long jobs,
	int capped)
{
	int i;
	if (s->my_id == 0) {
		return;
	}
	for (i = 0; i < MS_TASK_RING_N; i++) {
		if (g_task_ring[i].id == s->my_id) {
			g_task_ring[i].extra = jobs;
			g_task_ring[i].capped = (short) (capped ? 1 : 0);
			break;
		}
	}
}

unsigned long ms_diag_cur_script(void) { return g_cur_script; }
unsigned long ms_diag_cur_task(void)   { return g_cur_task; }
unsigned long ms_diag_cur_nav(void)    { return g_cur_nav; }

static const char *ms_script_kind_s(int k)
{
	return (k == MS_SCRIPT_MODULE) ? "module" : "classic";
}
static const char *ms_script_state_s(int st)
{
	switch (st) {
	case MS_SCR_DONE:         return "done";
	case MS_SCR_COMPILE_FAIL: return "compile_fail";
	case MS_SCR_RUN_FAIL:     return "run_fail";
	case MS_SCR_SKIPPED:      return "skipped";
	default:                  return "running";
	}
}
static const char *ms_task_kind_s(int k)
{
	switch (k) {
	case MS_TASK_TIMER:     return "timer";
	case MS_TASK_EVENT:     return "event";
	case MS_TASK_XHR:       return "xhr";
	case MS_TASK_MICROTASK: return "microtask";
	default:                return "none";
	}
}

long macsurf_diag_serialize_scripts(char *buf, long cap)
{
	char line[128];
	long n = 0;
	int i;

	if (buf == NULL || cap < 2) {
		return 0;
	}
	buf[0] = '\0';
	n = diag_cat(buf, cap, n, "MSDIAG 1 scripts\n");
	for (i = 0; i < MS_SCRIPT_RING_N; i++) {
		int idx = (g_script_ring_head - 1 - i + 2 * MS_SCRIPT_RING_N)
			% MS_SCRIPT_RING_N;
		struct ms_diag_script *e = &g_script_ring[idx];
		if (e->id == 0 || n >= cap - 1) {
			continue;
		}
		snprintf(line, sizeof line,
			"script=%lu nav=%lu kind=%s state=%s name=%s\n",
			(unsigned long) e->id, (unsigned long) e->nav_id,
			ms_script_kind_s(e->kind), ms_script_state_s(e->state),
			e->name[0] ? e->name : "-");
		n = diag_cat(buf, cap, n, line);
	}
	return n;
}

long macsurf_diag_serialize_tasks(char *buf, long cap)
{
	char line[128];
	long n = 0;
	int i;

	if (buf == NULL || cap < 2) {
		return 0;
	}
	buf[0] = '\0';
	n = diag_cat(buf, cap, n, "MSDIAG 1 tasks\n");
	for (i = 0; i < MS_TASK_RING_N; i++) {
		int idx = (g_task_ring_head - 1 - i + 2 * MS_TASK_RING_N)
			% MS_TASK_RING_N;
		struct ms_diag_task *e = &g_task_ring[idx];
		if (e->id == 0 || n >= cap - 1) {
			continue;
		}
		if (e->kind == MS_TASK_XHR) {
			snprintf(line, sizeof line,
				"task=%lu nav=%lu kind=xhr script=%lu req=%lu\n",
				(unsigned long) e->id, (unsigned long) e->nav_id,
				(unsigned long) e->script_id,
				(unsigned long) e->extra);
		} else if (e->kind == MS_TASK_MICROTASK) {
			snprintf(line, sizeof line,
				"task=%lu nav=%lu kind=microtask script=0 "
				"jobs=%lu capped=%d\n",
				(unsigned long) e->id, (unsigned long) e->nav_id,
				(unsigned long) e->extra, (int) e->capped);
		} else if (e->kind == MS_TASK_EVENT) {
			snprintf(line, sizeof line,
				"task=%lu nav=%lu kind=event script=0 event=%s\n",
				(unsigned long) e->id, (unsigned long) e->nav_id,
				e->name[0] ? e->name : "-");
		} else {
			snprintf(line, sizeof line,
				"task=%lu nav=%lu kind=%s script=%lu\n",
				(unsigned long) e->id, (unsigned long) e->nav_id,
				ms_task_kind_s(e->kind),
				(unsigned long) e->script_id);
		}
		n = diag_cat(buf, cap, n, line);
	}
	return n;
}
