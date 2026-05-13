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
 * Parse -macsurf-gradient.
 *
 * Accepted forms (all map to a vertical or horizontal 2-stop gradient
 * because the storage slot only carries two colours):
 *
 *   -macsurf-gradient: linear-gradient(c1, c2)
 *   -macsurf-gradient: linear-gradient(c1, c2, c3, ...)        fixes49
 *   -macsurf-gradient: linear-gradient(to right, c1, c2)       fixes48
 *   -macsurf-gradient: linear-gradient(to left, c1, c2)        fixes48
 *   -macsurf-gradient: linear-gradient(to top|bottom, c1, c2)  fixes49
 *   -macsurf-gradient: linear-gradient(0deg, c1, c2)           fixes49
 *   -macsurf-gradient: linear-gradient(90deg, c1, c2)          fixes49
 *   -macsurf-gradient: linear-gradient(180deg, c1, c2)         fixes49
 *   -macsurf-gradient: linear-gradient(270deg, c1, c2)         fixes49
 *
 * Multi-stop forms: only the FIRST and LAST colour are kept; the
 * intermediate stops are silently dropped. Good-enough rendering for
 * most multi-stop CSS pages on a 2-stop renderer.
 *
 * Angle handling: anything in 45..134 or 225..314 degrees is treated
 * as horizontal; everything else is vertical. Defaults to vertical.
 *
 * Direction is encoded in the emitted OPV value:
 *   0x0080 = SET vertical (default, fixes37/47)
 *   0x00C0 = SET horizontal (fixes48)
 */
