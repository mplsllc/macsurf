/*
 * This file is part of LibCSS.
 * Licensed under the MIT License,
 *                http://www.opensource.org/licenses/mit-license.php
 * Copyright 2026 MacSurf (fixes1052)
 *
 * Parse appearance (#80).
 *
 * Accepted forms:
 *   appearance: auto    (default -- paint the built-in widget)
 *   appearance: none    (suppress it; the element is drawn from its own
 *                        CSS box: background, border, border-radius)
 *
 * Does NOT inherit.
 *
 * The legacy value set (button / textfield / menulist / checkbox / radio /
 * ...) is deliberately NOT accepted. Those names only ever meant "use the
 * platform widget", which is what `auto` already means here, and accepting
 * them would imply MacSurf can render one widget as another. A declaration
 * using them is invalid and drops, exactly as it did before this property
 * existed -- no behaviour regresses.
 *
 * `-webkit-appearance` is a separate property name and is not handled here;
 * it belongs in the cssh_css preprocessor as an alias if a real page needs it.
 */

#include <assert.h>
#include <string.h>

#include "bytecode/bytecode.h"
#include "bytecode/opcodes.h"
#include "parse/properties/properties.h"
#include "parse/properties/utils.h"

css_error css__parse_appearance(css_language *c,
		const parserutils_vector *vector, int32_t *ctx,
		css_style *result)
{
	int32_t orig_ctx = *ctx;
	css_error error;
	const css_token *token;
	bool match;

	token = parserutils_vector_iterate(vector, ctx);
	if (token == NULL || token->type != CSS_TOKEN_IDENT) {
		*ctx = orig_ctx;
		return CSS_INVALID;
	}

	if (lwc_string_caseless_isequal(token->idata,
			c->strings[INHERIT], &match) == lwc_error_ok && match) {
		error = css_stylesheet_style_inherit(result,
				CSS_PROP_APPEARANCE);
	} else if (lwc_string_caseless_isequal(token->idata,
			c->strings[INITIAL], &match) == lwc_error_ok && match) {
		error = css_stylesheet_style_initial(result,
				CSS_PROP_APPEARANCE);
	} else if (lwc_string_caseless_isequal(token->idata,
			c->strings[UNSET], &match) == lwc_error_ok && match) {
		error = css_stylesheet_style_unset(result,
				CSS_PROP_APPEARANCE);
	} else if (lwc_string_caseless_isequal(token->idata,
			c->strings[AUTO], &match) == lwc_error_ok && match) {
		error = css__stylesheet_style_appendOPV(result,
				CSS_PROP_APPEARANCE, 0, APPEARANCE_AUTO);
	} else if (lwc_string_caseless_isequal(token->idata,
			c->strings[NONE], &match) == lwc_error_ok && match) {
		error = css__stylesheet_style_appendOPV(result,
				CSS_PROP_APPEARANCE, 0, APPEARANCE_NONE);
	} else {
		*ctx = orig_ctx;
		return CSS_INVALID;
	}

	if (error != CSS_OK) {
		*ctx = orig_ctx;
	}
	return error;
}
