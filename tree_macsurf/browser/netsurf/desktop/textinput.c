/*
 * Copyright 2003 Phil Mellor <monkeyson@users.sourceforge.net>
 * Copyright 2004 James Bursa <bursa@users.sourceforge.net>
 * Copyright 2004 Andrew Timmins <atimmins@blueyonder.co.uk>
 * Copyright 2004 John Tytgat <joty@netsurf-browser.org>
 * Copyright 2005 Adrian Lees <adrianl@users.sourceforge.net>
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

/** \file
 * Textual input handling implementation
 */

#include <assert.h>
#include <ctype.h>
#include <string.h>
#include <dom/dom.h>

#include "utils/log.h"
#include "utils/talloc.h"
#include "utils/utf8.h"
#include "utils/utils.h"
#include "netsurf/types.h"
#include "netsurf/mouse.h"
#include "netsurf/form.h"
#include "netsurf/window.h"
#include "netsurf/browser_window.h"
#include "netsurf/keypress.h"
#include "content/content.h"

#include "desktop/browser_private.h"
#include "desktop/textinput.h"
#include "desktop/gui_internal.h"

/* Define to enable textinput debug */
#undef TEXTINPUT_DEBUG


/* exported interface documented in desktop/textinput.h */
void browser_window_place_caret(struct browser_window *bw, int x, int y,
		int height, const struct rect *clip)
{
	struct browser_window *root_bw;
	int pos_x = 0;
	int pos_y = 0;
	struct rect cr;
	struct rect *crp = NULL;

	/* Find top level browser window */
	root_bw = browser_window_get_root(bw);
	browser_window_get_position(bw, true, &pos_x, &pos_y);

	x = x * bw->scale + pos_x;
	y = y * bw->scale + pos_y;

	if (clip != NULL) {
		cr = *clip;
		cr.x0 += pos_x;
		cr.y0 += pos_y;
		cr.x1 += pos_x;
		cr.y1 += pos_y;
		crp = &cr;
	}

	/** \todo intersect with bw viewport */

	guit->window->place_caret(root_bw->window, x, y, height * bw->scale, crp);

	/* Set focus browser window */
	root_bw->focus = bw;
	root_bw->can_edit = true;
}

/* exported interface documented in desktop/textinput.h */
void browser_window_remove_caret(struct browser_window *bw, bool only_hide)
{
	struct browser_window *root_bw;

	root_bw = browser_window_get_root(bw);
	assert(root_bw != NULL);

	if (only_hide) {
		root_bw->can_edit = true;
	} else {
		root_bw->can_edit = false;
	}

	if (root_bw->window) {
		guit->window->event(root_bw->window, GW_EVENT_REMOVE_CARET);
	}
}

/* exported interface documented in netsurf/keypress.h */
bool browser_window_key_press(struct browser_window *bw, uint32_t key)
{
	struct browser_window *focus = bw->focus;

	assert(bw->window != NULL);

	if (focus == NULL)
		focus = bw;

	if (focus->current_content == NULL)
		return false;

	return content_keypress(focus->current_content, key);
}

