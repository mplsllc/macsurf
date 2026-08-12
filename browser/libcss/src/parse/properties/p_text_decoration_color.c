/*
 * This file is part of LibCSS.
 * Licensed under the MIT License,
 *                http://www.opensource.org/licenses/mit-license.php
 * Copyright 2026 MacSurf (fixes357)
 *
 * Parse text-decoration-color (#44).
 *
 * Accepted forms:
 *   text-decoration-color: <color>
 *   text-decoration-color: currentcolor (default)
 *
 * Not inherited.
 *
 * Storage in css_computed_style_i.text_decoration_color_status (int32_t):
 *   0 = INHERIT
 *   1 = CURRENTCOLOR (default - follow `color` property)
 *   2 = SET (use the css_color word)
 */

#include <assert.h>
#include <string.h>

#include "bytecode/bytecode.h"
#include "bytecode/opcodes.h"
#include "parse/properties/properties.h"
#include "parse/properties/utils.h"

css_error css__parse_text_decoration_color(css_language *c,
		const parserutils_vector *vector, int32_t *ctx,
		css_style *result)
{
	int32_t orig_ctx = *ctx;
	css_error error;
	const css_token *token;
	bool match;

	token = parserutils_vector_iterate(vector, ctx);
	if (token == NULL) {
		*ctx = orig_ctx;
		return CSS_INVALID;
	}

	if ((token->type == CSS_TOKEN_IDENT) &&
			(lwc_string_caseless_isequal(
			token->idata, c->strings[INHERIT],
			&match) == lwc_error_ok && match)) {
		error = css_stylesheet_style_inherit(result,
				CSS_PROP_TEXT_DECORATION_COLOR);
	} else if ((token->type == CSS_TOKEN_IDENT) &&
			(lwc_string_caseless_isequal(
			token->idata, c->strings[INITIAL],
			&match) == lwc_error_ok && match)) {
		error = css_stylesheet_style_initial(result,
				CSS_PROP_TEXT_DECORATION_COLOR);
	} else if ((token->type == CSS_TOKEN_IDENT) &&
			(lwc_string_caseless_isequal(
			token->idata, c->strings[UNSET],
			&match) == lwc_error_ok && match)) {
		error = css_stylesheet_style_unset(result,
				CSS_PROP_TEXT_DECORATION_COLOR);
	} else if ((token->type == CSS_TOKEN_IDENT) &&
			(lwc_string_caseless_isequal(
			token->idata, c->strings[CURRENTCOLOR],
			&match) == lwc_error_ok && match)) {
		error = css__stylesheet_style_appendOPV(result,
				CSS_PROP_TEXT_DECORATION_COLOR, 0,
				TEXT_DECORATION_COLOR_CURRENT_COLOR);
	} else {
		uint16_t value = 0;
		uint32_t color = 0;
		*ctx = orig_ctx;

		error = css__parse_colour_specifier(c, vector, ctx,
				&value, &color);
		if (error != CSS_OK) {
			*ctx = orig_ctx;
			return error;
		}

		error = css__stylesheet_style_appendOPV(result,
				CSS_PROP_TEXT_DECORATION_COLOR, 0,
				value == COLOR_SET
					? TEXT_DECORATION_COLOR_SET : value);
		if (error != CSS_OK) {
			*ctx = orig_ctx;
			return error;
		}

		if (value == COLOR_SET) {
			error = css__stylesheet_style_append(result, color);
		}
	}

	if (error != CSS_OK)
		*ctx = orig_ctx;

	return error;
}
