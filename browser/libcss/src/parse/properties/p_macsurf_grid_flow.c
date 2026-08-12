/*
 * This file is part of LibCSS.
 * Licensed under the MIT License,
 *                http://www.opensource.org/licenses/mit-license.php
 * Copyright 2026 MacSurf (fixes275, #65)
 *
 * Parse -macsurf-grid-flow.
 *
 * The cssh_css.c preprocessor rewrites the standard CSS
 * `grid-auto-flow: VALUE` declarations into `-macsurf-grid-flow: N`
 * where N is a small integer matching enum css_macsurf_grid_flow_e:
 *
 *   0 = inherit (preprocessor never emits this)
 *   1 = row (default sparse row-major)
 *   2 = column
 *   3 = row dense
 *   4 = column dense
 *
 * Bytecode payload after appendOPV(SET): one int32 holding the value.
 * Storage in css_computed_style_i.macsurf_grid_flow (int32_t).
 */

#include <assert.h>
#include <string.h>

#include "bytecode/bytecode.h"
#include "bytecode/opcodes.h"
#include "parse/properties/properties.h"
#include "parse/properties/utils.h"

css_error css__parse_macsurf_grid_flow(css_language *c,
		const parserutils_vector *vector, int32_t *ctx,
		css_style *result)
{
	int32_t orig_ctx = *ctx;
	css_error error;
	const css_token *token;
	enum flag_value flag_value;
	css_fixed num;
	size_t consumed = 0;
	int32_t val;

	token = parserutils_vector_peek(vector, *ctx);
	if (token == NULL) return CSS_INVALID;

	flag_value = get_css_flag_value(c, token);
	if (flag_value != FLAG_VALUE__NONE) {
		parserutils_vector_iterate(vector, ctx);
		return css_stylesheet_style_flag_value(result, flag_value,
				CSS_PROP_MACSURF_GRID_FLOW);
	}

	consumeWhitespace(vector, ctx);
	token = parserutils_vector_peek(vector, *ctx);
	if (token == NULL || token->type != CSS_TOKEN_NUMBER ||
			token->idata == NULL) {
		*ctx = orig_ctx;
		return CSS_INVALID;
	}
	num = css__number_from_lwc_string(token->idata, true, &consumed);
	val = (int32_t)(num >> 10);
	parserutils_vector_iterate(vector, ctx);

	if (val < 1 || val > 4) {
		/* Anything outside the recognised enum range - drop. */
		*ctx = orig_ctx;
		return CSS_INVALID;
	}

	error = css__stylesheet_style_appendOPV(result,
			CSS_PROP_MACSURF_GRID_FLOW, 0, 0x0080 /* SET */);
	if (error != CSS_OK) {
		*ctx = orig_ctx;
		return error;
	}

	error = css__stylesheet_style_vappend(result, 1, (css_fixed)val);
	if (error != CSS_OK) {
		*ctx = orig_ctx;
		return error;
	}

	return CSS_OK;
}
