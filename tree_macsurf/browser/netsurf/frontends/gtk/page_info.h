/*
 * Copyright 2019 Vincent Sanders <vince@netsurf-browser.org>
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

#ifndef NETSURF_GTK_PAGE_INFO_H
#define NETSURF_GTK_PAGE_INFO_H 1

/**
 * Page information window
 *
 * \param bw the browser window to get page information for
 * \return NSERROR_OK or error code if prompt creation failed.
 */
nserror nsgtk_page_info(struct browser_window *bw);

/**
 * Position the given page information window at the given
 * coordinates.
 *
 * \param pi the page info window to position
 * \param x the X coordinate for the top left of the window
 * \param y the Y coordinate for the top left of the window
 */
void nsgtk_page_info_set_position(struct nsgtk_pi_window *pi, int x, int y);

#endif
