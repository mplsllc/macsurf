/*
 * This file is part of LibCSS.
 * Licensed under the MIT License,
 *                http://www.opensource.org/licenses/mit-license.php
 * Copyright 2026 MacSurf (fixes357)
 *
 * Parse text-decoration-thickness (#44).
 *
 * Accepted forms (V1):
 *   text-decoration-thickness: auto    (default; from-font)
 *   text-decoration-thickness: from-font (treated same as auto for V1)
 *   text-decoration-thickness: <integer px>
 *
 * Length forms beyond integer-px and percent-of-1em deferred to V2.
 *
 * Storage in css_computed_style_i.text_decoration_thickness (int32_t):
 *   0 = auto / from-font (consumer picks platform default)
 *   N>0 = explicit pixel thickness
 *   N<0 = sentinel (reserved for V2 percent or length-with-unit)
 */

#include <assert.h>
#include <string.h>

#include "bytecode/bytecode.h"
#include "bytecode/opcodes.h"
#include "parse/properties/properties.h"
#include "parse/properties/utils.h"

css_error css__parse_text_decoration_thickness(css_language *c,
		const parserutils_vector *vector, int32_t *ctx,
		css_style *result)
{
	int32_t orig_ctx = *ctx;
	css_error error;
	const css_token *token;
	enum flag_value flag_value;
	bool match;

	token = parserutils_vector_peek(vector, *ctx);
	if (token == NULL) return CSS_INVALID;

	flag_value = get_css_flag_value(c, token);
	if (flag_value != FLAG_VALUE__NONE) {
		parserutils_vector_iterate(vector, ctx);
		return css_stylesheet_style_flag_value(result, flag_value,
				CSS_PROP_TEXT_DECORATION_THICKNESS);
	}

	consumeWhitespace(vector, ctx);

	token = parserutils_vector_peek(vector, *ctx);
	if (token == NULL) {
		*ctx = orig_ctx;
		return CSS_INVALID;
	}

	if (token->type == CSS_TOKEN_IDENT &&
			((lwc_string_caseless_isequal(
				token->idata, c->strings[AUTO],
				&match) == lwc_error_ok && match) ||
			 (lwc_string_caseless_isequal(
				token->idata, c->strings[FROM_FONT],
				&match) == lwc_error_ok && match))) {
		parserutils_vector_iterate(vector, ctx);
		error = css__stylesheet_style_appendOPV(result,
				CSS_PROP_TEXT_DECORATION_THICKNESS, 0,
				TEXT_DECORATION_THICKNESS_AUTO);
	} else if ((token->type == CSS_TOKEN_NUMBER ||
			token->type == CSS_TOKEN_DIMENSION) &&
			token->idata != NULL) {
		/* #44 follow-up: accept `Npx` dimension form too, not just a
		 * bare number. css__number_from_lwc_string parses the numeric
		 * prefix ("2" of "2px"); the unit is treated as px. */
		size_t consumed = 0;
		css_fixed num;
		int32_t n;
		num = css__number_from_lwc_string(token->idata, true,
				&consumed);
		parserutils_vector_iterate(vector, ctx);
		n = (int32_t)(num >> 10);
		if (n < 0) n = 0;
		if (n > 1000) n = 1000;

		error = css__stylesheet_style_appendOPV(result,
				CSS_PROP_TEXT_DECORATION_THICKNESS, 0,
				TEXT_DECORATION_THICKNESS_SET);
		if (error != CSS_OK) {
			*ctx = orig_ctx;
			return error;
		}
		error = css__stylesheet_style_vappend(result, 1,
				(css_fixed)n);
	} else {
		*ctx = orig_ctx;
		return CSS_INVALID;
	}

	if (error != CSS_OK) {
		*ctx = orig_ctx;
	}
	return error;
}
