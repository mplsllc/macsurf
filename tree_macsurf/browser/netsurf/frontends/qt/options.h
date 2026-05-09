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

#ifndef NETSURF_QT_OPTIONS_H_
#define NETSURF_QT_OPTIONS_H_

/* currently nothing here */

#endif

/* path to save hotlist file */
NSOPTION_STRING(hotlist_path, NULL)

NSOPTION_STRING(url_file, NULL)

NSOPTION_BOOL(new_blank, false)

/* page colour selection */
NSOPTION_UINT(colour_selection, 0)
