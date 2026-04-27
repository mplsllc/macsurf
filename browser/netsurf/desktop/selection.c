/*
 * Copyright 2005 Adrian Lees <adrianl@users.sourceforge.net>
 * Copyright 2008 Michael Drake <tlsa@netsurf-browser.org>
 *
 * This file is part of NetSurf, http://www.netsurf-browser.org/
 *
 * NetSurf is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * NetSurf is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * \file
 * implementation of text selection within browser windows.
 */

#include <stdlib.h>
#include <string.h>

#include "netsurf/clipboard.h"
#include "netsurf/browser_window.h"
#include "netsurf/window.h"
#include "utils/utils.h"
#include "content/content_protected.h"

#include "desktop/browser_private.h"
#include "desktop/gui_internal.h"
#include "desktop/selection.h"


struct selection_string {
	char *buffer;
	size_t buffer_len;
	size_t length;

	int n_styles;
	nsclipboard_styles *styles;
};


typedef enum {
	DRAG_NONE,
	DRAG_START,
	DRAG_END
} seln_drag_state;

struct selection {
	struct content *c;

	unsigned max_idx;  /* total bytes in text representation */

	unsigned start_idx;  /* offset in bytes within text representation */
	unsigned end_idx;

	bool defined;

	seln_drag_state drag_state;
};

/**
 * Redraws the given range of text.
 *
 * \param s selection object
 * \param start_idx start offset (bytes) within the textual representation
 * \param end_idx end offset (bytes) within the textual representation
 */
static nserror
selection_redraw(struct selection *s, unsigned start_idx, unsigned end_idx)
{
	nserror res;

	if (s->c->handler->textselection_redraw != NULL) {
		res = s->c->handler->textselection_redraw(s->c,
							  start_idx,
							  end_idx);
	} else {
		res = NSERROR_NOT_IMPLEMENTED;
	}

	return res;
}


/**
 * Set the start position of the current selection, updating the screen.
 *
 * \param s selection object
 * \param offset byte offset within textual representation
 */
static void selection_set_start(struct selection *s, unsigned offset)
{
	bool was_defined;
	unsigned old_start;

	old_start = s->start_idx;
	s->start_idx = offset;

	was_defined = s->defined;
	s->defined = (s->start_idx < s->end_idx);

	if (was_defined) {
		if (offset < old_start) {
			selection_redraw(s, s->start_idx, old_start);
		} else {
			selection_redraw(s, old_start, s->start_idx);
		}
	} else if (s->defined) {
		selection_redraw(s, s->start_idx, s->end_idx);
	}
}


/**
 * Set the end position of the current selection, updating the screen.
 *
 * \param s selection object
 * \param offset byte offset within textual representation
 */
static void selection_set_end(struct selection *s, unsigned offset)
{
	bool was_defined;
	unsigned old_end;

	old_end = s->end_idx;
	s->end_idx = offset;

	was_defined = s->defined;
	s->defined = (s->start_idx < s->end_idx);

	if (was_defined) {
		if (offset < old_end) {
			selection_redraw(s, s->end_idx, old_end);
		} else {
			selection_redraw(s, old_end, s->end_idx);
		}
	} else if (s->defined) {
		selection_redraw(s, s->start_idx, s->end_idx);
	}
}


/**
 * Traverse the current selection, calling the handler function (with its
 * handle) for all boxes that lie (partially) within the given range
 *
 * \param s The selection context.
 * \param handler handler function to call
 * \param handle  handle to pass
 * \return false iff traversal abandoned part-way through
 */
static bool
selection_copy(struct selection *s, struct selection_string *selstr)
{
	nserror res;

	if (s->c->handler->textselection_copy != NULL) {
		res = s->c->handler->textselection_copy(s->c,
							s->start_idx,
							s->end_idx,
							selstr);
	} else {
		res = NSERROR_NOT_IMPLEMENTED;
	}

	if (res != NSERROR_OK) {
		return false;
	}
	return true;
}


/**
 * Append text to selection string.
 *
 * \param text text to be added
 * \param length length of text in bytes
 * \param space indicates whether a trailing space should be appended
 * \param style The font style to use.
 * \param sel_string string to append to, may be resized
 * \return true iff successful
 */
bool
selection_string_append(const char *text,
			size_t length,
			bool space,
			plot_font_style_t *style,
			struct selection_string *sel_string)
{
	size_t new_length = sel_string->length + length + (space ? 1 : 0) + 1;

