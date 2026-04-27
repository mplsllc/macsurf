/*
 * MacSurf — Mac OS 9 frontend for NetSurf
 * macos9_utf8.c — All gui_utf8_table callbacks
 *
 * This file is part of MacSurf, built on the NetSurf engine.
 * Licensed under GPL v2.
 */

#include <stdlib.h>
#include <string.h>

#include "utils/errors.h"
#include "utils/log.h"
#include "netsurf/utf8.h"

#include "macos9/macos9.h"

/*
 * strndup is POSIX.1-2008, not available in CW8 C89.
 * Provide a local implementation for the Mac build.
 */
static char *
macos9_strndup(const char *s, size_t n)
{
	size_t len;
	char *copy;

	for (len = 0; len < n && s[len] != '\0'; len++)
		;

	copy = malloc(len + 1);
	if (copy != NULL) {
		memcpy(copy, s, len);
		copy[len] = '\0';
	}
	return copy;
}
#define strndup macos9_strndup

static nserror
macos9_utf8_to_local(const char *string, size_t len, char **result)
{
	/* TODO: TECConvertText() UTF-8 to MacRoman */
	/* Stub: return a copy (assumes UTF-8 is acceptable) */
	if (len == 0) {
		len = strlen(string);
	}

	*result = strndup(string, len);
	if (*result == NULL) {
		return NSERROR_NOMEM;
	}
	return NSERROR_OK;
}

static nserror
macos9_local_to_utf8(const char *string, size_t len, char **result)
{
	/* TODO: TECConvertText() MacRoman to UTF-8 */
	/* Stub: return a copy (assumes input is valid UTF-8) */
	if (len == 0) {
		len = strlen(string);
	}

	*result = strndup(string, len);
	if (*result == NULL) {
		return NSERROR_NOMEM;
	}
	return NSERROR_OK;
}

/* Field order: utf8_to_local, local_to_utf8 (see include/netsurf/utf8.h) */
static struct gui_utf8_table utf8_table = {
	macos9_utf8_to_local,
	macos9_local_to_utf8
};

struct gui_utf8_table *macos9_utf8_table = &utf8_table;
