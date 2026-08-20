/*
 * This file is part of LibCSS.
 * Licensed under the MIT License.
 */

#include <string.h>

#include "bytecode/bytecode.h"
#include "bytecode/opcodes.h"
#include "parse/properties/properties.h"
#include "parse/properties/utils.h"

css_error css__parse_background_blend_mode(css_language *c,
		const parserutils_vector *vector, int32_t *ctx,
		css_style *result)
{
	int32_t orig_ctx = *ctx;
	css_error error;
	const css_token *token;
	bool match;
	uint16_t value = CSS_BACKGROUND_BLEND_MODE_INHERIT;
	int keyword = -1;

	token = parserutils_vector_iterate(vector, ctx);
	if (token == NULL || token->type != CSS_TOKEN_IDENT) {
		*ctx = orig_ctx;
		return CSS_INVALID;
	}

	if (lwc_string_caseless_isequal(token->idata, c->strings[INHERIT],
			&match) == lwc_error_ok && match) {
		return css_stylesheet_style_inherit(result,
				CSS_PROP_BACKGROUND_BLEND_MODE);
	}
	if (lwc_string_caseless_isequal(token->idata, c->strings[INITIAL],
			&match) == lwc_error_ok && match) {
		return css_stylesheet_style_initial(result,
				CSS_PROP_BACKGROUND_BLEND_MODE);
	}
	if (lwc_string_caseless_isequal(token->idata, c->strings[REVERT],
			&match) == lwc_error_ok && match) {
		return css_stylesheet_style_revert(result,
				CSS_PROP_BACKGROUND_BLEND_MODE);
	}
	if (lwc_string_caseless_isequal(token->idata, c->strings[UNSET],
			&match) == lwc_error_ok && match) {
		return css_stylesheet_style_unset(result,
				CSS_PROP_BACKGROUND_BLEND_MODE);
	}

	if (lwc_string_caseless_isequal(token->idata, c->strings[NORMAL],
			&match) == lwc_error_ok && match) {
		value = CSS_BACKGROUND_BLEND_MODE_NORMAL;
	} else {
		static const int keywords[] = {
			BLEND_MULTIPLY, BLEND_SCREEN, BLEND_OVERLAY, BLEND_DARKEN,
			BLEND_LIGHTEN, BLEND_COLOR_DODGE, BLEND_COLOR_BURN,
			BLEND_HARD_LIGHT, BLEND_SOFT_LIGHT, BLEND_DIFFERENCE,
			BLEND_EXCLUSION, BLEND_HUE, BLEND_SATURATION, BLEND_COLOR,
			BLEND_LUMINOSITY
		};
		int i;
		for (i = 0; i < (int)(sizeof(keywords) / sizeof(keywords[0])); i++) {
			if (lwc_string_caseless_isequal(token->idata,
					c->strings[keywords[i]], &match) == lwc_error_ok &&
					match) {
				keyword = i;
				break;
			}
		}
		if (keyword < 0) {
			*ctx = orig_ctx;
			return CSS_INVALID;
		}
		value = (uint16_t)(CSS_BACKGROUND_BLEND_MODE_MULTIPLY + keyword);
	}

	error = css__stylesheet_style_appendOPV(result,
			CSS_PROP_BACKGROUND_BLEND_MODE, 0, value);
	if (error != CSS_OK)
		*ctx = orig_ctx;
	return error;
}
