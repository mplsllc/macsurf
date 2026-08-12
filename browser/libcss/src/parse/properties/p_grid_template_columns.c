/*
 * This file is part of LibCSS.
 * Licensed under the MIT License,
 *                http://www.opensource.org/licenses/mit-license.php
 * Copyright 2026 MacSurf (fixes112, fixes148)
 *
 * Parse `grid-template-columns`.
 *
 * fixes148 rewrite: extract real track widths from the standard CSS Grid
 * grammar and emit them in the same packed-track-array bytecode format
 * `p_macsurf_grid.c` uses. Both properties target the same opcode
 * (CSS_PROP_MACSURF_GRID), so the select-side cascade in s_macsurf_grid.c
 * reads exactly 1 packed int + 8 track ints regardless of which parser
 * emitted them.
 *
 * Pre-fixes148 behaviour: counted tracks, folded onto packed cols<<16,
 * emitted OPV + packed only (no track ints). The select side then read
 * 8 garbage values off the bytecode stream for the track array. Silent
 * because grid-template-columns wasn't widely tested and the garbage
 * happened to round-trip through arena__compare_grid_tracks identically
 * for identical sources. Genuinely broken whenever tracks really
 * mattered.
 *
 * Grammar supported in fixes148:
 *   none / auto / initial / unset       -> single auto track
 *   <track> <track> <track> ...         -> N explicit tracks
 *   repeat(<int>, <track>+)             -> expanded inline N * inner
 *   repeat(auto-fill|auto-fit, <track>+) -> heuristic 3 * inner
 *   minmax(<min>, <max>)                -> one track (max if max is
 *                                          fr, else min if min is fr,
 *                                          else max as fixed; ignores
 *                                          the min constraint for V2)
 *   fit-content(<length-percentage>)    -> one track, treated as 1fr
 *   <length>                            -> PX track
 *   <percentage>                        -> PERCENT track (Q20.8 of 100%)
 *   <flex>                              -> FR track (Q20.8 ratio)
 *   bare <number>                       -> FR track
 *   <ident>: auto / min-content / max-content -> 1fr fallback
 *   [<line-name>]                       -> skipped, doesn't count as a track
 *
 * Track encoding (matches p_macsurf_grid.c MACSURF_GRID_TRACK_UNIT_*):
 *   bits 31..28 = unit type (NONE=0, FR=1, PX=2, PERCENT=3)
 *   bits 27..0  = value (FR/PERCENT: Q20.8 fixed-point; PX: pixels)
 *
 * Bytecode payload (matches p_macsurf_grid.c):
 *   appendOPV(CSS_PROP_MACSURF_GRID, SET) +
 *   packed cols<<16|rows (rows always 0 for grid-template-columns) +
 *   8 track ints (unused slots = 0).
 *
 * Layout-side consumer: layout_grid.c reads macsurf_grid_tracks[8] and
 * distributes the container width:
 *   1. sum PX + PERCENT (PERCENT resolved against container width)
 *   2. compute total_gap (column-gap * (n-1))
 *   3. remaining = container_width - total_gap - fixed_total
 *   4. distribute remaining across FR tracks proportionally
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

#define AUTO_FILL_FALLBACK_COLUMNS      3

static int32_t pack_track(uint8_t unit, int32_t value)
{
	uint32_t v = ((uint32_t)value) & 0x0FFFFFFFU;
	return (int32_t)((((uint32_t)unit & 0xF) << 28) | v);
}

/* Returns true if the unit token (the trailing part of a DIMENSION) is "fr". */
static bool unit_is_fr(lwc_string *unit_str)
{
	const char *s;
	if (unit_str == NULL) return false;
	if (lwc_string_length(unit_str) != 2) return false;
	s = lwc_string_data(unit_str);
	return (s[0] == 'f' || s[0] == 'F') &&
	       (s[1] == 'r' || s[1] == 'R');
}

