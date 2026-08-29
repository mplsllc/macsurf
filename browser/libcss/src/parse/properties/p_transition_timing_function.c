/*
 * This file is part of LibCSS.
 * Licensed under the MIT License.
 */

#include <assert.h>
#include <string.h>

#include <libcss/computed.h>
#include "bytecode/bytecode.h"
#include "bytecode/opcodes.h"
#include "parse/properties/properties.h"
#include "parse/properties/utils.h"

static inline bool is_token_comma(const css_token *t)
{
	return (t != NULL && t->type == CSS_TOKEN_CHAR && lwc_string_data(t->idata)[0] == ',');
}

/* Convert standard float to Q16.16 fixed-point */
static int32_t flt_to_q16(double val)
{
	return (int32_t)(val * 65536.0 + (val >= 0 ? 0.5 : -0.5));
}

css_error css__parse_transition_timing_function_item(css_language *c,
		const parserutils_vector *vector, int32_t *ctx,
		css_transition_timing_entry *out)
{
	const css_token *token;

	token = parserutils_vector_peek(vector, *ctx);
	if (token == NULL) return CSS_INVALID;

	if (token->type == CSS_TOKEN_IDENT) {
		const char *data = lwc_string_data(token->idata);
		size_t len = lwc_string_length(token->idata);

		if (len == 4 && strncasecmp(data, "ease", 4) == 0) {
			out->type = CSS_TIMING_EASE;
			out->x1 = flt_to_q16(0.25); out->y1 = flt_to_q16(0.1);
			out->x2 = flt_to_q16(0.25); out->y2 = flt_to_q16(1.0);
			parserutils_vector_iterate(vector, ctx);
			return CSS_OK;
		} else if (len == 6 && strncasecmp(data, "linear", 6) == 0) {
			out->type = CSS_TIMING_LINEAR;
			out->x1 = flt_to_q16(0.0); out->y1 = flt_to_q16(0.0);
			out->x2 = flt_to_q16(1.0); out->y2 = flt_to_q16(1.0);
			parserutils_vector_iterate(vector, ctx);
			return CSS_OK;
		} else if (len == 7 && strncasecmp(data, "ease-in", 7) == 0) {
			out->type = CSS_TIMING_EASE_IN;
			out->x1 = flt_to_q16(0.42); out->y1 = flt_to_q16(0.0);
			out->x2 = flt_to_q16(1.0);  out->y2 = flt_to_q16(1.0);
			parserutils_vector_iterate(vector, ctx);
			return CSS_OK;
		} else if (len == 8 && strncasecmp(data, "ease-out", 8) == 0) {
			out->type = CSS_TIMING_EASE_OUT;
			out->x1 = flt_to_q16(0.0);  out->y1 = flt_to_q16(0.0);
			out->x2 = flt_to_q16(0.58); out->y2 = flt_to_q16(1.0);
			parserutils_vector_iterate(vector, ctx);
			return CSS_OK;
		} else if (len == 11 && strncasecmp(data, "ease-in-out", 11) == 0) {
			out->type = CSS_TIMING_EASE_IN_OUT;
			out->x1 = flt_to_q16(0.42); out->y1 = flt_to_q16(0.0);
			out->x2 = flt_to_q16(0.58); out->y2 = flt_to_q16(1.0);
			parserutils_vector_iterate(vector, ctx);
			return CSS_OK;
		} else if (len == 10 && strncasecmp(data, "step-start", 10) == 0) {
			out->type = CSS_TIMING_STEPS;
			out->step_count = 1;
			out->step_pos = 0; /* JUMP_START */
			parserutils_vector_iterate(vector, ctx);
			return CSS_OK;
		} else if (len == 8 && strncasecmp(data, "step-end", 8) == 0) {
			out->type = CSS_TIMING_STEPS;
			out->step_count = 1;
			out->step_pos = 1; /* JUMP_END */
			parserutils_vector_iterate(vector, ctx);
			return CSS_OK;
		}
		return CSS_INVALID;
	} else if (token->type == CSS_TOKEN_FUNCTION) {
		const char *data = lwc_string_data(token->idata);
		size_t len = lwc_string_length(token->idata);

		if (len == 12 && strncasecmp(data, "cubic-bezier", 12) == 0) {
			css_fixed raw_vals[4];
			int i;
			parserutils_vector_iterate(vector, ctx); /* consume cubic-bezier( */

			for (i = 0; i < 4; i++) {
				size_t consumed = 0;
				token = parserutils_vector_peek(vector, *ctx);
				if (token == NULL || token->type != CSS_TOKEN_NUMBER)
					return CSS_INVALID;

				raw_vals[i] = css__number_from_lwc_string(token->idata, true, &consumed);
				parserutils_vector_iterate(vector, ctx);
				consumeWhitespace(vector, ctx);

				token = parserutils_vector_peek(vector, *ctx);
				if (i < 3) {
					if (!is_token_comma(token))
						return CSS_INVALID;
					parserutils_vector_iterate(vector, ctx);
					consumeWhitespace(vector, ctx);
				}
			}

			token = parserutils_vector_peek(vector, *ctx);
			if (token == NULL || token->type != CSS_TOKEN_CHAR ||
				lwc_string_data(token->idata)[0] != ')')
				return CSS_INVALID;
			parserutils_vector_iterate(vector, ctx); /* consume ) */

			/* CSS spec: x1 and x2 MUST be in range [0, 1] */
			/* Q22.10: 0 is 0, 1.0 is 1024 (1 << 10) */
			if (raw_vals[0] < 0 || raw_vals[0] > (1 << 10) ||
				raw_vals[2] < 0 || raw_vals[2] > (1 << 10)) {
				return CSS_INVALID;
			}

			out->type = CSS_TIMING_CUBIC_BEZIER;
			/* Convert Q22.10 to Q16.16: << 6 */
			out->x1 = (int32_t)(raw_vals[0] << 6);
			out->y1 = (int32_t)(raw_vals[1] << 6);
			out->x2 = (int32_t)(raw_vals[2] << 6);
			out->y2 = (int32_t)(raw_vals[3] << 6);
			out->step_count = 0;
			out->step_pos = 0;
			return CSS_OK;
		} else if (len == 5 && strncasecmp(data, "steps", 5) == 0) {
			size_t consumed = 0;
			css_fixed count_val;
			parserutils_vector_iterate(vector, ctx); /* consume steps( */

			token = parserutils_vector_peek(vector, *ctx);
			if (token == NULL || token->type != CSS_TOKEN_NUMBER)
				return CSS_INVALID;

			count_val = css__number_from_lwc_string(token->idata, true, &consumed);
			if (count_val <= 0) return CSS_INVALID;
			out->step_count = (uint32_t)count_val;
			out->step_pos = 1; /* default: end / jump-end */
			parserutils_vector_iterate(vector, ctx);
			consumeWhitespace(vector, ctx);

			token = parserutils_vector_peek(vector, *ctx);
			if (is_token_comma(token)) {
				parserutils_vector_iterate(vector, ctx);
				consumeWhitespace(vector, ctx);
				token = parserutils_vector_peek(vector, *ctx);
				if (token != NULL && token->type == CSS_TOKEN_IDENT) {
					const char *pdata = lwc_string_data(token->idata);
					size_t plen = lwc_string_length(token->idata);
					if (plen == 5 && strncasecmp(pdata, "start", 5) == 0) {
						out->step_pos = 0;
					} else if (plen == 3 && strncasecmp(pdata, "end", 3) == 0) {
						out->step_pos = 1;
					} else if (plen == 10 && strncasecmp(pdata, "jump-start", 10) == 0) {
						out->step_pos = 0;
					} else if (plen == 8 && strncasecmp(pdata, "jump-end", 8) == 0) {
						out->step_pos = 1;
					} else if (plen == 9 && strncasecmp(pdata, "jump-none", 9) == 0) {
						out->step_pos = 2;
					} else if (plen == 9 && strncasecmp(pdata, "jump-both", 9) == 0) {
						out->step_pos = 3;
					}
					parserutils_vector_iterate(vector, ctx);
				}
			}

			token = parserutils_vector_peek(vector, *ctx);
			if (token == NULL || token->type != CSS_TOKEN_CHAR ||
				lwc_string_data(token->idata)[0] != ')')
				return CSS_INVALID;
			parserutils_vector_iterate(vector, ctx); /* consume ) */

			out->type = CSS_TIMING_STEPS;
			out->x1 = out->y1 = out->x2 = out->y2 = 0;
			return CSS_OK;
		}
	}

	return CSS_INVALID;
}

