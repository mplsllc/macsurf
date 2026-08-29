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

static bool is_css_wide_keyword(css_language *c, const css_token *token)
{
	bool match;
	if (token == NULL || token->type != CSS_TOKEN_IDENT)
		return false;

	if (lwc_string_caseless_isequal(token->idata, c->strings[INHERIT], &match) == lwc_error_ok && match)
		return true;
	if (lwc_string_caseless_isequal(token->idata, c->strings[INITIAL], &match) == lwc_error_ok && match)
		return true;
	if (lwc_string_caseless_isequal(token->idata, c->strings[UNSET], &match) == lwc_error_ok && match)
		return true;
	if (lwc_string_caseless_isequal(token->idata, c->strings[REVERT], &match) == lwc_error_ok && match)
		return true;
	return false;
}

static uint32_t resolve_known_property_id(css_language *c, lwc_string *ident)
{
	bool match;

	/* The parser's propstring enum also contains shorthand and legacy names,
	 * so its index is not the selector engine's CSS_PROP_* opcode.  Keep the
	 * transition identity on the canonical opcode used by the cascade. */
#define TRANSITION_KNOWN(name, prop) \
	if (lwc_string_caseless_isequal(ident, c->strings[name], &match) == lwc_error_ok && match) return prop
	TRANSITION_KNOWN(OPACITY, CSS_PROP_OPACITY);
	TRANSITION_KNOWN(COLOR, CSS_PROP_COLOR);
	TRANSITION_KNOWN(MACSURF_TRANSFORM, CSS_PROP_MACSURF_TRANSFORM);
	TRANSITION_KNOWN(BACKGROUND_COLOR, CSS_PROP_BACKGROUND_COLOR);
	TRANSITION_KNOWN(MAX_WIDTH, CSS_PROP_MAX_WIDTH);
	TRANSITION_KNOWN(MAX_HEIGHT, CSS_PROP_MAX_HEIGHT);
	TRANSITION_KNOWN(WIDTH, CSS_PROP_WIDTH);
	TRANSITION_KNOWN(HEIGHT, CSS_PROP_HEIGHT);
#undef TRANSITION_KNOWN
	return (uint32_t)-1;
}

static inline bool is_token_comma(const css_token *t)
{
	return (t != NULL && t->type == CSS_TOKEN_CHAR && lwc_string_data(t->idata)[0] == ',');
}

css_error css__parse_transition_property(css_language *c,
		const parserutils_vector *vector, int32_t *ctx,
		css_style *result)
{
	int32_t orig_ctx = *ctx;
	css_error error;
	const css_token *token;
	enum flag_value flag_value;
	uint8_t count = 0;
	struct {
		uint8_t kind; /* 0=ALL, 1=NONE, 2=KNOWN, 3=CUSTOM */
		uint32_t prop_id;
		uint32_t snumber;
	} entries[CSS_TRANSITION_MAX_LIST];
	uint8_t i;

	token = parserutils_vector_peek(vector, *ctx);
	if (token == NULL)
		return CSS_INVALID;

	flag_value = get_css_flag_value(c, token);
	if (flag_value != FLAG_VALUE__NONE) {
		parserutils_vector_iterate(vector, ctx);
		return css_stylesheet_style_flag_value(result, flag_value,
				CSS_PROP_TRANSITION_PROPERTY);
	}

	while (token != NULL) {
		bool match;

		if (token->type != CSS_TOKEN_IDENT) {
			*ctx = orig_ctx;
			return CSS_INVALID;
		}

		/* Check for CSS-wide keywords in list - invalid */
		if (is_css_wide_keyword(c, token)) {
			*ctx = orig_ctx;
			return CSS_INVALID;
		}

		if (count >= CSS_TRANSITION_MAX_LIST) {
			*ctx = orig_ctx;
			return CSS_INVALID;
		}

		if (lwc_string_caseless_isequal(token->idata, c->strings[NONE], &match) == lwc_error_ok && match) {
			if (count > 0) {
				*ctx = orig_ctx;
				return CSS_INVALID;
			}
			entries[count].kind = CSS_TRANS_PROP_NONE;
			entries[count].prop_id = 0;
			entries[count].snumber = 0;
			count++;
			parserutils_vector_iterate(vector, ctx);
			token = parserutils_vector_peek(vector, *ctx);
			if (is_token_comma(token)) {
				*ctx = orig_ctx;
				return CSS_INVALID;
			}
			break;
		} else if (lwc_string_caseless_isequal(token->idata, c->strings[ALL], &match) == lwc_error_ok && match) {
			entries[count].kind = CSS_TRANS_PROP_ALL;
			entries[count].prop_id = 0;
			entries[count].snumber = 0;
			count++;
		} else {
			uint32_t prop_id = resolve_known_property_id(c, token->idata);
			if (prop_id != (uint32_t)-1) {
				entries[count].kind = CSS_TRANS_PROP_KNOWN;
				entries[count].prop_id = prop_id;
				entries[count].snumber = 0;
			} else {
				uint32_t snum = 0;
				error = css__stylesheet_string_add(c->sheet,
						lwc_string_ref(token->idata), &snum);
				if (error != CSS_OK) {
					*ctx = orig_ctx;
					return error;
				}
				entries[count].kind = CSS_TRANS_PROP_CUSTOM_IDENT;
				entries[count].prop_id = 0;
				entries[count].snumber = snum;
			}
			count++;
		}

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
			CSS_PROP_TRANSITION_PROPERTY, 0,
			CSS_TRANSITION_PROPERTY_SET);
	if (error != CSS_OK) { *ctx = orig_ctx; return error; }

	error = css__stylesheet_style_append(result, (css_code_t)count);
	if (error != CSS_OK) { *ctx = orig_ctx; return error; }

	for (i = 0; i < count; i++) {
		error = css__stylesheet_style_append(result, (css_code_t)entries[i].kind);
		if (error != CSS_OK) { *ctx = orig_ctx; return error; }
		error = css__stylesheet_style_append(result, (css_code_t)entries[i].prop_id);
		if (error != CSS_OK) { *ctx = orig_ctx; return error; }
		error = css__stylesheet_style_append(result, (css_code_t)entries[i].snumber);
		if (error != CSS_OK) { *ctx = orig_ctx; return error; }
	}

	return CSS_OK;
}