/* Case-insensitive byte compare of an lwc_string against a C literal. */
static bool lwc_eq_ci(lwc_string *s, const char *lit)
{
	const char *raw;
	size_t i, n;
	if (s == NULL || lit == NULL) return false;
	n = lwc_string_length(s);
	raw = lwc_string_data(s);
	for (i = 0; i < n; i++) {
		char a = raw[i];
		char b = lit[i];
		if (b == '\0') return false;
		if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
		if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
		if (a != b) return false;
	}
	return lit[n] == '\0';
}

/* Extract unit_str from a DIMENSION token. Returns NULL if not present.
 * Caller must lwc_string_unref() when done. */
static lwc_string *dim_unit_str(const css_token *t, size_t value_consumed)
{
	const char *raw;
	size_t total;
	lwc_string *out = NULL;
	if (t == NULL || t->idata == NULL) return NULL;
	total = lwc_string_length(t->idata);
	if (value_consumed >= total) return NULL;
	raw = lwc_string_data(t->idata);
	if (lwc_intern_string(raw + value_consumed, total - value_consumed,
			&out) != lwc_error_ok) {
		return NULL;
	}
	return out;
}

/* Emit a track into tracks[] if space remains. */
static void emit_track(int32_t tracks[], int *n_tracks,
		uint8_t unit, int32_t value)
{
	if (*n_tracks >= MACSURF_GRID_TRACK_MAX) return;
	tracks[(*n_tracks)++] = pack_track(unit, value);
}

/* Emit a track for a DIMENSION token: <length> as PX, <flex> as FR. */
static void emit_dimension(const css_token *t,
		int32_t tracks[], int *n_tracks)
{
	size_t consumed = 0;
	css_fixed num;
	lwc_string *unit_str;
	int32_t value;

	if (t == NULL || t->idata == NULL) return;
	num = css__number_from_lwc_string(t->idata, false, &consumed);
	unit_str = dim_unit_str(t, consumed);

	if (unit_is_fr(unit_str)) {
		int32_t fr_q88 = (int32_t)(num >> 2);
		if (fr_q88 < 0) fr_q88 = 0;
		if (fr_q88 > 0x0FFFFFFF) fr_q88 = 0x0FFFFFFF;
		value = fr_q88;
		emit_track(tracks, n_tracks, MACSURF_GRID_TRACK_UNIT_FR, value);
	} else {
		int32_t px = (int32_t)(num >> 10);
		if (px < 0) px = 0;
		if (px > 0x0FFFFFFF) px = 0x0FFFFFFF;
		value = px;
		emit_track(tracks, n_tracks, MACSURF_GRID_TRACK_UNIT_PX, value);
	}

	if (unit_str != NULL) lwc_string_unref(unit_str);
}

/* Emit a track for a PERCENTAGE token. */
static void emit_percentage(const css_token *t,
		int32_t tracks[], int *n_tracks)
{
	size_t consumed = 0;
	css_fixed num;
	int32_t pct_q88;
	if (t == NULL || t->idata == NULL) return;
	num = css__number_from_lwc_string(t->idata, false, &consumed);
	pct_q88 = (int32_t)(num >> 2);
	if (pct_q88 < 0) pct_q88 = 0;
	if (pct_q88 > 0x0FFFFFFF) pct_q88 = 0x0FFFFFFF;
	emit_track(tracks, n_tracks, MACSURF_GRID_TRACK_UNIT_PERCENT, pct_q88);
}

/* Emit a track for a bare NUMBER token (treated as <flex>, i.e. fr). */
static void emit_number_as_fr(const css_token *t,
		int32_t tracks[], int *n_tracks)
{
	size_t consumed = 0;
	css_fixed num;
	int32_t fr_q88;
	if (t == NULL || t->idata == NULL) return;
	num = css__number_from_lwc_string(t->idata, false, &consumed);
	fr_q88 = (int32_t)(num >> 2);
	if (fr_q88 < 0) fr_q88 = 0;
	if (fr_q88 > 0x0FFFFFFF) fr_q88 = 0x0FFFFFFF;
	emit_track(tracks, n_tracks, MACSURF_GRID_TRACK_UNIT_FR, fr_q88);
}

