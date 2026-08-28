/*
 * MacSurf  -  macsurf_gap.c
 *
 * Compatibility-gap census (MacSurf Trace). See macsurf_gap.h.
 *
 * Two generations of state:
 *   g_gap_cur[]   -- accumulating for the navigation in progress
 *   g_gap_last[]  -- frozen snapshot of the last COMPLETED navigation
 * At NAV: DONE, macsurf_gap_emit_summary() freezes cur -> last, clears cur, and
 * emits the (temporary) LIFE GAPSUMMARY/GAPTOP bridge from the frozen copy.
 * macsurf_diag.c serialises the SAME frozen copy on demand for `MSdg GET gaps`,
 * so a query after the navigation returns real data, not zeros. Reads are
 * non-destructive.
 */

#include "macsurf_gap.h"
#include "macsurf_debug_log.h"

/* Plain file-static: every writer runs on the cooperative main / notifier
 * context, so no locking is needed. */
static unsigned long g_gap_cur[MS_GAP__N];
static unsigned long g_gap_last[MS_GAP__N];
static unsigned long g_gap_last_nav_id;
static unsigned long g_gap_last_total;
static int           g_gap_last_unique;

/* Serialisation-only. MUST stay in enum order and in lockstep with
 * enum ms_gap_id in macsurf_gap.h. Never read at a hit site. */
static const char *const g_gap_slug[MS_GAP__N] = {
	"geometry_undefined",
	"resize_observer_missing",
	"intersection_observer_missing",
	"mutation_observer_callback_unsupported",
	"computed_style_property_unsupported",
	"css_transition_dropped",
	"css_animation_dropped",
	"grid_minmax_collapsed",
	"grid_fit_content_collapsed",
	"webfont_format_unsupported"
};

void macsurf_gap_hit(int id)
{
	if (id >= 0 && id < MS_GAP__N) {
		g_gap_cur[id]++;
	}
}

void macsurf_gap_emit_summary(unsigned long nav_id)
{
	unsigned long total = 0;
	int unique = 0;
	int order[MS_GAP__N];
	int i;
	int j;
	int limit;

	/* Freeze cur -> last for on-demand queries, then clear cur. */
	for (i = 0; i < MS_GAP__N; i++) {
		order[i] = i;
		g_gap_last[i] = g_gap_cur[i];
		total += g_gap_cur[i];
		if (g_gap_cur[i] != 0) {
			unique++;
		}
		g_gap_cur[i] = 0;
	}
	g_gap_last_nav_id = nav_id;
	g_gap_last_total = total;
	g_gap_last_unique = unique;

	if (total == 0) {
		return;		/* nothing to log; the frozen zero is still queryable */
	}

	/* insertion sort order[] by count, highest first; ties keep enum order */
	for (i = 1; i < MS_GAP__N; i++) {
		int key = order[i];
		j = i - 1;
		while (j >= 0 && g_gap_last[order[j]] < g_gap_last[key]) {
			order[j + 1] = order[j];
			j--;
		}
		order[j + 1] = key;
	}

	/* macsurf_debug_log_writef understands %d/%ld/%p/%s/%% only; counts are
	 * unsigned long, cast to long (a session cannot plausibly overflow). */
	macsurf_debug_log_writef("LIFE GAPSUMMARY nav=%ld unique=%d total=%ld",
		(long) nav_id, unique, (long) total);

	limit = (unique < 6) ? unique : 6;
	for (i = 0; i < limit; i++) {
		int id = order[i];
		macsurf_debug_log_writef("LIFE GAPTOP id=%s n=%ld",
			g_gap_slug[id], (long) g_gap_last[id]);
	}
}

/* --- frozen-snapshot accessors for macsurf_diag.c (non-destructive) --- */

int macsurf_gap_kind_count(void)
{
	return MS_GAP__N;
}

unsigned long macsurf_gap_last_nav(void)
{
	return g_gap_last_nav_id;
}

int macsurf_gap_last_unique(void)
{
	return g_gap_last_unique;
}

unsigned long macsurf_gap_last_total(void)
{
	return g_gap_last_total;
}

unsigned long macsurf_gap_last_count(int id)
{
	return (id >= 0 && id < MS_GAP__N) ? g_gap_last[id] : 0;
}

const char *macsurf_gap_slug(int id)
{
	return (id >= 0 && id < MS_GAP__N) ? g_gap_slug[id] : "?";
}
