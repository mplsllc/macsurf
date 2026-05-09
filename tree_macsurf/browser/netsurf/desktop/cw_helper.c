/*
 * Copyright 2019 Daniel Silverstone <dsilvers@netsurf-browser.org>
 * Copyright 2019 Michael Drake <tlsa@netsurf-browser.org>
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
 *
 * Helpers to simplify core use of corewindow.
 */

#include "utils/errors.h"
#include "netsurf/browser.h"
#include "netsurf/core_window.h"
#include "netsurf/types.h"
#include "css/utils.h"
#include "desktop/cw_helper.h"
#include "desktop/gui_internal.h"

/* exported interface documented in cw_helper.h */
nserror cw_helper_scroll_visible(struct core_window *cw_h, const struct rect *r)
{
	nserror err;
	int height;
	int width;
	int x0;
	int y0;
	int x1;
	int y1;

	assert(cw_h != NULL);

	err = guit->corewindow->get_dimensions(cw_h, &width, &height);
	if (err != NSERROR_OK) {
		return err;
	}

	guit->corewindow->get_scroll(cw_h, &x0, &y0);
	if (err != NSERROR_OK) {
		return err;
	}

	y1 = y0 + height;
	x1 = x0 + width;

	if (r->y1 > y1) {
		/* The bottom of the rectangle is off the bottom of the
		 * window, so scroll down to fit it
		 */
		y0 = r->y1 - height;
	}
	if (r->y0 < y0) {
		/* The top of the rectangle is off the top of the window,
		 * so scroll up to fit it
		 */
		y0 = r->y0;
	}
	if (r->x1 > x1) {
		/* The right of the rectangle is off the right of the window
		 * so scroll right to fit it
		 */
		x0 = r->x1 - width;
	}
	if (r->x0 < x0) {
		/* The left of the rectangle is off the left of the window
		 * so scroll left to fit it
		 */
		x0 = r->x0;
	}

	return guit->corewindow->set_scroll(cw_h, x0, y0);
}
