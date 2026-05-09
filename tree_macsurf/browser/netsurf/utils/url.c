/*
 * Copyright 2006 Richard Wilson <info@tinct.net>
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
 * \file
 * \brief Implementation of URI percent escaping.
 *
 * Percent encoding of URI is subject to RFC3986 however this is not
 * implementing URI behaviour purely the percent encoding so only the
 * unreserved set is not encoded and arbitrary binary data may be
 * unescaped.
 *
 * \note Earlier RFC (2396, 1738 and 1630) list the tilde ~ character
 * as special so its handling is ambiguious
 */

#include <ctype.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

#include "utils/ascii.h"
#include "utils/config.h"
#include "utils/log.h"
#include "utils/url.h"


/**
 * Convert a hex digit to a hex value
 *
 * Must be called with valid hex char, results undefined otherwise.
 *
 * \param[in] c character to convert to value
 * \return the value of c
 */
static inline char xdigit_to_hex(char c)
{
	if (c >= '0' && c <= '9') {
		return c - '0';
	} else if (c >= 'A' && c <= 'F') {
		return c - 'A' + 10;
	} else {
		return c - 'a' + 10;
	}
}


/* exported interface documented in utils/url.h */
nserror url_unescape(const char *str, size_t length,
		size_t *length_out, char **result_out)
{
	const char *str_end;
	size_t new_len;
	char *res_pos;
	char *result;

	if ((str == NULL) || (result_out == NULL)) {
		return NSERROR_BAD_PARAMETER;
	}

	if (length == 0) {
		length = strlen(str);
	}

	result = malloc(length + 1);
	if (result == NULL) {
		return NSERROR_NOMEM;
	}

	res_pos = result;
	str_end = str + length;
	if (length >= 3) {
		str_end -= 2;
		while (str < str_end) {
			char c = *str;
			char c1 = *(str + 1);
			char c2 = *(str + 2);

			if (c == '%' && ascii_is_hex(c1) && ascii_is_hex(c2)) {
				c = xdigit_to_hex(c1) << 4 | xdigit_to_hex(c2);
				str += 2;
			}
			*res_pos++ = c;
			str++;
		}
		str_end += 2;
	}

	while (str < str_end) {
		*res_pos++ = *str++;
	}

	*res_pos = '\0';
	new_len = res_pos - result;

	if (new_len != length) {
		/* Shrink wrap the allocation around the string */
		char *tmp = realloc(result, new_len + 1);
		if (tmp != NULL) {
			result = tmp;
		}
	}

	if (length_out != NULL) {
		*length_out = new_len;
	}
	*result_out = result;
	return NSERROR_OK;
}


/* exported interface documented in utils/url.h */
nserror url_escape(const char *unescaped, bool sptoplus,
		const char *escexceptions, char **result)
{
	size_t len, new_len;
	char *escaped, *pos;
	const char *c;

	if (unescaped == NULL || result == NULL) {
		return NSERROR_BAD_PARAMETER;
	}

	len = strlen(unescaped);

	escaped = malloc(len * 3 + 1);
	if (escaped == NULL) {
		return NSERROR_NOMEM;
	}
	pos = escaped;

	for (c = unescaped; *c != '\0'; c++) {
		/* Check if we should escape this byte.
		 * '~' is unreserved and should not be percent encoded, if
		 * you believe the spec; however, leaving it unescaped
		 * breaks a bunch of websites, so we escape it anyway. */
		if (!isascii(*c) ||
				(strchr(":/?#[]@" /* gen-delims */
				 "!$&'()*+,;=" /* sub-delims */
				 "<>%\"{}|\\^`~" /* others */, *c) &&
				 (!escexceptions ||
				  !strchr(escexceptions, *c))) ||
				*c <= 0x20 || *c == 0x7f) {
			if (*c == 0x20 && sptoplus) {
				*pos++ = '+';
			} else {
				*pos++ = '%';
				*pos++ = "0123456789ABCDEF"[(*c >> 4) & 0xf];
				*pos++ = "0123456789ABCDEF"[*c & 0xf];
			}
		} else {
			/* unreserved characters: [a-zA-Z0-9-._] */
			*pos++ = *c;
		}
	}
	*pos = '\0';
	new_len = pos - escaped;

	if (new_len != len) {
		/* Shrink wrap the allocation around the escaped string */
		char *tmp = realloc(escaped, new_len + 1);
		if (tmp != NULL) {
			escaped = tmp;
		}
	}

	*result = escaped;
	return NSERROR_OK;
}
