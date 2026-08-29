/*
 * MacSurf  -  macsurf_diag.c   (MacSurf Trace diagnostic state boundary)
 * See macsurf_diag.h.
 */

#include <stdio.h>	/* snprintf -- real MSL on the Mac build, not the
			 * hand-rolled macsurf_debug_log_writef formatter */
#include <string.h>

#include "content/fetch.h"	/* fetch_get_{nav_id,request_id,redirect_from} */
#include "utils/nsoption.h"

#include "macsurf_diag.h"
#include "macsurf_gap.h"
#include "macsurf_trace.h"	/* Milestone 1c: mirror lifecycle to the ring */

/* All writers run on the cooperative main / notifier context; no locking. */

/* Phase 2 contract helpers are defined below the existing trace rings, but
 * module/observer writers occur earlier in this file. */
static void ms_contract_module_event(unsigned long mod_id,
	unsigned long wait_id, int event_type, int reason);
static void ms_contract_io_event(unsigned long io_id, unsigned long target_id,
	int event_type);

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
	char line[128];
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
	snprintf(line, sizeof line, "prefs js=%d css=%d fg_img=%d bg_img=%d anim=%d cookies=%d referer=%d dnt=%d ads=%d popups=%d\n",
		(int)nsoption_bool(enable_javascript),
		(int)nsoption_bool(author_level_css),
		(int)nsoption_bool(foreground_images),
		(int)nsoption_bool(background_images),
		(int)nsoption_bool(animate_images),
		(int)nsoption_bool(accept_cookies),
		(int)nsoption_bool(send_referer),
		(int)nsoption_bool(do_not_track),
		(int)nsoption_bool(block_advertisements),
		(int)nsoption_bool(disable_popups));
	n = diag_cat(buf, cap, n, line);

	return n;
}

