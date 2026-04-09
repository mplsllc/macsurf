/*
 * MacSurf — browser_history_stub.c
 * Stub implementations for browser history, scrollbars, frames, carets.
 * Licensed under GPL v2.
 */

#include <stddef.h>
#include "utils/errors.h"

struct browser_window;
struct hlcache_handle;
struct scrollbar;
struct rect;
struct nsurl;
struct content;

/* Browser history */
nserror browser_window_history_create(struct browser_window *bw)
{
	return NSERROR_OK;
}

void browser_window_history_destroy(struct browser_window *bw) {}

nserror browser_window_history_add(struct browser_window *bw,
		struct hlcache_handle *content, struct nsurl *frag_id)
{
	return NSERROR_OK;
}

nserror browser_window_history_update(struct browser_window *bw,
		struct hlcache_handle *content)
{
	return NSERROR_OK;
}

nserror browser_window_history_get_scroll(struct browser_window *bw,
		float *sx, float *sy)
{
	if (sx) *sx = 0;
	if (sy) *sy = 0;
	return NSERROR_OK;
}

/* Scrollbars */
nserror scrollbar_create(int horizontal, int length, int full_size,
		int visible_size, void *pw,
		void (*cb)(void *pw, int msg, void *data),
		struct scrollbar **bar)
{
	*bar = NULL;
	return NSERROR_OK;
}

void scrollbar_destroy(struct scrollbar *s) {}

nserror scrollbar_set(struct scrollbar *s, int value, int bar_full)
{
	return NSERROR_OK;
}

int scrollbar_get_offset(struct scrollbar *s)
{
	return 0;
}

/* Caret */
void browser_window_place_caret(struct browser_window *bw,
		int x, int y, int height,
		void (*remove_fn)(struct browser_window *bw))
{
}

void browser_window_remove_caret(struct browser_window *bw, int only_check) {}

/* Frames / Iframes */
nserror browser_window_create_iframes(struct browser_window *bw)
{
	return NSERROR_OK;
}

void browser_window_recalculate_iframes(struct browser_window *bw) {}

void browser_window_invalidate_iframe(struct browser_window *bw) {}

nserror browser_window_create_frameset(struct browser_window *bw)
{
	return NSERROR_OK;
}

void browser_window_recalculate_frameset(struct browser_window *bw) {}

void browser_window_handle_scrollbars(struct browser_window *bw) {}
