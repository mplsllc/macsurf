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

/**
 * \file
 *
 * Interface to platform-specific utf8 operations.
 */

#ifndef _NETSURF_UTF8_H_
#define _NETSURF_UTF8_H_

/**
 * User interface utf8 characterset conversion routines.
 */
struct gui_utf8_table {
	/**
	 * Convert a UTF-8 encoded string into the system local encoding
	 *
	 * \param string The string to convert
	 * \param len The length (in bytes) of the string, or 0
	 * \param result Pointer to location in which to store result
	 * \return An nserror code
	 */
	nserror (*utf8_to_local)(const char *string, size_t len, char **result);

	/**
	 * Convert a string encoded in the system local encoding to UTF-8
	 *
	 * \param string The string to convert
	 * \param len The length (in bytes) of the string, or 0
	 * \param result Pointer to location in which to store result
	 * \return An nserror code
	 */
	nserror (*local_to_utf8)(const char *string, size_t len, char **result);
};

#endif
