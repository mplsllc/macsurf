/*
 * MacSurf  -  macsurf_diag.c   (MacSurf Trace diagnostic state boundary)
 * See macsurf_diag.h.
 */

#include <stdio.h>	/* snprintf -- real MSL on the Mac build, not the
			 * hand-rolled macsurf_debug_log_writef formatter */
#include <string.h>

#include "content/fetch.h"	/* fetch_get_{nav_id,request_id,redirect_from} */
#include "nsutils/time.h"
#include "utils/nsoption.h"
#include "utils/corestrings.h"
#include "macos9_content_registry.h"
#include "html/box.h"
#include "html/private.h"
#include "html/html.h"

#ifdef __MACOS9__
#include "macos9.h"		/* struct gui_window -- Mac-only full definition */
#include "desktop/browser_private.h"	/* struct browser_window fields */
#include "content/hlcache.h"	/* hlcache_handle_get_content */
#endif

#include "macsurf_diag.h"
#include "macsurf_gap.h"
#include "macsurf_trace.h"	/* Milestone 1c: mirror lifecycle to the ring */
#include "macsurf_qjs.h"

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
static unsigned long g_readiness_epoch;

/* Readiness is derived from progress MacSurf can actually observe.  It does
 * not guess application completion from elapsed navigation time.  The low
 * 32 bits of the monotonic millisecond clock are sufficient: unsigned
 * subtraction keeps elapsed intervals correct across a wrap. */
enum ms_progress_kind {
	MS_PROGRESS_NETWORK = 0, MS_PROGRESS_SCRIPT, MS_PROGRESS_TASK,
	MS_PROGRESS_MUTATION, MS_PROGRESS_LAYOUT, MS_PROGRESS_PAINT,
	MS_PROGRESS_TIMER, MS_PROGRESS_IO, MS_PROGRESS_MODULE,
	MS_PROGRESS_CONTRACT
};
struct ms_diag_progress {
	unsigned long seq, last_ms;
	unsigned long network, tasks, mutations, layout, paint;
};
static struct ms_diag_progress g_progress;
struct ms_diag_readiness_sample {
	unsigned long network, tasks, mutations, layout, paint;
	int valid;
};
static struct ms_diag_readiness_sample g_readiness_sample;

static unsigned long ms_diag_progress_now(void)
{
	nsutils_ms_t now = 0;
	(void)nsu_getmonotonic_ms(&now);
	return (unsigned long)now;
}

static void ms_diag_progress(int kind)
{
	g_progress.seq++;
	if (g_progress.seq == 0) g_progress.seq = 1;
	g_progress.last_ms = ms_diag_progress_now();
	if (kind == MS_PROGRESS_NETWORK) g_progress.network++;
	else if (kind == MS_PROGRESS_TASK) g_progress.tasks++;
	else if (kind == MS_PROGRESS_MUTATION) g_progress.mutations++;
	else if (kind == MS_PROGRESS_LAYOUT) g_progress.layout++;
	else if (kind == MS_PROGRESS_PAINT) g_progress.paint++;
}

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
	ms_diag_progress(MS_PROGRESS_NETWORK);

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
	ms_diag_progress(MS_PROGRESS_NETWORK);
}

