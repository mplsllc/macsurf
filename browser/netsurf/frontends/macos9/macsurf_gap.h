/*
 * MacSurf  -  macsurf_gap.h
 *
 * Compatibility-gap census (MacSurf Trace, Phase 0).
 *
 * A gap is a point where the engine took an unsupported / fallback path
 * because a real browser capability is missing -- NOT an ordinary runtime
 * error (a failed download, a thrown exception): those belong in the
 * normalized ERROR model, added later.
 *
 * The hit site does ONE array increment. No string is formatted and no I/O
 * happens until macsurf_gap_emit_summary() is called (currently from the
 * NAV: DONE hook as a temporary validation bridge; it is replaced by an
 * on-demand 'MSdg GET gaps' AppleEvent query in a later phase).
 *
 * A gap id names the OBSERVABLE MISSING BEHAVIOUR, not the subsystem it was
 * seen in -- MacSurf already has partial MutationObserver / grid surface, so
 * the census must count the specific fallback actually taken.
 */

#ifndef MACSURF_GAP_H
#define MACSURF_GAP_H

/* Keep this enum and g_gap_slug[] in macsurf_gap.c in lockstep and in order.
 * Append new ids immediately before MS_GAP__N; never renumber. */
enum ms_gap_id {
	MS_GAP_GEOMETRY_UNDEFINED = 0,          /* layout geometry read answered undefined */
	MS_GAP_RESIZE_OBSERVER_MISSING,         /* ResizeObserver.observe was a no-op */
	MS_GAP_INTERSECTION_OBSERVER_MISSING,   /* IntersectionObserver.observe was a no-op */
	MS_GAP_MUTATION_OBSERVER_CALLBACK_UNSUPPORTED, /* observe() accepted, no delivery path */
	MS_GAP_COMPUTED_STYLE_PROPERTY_UNSUPPORTED,    /* getComputedStyle returned no value */
	MS_GAP_CSS_TRANSITION_DROPPED,          /* transition* declaration stripped pre-parse */
	MS_GAP_CSS_ANIMATION_DROPPED,           /* animation* declaration stripped pre-parse */
	MS_GAP_GRID_MINMAX_COLLAPSED,           /* minmax() track collapsed to 1fr */
	MS_GAP_GRID_FIT_CONTENT_COLLAPSED,      /* fit-content() track collapsed to 1fr */
	MS_GAP_WEBFONT_FORMAT_UNSUPPORTED,      /* @font-face format/decode not supported */
	MS_GAP__N
};

/* Cheap: bounds-check + one increment. Safe to call from any thread context
 * MacSurf actually has (all cooperative). Out-of-range ids are ignored. */
void macsurf_gap_hit(int id);

/* Called at NAV: DONE. Freezes the in-progress counters into the last-nav
 * snapshot, clears the in-progress counters, and (only if non-empty) emits the
 * temporary LIFE GAPSUMMARY / LIFE GAPTOP bridge lines from the frozen copy. */
void macsurf_gap_emit_summary(unsigned long nav_id);

/* Frozen last-completed-navigation snapshot -- read-only, non-destructive.
 * Backs `MSdg GET gaps` in macsurf_diag.c. */
int           macsurf_gap_kind_count(void);        /* == MS_GAP__N */
unsigned long macsurf_gap_last_nav(void);
int           macsurf_gap_last_unique(void);
unsigned long macsurf_gap_last_total(void);
unsigned long macsurf_gap_last_count(int id);      /* 0 if id out of range */
const char   *macsurf_gap_slug(int id);           /* "?" if id out of range */

#endif /* MACSURF_GAP_H */
