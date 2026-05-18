/*
 * This file is part of LibCSS.
 * Licensed under the MIT License,
 *                http://www.opensource.org/licenses/mit-license.php
 * Copyright 2026 MacSurf (fixes75, fixes117)
 *
 * Parse -macsurf-grid.
 *
 * V1 forms (fixes75 — still supported):
 *   -macsurf-grid: none
 *   -macsurf-grid: <cols>
 *   -macsurf-grid: <cols> <rows>
 *
 * V2 forms (fixes117 — explicit track widths):
 *   -macsurf-grid: <track-list>
 *
 * <track-list> is a space-separated list of up to 8 tracks where each
 * <track> is one of:
 *   <length>    e.g. 210px, 1em
 *   <flex>      e.g. 1fr, 1.5fr
 *   <percentage> e.g. 50%
 *
 * The libcss text preprocessor (fixes115/117 in cssh_css.c) expands
 * `grid-template-columns: ...` into one of these forms before libcss
 * sees the buffer, so this parser only needs to handle the canonical
 * `-macsurf-grid` grammar above.
 *
 * V1 storage (one int32_t):
 *   bits 31..16: column count (0..65535)
 *   bits 15..0:  row count (0 = auto)
 *
 * V2 storage (8 int32_t track descriptors after the packed value):
 *   bits 31..28: unit (0=NONE, 1=FR, 2=PX, 3=PERCENT)
 *   bits 27..0:  value (signed 28-bit)
 *
 * Bytecode payload after appendOPV(SET):
 *   - int32 packed value (cols<<16 | rows)
 *   - int32 track[0..7]   (8 entries; 0 = unused slot)
 *
 * For the legacy `<cols>` and `<cols> <rows>` integer forms, tracks
 * are all emitted as 0 — the layout layer falls back to equal-width
 * columns when track[0] == 0.
 */

#include <assert.h>
#include <string.h>

#include "bytecode/bytecode.h"
#include "bytecode/opcodes.h"
#include "parse/properties/properties.h"
#include "parse/properties/utils.h"

#define MACSURF_GRID_TRACK_UNIT_NONE    0
#define MACSURF_GRID_TRACK_UNIT_FR      1
#define MACSURF_GRID_TRACK_UNIT_PX      2
#define MACSURF_GRID_TRACK_UNIT_PERCENT 3
#define MACSURF_GRID_TRACK_MAX          8

/* Pack a track descriptor. value is a signed 28-bit quantity. */
static int32_t
macsurf__pack_track(uint8_t unit, int32_t value)
{
	uint32_t v = ((uint32_t)value) & 0x0FFFFFFFU;
	return (int32_t)((((uint32_t)unit & 0xF) << 28) | v);
}

/* Returns true if `unit_str` matches "fr" (case-insensitive). */
static bool
macsurf__token_is_fr(css_language *c, lwc_string *unit_str)
{
	bool match = false;
	(void)c;
	if (unit_str == NULL) return false;
	if (lwc_string_length(unit_str) == 2) {
		const char *s = lwc_string_data(unit_str);
		if ((s[0] == 'f' || s[0] == 'F') &&
				(s[1] == 'r' || s[1] == 'R')) {
			match = true;
		}
	}
	return match;
}

