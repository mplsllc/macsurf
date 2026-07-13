/*
 * This file is part of LibCSS.
 * Licensed under the MIT License,
 *                http://www.opensource.org/licenses/mit-license.php
 * Copyright 2026 MacSurf (#251)
 *
 * Parse `text-align-last`:
 *   auto | left | right | center | justify | start | end
 *
 * Mapped onto CSS_PROP_TEXT_ALIGN_LAST. start/end are logical; the cascade
 * stores them and layout maps them to left/right for horizontal-tb LTR.
 */

#include <assert.h>
#include <string.h>

#include "bytecode/bytecode.h"
#include "bytecode/opcodes.h"
#include "parse/properties/properties.h"
#include "parse/properties/utils.h"

css_error css__parse_text_align_last(css_language *c,
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
				CSS_PROP_TEXT_ALIGN_LAST);

	} else if ((lwc_string_caseless_isequal(
			token->idata, c->strings[INITIAL],
			&match) == lwc_error_ok && match)) {
		error = css_stylesheet_style_initial(result,
				CSS_PROP_TEXT_ALIGN_LAST);

	} else if ((lwc_string_caseless_isequal(
			token->idata, c->strings[REVERT],
			&match) == lwc_error_ok && match)) {
		error = css_stylesheet_style_revert(result,
				CSS_PROP_TEXT_ALIGN_LAST);

	} else if ((lwc_string_caseless_isequal(
			token->idata, c->strings[UNSET],
			&match) == lwc_error_ok && match)) {
		error = css_stylesheet_style_unset(result,
				CSS_PROP_TEXT_ALIGN_LAST);

	} else if ((lwc_string_caseless_isequal(
			token->idata, c->strings[AUTO],
			&match) == lwc_error_ok && match)) {
		error = css__stylesheet_style_appendOPV(result,
				CSS_PROP_TEXT_ALIGN_LAST,
				0, CSS_TEXT_ALIGN_LAST_AUTO);

	} else if ((lwc_string_caseless_isequal(
			token->idata, c->strings[LEFT],
			&match) == lwc_error_ok && match)) {
		error = css__stylesheet_style_appendOPV(result,
				CSS_PROP_TEXT_ALIGN_LAST,
				0, CSS_TEXT_ALIGN_LAST_LEFT);

	} else if ((lwc_string_caseless_isequal(
			token->idata, c->strings[RIGHT],
			&match) == lwc_error_ok && match)) {
		error = css__stylesheet_style_appendOPV(result,
				CSS_PROP_TEXT_ALIGN_LAST,
				0, CSS_TEXT_ALIGN_LAST_RIGHT);

	} else if ((lwc_string_caseless_isequal(
			token->idata, c->strings[CENTER],
			&match) == lwc_error_ok && match)) {
		error = css__stylesheet_style_appendOPV(result,
				CSS_PROP_TEXT_ALIGN_LAST,
				0, CSS_TEXT_ALIGN_LAST_CENTER);

	} else if ((lwc_string_caseless_isequal(
			token->idata, c->strings[JUSTIFY],
			&match) == lwc_error_ok && match)) {
		error = css__stylesheet_style_appendOPV(result,
				CSS_PROP_TEXT_ALIGN_LAST,
				0, CSS_TEXT_ALIGN_LAST_JUSTIFY);

	} else if ((lwc_string_caseless_isequal(
			token->idata, c->strings[START],
			&match) == lwc_error_ok && match)) {
		error = css__stylesheet_style_appendOPV(result,
				CSS_PROP_TEXT_ALIGN_LAST,
				0, CSS_TEXT_ALIGN_LAST_START);

	} else if ((lwc_string_caseless_isequal(
			token->idata, c->strings[END],
			&match) == lwc_error_ok && match)) {
		error = css__stylesheet_style_appendOPV(result,
				CSS_PROP_TEXT_ALIGN_LAST,
				0, CSS_TEXT_ALIGN_LAST_END);

	} else {
		error = CSS_INVALID;
	}

	if (error != CSS_OK)
		*ctx = orig_ctx;

	return error;
}
