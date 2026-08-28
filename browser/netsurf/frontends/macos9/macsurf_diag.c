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