void macsurf_diag_navigation_begin(void)
{
	g_readiness_epoch++;
	if (g_readiness_epoch == 0) g_readiness_epoch = 1;
	memset(&g_readiness_sample, 0, sizeof(g_readiness_sample));
	ms_diag_progress(MS_PROGRESS_NETWORK);
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

static const char *ms_diag_realm_state_name(int state)
{
	if (state == MS_REALM_LIVE) return "live";
	if (state == MS_REALM_NAVIGATION_REQUESTED) return "navigation_requested";
	if (state == MS_REALM_TEARING_DOWN) return "tearing_down";
	if (state == MS_REALM_RETIRED) return "retired";
	return "unknown";
}

static void ms_diag_realm_count_text(char *buf, size_t cap,
		unsigned long value)
{
	if (value == QJS_REALM_DIAG_UNAVAILABLE)
		snprintf(buf, cap, "unavailable");
	else
		snprintf(buf, cap, "%lu", value);
}

long macsurf_diag_serialize_realms(char *buf, long cap)
{
	char line[512];
	char timers[24], xhr[24], microtasks[24], modules[24];
	char listeners[24], wrappers[24], deferred[24];
	long n = 0;
	int total;
	int emitted = 0;
	int truncated = 0;
	int i;
	unsigned long retired_total = 0;
	unsigned long retired_cap = 0;

	if (buf == NULL || cap < 2) return 0;
	buf[0] = '\0';
	n = diag_cat(buf, cap, n, "MSDIAG 1 realms\n");
	total = macsurf_qjs_realm_count();
	retired_total = macsurf_qjs_realm_retired_total();
	retired_cap = macsurf_qjs_realm_retired_capacity();
	snprintf(line, sizeof(line), "records_total=%d\nretired_capacity=%lu\nretired_total=%lu\nretired_overwritten=%lu\n",
		total, retired_cap, retired_total,
		(retired_total > retired_cap) ? retired_total - retired_cap : 0);
	n = diag_cat(buf, cap, n, line);
	for (i = 0; i < total; i++) {
		struct qjs_realm_diag realm;
		if (!macsurf_qjs_realm_get(i, &realm)) continue;
		ms_diag_realm_count_text(timers, sizeof(timers), realm.timers_owned);
		ms_diag_realm_count_text(xhr, sizeof(xhr), realm.xhr_owned);
		ms_diag_realm_count_text(microtasks, sizeof(microtasks), realm.microtasks_pending);
		ms_diag_realm_count_text(modules, sizeof(modules), realm.modules_waiting);
		ms_diag_realm_count_text(listeners, sizeof(listeners), realm.event_listeners);
		ms_diag_realm_count_text(wrappers, sizeof(wrappers), realm.wrappers);
		ms_diag_realm_count_text(deferred, sizeof(deferred), realm.deferred_notifications);
		snprintf(line, sizeof(line),
			"realm=%lu state=%s frame=%lu doc=%lu nav=%lu heap=%lu ctx_gen=%lu ctx=%p rt=%p content=%p document=%p timers=%s xhr=%s microtasks=%s modules_waiting=%s event_listeners=%s wrappers=%s deferred_notifications=%s\n",
			realm.realm_id, ms_diag_realm_state_name(realm.state),
			realm.frame_id, realm.document_id, realm.nav_id, realm.heap_id,
			realm.ctx_gen, (void *)realm.ctx, (void *)realm.rt,
			(void *)realm.content, realm.document, timers, xhr, microtasks,
			modules, listeners, wrappers, deferred);
		/* Reserve enough room for an unambiguous footer. A reply never
		 * contains a partial realm record: omitted records are explicit. */
		if (n + (long)strlen(line) + 64 >= cap) {
			truncated = 1;
			continue;
		}
		n = diag_cat(buf, cap, n, line);
		emitted++;
	}
	snprintf(line, sizeof(line), "records_emitted=%d\ntruncated=%d\ncomplete=%d\n",
		emitted, truncated, emitted == total && !truncated);
	n = diag_cat(buf, cap, n, line);
	return n;
}

/* ===================== Realm lifetime invariant events ==================== */
#define MS_REALM_INVARIANT_RING_N 64
struct ms_realm_invariant_event {
	unsigned long id, realm_id, frame_id, queued_doc, live_doc;
	unsigned long queued_nav, live_nav, heap_id, ctx_gen, work_id;
	unsigned char kind, state;
};
static struct ms_realm_invariant_event
	g_realm_invariant_ring[MS_REALM_INVARIANT_RING_N];
static unsigned long g_realm_invariant_total;
static int g_realm_invariant_head;

void ms_diag_realm_invariant_record(int kind, int state,
	unsigned long realm_id, unsigned long frame_id,
	unsigned long queued_doc, unsigned long live_doc,
	unsigned long queued_nav, unsigned long live_nav,
	unsigned long heap_id, unsigned long ctx_gen, unsigned long work_id)
{
	struct ms_realm_invariant_event *e =
		&g_realm_invariant_ring[g_realm_invariant_head];
	memset(e, 0, sizeof(*e));
	e->id = ++g_realm_invariant_total;
	if (e->id == 0) e->id = ++g_realm_invariant_total;
	e->kind = (unsigned char)kind;
	e->state = (unsigned char)state;
	e->realm_id = realm_id; e->frame_id = frame_id;
	e->queued_doc = queued_doc; e->live_doc = live_doc;
	e->queued_nav = queued_nav; e->live_nav = live_nav;
	e->heap_id = heap_id; e->ctx_gen = ctx_gen; e->work_id = work_id;
	g_realm_invariant_head = (g_realm_invariant_head + 1) %
		MS_REALM_INVARIANT_RING_N;
}

static const char *ms_realm_invariant_kind_name(int kind)
{
	if (kind == MS_RI_DEFERRED_CALLBACK) return "DEFERRED_CALLBACK";
	if (kind == MS_RI_REALM_CTX_NOT_REGISTERED) return "REALM_CTX_NOT_REGISTERED";
	if (kind == MS_RI_CALLBACK_DOC_GENERATION_MISMATCH) return "CALLBACK_DOC_GENERATION_MISMATCH";
	if (kind == MS_RI_TIMER_REALM_OWNER_MISMATCH) return "TIMER_REALM_OWNER_MISMATCH";
	if (kind == MS_RI_PENDING_WORK_ON_RETIRED_REALM) return "PENDING_WORK_ON_RETIRED_REALM";
	if (kind == MS_RI_REALM_RUNTIME_MISMATCH) return "REALM_RUNTIME_MISMATCH";
	return "unknown";
}
static const char *ms_realm_invariant_state_name(int state)
{
	if (state == MS_RIS_QUEUED) return "queued_for_live_matching_realm";
	if (state == MS_RIS_DELIVERED) return "delivered";
	if (state == MS_RIS_CANCELLED_DOCUMENT_DESTROYED) return "cancelled_document_destroyed";
	if (state == MS_RIS_CANCELLED_NAVIGATION_REPLACED) return "cancelled_navigation_replaced";
	if (state == MS_RIS_CANCELLED_REALM_RETIRED) return "cancelled_realm_retired";
	if (state == MS_RIS_REJECTED_CTX_GENERATION_MISMATCH) return "rejected_ctx_generation_mismatch";
	if (state == MS_RIS_REJECTED_RUNTIME_REALM_MISMATCH) return "rejected_runtime_realm_mismatch";
	if (state == MS_RIS_CALLBACK_INVALIDATED_DOCUMENT) return "callback_invalidated_document";
	if (state == MS_RIS_CALLBACK_INVALIDATED_REALM) return "callback_invalidated_realm";
	if (state == MS_RIS_CALLBACK_REQUESTED_NAVIGATION) return "callback_requested_navigation";
	return "unknown";
}

long macsurf_diag_serialize_warnings(char *buf, long cap)
{
	long n = 0;
	unsigned long used = g_realm_invariant_total;
	unsigned long first, i;
	char line[320];
	if (buf == NULL || cap < 2) return 0;
	if (used > MS_REALM_INVARIANT_RING_N) used = MS_REALM_INVARIANT_RING_N;
	buf[0] = '\0';
	n = diag_cat(buf, cap, n, "MSDIAG 1 warnings\n");
	snprintf(line, sizeof(line), "capacity=%d\nused=%lu\ntotal=%lu\ndropped=%lu\n",
		MS_REALM_INVARIANT_RING_N, used, g_realm_invariant_total,
		g_realm_invariant_total > MS_REALM_INVARIANT_RING_N ?
		g_realm_invariant_total - MS_REALM_INVARIANT_RING_N : 0UL);
	n = diag_cat(buf, cap, n, line);
	/* These are deliberately explicit rather than synthetic zero-warning
	 * answers.  The current owners do not retain the linkage needed to prove
	 * them at a native boundary. */
	n = diag_cat(buf, cap, n,
		"thread_heap_ctx_mismatch=unavailable missing=thread_heap_owner\n"
		"wrapper_runtime_owner_mismatch=unavailable missing=wrapper_realm_id\n"
		"xhr_realm_owner_mismatch=unavailable missing=xhr_runtime_snapshot\n"
		"observer_document_owner_mismatch=unavailable missing=observer_document_id\n"
		"microtask_runtime_owner_mismatch=unavailable missing=job_realm_identity\n");
	first = g_realm_invariant_total > MS_REALM_INVARIANT_RING_N ?
		(unsigned long)g_realm_invariant_head : 0;
	for (i = 0; i < used; i++) {
		struct ms_realm_invariant_event *e =
			&g_realm_invariant_ring[(first + i) % MS_REALM_INVARIANT_RING_N];
		snprintf(line, sizeof(line), "id=%lu kind=%s state=%s realm=%lu frame=%lu queued_doc=%lu live_doc=%lu queued_nav=%lu live_nav=%lu heap=%lu ctx_gen=%lu work=%lu count=1\n",
			e->id, ms_realm_invariant_kind_name(e->kind),
			ms_realm_invariant_state_name(e->state), e->realm_id, e->frame_id,
			e->queued_doc, e->live_doc, e->queued_nav, e->live_nav,
			e->heap_id, e->ctx_gen, e->work_id);
		if (n + (long)strlen(line) + 32 >= cap) break;
		n = diag_cat(buf, cap, n, line);
	}
	snprintf(line, sizeof(line), "complete=%d\n", i == used);
	return diag_cat(buf, cap, n, line);
}

/* ======================= Phase 1b: script / task ======================= */

static unsigned long g_cur_script;
static unsigned long g_cur_task;
static unsigned long g_cur_nav;
static unsigned long g_script_seq;
static unsigned long g_task_seq;

#define MS_SCRIPT_RING_N 64
#define MS_SCRIPT_DEFAULT_LIMIT 16
#define MS_SCRIPT_FOOTER_RESERVE 256
#define MS_TASK_RING_N   128
#define MS_NAME_MAX      40

static long ms_diag_history_header(char *buf, long cap, long n,
	const char *history, unsigned long total, int capacity);

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
	ms_diag_progress(MS_PROGRESS_SCRIPT);
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
	ms_diag_progress(MS_PROGRESS_SCRIPT);
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
	ms_diag_progress(MS_PROGRESS_TASK);
	return s->my_id;
}

