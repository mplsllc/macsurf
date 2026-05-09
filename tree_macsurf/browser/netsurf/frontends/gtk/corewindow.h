/*
 * Copyright 2016 Vincent Sanders <vince@netsurf-browser.org>
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

#ifndef GTK_COREWINDOW_H
#define GTK_COREWINDOW_H

#include "netsurf/core_window.h"

extern struct core_window_table *nsgtk_core_window_table;

/**
 * nsgtk core window mouse state
 */
struct nsgtk_corewindow_mouse {
	browser_mouse_state state; /**< last event status */
	bool pressed;
	int pressed_x;
	int pressed_y;
	int last_x;
	int last_y;
};

/**
 * nsgtk core window state
 */
struct nsgtk_corewindow {
	/* public variables */
	/** GTK drawable widget */
	GtkDrawingArea *drawing_area;
	/** scrollable area drawing area is within */
	GtkScrolledWindow *scrolled;

	/* private variables */
	/** Input method */
	GtkIMContext *input_method;

	/** mouse state */
	struct nsgtk_corewindow_mouse mouse_state;

	/** drag status set by core */
	core_window_drag_status drag_status;

	/**
	 * callback to draw on drawable area of nsgtk core window
	 *
	 * \param nsgtk_cw The nsgtk core window structure.
	 * \param r The rectangle of the window that needs updating.
	 * \return NSERROR_OK on success otherwise appropriate error code
	 */
	nserror (*draw)(struct nsgtk_corewindow *nsgtk_cw, struct rect *r);

	/**
	 * callback for keypress on nsgtk core window
	 *
	 * \param nsgtk_cw The nsgtk core window structure.
	 * \param nskey The netsurf key code.
	 * \return NSERROR_OK if key processed,
	 *         NSERROR_NOT_IMPLEMENTED if key not processed
	 *         otherwise appropriate error code
	 */
	nserror (*key)(struct nsgtk_corewindow *nsgtk_cw, uint32_t nskey);

	/**
	 * callback for mouse event on nsgtk core window
	 *
	 * \param nsgtk_cw The nsgtk core window structure.
	 * \param mouse_state mouse state
	 * \param x location of event
	 * \param y location of event
	 * \return NSERROR_OK on success otherwise appropriate error code.
	 */
	nserror (*mouse)(struct nsgtk_corewindow *nsgtk_cw, browser_mouse_state mouse_state, int x, int y);
};

/**
 * initialise elements of gtk core window.
 *
 * \param nsgtk_cw A gtk core window structure to initialise
 * \return NSERROR_OK on successful initialisation otherwise error code.
 */
nserror nsgtk_corewindow_init(struct nsgtk_corewindow *nsgtk_cw);

/**
 * finalise elements of gtk core window.
 *
 * \param nsgtk_cw A gtk core window structure to initialise
 * \return NSERROR_OK on successful finalisation otherwise error code.
 */
nserror nsgtk_corewindow_fini(struct nsgtk_corewindow *nsgtk_cw);

#endif
