/*
 * This file is part of LibCSS.
 * Licensed under the MIT License,
 *                http://www.opensource.org/licenses/mit-license.php
 * Copyright 2026 MacSurf (fixes75)
 *
 * Parse -macsurf-grid.
 *
 * Accepted forms (V1):
 *   -macsurf-grid: none
 *   -macsurf-grid: <cols>
 *   -macsurf-grid: <cols> <rows>
 *
 * <cols> and <rows> are bare integers (no unit). rows defaults to 0
 * meaning auto-rows (size to content). Container with display:grid
 * lays out children row-major in `cols` equal-width columns.
 *
 * V1 storage (one int32_t):
 *   bits 31..16: column count (0..65535)
 *   bits 15..0:  row count (0 = auto)
 *
 * Bytecode payload after appendOPV(SET):
 *   - int32_t packed value (cols<<16 | rows)
 */

#include <assert.h>
#include <string.h>

#include "bytecode/bytecode.h"
#include "bytecode/opcodes.h"
#include "parse/properties/properties.h"
#include "parse/properties/utils.h"

css_error css__parse_macsurf_grid(css_language *c,
		const parserutils_vector *vector, int32_t *ctx,
		css_style *result)
{
	int32_t orig_ctx = *ctx;
	css_error error;
	const css_token *token;
	enum flag_value flag_value;
	uint32_t cols = 0;
	uint32_t rows = 0;
	uint32_t packed;

	token = parserutils_vector_peek(vector, *ctx);
	if (token == NULL) return CSS_INVALID;

	flag_value = get_css_flag_value(c, token);
	if (flag_value != FLAG_VALUE__NONE) {
		parserutils_vector_iterate(vector, ctx);
		return css_stylesheet_style_flag_value(result, flag_value,
				CSS_PROP_MACSURF_GRID);
	}

	consumeWhitespace(vector, ctx);

	/* `none` keyword. */
	token = parserutils_vector_peek(vector, *ctx);
	if (token != NULL && token->type == CSS_TOKEN_IDENT &&
			token->idata != NULL) {
		bool match = false;
		if (lwc_string_caseless_isequal(token->idata,
				c->strings[NONE], &match) == lwc_error_ok &&
				match) {
			parserutils_vector_iterate(vector, ctx);
			return css__stylesheet_style_appendOPV(result,
					CSS_PROP_MACSURF_GRID, 0,
					0x0000 /* NONE */);
		}
	}

	/* First integer: column count. */
	token = parserutils_vector_peek(vector, *ctx);
	if (token == NULL || token->type != CSS_TOKEN_NUMBER ||
			token->idata == NULL) {
		*ctx = orig_ctx;
		return CSS_INVALID;
	}
	{
		size_t consumed = 0;
		css_fixed cnum = css__number_from_lwc_string(token->idata,
				true /* int only */, &consumed);
		int32_t cval = (int32_t)(cnum >> 10);  /* Q22.10 -> int */
		if (cval < 1) cval = 1;
		if (cval > 255) cval = 255;
		cols = (uint32_t)cval;
		parserutils_vector_iterate(vector, ctx);
	}

	/* Optional second integer: row count. */
	consumeWhitespace(vector, ctx);
	token = parserutils_vector_peek(vector, *ctx);
	if (token != NULL && token->type == CSS_TOKEN_NUMBER &&
			token->idata != NULL) {
		size_t consumed = 0;
		css_fixed rnum = css__number_from_lwc_string(token->idata,
				true, &consumed);
		int32_t rval = (int32_t)(rnum >> 10);
		if (rval < 0) rval = 0;
		if (rval > 255) rval = 255;
		rows = (uint32_t)rval;
		parserutils_vector_iterate(vector, ctx);
	}

	packed = (cols << 16) | (rows & 0xffff);

	error = css__stylesheet_style_appendOPV(result,
			CSS_PROP_MACSURF_GRID, 0, 0x0080 /* SET */);
	if (error != CSS_OK) return error;

	return css__stylesheet_style_vappend(result, 1, (css_fixed)packed);
}
