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

static css_error parse_single_time(css_language *c, const css_token *token, css_fixed *out_val)
{
	size_t consumed = 0;
	css_fixed num;
	const char *data;
	size_t len;

	if (token == NULL) return CSS_INVALID;

	if (token->type == CSS_TOKEN_NUMBER) {
		num = css__number_from_lwc_string(token->idata, false, &consumed);
		if (num == 0) {
			*out_val = 0;
			return CSS_OK;
		}
		return CSS_INVALID;
	}

	if (token->type != CSS_TOKEN_DIMENSION) {
		return CSS_INVALID;
	}

	data = lwc_string_data(token->idata);
	len = lwc_string_length(token->idata);

	num = css__number_from_lwc_string(token->idata, false, &consumed);
	if (consumed >= len) return CSS_INVALID;

	if (len - consumed == 1 && (data[consumed] == 's' || data[consumed] == 'S')) {
		*out_val = num; /* in seconds (Q22.10) */
		return CSS_OK;
	} else if (len - consumed == 2 &&
		(data[consumed] == 'm' || data[consumed] == 'M') &&
		(data[consumed+1] == 's' || data[consumed+1] == 'S')) {
		*out_val = (css_fixed)(((int64_t)num) / 1000);
		if (*out_val == 0 && num > 0) {
			*out_val = 1;
		}
		return CSS_OK;
	}

	return CSS_INVALID;
}

css_error css__parse_transition_duration(css_language *c,
		const parserutils_vector *vector, int32_t *ctx,
		css_style *result)
{
	int32_t orig_ctx = *ctx;
	css_error error;
	const css_token *token;
	enum flag_value flag_value;
	uint8_t count = 0;
	css_fixed durations[CSS_TRANSITION_MAX_LIST];
	uint8_t i;

	token = parserutils_vector_peek(vector, *ctx);
	if (token == NULL)
		return CSS_INVALID;

	flag_value = get_css_flag_value(c, token);
	if (flag_value != FLAG_VALUE__NONE) {
		parserutils_vector_iterate(vector, ctx);
		return css_stylesheet_style_flag_value(result, flag_value,
				CSS_PROP_TRANSITION_DURATION);
	}

	while (token != NULL) {
		css_fixed dur = 0;

		if (count >= CSS_TRANSITION_MAX_LIST) {
			*ctx = orig_ctx;
			return CSS_INVALID;
		}

		error = parse_single_time(c, token, &dur);
		if (error != CSS_OK) {
			*ctx = orig_ctx;
			return error;
		}

		/* CSS transition-duration must be non-negative */
		if (dur < 0) {
			*ctx = orig_ctx;
			return CSS_INVALID;
		}

		durations[count++] = dur;
		parserutils_vector_iterate(vector, ctx);
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
			CSS_PROP_TRANSITION_DURATION, 0,
			CSS_TRANSITION_DURATION_SET);
	if (error != CSS_OK) { *ctx = orig_ctx; return error; }

	error = css__stylesheet_style_append(result, (css_code_t)count);
	if (error != CSS_OK) { *ctx = orig_ctx; return error; }

	for (i = 0; i < count; i++) {
		error = css__stylesheet_style_append(result, (css_code_t)durations[i]);
		if (error != CSS_OK) { *ctx = orig_ctx; return error; }
	}

	return CSS_OK;
}