css_error css__parse_macsurf_grid(css_language *c,
		const parserutils_vector *vector, int32_t *ctx,
		css_style *result)
{
	int32_t orig_ctx = *ctx;
	css_error error;
	const css_token *token;
	enum flag_value flag_value;
	uint32_t cols = 0;
	uint32_t rows = 0;
	uint32_t packed;
	int32_t tracks[MACSURF_GRID_TRACK_MAX];
	int n_tracks = 0;
	bool track_list_form = false;
	int i;

	for (i = 0; i < MACSURF_GRID_TRACK_MAX; i++) tracks[i] = 0;

	token = parserutils_vector_peek(vector, *ctx);
	if (token == NULL) return CSS_INVALID;

	flag_value = get_css_flag_value(c, token);
	if (flag_value != FLAG_VALUE__NONE) {
		parserutils_vector_iterate(vector, ctx);
		return css_stylesheet_style_flag_value(result, flag_value,
				CSS_PROP_MACSURF_GRID);
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
			error = css__stylesheet_style_appendOPV(result,
					CSS_PROP_MACSURF_GRID, 0,
					0x0000 /* NONE */);
			return error;
		}
	}

	/* Decide form by inspecting the first non-whitespace token.
	 * NUMBER (no unit) -> legacy <cols> [<rows>] integer form.
	 * DIMENSION / PERCENTAGE -> V2 track-list. */
	token = parserutils_vector_peek(vector, *ctx);
	if (token == NULL) {
		*ctx = orig_ctx;
		return CSS_INVALID;
	}

	if (token->type == CSS_TOKEN_DIMENSION ||
			token->type == CSS_TOKEN_PERCENTAGE) {
		track_list_form = true;
	}

	if (track_list_form) {
		/* Parse up to 8 tracks. */
		while (n_tracks < MACSURF_GRID_TRACK_MAX) {
			consumeWhitespace(vector, ctx);
			token = parserutils_vector_peek(vector, *ctx);
			if (token == NULL) break;
			if (token->type != CSS_TOKEN_DIMENSION &&
					token->type != CSS_TOKEN_PERCENTAGE &&
					token->type != CSS_TOKEN_NUMBER) {
				break;
			}

			if (token->type == CSS_TOKEN_DIMENSION) {
				/* Either <flex> (1fr) or <length> (210px). */
				size_t consumed = 0;
				css_fixed num = css__number_from_lwc_string(
						token->idata, false, &consumed);
				lwc_string *unit_str = NULL;
				uint8_t unit = MACSURF_GRID_TRACK_UNIT_PX;
				int32_t value = 0;

				if (consumed < lwc_string_length(token->idata)) {
					/* Synthesise a substring for the unit
					 * portion. */
					const char *raw =
						lwc_string_data(token->idata);
					size_t total =
						lwc_string_length(token->idata);
					if (lwc_intern_string(raw + consumed,
							total - consumed,
							&unit_str)
							!= lwc_error_ok) {
						unit_str = NULL;
					}
				}

				if (macsurf__token_is_fr(c, unit_str)) {
					/* num is Q22.10. Re-quantise to
					 * Q20.8 with saturation. */
					int32_t fr_q88 = (int32_t)(num >> 2);
					if (fr_q88 < 0) fr_q88 = 0;
					if (fr_q88 > 0x0FFFFFFF)
						fr_q88 = 0x0FFFFFFF;
					unit = MACSURF_GRID_TRACK_UNIT_FR;
					value = fr_q88;
				} else {
					/* Treat as <length>. For V1 we assume
					 * px-equivalent at the source level.
					 * css__number_from_lwc Q22.10 -> int
					 * via >> 10. Caller already expanded
					 * em/rem etc to px in the preprocessor
					 * for the common case. */
					int32_t px = (int32_t)(num >> 10);
					if (px < 0) px = 0;
					if (px > 0x0FFFFFFF) px = 0x0FFFFFFF;
					unit = MACSURF_GRID_TRACK_UNIT_PX;
					value = px;
				}
				if (unit_str != NULL) {
					lwc_string_unref(unit_str);
				}

				tracks[n_tracks++] = macsurf__pack_track(
						unit, value);
			} else if (token->type == CSS_TOKEN_PERCENTAGE) {
				size_t consumed = 0;
				css_fixed num = css__number_from_lwc_string(
						token->idata, false, &consumed);
				int32_t pct_q88 = (int32_t)(num >> 2);
				if (pct_q88 < 0) pct_q88 = 0;
				if (pct_q88 > 0x0FFFFFFF) pct_q88 = 0x0FFFFFFF;
				tracks[n_tracks++] = macsurf__pack_track(
						MACSURF_GRID_TRACK_UNIT_PERCENT,
						pct_q88);
			} else {
				/* Plain NUMBER inside a track list -- treat as
				 * an fr ratio (some authors write 1 instead of
				 * 1fr). */
				size_t consumed = 0;
				css_fixed num = css__number_from_lwc_string(
						token->idata, false, &consumed);
				int32_t fr_q88 = (int32_t)(num >> 2);
				if (fr_q88 < 0) fr_q88 = 0;
				if (fr_q88 > 0x0FFFFFFF) fr_q88 = 0x0FFFFFFF;
				tracks[n_tracks++] = macsurf__pack_track(
						MACSURF_GRID_TRACK_UNIT_FR,
						fr_q88);
			}

			parserutils_vector_iterate(vector, ctx);
		}

		cols = (uint32_t)n_tracks;
		if (cols < 1) cols = 1;
		rows = 0;
	} else {
		/* Legacy integer form: <cols> [<rows>]. */
		if (token->type != CSS_TOKEN_NUMBER ||
				token->idata == NULL) {
			*ctx = orig_ctx;
			return CSS_INVALID;
		}
		{
			size_t consumed = 0;
			css_fixed cnum = css__number_from_lwc_string(
					token->idata, true, &consumed);
			int32_t cval = (int32_t)(cnum >> 10);
			if (cval < 1) cval = 1;
			if (cval > 255) cval = 255;
			cols = (uint32_t)cval;
			parserutils_vector_iterate(vector, ctx);
		}

		consumeWhitespace(vector, ctx);
		token = parserutils_vector_peek(vector, *ctx);
		if (token != NULL && token->type == CSS_TOKEN_NUMBER &&
				token->idata != NULL) {
			size_t consumed = 0;
			css_fixed rnum = css__number_from_lwc_string(
					token->idata, true, &consumed);
			int32_t rval = (int32_t)(rnum >> 10);
			if (rval < 0) rval = 0;
			if (rval > 255) rval = 255;
			rows = (uint32_t)rval;
			parserutils_vector_iterate(vector, ctx);
		}
	}

	packed = (cols << 16) | (rows & 0xffff);

	error = css__stylesheet_style_appendOPV(result,
			CSS_PROP_MACSURF_GRID, 0, 0x0080 /* SET */);
	if (error != CSS_OK) return error;

	/* Emit packed cols/rows + 8 track ints, every time. The cascade
	 * reads a fixed 9-int payload following the SET opcode. */
	error = css__stylesheet_style_vappend(result, 1, (css_fixed)packed);
	if (error != CSS_OK) return error;

	for (i = 0; i < MACSURF_GRID_TRACK_MAX; i++) {
		error = css__stylesheet_style_vappend(result, 1,
				(css_fixed)tracks[i]);
		if (error != CSS_OK) return error;
	}

	return CSS_OK;
}
