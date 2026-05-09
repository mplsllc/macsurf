/*
 * Copyright 2008 Michael Lester <element3260@gmail.com>
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

#ifndef GTK_DOWNLOAD_H
#define GTK_DOWNLOAD_H

/**
 * download operation table for gtk
 */
extern struct gui_download_table *nsgtk_download_table;


/**
 * Initialise download window ready for use.
 *
 * \return NSERROR_OK on success else appropriate error code on faliure.
 */
nserror nsgtk_download_init(void);


/**
 * Destroy download window
 */
void nsgtk_download_destroy(void);


/**
 * Check with user if download is in progress they want to complete
 *
 * \param parent The parent window for the prompt dialog.
 * \return true if the user wants to continue else false.
 */
bool nsgtk_check_for_downloads(GtkWindow *parent);


/**
 * Show the download window
 *
 * \param parent The parent window to use for the shown window
 */
void nsgtk_download_show(GtkWindow *parent);

#endif
