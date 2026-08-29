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

static inline bool is_token_important_start(const css_token *t)
{
	return (t != NULL && t->type == CSS_TOKEN_CHAR &&
		lwc_string_data(t->idata)[0] == '!');
}

css_error css__parse_transition(css_language *c,
		const parserutils_vector *vector, int32_t *ctx,
		css_style *result)
{
	int32_t orig_ctx = *ctx;
	css_error error;
	const css_token *token;
	enum flag_value flag_value;
	css_style *prop_style = NULL;
	css_style *dur_style = NULL;
	css_style *timing_style = NULL;
	css_style *delay_style = NULL;

	token = parserutils_vector_peek(vector, *ctx);
	if (token == NULL)
		return CSS_INVALID;

	flag_value = get_css_flag_value(c, token);
	if (flag_value != FLAG_VALUE__NONE) {
		parserutils_vector_iterate(vector, ctx);

		error = css_stylesheet_style_flag_value(result, flag_value,
				CSS_PROP_TRANSITION_PROPERTY);
		if (error != CSS_OK) return error;

		error = css_stylesheet_style_flag_value(result, flag_value,
				CSS_PROP_TRANSITION_DURATION);
		if (error != CSS_OK) return error;

		error = css_stylesheet_style_flag_value(result, flag_value,
				CSS_PROP_TRANSITION_TIMING_FUNCTION);
		if (error != CSS_OK) return error;

		error = css_stylesheet_style_flag_value(result, flag_value,
				CSS_PROP_TRANSITION_DELAY);
		return error;
	}

	error = css__stylesheet_style_create(c->sheet, &prop_style);
	if (error != CSS_OK) return error;

	error = css__stylesheet_style_create(c->sheet, &dur_style);
	if (error != CSS_OK) goto cleanup;

	error = css__stylesheet_style_create(c->sheet, &timing_style);
	if (error != CSS_OK) goto cleanup;

	error = css__stylesheet_style_create(c->sheet, &delay_style);
	if (error != CSS_OK) goto cleanup;

	/* Parse comma-separated transition shorthand list items */
	{
		uint8_t count = 0;
		struct {
			uint8_t prop_kind;
			uint32_t prop_id;
			uint32_t snumber;
			css_fixed duration;
			css_transition_timing_entry timing;
			css_fixed delay;
		} items[CSS_TRANSITION_MAX_LIST];
		uint8_t i;

		while (token != NULL) {
			bool got_prop = false;
			bool got_timing = false;
			int time_count = 0;
			css_fixed times[2] = { 0, 0 };

			if (count >= CSS_TRANSITION_MAX_LIST) {
				error = CSS_INVALID;
				goto cleanup;
			}

			/* Defaults for this transition item */
			items[count].prop_kind = CSS_TRANS_PROP_ALL;
			items[count].prop_id = 0;
			items[count].snumber = 0;
			items[count].duration = 0; /* 0s */
			items[count].timing.type = CSS_TIMING_EASE;
			items[count].timing.x1 = (int32_t)(0.25 * 65536.0);
			items[count].timing.y1 = (int32_t)(0.1 * 65536.0);
			items[count].timing.x2 = (int32_t)(0.25 * 65536.0);
			items[count].timing.y2 = (int32_t)(1.0 * 65536.0);
			items[count].timing.step_count = 0;
			items[count].timing.step_pos = 0;
			items[count].delay = 0;    /* 0s */

			while (token != NULL && !is_token_comma(token) &&
					!is_token_important_start(token)) {
				bool handled = false;

				/* 1. Try time (duration or delay) */
				if (token->type == CSS_TOKEN_DIMENSION ||
					token->type == CSS_TOKEN_NUMBER) {
					if (time_count < 2) {
						size_t consumed = 0;
						css_fixed tnum = css__number_from_lwc_string(token->idata, false, &consumed);
						const char *tdata = lwc_string_data(token->idata);
						size_t tlen = lwc_string_length(token->idata);

						if (token->type == CSS_TOKEN_NUMBER && tnum == 0) {
							times[time_count++] = 0;
							parserutils_vector_iterate(vector, ctx);
							handled = true;
						} else if (token->type == CSS_TOKEN_DIMENSION && consumed < tlen) {
							if (tlen - consumed == 1 && (tdata[consumed] == 's' || tdata[consumed] == 'S')) {
								times[time_count++] = tnum;
								parserutils_vector_iterate(vector, ctx);
								handled = true;
							} else if (tlen - consumed == 2 &&
								(tdata[consumed] == 'm' || tdata[consumed] == 'M') &&
								(tdata[consumed+1] == 's' || tdata[consumed+1] == 'S')) {
								times[time_count++] = (css_fixed)(((int64_t)tnum) / 1000);
								parserutils_vector_iterate(vector, ctx);
								handled = true;
							}
						}
					}
				}

				/* 2. Try timing-function */
				if (!handled && !got_timing) {
					int32_t test_ctx = *ctx;
					css_style *temp_timing = NULL;
					if (css__stylesheet_style_create(c->sheet, &temp_timing) == CSS_OK) {
						if (css__parse_transition_timing_function(c, vector, &test_ctx, temp_timing) == CSS_OK) {
							css_code_t *bc = (css_code_t *)temp_timing->bytecode;
							/* OPV (1), count (1), type, x1, y1, x2, y2, step_count, step_pos */
							items[count].timing.type = (enum css_transition_timing_type)bc[2];
							items[count].timing.x1 = (int32_t)bc[3];
							items[count].timing.y1 = (int32_t)bc[4];
							items[count].timing.x2 = (int32_t)bc[5];
							items[count].timing.y2 = (int32_t)bc[6];
							items[count].timing.step_count = (uint32_t)bc[7];
							items[count].timing.step_pos = (uint8_t)bc[8];
							got_timing = true;
							handled = true;
							*ctx = test_ctx;
						}
						css__stylesheet_style_destroy(temp_timing);
					}
				}

				/* 3. Try property */
				if (!handled && !got_prop && token->type == CSS_TOKEN_IDENT) {
					int32_t test_ctx = *ctx;
					css_style *temp_prop = NULL;
					if (css__stylesheet_style_create(c->sheet, &temp_prop) == CSS_OK) {
						if (css__parse_transition_property(c, vector, &test_ctx, temp_prop) == CSS_OK) {
							css_code_t *bc = (css_code_t *)temp_prop->bytecode;
							/* OPV (1), count (1), kind (1), prop_id (1), snumber (1) */
							items[count].prop_kind = (uint8_t)bc[2];
							items[count].prop_id = (uint32_t)bc[3];
							items[count].snumber = (uint32_t)bc[4];
							got_prop = true;
							handled = true;
							*ctx = test_ctx;
						}
						css__stylesheet_style_destroy(temp_prop);
					}
				}

				if (!handled) {
					error = CSS_INVALID;
					goto cleanup;
				}

				consumeWhitespace(vector, ctx);
				token = parserutils_vector_peek(vector, *ctx);
			}

			if (time_count >= 1) {
				if (times[0] < 0) {
					error = CSS_INVALID;
					goto cleanup;
				}
				items[count].duration = times[0];
			}
			if (time_count == 2) {
				items[count].delay = times[1];
			}

			count++;

			if (is_token_comma(token)) {
				parserutils_vector_iterate(vector, ctx);
				consumeWhitespace(vector, ctx);
				token = parserutils_vector_peek(vector, *ctx);
				if (token == NULL) {
					error = CSS_INVALID;
					goto cleanup;
				}
			} else {
				break;
			}
		}

		if (count == 0) {
			error = CSS_INVALID;
			goto cleanup;
		}

		/* Emit property bytecode */
		css__stylesheet_style_appendOPV(prop_style, CSS_PROP_TRANSITION_PROPERTY, 0, CSS_TRANSITION_PROPERTY_SET);
		css__stylesheet_style_append(prop_style, (css_code_t)count);
		for (i = 0; i < count; i++) {
			css__stylesheet_style_append(prop_style, (css_code_t)items[i].prop_kind);
			css__stylesheet_style_append(prop_style, (css_code_t)items[i].prop_id);
			css__stylesheet_style_append(prop_style, (css_code_t)items[i].snumber);
		}

		/* Emit duration bytecode */
		css__stylesheet_style_appendOPV(dur_style, CSS_PROP_TRANSITION_DURATION, 0, CSS_TRANSITION_DURATION_SET);
		css__stylesheet_style_append(dur_style, (css_code_t)count);
		for (i = 0; i < count; i++) {
			css__stylesheet_style_append(dur_style, (css_code_t)items[i].duration);
		}

		/* Emit timing bytecode */
		css__stylesheet_style_appendOPV(timing_style, CSS_PROP_TRANSITION_TIMING_FUNCTION, 0, CSS_TRANSITION_TIMING_FUNCTION_SET);
		css__stylesheet_style_append(timing_style, (css_code_t)count);
		for (i = 0; i < count; i++) {
			css__stylesheet_style_append(timing_style, (css_code_t)items[i].timing.type);
			css__stylesheet_style_append(timing_style, (css_code_t)items[i].timing.x1);
			css__stylesheet_style_append(timing_style, (css_code_t)items[i].timing.y1);
			css__stylesheet_style_append(timing_style, (css_code_t)items[i].timing.x2);
			css__stylesheet_style_append(timing_style, (css_code_t)items[i].timing.y2);
			css__stylesheet_style_append(timing_style, (css_code_t)items[i].timing.step_count);
			css__stylesheet_style_append(timing_style, (css_code_t)items[i].timing.step_pos);
		}

		/* Emit delay bytecode */
		css__stylesheet_style_appendOPV(delay_style, CSS_PROP_TRANSITION_DELAY, 0, CSS_TRANSITION_DELAY_SET);
		css__stylesheet_style_append(delay_style, (css_code_t)count);
		for (i = 0; i < count; i++) {
			css__stylesheet_style_append(delay_style, (css_code_t)items[i].delay);
		}

		/* Merge all 4 longhands into result style */
		css__stylesheet_merge_style(result, prop_style);
		css__stylesheet_merge_style(result, dur_style);
		css__stylesheet_merge_style(result, timing_style);
		css__stylesheet_merge_style(result, delay_style);

		css__stylesheet_style_destroy(prop_style);
		css__stylesheet_style_destroy(dur_style);
		css__stylesheet_style_destroy(timing_style);
		css__stylesheet_style_destroy(delay_style);
		return CSS_OK;
	}

cleanup:
	if (prop_style) css__stylesheet_style_destroy(prop_style);
	if (dur_style) css__stylesheet_style_destroy(dur_style);
	if (timing_style) css__stylesheet_style_destroy(timing_style);
	if (delay_style) css__stylesheet_style_destroy(delay_style);
	*ctx = orig_ctx;
	return error;
}
