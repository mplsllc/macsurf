/*
 * Copyright 2006 Rob Kendrick <rjek@rjek.com>
 * Copyright 2009 Paul Blokus <paul_pl@users.sourceforge.net> 
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
 * Interface to GTK global history manager
 */

#ifndef __NSGTK_HISTORY_H__
#define __NSGTK_HISTORY_H__

/**
 * make the global history window visible.
 *
 * \return NSERROR_OK on success else appropriate error code on faliure.
 */
nserror nsgtk_global_history_present(void);

/**
 * Destroys the global history window and performs any other necessary cleanup
 * actions.
 */
nserror nsgtk_global_history_destroy(void);

#endif
