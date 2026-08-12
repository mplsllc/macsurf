/*
 * Copyright 2026 MacSurf (fixes76)
 *
 * Animation tick loop. Drives redraws when boxes on the current page
 * have `-macsurf-animation-opacity` SET. Cheap when no animations are
 * active.
 *
 * Architecture (fixes76b):
 *   - main.c update handler sets `macos9_paint_gw` before
 *     browser_window_redraw and clears it after.
 *   - redraw.c calls macos9_animation_register_rect(x, y, w, h) for each
 *     animated box it paints. Rect is in page coords; we attach it to
 *     macos9_paint_gw so the tick knows which window to invalidate.
 *   - macos9_animation_tick() (called from main.c's idle pass) drains
 *     the queue at ~10 fps via macos9_window_invalidate_rect, which
 *     maps the page-coord rect to window coords and InvalWindowRect's
 *     just that area. Result: only animated boxes' rects flash, not
 *     the whole page.
 *   - When the queue empties and the next redraw doesn't refill it
 *     (page no longer has live animations) the tick goes quiet.
 *   - macos9_animation_now_ticks() returns TickCount() and is the
 *     single clock source the renderer uses for animation phase.
 */

#include "macos9.h"

/* Ticks at 60Hz. 6 = ~10 fps - slower than fixes76a's 20fps so each
 * redraw cycle has time to complete on real hardware. */
#define ANIM_TICK_INTERVAL 6

/* Soft cap. Plenty for typical pages - 4 animated badges on the test
 * page, MacTrove has none. Overflow is silently dropped (last writer
 * loses); the next tick will re-fill from the next redraw pass. */
#define ANIM_MAX_RECTS 64

struct anim_rect_entry {
	struct gui_window *gw;
	int x, y, w, h;
};

static struct anim_rect_entry anim_rects[ANIM_MAX_RECTS];
static int anim_rect_count = 0;
static uint32_t last_anim_tick = 0;

/* Set by main.c around browser_window_redraw so register_rect knows
 * which window the page-coord rect belongs to. NULL outside paint. */
struct gui_window *macos9_paint_gw = NULL;

void macos9_animation_register(void) {
	/* Legacy fixes76a entrypoint. Kept for ABI; new code uses
	 * macos9_animation_register_rect with the box bounding rect. */
}

void macos9_animation_register_rect(int x, int y, int w, int h) {
	if (macos9_paint_gw == NULL) return;
	if (anim_rect_count >= ANIM_MAX_RECTS) return;
	anim_rects[anim_rect_count].gw = macos9_paint_gw;
	anim_rects[anim_rect_count].x = x;
	anim_rects[anim_rect_count].y = y;
	anim_rects[anim_rect_count].w = w;
	anim_rects[anim_rect_count].h = h;
	anim_rect_count++;
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
	int i;

	if (anim_rect_count == 0) return;

	now = (uint32_t)TickCount();
	if ((now - last_anim_tick) < ANIM_TICK_INTERVAL) return;
	last_anim_tick = now;

	for (i = 0; i < anim_rect_count; i++) {
		macos9_window_invalidate_rect(anim_rects[i].gw,
			anim_rects[i].x, anim_rects[i].y,
			anim_rects[i].w, anim_rects[i].h);
	}
	/* Drain. The next redraw pass repopulates the queue from any
	 * still-animated boxes; if none, the tick goes idle. */
	anim_rect_count = 0;
#endif
}