	if (style != NULL) {
		/* Add text run style */
		nsclipboard_styles *new_styles;

		if (sel_string->n_styles == 0) {
			assert(sel_string->length == 0);
		}

		new_styles = realloc(sel_string->styles,
				     (sel_string->n_styles + 1) *
				     sizeof(nsclipboard_styles));
		if (new_styles == NULL) {
			return false;
		}

		sel_string->styles = new_styles;

		sel_string->styles[sel_string->n_styles].style = *style;
		sel_string->styles[sel_string->n_styles].start =
			sel_string->length;

		sel_string->n_styles++;
	}

	if (new_length > sel_string->buffer_len) {
		/* Need to extend buffer */
		size_t new_alloc = new_length + (new_length / 4);
		char *new_buff;

		new_buff = realloc(sel_string->buffer, new_alloc);
		if (new_buff == NULL) {
			return false;
		}

		sel_string->buffer = new_buff;
		sel_string->buffer_len = new_alloc;
	}

	/* Copy text onto end of existing text in buffer */
	memcpy(sel_string->buffer + sel_string->length, text, length);
	sel_string->length += length;

	if (space) {
		sel_string->buffer[sel_string->length++] = ' ';
	}

	/* Ensure NULL termination */
	sel_string->buffer[sel_string->length] = '\0';

	return true;
}


/* exported interface documented in desktop/selection.h */
struct selection *selection_create(struct content *c)
{
	struct selection *sel;
	sel = calloc(1, sizeof(struct selection));
	if (sel) {
		sel->c = c;
		sel->drag_state = DRAG_NONE;
		sel->max_idx = 0;
		selection_clear(sel, false);
	}

	return sel;
}


/* exported interface documented in desktop/selection.h */
void selection_destroy(struct selection *s)
{
	if (s == NULL) {
		return;
	}

	selection_clear(s, true);
	free(s);
}


/* exported interface documented in desktop/selection.h */
void selection_reinit(struct selection *s)
{
	s->max_idx = 0;

	if (s->c->handler->textselection_get_end != NULL) {
		s->c->handler->textselection_get_end(s->c, &s->max_idx);
	}

	if (s->defined) {
		if (s->end_idx > s->max_idx) {
			s->end_idx = s->max_idx;
		}
		if (s->start_idx > s->max_idx) {
			s->start_idx = s->max_idx;
		}
		s->defined = (s->end_idx > s->start_idx);
	}
}


/* exported interface documented in desktop/selection.h */
void selection_init(struct selection *s)
{
	if (s->defined) {
		selection_clear(s, true);
	}

	s->defined = false;
	s->start_idx = 0;
	s->end_idx = 0;
	s->drag_state = DRAG_NONE;

	selection_reinit(s);
}


/* exported interface documented in desktop/selection.h */
bool
selection_click(struct selection *s,
		struct browser_window *top,
		browser_mouse_state mouse,
		unsigned idx)
{
	browser_mouse_state modkeys;
	int pos = -1;  /* 0 = inside selection, 1 = after it */

	modkeys = (mouse & (BROWSER_MOUSE_MOD_1 | BROWSER_MOUSE_MOD_2));

	top = browser_window_get_root(top);

	if (s->defined) {
		if (idx > s->start_idx) {
			if (idx <= s->end_idx) {
				pos = 0;
			} else {
				pos = 1;
			}
		}
	}

	if (!pos &&
	    ((mouse & BROWSER_MOUSE_DRAG_1) ||
	     (modkeys && (mouse & BROWSER_MOUSE_DRAG_2)))) {
		/* drag-saving selection */
		char *sel = selection_get_copy(s);
		guit->window->drag_save_selection(top->window, sel);
		free(sel);
	} else if (!modkeys) {
		if (pos && (mouse & BROWSER_MOUSE_PRESS_1)) {
			/* Clear the selection if mouse is pressed
			 * outside the selection, Otherwise clear on
			 * release (to allow for drags)
			 */

			selection_clear(s, true);

		} else if (mouse & BROWSER_MOUSE_DRAG_1) {
			/* start new selection drag */

			selection_clear(s, true);

			selection_set_start(s, idx);
			selection_set_end(s, idx);

			s->drag_state = DRAG_END;

			guit->window->event(top->window,
					    GW_EVENT_START_SELECTION);

		} else if (mouse & BROWSER_MOUSE_DRAG_2) {

			/* adjust selection, but only if there is one */
			if (!s->defined) {
				return false;	/* ignore Adjust drags */
			}

			if (pos >= 0) {
				selection_set_end(s, idx);

				s->drag_state = DRAG_END;
			} else {
				selection_set_start(s, idx);

				s->drag_state = DRAG_START;
			}

			guit->window->event(top->window,
					    GW_EVENT_START_SELECTION);

		} else if (mouse & BROWSER_MOUSE_CLICK_2) {

			/* ignore Adjust clicks when there's no selection */
			if (!s->defined) {
				return false;
			}

			if (pos >= 0) {
				selection_set_end(s, idx);
			} else {
				selection_set_start(s, idx);
			}
			s->drag_state = DRAG_NONE;

		} else {
			return false;
		}

	} else {
		/* not our problem */
		return false;
	}

	/* this mouse click is selection-related */
	return true;
}


