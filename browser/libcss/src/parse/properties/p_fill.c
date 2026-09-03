/* SVG fill V1: none | currentColor | <color>. */
#include <string.h>

#include "bytecode/bytecode.h"
#include "bytecode/opcodes.h"
#include "parse/properties/properties.h"
#include "parse/properties/utils.h"

css_error css__parse_fill(css_language *c,
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

	if (token->type == CSS_TOKEN_IDENT &&
			lwc_string_caseless_isequal(token->idata,
			c->strings[INHERIT], &match) == lwc_error_ok && match) {
		error = css_stylesheet_style_inherit(result, CSS_PROP_FILL);
	} else if (token->type == CSS_TOKEN_IDENT &&
			lwc_string_caseless_isequal(token->idata,
			c->strings[INITIAL], &match) == lwc_error_ok && match) {
		error = css_stylesheet_style_initial(result, CSS_PROP_FILL);
	} else if (token->type == CSS_TOKEN_IDENT &&
			lwc_string_caseless_isequal(token->idata,
			c->strings[UNSET], &match) == lwc_error_ok && match) {
		error = css_stylesheet_style_unset(result, CSS_PROP_FILL);
	} else if (token->type == CSS_TOKEN_IDENT &&
			lwc_string_caseless_isequal(token->idata,
			c->strings[NONE], &match) == lwc_error_ok && match) {
		error = css__stylesheet_style_appendOPV(result, CSS_PROP_FILL, 0,
				FILL_NONE);
	} else if (token->type == CSS_TOKEN_IDENT &&
			lwc_string_caseless_isequal(token->idata,
			c->strings[CURRENTCOLOR], &match) == lwc_error_ok && match) {
		error = css__stylesheet_style_appendOPV(result, CSS_PROP_FILL, 0,
				FILL_CURRENT_COLOR);
	} else {
		uint16_t value = 0;
		uint32_t color = 0;
		*ctx = orig_ctx;
		error = css__parse_colour_specifier(c, vector, ctx, &value, &color);
		if (error != CSS_OK) {
			*ctx = orig_ctx;
			return error;
		}
		if (value != COLOR_SET) {
			*ctx = orig_ctx;
			return CSS_INVALID;
		}
		error = css__stylesheet_style_appendOPV(result, CSS_PROP_FILL, 0,
				FILL_SET);
		if (error == CSS_OK)
			error = css__stylesheet_style_append(result, color);
	}

	if (error != CSS_OK) *ctx = orig_ctx;
	return error;
}
