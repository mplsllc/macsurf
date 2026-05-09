/*
 * MacSurf — lwc_stub.c
 * Minimal libwapcaplet implementation for Mac OS 9.
 * Allocates lwc_string structs with the string data stored
 * immediately after the struct (at (char *)(str + 1)).
 * Licensed under GPL v2.
 */

#include <stdlib.h>
#include <string.h>
#include <libwapcaplet/libwapcaplet.h>

lwc_error lwc_intern_string(const char *s, size_t slen,
		lwc_string **ret)
{
	lwc_string *str;

	str = (lwc_string *)malloc(sizeof(lwc_string) + slen + 1);
	if (str == NULL) {
		return lwc_error_oom;
	}

	memset(str, 0, sizeof(lwc_string));
	str->len = slen;
	str->hash = 0;
	str->refcnt = 1;
	str->insensitive = NULL;
	str->prevptr = NULL;
	str->next = NULL;

	/* Copy string data right after the struct */
	memcpy((char *)(str + 1), s, slen);
	((char *)(str + 1))[slen] = '\0';

	*ret = str;
	return lwc_error_ok;
}

void lwc_string_destroy(lwc_string *str)
{
	if (str != NULL) {
		free(str);
	}
}

lwc_error lwc__intern_caseless_string(lwc_string *str)
{
	if (str != NULL && str->insensitive == NULL) {
		str->insensitive = str;
	}
	return lwc_error_ok;
}
