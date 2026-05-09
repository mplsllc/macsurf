/*
 * Copyright 2007 Rob Kendrick <rjek@netsurf-browser.org>
 * Copyright 2004-2007 James Bursa <bursa@users.sourceforge.net>
 * Copyright 2003 Phil Mellor <monkeyson@users.sourceforge.net>
 * Copyright 2003 John M Bell <jmb202@ecs.soton.ac.uk>
 * Copyright 2004 John Tytgat <joty@netsurf-browser.org>
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

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include "utils/errors.h"
#include "utils/messages.h"
#include "utils/dirent.h"
#include "utils/inet.h"
#include "utils/string.h"
#include "utils/utils.h"

/* exported interface documented in utils/string.h */
char *squash_whitespace(const char *s)
{
	char *c;
	int i = 0, j = 0;

	assert(s != NULL);

	c = malloc(strlen(s) + 1);
	if (c != NULL) {
		do {
			if (s[i] == ' ' ||
			    s[i] == '\n' ||
			    s[i] == '\r' ||
			    s[i] == '\t') {
				c[j++] = ' ';
				while (s[i] == ' ' ||
				       s[i] == '\n' ||
				       s[i] == '\r' ||
				       s[i] == '\t')
					i++;
			}
			c[j++] = s[i++];
		} while (s[i - 1] != 0);
	}
	return c;
}


/* exported interface documented in utils/utils.h */
char *cnv_space2nbsp(const char *s)
{
	const char *srcP;
	char *d, *d0;
	unsigned int numNBS;
	/* Convert space & TAB into non breaking space character (0xA0) */
	for (numNBS = 0, srcP = (const char *)s; *srcP != '\0'; ++srcP) {
		if (*srcP == ' ' || *srcP == '\t') {
			++numNBS;
                }
        }
	if ((d = (char *)malloc((srcP - s) + numNBS + 1)) == NULL) {
		return NULL;
        }
	for (d0 = d, srcP = (const char *)s; *srcP != '\0'; ++srcP) {
		if (*srcP == ' ' || *srcP == '\t') {
			*d0++ = 0xC2;
			*d0++ = 0xA0;
		} else
			*d0++ = *srcP;
	}
	*d0 = '\0';
	return d;
}


/* exported interface documented in utils/utils.h */
bool is_dir(const char *path)
{
	struct stat s;

	if (stat(path, &s)) {
		return false;
        }

	return S_ISDIR(s.st_mode) ? true : false;
}


/* exported interface documented in utils/utils.h */
nserror vsnstrjoin(char **str, size_t *size, char sep, size_t nelm, va_list ap)
{
	const char *elm[16];
	size_t elm_len[16];
	size_t elm_idx;
	char *fname;
	size_t fname_len = 0;
	char *curp;

	/* check the parameters are all sensible */
	if ((nelm == 0) || (nelm > 16)) {
		return NSERROR_BAD_PARAMETER;
	}
	if ((*str != NULL) && (size == NULL)) {
		/* if the caller is providing the buffer they must say
		 * how much space is available.
		 */
		return NSERROR_BAD_PARAMETER;
	}

	/* calculate how much storage we need for the complete path
	 * with all the elements.
	 */
	for (elm_idx = 0; elm_idx < nelm; elm_idx++) {
		elm[elm_idx] = va_arg(ap, const char *);
		/* check the argument is not NULL */
		if (elm[elm_idx] == NULL) {
			return NSERROR_BAD_PARAMETER;
		}
		elm_len[elm_idx] = strlen(elm[elm_idx]);
		fname_len += elm_len[elm_idx];
	}
	fname_len += nelm; /* allow for separators and terminator */

	/* ensure there is enough space */
	fname = *str;
	if (fname != NULL) {
		if (fname_len > *size) {
			return NSERROR_NOSPACE;
		}
	} else {
		fname = malloc(fname_len);
		if (fname == NULL) {
			return NSERROR_NOMEM;
		}
	}

	/* copy the elements in with apropriate separator */
	curp = fname;
	for (elm_idx = 0; elm_idx < nelm; elm_idx++) {
		memmove(curp, elm[elm_idx], elm_len[elm_idx]);
		curp += elm_len[elm_idx];
		/* ensure string are separated */
		if (curp[-1] != sep) {
			*curp = sep;
			curp++;
		}
	}
	curp[-1] = 0; /* NULL terminate */

	assert((curp - fname) <= (int)fname_len);

	*str = fname;
	if (size != NULL) {
		*size = fname_len;
	}

	return NSERROR_OK;
}


