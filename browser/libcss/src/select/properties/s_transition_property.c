/*
 * This file is part of LibCSS.
 * Licensed under the MIT License.
 */

#include <assert.h>
#include <string.h>

#include <libcss/computed.h>
#include "bytecode/bytecode.h"
#include "bytecode/opcodes.h"
#include "css_internal_stylesheet.h"
#include "select/properties/properties.h"
#include "select/properties/helpers.h"
#include "select/properties/s_transition_helpers.h"

css_error css__cascade_transition_property(uint32_t opv, css_style *style,
		css_select_state *state)
{
	uint8_t count = 0;
	css_transition_prop_entry props[CSS_TRANSITION_MAX_LIST];
	uint8_t i;
	memset(props, 0, sizeof(props));

	if (hasFlagValue(opv) == false) {
		switch (getValue(opv)) {
		case CSS_TRANSITION_PROPERTY_SET: {
			count = (uint8_t)*((css_code_t *)style->bytecode);
			advance_bytecode(style, sizeof(css_code_t));
			if (count > CSS_TRANSITION_MAX_LIST) count = CSS_TRANSITION_MAX_LIST;

			for (i = 0; i < count; i++) {
				uint32_t snum;
				props[i].kind = (enum css_transition_prop_kind)*((css_code_t *)style->bytecode);
				advance_bytecode(style, sizeof(css_code_t));
				props[i].prop_id = (uint32_t)*((css_code_t *)style->bytecode);
				advance_bytecode(style, sizeof(css_code_t));
				snum = (uint32_t)*((css_code_t *)style->bytecode);
				advance_bytecode(style, sizeof(css_code_t));

				if (props[i].kind == CSS_TRANS_PROP_CUSTOM_IDENT && style->sheet != NULL) {
					lwc_string *str = NULL;
					if (css__stylesheet_string_get(style->sheet, snum, &str) == CSS_OK && str != NULL) {
						props[i].custom_name = lwc_string_ref(str);
					}
				}
			}
			break;
		}
		}
	}

	if (css__outranks_existing(getOpcode(opv), isImportant(opv), state,
			getFlagValue(opv))) {
		if (hasFlagValue(opv) && getFlagValue(opv) == FLAG_VALUE_INHERIT) {
			/* Mark that property is explicitly inherited from parent */
			css_computed_transition_data *d = css__ensure_transition_data(state->computed);
			if (d != NULL) {
				d->inherit_flags |= CSS_TRANS_INHERIT_PROP;
			}
		} else if (hasFlagValue(opv) &&
				(getFlagValue(opv) == FLAG_VALUE_INITIAL ||
				 getFlagValue(opv) == FLAG_VALUE_UNSET)) {
			if (state->computed->transition_data != NULL) {
				css_computed_transition_data *d = state->computed->transition_data;
				d->inherit_flags &= ~CSS_TRANS_INHERIT_PROP;
				for (i = 0; i < d->prop_count; i++) {
					if (d->props[i].kind == CSS_TRANS_PROP_CUSTOM_IDENT && d->props[i].custom_name != NULL) {
						lwc_string_unref(d->props[i].custom_name);
						d->props[i].custom_name = NULL;
					}
				}
				d->prop_count = 1;
				d->props[0].kind = CSS_TRANS_PROP_ALL;
				d->props[0].prop_id = 0;
				d->props[0].custom_name = NULL;
			}
		} else {
			css_computed_transition_data *d = css__ensure_transition_data(state->computed);
			if (d != NULL) {
				d->inherit_flags &= ~CSS_TRANS_INHERIT_PROP;
				for (i = 0; i < d->prop_count; i++) {
					if (d->props[i].kind == CSS_TRANS_PROP_CUSTOM_IDENT && d->props[i].custom_name != NULL) {
						lwc_string_unref(d->props[i].custom_name);
						d->props[i].custom_name = NULL;
					}
				}
				d->prop_count = count;
				for (i = 0; i < count; i++) {
					d->props[i] = props[i];
					/* transfer ref ownership to d->props[i] */
					props[i].custom_name = NULL;
				}
			}
		}
	}

	/* Cleanup unreferenced strings if we did not store them */
	for (i = 0; i < count; i++) {
		if (props[i].custom_name != NULL) {
			lwc_string_unref(props[i].custom_name);
			props[i].custom_name = NULL;
		}
	}

	return CSS_OK;
}

css_error css__set_transition_property_from_hint(const css_hint *hint,
		css_computed_style *style)
{
	(void)hint;
	(void)style;
	return CSS_OK;
}

css_error css__initial_transition_property(css_select_state *state)
{
	(void)state;
	return CSS_OK;
}

css_error css__copy_transition_property(const css_computed_style *from,
		css_computed_style *to)
{
	if (from->transition_data == NULL) return CSS_OK;
	{
		css_computed_transition_data *d = css__ensure_transition_data(to);
		uint8_t i;
		if (d == NULL) return CSS_NOMEM;
		d->prop_count = from->transition_data->prop_count;
		for (i = 0; i < d->prop_count; i++) {
			d->props[i] = from->transition_data->props[i];
			if (d->props[i].kind == CSS_TRANS_PROP_CUSTOM_IDENT && d->props[i].custom_name != NULL) {
				lwc_string_ref(d->props[i].custom_name);
			}
		}
	}
	return CSS_OK;
}

css_error css__compose_transition_property(const css_computed_style *parent,
		const css_computed_style *child,
		css_computed_style *result)
{
	bool inherit = false;
	if (child->transition_data != NULL && (child->transition_data->inherit_flags & CSS_TRANS_INHERIT_PROP)) {
		inherit = true;
	}

	if (inherit && parent->transition_data != NULL) {
		css_computed_transition_data *d = css__ensure_transition_data(result);
		uint8_t i;
		if (d == NULL) return CSS_NOMEM;
		d->prop_count = parent->transition_data->prop_count;
		for (i = 0; i < d->prop_count; i++) {
			d->props[i] = parent->transition_data->props[i];
			if (d->props[i].kind == CSS_TRANS_PROP_CUSTOM_IDENT && d->props[i].custom_name != NULL) {
				lwc_string_ref(d->props[i].custom_name);
			}
		}
	} else if (child->transition_data != NULL) {
		css_computed_transition_data *d = css__ensure_transition_data(result);
		uint8_t i;
		if (d == NULL) return CSS_NOMEM;
		d->prop_count = child->transition_data->prop_count;
		for (i = 0; i < d->prop_count; i++) {
			d->props[i] = child->transition_data->props[i];
			if (d->props[i].kind == CSS_TRANS_PROP_CUSTOM_IDENT && d->props[i].custom_name != NULL) {
				lwc_string_ref(d->props[i].custom_name);
			}
		}
	}
	return CSS_OK;
}

uint32_t destroy_transition_property(void *bytecode)
{
	(void)bytecode;
	return 0;
}
