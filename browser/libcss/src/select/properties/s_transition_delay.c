/*
 * This file is part of LibCSS.
 * Licensed under the MIT License.
 */

#include <stdlib.h>
#include <string.h>

#include "bytecode/bytecode.h"
#include "bytecode/opcodes.h"
#include "select/propset.h"
#include "select/propget.h"
#include "utils/utils.h"

#include "select/properties/properties.h"
#include "select/properties/helpers.h"
#include "select/properties/s_transition_helpers.h"

css_error css__cascade_transition_delay(uint32_t opv, css_style *style,
		css_select_state *state)
{
	uint8_t count = 0;
	css_fixed delays[CSS_TRANSITION_MAX_LIST];
	uint8_t i;
	if (hasFlagValue(opv) == false) {
		switch (getValue(opv)) {
		case CSS_TRANSITION_DELAY_SET: {
			count = (uint8_t)*((css_code_t *)style->bytecode);
			advance_bytecode(style, sizeof(css_code_t));
			if (count > CSS_TRANSITION_MAX_LIST) count = CSS_TRANSITION_MAX_LIST;

			for (i = 0; i < count; i++) {
				delays[i] = (css_fixed)*((css_code_t *)style->bytecode);
				advance_bytecode(style, sizeof(css_code_t));
			}
			break;
		}
		}
	}

	if (css__outranks_existing(getOpcode(opv), isImportant(opv), state,
			getFlagValue(opv))) {
		if (hasFlagValue(opv) && getFlagValue(opv) == FLAG_VALUE_INHERIT) {
			css_computed_transition_data *d = css__ensure_transition_data(state->computed);
			if (d != NULL) {
				d->inherit_flags |= CSS_TRANS_INHERIT_DELAY;
			}
		} else if (hasFlagValue(opv) &&
				(getFlagValue(opv) == FLAG_VALUE_INITIAL ||
				 getFlagValue(opv) == FLAG_VALUE_UNSET)) {
			if (state->computed->transition_data != NULL) {
				css_computed_transition_data *d = state->computed->transition_data;
				d->inherit_flags &= ~CSS_TRANS_INHERIT_DELAY;
				d->delay_count = 1;
				d->delays[0] = 0;
			}
		} else if (count > 0) {
			css_computed_transition_data *d = css__ensure_transition_data(state->computed);
			if (d != NULL) {
				d->inherit_flags &= ~CSS_TRANS_INHERIT_DELAY;
				d->delay_count = count;
				for (i = 0; i < count; i++) {
					d->delays[i] = delays[i];
				}
			}
		}
	}

	return CSS_OK;
}

css_error css__set_transition_delay_from_hint(const css_hint *hint,
		css_computed_style *style)
{
	(void)hint;
	(void)style;
	return CSS_OK;
}

css_error css__initial_transition_delay(css_select_state *state)
{
	(void)state;
	return CSS_OK;
}

css_error css__copy_transition_delay(
		const css_computed_style *from,
		css_computed_style *to)
{
	(void)from;
	(void)to;
	return CSS_OK;
}

css_error css__compose_transition_delay(const css_computed_style *parent,
		const css_computed_style *child,
		css_computed_style *result)
{
	(void)parent;
	(void)child;
	(void)result;
	return CSS_OK;
}

uint32_t destroy_transition_delay(void *bytecode)
{
	(void)bytecode;
	return 0;
}