/* exported interface documented in desktop/selection.h */
void
selection_track(struct selection *s, browser_mouse_state mouse, unsigned idx)
{
	if (!mouse) {
		s->drag_state = DRAG_NONE;
	}

	switch (s->drag_state) {

	case DRAG_START:
		if (idx > s->end_idx) {
			unsigned old_end = s->end_idx;
			selection_set_end(s, idx);
			selection_set_start(s, old_end);
			s->drag_state = DRAG_END;
		} else {
			selection_set_start(s, idx);
		}
		break;

	case DRAG_END:
		if (idx < s->start_idx) {
			unsigned old_start = s->start_idx;
			selection_set_start(s, idx);
			selection_set_end(s, old_start);
			s->drag_state = DRAG_START;
		} else {
			selection_set_end(s, idx);
		}
		break;

	default:
		break;
	}
}


/* exported interface documented in desktop/selection.h */
char *selection_get_copy(struct selection *s)
{
	struct selection_string sel_string = {
		.buffer = NULL,
		.buffer_len = 0,
		.length = 0,

		.n_styles = 0,
		.styles = NULL
	};

	if (s == NULL || !s->defined)
		return NULL;

	if (!selection_copy(s, &sel_string)) {
		free(sel_string.buffer);
		free(sel_string.styles);
		return NULL;
	}

	free(sel_string.styles);

	return sel_string.buffer;
}


/* exported interface documented in desktop/selection.h */
bool selection_copy_to_clipboard(struct selection *s)
{
	struct selection_string sel_string = {
		.buffer = NULL,
		.buffer_len = 0,
		.length = 0,

		.n_styles = 0,
		.styles = NULL
	};

	if (s == NULL || !s->defined) {
		return false;
	}

	if (!selection_copy(s, &sel_string)) {
		free(sel_string.buffer);
		free(sel_string.styles);
		return false;
	}

	guit->clipboard->set(sel_string.buffer,
			     sel_string.length,
			     sel_string.styles,
			     sel_string.n_styles);

	free(sel_string.buffer);
	free(sel_string.styles);

	return true;
}


/* exported interface documented in desktop/selection.h */
bool selection_clear(struct selection *s, bool redraw)
{
	int old_start, old_end;
	bool was_defined;

	assert(s);

	was_defined = s->defined;
	old_start = s->start_idx;
	old_end = s->end_idx;

	s->defined = false;
	s->start_idx = 0;
	s->end_idx = 0;

	if (redraw && was_defined) {
		selection_redraw(s, old_start, old_end);
	}

	return was_defined;
}


/* exported interface documented in desktop/selection.h */
void selection_select_all(struct selection *s)
{
	assert(s);
	s->defined = true;

	selection_set_start(s, 0);
	selection_set_end(s, s->max_idx);
}


/* exported interface documented in desktop/selection.h */
void selection_set_position(struct selection *s, unsigned start, unsigned end)
{
	selection_set_start(s, start);
	selection_set_end(s, end);
}


/* exported interface documented in desktop/selection.h */
bool
selection_highlighted(const struct selection *s,
		      unsigned start,
		      unsigned end,
		      unsigned *start_idx,
		      unsigned *end_idx)
{
	assert(s);

	if (!s->defined) {
		return false;
	}

	if ((end <= s->start_idx) ||
	    (start >= s->end_idx)) {
		return false;
	}

	*start_idx = (s->start_idx >= start) ? (s->start_idx - start) : 0;
	*end_idx = min(end, s->end_idx) - start;

	return true;
}

/* exported interface documented in desktop/selection.h */
bool selection_active(struct selection *s)
{
	return s->defined;
}

bool selection_dragging(struct selection *s)
{
	return s->drag_state != DRAG_NONE;
}

bool selection_dragging_start(struct selection *s)
{
	return s->drag_state == DRAG_START;
}

void selection_drag_end(struct selection *s)
{
	s->drag_state = DRAG_NONE;
}
