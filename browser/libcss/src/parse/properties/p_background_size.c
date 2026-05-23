/*
 * This file is part of LibCSS.
 * Licensed under the MIT License,
 *                http://www.opensource.org/licenses/mit-license.php
 * Copyright 2026 MacSurf (fixes191b)
 *
 * Parse background-size.
 *
 * Accepted V1 forms:
 *   background-size: auto                  -> (0, 0)
 *   background-size: cover                 -> (-1, -1)
 *   background-size: contain               -> (-2, -2)
 *   background-size: <length>              -> (px, 0)    (h = auto)
 *   background-size: <length> <length>     -> (px, px)
 *   background-size: <length> auto         -> (px, 0)
 *   background-size: auto <length>         -> (0, px)
 *
 * Deferred (V2):
 *   <percentage>                            -- needs a second 16-bit
 *                                              tag bit; bias-pack would
 *                                              push values out of int16.
 *   comma-separated multi-background lists  -- only the first is kept.
 *
 * Storage in css_computed_style_i.background_size (int32_t scalar,
 * self-aligning to avoid the fixes151b padding trap):
 *   bits 31..16: w_code (int16 sign-extended)
 *   bits 15..0:  h_code (int16)
 *   0 (whole word) = unset (treat as auto auto).
 */

#include <assert.h>
#include <string.h>

#include "bytecode/bytecode.h"
#include "bytecode/opcodes.h"
#include "parse/properties/properties.h"
#include "parse/properties/utils.h"

/* Sentinel codes, mirrored in s_background_size.c and consumer. */
#define BG_SIZE_AUTO     ((int16_t)0)
#define BG_SIZE_COVER    ((int16_t)-1)
#define BG_SIZE_CONTAIN  ((int16_t)-2)

static int bg_size__match_ident(css_language *c, const css_token *t, int which)
{
	bool match = false;
	if (t == NULL || t->type != CSS_TOKEN_IDENT || t->idata == NULL)
		return 0;
	if (lwc_string_caseless_isequal(t->idata, c->strings[which],
			&match) == lwc_error_ok && match)
		return 1;
	return 0;
}

