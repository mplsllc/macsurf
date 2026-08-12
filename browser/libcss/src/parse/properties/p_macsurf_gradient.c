/*
 * This file is part of LibCSS.
 * Licensed under the MIT License,
 *                http://www.opensource.org/licenses/mit-license.php
 * Copyright 2025 Gemini CLI
 */

#include <assert.h>
#include <string.h>

#include "bytecode/bytecode.h"
#include "bytecode/opcodes.h"
#include "parse/properties/properties.h"
#include "parse/properties/utils.h"

/**
 * Parse -macsurf-gradient.
 *
 * Accepted forms (all map to a vertical or horizontal 2-stop gradient
 * because the storage slot only carries two colours):
 *
 *   -macsurf-gradient: linear-gradient(c1, c2)
 *   -macsurf-gradient: linear-gradient(c1, c2, c3, ...)        fixes49
 *   -macsurf-gradient: linear-gradient(to right, c1, c2)       fixes48
 *   -macsurf-gradient: linear-gradient(to left, c1, c2)        fixes48
 *   -macsurf-gradient: linear-gradient(to top|bottom, c1, c2)  fixes49
 *   -macsurf-gradient: linear-gradient(0deg, c1, c2)           fixes49
 *   -macsurf-gradient: linear-gradient(90deg, c1, c2)          fixes49
 *   -macsurf-gradient: linear-gradient(180deg, c1, c2)         fixes49
 *   -macsurf-gradient: linear-gradient(270deg, c1, c2)         fixes49
 *
 * Multi-stop forms: only the FIRST and LAST colour are kept; the
 * intermediate stops are silently dropped. Good-enough rendering for
 * most multi-stop CSS pages on a 2-stop renderer.
 *
 * Angle handling: anything in 45..134 or 225..314 degrees is treated
 * as horizontal; everything else is vertical. Defaults to vertical.
 *
 * Direction is encoded in the emitted OPV value:
 *   0x0080 = SET vertical (default, fixes37/47)
 *   0x00C0 = SET horizontal (fixes48)
 */
