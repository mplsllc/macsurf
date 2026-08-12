/*
 * This file is part of LibCSS.
 * Licensed under the MIT License,
 *                http://www.opensource.org/licenses/mit-license.php
 * Copyright 2026 MacSurf (fixes151)
 *
 * Parse -macsurf-grid-col-span.
 *
 * The cssh_css.c preprocessor rewrites the standard CSS grid placement
 * shorthands `grid-column: ...` and `grid-row: ...` into a single
 * `-macsurf-grid-col-span: <packed_int>` declaration. The packed int
 * encodes four placement fields:
 *
 *   bits  0..7  col_span    - 0=unset, 1..254 literal, 255 "fill row"
 *   bits  8..15 col_start   - 0=auto, 1..254 explicit grid line
 *   bits 16..23 row_start   - 0=auto, 1..254 explicit grid line
 *   bits 24..31 row_span    - 0=unset, 1..254 literal, 255 "fill"
 *
 * Bytecode payload after appendOPV(SET): one int32 holding the packed
 * value verbatim. Storage in css_computed_style_i.macsurf_grid_col_span
 * (int32_t).
 *
 * fixes151 - original col-span only; values were a positive integer
 *           clamped to 1..255 (uint8 semantics).
 * fixes158 - full int32 placement; no clamping. Source CSS that only
 *           sets col-span still lands cleanly in the low byte.
 */

#include <assert.h>
#include <string.h>

#include "bytecode/bytecode.h"
#include "bytecode/opcodes.h"
#include "parse/properties/properties.h"
#include "parse/properties/utils.h"

/* Read a positive integer (0..255) from a NUMBER token. Returns -1 on
 * type mismatch or missing token. Advances *ctx if successful. */
static int32_t macsurf_grid__read_int(const parserutils_vector *vector,
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
	if (val > 255) val = 255;
	return val;
}

css_error css__parse_macsurf_grid_col_span(css_language *c,
		const parserutils_vector *vector, int32_t *ctx,
		css_style *result)
{
	int32_t orig_ctx = *ctx;
	css_error error;
	const css_token *token;
	enum flag_value flag_value;
	int32_t col_span;
	int32_t col_start;
	int32_t row_start;
	int32_t row_span;
	int32_t packed;

	token = parserutils_vector_peek(vector, *ctx);
	if (token == NULL) return CSS_INVALID;

	flag_value = get_css_flag_value(c, token);
	if (flag_value != FLAG_VALUE__NONE) {
		parserutils_vector_iterate(vector, ctx);
		return css_stylesheet_style_flag_value(result, flag_value,
				CSS_PROP_MACSURF_GRID_COL_SPAN);
	}

	/* fixes158: read four space-separated unsigned integers
	 * <col_span> <col_start> <row_start> <row_span>. Each is 0..255.
	 * The preprocessor (cssh_css.c) is the only producer; missing
	 * trailing fields fall back to 0. */
	col_span = macsurf_grid__read_int(vector, ctx);
	if (col_span < 0) {
		*ctx = orig_ctx;
		return CSS_INVALID;
	}

	col_start = macsurf_grid__read_int(vector, ctx);
	if (col_start < 0) col_start = 0;
	row_start = macsurf_grid__read_int(vector, ctx);
	if (row_start < 0) row_start = 0;
	row_span = macsurf_grid__read_int(vector, ctx);
	if (row_span < 0) row_span = 0;

	if (col_span == 0 && col_start == 0 && row_start == 0 &&
			row_span == 0) {
		/* All zero - preprocessor only emits this when nothing was
		 * recognised; treat as invalid so libcss drops the decl. */
		*ctx = orig_ctx;
		return CSS_INVALID;
	}

	packed = ((int32_t)((uint32_t)row_span  & 0xFFu) << 24) |
	         ((int32_t)((uint32_t)row_start & 0xFFu) << 16) |
	         ((int32_t)((uint32_t)col_start & 0xFFu) <<  8) |
	         ((int32_t)((uint32_t)col_span  & 0xFFu));

	error = css__stylesheet_style_appendOPV(result,
			CSS_PROP_MACSURF_GRID_COL_SPAN, 0, 0x0080 /* SET */);
	if (error != CSS_OK) {
		*ctx = orig_ctx;
		return error;
	}

	error = css__stylesheet_style_vappend(result, 1, (css_fixed)packed);
	if (error != CSS_OK) {
		*ctx = orig_ctx;
		return error;
	}

	return CSS_OK;
}
