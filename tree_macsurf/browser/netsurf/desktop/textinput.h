/*
 * Copyright 2003 Phil Mellor <monkeyson@users.sourceforge.net>
 * Copyright 2004 James Bursa <bursa@users.sourceforge.net>
 * Copyright 2004 Andrew Timmins <atimmins@blueyonder.co.uk>
 * Copyright 2004 John Tytgat <joty@netsurf-browser.org>
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
 * Textual input handling interface
 */

#ifndef NETSURF_DESKTOP_TEXTINPUT_H
#define NETSURF_DESKTOP_TEXTINPUT_H

struct browser_window;

/**
 * Position the caret and assign a callback for key presses.
 *
 * \param bw The browser window in which to place the caret
 * \param x X coordinate of the caret
 * \param y Y coordinate
 * \param height Height of caret
 * \param clip Clip rectangle for caret, or NULL if none
 */
void browser_window_place_caret(struct browser_window *bw, int x, int y,
		int height, const struct rect *clip);

/**
 * Removes the caret and callback for key process.
 *
 * \param bw The browser window from which to remove caret.
 * \param only_hide Revove the caret but leave the textinput editable.
 */
void browser_window_remove_caret(struct browser_window *bw, bool only_hide);


#endif
