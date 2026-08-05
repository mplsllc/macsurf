/*
 * MacSurf — macsurf_lwc_compat.c
 *
 * Extracted from misc_stub.c (2026-08-05 cleanup).  Provides the ONE
 * genuinely critical function that was buried in a kitchen-sink file:
 *
 *   lwc_string_caseless_hash_value()
 *
 * CRITICAL: this MUST return the same value as
 *   lwc_string_hash_value(str->insensitive)
 * because libcss's selector hash uses lwc_string_hash_value of the
 * insensitive intern at INSERT time (via _hash_name() in select/hash.c),
 * and then uses lwc_string_caseless_hash_value at FIND time. The two
 * must agree or every find lands in a different bucket than insert,
 * and the selector hash silently returns no matches.
 *
 * The original implementation (pre-fixes12) used a (h*31+lowered_c) hash,
 * which disagreed with libwapcaplet's FNV-1a-style lwc__calculate_lcase_hash.
 * Every CSS selector in every parsed sheet was unreachable as a result;
 * css_select_style consulted an empty hash and returned libcss initial
 * values for every element. No UA or author CSS ever took effect.
 *
 * This file is NEW in MacSurf.mcp — add it alongside misc_stub.c.
 * Licensed under GPL v2.
 */

#include <stddef.h>
#include <libwapcaplet/libwapcaplet.h>

extern lwc_error lwc__intern_caseless_string(lwc_string *str);

lwc_error lwc_string_caseless_hash_value(lwc_string *str, lwc_hash *hash)
{
	lwc_error err;

	if (str == NULL || hash == NULL) return lwc_error_range;

	if (str->insensitive == NULL) {
		err = lwc__intern_caseless_string(str);
		if (err != lwc_error_ok) return err;
	}

	*hash = lwc_string_hash_value(str->insensitive);
	return lwc_error_ok;
}
