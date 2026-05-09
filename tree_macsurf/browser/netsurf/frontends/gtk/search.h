/*
 * Copyright 2009 Mark Benjamin <netsurf-browser.org.MarkBenjamin@dfgh.net>
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
 * free text page search for gtk interface
 */

#ifndef NETSURF_GTK_SEARCH_H_
#define NETSURF_GTK_SEARCH_H_

extern struct gui_search_table *nsgtk_search_table;

struct gtk_search;

/**
 * create text search context
 *
 * \param builder the gtk builder containing the search toolbar
 * \param bw The browsing context to run the find operations against
 * \param search search context result
 * \return NSERROR_OK and search_out updated
 */
nserror nsgtk_search_create(GtkBuilder *builder, struct browser_window *bw, struct gtk_search **search);

/**
 * update search toolbar size and style
 */
nserror nsgtk_search_restyle(struct gtk_search *search);

/**
 * toggle search bar visibility
 */
nserror nsgtk_search_toggle_visibility(struct gtk_search *search);

#endif
