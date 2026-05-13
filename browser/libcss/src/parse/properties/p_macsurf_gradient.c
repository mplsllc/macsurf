/*
 * This file is part of LibCSS.
 * Licensed under the MIT License,
 *                http://www.opensource.org/licenses/mit-license.php
 * Copyright 2025 Gemini CLI
 */

#include <assert.h>
#include <string.h>

#include "bytecode/bytecode.h"
#include "bytecode/opcodes.h"
#include "parse/properties/properties.h"
#include "parse/properties/utils.h"

/**
 * Parse -macsurf-gradient (simplified linear-gradient)
 */
css_error css__parse_macsurf_gradient(css_language *c,
		const parserutils_vector *vector, int32_t *ctx,
		css_style *result)
{
	int32_t orig_ctx = *ctx;
	css_error error;
	const css_token *token;
	css_color color1 = 0, color2 = 0;
	uint16_t type1 = 0, type2 = 0;
	enum flag_value flag_value;
	bool match = false;

	token = parserutils_vector_peek(vector, *ctx);
	if (token == NULL) return CSS_INVALID;

	flag_value = get_css_flag_value(c, token);
	if (flag_value != FLAG_VALUE__NONE) {
		parserutils_vector_iterate(vector, ctx);
		return css_stylesheet_style_flag_value(result, flag_value, CSS_PROP_MACSURF_GRADIENT);
	}

	/* Look for linear-gradient(...).
	 *
	 * fixes44 -- inverted-predicate fix (see git log).
	 * fixes48 -- detect optional `to <side>` direction prefix.
	 *   linear-gradient(to right, c1, c2)  -> horizontal
	 *   linear-gradient(to left, c1, c2)   -> horizontal (renderer
	 *                                          treats as right; cheap
	 *                                          first-cut)
	 *   linear-gradient(to top|bottom, c1, c2) -> vertical
	 *   linear-gradient(c1, c2)            -> vertical (default).
	 * Direction is encoded in the emitted OPV value:
	 *   0x0080 = SET vertical (default, matches fixes37)
	 *   0x00C0 = SET horizontal
	 */
	if (token->type == CSS_TOKEN_FUNCTION &&
	    lwc_string_caseless_isequal(token->idata,
	        c->strings[LINEAR_GRADIENT], &match) == lwc_error_ok &&
	    match) {
		bool horizontal = false;
		uint16_t set_value = 0x0080;
		parserutils_vector_iterate(vector, ctx);

		/* Optional `to <side>` direction prefix. */
		token = parserutils_vector_peek(vector, *ctx);
		if (token != NULL && token->type == CSS_TOKEN_IDENT) {
			bool to_match = false;
			if (lwc_string_caseless_isequal(token->idata,
					c->strings[TO], &to_match)
					== lwc_error_ok && to_match) {
				parserutils_vector_iterate(vector, ctx);
				/* Next ident: right|left|top|bottom */
				token = parserutils_vector_peek(vector, *ctx);
				if (token != NULL &&
				    token->type == CSS_TOKEN_IDENT) {
					bool side_match = false;
					if ((lwc_string_caseless_isequal(
					        token->idata,
					        c->strings[LIBCSS_RIGHT],
					        &side_match) == lwc_error_ok
					     && side_match) ||
					    (lwc_string_caseless_isequal(
					        token->idata,
					        c->strings[LIBCSS_LEFT],
					        &side_match) == lwc_error_ok
					     && side_match)) {
						horizontal = true;
					}
					parserutils_vector_iterate(vector, ctx);
				}
				/* Optional comma after direction. */
				token = parserutils_vector_peek(vector, *ctx);
				if (token != NULL && token->type == CSS_TOKEN_CHAR &&
				    lwc_string_data(token->idata)[0] == ',')
					parserutils_vector_iterate(vector, ctx);
			}
		}

		if (horizontal) {
			set_value = 0x00C0;
		}

		/* Parse first color */
		error = css__parse_colour_specifier(c, vector, ctx, &type1, &color1);
		if (error != CSS_OK) { *ctx = orig_ctx; return error; }

		/* Optional comma */
		token = parserutils_vector_peek(vector, *ctx);
		if (token != NULL && token->type == CSS_TOKEN_CHAR && lwc_string_data(token->idata)[0] == ',')
			parserutils_vector_iterate(vector, ctx);

		/* Parse second color */
		error = css__parse_colour_specifier(c, vector, ctx, &type2, &color2);
		if (error != CSS_OK) { *ctx = orig_ctx; return error; }

		/* Close parenthesis */
		token = parserutils_vector_peek(vector, *ctx);
		if (token != NULL && token->type == CSS_TOKEN_CHAR && lwc_string_data(token->idata)[0] == ')')
			parserutils_vector_iterate(vector, ctx);

		error = css__stylesheet_style_appendOPV(result,
				CSS_PROP_MACSURF_GRADIENT, 0, set_value);
		if (error != CSS_OK) return error;

		return css__stylesheet_style_vappend(result, 2,
				color1, color2);
	}

	return CSS_INVALID;
}
