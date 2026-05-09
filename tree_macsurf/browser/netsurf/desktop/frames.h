/*
 * Copyright 2006 Richard Wilson <info@tinct.net>
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
 * Frame and frameset creation and manipulation (interface).
 */

#ifndef NETSURF_DESKTOP_FRAMES_H_
#define NETSURF_DESKTOP_FRAMES_H_

struct scrollbar_msg_data;
struct content_html_iframe;
struct content_html_frames;

/**
 * Create and open iframes for a browser window.
 *
 * \param bw The browser window to create iframes for.
 * \return NSERROR_OK or error code on faliure.
 */
nserror browser_window_create_iframes(struct browser_window *bw);

/**
 * Recalculate iframe positions following a resize.
 *
 * \param bw The browser window to reposition iframes for
 */
void browser_window_recalculate_iframes(struct browser_window *bw);

/**
 * Invalidate an iframe causing a redraw.
 *
 * \param bw The browser window to invalidate
 */
nserror browser_window_invalidate_iframe(struct browser_window *bw);

/**
 * Destroy iframes opened in browser_window_create_iframes()
 *
 * \param bw The browser window to destroy iframes for.
 * \return NSERROR_OK
 */
nserror browser_window_destroy_iframes(struct browser_window *bw);

/**
 * Create and open a frameset for a browser window.
 *
 * \param[in,out] bw The browser window to create the frameset for
 * \return NSERROR_OK or error code on faliure
 */
nserror browser_window_create_frameset(struct browser_window *bw);

void browser_window_recalculate_frameset(struct browser_window *bw);
bool browser_window_frame_resize_start(struct browser_window *bw,
		browser_mouse_state mouse, int x, int y,
		browser_pointer_shape *pointer);
void browser_window_resize_frame(struct browser_window *bw, int x, int y);

void browser_window_scroll_callback(void *client_data,
		struct scrollbar_msg_data *scrollbar_data);


/**
 * Create, remove, and update browser window scrollbars
 *
 * \param  bw    The browser window
 */
void browser_window_handle_scrollbars(struct browser_window *bw);

#endif