long macsurf_diag_serialize_prefs(char *buf, long cap)
{
	char line[128];
	long n = 0;
	int i;

	if (buf == NULL || cap < 2) {
		return 0;
	}
	buf[0] = '\0';

	n = diag_cat(buf, cap, n, "MSDIAG 1 prefs\n");
	snprintf(line, sizeof line, "js=%d css=%d fg_img=%d bg_img=%d anim=%d cookies=%d referer=%d dnt=%d ads=%d popups=%d\n",
		(int)nsoption_bool(enable_javascript),
		(int)nsoption_bool(author_level_css),
		(int)nsoption_bool(foreground_images),
		(int)nsoption_bool(background_images),
		(int)nsoption_bool(animate_images),
		(int)nsoption_bool(accept_cookies),
		(int)nsoption_bool(send_referer),
		(int)nsoption_bool(do_not_track),
		(int)nsoption_bool(block_advertisements),
		(int)nsoption_bool(disable_popups));
	n = diag_cat(buf, cap, n, line);

	if (nsoptions != NULL && nsoptions_default != NULL) {
		for (i = 0; i < NSOPTION_LISTEND; i++) {
			struct nsoption_s *o = &nsoptions[i];
			struct nsoption_s *d = &nsoptions_default[i];
			if (o->type != d->type) continue;
			switch (o->type) {
			case OPTION_BOOL:
				if (o->value.b != d->value.b) {
					snprintf(line, sizeof line, "delta %s=%d default=%d\n",
						o->key, (int)o->value.b, (int)d->value.b);
					n = diag_cat(buf, cap, n, line);
				}
				break;
			case OPTION_INTEGER:
				if (o->value.i != d->value.i) {
					snprintf(line, sizeof line, "delta %s=%d default=%d\n",
						o->key, o->value.i, d->value.i);
					n = diag_cat(buf, cap, n, line);
				}
				break;
			default:
				break;
			}
		}
	}
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

/* ================= Milestone 1c: Causal Render Trace ================= */

/* Mutation-kind slug table. Index order MUST match MACOS9_DOMMUT_* in
 * macos9_reconvert.h (0=unknown .. 10=setattr_style); not included here to keep
 * this TU independent of the frontend reconvert header. */
#define MS_MUT_KINDS 11
static const char *ms_mut_slug[MS_MUT_KINDS] = {
	"unknown", "setattr", "rmattr", "text", "innerhtml", "append",
	"remove", "insert", "chardata", "class", "style"
};

#define MS_DOC_RING_N    32
#define MS_BATCH_RING_N  64
#define MS_STAGE_RING_N  96
#define MS_PASS_RING_N   64
#define MS_PAINT_RING_N  48

enum { MS_DOC_LIVE = 0, MS_DOC_DEAD = 1 };

struct ms_diag_document {
	unsigned long id;		/* 0 == empty */
	unsigned long nav;
	unsigned long frame;
	short state;
};
struct ms_diag_mut_batch {
	unsigned long id;		/* 0 == empty */
	unsigned long nav, frame, doc, script, task;
	unsigned long total;
	unsigned long counts[MS_MUT_KINDS];
	short mixed_tasks;
	short frozen;
};
struct ms_diag_pass {
	unsigned long id;		/* 0 == empty */
	unsigned long nav, frame, doc, batch, task, script;
	short kind;			/* enum ms_render_kind */
	short result;			/* enum ms_render_result */
	short reason;			/* enum ms_stage_reason */
	unsigned long paint;		/* bound paint id, 0 if none */
};
struct ms_diag_stage {
	unsigned long id;		/* 0 == empty */
	unsigned long nav, doc, task, batch, pass;
	unsigned long bits_old, bits_new;
	short kind;			/* enum ms_stage_kind */
	short result;			/* enum ms_stage_result */
	short reason;			/* enum ms_stage_reason */
	short detail;			/* stage-specific sub-code, 0 if n/a */
	short cand;
	char tag[16];
};
struct ms_diag_paint {
	unsigned long id;		/* 0 == empty */
	unsigned long nav, doc, pass, task;
	unsigned long invalidations;
	short requested;			/* 1 once a redraw was requested */
};

static struct ms_diag_document g_doc_ring[MS_DOC_RING_N];
static int g_doc_ring_head;
static struct ms_diag_mut_batch g_batch_ring[MS_BATCH_RING_N];
static int g_batch_ring_head;
static struct ms_diag_pass g_pass_ring[MS_PASS_RING_N];
static int g_pass_ring_head;
static struct ms_diag_stage g_stage_ring[MS_STAGE_RING_N];
static int g_stage_ring_head;
static struct ms_diag_paint g_paint_ring[MS_PAINT_RING_N];
static int g_paint_ring_head;

static unsigned long g_doc_seq;
static unsigned long g_frame_seq;
static unsigned long g_batch_seq;
static unsigned long g_pass_seq;
static unsigned long g_stage_seq;
static unsigned long g_paint_seq;

/* Browser-window identity is deliberately external to struct browser_window.
 * The core owns contiguous arrays of that struct for frames/iframes, so
 * expanding it makes the trace ABI-sensitive across all of those users.  A
 * bounded pointer-keyed table preserves a stable id for each live browsing
 * context without widening a core object.  128 covers every concurrently live
 * top-level/frame context on the target; a full table rotates only diagnostic
 * attribution, never browser state. */
#define MS_FRAME_RING_N 128
struct ms_diag_frame {
	const void *bw;
	unsigned long id;
};
static struct ms_diag_frame g_frame_ring[MS_FRAME_RING_N];
static int g_frame_ring_head;

/* ambient render scope (pushed by render_enter / render_slice_push) */
static unsigned long g_cur_doc;
static unsigned long g_cur_frame;
static unsigned long g_cur_batch;
static unsigned long g_cur_pass;
static unsigned long g_cur_paint;

static unsigned long ms_next(unsigned long *seq)
{
	unsigned long v = ++(*seq);
	if (v == 0) {
		v = *seq = 1;
	}
	return v;
}

void ms_diag_frame_open(void *bw)
{
	int i;
	int free_slot = -1;
	struct ms_diag_frame *e;

	if (bw == NULL) {
		return;
	}
	for (i = 0; i < MS_FRAME_RING_N; i++) {
		if (g_frame_ring[i].bw == bw) {
			return;
		}
		if (free_slot == -1 && g_frame_ring[i].bw == NULL) {
			free_slot = i;
		}
	}
	if (free_slot != -1) {
		e = &g_frame_ring[free_slot];
	} else {
		e = &g_frame_ring[g_frame_ring_head];
		g_frame_ring_head = (g_frame_ring_head + 1) % MS_FRAME_RING_N;
	}
	e->bw = bw;
	e->id = ms_next(&g_frame_seq);
}

unsigned long ms_diag_frame_get(const void *bw)
{
	int i;

	if (bw == NULL) {
		return 0;
	}
	for (i = 0; i < MS_FRAME_RING_N; i++) {
		if (g_frame_ring[i].bw == bw) {
			return g_frame_ring[i].id;
		}
	}
	return 0;
}

void ms_diag_frame_close(void *bw)
{
	int i;

	if (bw == NULL) {
		return;
	}
	for (i = 0; i < MS_FRAME_RING_N; i++) {
		if (g_frame_ring[i].bw == bw) {
			g_frame_ring[i].bw = NULL;
			g_frame_ring[i].id = 0;
			return;
		}
	}
}

/* --- documents --- */

unsigned long ms_diag_document_open(unsigned long nav, unsigned long frame)
{
	struct ms_diag_document *e = &g_doc_ring[g_doc_ring_head];
	g_doc_ring_head = (g_doc_ring_head + 1) % MS_DOC_RING_N;
	e->id = ms_next(&g_doc_seq);
	e->nav = nav;
	e->frame = frame;
	e->state = MS_DOC_LIVE;
	macsurf_trace_emit(MS_TC_DOC, MS_TE_DOC_CREATE, 0, 0, e->id, nav);
	return e->id;
}

void ms_diag_document_set_frame(unsigned long doc_id, unsigned long frame)
{
	int i;
	if (doc_id == 0) {
		return;
	}
	for (i = 0; i < MS_DOC_RING_N; i++) {
		if (g_doc_ring[i].id == doc_id) {
			g_doc_ring[i].frame = frame;
			return;
		}
	}
}

void ms_diag_document_close(unsigned long doc_id)
{
	int i;
	if (doc_id == 0) {
		return;
	}
	for (i = 0; i < MS_DOC_RING_N; i++) {
		if (g_doc_ring[i].id == doc_id) {
			g_doc_ring[i].state = MS_DOC_DEAD;
			macsurf_trace_emit(MS_TC_DOC, MS_TE_DOC_DESTROY, 0, 0,
				doc_id, 0);
			return;
		}
	}
}

/* --- mutation batches --- */

static struct ms_diag_mut_batch *ms_batch_find(unsigned long id)
{
	int i;
	if (id == 0) {
		return (struct ms_diag_mut_batch *) 0;
	}
	for (i = 0; i < MS_BATCH_RING_N; i++) {
		if (g_batch_ring[i].id == id) {
			return &g_batch_ring[i];
		}
	}
	return (struct ms_diag_mut_batch *) 0;
}

unsigned long ms_diag_batch_open(const struct ms_diag_provenance *prov)
{
	struct ms_diag_mut_batch *e = &g_batch_ring[g_batch_ring_head];
	g_batch_ring_head = (g_batch_ring_head + 1) % MS_BATCH_RING_N;
	memset(e, 0, sizeof(*e));
	e->id = ms_next(&g_batch_seq);
	if (prov != (const struct ms_diag_provenance *) 0) {
		e->nav = prov->nav;
		e->frame = prov->frame;
		e->doc = prov->doc;
		e->script = prov->script;
		e->task = prov->task;
	}
	macsurf_trace_emit(MS_TC_MUTATION, MS_TE_MUTATION_BEGIN, 0, 0,
		e->id, e->doc);
	return e->id;
}

void ms_diag_batch_add(unsigned long batch_id, int mut_kind, unsigned long task)
{
	struct ms_diag_mut_batch *e = ms_batch_find(batch_id);
	if (e == (struct ms_diag_mut_batch *) 0) {
		return;
	}
	if (mut_kind < 0 || mut_kind >= MS_MUT_KINDS) {
		mut_kind = 0;	/* unknown */
	}
	e->counts[mut_kind]++;
	e->total++;
	/* R2: task ambiguity is independent of node/kind ambiguity. */
	if (task != 0 && e->task != 0 && task != e->task) {
		e->task = 0;
		e->mixed_tasks = 1;
	} else if (e->task == 0 && !e->mixed_tasks && task != 0) {
		e->task = task;
	}
	macsurf_trace_emit(MS_TC_MUTATION, MS_TE_MUTATION_MERGE, 0, 0,
		batch_id, (unsigned long) mut_kind);
}

void ms_diag_batch_freeze(unsigned long batch_id)
{
	struct ms_diag_mut_batch *e = ms_batch_find(batch_id);
	if (e != (struct ms_diag_mut_batch *) 0) {
		e->frozen = 1;
		macsurf_trace_emit(MS_TC_MUTATION, MS_TE_MUTATION_FREEZE, 0, 0,
			batch_id, e->total);
	}
}

/* --- render passes --- */

static struct ms_diag_pass *ms_pass_find(unsigned long id)
{
	int i;
	if (id == 0) {
		return (struct ms_diag_pass *) 0;
	}
	for (i = 0; i < MS_PASS_RING_N; i++) {
		if (g_pass_ring[i].id == id) {
			return &g_pass_ring[i];
		}
	}
	return (struct ms_diag_pass *) 0;
}

static unsigned long ms_pass_alloc(const struct ms_diag_provenance *prov, int kind)
{
	struct ms_diag_pass *e = &g_pass_ring[g_pass_ring_head];
	g_pass_ring_head = (g_pass_ring_head + 1) % MS_PASS_RING_N;
	memset(e, 0, sizeof(*e));
	e->id = ms_next(&g_pass_seq);
	if (prov != (const struct ms_diag_provenance *) 0) {
		e->nav = prov->nav;
		e->frame = prov->frame;
		e->doc = prov->doc;
		e->batch = prov->batch;
		e->task = prov->task;
		e->script = prov->script;
	}
	e->kind = (short) kind;
	e->result = (short) MS_RRES_RUNNING;
	e->reason = (short) MS_SREASON_NONE;
	macsurf_trace_emit(MS_TC_LAYOUT, MS_TE_LAYOUT_BEGIN, 0, 0,
		e->id, (unsigned long) kind);
	return e->id;
}

static void ms_scope_push(struct ms_diag_render_scope *s,
	const struct ms_diag_provenance *prov, unsigned long pass)
{
	s->prev_nav = g_cur_nav;
	s->prev_frame = g_cur_frame;
	s->prev_doc = g_cur_doc;
	s->prev_batch = g_cur_batch;
	s->prev_pass = g_cur_pass;
	s->my_pass = pass;
	if (prov != (const struct ms_diag_provenance *) 0) {
		if (prov->nav != 0) {
			g_cur_nav = prov->nav;
		}
		g_cur_frame = prov->frame;
		g_cur_doc = prov->doc;
		g_cur_batch = prov->batch;
	}
	g_cur_pass = pass;
	g_cur_paint = 0;	/* a fresh pass has not requested paint yet */
}

static void ms_scope_pop(struct ms_diag_render_scope *s)
{
	g_cur_nav = s->prev_nav;
	g_cur_frame = s->prev_frame;
	g_cur_doc = s->prev_doc;
	g_cur_batch = s->prev_batch;
	g_cur_pass = s->prev_pass;
	g_cur_paint = 0;
}

unsigned long ms_diag_render_enter(struct ms_diag_render_scope *s, int kind,
	const struct ms_diag_provenance *prov)
{
	unsigned long pass = ms_pass_alloc(prov, kind);
	ms_scope_push(s, prov, pass);
	return pass;
}

void ms_diag_render_leave(struct ms_diag_render_scope *s, int result, int reason)
{
	struct ms_diag_pass *e = ms_pass_find(s->my_pass);
	if (e != (struct ms_diag_pass *) 0) {
		e->result = (short) result;
		if (reason != MS_SREASON_NONE) {
			e->reason = (short) reason;
		}
		if (g_cur_paint != 0) {
			e->paint = g_cur_paint;
		}
	}
	macsurf_trace_emit(MS_TC_LAYOUT,
		(result == MS_RRES_FAIL) ? MS_TE_LAYOUT_FAIL : MS_TE_LAYOUT_DONE,
		(short) result, (short) reason, s->my_pass, 0);
	ms_scope_pop(s);
}

unsigned long ms_diag_render_open(struct ms_diag_provenance *prov, int kind)
{
	unsigned long pass = ms_pass_alloc(prov, kind);
	if (prov != (struct ms_diag_provenance *) 0) {
		prov->pass = pass;
	}
	return pass;
}

void ms_diag_render_slice_push(struct ms_diag_render_scope *s,
	const struct ms_diag_provenance *prov)
{
	unsigned long pass = (prov != (const struct ms_diag_provenance *) 0)
		? prov->pass : 0;
	ms_scope_push(s, prov, pass);
	macsurf_trace_emit(MS_TC_LAYOUT, MS_TE_LAYOUT_ASYNC, 0, 0, pass, 0);
}

void ms_diag_render_slice_pop(struct ms_diag_render_scope *s)
{
	struct ms_diag_pass *e = ms_pass_find(s->my_pass);
	if (e != (struct ms_diag_pass *) 0 && g_cur_paint != 0) {
		e->paint = g_cur_paint;	/* remember any paint from this slice */
	}
	ms_scope_pop(s);
}

void ms_diag_render_close(unsigned long pass_id, int result, int reason)
{
	struct ms_diag_pass *e = ms_pass_find(pass_id);
	if (e != (struct ms_diag_pass *) 0) {
		e->result = (short) result;
		if (reason != MS_SREASON_NONE) {
			e->reason = (short) reason;
		}
	}
	macsurf_trace_emit(MS_TC_LAYOUT,
		(result == MS_RRES_FAIL) ? MS_TE_LAYOUT_FAIL : MS_TE_LAYOUT_DONE,
		(short) result, (short) reason, pass_id, 0);
}

/* --- structured stage record --- */

void ms_diag_render_stage(int stage_kind, int result, int reason, int detail,
	unsigned long bits_old, unsigned long bits_new,
	const char *tag, int candidate_count)
{
	struct ms_diag_stage *e = &g_stage_ring[g_stage_ring_head];
	g_stage_ring_head = (g_stage_ring_head + 1) % MS_STAGE_RING_N;
	memset(e, 0, sizeof(*e));
	e->id = ms_next(&g_stage_seq);
	e->nav = g_cur_nav;
	e->doc = g_cur_doc;
	e->task = g_cur_task;
	e->batch = g_cur_batch;
	e->pass = g_cur_pass;
	e->bits_old = bits_old;
	e->bits_new = bits_new;
	e->kind = (short) stage_kind;
	e->result = (short) result;
	e->reason = (short) reason;
	e->detail = (short) detail;
	e->cand = (short) candidate_count;
	{
		int j = 0;
		if (tag != (const char *) 0) {
			while (tag[j] != '\0' && j < (int) sizeof(e->tag) - 1) {
				char c = tag[j];
				e->tag[j] = (c == ' ' || c == '\n' ||
					c == '\r' || c == '=') ? '_' : c;
				j++;
			}
		}
		if (j == 0) {
			e->tag[j++] = '-';
		}
		e->tag[j] = '\0';
	}

	{
		int ev = MS_TE_STYLEFAST_DECLINE;
		if (stage_kind == MS_STAGE_INHERITED_COLOR) {
			ev = (result == MS_SRES_COMMIT) ? MS_TE_INHERITED_COMMIT
				: MS_TE_INHERITED_DECLINE;
		} else {
			ev = (result == MS_SRES_COMMIT) ? MS_TE_STYLEFAST_COMMIT
				: MS_TE_STYLEFAST_DECLINE;
		}
		macsurf_trace_emit(MS_TC_STYLE, ev, (short) result,
			(short) reason, bits_old, bits_new);
	}
}

/* --- paint --- */

void ms_diag_paint_note(void)
{
	struct ms_diag_paint *e;
	struct ms_diag_pass *p;

	if (g_cur_pass == 0) {
		return;	/* not render-driven (caret blink, selection, ...) */
	}
	if (g_cur_paint != 0) {
		int i;
		for (i = 0; i < MS_PAINT_RING_N; i++) {
			if (g_paint_ring[i].id == g_cur_paint) {
				g_paint_ring[i].invalidations++;
				macsurf_trace_emit(MS_TC_PAINT,
					MS_TE_PAINT_INVALIDATE, 0, 0,
					g_cur_paint,
					g_paint_ring[i].invalidations);
				return;
			}
		}
		return;
	}
	e = &g_paint_ring[g_paint_ring_head];
	g_paint_ring_head = (g_paint_ring_head + 1) % MS_PAINT_RING_N;
	memset(e, 0, sizeof(*e));
	e->id = ms_next(&g_paint_seq);
	e->nav = g_cur_nav;
	e->doc = g_cur_doc;
	e->pass = g_cur_pass;
	e->task = g_cur_task;
	e->invalidations = 1;
	e->requested = 1;
	g_cur_paint = e->id;
	p = ms_pass_find(g_cur_pass);
	if (p != (struct ms_diag_pass *) 0) {
		p->paint = e->id;
	}
	macsurf_trace_emit(MS_TC_PAINT, MS_TE_PAINT_INVALIDATE, 0, 0,
		e->id, 1);
}

/* --- cur scope readers --- */

void ms_diag_cur_provenance(struct ms_diag_provenance *out)
{
	if (out == (struct ms_diag_provenance *) 0) {
		return;
	}
	out->nav = g_cur_nav;
	out->frame = g_cur_frame;
	out->doc = g_cur_doc;
	out->script = g_cur_script;
	out->task = g_cur_task;
	out->batch = g_cur_batch;
	out->pass = g_cur_pass;
}

unsigned long ms_diag_cur_doc(void)   { return g_cur_doc; }
unsigned long ms_diag_cur_frame(void) { return g_cur_frame; }
unsigned long ms_diag_cur_batch(void) { return g_cur_batch; }
unsigned long ms_diag_cur_pass(void)  { return g_cur_pass; }
unsigned long ms_diag_cur_paint(void) { return g_cur_paint; }

/* --- serialisers --- */

static const char *ms_render_kind_s(int k)
{
	switch (k) {
	case MS_RENDER_RECONVERT:      return "reconvert";
	case MS_RENDER_FAST_STYLE:     return "fast_style";
	case MS_RENDER_FAST_INHERITED: return "fast_inherited";
	default:                       return "initial";
	}
}
static const char *ms_render_result_s(int r)
{
	switch (r) {
	case MS_RRES_DONE:     return "done";
	case MS_RRES_FALLBACK: return "fallback";
	case MS_RRES_FAIL:     return "fail";
	case MS_RRES_QUEUED:   return "queued";
	default:               return "running";
	}
}
static const char *ms_stage_kind_s(int k)
{
	return (k == MS_STAGE_INHERITED_COLOR) ? "inherited_color" : "stylefast";
}
static const char *ms_stage_result_s(int r)
{
	switch (r) {
	case MS_SRES_COMMIT:   return "commit";
	case MS_SRES_FALLBACK: return "fallback";
	default:               return "decline";
	}
}
static const char *ms_stage_reason_s(int r)
{
	switch (r) {
	case MS_SREASON_CLASSIFIER_OTHER:          return "classifier_other";
	case MS_SREASON_BORDER_WIDTH_BITS_DIFFER:  return "border_width_bits_differ";
	case MS_SREASON_STRUCTURAL_IN_BATCH:       return "structural_in_batch";
	case MS_SREASON_NOT_READY:                 return "not_ready";
	case MS_SREASON_NO_CANDIDATE:              return "no_candidate";
	default:                                   return "none";
	}
}

long macsurf_diag_serialize_documents(char *buf, long cap)
{
	char line[128];
	long n = 0;
	int i;

	if (buf == NULL || cap < 2) {
		return 0;
	}
	buf[0] = '\0';
	n = diag_cat(buf, cap, n, "MSDIAG 1 documents\n");
	for (i = 0; i < MS_DOC_RING_N; i++) {
		int idx = (g_doc_ring_head - 1 - i + 2 * MS_DOC_RING_N)
			% MS_DOC_RING_N;
		struct ms_diag_document *e = &g_doc_ring[idx];
		if (e->id == 0 || n >= cap - 1) {
			continue;
		}
		snprintf(line, sizeof line,
			"doc=%lu nav=%lu frame=%lu state=%s kind=html\n",
			(unsigned long) e->id, (unsigned long) e->nav,
			(unsigned long) e->frame,
			(e->state == MS_DOC_DEAD) ? "dead" : "live");
		n = diag_cat(buf, cap, n, line);
	}
	return n;
}

long macsurf_diag_serialize_mutations(char *buf, long cap)
{
	char line[224];
	char frag[32];
	long n = 0;
	int i;
	int k;

	if (buf == NULL || cap < 2) {
		return 0;
	}
	buf[0] = '\0';
	n = diag_cat(buf, cap, n, "MSDIAG 1 mutations\n");
	for (i = 0; i < MS_BATCH_RING_N; i++) {
		int idx = (g_batch_ring_head - 1 - i + 2 * MS_BATCH_RING_N)
			% MS_BATCH_RING_N;
		struct ms_diag_mut_batch *e = &g_batch_ring[idx];
		if (e->id == 0 || n >= cap - 1) {
			continue;
		}
		snprintf(line, sizeof line,
			"batch=%lu nav=%lu frame=%lu doc=%lu task=%lu script=%lu "
			"total=%lu mixed=%d frozen=%d",
			(unsigned long) e->id, (unsigned long) e->nav,
			(unsigned long) e->frame, (unsigned long) e->doc,
			(unsigned long) e->task, (unsigned long) e->script,
			(unsigned long) e->total, (int) e->mixed_tasks,
			(int) e->frozen);
		n = diag_cat(buf, cap, n, line);
		for (k = 0; k < MS_MUT_KINDS; k++) {
			if (e->counts[k] == 0) {
				continue;
			}
			snprintf(frag, sizeof frag, " %s=%lu",
				ms_mut_slug[k], (unsigned long) e->counts[k]);
			n = diag_cat(buf, cap, n, frag);
		}
		n = diag_cat(buf, cap, n, "\n");
	}
	return n;
}

long macsurf_diag_serialize_layout(char *buf, long cap)
{
	char line[256];
	long n = 0;
	int i;

	if (buf == NULL || cap < 2) {
		return 0;
	}
	buf[0] = '\0';
	n = diag_cat(buf, cap, n, "MSDIAG 1 layout\n");

	for (i = 0; i < MS_PASS_RING_N; i++) {
		int idx = (g_pass_ring_head - 1 - i + 2 * MS_PASS_RING_N)
			% MS_PASS_RING_N;
		struct ms_diag_pass *e = &g_pass_ring[idx];
		if (e->id == 0 || n >= cap - 1) {
			continue;
		}
		snprintf(line, sizeof line,
			"pass=%lu nav=%lu frame=%lu doc=%lu batch=%lu task=%lu "
			"kind=%s result=%s reason=%s paint=%lu\n",
			(unsigned long) e->id, (unsigned long) e->nav,
			(unsigned long) e->frame, (unsigned long) e->doc,
			(unsigned long) e->batch, (unsigned long) e->task,
			ms_render_kind_s(e->kind), ms_render_result_s(e->result),
			ms_stage_reason_s(e->reason), (unsigned long) e->paint);
		n = diag_cat(buf, cap, n, line);
	}

	for (i = 0; i < MS_STAGE_RING_N; i++) {
		int idx = (g_stage_ring_head - 1 - i + 2 * MS_STAGE_RING_N)
			% MS_STAGE_RING_N;
		struct ms_diag_stage *e = &g_stage_ring[idx];
		if (e->id == 0 || n >= cap - 1) {
			continue;
		}
		snprintf(line, sizeof line,
			"stage=%lu pass=%lu batch=%lu doc=%lu task=%lu kind=%s "
			"result=%s reason=%s detail=%d tag=%s cand=%d "
			"old=%lu new=%lu\n",
			(unsigned long) e->id, (unsigned long) e->pass,
			(unsigned long) e->batch, (unsigned long) e->doc,
			(unsigned long) e->task, ms_stage_kind_s(e->kind),
			ms_stage_result_s(e->result), ms_stage_reason_s(e->reason),
			(int) e->detail, e->tag, (int) e->cand,
			(unsigned long) e->bits_old, (unsigned long) e->bits_new);
		n = diag_cat(buf, cap, n, line);
	}

	for (i = 0; i < MS_PAINT_RING_N; i++) {
		int idx = (g_paint_ring_head - 1 - i + 2 * MS_PAINT_RING_N)
			% MS_PAINT_RING_N;
		struct ms_diag_paint *e = &g_paint_ring[idx];
		if (e->id == 0 || n >= cap - 1) {
			continue;
		}
		snprintf(line, sizeof line,
			"paint=%lu nav=%lu doc=%lu pass=%lu task=%lu "
			"invalidations=%lu state=%s\n",
			(unsigned long) e->id, (unsigned long) e->nav,
			(unsigned long) e->doc, (unsigned long) e->pass,
			(unsigned long) e->task,
			(unsigned long) e->invalidations,
			e->requested ? "requested" : "none");
		n = diag_cat(buf, cap, n, line);
	}

	return n;
}

/* ===================== Module Trace v1 ===================== */

#define MS_MOD_NAME_CAP 256
#define MS_MOD_RING_CAP 256
#define MS_MOD_NAME_MAX 64

struct ms_diag_module_name {
	unsigned long id;
	char name[MS_MOD_NAME_MAX];
};

struct ms_diag_module_event {
	unsigned long id;
	unsigned long nav_id;
	unsigned long script_id;
	unsigned long task_id;
	unsigned long wait_id;
	unsigned long module_id;
	unsigned long dep_module_id;
	unsigned char event_type;
	unsigned char reason;
	unsigned char depth;
};

static struct ms_diag_module_name g_mod_names[MS_MOD_NAME_CAP];
static int g_mod_name_count = 0;
static unsigned long g_mod_name_seq = 0;

static struct ms_diag_module_event g_mod_ring[MS_MOD_RING_CAP];
static int g_mod_ring_head = 0;
static unsigned long g_mod_event_seq = 0;

static const char *ms_mod_event_s(int t)
{
	switch (t) {
	case MS_MOD_DEFINE:   return "define";
	case MS_MOD_REQUEST:  return "request";
	case MS_MOD_RESOLVE:  return "resolve";
	case MS_MOD_EXECUTE:  return "execute";
	case MS_MOD_FAIL:     return "fail";
	case MS_MOD_DECLARE_DEP: return "declare_dep";
	case MS_MOD_WAIT_REGISTERED: return "wait_registered";
	case MS_MOD_CALLBACK_BEGIN: return "callback_begin";
	case MS_MOD_CALLBACK_RETURN: return "callback_return";
	default:              return "unknown";
	}
}

static const char *ms_mod_reason_s(int r)
{
	switch (r) {
	case MS_MOD_REASON_NONE:          return "none";
	case MS_MOD_REASON_MISSING:       return "missing";
	case MS_MOD_REASON_DEP_MISSING:   return "dep_missing";
	case MS_MOD_REASON_FACTORY_THROW: return "factory_throw";
	case MS_MOD_REASON_CYCLE:         return "cycle";
	default:                          return "unknown";
	}
}

unsigned long ms_diag_module_id(const char *name)
{
	int i;
	if (name == NULL || name[0] == '\0')
		return 0;

	for (i = 0; i < g_mod_name_count; i++) {
		if (strncmp(g_mod_names[i].name, name, MS_MOD_NAME_MAX - 1) == 0)
			return g_mod_names[i].id;
	}

	if (g_mod_name_count < MS_MOD_NAME_CAP) {
		int idx = g_mod_name_count++;
		g_mod_names[idx].id = ++g_mod_name_seq;
		ms_name_copy(g_mod_names[idx].name, name);
		return g_mod_names[idx].id;
	}

	return 0;
}

void ms_diag_module_record(unsigned long mod_id, unsigned long dep_mod_id,
	int event_type, int reason, int depth, unsigned long wait_id)
{
	struct ms_diag_module_event *e;
	int idx;

	if (mod_id == 0)
		return;

	idx = g_mod_ring_head % MS_MOD_RING_CAP;
	g_mod_ring_head = (g_mod_ring_head + 1) % MS_MOD_RING_CAP;

	e = &g_mod_ring[idx];
	e->id = ++g_mod_event_seq;
	e->nav_id = ms_diag_cur_nav();
	e->script_id = ms_diag_cur_script();
	e->task_id = ms_diag_cur_task();
	e->wait_id = wait_id;
	e->module_id = mod_id;
	e->dep_module_id = dep_mod_id;
	e->event_type = (unsigned char)event_type;
	e->reason = (unsigned char)reason;
	e->depth = (unsigned char)(depth > 255 ? 255 : depth);
	ms_contract_module_event(mod_id, wait_id, event_type, reason);
}

void ms_diag_module_record_by_name(const char *name, const char *dep_name,
	int event_type, int reason, int depth, unsigned long wait_id)
{
	unsigned long mod_id = ms_diag_module_id(name);
	unsigned long dep_mod_id = dep_name ? ms_diag_module_id(dep_name) : 0;
	ms_diag_module_record(mod_id, dep_mod_id, event_type, reason, depth, wait_id);
}

long macsurf_diag_serialize_modules(char *buf, long cap)
{
	char line[160];
	long n = 0;
	int i;

	if (buf == NULL || cap < 2) {
		return 0;
	}
	buf[0] = '\0';
	n = diag_cat(buf, cap, n, "MSDIAG 1 modules\n");

	for (i = 0; i < g_mod_name_count; i++) {
		if (n >= cap - 1)
			break;
		snprintf(line, sizeof line, "mod=%lu name=%s\n",
			(unsigned long) g_mod_names[i].id,
			g_mod_names[i].name);
		n = diag_cat(buf, cap, n, line);
	}

	for (i = 0; i < MS_MOD_RING_CAP; i++) {
		int idx = (g_mod_ring_head - 1 - i + 2 * MS_MOD_RING_CAP)
			% MS_MOD_RING_CAP;
		struct ms_diag_module_event *e = &g_mod_ring[idx];
		if (e->id == 0 || n >= cap - 1) {
			continue;
		}
		snprintf(line, sizeof line,
			"ev=%lu nav=%lu script=%lu task=%lu mod=%lu dep=%lu wait=%lu "
			"event=%s reason=%s depth=%d\n",
			(unsigned long) e->id, (unsigned long) e->nav_id,
			(unsigned long) e->script_id, (unsigned long) e->task_id,
			(unsigned long) e->module_id, (unsigned long) e->dep_module_id,
			(unsigned long) e->wait_id,
			ms_mod_event_s(e->event_type),
			ms_mod_reason_s(e->reason),
			(int) e->depth);
		n = diag_cat(buf, cap, n, line);
	}

	return n;
}

/* ================== IntersectionObserver Trace v1 ================== */
#define MS_IO_RING_CAP 128
#define MS_IO_NAME_CAP 64
#define MS_IO_NAME_MAX 64

struct ms_diag_io_name {
	unsigned long id;
	char name[MS_IO_NAME_MAX];
};

struct ms_diag_io_event {
	unsigned long id;
	unsigned long nav_id;
	unsigned long script_id;
	unsigned long task_id;
	unsigned long io_id;
	unsigned long target_id;
	long x, y, w, h;
	unsigned char event_type;
	unsigned char intersecting;
	unsigned char ratio_pct;
	unsigned char entries;
};

static struct ms_diag_io_name g_io_names[MS_IO_NAME_CAP];
static int g_io_name_count = 0;
static unsigned long g_io_name_seq = 0;

static struct ms_diag_io_event g_io_ring[MS_IO_RING_CAP];
static int g_io_ring_head = 0;
static unsigned long g_io_event_seq = 0;

static const char *ms_io_event_s(int t)
{
	switch (t) {
	case MS_IO_CONSTRUCT:   return "construct";
	case MS_IO_OBSERVE:     return "observe";
	case MS_IO_QUERY:       return "query";
	case MS_IO_CHECK:       return "check";
	case MS_IO_CALLBACK:    return "callback";
	case MS_IO_SKIP:        return "skip";
	case MS_IO_UNOBSERVE:   return "unobserve";
	case MS_IO_DISCONNECT:  return "disconnect";
	default:                return "unknown";
	}
}

unsigned long ms_diag_io_target_id(const char *name)
{
	int i;
	if (name == NULL || name[0] == '\0')
		return 0;

	for (i = 0; i < g_io_name_count; i++) {
		if (strncmp(g_io_names[i].name, name, MS_IO_NAME_MAX - 1) == 0)
			return g_io_names[i].id;
	}

	if (g_io_name_count < MS_IO_NAME_CAP) {
		int idx = g_io_name_count++;
		g_io_names[idx].id = ++g_io_name_seq;
		ms_name_copy(g_io_names[idx].name, name);
		return g_io_names[idx].id;
	}
	return 0;
}

void ms_diag_io_record(unsigned long io_id, int ev_type, const char *target_name,
	long x, long y, long w, long h, int intersecting, int ratio_pct, int entries)
{
	struct ms_diag_io_event *e = &g_io_ring[g_io_ring_head];
	g_io_ring_head = (g_io_ring_head + 1) % MS_IO_RING_CAP;

	e->id = ++g_io_event_seq;
	e->nav_id = g_cur_nav ? g_cur_nav : g_diag_last_nav;
	e->script_id = g_cur_script;
	e->task_id = g_cur_task;
	e->io_id = io_id;
	e->target_id = ms_diag_io_target_id(target_name);
	e->x = x;
	e->y = y;
	e->w = w;
	e->h = h;
	e->event_type = (unsigned char) ev_type;
	e->intersecting = (unsigned char) (intersecting ? 1 : 0);
	e->ratio_pct = (unsigned char) (ratio_pct > 255 ? 255 : ratio_pct);
	e->entries = (unsigned char) (entries > 255 ? 255 : entries);
	ms_contract_io_event(io_id, e->target_id, ev_type);
}

long macsurf_diag_serialize_io(char *buf, long cap)
{
	char line[160];
	long n = 0;
	int i;

	if (buf == NULL || cap < 2) {
		return 0;
	}
	buf[0] = '\0';
	n = diag_cat(buf, cap, n, "MSDIAG 1 io\n");

	for (i = 0; i < g_io_name_count; i++) {
		if (n >= cap - 1)
			break;
		snprintf(line, sizeof line, "target=%lu name=%s\n",
			(unsigned long) g_io_names[i].id,
			g_io_names[i].name);
		n = diag_cat(buf, cap, n, line);
	}

	for (i = 0; i < MS_IO_RING_CAP; i++) {
		int idx = (g_io_ring_head - 1 - i + 2 * MS_IO_RING_CAP) % MS_IO_RING_CAP;
		struct ms_diag_io_event *e = &g_io_ring[idx];
		if (e->id == 0 || n >= cap - 1) {
			continue;
		}
		if (e->event_type == MS_IO_QUERY) {
			snprintf(line, sizeof line,
				"ev=%lu nav=%lu script=%lu task=%lu io=%lu target=%lu "
				"event=%s x=%ld y=%ld w=%ld h=%ld\n",
				(unsigned long) e->id, (unsigned long) e->nav_id,
				(unsigned long) e->script_id, (unsigned long) e->task_id,
				(unsigned long) e->io_id, (unsigned long) e->target_id,
				ms_io_event_s(e->event_type),
				e->x, e->y, e->w, e->h);
		} else if (e->event_type == MS_IO_CHECK) {
			snprintf(line, sizeof line,
				"ev=%lu nav=%lu script=%lu task=%lu io=%lu target=%lu "
				"event=%s intersecting=%d ratio=%d%%\n",
				(unsigned long) e->id, (unsigned long) e->nav_id,
				(unsigned long) e->script_id, (unsigned long) e->task_id,
				(unsigned long) e->io_id, (unsigned long) e->target_id,
				ms_io_event_s(e->event_type),
				(int) e->intersecting, (int) e->ratio_pct);
		} else if (e->event_type == MS_IO_CALLBACK) {
			snprintf(line, sizeof line,
				"ev=%lu nav=%lu script=%lu task=%lu io=%lu target=%lu "
				"event=%s entries=%d\n",
				(unsigned long) e->id, (unsigned long) e->nav_id,
				(unsigned long) e->script_id, (unsigned long) e->task_id,
				(unsigned long) e->io_id, (unsigned long) e->target_id,
				ms_io_event_s(e->event_type),
				(int) e->entries);
		} else {
			snprintf(line, sizeof line,
				"ev=%lu nav=%lu script=%lu task=%lu io=%lu target=%lu "
				"event=%s\n",
				(unsigned long) e->id, (unsigned long) e->nav_id,
				(unsigned long) e->script_id, (unsigned long) e->task_id,
				(unsigned long) e->io_id, (unsigned long) e->target_id,
				ms_io_event_s(e->event_type));
		}
		n = diag_cat(buf, cap, n, line);
	}
	return n;
}

/* ================= Phase 2: expected-transition contracts =================
 * This is deliberately a fixed, side-table state machine.  It records only
 * transitions MacSurf itself schedules or owns; it does not guess whether a
 * page's application state is complete. */
#define MS_CONTRACT_CAP 128

struct ms_diag_contract {
	unsigned long id, nav_id, script_id, task_id;
	unsigned long operation_id, request_id;
	unsigned long io_id, target_id, timer_id;
	unsigned long module_id, wait_id;
	unsigned long callback_count;
	unsigned char kind, operation_kind, state, expected, last_event, eligible;
};

static struct ms_diag_contract g_contracts[MS_CONTRACT_CAP];
static int g_contract_head;
static unsigned long g_contract_seq;
static unsigned long g_contract_dropped_waiting;

#define MS_TIMER_DIAG_CAP 256
struct ms_diag_timer {
	unsigned long id, nav_id, script_id, task_id, ctx_gen;
	unsigned short io_refs;
	unsigned char state;
};
static struct ms_diag_timer g_timers[MS_TIMER_DIAG_CAP];
static int g_timer_head;

static const char *ms_op_kind_s(int v);
static const char *ms_op_phase_s(int v);

static const char *ms_timer_state_s(int state)
{
	switch (state) {
	case MS_TIMER_ARMED: return "armed";
	case MS_TIMER_DUE: return "due";
	case MS_TIMER_FIRING: return "firing";
	case MS_TIMER_FIRED: return "fired";
	case MS_TIMER_CANCELLED: return "cancelled";
	case MS_TIMER_EVICTED: return "evicted";
	case MS_TIMER_REALM_TEARDOWN: return "realm_teardown";
	case MS_TIMER_OWNER_MISMATCH: return "owner_mismatch";
	case MS_TIMER_ABANDONED: return "abandoned";
	default: return "unknown";
	}
}

static struct ms_diag_timer *ms_timer_find(unsigned long id)
{
	int i;
	for (i = 0; i < MS_TIMER_DIAG_CAP; i++)
		if (g_timers[i].id == id) return &g_timers[i];
	return NULL;
}

void ms_diag_timer_arm(unsigned long id, unsigned long nav, unsigned long script,
	unsigned long task, unsigned long ctx_gen)
{
	struct ms_diag_timer *t;
	int i, slot;
	if (id == 0) return;
	t = ms_timer_find(id);
	if (t == NULL) {
		/* A joined IO timer is the causal record this table exists to keep.
		 * Ordinary timer history may rotate, but never evict a joined record
		 * merely because a long-lived page creates more timers. */
		for (i = 0; i < MS_TIMER_DIAG_CAP; i++) {
			slot = (g_timer_head + i) % MS_TIMER_DIAG_CAP;
			if (g_timers[slot].io_refs == 0) {
				t = &g_timers[slot];
				g_timer_head = (slot + 1) % MS_TIMER_DIAG_CAP;
				break;
			}
		}
		if (t == NULL) return;
	}
	memset(t, 0, sizeof(*t));
	t->id = id; t->nav_id = nav; t->script_id = script;
	t->task_id = task; t->ctx_gen = ctx_gen; t->state = MS_TIMER_ARMED;
}

void ms_diag_timer_state(unsigned long id, int state)
{
	struct ms_diag_timer *t = ms_timer_find(id);
	if (t != NULL) t->state = (unsigned char)state;
}

static int ms_contract_is_unresolved(const struct ms_diag_contract *c)
{
	return c->state == MS_CONTRACT_WAITING ||
		(c->state == MS_CONTRACT_FIRED &&
		 c->expected != MS_EXPECT_NONE);
}

static const char *ms_contract_state_s(int v)
{
	switch (v) {
	case MS_CONTRACT_WAITING: return "waiting";
	case MS_CONTRACT_COMPLETED: return "completed";
	case MS_CONTRACT_FIRED: return "fired";
	case MS_CONTRACT_SATISFIED: return "satisfied";
	case MS_CONTRACT_FAILED: return "failed";
	case MS_CONTRACT_CANCELLED: return "cancelled";
	case MS_CONTRACT_DECLINED: return "declined";
	case MS_CONTRACT_EXPIRED: return "expired";
	default: return "unknown";
	}
}

static const char *ms_contract_expected_s(int v)
{
	switch (v) {
	case MS_EXPECT_WIRE_START: return "wire_start";
	case MS_EXPECT_SETTLE: return "settle";
	case MS_EXPECT_CHECK: return "check";
	case MS_EXPECT_CALLBACK: return "callback";
	case MS_EXPECT_RELEASE: return "release";
	case MS_EXPECT_CALLBACK_RETURN: return "callback_return";
	default: return "none";
	}
}

static struct ms_diag_contract *ms_contract_open(int kind)
{
	struct ms_diag_contract *c = &g_contracts[g_contract_head];
	g_contract_head = (g_contract_head + 1) % MS_CONTRACT_CAP;
	if (c->id != 0 && ms_contract_is_unresolved(c)) {
		g_contract_dropped_waiting++;
	}
	memset(c, 0, sizeof(*c));
	c->id = ++g_contract_seq;
	if (c->id == 0) c->id = ++g_contract_seq;
	c->nav_id = ms_diag_cur_nav();
	if (c->nav_id == 0) c->nav_id = g_diag_last_nav;
	c->script_id = ms_diag_cur_script();
	c->task_id = ms_diag_cur_task();
	c->kind = (unsigned char)kind;
	c->state = MS_CONTRACT_WAITING;
	return c;
}

static struct ms_diag_contract *ms_contract_operation_find(unsigned long op_id)
{
	int i;
	for (i = 0; i < MS_CONTRACT_CAP; i++)
		if (g_contracts[i].kind == MS_CONTRACT_OPERATION &&
			g_contracts[i].operation_id == op_id)
			return &g_contracts[i];
	return NULL;
}

static void ms_contract_operation_event(unsigned long op_id, int kind,
	int phase, int result, unsigned long request_id)
{
	struct ms_diag_contract *c;
	if (op_id == 0) return;
	c = ms_contract_operation_find(op_id);
	if (c == NULL && phase == MS_OP_ATTEMPT) {
		c = ms_contract_open(MS_CONTRACT_OPERATION);
		c->operation_id = op_id;
		c->operation_kind = (unsigned char)kind;
		c->expected = MS_EXPECT_WIRE_START;
	}
	if (c == NULL) return;
	if (request_id != 0) c->request_id = request_id;
	c->last_event = (unsigned char)phase;
	if (phase == MS_OP_ABORT) {
		c->state = MS_CONTRACT_CANCELLED;
		c->expected = MS_EXPECT_NONE;
	} else if (result == MS_OP_DECLINE) {
		c->state = MS_CONTRACT_DECLINED;
		c->expected = MS_EXPECT_NONE;
	} else if (result == MS_OP_REJECT) {
		c->state = MS_CONTRACT_FAILED;
		c->expected = MS_EXPECT_NONE;
	} else if (result == MS_OP_RESOLVE) {
		c->state = MS_CONTRACT_COMPLETED;
		c->expected = MS_EXPECT_NONE;
	} else if (phase == MS_OP_WIRE_START && result == MS_OP_OK) {
		c->state = MS_CONTRACT_WAITING;
		c->expected = MS_EXPECT_SETTLE;
	}
}

static struct ms_diag_contract *ms_contract_io_find(unsigned long io_id,
	unsigned long target_id)
{
	int i;
	for (i = 0; i < MS_CONTRACT_CAP; i++)
		if (g_contracts[i].kind == MS_CONTRACT_IO &&
			g_contracts[i].io_id == io_id &&
			g_contracts[i].target_id == target_id)
			return &g_contracts[i];
	return NULL;
}

void ms_diag_io_timer_bind(unsigned long io_id, const char *target_name,
	unsigned long timer_id)
{
	struct ms_diag_contract *c;
	struct ms_diag_timer *t;
	unsigned long target_id = ms_diag_io_target_id(target_name);
	if (io_id == 0 || target_id == 0 || timer_id == 0) return;
	c = ms_contract_io_find(io_id, target_id);
	if (c == NULL) return;
	c->timer_id = timer_id;
	t = ms_timer_find(timer_id);
	if (t != NULL && t->io_refs != 65535) t->io_refs++;
}

static void ms_contract_io_event(unsigned long io_id, unsigned long target_id,
	int event_type)
{
	struct ms_diag_contract *c;
	int i;
	if (io_id == 0) return;
	if (event_type == MS_IO_DISCONNECT) {
		for (i = 0; i < MS_CONTRACT_CAP; i++) {
			c = &g_contracts[i];
			if (c->kind == MS_CONTRACT_IO && c->io_id == io_id &&
				ms_contract_is_unresolved(c)) {
				c->state = MS_CONTRACT_CANCELLED;
				c->expected = MS_EXPECT_NONE;
				c->last_event = (unsigned char)event_type;
			}
		}
		return;
	}
	if (target_id == 0) return;
	c = ms_contract_io_find(io_id, target_id);
	if (c == NULL && event_type == MS_IO_OBSERVE) {
		c = ms_contract_open(MS_CONTRACT_IO);
		c->io_id = io_id;
		c->target_id = target_id;
		c->expected = MS_EXPECT_CHECK;
	}
	if (c == NULL) return;
	c->last_event = (unsigned char)event_type;
	if (event_type == MS_IO_QUERY) {
		c->expected = MS_EXPECT_CHECK;
	} else if (event_type == MS_IO_CHECK) {
		c->expected = MS_EXPECT_CALLBACK;
	} else if (event_type == MS_IO_CALLBACK) {
		c->callback_count++;
		c->state = MS_CONTRACT_FIRED;
		c->expected = MS_EXPECT_NONE;
	} else if (event_type == MS_IO_SKIP || event_type == MS_IO_UNOBSERVE) {
		c->state = MS_CONTRACT_CANCELLED;
		c->expected = MS_EXPECT_NONE;
	}
}

static struct ms_diag_contract *ms_contract_module_find(unsigned long mod_id,
	unsigned long wait_id)
{
	int i;
	for (i = 0; i < MS_CONTRACT_CAP; i++)
		if (g_contracts[i].kind == MS_CONTRACT_MODULE_WAIT &&
			g_contracts[i].module_id == mod_id &&
			g_contracts[i].wait_id == wait_id)
			return &g_contracts[i];
	return NULL;
}

static void ms_contract_module_event(unsigned long mod_id,
	unsigned long wait_id, int event_type, int reason)
{
	struct ms_diag_contract *c;
	int i;
	if (mod_id == 0) return;
	if (event_type == MS_MOD_WAIT_REGISTERED && wait_id != 0) {
		c = ms_contract_module_find(mod_id, wait_id);
		if (c == NULL) {
			c = ms_contract_open(MS_CONTRACT_MODULE_WAIT);
			c->module_id = mod_id;
			c->wait_id = wait_id;
		}
		c->state = MS_CONTRACT_WAITING;
		c->expected = MS_EXPECT_RELEASE;
		c->last_event = (unsigned char)event_type;
		return;
	}
	if (event_type == MS_MOD_DEFINE) {
		for (i = 0; i < MS_CONTRACT_CAP; i++) {
			c = &g_contracts[i];
			if (c->kind == MS_CONTRACT_MODULE_WAIT &&
				c->module_id == mod_id && ms_contract_is_unresolved(c)) {
				c->eligible = 1;
				c->last_event = (unsigned char)event_type;
			}
		}
		return;
	}
	if (wait_id == 0) return;
	for (i = 0; i < MS_CONTRACT_CAP; i++) {
		c = &g_contracts[i];
		if (c->kind != MS_CONTRACT_MODULE_WAIT || c->wait_id != wait_id)
			continue;
		c->last_event = (unsigned char)event_type;
		if (event_type == MS_MOD_CALLBACK_BEGIN) {
			c->state = MS_CONTRACT_FIRED;
			c->expected = MS_EXPECT_CALLBACK_RETURN;
		} else if (event_type == MS_MOD_CALLBACK_RETURN) {
			c->state = MS_CONTRACT_COMPLETED;
			c->expected = MS_EXPECT_NONE;
		} else if (event_type == MS_MOD_FAIL && reason != MS_MOD_REASON_NONE) {
			c->state = MS_CONTRACT_FAILED;
			c->expected = MS_EXPECT_NONE;
		}
	}
}

long macsurf_diag_serialize_pending(char *buf, long cap)
{
	char line[224];
	long n = 0;
	int i;
	if (buf == NULL || cap < 2) return 0;
	buf[0] = '\0';
	n = diag_cat(buf, cap, n, "MSDIAG 1 pending\n");
	for (i = 0; i < MS_CONTRACT_CAP; i++) {
		struct ms_diag_contract *c = &g_contracts[i];
		if (c->id == 0 || !ms_contract_is_unresolved(c)) continue;
		if (c->kind == MS_CONTRACT_OPERATION) {
			snprintf(line, sizeof line,
				"contract=%lu nav=%lu script=%lu task=%lu kind=operation op=%lu op_kind=%s state=%s expected=%s last=%s req=%lu\n",
				c->id, c->nav_id, c->script_id, c->task_id,
				c->operation_id, ms_op_kind_s((int)c->operation_kind),
				ms_contract_state_s(c->state),
				ms_contract_expected_s(c->expected),
				ms_op_phase_s(c->last_event), c->request_id);
		} else if (c->kind == MS_CONTRACT_IO) {
			struct ms_diag_timer *t = ms_timer_find(c->timer_id);
			snprintf(line, sizeof line,
				"contract=%lu nav=%lu script=%lu task=%lu kind=io io=%lu target=%lu state=%s expected=%s last=%s callbacks=%lu timer=%lu timer_state=%s\n",
				c->id, c->nav_id, c->script_id, c->task_id,
				c->io_id, c->target_id,
				ms_contract_state_s(c->state),
				ms_contract_expected_s(c->expected),
				ms_io_event_s(c->last_event), c->callback_count, c->timer_id,
				t == NULL ? "unknown" : ms_timer_state_s(t->state));
		} else {
			snprintf(line, sizeof line,
				"contract=%lu nav=%lu script=%lu task=%lu kind=module_wait wait=%lu mod=%lu state=%s expected=%s last=%s eligible=%d\n",
				c->id, c->nav_id, c->script_id, c->task_id,
				c->wait_id, c->module_id,
				ms_contract_state_s(c->state),
				ms_contract_expected_s(c->expected),
				ms_mod_event_s(c->last_event), (int)c->eligible);
		}
		n = diag_cat(buf, cap, n, line);
	}
	snprintf(line, sizeof line, "dropped_waiting=%lu\n",
		(unsigned long)g_contract_dropped_waiting);
	n = diag_cat(buf, cap, n, line);
	return n;
}

long macsurf_diag_serialize_timers(char *buf, long cap)
{
	char line[192];
	long n = 0;
	int i;
	if (buf == NULL || cap < 2) return 0;
	buf[0] = '\0';
	n = diag_cat(buf, cap, n, "MSDIAG 1 timers\n");
	for (i = 0; i < MS_TIMER_DIAG_CAP; i++) {
		struct ms_diag_timer *t = &g_timers[i];
		if (t->id == 0 || t->io_refs == 0) continue;
		snprintf(line, sizeof line,
			"timer=%lu nav=%lu origin_script=%lu origin_task=%lu ctx_gen=%lu state=%s io_refs=%u\n",
			t->id, t->nav_id, t->script_id, t->task_id, t->ctx_gen,
			ms_timer_state_s(t->state), (unsigned)t->io_refs);
		n = diag_cat(buf, cap, n, line);
	}
	return n;
}

long macsurf_diag_serialize_settlement(char *buf, long cap)
{
	char line[160];
	long n = 0;
	unsigned long unresolved = 0;
	int i;
	if (buf == NULL || cap < 2) return 0;
	for (i = 0; i < MS_CONTRACT_CAP; i++)
		if (g_contracts[i].id != 0 && ms_contract_is_unresolved(&g_contracts[i]))
			unresolved++;
	buf[0] = '\0';
	n = diag_cat(buf, cap, n, "MSDIAG 1 settlement\n");
	n = diag_cat(buf, cap, n, "network_idle=unknown\nengine_idle=unknown\nrender_idle=unknown\n");
	snprintf(line, sizeof line, "known_unresolved=%lu\ndropped_waiting=%lu\n",
		unresolved, (unsigned long)g_contract_dropped_waiting);
	n = diag_cat(buf, cap, n, line);
	if (unresolved != 0) {
		n = diag_cat(buf, cap, n, "settled=0\nreason=unresolved_contracts\n");
	} else if (g_contract_dropped_waiting != 0) {
		n = diag_cat(buf, cap, n, "settled=0\nreason=contract_capacity\n");
	} else {
		n = diag_cat(buf, cap, n, "settled=1\nreason=no_known_unresolved_contracts\n");
	}
	n = diag_cat(buf, cap, n, "scope=known_browser_contracts\npage_complete=unknown\n");
	return n;
}

/* ================= Browser API operation / error diagnostics =================
 * These records intentionally sit before the fetch request ring.  A declined
 * XHR must remain visible even when no struct fetch (and therefore no req id)
 * was ever created. */
#define MS_OP_RING_CAP 256
#define MS_ERR_RING_CAP 128
#define MS_ERR_NAME_CAP 64
#define MS_ERR_MSG_CAP 64
#define MS_ERR_TEXT_MAX 120

struct ms_diag_operation {
	unsigned long id, nav_id, script_id, task_id, request_id;
	unsigned char kind, phase, result, reason, quality;
};
struct ms_diag_error_text {
	unsigned long id;
	char text[MS_ERR_TEXT_MAX];
};
struct ms_diag_error {
	unsigned long id, nav_id, script_id, task_id, op_id, request_id;
	unsigned long name_id, message_id;
	unsigned char kind, boundary, reason;
};

static struct ms_diag_operation g_op_ring[MS_OP_RING_CAP];
static int g_op_ring_head;
static unsigned long g_op_seq;
static struct ms_diag_error_text g_err_names[MS_ERR_NAME_CAP];
static struct ms_diag_error_text g_err_messages[MS_ERR_MSG_CAP];
static int g_err_name_count, g_err_message_count;
static unsigned long g_err_name_seq, g_err_message_seq;
static struct ms_diag_error g_err_ring[MS_ERR_RING_CAP];
static int g_err_ring_head;
static unsigned long g_err_seq;

static const char *ms_op_kind_s(int v)
{
	switch (v) {
	case MS_OP_FETCH: return "fetch";
	case MS_OP_XHR: return "xhr";
	case MS_OP_BEACON: return "beacon";
	default: return "unknown";
	}
}
static const char *ms_op_phase_s(int v)
{
	switch (v) {
	case MS_OP_ATTEMPT: return "attempt";
	case MS_OP_OPEN: return "open";
	case MS_OP_SEND_ATTEMPT: return "send";
	case MS_OP_NATIVE_ALLOC: return "native_alloc";
	case MS_OP_WIRE_START: return "wire_start";
	case MS_OP_ABORT: return "abort";
	case MS_OP_DELIVER: return "deliver";
	case MS_OP_SETTLE: return "settle";
	case MS_OP_NATIVE_EVENT: return "native_event";
	default: return "unknown";
	}
}
static const char *ms_op_result_s(int v)
{
	switch (v) {
	case MS_OP_PENDING: return "pending";
	case MS_OP_OK: return "ok";
	case MS_OP_DECLINE: return "decline";
	case MS_OP_RESOLVE: return "resolve";
	case MS_OP_REJECT: return "reject";
	case MS_OP_IGNORED: return "ignored";
	default: return "unknown";
	}
}
static const char *ms_op_reason_s(int v)
{
	switch (v) {
	case MS_OPR_NONE: return "none";
	case MS_OPR_PRE_ABORTED: return "pre_aborted";
	case MS_OPR_BAD_URL: return "bad_url";
	case MS_OPR_NO_BASE: return "no_base";
	case MS_OPR_ARENA_FULL: return "arena_full";
	case MS_OPR_BODY_ALLOC: return "body_alloc";
	case MS_OPR_HEADER_LIMIT: return "header_limit";
	case MS_OPR_FETCH_START_FAIL: return "fetch_start_fail";
	case MS_OPR_NETWORK_ERROR: return "network_error";
	case MS_OPR_RESPONSE_POISONED: return "response_poisoned";
	case MS_OPR_REDIRECT_LIMIT: return "redirect_limit";
	case MS_OPR_REDIRECT_DOWNGRADE: return "redirect_downgrade";
	case MS_OPR_ABORTED: return "aborted";
	case MS_OPR_REALM_GONE: return "realm_gone";
	case MS_OPR_TIMEOUT: return "timeout";
	case MS_OPR_AUTH: return "auth";
	case MS_OPR_CERT: return "cert";
	case MS_OPR_SSL_ERROR: return "ssl_error";
	case MS_OPR_NOT_MODIFIED: return "not_modified";
	default: return "unknown";
	}
}
static const char *ms_answer_quality_s(int v)
{
	switch (v) {
	case MS_ANSWER_NATIVE: return "authoritative";
	case MS_ANSWER_APPROX: return "approximate";
	case MS_ANSWER_FALLBACK: return "fallback";
	case MS_ANSWER_UNSUPPORTED: return "unsupported";
	default: return "unknown";
	}
}
static const char *ms_err_kind_s(int v)
{
	switch (v) {
	case MS_ERR_JS_EXCEPTION: return "js_exception";
	case MS_ERR_API_DECLINE: return "api_decline";
	case MS_ERR_PROMISE_REJECTION: return "promise_rejection";
	case MS_ERR_CALLBACK_FAILURE: return "callback_failure";
	default: return "unknown";
	}
}

unsigned long ms_diag_operation_begin(int kind, int quality)
{
	unsigned long id = ++g_op_seq;
	ms_diag_operation_record(id, kind, MS_OP_ATTEMPT, MS_OP_PENDING,
		MS_OPR_NONE, quality, 0);
	return id;
}

void ms_diag_operation_record(unsigned long op_id, int kind, int phase,
	int result, int reason, int quality, unsigned long request_id)
{
	struct ms_diag_operation *e;
	if (op_id == 0) return;
	e = &g_op_ring[g_op_ring_head];
	g_op_ring_head = (g_op_ring_head + 1) % MS_OP_RING_CAP;
	e->id = op_id;
	e->nav_id = ms_diag_cur_nav();
	if (e->nav_id == 0) e->nav_id = g_diag_last_nav;
	e->script_id = ms_diag_cur_script();
	e->task_id = ms_diag_cur_task();
	e->request_id = request_id;
	e->kind = (unsigned char)kind;
	e->phase = (unsigned char)phase;
	e->result = (unsigned char)result;
	e->reason = (unsigned char)reason;
	e->quality = (unsigned char)quality;
	ms_contract_operation_event(op_id, kind, phase, result, request_id);
}

static unsigned long ms_diag_error_text_id(struct ms_diag_error_text *table,
	int *count, int cap, unsigned long *seq, const char *text)
{
	int i;
	if (text == NULL || text[0] == '\0') return 0;
	for (i = 0; i < *count; i++) {
		if (strncmp(table[i].text, text, MS_ERR_TEXT_MAX - 1) == 0)
			return table[i].id;
	}
	if (*count >= cap) return 0;
	table[*count].id = ++*seq;
	ms_name_copy(table[*count].text, text);
	(*count)++;
	return table[*count - 1].id;
}

void ms_diag_error_record(unsigned long op_id, unsigned long request_id,
	int kind, int boundary, int reason, const char *name, const char *message)
{
	struct ms_diag_error *e = &g_err_ring[g_err_ring_head];
	g_err_ring_head = (g_err_ring_head + 1) % MS_ERR_RING_CAP;
	e->id = ++g_err_seq;
	e->nav_id = ms_diag_cur_nav();
	if (e->nav_id == 0) e->nav_id = g_diag_last_nav;
	e->script_id = ms_diag_cur_script();
	e->task_id = ms_diag_cur_task();
	e->op_id = op_id;
	e->request_id = request_id;
	e->name_id = ms_diag_error_text_id(g_err_names, &g_err_name_count,
		MS_ERR_NAME_CAP, &g_err_name_seq, name);
	e->message_id = ms_diag_error_text_id(g_err_messages, &g_err_message_count,
		MS_ERR_MSG_CAP, &g_err_message_seq, message);
	e->kind = (unsigned char)kind;
	e->boundary = (unsigned char)boundary;
	e->reason = (unsigned char)reason;
}

long macsurf_diag_serialize_operations(char *buf, long cap)
{
	char line[192]; long n = 0; int i;
	if (buf == NULL || cap < 2) return 0;
	buf[0] = '\0';
	n = diag_cat(buf, cap, n, "MSDIAG 1 operations\n");
	for (i = 0; i < MS_OP_RING_CAP; i++) {
		int idx = (g_op_ring_head - 1 - i + 2 * MS_OP_RING_CAP) % MS_OP_RING_CAP;
		struct ms_diag_operation *e = &g_op_ring[idx];
		if (e->id == 0) continue;
		snprintf(line, sizeof line,
			"op=%lu nav=%lu script=%lu task=%lu kind=%s phase=%s result=%s reason=%s quality=%s req=%lu\n",
			e->id, e->nav_id, e->script_id, e->task_id,
			ms_op_kind_s(e->kind), ms_op_phase_s(e->phase),
			ms_op_result_s(e->result), ms_op_reason_s(e->reason),
			ms_answer_quality_s(e->quality), e->request_id);
		n = diag_cat(buf, cap, n, line);
	}
	return n;
}

long macsurf_diag_serialize_errors(char *buf, long cap)
{
	char line[192]; long n = 0; int i;
	if (buf == NULL || cap < 2) return 0;
	buf[0] = '\0';
	n = diag_cat(buf, cap, n, "MSDIAG 1 errors\n");
	for (i = 0; i < g_err_name_count; i++) {
		snprintf(line, sizeof line, "name=%lu text=%s\n",
			g_err_names[i].id, g_err_names[i].text);
		n = diag_cat(buf, cap, n, line);
	}
	for (i = 0; i < g_err_message_count; i++) {
		snprintf(line, sizeof line, "message=%lu text=%s\n",
			g_err_messages[i].id, g_err_messages[i].text);
		n = diag_cat(buf, cap, n, line);
	}
	for (i = 0; i < MS_ERR_RING_CAP; i++) {
		int idx = (g_err_ring_head - 1 - i + 2 * MS_ERR_RING_CAP) % MS_ERR_RING_CAP;
		struct ms_diag_error *e = &g_err_ring[idx];
		if (e->id == 0) continue;
		snprintf(line, sizeof line,
			"err=%lu nav=%lu script=%lu task=%lu kind=%s boundary=%d reason=%s op=%lu req=%lu name=%lu message=%lu\n",
			e->id, e->nav_id, e->script_id, e->task_id,
			ms_err_kind_s(e->kind), (int)e->boundary, ms_op_reason_s(e->reason),
			e->op_id, e->request_id, e->name_id, e->message_id);
		n = diag_cat(buf, cap, n, line);
	}
	return n;
}
