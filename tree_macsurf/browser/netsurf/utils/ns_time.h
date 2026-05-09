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
 * \file utils/time.h
 * \brief Interface to time operations.
 */

#ifndef _NETSURF_UTILS_TIME_H_
#define _NETSURF_UTILS_TIME_H_

#include <time.h>

/**
 * Write the time in seconds since epoch to a buffer.
 *
 * This is provided as strftime is not generally portable.
 *
 * @param str The destination buffer.
 * @param size The length of the destination buffer.
 * @param timep The pointer to the time to write.
 * @return The length of the string written.
 */
int nsc_sntimet(char *str, size_t size, time_t *timep);

/**
 * Parse time in seconds since epoc.
 *
 * This is provided as strptime is not generally portable.
 *
 * @param str The source buffer.
 * @param size The length of the source buffer.
 * @param timep Pointer to result.
 * @return NSERROR_OK on success or error code on faliure.
 */
nserror nsc_snptimet(const char *str, size_t size, time_t *timep);


/**
 * Converts a date string to a number of seconds since epoch
 *
 * returns the number of seconds since the Epoch, January 1st 1970
 * 00:00:00 in the UTC time zone, for the date and time that the
 * \a str parameter specifies.
 *
 * datetime strings passed must be in one of the formats specified in:
 *  - RFC 822 (updated in RFC 1123) using time zone name or time zone delta
 *  - RFC 850 (obsoleted by RFC 1036)
 *  - ANSI C's asctime() format.
 *
 * @param[in] str The datetime string to parse
 * @param[in] size The length of the source string
 * @param[out] timep Pointer to result on success unmodified on error.
 * @return NSERROR_OK on success and timep updated else
 *          NSERROR_INVALID if the string parsing failed otherwise a suitable
 *          error code
 */
nserror nsc_strntimet(const char *str, size_t size, time_t *timep);

/**
 * Create an RFC 1123 compliant date string from a Unix timestamp
 *
 * \param t The timestamp to consider
 * \return Pointer to buffer containing string - invalidated by next call.
 */
const char *rfc1123_date(time_t t);

#endif
