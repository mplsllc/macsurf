/*
 * Copyright 2008 Rob Kendrick <rjek@netsurf-browser.org>
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

#ifndef NETSURF_GTK_THROBBER_H
#define NETSURF_GTK_THROBBER_H

/**
 * Initialise global throbber context
 */
nserror nsgtk_throbber_init(void);

/**
 * release global throbber context
 */
void nsgtk_throbber_finalise(void);

/**
 * get the pixbuf of a given frame of the throbber
 *
 * \param frame The frame number starting at 0 for stopped frame
 * \param pixbuf updated on success
 * \return NSERROR_OK and pixbuf updated on success, NSERROR_BAD_SIZE if frame
 *          is out of range else error code.
 */
nserror nsgtk_throbber_get_frame(int frame, GdkPixbuf **pixbuf);

#endif /* NETSURF_GTK_THROBBER_H */
