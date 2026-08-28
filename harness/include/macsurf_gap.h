#ifndef MACSURF_GAP_H
#define MACSURF_GAP_H
/* Harness stub mirror of frontends/macos9/macsurf_gap.h (no Carbon deps;
 * kept in lockstep with the real enum). */
enum ms_gap_id {
	MS_GAP_GEOMETRY_UNDEFINED = 0,
	MS_GAP_RESIZE_OBSERVER_MISSING,
	MS_GAP_INTERSECTION_OBSERVER_MISSING,
	MS_GAP_MUTATION_OBSERVER_CALLBACK_UNSUPPORTED,
	MS_GAP_COMPUTED_STYLE_PROPERTY_UNSUPPORTED,
	MS_GAP_CSS_TRANSITION_DROPPED,
	MS_GAP_CSS_ANIMATION_DROPPED,
	MS_GAP_GRID_MINMAX_COLLAPSED,
	MS_GAP_GRID_FIT_CONTENT_COLLAPSED,
	MS_GAP_WEBFONT_FORMAT_UNSUPPORTED,
	MS_GAP__N
};
void macsurf_gap_hit(int id);
void macsurf_gap_emit_summary(unsigned long nav_id);
int           macsurf_gap_kind_count(void);
unsigned long macsurf_gap_last_nav(void);
int           macsurf_gap_last_unique(void);
unsigned long macsurf_gap_last_total(void);
unsigned long macsurf_gap_last_count(int id);
const char   *macsurf_gap_slug(int id);
#endif
