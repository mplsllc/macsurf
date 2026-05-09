/*
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

#ifndef NETSURF_DESKTOP_CW_HELPER_H_
#define NETSURF_DESKTOP_CW_HELPER_H_

struct rect;
struct core_window;

/**
 * Scroll a core window to make the given rectangle visible.
 *
 * \param[in] cw_t  The core window callback table to use.
 * \param[in] cw_h  The core window's handle.
 * \param[in] r     The rectangle to make visisble by scrolling.
 * \return NSERROR_OK on success or appropriate error code
 */
nserror cw_helper_scroll_visible(struct core_window *cw_h, const struct rect *r);

#endif
