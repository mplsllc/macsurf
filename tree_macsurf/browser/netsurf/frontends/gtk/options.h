/*
 * Copyright 2012 Vincent Sanders <vince@netsurf-browser.org>
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

#ifndef NETSURF_GTK_OPTIONS_H_
#define NETSURF_GTK_OPTIONS_H_

/* currently nothing here */

#endif

/* clear downloads */
NSOPTION_BOOL(downloads_clear, false)

/* prompt before overwriting downloads */
NSOPTION_BOOL(request_overwrite, true)

/* location to download files to */
NSOPTION_STRING(downloads_directory, NULL)

/* where to store URL database */
NSOPTION_STRING(url_file, NULL)

/* Always show tabs even if there is only one */
NSOPTION_BOOL(show_single_tab, false)

/* size of buttons */
NSOPTION_INTEGER(button_type, 0)

/* number of days to keep history data */
NSOPTION_INTEGER(history_age, 0)

/* show urls in local history browser */
NSOPTION_BOOL(hover_urls, false)

/* new tabs are blank instead of homepage */
NSOPTION_BOOL(new_blank, false)

/* path to save hotlist file */
NSOPTION_STRING(hotlist_path, NULL)

/* Developer information viewer display method */
NSOPTION_INTEGER(developer_view, 0)

/* where tabs are positioned */
NSOPTION_INTEGER(position_tab, 0)

/* Toolbar customisation */
NSOPTION_STRING(toolbar_items, NULL)

/* The menu and tool bars that are shown */
NSOPTION_STRING(bar_show, NULL)