void ms_diag_task_leave(struct ms_diag_scope *s)
{
	g_cur_script = s->prev_script;
	g_cur_task = s->prev_task;
	if (s->my_id != 0) ms_diag_progress(MS_PROGRESS_TASK);
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
			if (jobs != 0) ms_diag_progress(MS_PROGRESS_TASK);
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
	n = ms_diag_history_header(buf, cap, n, "scripts", g_script_seq,
		MS_SCRIPT_RING_N);
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

/* The v1 dump above stays for existing tools.  New tools drain this cursor
 * view in increasing execution-attempt order.  The script ring is live rather
 * than snapshotted: a page reports an overwritten requested interval instead
 * of pretending its retained tail began at the caller's cursor. */
long macsurf_diag_serialize_scripts_since(char *buf, long cap,
	unsigned long after, unsigned long limit)
{
	char line[192];
	long n = 0;
	unsigned long latest;
	unsigned long retained;
	unsigned long first;
	unsigned long want = 0;
	unsigned long seq;
	unsigned long max_return = 0;
	unsigned long next_after;
	unsigned long returned = 0;
	unsigned long lost_from = 0;
	unsigned long lost_to = 0;
	int oldest_idx;
	int have_wanted = 0;
	int lost = 0;
	int truncated = 0;

	if (buf == NULL || cap < 2) return 0;
	buf[0] = '\0';
	latest = g_script_seq;
	retained = latest;
	if (retained > MS_SCRIPT_RING_N) retained = MS_SCRIPT_RING_N;
	first = retained == 0 ? 0 : latest - retained + 1;
	if (limit == 0) limit = MS_SCRIPT_DEFAULT_LIMIT;
	if (limit > MS_SCRIPT_RING_N) limit = MS_SCRIPT_RING_N;

	n = diag_cat(buf, cap, n, "MSDIAG 2 scripts\n");
	n = ms_diag_history_header(buf, cap, n, "scripts", latest,
		MS_SCRIPT_RING_N);
	snprintf(line, sizeof(line), "requested_after=%lu\nlimit=%lu\n",
		after, limit);
	n = diag_cat(buf, cap, n, line);

	if (after < latest) {
		want = after + 1;
		have_wanted = 1;
	}
	if (have_wanted && first != 0 && want < first) {
		lost = 1;
		lost_from = want;
		lost_to = first - 1;
		snprintf(line, sizeof(line), "lost_from=%lu\nlost_to=%lu\n",
			lost_from, lost_to);
		n = diag_cat(buf, cap, n, line);
		want = first;
	}
	next_after = after;
	if (have_wanted && first != 0 && want <= latest) {
		max_return = want + limit;
		if (max_return < want || max_return > latest + 1) {
			max_return = latest + 1;
		}
		oldest_idx = (g_script_ring_head - (int)retained +
			2 * MS_SCRIPT_RING_N) % MS_SCRIPT_RING_N;
		for (seq = want; seq < max_return; seq++) {
			int idx = (oldest_idx + (int)(seq - first)) % MS_SCRIPT_RING_N;
			struct ms_diag_script *e = &g_script_ring[idx];

			if (e->id != seq) {
				lost = 1;
				if (lost_from == 0) {
					lost_from = seq;
					lost_to = seq;
					snprintf(line, sizeof(line),
						"lost_from=%lu\nlost_to=%lu\n", lost_from,
						lost_to);
					n = diag_cat(buf, cap, n, line);
				}
				break;
			}
			snprintf(line, sizeof(line),
				"script=%lu nav=%lu kind=%s state=%s name=%s\n",
				e->id, e->nav_id, ms_script_kind_s(e->kind),
				ms_script_state_s(e->state),
				e->name[0] ? e->name : "-");
			if (n + (long)strlen(line) >= cap - MS_SCRIPT_FOOTER_RESERVE) {
				truncated = 1;
				break;
			}
			n = diag_cat(buf, cap, n, line);
			returned++;
			next_after = seq;
		}
	}
	snprintf(line, sizeof(line), "lost=%d\nreturned=%lu\nnext_after=%lu\n"
		"complete=%d\ntruncated=%d\n", lost, returned, next_after,
		next_after >= latest ? 1 : 0, truncated);
	n = diag_cat(buf, cap, n, line);
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

/* All causal-context rings use this same retention contract.  Their ids are
 * monotonically allocated write generations, so a reader need not guess from
 * an empty slot whether nothing was recorded or older history was displaced.
 * `first_available=0` means no record has ever existed; otherwise every id
 * before first_available has been overwritten by the bounded ring. */
static long ms_diag_history_header(char *buf, long cap, long n,
	const char *history, unsigned long total, int capacity)
{
	unsigned long first = 0;
	unsigned long overwritten = 0;

	if (total != 0) {
		if (total > (unsigned long) capacity) {
			overwritten = total - (unsigned long) capacity;
			first = overwritten + 1;
		} else {
			first = 1;
		}
	}
	{
		char line[160];
		snprintf(line, sizeof(line),
			"history=%s records_total=%lu capacity=%d first_available=%lu latest=%lu overwritten=%lu\n",
			history, total, capacity, first, total, overwritten);
		n = diag_cat(buf, cap, n, line);
	}
	return n;
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
	ms_diag_progress(MS_PROGRESS_MUTATION);
}

void ms_diag_batch_freeze(unsigned long batch_id)
{
	struct ms_diag_mut_batch *e = ms_batch_find(batch_id);
	if (e != (struct ms_diag_mut_batch *) 0) {
		e->frozen = 1;
		macsurf_trace_emit(MS_TC_MUTATION, MS_TE_MUTATION_FREEZE, 0, 0,
			batch_id, e->total);
		ms_diag_progress(MS_PROGRESS_MUTATION);
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
	ms_diag_progress(MS_PROGRESS_LAYOUT);
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
	ms_diag_progress(MS_PROGRESS_LAYOUT);
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
				ms_diag_progress(MS_PROGRESS_PAINT);
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
	ms_diag_progress(MS_PROGRESS_PAINT);
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
	n = ms_diag_history_header(buf, cap, n, "documents", g_doc_seq,
		MS_DOC_RING_N);
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
	n = ms_diag_history_header(buf, cap, n, "mutations", g_batch_seq,
		MS_BATCH_RING_N);
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
	n = ms_diag_history_header(buf, cap, n, "passes", g_pass_seq,
		MS_PASS_RING_N);
	n = ms_diag_history_header(buf, cap, n, "stages", g_stage_seq,
		MS_STAGE_RING_N);

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
	ms_diag_progress(MS_PROGRESS_MODULE);
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
	ms_diag_progress(MS_PROGRESS_IO);
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
	unsigned long id, nav_id, script_id, task_id, readiness_epoch;
	unsigned long operation_id, request_id;
	unsigned long io_id, target_id, timer_id;
	unsigned long module_id, wait_id;
	unsigned long callback_count;
	unsigned char kind, operation_kind, state, expected, last_event, eligible;
	unsigned char timer_expected, timer_native_state;
};

static struct ms_diag_contract g_contracts[MS_CONTRACT_CAP];
static int g_contract_head;
static unsigned long g_contract_seq;
static unsigned long g_contract_dropped_waiting;
static unsigned long g_contract_dropped_io;
static unsigned long g_contract_dropped_module_wait;
static unsigned long g_contract_dropped_network;

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

static const char *ms_io_timer_native_state_s(int state)
{
	switch (state) {
	case MS_IO_TIMER_NATIVE_ENTERED: return "entered";
	case MS_IO_TIMER_NATIVE_BAD_CALLBACK: return "rejected_bad_callback";
	case MS_IO_TIMER_NATIVE_NO_SLOT: return "rejected_no_slot";
	case MS_IO_TIMER_NATIVE_ALLOCATED: return "allocated";
	default: return "not_entered";
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
	ms_diag_progress(MS_PROGRESS_TIMER);
}

void ms_diag_timer_state(unsigned long id, int state)
{
	struct ms_diag_timer *t = ms_timer_find(id);
	if (t != NULL) {
		t->state = (unsigned char)state;
		ms_diag_progress(MS_PROGRESS_TIMER);
	}
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
		if (c->kind == MS_CONTRACT_IO) g_contract_dropped_io++;
		else if (c->kind == MS_CONTRACT_MODULE_WAIT)
			g_contract_dropped_module_wait++;
		else if (c->kind == MS_CONTRACT_OPERATION)
			g_contract_dropped_network++;
	}
	memset(c, 0, sizeof(*c));
	c->id = ++g_contract_seq;
	if (c->id == 0) c->id = ++g_contract_seq;
	c->nav_id = ms_diag_cur_nav();
	if (c->nav_id == 0) c->nav_id = g_diag_last_nav;
	c->script_id = ms_diag_cur_script();
	c->task_id = ms_diag_cur_task();
	c->readiness_epoch = g_readiness_epoch;
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

void ms_diag_io_timer_expect(unsigned long io_id, const char *target_name)
{
	struct ms_diag_contract *c;
	unsigned long target_id = ms_diag_io_target_id(target_name);
	if (io_id == 0 || target_id == 0) return;
	c = ms_contract_io_find(io_id, target_id);
	if (c == NULL) return;
	c->timer_expected = 1;
	ms_diag_progress(MS_PROGRESS_CONTRACT);
}

void ms_diag_io_timer_native_state(unsigned long io_id,
	const char *target_name, int state)
{
	struct ms_diag_contract *c;
	unsigned long target_id = ms_diag_io_target_id(target_name);
	if (io_id == 0 || target_id == 0) return;
	c = ms_contract_io_find(io_id, target_id);
	if (c == NULL) return;
	c->timer_native_state = (unsigned char)state;
	if (state == MS_IO_TIMER_NATIVE_BAD_CALLBACK ||
		state == MS_IO_TIMER_NATIVE_NO_SLOT)
		c->timer_expected = 0;
	ms_diag_progress(MS_PROGRESS_CONTRACT);
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
	c->timer_expected = 0;
	c->timer_native_state = MS_IO_TIMER_NATIVE_ALLOCATED;
	t = ms_timer_find(timer_id);
	/* setTimeout normally arms this record before returning its id. Keep the
	 * join useful even if a bounded ordinary-timer record was already rotated:
	 * subsequent due/firing/fired transitions address this recovered id. */
	if (t == NULL) {
		ms_diag_timer_arm(timer_id, c->nav_id, c->script_id, c->task_id, 0);
		t = ms_timer_find(timer_id);
	}
	if (t != NULL && t->io_refs != 65535) t->io_refs++;
	ms_diag_progress(MS_PROGRESS_CONTRACT);
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
			struct ms_diag_timer *t = c->timer_id == 0 ? NULL :
				ms_timer_find(c->timer_id);
			const char *timer_state = t == NULL ?
				(c->timer_expected ? "awaiting_native_allocation" : "unbound") :
				ms_timer_state_s(t->state);
			snprintf(line, sizeof line,
				"contract=%lu nav=%lu script=%lu task=%lu kind=io io=%lu target=%lu state=%s expected=%s last=%s callbacks=%lu timer=%lu timer_state=%s timer_native=%s\n",
				c->id, c->nav_id, c->script_id, c->task_id,
				c->io_id, c->target_id,
				ms_contract_state_s(c->state),
				ms_contract_expected_s(c->expected),
				ms_io_event_s(c->last_event), c->callback_count, c->timer_id,
				timer_state, ms_io_timer_native_state_s(c->timer_native_state));
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
	snprintf(line, sizeof line,
		"dropped_waiting=%lu\ndropped_total=%lu\ndropped_io=%lu\ndropped_module_wait=%lu\ndropped_network=%lu\n",
		(unsigned long)g_contract_dropped_waiting,
		(unsigned long)g_contract_dropped_waiting,
		(unsigned long)g_contract_dropped_io,
		(unsigned long)g_contract_dropped_module_wait,
		(unsigned long)g_contract_dropped_network);
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
	snprintf(line, sizeof line,
		"known_unresolved=%lu\ndropped_waiting=%lu\ndropped_total=%lu\ndropped_io=%lu\ndropped_module_wait=%lu\ndropped_network=%lu\n",
		unresolved, (unsigned long)g_contract_dropped_waiting,
		(unsigned long)g_contract_dropped_waiting,
		(unsigned long)g_contract_dropped_io,
		(unsigned long)g_contract_dropped_module_wait,
		(unsigned long)g_contract_dropped_network);
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

/* Readiness is for the hardware runner.  Unlike the other snapshots it keeps
 * only a tiny poll cursor so the *_recent fields mean "since the previous
 * readiness poll".  It never touches browser, script, or contract state. */
#define MS_READY_QUIESCING_MS 3000UL
#define MS_READY_STALLED_MS 8000UL

static unsigned long ms_readiness_unresolved(int operations_only)
{
	unsigned long count = 0;
	int i;
	for (i = 0; i < MS_CONTRACT_CAP; i++) {
		if (g_contracts[i].id == 0 ||
			g_contracts[i].readiness_epoch != g_readiness_epoch ||
			!ms_contract_is_unresolved(&g_contracts[i])) continue;
		if (!operations_only || g_contracts[i].kind == MS_CONTRACT_OPERATION)
			count++;
	}
	return count;
}

static unsigned long ms_readiness_recent(unsigned long total,
	unsigned long *previous, int valid)
{
	unsigned long result = valid ? total - *previous : 0;
	*previous = total;
	return result;
}

long macsurf_diag_serialize_readiness(char *buf, long cap)
{
	char line[384];
	long n = 0;
	unsigned long now, idle, unresolved, network_active;
	unsigned long network_recent, tasks_recent, mutations_recent;
	unsigned long layout_recent, paint_recent, nav;
	const char *state, *reason;
	int capture_ready = 0, settled = 0;

	if (buf == NULL || cap < 2) return 0;
	now = ms_diag_progress_now();
	idle = (g_progress.seq == 0) ? 0 : now - g_progress.last_ms;
	unresolved = ms_readiness_unresolved(0);
	network_active = ms_readiness_unresolved(1);
	network_recent = ms_readiness_recent(g_progress.network,
		&g_readiness_sample.network, g_readiness_sample.valid);
	tasks_recent = ms_readiness_recent(g_progress.tasks,
		&g_readiness_sample.tasks, g_readiness_sample.valid);
	mutations_recent = ms_readiness_recent(g_progress.mutations,
		&g_readiness_sample.mutations, g_readiness_sample.valid);
	layout_recent = ms_readiness_recent(g_progress.layout,
		&g_readiness_sample.layout, g_readiness_sample.valid);
	paint_recent = ms_readiness_recent(g_progress.paint,
		&g_readiness_sample.paint, g_readiness_sample.valid);
	g_readiness_sample.valid = 1;
	nav = g_cur_nav ? g_cur_nav : g_diag_last_nav;

	if (g_progress.seq == 0) {
		state = "loading";
		reason = "awaiting_progress";
	} else if (idle < MS_READY_QUIESCING_MS) {
		state = "active";
		reason = "progressing";
	} else if (idle < MS_READY_STALLED_MS) {
		state = "quiescing";
		reason = "idle_grace";
	} else if (unresolved != 0) {
		state = "stalled";
		reason = "unresolved_contracts";
		capture_ready = 1;
	} else if (g_contract_dropped_waiting != 0) {
		state = "stalled";
		reason = "contract_capacity";
		capture_ready = 1;
	} else {
		state = "settled";
		reason = "no_known_unresolved_contracts";
		capture_ready = 1;
		settled = 1;
	}

	buf[0] = '\0';
	n = diag_cat(buf, cap, n, "MSDIAG 1 readiness\n");
	snprintf(line, sizeof line,
		"state=%s\nnav=%lu\nrun_epoch=%lu\nprogress_seq=%lu\nlast_progress_ms=%lu\nidle_ms=%lu\nnetwork_active=%lu\nnetwork_recent=%lu\nnetwork_scope=known_browser_operations\ntasks_recent=%lu\nmutations_recent=%lu\nlayout_recent=%lu\npaint_recent=%lu\npending_contracts=%lu\ncapture_ready=%d\nsettled=%d\nreason=%s\nscope=known_browser_progress\n",
		state, nav, g_readiness_epoch, g_progress.seq, g_progress.last_ms, idle,
		network_active, network_recent, tasks_recent, mutations_recent,
		layout_recent, paint_recent, unresolved, capture_ready, settled, reason);
	n = diag_cat(buf, cap, n, line);
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
	ms_diag_progress(MS_PROGRESS_CONTRACT);
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

/* ================== DOM and Box Forensic Entity Graph ================== */

extern long macsurf_free_mem(void);

/* Minimal persistent node user data: holds ONLY node_id */
struct ms_diag_node_info {
	unsigned long node_id;
};

static unsigned long g_diag_node_seq = 0;

/* Visited box hash table for O(1) deduplication during traversal */
struct ms_box_visited_entry {
	struct box *b;
	unsigned long id;
};

struct ms_box_visited_map {
	struct ms_box_visited_entry *table;
	unsigned long capacity;
	unsigned long count;
};

static void ms_box_map_init(struct ms_box_visited_map *map, struct ms_box_visited_entry *storage, unsigned long cap)
{
	map->table = storage;
	map->capacity = cap;
	map->count = 0;
	if (storage != NULL && cap > 0) {
		memset(storage, 0, (size_t)(cap * sizeof(struct ms_box_visited_entry)));
	}
}

static unsigned long ms_box_ptr_hash(const struct box *b, unsigned long cap)
{
	unsigned long v = (unsigned long) b;
	v = ((v >> 4) ^ (v >> 9) ^ (v * 2654435761UL));
	return v & (cap - 1);
}

static unsigned long ms_box_map_get(const struct ms_box_visited_map *map, struct box *b)
{
	unsigned long idx, start;
	if (map->table == NULL || map->capacity == 0 || b == NULL) return 0;
	start = ms_box_ptr_hash(b, map->capacity);
	idx = start;
	while (map->table[idx].b != NULL) {
		if (map->table[idx].b == b) return map->table[idx].id;
		idx = (idx + 1) & (map->capacity - 1);
		if (idx == start) break;
	}
	return 0;
}

static int ms_box_map_put(struct ms_box_visited_map *map, struct box *b, unsigned long id)
{
	unsigned long idx, start;
	if (map->table == NULL || map->capacity == 0 || b == NULL) return 0;
	start = ms_box_ptr_hash(b, map->capacity);
	idx = start;
	while (map->table[idx].b != NULL) {
		if (map->table[idx].b == b) return 1; /* already present */
		idx = (idx + 1) & (map->capacity - 1);
		if (idx == start) return 0; /* full */
	}
	map->table[idx].b = b;
	map->table[idx].id = id;
	map->count++;
	return 1;
}

/* User-data callback strictly adhering to libdom dom_user_data_handler */
static void ms_diag_node_data_handler(dom_node_operation operation,
		dom_string *key, void *data, struct dom_node *src,
		struct dom_node *dst)
{
	(void)key;
	(void)src;
	(void)dst;
	if (operation == DOM_NODE_DELETED) {
		if (data != NULL) {
			free(data);
		}
	}
	/* CLONED and IMPORTED: do nothing; dst gets no copied identity.
	 * ADOPTED and RENAMED: do not allocate a second slot sharing the heap pointer. */
}

/* Ensure a node ID is assigned and attached to the DOM node */
static unsigned long ms_diag_node_id_ensure(dom_node *node)
{
	struct ms_diag_node_info *info = NULL;
	dom_exception exc;
	void *old_data = NULL;

	if (node == NULL) return 0;

	exc = dom_node_get_user_data(node, corestring_dom___ns_key_diag_node_id, (void **) &info);
	if (exc == DOM_NO_ERR && info != NULL) {
		return info->node_id;
	}

	info = (struct ms_diag_node_info *) malloc(sizeof(struct ms_diag_node_info));
	if (info == NULL) return 0;

	g_diag_node_seq++;
	if (g_diag_node_seq == 0) g_diag_node_seq = 1;
	info->node_id = g_diag_node_seq;

	exc = dom_node_set_user_data(node, corestring_dom___ns_key_diag_node_id,
			info, ms_diag_node_data_handler, &old_data);
	if (exc != DOM_NO_ERR) {
		free(info);
		return 0;
	}
	return info->node_id;
}

/* Immutable Snapshot Structures */

struct ms_diag_snapshot_node {
	unsigned long node_id;
	unsigned long parent_node_id;
	unsigned long box_id;
	short node_type;
	char tag[32];
	char class_name[64];
	char node_id_attr[32];
};

struct ms_diag_snapshot_box {
	unsigned long box_id;
	unsigned long parent_box_id;
	unsigned long node_id;
	short type;
	short flags;
	long x, y, width, height;
	unsigned long box_ptr; /* debug token only, never dereferenced */
};

struct ms_diag_snapshot {
	unsigned long doc_id;
	unsigned long frame_id;
	unsigned long nav_id;
	unsigned long content_token;
	unsigned long box_generation;
	struct html_content *htmlc;

	unsigned long node_count;
	struct ms_diag_snapshot_node *nodes;

	unsigned long box_count;
	struct ms_diag_snapshot_box *boxes;

	int valid;
	int live_changed;
};

static struct ms_diag_snapshot g_dom_snapshot;
static int g_dom_capture_in_progress = 0;

static void ms_diag_snapshot_free(void)
{
	if (g_dom_snapshot.nodes != NULL) {
		free(g_dom_snapshot.nodes);
		g_dom_snapshot.nodes = NULL;
	}
	if (g_dom_snapshot.boxes != NULL) {
		free(g_dom_snapshot.boxes);
		g_dom_snapshot.boxes = NULL;
	}
	g_dom_snapshot.node_count = 0;
	g_dom_snapshot.box_count = 0;
	g_dom_snapshot.valid = 0;
	g_dom_snapshot.live_changed = 0;
	g_dom_snapshot.htmlc = NULL;
}

/* Count connected DOM nodes (Pass 1) */
static void ms_diag_count_dom_nodes(dom_node *root, unsigned long *count)
{
	dom_node *cur;
	dom_node *next = NULL;
	dom_exception exc;

	if (root == NULL) return;
	cur = dom_node_ref(root);

	while (cur != NULL) {
		(*count)++;

		exc = dom_node_get_first_child(cur, &next);
		if (exc == DOM_NO_ERR && next != NULL) {
			dom_node_unref(cur);
			cur = next;
			continue;
		}

		exc = dom_node_get_next_sibling(cur, &next);
		if (exc == DOM_NO_ERR && next != NULL) {
			dom_node_unref(cur);
			cur = next;
			continue;
		}

		while (cur != NULL) {
			dom_node *parent = NULL;
			exc = dom_node_get_parent_node(cur, &parent);
			dom_node_unref(cur);
			cur = parent;
			if (cur == NULL || cur == root) {
				if (cur != NULL) dom_node_unref(cur);
				cur = NULL;
				break;
			}
			exc = dom_node_get_next_sibling(cur, &next);
			if (exc == DOM_NO_ERR && next != NULL) {
				dom_node_unref(cur);
				cur = next;
				break;
			}
		}
	}
}

/* Count unique boxes (Pass 1) */
static void ms_diag_count_boxes(struct box *b, struct ms_box_visited_map *map, unsigned long *count)
{
	struct box *fl;

	while (b != NULL) {
		if (ms_box_map_get(map, b) != 0) {
			b = b->next;
			continue;
		}
		(*count)++;
		(void) ms_box_map_put(map, b, *count);

		if (b->list_marker != NULL) {
			ms_diag_count_boxes(b->list_marker, map, count);
		}
		for (fl = b->float_children; fl != NULL; fl = fl->next_float) {
			ms_diag_count_boxes(fl, map, count);
		}
		if (b->children != NULL) {
			ms_diag_count_boxes(b->children, map, count);
		}
		b = b->next;
	}
}

/* Fill DOM nodes (Pass 2) */
static void ms_diag_fill_dom_nodes(dom_node *root, struct ms_diag_snapshot_node *nodes,
		unsigned long *idx, unsigned long max_nodes)
{
	dom_node *cur;
	dom_node *next = NULL;
	dom_exception exc;
	static dom_string *s_id = NULL;
	static dom_string *s_class = NULL;

	if (root == NULL || nodes == NULL) return;
	if (s_id == NULL) (void) dom_string_create((const uint8_t *)"id", 2, &s_id);
	if (s_class == NULL) (void) dom_string_create((const uint8_t *)"class", 5, &s_class);

	cur = dom_node_ref(root);

	while (cur != NULL && *idx < max_nodes) {
		struct ms_diag_snapshot_node *sn = &nodes[*idx];
		dom_node_type ntype = DOM_ELEMENT_NODE;
		dom_string *name = NULL;
		dom_node *parent = NULL;
		struct box *box_for_n = NULL;

		sn->node_id = ms_diag_node_id_ensure(cur);
		(void) dom_node_get_node_type(cur, &ntype);
		sn->node_type = (short) ntype;
		sn->tag[0] = '\0';
		sn->class_name[0] = '\0';
		sn->node_id_attr[0] = '\0';
		sn->parent_node_id = 0;
		sn->box_id = 0;

		exc = dom_node_get_node_name(cur, &name);
		if (exc == DOM_NO_ERR && name != NULL) {
			const char *d = dom_string_data(name);
			if (d != NULL) {
				strncpy(sn->tag, d, sizeof(sn->tag) - 1);
				sn->tag[sizeof(sn->tag) - 1] = '\0';
			}
			dom_string_unref(name);
		}

		if (ntype == DOM_ELEMENT_NODE) {
			dom_string *val = NULL;
			if (s_id != NULL) {
				exc = dom_element_get_attribute((dom_element *)cur, s_id, &val);
				if (exc == DOM_NO_ERR && val != NULL) {
					const char *d = dom_string_data(val);
					if (d != NULL) {
						strncpy(sn->node_id_attr, d, sizeof(sn->node_id_attr) - 1);
						sn->node_id_attr[sizeof(sn->node_id_attr) - 1] = '\0';
					}
					dom_string_unref(val);
				}
			}
			if (s_class != NULL) {
				val = NULL;
				exc = dom_element_get_attribute((dom_element *)cur, s_class, &val);
				if (exc == DOM_NO_ERR && val != NULL) {
					const char *d = dom_string_data(val);
					if (d != NULL) {
						strncpy(sn->class_name, d, sizeof(sn->class_name) - 1);
						sn->class_name[sizeof(sn->class_name) - 1] = '\0';
					}
					dom_string_unref(val);
				}
			}
		}

		exc = dom_node_get_parent_node(cur, &parent);
		if (exc == DOM_NO_ERR && parent != NULL) {
			sn->parent_node_id = ms_diag_node_id_ensure(parent);
			dom_node_unref(parent);
		}

		exc = dom_node_get_user_data(cur, corestring_dom___ns_key_box_node_data, (void **) &box_for_n);
		if (exc == DOM_NO_ERR && box_for_n != NULL) {
			/* Box ID will be resolved after boxes are filled */
		}

		(*idx)++;

		exc = dom_node_get_first_child(cur, &next);
		if (exc == DOM_NO_ERR && next != NULL) {
			dom_node_unref(cur);
			cur = next;
			continue;
		}

		exc = dom_node_get_next_sibling(cur, &next);
		if (exc == DOM_NO_ERR && next != NULL) {
			dom_node_unref(cur);
			cur = next;
			continue;
		}

		while (cur != NULL) {
			parent = NULL;
			exc = dom_node_get_parent_node(cur, &parent);
			dom_node_unref(cur);
			cur = parent;
			if (cur == NULL || cur == root) {
				if (cur != NULL) dom_node_unref(cur);
				cur = NULL;
				break;
			}
			exc = dom_node_get_next_sibling(cur, &next);
			if (exc == DOM_NO_ERR && next != NULL) {
				dom_node_unref(cur);
				cur = next;
				break;
			}
		}
	}
}

/* Fill Boxes (Pass 2) */
static void ms_diag_fill_boxes(struct box *b, struct ms_box_visited_map *map,
		struct ms_diag_snapshot_box *boxes, unsigned long *idx,
		unsigned long max_boxes, unsigned long parent_box_id)
{
	struct box *fl;

	while (b != NULL && *idx < max_boxes) {
		unsigned long my_id = ms_box_map_get(map, b);
		struct ms_diag_snapshot_box *sb;
		if (my_id == 0) {
			b = b->next;
			continue;
		}
		sb = &boxes[*idx];
		sb->box_id = my_id;
		sb->parent_box_id = parent_box_id;
		sb->node_id = (b->node != NULL) ? ms_diag_node_id_ensure(b->node) : 0;
		sb->type = (short) b->type;
		sb->flags = (short) b->flags;
		sb->x = b->x;
		sb->y = b->y;
		sb->width = b->width;
		sb->height = b->height;
		sb->box_ptr = (unsigned long) b;
		(*idx)++;

		if (b->list_marker != NULL) {
			ms_diag_fill_boxes(b->list_marker, map, boxes, idx, max_boxes, my_id);
		}
		for (fl = b->float_children; fl != NULL; fl = fl->next_float) {
			ms_diag_fill_boxes(fl, map, boxes, idx, max_boxes, my_id);
		}
		if (b->children != NULL) {
			ms_diag_fill_boxes(b->children, map, boxes, idx, max_boxes, my_id);
		}
		b = b->next;
	}
}

static unsigned long ms_next_pow2(unsigned long n)
{
	unsigned long p = 16;
	while (p < n && p < 1048576UL) p <<= 1;
	return p;
}

#ifdef __MACOS9__
static struct html_content *ms_diag_find_in_bw(struct browser_window *bw,
	unsigned long doc_id)
{
	struct html_content *found = NULL;
	struct content *c;
	struct html_content *hc;
	int i;

	if (bw == NULL) return NULL;
	if (bw->current_content != NULL) {
		if (content_get_type(bw->current_content) == CONTENT_HTML) {
			c = hlcache_handle_get_content(bw->current_content);
			if (c != NULL) {
				hc = (struct html_content *) c;
				if (hc->doc_id == doc_id)
					return hc;
			}
		}
	}
	if (bw->loading_content != NULL) {
		if (content_get_type(bw->loading_content) == CONTENT_HTML) {
			c = hlcache_handle_get_content(bw->loading_content);
			if (c != NULL) {
				hc = (struct html_content *) c;
				if (hc->doc_id == doc_id)
					return hc;
			}
		}
	}
	if (bw->children != NULL) {
		for (i = 0; i < bw->rows * bw->cols; i++) {
			found = ms_diag_find_in_bw(&bw->children[i], doc_id);
			if (found != NULL) return found;
		}
	}
	if (bw->iframes != NULL) {
		for (i = 0; i < bw->iframe_count; i++) {
			found = ms_diag_find_in_bw(&bw->iframes[i], doc_id);
			if (found != NULL) return found;
		}
	}
	return NULL;
}

static struct html_content *ms_diag_find_by_doc_id(unsigned long doc_id)
{
	struct gui_window *gw;
	struct html_content *found;

	if (doc_id == 0) return NULL;
	for (gw = macos9_window_list_head(); gw != NULL; gw = gw->next) {
		found = ms_diag_find_in_bw(gw->bw, doc_id);
		if (found != NULL)
			return found;
	}
	return NULL;
}
#endif /* __MACOS9__ */

long macsurf_diag_dom_start(unsigned long target_doc, char *buf, long cap)
{
	struct html_content *htmlc = NULL;
	unsigned long node_count = 0;
	unsigned long box_count = 0;
	unsigned long map_cap = 0;
	unsigned long required_bytes = 0;
	long free_mem = 0;
	unsigned long n_idx = 0;
	unsigned long b_idx = 0;
	unsigned long i;
	int reg_cap;
	struct content *reg_c;
	struct html_content *reg_cand;
	struct ms_box_visited_map box_map;
	struct ms_box_visited_entry *map_entries = NULL;
	char line[256];
	long n = 0;

	if (buf == NULL || cap < 2) return 0;
	buf[0] = '\0';

	if (g_dom_capture_in_progress) {
		snprintf(line, sizeof line,
			"MSDIAG 1 domstart complete=0 status=error reason=reentrant\n");
		return diag_cat(buf, cap, 0, line);
	}
	g_dom_capture_in_progress = 1;

	if (target_doc != 0) {
#ifdef __MACOS9__
		htmlc = ms_diag_find_by_doc_id(target_doc);
#endif
	} else {
#ifdef __MACOS9__
		{
			struct content *c;
			struct gui_window *gw = macos9_window_list_head();
			if (gw != NULL && gw->bw != NULL &&
					gw->bw->current_content != NULL &&
					content_get_type(gw->bw->current_content) == CONTENT_HTML) {
				c = hlcache_handle_get_content(
					gw->bw->current_content);
				if (c != NULL)
					htmlc = (struct html_content *) c;
			}
		}
#endif
	}

	if (htmlc == NULL) {
		reg_cap = macos9_content_registry_count();
		for (i = 0; (int)i < reg_cap; i++) {
			reg_c = macos9_content_registry_get((int)i);
			if (reg_c == NULL) continue;
			reg_cand = (struct html_content *)reg_c;
			if (reg_cand->document != NULL &&
			    (target_doc == 0 || reg_cand->doc_id == target_doc)) {
				htmlc = reg_cand;
				break;
			}
		}
	}

	if (htmlc == NULL || htmlc->document == NULL) {
		g_dom_capture_in_progress = 0;
		snprintf(line, sizeof line,
			"MSDIAG 1 domstart complete=0 status=error reason=not_found target_doc=%lu\n",
			target_doc);
		return diag_cat(buf, cap, 0, line);
	}

	/* Atomic Pass 1: count nodes and boxes without yielding */
	ms_diag_count_dom_nodes((dom_node *) htmlc->document, &node_count);

	/* Prepare visited map for box count */
	map_cap = 1024;
	map_entries = (struct ms_box_visited_entry *) malloc(map_cap * sizeof(struct ms_box_visited_entry));
	if (map_entries == NULL) {
		g_dom_capture_in_progress = 0;
		snprintf(line, sizeof line,
			"MSDIAG 1 domstart complete=0 status=error reason=allocation\n");
		return diag_cat(buf, cap, 0, line);
	}
	ms_box_map_init(&box_map, map_entries, map_cap);

	if (htmlc->layout != NULL) {
		ms_diag_count_boxes(htmlc->layout, &box_map, &box_count);
	}
	free(map_entries);
	map_entries = NULL;

	/* Dynamic capacity and headroom checks */
	map_cap = ms_next_pow2((box_count > 0 ? box_count * 2 : 16));
	required_bytes = (unsigned long)(node_count * sizeof(struct ms_diag_snapshot_node) +
			 box_count * sizeof(struct ms_diag_snapshot_box) +
			 map_cap * sizeof(struct ms_box_visited_entry));

	free_mem = macsurf_free_mem();
	/* Hard diagnostic limit: 2MB total for snapshot on OS 9 */
	if (required_bytes > 2097152UL || (free_mem > 0 && (long)required_bytes > free_mem / 2)) {
		g_dom_capture_in_progress = 0;
		snprintf(line, sizeof line,
			"MSDIAG 1 domstart complete=0 status=error reason=capacity required=%lu limit=%lu nodes=%lu boxes=%lu\n",
			required_bytes, (free_mem > 0) ? (unsigned long)(free_mem / 2) : 2097152UL,
			node_count, box_count);
		return diag_cat(buf, cap, 0, line);
	}

	/* Allocate snapshot structures */
	ms_diag_snapshot_free();

	g_dom_snapshot.nodes = (struct ms_diag_snapshot_node *) malloc(
			(size_t)(node_count > 0 ? node_count * sizeof(struct ms_diag_snapshot_node) : sizeof(struct ms_diag_snapshot_node)));
	g_dom_snapshot.boxes = (struct ms_diag_snapshot_box *) malloc(
			(size_t)(box_count > 0 ? box_count * sizeof(struct ms_diag_snapshot_box) : sizeof(struct ms_diag_snapshot_box)));
	map_entries = (struct ms_box_visited_entry *) malloc(
			(size_t)(map_cap * sizeof(struct ms_box_visited_entry)));

	if (g_dom_snapshot.nodes == NULL || (box_count > 0 && g_dom_snapshot.boxes == NULL) || map_entries == NULL) {
		ms_diag_snapshot_free();
		if (map_entries != NULL) free(map_entries);
		g_dom_capture_in_progress = 0;
		snprintf(line, sizeof line,
			"MSDIAG 1 domstart complete=0 status=error reason=allocation\n");
		return diag_cat(buf, cap, 0, line);
	}

	ms_box_map_init(&box_map, map_entries, map_cap);

	/* Assign box IDs in map first */
	if (htmlc->layout != NULL) {
		unsigned long b_counter = 0;
		ms_diag_count_boxes(htmlc->layout, &box_map, &b_counter);
	}

	/* Atomic Pass 2: fill snapshot */
	n_idx = 0;
	ms_diag_fill_dom_nodes((dom_node *) htmlc->document, g_dom_snapshot.nodes, &n_idx, node_count);
	g_dom_snapshot.node_count = n_idx;

	b_idx = 0;
	if (htmlc->layout != NULL) {
		ms_diag_fill_boxes(htmlc->layout, &box_map, g_dom_snapshot.boxes, &b_idx, box_count, 0);
	}
	g_dom_snapshot.box_count = b_idx;

	/* Connect node -> box_id in snapshot */
	for (i = 0; i < g_dom_snapshot.node_count; i++) {
		unsigned long bid = 0;
		/* match by node_id */
		unsigned long k;
		for (k = 0; k < g_dom_snapshot.box_count; k++) {
			if (g_dom_snapshot.boxes[k].node_id == g_dom_snapshot.nodes[i].node_id) {
				bid = g_dom_snapshot.boxes[k].box_id;
				break;
			}
		}
		g_dom_snapshot.nodes[i].box_id = bid;
	}

	free(map_entries);
	map_entries = NULL;

	g_dom_snapshot.doc_id = htmlc->doc_id;
	g_dom_snapshot.frame_id = htmlc->frame_id;
	g_dom_snapshot.nav_id = content_get_nav_id((struct content *) htmlc);
	g_dom_snapshot.content_token = macos9_content_token((struct content *) htmlc);
	g_dom_snapshot.box_generation = htmlc->live_box_generation;
	g_dom_snapshot.htmlc = htmlc;
	g_dom_snapshot.valid = 1;
	g_dom_snapshot.live_changed = 0;

	g_dom_capture_in_progress = 0;

	snprintf(line, sizeof line,
		"MSDIAG 1 domstart complete=1 coverage=connected_tree doc=%lu frame=%lu nav=%lu content_token=%lu box_generation=%lu nodes=%lu boxes=%lu\n",
		g_dom_snapshot.doc_id, g_dom_snapshot.frame_id, g_dom_snapshot.nav_id,
		g_dom_snapshot.content_token, g_dom_snapshot.box_generation,
		g_dom_snapshot.node_count, g_dom_snapshot.box_count);
	n = diag_cat(buf, cap, 0, line);
	return n;
}

static void ms_diag_check_snapshot_drift(void)
{
	if (!g_dom_snapshot.valid || g_dom_snapshot.htmlc == NULL) return;
	/* Validate the captured generation, not pointer membership alone.  A newly
	 * registered content may reuse this address after the snapshot owner dies;
	 * its fields belong to the new content and must never be sampled as though
	 * they described the retained snapshot. */
	if (macos9_content_token_valid((struct content *) g_dom_snapshot.htmlc,
			g_dom_snapshot.content_token)) {
		if (g_dom_snapshot.htmlc->live_box_generation != g_dom_snapshot.box_generation ||
		    g_dom_snapshot.htmlc->doc_id != g_dom_snapshot.doc_id) {
			g_dom_snapshot.live_changed = 1;
		}
	} else {
		g_dom_snapshot.live_changed = 1;
	}
}

long macsurf_diag_serialize_dom(char *buf, long cap, unsigned long after, unsigned long limit)
{
	char line[256];
	long n = 0;
	unsigned long start_seq;
	unsigned long max_return;
	unsigned long returned = 0;
	unsigned long next_after = after;
	unsigned long i;
	int complete = 0;
	int truncated = 0;

	if (buf == NULL || cap < 2) return 0;
	buf[0] = '\0';

	ms_diag_check_snapshot_drift();

	if (!g_dom_snapshot.valid) {
		snprintf(line, sizeof line,
			"MSDIAG 1 dom complete=0 status=error reason=no_snapshot\n");
		return diag_cat(buf, cap, 0, line);
	}

	if (limit == 0 || limit > 128) limit = 64;

	n = diag_cat(buf, cap, n, "MSDIAG 1 dom\n");
	snprintf(line, sizeof line,
		"doc=%lu frame=%lu nav=%lu coverage=connected_tree total_nodes=%lu live_changed=%d snapshot_stale=%d\n",
		g_dom_snapshot.doc_id, g_dom_snapshot.frame_id, g_dom_snapshot.nav_id,
		g_dom_snapshot.node_count, g_dom_snapshot.live_changed, g_dom_snapshot.live_changed);
	n = diag_cat(buf, cap, n, line);

	start_seq = after;
	max_return = start_seq + limit;
	if (max_return > g_dom_snapshot.node_count) max_return = g_dom_snapshot.node_count;

	for (i = start_seq; i < max_return; i++) {
		struct ms_diag_snapshot_node *sn = &g_dom_snapshot.nodes[i];
		snprintf(line, sizeof line,
			"node seq=%lu id=%lu parent=%lu type=%d tag=%s id_attr=%s class=%s box=%lu\n",
			i + 1, sn->node_id, sn->parent_node_id, (int)sn->node_type,
			sn->tag[0] ? sn->tag : "-",
			sn->node_id_attr[0] ? sn->node_id_attr : "-",
			sn->class_name[0] ? sn->class_name : "-",
			sn->box_id);
		if (n + (long)strlen(line) >= cap - 128) {
			truncated = 1;
			break;
		}
		n = diag_cat(buf, cap, n, line);
		returned++;
		next_after = i + 1;
	}

	complete = (next_after >= g_dom_snapshot.node_count) ? 1 : 0;
	snprintf(line, sizeof line,
		"returned=%lu next_after=%lu complete=%d truncated=%d\n",
		returned, next_after, complete, truncated);
	n = diag_cat(buf, cap, n, line);
	return n;
}

long macsurf_diag_serialize_boxes(char *buf, long cap, unsigned long after, unsigned long limit)
{
	char line[256];
	long n = 0;
	unsigned long start_seq;
	unsigned long max_return;
	unsigned long returned = 0;
	unsigned long next_after = after;
	unsigned long i;
	int complete = 0;
	int truncated = 0;

	if (buf == NULL || cap < 2) return 0;
	buf[0] = '\0';

	ms_diag_check_snapshot_drift();

	if (!g_dom_snapshot.valid) {
		snprintf(line, sizeof line,
			"MSDIAG 1 boxes complete=0 status=error reason=no_snapshot\n");
		return diag_cat(buf, cap, 0, line);
	}

	if (limit == 0 || limit > 128) limit = 64;

	n = diag_cat(buf, cap, n, "MSDIAG 1 boxes\n");
	snprintf(line, sizeof line,
		"doc=%lu box_generation=%lu total_boxes=%lu live_changed=%d snapshot_stale=%d\n",
		g_dom_snapshot.doc_id, g_dom_snapshot.box_generation,
		g_dom_snapshot.box_count, g_dom_snapshot.live_changed, g_dom_snapshot.live_changed);
	n = diag_cat(buf, cap, n, line);

	start_seq = after;
	max_return = start_seq + limit;
	if (max_return > g_dom_snapshot.box_count) max_return = g_dom_snapshot.box_count;

	for (i = start_seq; i < max_return; i++) {
		struct ms_diag_snapshot_box *sb = &g_dom_snapshot.boxes[i];
		snprintf(line, sizeof line,
			"box seq=%lu id=%lu parent=%lu node=%lu type=%d flags=0x%x x=%ld y=%ld w=%ld h=%ld ptr=0x%lx\n",
			i + 1, sb->box_id, sb->parent_box_id, sb->node_id,
			(int)sb->type, (unsigned int)sb->flags,
			sb->x, sb->y, sb->width, sb->height, sb->box_ptr);
		if (n + (long)strlen(line) >= cap - 128) {
			truncated = 1;
			break;
		}
		n = diag_cat(buf, cap, n, line);
		returned++;
		next_after = i + 1;
	}

	complete = (next_after >= g_dom_snapshot.box_count) ? 1 : 0;
	snprintf(line, sizeof line,
		"returned=%lu next_after=%lu complete=%d truncated=%d\n",
		returned, next_after, complete, truncated);
	n = diag_cat(buf, cap, n, line);
	return n;
}
