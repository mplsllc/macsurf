/*
 * This file is part of LibCSS.
 * Licensed under the MIT License,
 *                http://www.opensource.org/licenses/mit-license.php
 * Copyright 2026 MacSurf (fixes76)
 *
 * Parse -macsurf-animation-opacity.
 *
 * Accepted forms (V1):
 *   -macsurf-animation-opacity: none
 *   -macsurf-animation-opacity: <from> <to> <duration_ms>
 *
 * from, to: CSS numbers 0..1 (mapped to 0..255 internally).
 * duration_ms: integer (1..65535).
 *
 * V1 storage (one int32_t):
 *   bits 31..16: duration_ms (uint16)
 *   bits 15..8:  to_opacity (uint8 0..255)
 *   bits 7..0:   from_opacity (uint8 0..255)
 *
 * Bytecode payload after appendOPV(SET):
 *   - int32_t packed value
 */

#include <assert.h>
#include <string.h>

#include "bytecode/bytecode.h"
#include "bytecode/opcodes.h"
#include "parse/properties/properties.h"
#include "parse/properties/utils.h"

static int32_t macsurf_anim_op_parse_number_to_byte(
		const css_token *token)
{
	size_t consumed = 0;
	css_fixed n = css__number_from_lwc_string(token->idata,
			false /* allow float */, &consumed);
	/* n is Q22.10 fixed-point. 1.0 = 1024. Map 0..1 -> 0..255. */
	int32_t scaled = (int32_t)(((int64_t)n * 255) >> 10);
	if (scaled < 0) scaled = 0;
	if (scaled > 255) scaled = 255;
	return scaled;
}

css_error css__parse_macsurf_animation_opacity(css_language *c,
		const parserutils_vector *vector, int32_t *ctx,
		css_style *result)
{
	int32_t orig_ctx = *ctx;
	css_error error;
	const css_token *token;
	enum flag_value flag_value;
	uint32_t from_v = 0;
	uint32_t to_v = 0;
	uint32_t duration_ms = 0;
	uint32_t packed;

	token = parserutils_vector_peek(vector, *ctx);
	if (token == NULL) return CSS_INVALID;

	flag_value = get_css_flag_value(c, token);
	if (flag_value != FLAG_VALUE__NONE) {
		parserutils_vector_iterate(vector, ctx);
		return css_stylesheet_style_flag_value(result, flag_value,
				CSS_PROP_MACSURF_ANIMATION_OPACITY);
	}

	consumeWhitespace(vector, ctx);

	/* `none` keyword. */
	token = parserutils_vector_peek(vector, *ctx);
	if (token != NULL && token->type == CSS_TOKEN_IDENT &&
			token->idata != NULL) {
		bool match = false;
		if (lwc_string_caseless_isequal(token->idata,
				c->strings[NONE], &match) == lwc_error_ok &&
				match) {
			parserutils_vector_iterate(vector, ctx);
			return css__stylesheet_style_appendOPV(result,
					CSS_PROP_MACSURF_ANIMATION_OPACITY, 0,
					0x0000 /* NONE */);
		}
	}

	/* First number: from-opacity 0..1. */
	token = parserutils_vector_peek(vector, *ctx);
	if (token == NULL || token->type != CSS_TOKEN_NUMBER ||
			token->idata == NULL) {
		*ctx = orig_ctx;
		return CSS_INVALID;
	}
	from_v = (uint32_t)macsurf_anim_op_parse_number_to_byte(token);
	parserutils_vector_iterate(vector, ctx);
	consumeWhitespace(vector, ctx);

	/* Second number: to-opacity 0..1. */
	token = parserutils_vector_peek(vector, *ctx);
	if (token == NULL || token->type != CSS_TOKEN_NUMBER ||
			token->idata == NULL) {
		*ctx = orig_ctx;
		return CSS_INVALID;
	}
	to_v = (uint32_t)macsurf_anim_op_parse_number_to_byte(token);
	parserutils_vector_iterate(vector, ctx);
	consumeWhitespace(vector, ctx);

	/* Third number: duration in ms (integer). */
	token = parserutils_vector_peek(vector, *ctx);
	if (token == NULL || token->type != CSS_TOKEN_NUMBER ||
			token->idata == NULL) {
		*ctx = orig_ctx;
		return CSS_INVALID;
	}
	{
		size_t consumed = 0;
		css_fixed dn = css__number_from_lwc_string(token->idata,
				true /* int only */, &consumed);
		int32_t dv = (int32_t)(dn >> 10);  /* Q22.10 -> int */
		if (dv < 1) dv = 1;
		if (dv > 65535) dv = 65535;
		duration_ms = (uint32_t)dv;
		parserutils_vector_iterate(vector, ctx);
	}

	packed = (duration_ms << 16) | ((to_v & 0xff) << 8) |
			(from_v & 0xff);

	error = css__stylesheet_style_appendOPV(result,
			CSS_PROP_MACSURF_ANIMATION_OPACITY, 0,
			0x0080 /* SET */);
	if (error != CSS_OK) return error;

	return css__stylesheet_style_vappend(result, 1, (css_fixed)packed);
}
