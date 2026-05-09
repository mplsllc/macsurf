/*
 * Copyright 2005 James Bursa <bursa@users.sourceforge.net>
 * Copyright 2005 John M Bell <jmb202@ecs.soton.ac.uk>
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
 * \file utils/url.h
 * \brief Interface to URL parsing and joining operations.
 */

#ifndef _NETSURF_UTILS_URL_H_
#define _NETSURF_UTILS_URL_H_

#include "utils/errors.h"


/**
 * Escape a string suitable for inclusion in an URL.
 *
 * \param[in]  unescaped      the unescaped string
 * \param[in]  sptoplus       true iff spaces should be converted to +
 * \param[in]  escexceptions  NULL or a string of characters to be excluded
 *                            from escaping.
 * \param[out] result         Returns pointer to buffer to escaped string.
 *                            Returned string is '\0' terminated.
 * \return  NSERROR_OK on success
 */
nserror url_escape(const char *unescaped, bool sptoplus,
		const char *escexceptions, char **result);


/**
 * Convert an escaped string to plain.
 *
 * \param[in]  str         String to unescape.
 * \param[in]  length      Length of string or 0 to use strlen.
 * \param[out] length_out  Iff non-NULL, value updated to length of returned
 *                         result_out string (excluding trailing '\0'`).
 * \param[out] result_out  Returns unescaped string, owned by caller.
 *                         Must be freed with free().
 *                         Returned string has trailing '\0'.
 * \return NSERROR_OK on success
 */
nserror url_unescape(const char *str, size_t length,
		size_t *length_out, char **result_out);

#endif
