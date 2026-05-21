/*
 * This file is part of LibCSS.
 * Licensed under the MIT License,
 *                http://www.opensource.org/licenses/mit-license.php
 * Copyright 2026 MacSurf (fixes159)
 *
 * Parse -macsurf-justify.
 *
 * The cssh_css.c preprocessor rewrites the standard CSS grid alignment
 * shorthands `justify-items: X` and `justify-self: Y` into a single
 * `-macsurf-justify: <justify_items> <justify_self>` declaration. Two
 * space-separated unsigned integers, each 0..15:
 *   0 = unset (inherit container's value at layout time)
 *   1 = start
 *   2 = end
 *   3 = center
 *   4 = stretch
 *   5..15 = reserved
 *
 * Bytecode payload after appendOPV(SET): one int32 packed as
 * (justify_self << 4) | justify_items. Storage in
 * css_computed_style_i.macsurf_justify (int32_t scalar).
 *
 * Why a vendor property: libcss has `align-items` / `align-self` (used
 * by flexbox) but not `justify-items` / `justify-self` (grid-only). The
 * preprocessor pattern from fixes158 avoids adding two full property
 * pipelines for what semantically is one cell-position byte per item.
 */

#include <assert.h>
#include <string.h>

#include "bytecode/bytecode.h"
#include "bytecode/opcodes.h"
#include "parse/properties/properties.h"
#include "parse/properties/utils.h"

/* Read a positive integer (0..15) from a NUMBER token. Returns -1 on
 * type mismatch or missing token. Advances *ctx if successful. */
static int32_t macsurf_justify__read_int(const parserutils_vector *vector,
		int32_t *ctx)
{
	const css_token *t;
	size_t consumed = 0;
	css_fixed num;
	int32_t val;

	consumeWhitespace(vector, ctx);
	t = parserutils_vector_peek(vector, *ctx);
	if (t == NULL || t->type != CSS_TOKEN_NUMBER || t->idata == NULL) {
		return -1;
	}
	num = css__number_from_lwc_string(t->idata, true, &consumed);
	val = (int32_t)(num >> 10);
	parserutils_vector_iterate(vector, ctx);
	if (val < 0) val = 0;
	if (val > 15) val = 15;
	return val;
}

/* fixes159f: bounded probe at the parser entry. If JPARS never fires
 * but JEMIT does, libcss isn't dispatching to us (name-lookup or
 * property_handlers indexing is wrong). If JPARS fires but JCASC
 * doesn't, the crash is between parser-emits-bytecode and cascade.
 * Logger only supports %d/%ld/%p/%s — see project memory. */
extern void macsurf_debug_log_writef(const char *fmt, ...);
extern void macsurf_debug_log_flush(void);
static int macsurf__justify_parse_count = 0;

css_error css__parse_macsurf_justify(css_language *c,
		const parserutils_vector *vector, int32_t *ctx,
		css_style *result)
{
	int32_t orig_ctx = *ctx;
	css_error error;
	const css_token *token;
	enum flag_value flag_value;
	int32_t justify_items;
	int32_t justify_self;
	int32_t packed;
	int log_this = 0;

	if (macsurf__justify_parse_count < 16) {
		log_this = 1;
		macsurf_debug_log_writef(
			"JPARS[%d] enter ctx=%ld",
			macsurf__justify_parse_count,
			(long)*ctx);
		/* fixes159g: explicit flush so the entry-log line is on
		 * disk BEFORE we do anything else. The default log path
		 * skips per-write flushes (fixes96 perf change) and the
		 * previous JPARS rounds were lost in HFS cache when the
		 * crash hit. The flush costs ~10-50ms but only fires up
		 * to 16 times — acceptable for diagnostics. */
		macsurf_debug_log_flush();
		macsurf__justify_parse_count++;
	}

	token = parserutils_vector_peek(vector, *ctx);
	if (token == NULL) {
		if (log_this) {
			macsurf_debug_log_writef("JPARS  early null-token");
			macsurf_debug_log_flush();
		}
		return CSS_INVALID;
	}

	flag_value = get_css_flag_value(c, token);
	if (flag_value != FLAG_VALUE__NONE) {
		parserutils_vector_iterate(vector, ctx);
		if (log_this) {
			macsurf_debug_log_writef(
				"JPARS  flag-value path flag=%d",
				(int)flag_value);
			macsurf_debug_log_flush();
		}
		return css_stylesheet_style_flag_value(result, flag_value,
				CSS_PROP_MACSURF_JUSTIFY);
	}

	justify_items = macsurf_justify__read_int(vector, ctx);
	if (justify_items < 0) {
		if (log_this) {
			macsurf_debug_log_writef("JPARS  ji read failed");
			macsurf_debug_log_flush();
		}
		*ctx = orig_ctx;
		return CSS_INVALID;
	}
	justify_self = macsurf_justify__read_int(vector, ctx);
	if (justify_self < 0) justify_self = 0;

	if (justify_items == 0 && justify_self == 0) {
		if (log_this) {
			macsurf_debug_log_writef("JPARS  both zero -> drop");
			macsurf_debug_log_flush();
		}
		*ctx = orig_ctx;
		return CSS_INVALID;
	}

	packed = ((int32_t)((uint32_t)justify_self  & 0xFu) << 4) |
	         ((int32_t)((uint32_t)justify_items & 0xFu));

	if (log_this) {
		macsurf_debug_log_writef(
			"JPARS  ji=%d js=%d packed=%d before appendOPV",
			(int)justify_items, (int)justify_self, (int)packed);
		macsurf_debug_log_flush();
	}

	error = css__stylesheet_style_appendOPV(result,
			CSS_PROP_MACSURF_JUSTIFY, 0, 0x0080 /* SET */);
	if (error != CSS_OK) {
		if (log_this) {
			macsurf_debug_log_writef(
				"JPARS  appendOPV err=%d",
				(int)error);
			macsurf_debug_log_flush();
		}
		*ctx = orig_ctx;
		return error;
	}

	if (log_this) {
		macsurf_debug_log_writef(
			"JPARS  after appendOPV before vappend");
		macsurf_debug_log_flush();
	}

	error = css__stylesheet_style_vappend(result, 1, (css_fixed)packed);
	if (error != CSS_OK) {
		if (log_this) {
			macsurf_debug_log_writef(
				"JPARS  vappend err=%d",
				(int)error);
			macsurf_debug_log_flush();
		}
		*ctx = orig_ctx;
		return error;
	}

	if (log_this) {
		macsurf_debug_log_writef("JPARS  done OK");
		macsurf_debug_log_flush();
	}

	return CSS_OK;
}
