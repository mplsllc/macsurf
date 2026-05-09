/*
 * Copyright 2012 Michael Drake <tlsa@netsurf-browser.org>
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
 * Interface to core window handling.
 *
 * This provides a generalised API for frontends to implement which
 *  allows them to provide a single implementation for general window
 *  operations on their platform.
 *
 * General core implementations (cookie manager, global history,
 *  hotlist and ssl certificate viewer) use this API to perform
 *  operations like drawing and user input in a portable way.
 */

#ifndef _NETSURF_CORE_WINDOW_H_
#define _NETSURF_CORE_WINDOW_H_

struct core_window;
struct rect;

/**
 * drag status passed to drag_status callback
 */
typedef enum {
	CORE_WINDOW_DRAG_NONE,
	CORE_WINDOW_DRAG_SELECTION,
	CORE_WINDOW_DRAG_TEXT_SELECTION,
	CORE_WINDOW_DRAG_MOVE
} core_window_drag_status;

/**
 * Core user interface window function table.
 *
 * function table implementing core window operations
 */
struct core_window_table {
	/**
	 * Invalidate an area of a window.
	 *
	 * The specified area of the window should now be considered
	 *  out of date. If the area is NULL the entire window must be
	 *  invalidated. It is expected that the windowing system will
	 *  then subsequently cause redraw/expose operations as
	 *  necessary.
	 *
	 * \note the frontend should not attempt to actually start the
	 *  redraw operations as a result of this callback because the
	 *  core redraw functions may already be threaded.
	 *
	 * \param[in] cw The core window to invalidate.
	 * \param[in] rect area to redraw or NULL for the entire window area
	 * \return NSERROR_OK on success or appropriate error code
	 */
	nserror (*invalidate)(struct core_window *cw, const struct rect *rect);

	/**
	 * Update the logical extent of the window
	 *
	 * \param[in] cw the core window object
	 * \param[in] width the width in px, or negative if don't care
	 * \param[in] height the height in px, or negative if don't care
	 * \return NSERROR_OK on success or appropriate error code
	 */
	nserror (*set_extent)(struct core_window *cw, int width, int height);

	/**
	 * Scroll the window to given scroll offsets
	 *
	 * Note: Core callers of this may want to look at calling
	 * the `cw_helper_scroll_visible()`, rather than calling
	 * this directly.
	 *
	 * \param[in] cw the core window object
	 * \param[in] x x-scroll value to set
	 * \param[in] y y-scroll value to set
	 * \return NSERROR_OK on success or appropriate error code
	 */
	nserror (*set_scroll)(struct core_window *cw, int x, int y);

	/**
	 * Get the current scroll offsets
	 *
	 * \param[in] cw the core window object
	 * \param[out] returns horizontal scroll in px
	 * \param[out] returns vertical scroll in px
	 * \return NSERROR_OK on success or appropriate error code
	 */
	nserror (*get_scroll)(const struct core_window *cw, int *x, int *y);

	/**
	 * Get window viewport dimensions
	 *
	 * \param[in] cw the core window object
	 * \param[out] width to be set to viewport width in px
	 * \param[out] height to be set to viewport height in px
	 * \return NSERROR_OK on success or appropriate error code
	 */
	nserror (*get_dimensions)(const struct core_window *cw,
			int *width, int *height);

	/**
	 * Inform corewindow owner of drag status
	 *
	 * \param[in] cw the core window object
	 * \param[in] ds the current drag status
	 * \return NSERROR_OK on success or appropriate error code
	 */
	nserror (*drag_status)(struct core_window *cw,
			core_window_drag_status ds);
};


#endif