/* Skip a balanced function body. Consumes the matching ')'. */
static void skip_function_body(const parserutils_vector *vector, int32_t *ctx)
{
	int depth = 1;
	const css_token *t;
	while (depth > 0 &&
			(t = parserutils_vector_peek(vector, *ctx)) != NULL) {
		if (t->type == CSS_TOKEN_FUNCTION) {
			depth++;
			parserutils_vector_iterate(vector, ctx);
			continue;
		}
		if (t->type == CSS_TOKEN_CHAR && t->idata != NULL &&
				lwc_string_length(t->idata) == 1) {
			char ch = lwc_string_data(t->idata)[0];
			if (ch == '(') {
				depth++;
				parserutils_vector_iterate(vector, ctx);
				continue;
			}
			if (ch == ')') {
				depth--;
				parserutils_vector_iterate(vector, ctx);
				continue;
			}
		}
		if (t->type == CSS_TOKEN_EOF) break;
		parserutils_vector_iterate(vector, ctx);
	}
}

/* Returns true if the token signals end-of-declaration at top level. */
static bool token_is_decl_terminator(const css_token *t)
{
	if (t == NULL || t->type == CSS_TOKEN_EOF) return true;
	if (t->type == CSS_TOKEN_CHAR && t->idata != NULL &&
			lwc_string_length(t->idata) == 1) {
		char ch = lwc_string_data(t->idata)[0];
		if (ch == ';' || ch == '}' || ch == '!') return true;
	}
	return false;
}

/* Returns true if token is a "fixed-ish" track value (length or pct). */
static bool token_is_fixed_track(const css_token *t)
{
	if (t == NULL) return false;
	if (t->type == CSS_TOKEN_PERCENTAGE) return true;
	if (t->type == CSS_TOKEN_DIMENSION && t->idata != NULL) {
		/* Length unit, not fr. */
		size_t consumed = 0;
		(void)css__number_from_lwc_string(t->idata, false, &consumed);
		if (consumed < lwc_string_length(t->idata)) {
			const char *raw = lwc_string_data(t->idata) + consumed;
			size_t len = lwc_string_length(t->idata) - consumed;
			if (!(len == 2 &&
					(raw[0] == 'f' || raw[0] == 'F') &&
					(raw[1] == 'r' || raw[1] == 'R'))) {
				return true;
			}
		}
	}
	return false;
}

/* Returns true if token is an fr track (DIMENSION with fr unit OR bare
 * NUMBER which we treat as fr). */
static bool token_is_fr_track(const css_token *t)
{
	if (t == NULL) return false;
	if (t->type == CSS_TOKEN_NUMBER) return true;
	if (t->type == CSS_TOKEN_DIMENSION && t->idata != NULL) {
		size_t consumed = 0;
		(void)css__number_from_lwc_string(t->idata, false, &consumed);
		if (consumed < lwc_string_length(t->idata)) {
			const char *raw = lwc_string_data(t->idata) + consumed;
			size_t len = lwc_string_length(t->idata) - consumed;
			return (len == 2 &&
					(raw[0] == 'f' || raw[0] == 'F') &&
					(raw[1] == 'r' || raw[1] == 'R'));
		}
	}
	return false;
}

/* Forward declaration: parse a track-list at this scope. inside_function=true
 * means we're inside repeat() and stop at ')' instead of decl terminator. */
static void parse_track_list(css_language *c,
		const parserutils_vector *vector, int32_t *ctx,
		int32_t tracks[], int *n_tracks,
		bool inside_function);

/* Handle minmax(min, max). Emit one track using a simple heuristic:
 *   - if max is fr/number  -> emit fr track using max's value
 *   - else if min is fr    -> emit fr track using min's value
 *   - else                 -> emit max as length/percent/px
 * The min-floor for fr tracks is not honored in this V2 layout pass.
 */