css_error css__parse_macsurf_gradient(css_language *c,
		const parserutils_vector *vector, int32_t *ctx,
		css_style *result)
{
	int32_t orig_ctx = *ctx;
	css_error error;
	const css_token *token;
	css_color first_color = 0;
	css_color last_color = 0;
	int color_count = 0;
	/* fixes346 - track the first stop colour that differs from
	 * first_color. Used downstream so `repeating-linear-gradient`
	 * patterns like `white 0, white 1px, gray 1px, gray 2px, white 2px,
	 * white 3px` (mactrove's Platinum titlebar pinstripe) render as a
	 * white→gray fade instead of white→white solid. fixes318's
	 * preprocessor strips the `repeating-` prefix; here the same
	 * symptom (first == last across N>=3 stops) is the recovery
	 * trigger. */
	css_color second_distinct = 0;
	bool has_second_distinct = false;
	enum flag_value flag_value;
	bool match = false;
	/* fixes365b - capture the full angle (0..359), the first three stop
	 * colours, and their explicit positions. When any of these require
	 * fidelity the legacy 2-stop fast path can't express (diagonal
	 * angle 45/135/225/315, or a third colour, or positions other than
	 * the default 0%/100%), the cascade is asked to allocate the 7-int
	 * side-channel via an extra OPV value 0x0180. Cardinal 2-stop rules
	 * with default stop positions keep using the legacy 0x0080/0x00C0
	 * paths so existing visual output is byte-for-byte unchanged. */
	int captured_angle = 0;
	bool captured_angle_seen = false;
	css_color stop_colors[3];
	int32_t stop_positions[3];
	bool stop_position_set[3];
	int stop_index;
	for (stop_index = 0; stop_index < 3; stop_index++) {
		stop_colors[stop_index] = 0;
		stop_positions[stop_index] = -1;
		stop_position_set[stop_index] = false;
	}

	token = parserutils_vector_peek(vector, *ctx);
	if (token == NULL) return CSS_INVALID;

	flag_value = get_css_flag_value(c, token);
	if (flag_value != FLAG_VALUE__NONE) {
		parserutils_vector_iterate(vector, ctx);
		return css_stylesheet_style_flag_value(result, flag_value, CSS_PROP_MACSURF_GRADIENT);
	}

	/* fixes74: also accept radial-gradient(...). The two-stop storage
	 * is shared with linear-gradient; the difference is encoded in the
	 * emitted OPV value (0x0100 = SET radial). Radial-gradient's
	 * optional shape/position prefix (`circle at center`, etc.) is
	 * silently consumed before the colour stops. */
	{
		bool is_linear = false;
		bool is_radial = false;
		if (token->type == CSS_TOKEN_FUNCTION) {
			bool m_lin = false, m_rad = false;
			(void)lwc_string_caseless_isequal(token->idata,
			        c->strings[LINEAR_GRADIENT], &m_lin);
			(void)lwc_string_caseless_isequal(token->idata,
			        c->strings[RADIAL_GRADIENT], &m_rad);
			is_linear = m_lin;
			is_radial = m_rad;
			match = m_lin || m_rad;
		}
	if (match) {
		bool horizontal = false;
		bool radial = is_radial;
		uint16_t set_value = is_radial ? 0x0100 : 0x0080;
		/* fixes345: radial size + position prefix capture. */
		int32_t rad_data[4];
		bool rad_data_set = false;
		rad_data[0] = -1; rad_data[1] = -1;
		rad_data[2] = -1; rad_data[3] = -1;
		(void)is_linear;
		parserutils_vector_iterate(vector, ctx);
		consumeWhitespace(vector, ctx);

		/* Radial: optional shape/position prefix may appear before
		 * the first colour stop (`circle`, `ellipse`, `circle at
		 * center`, `closest-side`, etc.). Save ctx, try parsing a
		 * colour at the current position; if that fails, it's a
		 * prefix -- skip tokens up to and including the first
		 * top-level comma. If it succeeds, rewind so the main colour
		 * loop sees the same token first.
		 *
		 * fixes345: also try to extract `<W>px <H>px at <X>%
		 * <Y>%,` and capture into rad_data so the painter can place
		 * the gradient where the author intended. */
		if (radial) {
			int32_t probe_ctx = *ctx;
			css_color probe_color = 0;
			uint16_t probe_type = 0;
			css_error probe_err = css__parse_colour_specifier(c,
					vector, &probe_ctx, &probe_type,
					&probe_color);
			if (probe_err != CSS_OK) {
				/* Prefix present. Try to extract the
				 * specific pattern `<W>px <H>px at <X>%
				 * <Y>%,` first; on success rad_data_set
				 * is true and we fall through to the
				 * generic scan-to-comma to consume any
				 * leftover tokens. */
				int32_t before = *ctx;
				int got = 0;
				int captured[4] = {-1, -1, -1, -1};
				int i;
				for (i = 0; i < 4 && got == i; i++) {
					const css_token *t =
						parserutils_vector_peek(
							vector, *ctx);
					if (t == NULL) break;
					if (t->type == CSS_TOKEN_DIMENSION &&
					    i < 2) {
						const char *d = lwc_string_data(
								t->idata);
						size_t dl = lwc_string_length(
								t->idata);
						/* Accept "Npx" only. */
						if (dl >= 3 &&
						    (d[dl-2]=='p'||d[dl-2]=='P') &&
						    (d[dl-1]=='x'||d[dl-1]=='X')) {
							int val = 0;
							size_t j;
							int seen = 0;
							for (j = 0; j < dl - 2;
								j++) {
								char ch = d[j];
								if (ch >= '0' &&
								    ch <= '9') {
									val = val*10 + (ch-'0');
									seen = 1;
								} else break;
							}
							if (seen) {
								captured[i] = val;
								got++;
								parserutils_vector_iterate(
									vector, ctx);
								consumeWhitespace(
									vector, ctx);
								continue;
							}
						}
						break;
					}
					if (i == 2) {
						/* Expecting `at` ident. */
						if (t->type ==
							CSS_TOKEN_IDENT) {
							const char *d = lwc_string_data(
								t->idata);
							size_t dl = lwc_string_length(
								t->idata);
							if (dl == 2 &&
							    (d[0]=='a'||d[0]=='A') &&
							    (d[1]=='t'||d[1]=='T')) {
								parserutils_vector_iterate(
									vector, ctx);
								consumeWhitespace(
									vector, ctx);
								/* don't bump got;
								 * next iter
								 * still i=2 */
								continue;
							}
						}
						break;
					}
					/* i==2 or 3: percent for position. */
					if (t->type == CSS_TOKEN_PERCENTAGE) {
						const char *d = lwc_string_data(
								t->idata);
						size_t dl = lwc_string_length(
								t->idata);
						int sign = 1;
						int val = 0;
						size_t j = 0;
						int seen = 0;
						if (dl > 0 && d[0] == '-') {
							sign = -1; j = 1;
						} else if (dl > 0 && d[0] == '+') {
							j = 1;
						}
						for (; j < dl; j++) {
							char ch = d[j];
							if (ch >= '0' && ch <= '9') {
								val = val*100 + (ch-'0')*100;
								seen = 1;
							} else break;
						}
						if (seen) {
							captured[i] = sign * val;
							got++;
							parserutils_vector_iterate(
								vector, ctx);
							consumeWhitespace(
								vector, ctx);
							continue;
						}
						break;
					}
					break;
				}
				if (got >= 2) {
					rad_data[0] = captured[0];
					rad_data[1] = captured[1];
					rad_data[2] = captured[2];
					rad_data[3] = captured[3];
					rad_data_set = true;
				} else {
					/* Pattern not matched, rewind for the
					 * generic scan below. */
					*ctx = before;
				}
				{
				/* Generic: scan to first top-level comma
				 * and consume it. */
				int depth = 0;
				while (1) {
					token = parserutils_vector_peek(vector,
							*ctx);
					if (token == NULL) break;
					if (token->type == CSS_TOKEN_CHAR &&
					    token->idata != NULL) {
						const char *s = lwc_string_data(
								token->idata);
						if (s != NULL && s[0] == '(') {
							depth++;
						} else if (s != NULL &&
							   s[0] == ')') {
							if (depth == 0) break;
							depth--;
						} else if (s != NULL &&
							   s[0] == ',' &&
							   depth == 0) {
							parserutils_vector_iterate(
								vector, ctx);
							break;
						}
					}
					parserutils_vector_iterate(vector, ctx);
				}
				}
			}
			/* If colour probe succeeded, ctx is unchanged --
			 * main loop will reparse the colour. */
			consumeWhitespace(vector, ctx);
		}
		(void)rad_data; (void)rad_data_set;

		/* Optional direction prefix: `to <side>` or `<angle>deg`.
		 * Linear only - radial direction has already been consumed
		 * above. */
		token = parserutils_vector_peek(vector, *ctx);
		if (!radial && token != NULL && token->type == CSS_TOKEN_IDENT) {
			bool to_match = false;
			if (lwc_string_caseless_isequal(token->idata,
					c->strings[TO], &to_match)
					== lwc_error_ok && to_match) {
				parserutils_vector_iterate(vector, ctx);
				consumeWhitespace(vector, ctx);
				token = parserutils_vector_peek(vector, *ctx);
				if (token != NULL &&
				    token->type == CSS_TOKEN_IDENT) {
					bool side_match = false;
					if ((lwc_string_caseless_isequal(
					        token->idata,
					        c->strings[LIBCSS_RIGHT],
					        &side_match) == lwc_error_ok
					     && side_match) ||
					    (lwc_string_caseless_isequal(
					        token->idata,
					        c->strings[LIBCSS_LEFT],
					        &side_match) == lwc_error_ok
					     && side_match)) {
						horizontal = true;
					}
					/* `to top|bottom|center` falls through
					 * with horizontal=false (default). */
					parserutils_vector_iterate(vector, ctx);
				}
				consumeWhitespace(vector, ctx);
				token = parserutils_vector_peek(vector, *ctx);
				if (token != NULL && token->type == CSS_TOKEN_CHAR &&
				    lwc_string_data(token->idata)[0] == ',')
					parserutils_vector_iterate(vector, ctx);
			}
		} else if (!radial && token != NULL &&
		           token->type == CSS_TOKEN_DIMENSION) {
			/* fixes49 -- angle prefix: NUMBERdeg. The dimension
			 * token's idata holds the suffix; check that it's
			 * "deg" (case-insensitive). */
			bool deg_match = false;
			(void)lwc_string_caseless_isequal(token->idata,
					c->strings[c->strings ? 0 : 0],
					&deg_match);
			/* Simpler approach: just look at the trailing 3 chars
			 * of the dimension token's lower-case body. */
			{
				const char *data = lwc_string_data(token->idata);
				size_t len = lwc_string_length(token->idata);
				int angle = 0;
				size_t i;
				bool seen_digit = false;
				bool is_deg = (len >= 3) &&
					((data[len-3] == 'd' || data[len-3] == 'D') &&
					 (data[len-2] == 'e' || data[len-2] == 'E') &&
					 (data[len-1] == 'g' || data[len-1] == 'G'));
				if (is_deg) {
					/* Parse the leading integer portion.
					 * Floating-point is not supported here
					 * since 0/45/90/etc. cover the common
					 * angles. */
					int sign = 1;
					for (i = 0; i < len - 3; i++) {
						char ch = data[i];
						if (i == 0 && ch == '-') {
							sign = -1;
							continue;
						}
						if (i == 0 && ch == '+') {
							continue;
						}
						if (ch >= '0' && ch <= '9') {
							angle = angle * 10 +
								(ch - '0');
							seen_digit = true;
						} else {
							/* Non-integer; bail. */
							is_deg = false;
							break;
						}
					}
					if (seen_digit) {
						int a = (angle * sign) % 360;
						if (a < 0) a += 360;
						/* fixes365b - preserve full angle
						 * for the side-channel; legacy
						 * cardinal collapse stays for the
						 * fast path. */
						captured_angle = a;
						captured_angle_seen = true;
						if ((a >= 45 && a < 135) ||
						    (a >= 225 && a < 315))
							horizontal = true;
					}
				}
				if (is_deg) {
					parserutils_vector_iterate(vector, ctx);
					consumeWhitespace(vector, ctx);
					token = parserutils_vector_peek(vector, *ctx);
					if (token != NULL &&
					    token->type == CSS_TOKEN_CHAR &&
					    lwc_string_data(token->idata)[0] == ',')
						parserutils_vector_iterate(vector, ctx);
				}
			}
		}

		/* Radial keeps its 0x0100 set_value regardless of any
		 * direction tokens; horizontal only applies to linear. */
		if (!radial && horizontal) {
			set_value = 0x00C0;
		}
		(void)radial;

		/* fixes49 -- accept N colour stops. Only first and last
		 * survive into bytecode (storage slot is 2 colours). */
		while (1) {
			css_color tmp_color = 0;
			uint16_t tmp_type = 0;
			consumeWhitespace(vector, ctx);
			token = parserutils_vector_peek(vector, *ctx);
			if (token == NULL) break;
			if (token->type == CSS_TOKEN_CHAR &&
			    lwc_string_data(token->idata)[0] == ')')
				break;
			error = css__parse_colour_specifier(c, vector, ctx,
					&tmp_type, &tmp_color);
			if (error != CSS_OK) {
				*ctx = orig_ctx;
				return error;
			}
			if (color_count == 0) {
				first_color = tmp_color;
			} else if (!has_second_distinct &&
			           tmp_color != first_color) {
				second_distinct = tmp_color;
				has_second_distinct = true;
			}
			last_color = tmp_color;
			/* fixes365b - also record the first 3 colour stops
			 * into the side-channel capture array. */
			if (color_count < 3) {
				stop_colors[color_count] = tmp_color;
			}
			color_count++;
			consumeWhitespace(vector, ctx);
			/* fixes318 (#148) - accept (and historically ignored)
			 * optional stop position after each colour. CSS modern
			 * syntax allows `c1 0%, c2 100%` or `c1 50px, c2 75%`.
			 * The legacy 2-stop slot couldn't express stops other
			 * than 0%/100% so positions used to drop silently.
			 *
			 * fixes365b - now capture percentage positions into the
			 * stop_positions array (percent×100 fixed). Dimension
			 * / number positions still drop because the px→percent
			 * resolution needs the box width at paint time and
			 * isn't worth the round-trip for the common case (the
			 * mactrove rule uses 0%/50%/100% percentages). */
			token = parserutils_vector_peek(vector, *ctx);
			if (token != NULL &&
			    token->type == CSS_TOKEN_PERCENTAGE) {
				const char *data = lwc_string_data(
						token->idata);
				size_t len = lwc_string_length(
						token->idata);
				int sign = 1;
				int val = 0;
				size_t j = 0;
				bool seen = false;
				if (len > 0 && data[0] == '-') {
					sign = -1; j = 1;
				} else if (len > 0 && data[0] == '+') {
					j = 1;
				}
				for (; j < len; j++) {
					char ch = data[j];
					if (ch >= '0' && ch <= '9') {
						val = val * 10 +
							(ch - '0');
						seen = true;
					} else {
						break;
					}
				}
				/* fixes365b review fix - accumulate digits as
				 * plain integer, then scale to percent×100
				 * after the loop. Previous code multiplied by
				 * 100 per digit, producing 50000 for "50%"
				 * instead of 5000 and breaking every 3-stop
				 * gradient with explicit positions. */
				if (seen) {
					val = val * 100;
				}
				if (seen && color_count - 1 < 3) {
					stop_positions[color_count - 1] =
							sign * val;
					stop_position_set[color_count - 1] =
							true;
				}
				parserutils_vector_iterate(vector, ctx);
				consumeWhitespace(vector, ctx);
				token = parserutils_vector_peek(vector, *ctx);
			} else if (token != NULL &&
			    (token->type == CSS_TOKEN_DIMENSION ||
			     token->type == CSS_TOKEN_NUMBER)) {
				parserutils_vector_iterate(vector, ctx);
				consumeWhitespace(vector, ctx);
				token = parserutils_vector_peek(vector, *ctx);
			}
			if (token != NULL && token->type == CSS_TOKEN_CHAR &&
			    lwc_string_data(token->idata)[0] == ',')
				parserutils_vector_iterate(vector, ctx);
		}

		if (color_count < 1) {
			*ctx = orig_ctx;
			return CSS_INVALID;
		}
		/* Single-stop falls back to a uniform fill (first == last). */
		if (color_count == 1) last_color = first_color;

		/* fixes346 - pinstripe / repeating pattern recovery.
		 * When the source was `repeating-linear-gradient(white 0, white
		 * 1px, gray 1px, gray 2px, white 2px, white 3px)` (mactrove's
		 * Platinum titlebar) the fixes318 preprocessor strips the
		 * `repeating-` prefix and the loop above sees 6 colour stops
		 * with first == last == white. Without recovery the resulting
		 * gradient is white→white = solid white, completely losing the
		 * pinstripe appearance.
		 *
		 * Rule: if there are 3+ stops AND first == last AND we saw a
		 * distinct intermediate colour, swap last for that distinct
		 * colour. The 2-stop storage then renders as a soft fade between
		 * the dominant and accent colours, visually approximating the
		 * pinstripe at any zoom. Regular 3-stop gradients
		 * (white→mid→dark) aren't affected because first != last for
		 * them. */
		if (color_count >= 3 && first_color == last_color &&
		    has_second_distinct) {
			last_color = second_distinct;
		}

		/* Close parenthesis. */
		consumeWhitespace(vector, ctx);
		token = parserutils_vector_peek(vector, *ctx);
		if (token != NULL && token->type == CSS_TOKEN_CHAR &&
		    lwc_string_data(token->idata)[0] == ')')
			parserutils_vector_iterate(vector, ctx);

		/* fixes365b - promote to the extended-linear path when the
		 * rule expresses something the legacy 2-stop cardinal slot
		 * can't carry:
		 *   - 3+ distinct colour stops with positions
		 *   - non-cardinal angle (diagonals: 45/135/225/315 etc.)
		 *   - any stop position other than the default 0%/100%
		 *
		 * Promotion only applies to linear; radial keeps its own
		 * 0x0100 tail (size+position) untouched. */
		{
			bool promote_linear = false;
			int extended_pos[3];
			css_color extended_cols[3];
			int extended_angle = 0;
			int extended_count = (color_count >= 3) ? 3 :
					(color_count >= 2 ? 2 : 1);
			int p_idx;

			if (!radial) {
				/* Diagonal angle? */
				if (captured_angle_seen) {
					int a = captured_angle;
					if (a != 0 && a != 90 && a != 180 &&
					    a != 270) {
						promote_linear = true;
					}
				}
				/* Third stop with author intent? */
				if (color_count >= 3) {
					promote_linear = true;
				}
				/* Non-default 2-stop positions? */
				if (color_count == 2 &&
				    (stop_position_set[0] ||
				     stop_position_set[1])) {
					/* Default = 0%/100%. If positions
					 * are explicitly set to anything
					 * other than that, take the path. */
					int p0 = stop_position_set[0] ?
						stop_positions[0] : 0;
					int p1 = stop_position_set[1] ?
						stop_positions[1] : 10000;
					if (p0 != 0 || p1 != 10000) {
						promote_linear = true;
					}
				}
			}

			if (promote_linear) {
				/* Build the 7-int extended descriptor. */
				int default_positions[3];
				if (extended_count == 1) {
					default_positions[0] = 0;
					default_positions[1] = 10000;
					default_positions[2] = 0;
				} else if (extended_count == 2) {
					default_positions[0] = 0;
					default_positions[1] = 10000;
					default_positions[2] = 0;
				} else {
					default_positions[0] = 0;
					default_positions[1] = 5000;
					default_positions[2] = 10000;
				}
				for (p_idx = 0; p_idx < 3; p_idx++) {
					if (stop_position_set[p_idx]) {
						extended_pos[p_idx] =
							stop_positions[p_idx];
					} else {
						extended_pos[p_idx] =
							default_positions[p_idx];
					}
					extended_cols[p_idx] =
						stop_colors[p_idx];
				}
				/* Single-stop already falls through to
				 * uniform first==last above; the extended
				 * descriptor still needs valid colours, so
				 * mirror them. */
				if (extended_count == 1) {
					extended_cols[1] = extended_cols[0];
				}
				if (extended_count < 3) {
					extended_cols[2] = 0;
					extended_pos[2] = 0;
				}
				extended_angle = captured_angle_seen ?
					captured_angle : 0;
				/* Encode stop count (2 or 3) in the high
				 * bits of the angle word so the painter
				 * can distinguish 2-stop from 3-stop
				 * without ambiguity when col2/pos2 happen
				 * to be 0. Angle low 16 bits = 0..359,
				 * high 16 bits = stop_count (2 or 3). */
				{
					uint32_t enc = (uint32_t)extended_angle
						& 0xffffu;
					enc |= ((uint32_t)extended_count) << 16;
					extended_angle = (int)enc;
				}

				error = css__stylesheet_style_appendOPV(result,
						CSS_PROP_MACSURF_GRADIENT, 0,
						0x0180);
				if (error != CSS_OK) return error;
				/* Tail layout: legacy 2 colours (c1, last)
				 * for backward read-compat, then the 7-word
				 * extension. Cascade detects 0x0180 and
				 * reads 9 words total (2 legacy + 7
				 * extension). */
				return css__stylesheet_style_vappend(result, 9,
					(css_code_t)first_color,
					(css_code_t)last_color,
					(css_code_t)(uint32_t)extended_angle,
					(css_code_t)(uint32_t)extended_pos[0],
					(css_code_t)(uint32_t)extended_pos[1],
					(css_code_t)(uint32_t)extended_pos[2],
					(css_code_t)(uint32_t)extended_cols[0],
					(css_code_t)(uint32_t)extended_cols[1],
					(css_code_t)(uint32_t)extended_cols[2]);
			}
		}

		error = css__stylesheet_style_appendOPV(result,
				CSS_PROP_MACSURF_GRADIENT, 0, set_value);
		if (error != CSS_OK) return error;

		/* fixes345 - radial-gradient bytecode is now extended with
		 * a 5-slot tail: [rad_data_set_flag, sx, sy, px, py]. The
		 * cascade reads those when set_value == 0x0100. Linear
		 * bytecode is unchanged (2-slot c1, c2). */
		if (set_value == 0x0100) {
			return css__stylesheet_style_vappend(result, 7,
					first_color, last_color,
					(uint32_t)(rad_data_set ? 1 : 0),
					(uint32_t)rad_data[0],
					(uint32_t)rad_data[1],
					(uint32_t)rad_data[2],
					(uint32_t)rad_data[3]);
		}
		return css__stylesheet_style_vappend(result, 2,
				first_color, last_color);
	}
	}

	return CSS_INVALID;
}
