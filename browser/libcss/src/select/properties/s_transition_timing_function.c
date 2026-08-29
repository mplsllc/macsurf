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

css_error css__cascade_transition_timing_function(uint32_t opv, css_style *style,
		css_select_state *state)
{
	uint8_t count = 0;
	css_transition_timing_entry timings[CSS_TRANSITION_MAX_LIST];
	uint8_t i;
	if (hasFlagValue(opv) == false) {
		switch (getValue(opv)) {
		case CSS_TRANSITION_TIMING_FUNCTION_SET: {
			count = (uint8_t)*((css_code_t *)style->bytecode);
			advance_bytecode(style, sizeof(css_code_t));
			if (count > CSS_TRANSITION_MAX_LIST) count = CSS_TRANSITION_MAX_LIST;

			for (i = 0; i < count; i++) {
				timings[i].type = (enum css_transition_timing_type)*((css_code_t *)style->bytecode);
				advance_bytecode(style, sizeof(css_code_t));
				timings[i].x1 = (int32_t)*((css_code_t *)style->bytecode);
				advance_bytecode(style, sizeof(css_code_t));
				timings[i].y1 = (int32_t)*((css_code_t *)style->bytecode);
				advance_bytecode(style, sizeof(css_code_t));
				timings[i].x2 = (int32_t)*((css_code_t *)style->bytecode);
				advance_bytecode(style, sizeof(css_code_t));
				timings[i].y2 = (int32_t)*((css_code_t *)style->bytecode);
				advance_bytecode(style, sizeof(css_code_t));
				timings[i].step_count = (uint32_t)*((css_code_t *)style->bytecode);
				advance_bytecode(style, sizeof(css_code_t));
				timings[i].step_pos = (uint8_t)*((css_code_t *)style->bytecode);
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
				d->inherit_flags |= CSS_TRANS_INHERIT_TIMING;
			}
		} else if (hasFlagValue(opv) && getFlagValue(opv) == FLAG_VALUE_INITIAL) {
			if (state->computed->transition_data != NULL) {
				css_computed_transition_data *d = state->computed->transition_data;
				d->inherit_flags &= ~CSS_TRANS_INHERIT_TIMING;
				d->timing_count = 1;
				d->timings[0].type = CSS_TIMING_EASE;
				d->timings[0].x1 = (int32_t)(0.25 * 65536.0);
				d->timings[0].y1 = (int32_t)(0.1 * 65536.0);
				d->timings[0].x2 = (int32_t)(0.25 * 65536.0);
				d->timings[0].y2 = (int32_t)(1.0 * 65536.0);
				d->timings[0].step_count = 0;
				d->timings[0].step_pos = 0;
			}
		} else if (count > 0) {
			css_computed_transition_data *d = css__ensure_transition_data(state->computed);
			if (d != NULL) {
				d->inherit_flags &= ~CSS_TRANS_INHERIT_TIMING;
				d->timing_count = count;
				for (i = 0; i < count; i++) {
					d->timings[i] = timings[i];
				}
			}
		}
	}

	return CSS_OK;
}

css_error css__set_transition_timing_function_from_hint(const css_hint *hint,
		css_computed_style *style)
{
	(void)hint;
	(void)style;
	return CSS_OK;
}

css_error css__initial_transition_timing_function(css_select_state *state)
{
	(void)state;
	return CSS_OK;
}

css_error css__copy_transition_timing_function(
		const css_computed_style *from,
		css_computed_style *to)
{
	(void)from;
	(void)to;
	return CSS_OK;
}

css_error css__compose_transition_timing_function(const css_computed_style *parent,
		const css_computed_style *child,
		css_computed_style *result)
{
	(void)parent;
	(void)child;
	(void)result;
	return CSS_OK;
}

uint32_t destroy_transition_timing_function(void *bytecode)
{
	(void)bytecode;
	return 0;
}