css_error css__parse_transition_timing_function(css_language *c,
		const parserutils_vector *vector, int32_t *ctx,
		css_style *result)
{
	int32_t orig_ctx = *ctx;
	css_error error;
	const css_token *token;
	enum flag_value flag_value;
	uint8_t count = 0;
	css_transition_timing_entry timings[CSS_TRANSITION_MAX_LIST];
	uint8_t i;

	token = parserutils_vector_peek(vector, *ctx);
	if (token == NULL)
		return CSS_INVALID;

	flag_value = get_css_flag_value(c, token);
	if (flag_value != FLAG_VALUE__NONE) {
		parserutils_vector_iterate(vector, ctx);
		return css_stylesheet_style_flag_value(result, flag_value,
				CSS_PROP_TRANSITION_TIMING_FUNCTION);
	}

	while (token != NULL) {
		if (count >= CSS_TRANSITION_MAX_LIST) {
			*ctx = orig_ctx;
			return CSS_INVALID;
		}

		error = css__parse_transition_timing_function_item(c, vector, ctx,
				&timings[count]);
		if (error != CSS_OK) {
			*ctx = orig_ctx;
			return error;
		}
		count++;

		consumeWhitespace(vector, ctx);
		token = parserutils_vector_peek(vector, *ctx);
		if (is_token_comma(token)) {
			parserutils_vector_iterate(vector, ctx);
			consumeWhitespace(vector, ctx);
			token = parserutils_vector_peek(vector, *ctx);
			if (token == NULL) {
				*ctx = orig_ctx;
				return CSS_INVALID;
			}
		} else {
			break;
		}
	}

	if (count == 0) {
		*ctx = orig_ctx;
		return CSS_INVALID;
	}

	error = css__stylesheet_style_appendOPV(result,
			CSS_PROP_TRANSITION_TIMING_FUNCTION, 0,
			CSS_TRANSITION_TIMING_FUNCTION_SET);
	if (error != CSS_OK) { *ctx = orig_ctx; return error; }

	error = css__stylesheet_style_append(result, (css_code_t)count);
	if (error != CSS_OK) { *ctx = orig_ctx; return error; }

	for (i = 0; i < count; i++) {
		error = css__stylesheet_style_append(result, (css_code_t)timings[i].type);
		if (error != CSS_OK) { *ctx = orig_ctx; return error; }
		error = css__stylesheet_style_append(result, (css_code_t)timings[i].x1);
		if (error != CSS_OK) { *ctx = orig_ctx; return error; }
		error = css__stylesheet_style_append(result, (css_code_t)timings[i].y1);
		if (error != CSS_OK) { *ctx = orig_ctx; return error; }
		error = css__stylesheet_style_append(result, (css_code_t)timings[i].x2);
		if (error != CSS_OK) { *ctx = orig_ctx; return error; }
		error = css__stylesheet_style_append(result, (css_code_t)timings[i].y2);
		if (error != CSS_OK) { *ctx = orig_ctx; return error; }
		error = css__stylesheet_style_append(result, (css_code_t)timings[i].step_count);
		if (error != CSS_OK) { *ctx = orig_ctx; return error; }
		error = css__stylesheet_style_append(result, (css_code_t)timings[i].step_pos);
		if (error != CSS_OK) { *ctx = orig_ctx; return error; }
	}

	return CSS_OK;
}
