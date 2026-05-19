/*
 * This file is part of LibCSS.
 * Licensed under the MIT License,
 *                http://www.opensource.org/licenses/mit-license.php
 * Copyright 2026 MacSurf (fixes136a)
 *
 * Parse `word-break`: normal | break-all | keep-all.
 *
 * Mapped onto CSS_PROP_WORD_BREAK (0x088). Single-keyword grammar.
 * keep-all is parsed but semantically inert without CJK segmentation;
 * layout treats it as `normal` for now (documented in CSS_STATUS).
 */

#include <assert.h>
#include <string.h>

#include "bytecode/bytecode.h"
#include "bytecode/opcodes.h"
#include "parse/properties/properties.h"
#include "parse/properties/utils.h"

css_error css__parse_word_break(css_language *c,
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
				CSS_PROP_WORD_BREAK);

	} else if ((lwc_string_caseless_isequal(
			token->idata, c->strings[INITIAL],
			&match) == lwc_error_ok && match)) {
		error = css_stylesheet_style_initial(result,
				CSS_PROP_WORD_BREAK);

	} else if ((lwc_string_caseless_isequal(
			token->idata, c->strings[REVERT],
			&match) == lwc_error_ok && match)) {
		error = css_stylesheet_style_revert(result,
				CSS_PROP_WORD_BREAK);

	} else if ((lwc_string_caseless_isequal(
			token->idata, c->strings[UNSET],
			&match) == lwc_error_ok && match)) {
		error = css_stylesheet_style_unset(result,
				CSS_PROP_WORD_BREAK);

	} else if ((lwc_string_caseless_isequal(
			token->idata, c->strings[NORMAL],
			&match) == lwc_error_ok && match)) {
		error = css__stylesheet_style_appendOPV(result,
				CSS_PROP_WORD_BREAK,
				0, CSS_WORD_BREAK_NORMAL);

	} else if ((lwc_string_caseless_isequal(
			token->idata, c->strings[BREAK_ALL],
			&match) == lwc_error_ok && match)) {
		error = css__stylesheet_style_appendOPV(result,
				CSS_PROP_WORD_BREAK,
				0, CSS_WORD_BREAK_BREAK_ALL);

	} else if ((lwc_string_caseless_isequal(
			token->idata, c->strings[KEEP_ALL],
			&match) == lwc_error_ok && match)) {
		error = css__stylesheet_style_appendOPV(result,
				CSS_PROP_WORD_BREAK,
				0, CSS_WORD_BREAK_KEEP_ALL);

	} else {
		error = CSS_INVALID;
	}

	if (error != CSS_OK)
		*ctx = orig_ctx;

	return error;
}
