/*
 * Copyright 2003 Phil Mellor <monkeyson@users.sourceforge.net>
 * Copyright 2006 James Bursa <bursa@users.sourceforge.net>
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
 * Browser interfaces.
 */

#ifndef NETSURF_BROWSER_H_
#define NETSURF_BROWSER_H_

/**
 * Set the DPI of the browser.
 *
 * \param dpi The DPI to set.
 */
nserror browser_set_dpi(int dpi);

/**
 * Get the browser DPI.
 *
 * \return The DPI in use.
 */
int browser_get_dpi(void);

#endif
