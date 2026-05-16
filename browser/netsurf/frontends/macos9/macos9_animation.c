/*
 * Copyright 2026 MacSurf (fixes76)
 *
 * Animation tick loop. Drives ~20fps redraws when the renderer reports
 * that some box on the current page has a `-macsurf-animation-opacity`
 * SET value. Cheap when no animations are active.
 *
 * Architecture:
 *   - redraw.c calls macos9_animation_register() each time it paints
 *     a box with animation SET. Sets anim_pending.
 *   - macos9_animation_tick() is called from the event-loop idle pass.
 *     If anim_pending && TickCount() - last_tick >= ANIM_TICK_INTERVAL,
 *     invalidate content rects of all windows so the next redraw is
 *     scheduled. anim_pending is reset; the redraw will re-set it if
 *     the page still has live animations.
 *   - macos9_animation_now_ticks() returns TickCount() and is the
 *     single clock source the renderer uses for animation phase.
 */

#include "macos9.h"

/* Ticks at 60Hz. 3 = ~20 fps. 6 = ~10 fps. */
#define ANIM_TICK_INTERVAL 3

static bool anim_pending = false;
static uint32_t last_anim_tick = 0;

void macos9_animation_register(void) {
	anim_pending = true;
}

uint32_t macos9_animation_now_ticks(void) {
#ifdef __MACOS9__
	return (uint32_t)TickCount();
#else
	static uint32_t fake = 0;
	return fake++;
#endif
}

void macos9_animation_tick(void) {
#ifdef __MACOS9__
	uint32_t now;
	extern struct gui_window *macos9_window_list_head(void);
	struct gui_window *g;

	if (!anim_pending) return;

	now = (uint32_t)TickCount();
	if ((now - last_anim_tick) < ANIM_TICK_INTERVAL) return;
	last_anim_tick = now;

	/* Clear the flag. The next redraw will re-set it if any box on
	 * any window still has an active animation, which gives us a
	 * cheap "stop ticking when nothing is animating" behaviour. */
	anim_pending = false;

	for (g = macos9_window_list_head(); g != NULL; g = g->next) {
		if (g->window) {
			macos9_window_invalidate_content(g);
		}
	}
#endif
}
