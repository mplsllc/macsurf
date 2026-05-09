/*
 * Copyright 2017 Vincent Sanders <vince@kyllikki.org>
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
 * Interface to GTK local history manager
 */

#ifndef NSGTK_LOCAL_HISTORY_H
#define NSGTK_LOCAL_HISTORY_H

struct browser_window;

/**
 * make the local history window visible.
 *
 * \return NSERROR_OK on success else appropriate error code on faliure.
 */
nserror nsgtk_local_history_present(GtkWindow *parent, struct browser_window *bw);

/**
 * set the local history window position.
 */
void nsgtk_local_history_set_position(int x, int y);

/**
 * hide the local history window from being visible.
 *
 * \return NSERROR_OK on success else appropriate error code on faliure.
 */
nserror nsgtk_local_history_hide(void);

/**
 * Destroys the local history window and performs any other necessary cleanup
 * actions.
 */
nserror nsgtk_local_history_destroy(void);

#endif
