/*
 * Copyright 2014 Vincent Sanders <vince@netsurf-browser.org>
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

#ifndef _NETSURF_GTK_VIEWDATA_H_
#define _NETSURF_GTK_VIEWDATA_H_

/**
 * Display text to a user.
 *
 * The data is utf-8 encoded text and will be presented in a window, a
 * tab or an editor as per the user configuration.
 *
 * \param title The title of the data being displayed.
 * \param filename The suggested filename to be used.
 * \param data The data to be shown. This data will be freed once the
 *             display is complete, the caller no longer owns the allocation.
 * \param data_size The size of the data in data.
 */
nserror nsgtk_viewdata(const char *title, const char *filename, char *data, size_t data_size);

/**
 * Display file to a user.
 *
 * The file is interpreted as utf-8 encoded text and will be presented
 * in a window, a tab or an editor as per the user configuration.
 *
 * \param title The title of the data being displayed.
 * \param leafname The suggested leafname to be used.
 * \param filename The filename of the data to be viewed.
 */
nserror nsgtk_viewfile(const char *title, const char *leafname, const char *filename);

#endif