static void parse_minmax(css_language *c,
		const parserutils_vector *vector, int32_t *ctx,
		int32_t tracks[], int *n_tracks)
{
	const css_token *min_tok = NULL;
	const css_token *max_tok = NULL;
	const css_token *t;
	int phase = 0; /* 0 = before min, 1 = before max, 2 = after */
	(void)c;

	while ((t = parserutils_vector_peek(vector, *ctx)) != NULL) {
		if (t->type == CSS_TOKEN_S) {
			parserutils_vector_iterate(vector, ctx);
			continue;
		}
		if (t->type == CSS_TOKEN_CHAR && t->idata != NULL &&
				lwc_string_length(t->idata) == 1) {
			char ch = lwc_string_data(t->idata)[0];
			if (ch == ',') {
				parserutils_vector_iterate(vector, ctx);
				phase++;
				continue;
			}
			if (ch == ')') {
				parserutils_vector_iterate(vector, ctx);
				break;
			}
		}
		if (t->type == CSS_TOKEN_EOF) break;
		if (phase == 0 && min_tok == NULL) {
			min_tok = t;
		} else if (phase == 1 && max_tok == NULL) {
			max_tok = t;
		}
		parserutils_vector_iterate(vector, ctx);
	}

	if (max_tok != NULL && token_is_fr_track(max_tok)) {
		if (max_tok->type == CSS_TOKEN_NUMBER) {
			emit_number_as_fr(max_tok, tracks, n_tracks);
		} else {
			emit_dimension(max_tok, tracks, n_tracks);
		}
		return;
	}
	if (min_tok != NULL && token_is_fr_track(min_tok)) {
		if (min_tok->type == CSS_TOKEN_NUMBER) {
			emit_number_as_fr(min_tok, tracks, n_tracks);
		} else {
			emit_dimension(min_tok, tracks, n_tracks);
		}
		return;
	}
	/* Both fixed (or unrecognised). Prefer max if present, else min. */
	if (max_tok != NULL) {
		if (max_tok->type == CSS_TOKEN_DIMENSION) {
			emit_dimension(max_tok, tracks, n_tracks);
		} else if (max_tok->type == CSS_TOKEN_PERCENTAGE) {
			emit_percentage(max_tok, tracks, n_tracks);
		} else {
			/* Unknown ident (auto, min-content, etc.) - fall to fr. */
			emit_track(tracks, n_tracks,
					MACSURF_GRID_TRACK_UNIT_FR,
					(int32_t)(1 << 8));
		}
		return;
	}
	if (min_tok != NULL) {
		if (min_tok->type == CSS_TOKEN_DIMENSION) {
			emit_dimension(min_tok, tracks, n_tracks);
		} else if (min_tok->type == CSS_TOKEN_PERCENTAGE) {
			emit_percentage(min_tok, tracks, n_tracks);
		} else {
			emit_track(tracks, n_tracks,
					MACSURF_GRID_TRACK_UNIT_FR,
					(int32_t)(1 << 8));
		}
		return;
	}
	/* No tokens recognised - emit 1fr as fallback. */
	emit_track(tracks, n_tracks, MACSURF_GRID_TRACK_UNIT_FR,
			(int32_t)(1 << 8));
}