css_error css__parse_background_size(css_language *c,
		const parserutils_vector *vector, int32_t *ctx,
		css_style *result)
{
	int32_t orig_ctx = *ctx;
	css_error error;
	const css_token *token;
	enum flag_value flag_value;
	int16_t w_code = BG_SIZE_AUTO;
	int16_t h_code = BG_SIZE_AUTO;
	int32_t packed;
	int got_w = 0;
	int got_h = 0;

	token = parserutils_vector_peek(vector, *ctx);
	if (token == NULL) return CSS_INVALID;

	flag_value = get_css_flag_value(c, token);
	if (flag_value != FLAG_VALUE__NONE) {
		parserutils_vector_iterate(vector, ctx);
		return css_stylesheet_style_flag_value(result, flag_value,
				CSS_PROP_BACKGROUND_SIZE);
	}

	consumeWhitespace(vector, ctx);

	token = parserutils_vector_peek(vector, *ctx);
	if (token == NULL) { *ctx = orig_ctx; return CSS_INVALID; }

	/* cover / contain are single-value keywords applying to both axes. */
	if (bg_size__match_ident(c, token, COVER)) {
		w_code = h_code = BG_SIZE_COVER;
		parserutils_vector_iterate(vector, ctx);
		goto emit;
	}
	if (bg_size__match_ident(c, token, CONTAIN)) {
		w_code = h_code = BG_SIZE_CONTAIN;
		parserutils_vector_iterate(vector, ctx);
		goto emit;
	}

	/* Two-value form: each axis is `auto` or a positive length. */
	if (bg_size__match_ident(c, token, AUTO)) {
		w_code = BG_SIZE_AUTO;
		got_w = 1;
		parserutils_vector_iterate(vector, ctx);
	} else if (token->type == CSS_TOKEN_DIMENSION ||
			token->type == CSS_TOKEN_NUMBER) {
		css_fixed length = 0;
		uint32_t unit = CSS_UNIT_PX;
		size_t consumed = 0;
		if (token->idata == NULL) { *ctx = orig_ctx; return CSS_INVALID; }
		length = css__number_from_lwc_string(token->idata, false,
				&consumed);
		if (token->type == CSS_TOKEN_DIMENSION) {
			const char *u = lwc_string_data(token->idata) + consumed;
			size_t ulen = lwc_string_length(token->idata) - consumed;
			error = css__parse_unit_keyword(u, ulen, &unit);
			if (error != CSS_OK) { *ctx = orig_ctx; return error; }
		} else {
			/* Bare number: treat as px when 0, else invalid. */
			if (length != 0) { *ctx = orig_ctx; return CSS_INVALID; }
			unit = CSS_UNIT_PX;
		}
		/* Only PX accepted in V1; other units fall back to auto so
		 * the rest of the cascade still applies. */
		if (unit == CSS_UNIT_PX) {
			int32_t px = FIXTOINT(length);
			if (px < 1) px = 1;
			if (px > 32767) px = 32767;
			w_code = (int16_t)px;
		} else {
			w_code = BG_SIZE_AUTO;
		}
		got_w = 1;
		parserutils_vector_iterate(vector, ctx);
	} else {
		*ctx = orig_ctx;
		return CSS_INVALID;
	}

	consumeWhitespace(vector, ctx);

	token = parserutils_vector_peek(vector, *ctx);
	if (token != NULL && token->type != CSS_TOKEN_CHAR) {
		if (bg_size__match_ident(c, token, AUTO)) {
			h_code = BG_SIZE_AUTO;
			got_h = 1;
			parserutils_vector_iterate(vector, ctx);
		} else if (token->type == CSS_TOKEN_DIMENSION ||
				token->type == CSS_TOKEN_NUMBER) {
			css_fixed length = 0;
			uint32_t unit = CSS_UNIT_PX;
			size_t consumed = 0;
			length = css__number_from_lwc_string(token->idata,
					false, &consumed);
			if (token->type == CSS_TOKEN_DIMENSION) {
				const char *u =
					lwc_string_data(token->idata) +
					consumed;
				size_t ulen = lwc_string_length(token->idata) -
					consumed;
				error = css__parse_unit_keyword(u, ulen, &unit);
				if (error == CSS_OK && unit == CSS_UNIT_PX) {
					int32_t px = FIXTOINT(length);
					if (px < 1) px = 1;
					if (px > 32767) px = 32767;
					h_code = (int16_t)px;
				} else {
					h_code = BG_SIZE_AUTO;
				}
			} else if (length == 0) {
				h_code = BG_SIZE_AUTO;
			} else {
				h_code = BG_SIZE_AUTO;
			}
			got_h = 1;
			parserutils_vector_iterate(vector, ctx);
		}
	}

	if (!got_h) {
		/* Single-value form: per spec, the second axis becomes auto.
		 * w stays as parsed; h defaults to auto sentinel. */
		h_code = BG_SIZE_AUTO;
	}

emit:
	(void)got_w;
	packed = ((int32_t)((uint16_t)(uint16_t)w_code) << 16) |
			(int32_t)((uint16_t)h_code);
	/* If we landed on (0,0) the storage marks unset = auto auto. That
	 * is the CSS spec default and matches what an unset declaration
	 * would have produced; no harm. */

	error = css__stylesheet_style_appendOPV(result,
			CSS_PROP_BACKGROUND_SIZE, 0, 0x0080 /* SET */);
	if (error != CSS_OK) {
		*ctx = orig_ctx;
		return error;
	}

	error = css__stylesheet_style_vappend(result, 1, (css_fixed)packed);
	if (error != CSS_OK) {
		*ctx = orig_ctx;
		return error;
	}

	return CSS_OK;
}
