/*
 * This file is part of LibCSS.
 * Licensed under the MIT License,
 *                http://www.opensource.org/licenses/mit-license.php
 * Copyright 2025 Gemini CLI / 2026 MacSurf (fixes361b)
 */

#include <assert.h>
#include <string.h>

#include "bytecode/bytecode.h"
#include "bytecode/opcodes.h"
#include "parse/properties/properties.h"
#include "parse/properties/utils.h"

/**
 * Parse a single shadow record: [inset]? h v [blur] [spread] [color]?
 *
 * Returns CSS_OK on success and populates the out params. On invalid
 * shadow returns CSS_INVALID and leaves ctx unchanged from the caller's
 * perspective (caller checks ctx).
 */
static css_error parse_one_shadow(css_language *c,
		const parserutils_vector *vector, int32_t *ctx,
		css_fixed *h_out, css_fixed *v_out,
		css_fixed *blur_out, css_fixed *spread_out,
		bool *inset_out, css_color *color_out)
{
	int32_t saved_ctx = *ctx;
	css_error error;
	const css_token *token;
	css_fixed h = 0, v = 0, blur = 0, spread = 0;
	uint32_t h_u = 0, v_u = 0, b_u = 0, s_u = 0;
	css_color color = 0;
	uint16_t color_type = 0;
	bool inset = false;

	consumeWhitespace(vector, ctx);

	/* Optional inset keyword. */
	token = parserutils_vector_peek(vector, *ctx);
	if (token != NULL && token->type == CSS_TOKEN_IDENT) {
		bool inset_match = false;
		if (lwc_string_caseless_isequal(token->idata,
				c->strings[INSET], &inset_match)
				== lwc_error_ok && inset_match) {
			inset = true;
			parserutils_vector_iterate(vector, ctx);
			consumeWhitespace(vector, ctx);
		}
	}

	/* h-offset v-offset (required) */
	error = css__parse_unit_specifier(c, vector, ctx, UNIT_PX, &h, &h_u);
	if (error != CSS_OK) { *ctx = saved_ctx; return error; }

	error = css__parse_unit_specifier(c, vector, ctx, UNIT_PX, &v, &v_u);
	if (error != CSS_OK) { *ctx = saved_ctx; return error; }

	consumeWhitespace(vector, ctx);

	/* Optional blur */
	token = parserutils_vector_peek(vector, *ctx);
	if (token != NULL && (token->type == CSS_TOKEN_DIMENSION ||
			token->type == CSS_TOKEN_NUMBER)) {
		error = css__parse_unit_specifier(c, vector, ctx, UNIT_PX,
				&blur, &b_u);
		if (error == CSS_OK) {
			consumeWhitespace(vector, ctx);
			/* Optional spread */
			token = parserutils_vector_peek(vector, *ctx);
			if (token != NULL && (token->type == CSS_TOKEN_DIMENSION ||
					token->type == CSS_TOKEN_NUMBER)) {
				error = css__parse_unit_specifier(c, vector, ctx,
						UNIT_PX, &spread, &s_u);
				if (error != CSS_OK) {
					*ctx = saved_ctx; return error;
				}
				consumeWhitespace(vector, ctx);
			}
		}
	}

	/* Optional inset keyword AFTER offsets (CSS allows either position). */
	token = parserutils_vector_peek(vector, *ctx);
	if (token != NULL && token->type == CSS_TOKEN_IDENT && !inset) {
		bool inset_match = false;
		if (lwc_string_caseless_isequal(token->idata,
				c->strings[INSET], &inset_match)
				== lwc_error_ok && inset_match) {
			inset = true;
			parserutils_vector_iterate(vector, ctx);
			consumeWhitespace(vector, ctx);
		}
	}

	/* Optional color */
	token = parserutils_vector_peek(vector, *ctx);
	if (token != NULL && token->type != CSS_TOKEN_CHAR) {
		error = css__parse_colour_specifier(c, vector, ctx,
				&color_type, &color);
		if (error == CSS_OK) {
			/* color parsed */
		}
	}

	*h_out = h;
	*v_out = v;
	*blur_out = blur;
	*spread_out = spread;
	*inset_out = inset;
	*color_out = color;
	return CSS_OK;
}

/**
 * Parse box-shadow: <shadow> [, <shadow>]*
 *
 * fixes361b - multi-shadow support. Up to 2 shadows are stored: the
 * first goes into the existing inner-struct slot via the standard
 * SET=0x0080 path; the second goes into the outer-struct
 * box_shadow_2 side channel; the third (fixes362) goes into
 * box_shadow_3. Common Platinum shadow pattern is two inset bevels
 * (light top-left + dark bottom-right) plus one outer drop shadow.
 *
 * Bytecode layout (count == 1 case stays compatible with the old
 * single-shadow format):
 *
 *   OPV(BOX_SHADOW, flags=0, value=0x0080 SET)
 *   css_fixed count       (1, 2, or 3)
 *   for i in 0..count-1:
 *     css_fixed h, v, blur, spread, inset, color    (6 entries)
 */
