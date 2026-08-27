/*
 * MacSurf  -  macsurf_gap.c
 *
 * Compatibility-gap census (MacSurf Trace, Phase 0). See macsurf_gap.h.
 */

#include "macsurf_gap.h"
#include "macsurf_debug_log.h"

/* One counter per ms_gap_id. Plain file-static: every writer runs on the
 * cooperative main / notifier context, so no locking is needed. */
static unsigned long g_gap_count[MS_GAP__N];

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
		g_gap_count[id]++;
	}
}

void macsurf_gap_emit_summary(void)
{
	unsigned long total = 0;
	int unique = 0;
	int order[MS_GAP__N];
	int i;
	int j;
	int limit;

	for (i = 0; i < MS_GAP__N; i++) {
		order[i] = i;
		total += g_gap_count[i];
		if (g_gap_count[i] != 0) {
			unique++;
		}
	}
	if (total == 0) {
		return;
	}

	/* insertion sort order[] by count, highest first; ties keep enum order */
	for (i = 1; i < MS_GAP__N; i++) {
		int key = order[i];
		j = i - 1;
		while (j >= 0 && g_gap_count[order[j]] < g_gap_count[key]) {
			order[j + 1] = order[j];
			j--;
		}
		order[j + 1] = key;
	}

	/* macsurf_debug_log_writef understands %d/%ld/%p/%s/%% only; counts are
	 * unsigned long, cast to long (a session cannot plausibly overflow). */
	macsurf_debug_log_writef("LIFE GAPSUMMARY unique=%d total=%ld",
		unique, (long) total);

	limit = (unique < 6) ? unique : 6;
	for (i = 0; i < limit; i++) {
		int id = order[i];
		macsurf_debug_log_writef("LIFE GAPTOP id=%s n=%ld",
			g_gap_slug[id], (long) g_gap_count[id]);
	}

	for (i = 0; i < MS_GAP__N; i++) {
		g_gap_count[i] = 0;
	}
}
