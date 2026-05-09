/*
 * Copyright 2019 Vincent Sanders <vince@netxurf-browser.org>
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

#ifndef NETSURF_UTILS_REGEX_H_
#define NETSURF_UTILS_REGEX_H_

#include "utils/config.h"

#ifdef HAVE_REGEX
#include <sys/types.h>
#include <regex.h>
#else

#define REG_NOMATCH 1

#define REG_EXTENDED 1
#define REG_ICASE (1 << 1)
#define REG_NEWLINE (1 << 2)
#define REG_NOSUB (1 << 3)

typedef ssize_t regoff_t;

typedef struct {
	size_t re_nsub; /* Number of parenthesized subexpressions.*/
} regex_t;


typedef struct {
	regoff_t rm_so; /* Byte offset from start of string to start
			 *  of substring.
			 */
	regoff_t rm_eo; /* Byte offset from start of string of the
			 *  first character after the end of substring.
			 */
} regmatch_t;


int regcomp(regex_t *restrict preg, const char *restrictregex, int cflags);

size_t regerror(int errorcode, const regex_t *restrict preg, char *restrict errbuf, size_t errbuf_size);

int regexec(const regex_t *restrict preg, const char *restrict string, size_t nmatch, regmatch_t pmatch[restrict], int eflags);

void regfree(regex_t *preg);

#endif

#endif
