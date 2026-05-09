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
 * Parse box-shadow: [inset]? h v [blur] [spread] [color]?
 */
css_error css__parse_box_shadow(css_language *c,
		const parserutils_vector *vector, int32_t *ctx,
		css_style *result)
{
	int32_t orig_ctx = *ctx;
	css_error error;
	const css_token *token;
	css_fixed h = 0, v = 0, blur = 0, spread = 0;
	uint32_t h_u = 0, v_u = 0, b_u = 0, s_u = 0;
	css_color color = 0;
	uint16_t color_type = 0;
	bool inset = false;
	enum flag_value flag_value;

	token = parserutils_vector_peek(vector, *ctx);
	if (token == NULL) return CSS_INVALID;

	flag_value = get_css_flag_value(c, token);
	if (flag_value != FLAG_VALUE__NONE) {
		parserutils_vector_iterate(vector, ctx);
		return css_stylesheet_style_flag_value(result, flag_value, CSS_PROP_BOX_SHADOW);
	}

	/* Optional inset */
	if (token->type == CSS_TOKEN_IDENT && lwc_string_caseless_isequal(token->idata, c->strings[INSET], NULL)) {
		inset = true;
		parserutils_vector_iterate(vector, ctx);
	}

	/* h-offset v-offset */
	error = css__parse_unit_specifier(c, vector, ctx, UNIT_PX, &h, &h_u);
	if (error != CSS_OK) { *ctx = orig_ctx; return error; }

	error = css__parse_unit_specifier(c, vector, ctx, UNIT_PX, &v, &v_u);
	if (error != CSS_OK) { *ctx = orig_ctx; return error; }

	/* Optional blur */
	token = parserutils_vector_peek(vector, *ctx);
	if (token != NULL && (token->type == CSS_TOKEN_DIMENSION || token->type == CSS_TOKEN_NUMBER)) {
		error = css__parse_unit_specifier(c, vector, ctx, UNIT_PX, &blur, &b_u);
		if (error == CSS_OK) {
			/* Optional spread */
			token = parserutils_vector_peek(vector, *ctx);
			if (token != NULL && (token->type == CSS_TOKEN_DIMENSION || token->type == CSS_TOKEN_NUMBER)) {
				error = css__parse_unit_specifier(c, vector, ctx, UNIT_PX, &spread, &s_u);
				if (error != CSS_OK) { *ctx = orig_ctx; return error; }
			}
		}
	}

	/* Optional color */
	token = parserutils_vector_peek(vector, *ctx);
	if (token != NULL) {
		error = css__parse_colour_specifier(c, vector, ctx, &color_type, &color);
		if (error == CSS_OK) {
			/* color parsed */
		}
	}

	error = css__stylesheet_style_appendOPV(result, CSS_PROP_BOX_SHADOW, 0, 0x0080 /* SET */);
	if (error != CSS_OK) return error;

	/* Append 4 lengths + inset flag + color info */
	return css__stylesheet_style_vappend(result, 6, h, v, blur, spread, (css_fixed)inset, (css_fixed)color);
}