/* Handle repeat(N, track-list). Emit N copies of the inner track list. */
static void parse_repeat(css_language *c,
		const parserutils_vector *vector, int32_t *ctx,
		int32_t tracks[], int *n_tracks)
{
	int multiplier = 1;
	const css_token *t;
	int32_t inner[MACSURF_GRID_TRACK_MAX];
	int n_inner = 0;
	int i, k;

	for (i = 0; i < MACSURF_GRID_TRACK_MAX; i++) inner[i] = 0;

	while ((t = parserutils_vector_peek(vector, *ctx)) != NULL &&
			t->type == CSS_TOKEN_S) {
		parserutils_vector_iterate(vector, ctx);
	}
	t = parserutils_vector_peek(vector, *ctx);
	if (t != NULL && t->type == CSS_TOKEN_NUMBER && t->idata != NULL) {
		size_t consumed = 0;
		css_fixed v = css__number_from_lwc_string(t->idata, true,
				&consumed);
		int iv = (int)(v >> 10);
		if (iv > 0 && iv < 1000) {
			multiplier = iv;
		}
		parserutils_vector_iterate(vector, ctx);
	} else if (t != NULL && t->type == CSS_TOKEN_IDENT) {
		/* auto-fill / auto-fit / unknown ident - heuristic count. */
		multiplier = AUTO_FILL_FALLBACK_COLUMNS;
		parserutils_vector_iterate(vector, ctx);
	}

	while ((t = parserutils_vector_peek(vector, *ctx)) != NULL &&
			t->type == CSS_TOKEN_S) {
		parserutils_vector_iterate(vector, ctx);
	}
	t = parserutils_vector_peek(vector, *ctx);
	if (t != NULL && t->type == CSS_TOKEN_CHAR && t->idata != NULL &&
			lwc_string_length(t->idata) == 1 &&
			lwc_string_data(t->idata)[0] == ',') {
		parserutils_vector_iterate(vector, ctx);
	}

	/* Parse inner track-list (recursive). */
	parse_track_list(c, vector, ctx, inner, &n_inner, true);

	/* Consume the ')'. parse_track_list returns with ctx pointing AT it. */
	t = parserutils_vector_peek(vector, *ctx);
	if (t != NULL && t->type == CSS_TOKEN_CHAR && t->idata != NULL &&
			lwc_string_length(t->idata) == 1 &&
			lwc_string_data(t->idata)[0] == ')') {
		parserutils_vector_iterate(vector, ctx);
	}

	if (n_inner < 1) {
		/* repeat() with empty inner - emit one auto track. */
		emit_track(tracks, n_tracks, MACSURF_GRID_TRACK_UNIT_FR,
				(int32_t)(1 << 8));
		return;
	}

	for (k = 0; k < multiplier; k++) {
		for (i = 0; i < n_inner; i++) {
			if (*n_tracks >= MACSURF_GRID_TRACK_MAX) return;
			tracks[(*n_tracks)++] = inner[i];
		}
	}
}

/* Walk and parse tracks at this scope. */
static void parse_track_list(css_language *c,
		const parserutils_vector *vector, int32_t *ctx,
		int32_t tracks[], int *n_tracks,
		bool inside_function)
{
	const css_token *t;
	bool match;

	while ((t = parserutils_vector_peek(vector, *ctx)) != NULL) {
		if (inside_function) {
			if (t->type == CSS_TOKEN_CHAR && t->idata != NULL &&
					lwc_string_length(t->idata) == 1 &&
					lwc_string_data(t->idata)[0] == ')') {
				/* Caller consumes the ')'. */
				return;
			}
		} else {
			if (token_is_decl_terminator(t)) return;
		}

		if (t->type == CSS_TOKEN_S) {
			parserutils_vector_iterate(vector, ctx);
			continue;
		}

		if (t->type == CSS_TOKEN_CHAR && t->idata != NULL &&
				lwc_string_length(t->idata) == 1) {
			char ch = lwc_string_data(t->idata)[0];
			if (ch == ',') {
				/* Separator inside function arg list. */
				parserutils_vector_iterate(vector, ctx);
				continue;
			}
			if (ch == '[') {
				/* Line-name list - skip until ']'. */
				parserutils_vector_iterate(vector, ctx);
				while ((t = parserutils_vector_peek(vector,
						*ctx)) != NULL) {
					if (t->type == CSS_TOKEN_CHAR &&
							t->idata != NULL &&
							lwc_string_length(
								t->idata) == 1 &&
							lwc_string_data(
								t->idata)[0] ==
								']') {
						parserutils_vector_iterate(
								vector, ctx);
						break;
					}
					if (t->type == CSS_TOKEN_EOF) break;
					parserutils_vector_iterate(vector, ctx);
				}
				continue;
			}
		}

		if (t->type == CSS_TOKEN_FUNCTION && t->idata != NULL) {
			match = false;
			parserutils_vector_iterate(vector, ctx);
			if (lwc_string_caseless_isequal(t->idata,
					c->strings[REPEAT], &match) ==
					lwc_error_ok && match) {
				parse_repeat(c, vector, ctx, tracks, n_tracks);
				continue;
			}
			if (lwc_eq_ci(t->idata, "minmax")) {
				parse_minmax(c, vector, ctx, tracks, n_tracks);
				continue;
			}
			/* Other function (fit-content, etc.) - emit as 1fr,
			 * skip the body. */
			emit_track(tracks, n_tracks,
					MACSURF_GRID_TRACK_UNIT_FR,
					(int32_t)(1 << 8));
			skip_function_body(vector, ctx);
			continue;
		}

		if (t->type == CSS_TOKEN_DIMENSION) {
			emit_dimension(t, tracks, n_tracks);
			parserutils_vector_iterate(vector, ctx);
			continue;
		}
		if (t->type == CSS_TOKEN_PERCENTAGE) {
			emit_percentage(t, tracks, n_tracks);
			parserutils_vector_iterate(vector, ctx);
			continue;
		}
		if (t->type == CSS_TOKEN_NUMBER) {
			emit_number_as_fr(t, tracks, n_tracks);
			parserutils_vector_iterate(vector, ctx);
			continue;
		}
		if (t->type == CSS_TOKEN_IDENT) {
			/* auto / min-content / max-content / unknown idents
			 * count as one fr track so the column count is at
			 * least preserved. */
			emit_track(tracks, n_tracks,
					MACSURF_GRID_TRACK_UNIT_FR,
					(int32_t)(1 << 8));
			parserutils_vector_iterate(vector, ctx);
			continue;
		}

		/* Unknown token - consume so we don't loop forever. */
		parserutils_vector_iterate(vector, ctx);
	}
}


