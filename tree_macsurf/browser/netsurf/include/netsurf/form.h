/*
 * Copyright 2003 Phil Mellor <monkeyson@users.sourceforge.net>
 * Copyright 2003 James Bursa <bursa@users.sourceforge.net>
 * Copyright 2009 Paul Blokus <paul_pl@users.sourceforge.net>
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
 * Form handling public interface.
 */

#ifndef _NETSURF_FORM_H_
#define _NETSURF_FORM_H_

struct form_control;
struct rect;

/** Option in a select. */
struct form_option {
	void *node; /**< Corresponding DOM node */
	bool selected;
	bool initial_selected;
	char *value;
	char *text; /**< NUL terminated. */
	struct form_option* next;
};

/**
 * Process a selection from a form select menu.
 *
 * \param control form control with menu.
 * \param item	  index of item selected from the menu.
 */
nserror form_select_process_selection(struct form_control *control, int item);

/**
 * get a form select menus option.
 *
 * \param control The form control.
 * \param item The index of the menu entry to return.
 * \return The form option at that index.
 */
struct form_option *form_select_get_option(struct form_control *control, int item);

/**
 * Get a form control name
 *
 * \param control The form control
 * \return The form control name
 */
char *form_control_get_name(struct form_control *control);


/**
 * Get a form control bounding rectangle
 *
 * \param[in] control The form control
 * \param[out] r The rectangle to place the bounds in.
 * \return NSERROR_OK on success or error code.
 */
nserror form_control_bounding_rect(struct form_control *control, struct rect *r);

#endif