css_error css__parse_macsurf_gradient(css_language *c,
		const parserutils_vector *vector, int32_t *ctx,
		css_style *result)
{
	int32_t orig_ctx = *ctx;
	css_error error;
	const css_token *token;
	css_color first_color = 0;
	css_color last_color = 0;
	int color_count = 0;
	enum flag_value flag_value;
	bool match = false;

	token = parserutils_vector_peek(vector, *ctx);
	if (token == NULL) return CSS_INVALID;

	flag_value = get_css_flag_value(c, token);
	if (flag_value != FLAG_VALUE__NONE) {
		parserutils_vector_iterate(vector, ctx);
		return css_stylesheet_style_flag_value(result, flag_value, CSS_PROP_MACSURF_GRADIENT);
	}

	if (token->type == CSS_TOKEN_FUNCTION &&
	    lwc_string_caseless_isequal(token->idata,
	        c->strings[LINEAR_GRADIENT], &match) == lwc_error_ok &&
	    match) {
		bool horizontal = false;
		uint16_t set_value = 0x0080;
		parserutils_vector_iterate(vector, ctx);
		consumeWhitespace(vector, ctx);

		/* Optional direction prefix: `to <side>` or `<angle>deg`. */
		token = parserutils_vector_peek(vector, *ctx);
		if (token != NULL && token->type == CSS_TOKEN_IDENT) {
			bool to_match = false;
			if (lwc_string_caseless_isequal(token->idata,
					c->strings[TO], &to_match)
					== lwc_error_ok && to_match) {
				parserutils_vector_iterate(vector, ctx);
				consumeWhitespace(vector, ctx);
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
					/* `to top|bottom|center` falls through
					 * with horizontal=false (default). */
					parserutils_vector_iterate(vector, ctx);
				}
				consumeWhitespace(vector, ctx);
				token = parserutils_vector_peek(vector, *ctx);
				if (token != NULL && token->type == CSS_TOKEN_CHAR &&
				    lwc_string_data(token->idata)[0] == ',')
					parserutils_vector_iterate(vector, ctx);
			}
		} else if (token != NULL &&
		           token->type == CSS_TOKEN_DIMENSION) {
			/* fixes49 -- angle prefix: NUMBERdeg. The dimension
			 * token's idata holds the suffix; check that it's
			 * "deg" (case-insensitive). */
			bool deg_match = false;
			(void)lwc_string_caseless_isequal(token->idata,
					c->strings[c->strings ? 0 : 0],
					&deg_match);
			/* Simpler approach: just look at the trailing 3 chars
			 * of the dimension token's lower-case body. */
			{
				const char *data = lwc_string_data(token->idata);
				size_t len = lwc_string_length(token->idata);
				int angle = 0;
				size_t i;
				bool seen_digit = false;
				bool is_deg = (len >= 3) &&
					((data[len-3] == 'd' || data[len-3] == 'D') &&
					 (data[len-2] == 'e' || data[len-2] == 'E') &&
					 (data[len-1] == 'g' || data[len-1] == 'G'));
				if (is_deg) {
					/* Parse the leading integer portion.
					 * Floating-point is not supported here
					 * since 0/45/90/etc. cover the common
					 * angles. */
					int sign = 1;
					for (i = 0; i < len - 3; i++) {
						char ch = data[i];
						if (i == 0 && ch == '-') {
							sign = -1;
							continue;
						}
						if (i == 0 && ch == '+') {
							continue;
						}
						if (ch >= '0' && ch <= '9') {
							angle = angle * 10 +
								(ch - '0');
							seen_digit = true;
						} else {
							/* Non-integer; bail. */
							is_deg = false;
							break;
						}
					}
					if (seen_digit) {
						int a = (angle * sign) % 360;
						if (a < 0) a += 360;
						if ((a >= 45 && a < 135) ||
						    (a >= 225 && a < 315))
							horizontal = true;
					}
				}
				if (is_deg) {
					parserutils_vector_iterate(vector, ctx);
					consumeWhitespace(vector, ctx);
					token = parserutils_vector_peek(vector, *ctx);
					if (token != NULL &&
					    token->type == CSS_TOKEN_CHAR &&
					    lwc_string_data(token->idata)[0] == ',')
						parserutils_vector_iterate(vector, ctx);
				}
			}
		}

		if (horizontal) {
			set_value = 0x00C0;
		}

		/* fixes49 -- accept N colour stops. Only first and last
		 * survive into bytecode (storage slot is 2 colours). */
		while (1) {
			css_color tmp_color = 0;
			uint16_t tmp_type = 0;
			consumeWhitespace(vector, ctx);
			token = parserutils_vector_peek(vector, *ctx);
			if (token == NULL) break;
			if (token->type == CSS_TOKEN_CHAR &&
			    lwc_string_data(token->idata)[0] == ')')
				break;
			error = css__parse_colour_specifier(c, vector, ctx,
					&tmp_type, &tmp_color);
			if (error != CSS_OK) {
				*ctx = orig_ctx;
				return error;
			}
			if (color_count == 0) first_color = tmp_color;
			last_color = tmp_color;
			color_count++;
			consumeWhitespace(vector, ctx);
			token = parserutils_vector_peek(vector, *ctx);
			if (token != NULL && token->type == CSS_TOKEN_CHAR &&
			    lwc_string_data(token->idata)[0] == ',')
				parserutils_vector_iterate(vector, ctx);
		}

		if (color_count < 1) {
			*ctx = orig_ctx;
			return CSS_INVALID;
		}
		/* Single-stop falls back to a uniform fill (first == last). */
		if (color_count == 1) last_color = first_color;

		/* Close parenthesis. */
		consumeWhitespace(vector, ctx);
		token = parserutils_vector_peek(vector, *ctx);
		if (token != NULL && token->type == CSS_TOKEN_CHAR &&
		    lwc_string_data(token->idata)[0] == ')')
			parserutils_vector_iterate(vector, ctx);

		error = css__stylesheet_style_appendOPV(result,
				CSS_PROP_MACSURF_GRADIENT, 0, set_value);
		if (error != CSS_OK) return error;

		return css__stylesheet_style_vappend(result, 2,
				first_color, last_color);
	}

	return CSS_INVALID;
}