/* exported interface documented in utils/utils.h */
nserror snstrjoin(char **str, size_t *size, char sep, size_t nelm, ...)
{
	va_list ap;
	nserror ret;

	va_start(ap, nelm);
	ret = vsnstrjoin(str, size, sep, nelm, ap);
	va_end(ap);

	return ret;
}


/**
 * The size of buffers within human_friendly_bytesize.
 *
 * We can have a fairly good estimate of the output buffers maximum length.
 *
 * The unsigned long long int can store a value representing a maximum
 *   size of 16 EiB (exibytes).  Therefore the greatest space required is to
 *   represent 1023 PiB.
 * Currently that would be represented as "1023.00PiBytes" in english
 *   giving a 15 byte length including a null terminator.
 * Ideally we would be able to accurately know this length for other
 *   languages, in the mean time a largeish buffer size is selected
 *   and should suffice.
 */
#define BYTESIZE_BUFFER_SIZE 32

/* exported interface documented in utils/string.h */
char *human_friendly_bytesize(unsigned long long int bsize) {
	static char buffer1[BYTESIZE_BUFFER_SIZE];
	static char buffer2[BYTESIZE_BUFFER_SIZE];
	static char buffer3[BYTESIZE_BUFFER_SIZE];
	static char *curbuffer = buffer3;
	enum {
	      bytes,
	      kilobytes,
	      megabytes,
	      gibibytes,
	      tebibytes,
	      pebibytes,
	      exbibytes	} unit = bytes;
	static const char *const units[] = {
		"Bytes",
		"KiBytes",
		"MiBytes",
		"GiBytes",
		"TiBytes",
		"PiBytes",
		"EiBytes" };
	double bytesize = (double)bsize;
	const char *fmt;

	if (curbuffer == buffer1)
		curbuffer = buffer2;
	else if (curbuffer == buffer2)
		curbuffer = buffer3;
	else
		curbuffer = buffer1;

	if (bytesize > 1024) {
		bytesize /= 1024;
		unit = kilobytes;
	}

	if (bytesize > 1024) {
		bytesize /= 1024;
		unit = megabytes;
	}

	if (bytesize > 1024) {
		bytesize /= 1024;
		unit = gibibytes;
	}

	if (bytesize > 1024) {
		bytesize /= 1024;
		unit = tebibytes;
	}

	if (bytesize > 1024) {
		bytesize /= 1024;
		unit = pebibytes;
	}

	if (bytesize > 1024) {
		bytesize /= 1024;
		unit = exbibytes;
	}

	if (unit == bytes) {
		fmt = "%.0f%s";
	} else {
		fmt = "%3.2f%s";
	}

	snprintf(curbuffer,
		 BYTESIZE_BUFFER_SIZE,
		 fmt,
		 bytesize,
		 messages_get(units[unit]));

	return curbuffer;
}


#ifndef HAVE_STRTOULL
#include <stdlib.h>

/**
 * string to unsigned long long
 *
 */
unsigned long long int strtoull(const char *nptr, char **endptr, int base)
{
	return (unsigned long long int)strtoul(nptr, endptr, base);
}

#endif


#ifndef HAVE_STRCASESTR

/**
 * Case insensitive strstr implementation
 *
 * \param haystack String to search in
 * \param needle String to look for
 * \return Pointer to start of found substring, or NULL if not found
 */
char *strcasestr(const char *haystack, const char *needle)
{
	size_t needle_len = strlen(needle);
	const char * last_start = haystack + (strlen(haystack) - needle_len);

	while (haystack <= last_start) {
		if (strncasecmp(haystack, needle, needle_len) == 0)
			return (char *)haystack;
		haystack++;
	}

	return NULL;
}

#endif


#ifndef HAVE_STRNDUP

/**
 * Duplicate up to n characters of a string.
 */
char *strndup(const char *s, size_t n)
{
	size_t len;
	char *s2;

	for (len = 0; len != n && s[len]; len++)
		continue;

	s2 = malloc(len + 1);
	if (!s2)
		return 0;

	memcpy(s2, s, len);
	s2[len] = 0;
	return s2;
}

#endif


#ifndef HAVE_SCANDIR

/* exported function documented in utils/dirent.h */
int alphasort(const struct dirent **d1, const struct dirent **d2)
{
	return strcasecmp((*d1)->d_name, (*d2)->d_name);
}

