/*
 * This file is part of LibCSS.
 * Licensed under the MIT License,
 *                http://www.opensource.org/licenses/mit-license.php
 * Copyright 2026 MacSurf (#255)
 *
 * Parse `background-clip`: border-box | padding-box | content-box | text.
 * Single-keyword grammar,
 * cloned from p_object_fit.c.
 */

#include <assert.h>
#include <string.h>

#include "bytecode/bytecode.h"
#include "bytecode/opcodes.h"
#include "parse/properties/properties.h"
#include "parse/properties/utils.h"

css_error css__parse_background_clip(css_language *c,
		const parserutils_vector *vector, int32_t *ctx,
		css_style *result)
{
	int32_t orig_ctx = *ctx;
	css_error error;
	const css_token *token;
	bool match;

	token = parserutils_vector_iterate(vector, ctx);
	if ((token == NULL) || ((token->type != CSS_TOKEN_IDENT))) {
		*ctx = orig_ctx;
		return CSS_INVALID;
	}

	if ((lwc_string_caseless_isequal(
			token->idata, c->strings[INHERIT],
			&match) == lwc_error_ok && match)) {
		error = css_stylesheet_style_inherit(result,
				CSS_PROP_BACKGROUND_CLIP);

	} else if ((lwc_string_caseless_isequal(
			token->idata, c->strings[INITIAL],
			&match) == lwc_error_ok && match)) {
		error = css_stylesheet_style_initial(result,
				CSS_PROP_BACKGROUND_CLIP);

	} else if ((lwc_string_caseless_isequal(
			token->idata, c->strings[REVERT],
			&match) == lwc_error_ok && match)) {
		error = css_stylesheet_style_revert(result,
				CSS_PROP_BACKGROUND_CLIP);

	} else if ((lwc_string_caseless_isequal(
			token->idata, c->strings[UNSET],
			&match) == lwc_error_ok && match)) {
		error = css_stylesheet_style_unset(result,
				CSS_PROP_BACKGROUND_CLIP);

	} else if ((lwc_string_caseless_isequal(
			token->idata, c->strings[BORDER_BOX],
			&match) == lwc_error_ok && match)) {
		error = css__stylesheet_style_appendOPV(result,
				CSS_PROP_BACKGROUND_CLIP,
				0, CSS_BACKGROUND_CLIP_BORDER_BOX);

	} else if ((lwc_string_caseless_isequal(
			token->idata, c->strings[PADDING_BOX],
			&match) == lwc_error_ok && match)) {
		error = css__stylesheet_style_appendOPV(result,
				CSS_PROP_BACKGROUND_CLIP,
				0, CSS_BACKGROUND_CLIP_PADDING_BOX);

	} else if ((lwc_string_caseless_isequal(
			token->idata, c->strings[CONTENT_BOX],
			&match) == lwc_error_ok && match)) {
		error = css__stylesheet_style_appendOPV(result,
				CSS_PROP_BACKGROUND_CLIP,
				0, CSS_BACKGROUND_CLIP_CONTENT_BOX);

	} else if ((lwc_string_caseless_isequal(
			token->idata, c->strings[LIBCSS_TEXT],
			&match) == lwc_error_ok && match)) {
		error = css__stylesheet_style_appendOPV(result,
				CSS_PROP_BACKGROUND_CLIP,
				0, CSS_BACKGROUND_CLIP_TEXT);

	} else {
		error = CSS_INVALID;
	}

	if (error != CSS_OK)
		*ctx = orig_ctx;

	return error;
}