css_error css__parse_grid_template_columns(css_language *c,
		const parserutils_vector *vector, int32_t *ctx,
		css_style *result)
{
	int32_t orig_ctx = *ctx;
	css_error error;
	const css_token *token;
	enum flag_value flag_value;
	int32_t tracks[MACSURF_GRID_TRACK_MAX];
	int n_tracks = 0;
	uint32_t packed;
	uint32_t cols;
	bool match = false;
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

	/* none / auto -> single 1fr track (effectively block-like layout). */
	token = parserutils_vector_peek(vector, *ctx);
	if (token != NULL && token->type == CSS_TOKEN_IDENT &&
			token->idata != NULL) {
		if ((lwc_string_caseless_isequal(token->idata,
				c->strings[NONE], &match) == lwc_error_ok &&
				match) ||
		    (lwc_string_caseless_isequal(token->idata,
				c->strings[AUTO], &match) == lwc_error_ok &&
				match)) {
			parserutils_vector_iterate(vector, ctx);
			n_tracks = 1;
			tracks[0] = pack_track(MACSURF_GRID_TRACK_UNIT_FR,
					(int32_t)(1 << 8));
			goto emit;
		}
	}

	parse_track_list(c, vector, ctx, tracks, &n_tracks, false);

	if (n_tracks < 1) {
		/* Nothing parsed - emit a single 1fr track so the property
		 * still computes to SET and downstream code sees a valid
		 * 1-column grid. */
		n_tracks = 1;
		tracks[0] = pack_track(MACSURF_GRID_TRACK_UNIT_FR,
				(int32_t)(1 << 8));
	}

emit:
	cols = (uint32_t)n_tracks;
	if (cols < 1) cols = 1;
	if (cols > 255) cols = 255;
	packed = (cols << 16) | 0u; /* rows = 0 (auto) */

	error = css__stylesheet_style_appendOPV(result,
			CSS_PROP_MACSURF_GRID, 0, 0x0080 /* SET */);
	if (error != CSS_OK) {
		*ctx = orig_ctx;
		return error;
	}

	error = css__stylesheet_style_vappend(result, 1, (css_fixed)packed);
	if (error != CSS_OK) {
		*ctx = orig_ctx;
		return error;
	}

	for (i = 0; i < MACSURF_GRID_TRACK_MAX; i++) {
		error = css__stylesheet_style_vappend(result, 1,
				(css_fixed)tracks[i]);
		if (error != CSS_OK) {
			*ctx = orig_ctx;
			return error;
		}
	}

	return CSS_OK;
}