/* exported function documented in utils/dirent.h */
int scandir(const char *dir, struct dirent ***namelist,
		int (*sel)(const struct dirent *),
		int (*compar)(const struct dirent **, const struct dirent **))
{
	struct dirent **entlist = NULL;
	struct dirent **entlist_temp = NULL;
	struct dirent *ent = NULL, *new_ent;
	int alloc_n = 0;
	int n = 0;
	MAC_DIR *d;

	d = opendir(dir);
	if (d == NULL) {
		goto error;
	}

	while ((ent = readdir(d)) != NULL) {
		/* Avoid entries that caller doesn't want */
		if (sel && (*sel)(ent) == 0)
			continue;

		/* Ensure buffer is big enough to list this entry */
		if (n == alloc_n) {
			alloc_n *= 4;
			if (alloc_n == 0) {
				alloc_n = 32;
			}
			entlist_temp = realloc(entlist,
					sizeof(*entlist) * alloc_n);
			if (entlist_temp == NULL) {
				goto error;
			}
			entlist = entlist_temp;
		}

		/* Make copy of ent */
		new_ent = malloc(sizeof(*new_ent));
		if (new_ent == NULL) {
			goto error;
		}
		memcpy(new_ent, ent, sizeof(struct dirent));

		/* Make list entry point to this copy of ent */
		entlist[n] = new_ent;

		n++;
	}

	closedir(d);

	/* Sort */
	if (compar != NULL && n > 1)
		qsort(entlist, n, sizeof(*entlist),
				(int (*)(const void *, const void *))compar);
	*namelist = entlist;
	return n;

error:

	if (entlist != NULL) {
		int i;
		for (i = 0; i < n; i++) {
			free(entlist[i]);
		}
		free(entlist);
	}

	if (d != NULL) {
		closedir(d);
	}

	return -1;
}

#endif


#ifndef HAVE_STRCHRNUL

/**
 *  Find the first occurrence of C in S or the final NUL byte.
 */
char *strchrnul (const char *s, int c_in)
{
	const unsigned char *us = (const unsigned char *) s;

	while (*us != c_in && *us != '\0')
		us++;

	return (void *) us;
}

#endif

#ifndef HAVE_UTSNAME

#include "utils/utsname.h"

int uname(struct utsname *buf) {
	strcpy(buf->sysname,"windows");
	strcpy(buf->nodename,"nodename");
	strcpy(buf->release,"release");
	strcpy(buf->version,"version");
	strcpy(buf->machine,"pc");

	return 0;
}

#endif

#ifndef HAVE_REALPATH

char *realpath(const char *path, char *resolved_path)
{
	char *ret;
	if (resolved_path == NULL) {
		ret=strdup(path);
	} else {
		ret = resolved_path;
		strcpy(resolved_path, path);
	}
	return ret;
}

#endif

#ifndef HAVE_INETATON

int inet_aton(const char *cp, struct in_addr *inp)
{
	unsigned int b1, b2, b3, b4;
	unsigned char c;

	if (strspn(cp, "0123456789.") < strlen(cp))
		return 0;

	if (sscanf(cp, "%3u.%3u.%3u.%3u%c", &b1, &b2, &b3, &b4, &c) != 4)
		return 0;

	if ((b1 > 255) || (b2 > 255) || (b3 > 255) || (b4 > 255))
		return 0;

	inp->s_addr = b4 << 24 | b3 << 16 | b2 << 8 | b1;

	return 1;
}

#endif

#ifndef HAVE_INETPTON

int inet_pton(int af, const char *src, void *dst)
{
	int ret;

	if (af == AF_INET) {
		ret = inet_aton(src, dst);
	}
#if !defined(NO_IPV6)
	else if (af == AF_INET6) {
		/* TODO: implement v6 address support */
		ret = -1;
		errno = EAFNOSUPPORT;
	}
#endif
	else {
		ret = -1;
		errno = EAFNOSUPPORT;
	}

	return ret;
}

#endif


#ifndef HAVE_REGEX

#include "utils/regex.h"

int
regcomp(regex_t *restrict preg, const char *restrictregex, int cflags)
{
	return 0;
}

size_t
regerror(int errorcode,
	 const regex_t *restrict preg,
	 char *restrict errbuf,
	 size_t errbuf_size)
{
	if ((errbuf != NULL) && (errbuf_size != 0)) {
		*errbuf = 0;
	}
	return 0;
}

int
regexec(const regex_t *restrict preg,
	const char *restrict string,
	size_t nmatch,
	regmatch_t pmatch[restrict],
	int eflags)
{
	return REG_NOMATCH;
}

void regfree(regex_t *preg)
{
}

#endif