css_error css__parse_box_shadow(css_language *c,
		const parserutils_vector *vector, int32_t *ctx,
		css_style *result)
{
	int32_t orig_ctx = *ctx;
	css_error error;
	const css_token *token;
	enum flag_value flag_value;
	css_fixed h[3], v[3], blur[3], spread[3];
	bool inset[3];
	css_color color[3];
	int n = 0;

	token = parserutils_vector_peek(vector, *ctx);
	if (token == NULL) return CSS_INVALID;

	flag_value = get_css_flag_value(c, token);
	if (flag_value != FLAG_VALUE__NONE) {
		parserutils_vector_iterate(vector, ctx);
		return css_stylesheet_style_flag_value(result, flag_value,
				CSS_PROP_BOX_SHADOW);
	}

	/* "none" keyword. */
	if (token->type == CSS_TOKEN_IDENT) {
		bool none_match = false;
		if (lwc_string_caseless_isequal(token->idata,
				c->strings[NONE], &none_match)
				== lwc_error_ok && none_match) {
			parserutils_vector_iterate(vector, ctx);
			return css__stylesheet_style_appendOPV(result,
					CSS_PROP_BOX_SHADOW, 0, 0x0000 /* NONE */);
		}
	}

	/* Parse first shadow (required). */
	error = parse_one_shadow(c, vector, ctx,
			&h[0], &v[0], &blur[0], &spread[0],
			&inset[0], &color[0]);
	if (error != CSS_OK) { *ctx = orig_ctx; return error; }
	n = 1;

	/* Parse additional shadows up to N=3. Each separated by a comma. */
	while (n < 3) {
		int32_t comma_ctx = *ctx;
		consumeWhitespace(vector, ctx);
		token = parserutils_vector_peek(vector, *ctx);
		if (token == NULL || token->type != CSS_TOKEN_CHAR ||
				lwc_string_length(token->idata) != 1 ||
				lwc_string_data(token->idata)[0] != ',') {
			*ctx = comma_ctx;
			break;
		}
		parserutils_vector_iterate(vector, ctx);
		error = parse_one_shadow(c, vector, ctx,
				&h[n], &v[n], &blur[n], &spread[n],
				&inset[n], &color[n]);
		if (error != CSS_OK) {
			/* Bail without rolling back the first shadow - the
			 * comma was bad but the first parse is good. */
			*ctx = comma_ctx;
			break;
		}
		n++;
	}

	/* Skip any further comma-separated shadows beyond N=3 so the
	 * declaration parses as a whole even if author CSS has 4+. */
	while (1) {
		int32_t comma_ctx = *ctx;
		css_fixed dh, dv, dblur, dspread;
		bool dinset;
		css_color dcolor;
		consumeWhitespace(vector, ctx);
		token = parserutils_vector_peek(vector, *ctx);
		if (token == NULL || token->type != CSS_TOKEN_CHAR ||
				lwc_string_length(token->idata) != 1 ||
				lwc_string_data(token->idata)[0] != ',') {
			*ctx = comma_ctx;
			break;
		}
		parserutils_vector_iterate(vector, ctx);
		error = parse_one_shadow(c, vector, ctx,
				&dh, &dv, &dblur, &dspread, &dinset, &dcolor);
		if (error != CSS_OK) {
			*ctx = comma_ctx;
			break;
		}
		/* Discard. */
	}

	error = css__stylesheet_style_appendOPV(result,
			CSS_PROP_BOX_SHADOW, 0, 0x0080 /* SET */);
	if (error != CSS_OK) return error;

	/* Emit count + first shadow's 6 fields (always). */
	error = css__stylesheet_style_vappend(result, 7,
			(css_fixed)n,
			h[0], v[0], blur[0], spread[0],
			(css_fixed)inset[0], (css_fixed)color[0]);
	if (error != CSS_OK) return error;

	/* Emit second shadow's 6 fields when n>=2. */
	if (n >= 2) {
		error = css__stylesheet_style_vappend(result, 6,
				h[1], v[1], blur[1], spread[1],
				(css_fixed)inset[1], (css_fixed)color[1]);
		if (error != CSS_OK) return error;
	}

	/* fixes362 - emit third shadow's 6 fields when n==3. */
	if (n == 3) {
		error = css__stylesheet_style_vappend(result, 6,
				h[2], v[2], blur[2], spread[2],
				(css_fixed)inset[2], (css_fixed)color[2]);
		if (error != CSS_OK) return error;
	}

	return CSS_OK;
}
