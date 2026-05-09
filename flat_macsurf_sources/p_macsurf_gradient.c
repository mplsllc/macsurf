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

	token = parserutils_vector_peek(vector, *ctx);
	if (token == NULL) return CSS_INVALID;

	flag_value = get_css_flag_value(c, token);
	if (flag_value != FLAG_VALUE__NONE) {
		parserutils_vector_iterate(vector, ctx);
		return css_stylesheet_style_flag_value(result, flag_value, CSS_PROP_MACSURF_GRADIENT);
	}

	/* Look for linear-gradient(...) */
	if (token->type == CSS_TOKEN_FUNCTION &&
	    lwc_string_caseless_isequal(token->idata, c->strings[LINEAR_GRADIENT], NULL)) {
		parserutils_vector_iterate(vector, ctx);
		
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
	} else {
		return CSS_INVALID;
	}

	error = css__stylesheet_style_appendOPV(result, CSS_PROP_MACSURF_GRADIENT, 0, 0x0080 /* SET */);
	if (error != CSS_OK) return error;

	return css__stylesheet_style_vappend(result, 2, color1, color2);
}
