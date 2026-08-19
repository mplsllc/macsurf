/*
 * This file is part of LibCSS.
 * Licensed under the MIT License.
 */

#include "bytecode/bytecode.h"
#include "bytecode/opcodes.h"
#include "select/propset.h"
#include "select/propget.h"
#include "utils/utils.h"

#include "select/properties/properties.h"
#include "select/properties/helpers.h"

css_error css__cascade_background_origin(uint32_t opv, css_style *style,
		css_select_state *state)
{
	uint16_t value = CSS_BACKGROUND_ORIGIN_INHERIT;

	UNUSED(style);

	if (hasFlagValue(opv) == false) {
		switch (getValue(opv)) {
		case CSS_BACKGROUND_ORIGIN_PADDING_BOX:
			value = CSS_BACKGROUND_ORIGIN_PADDING_BOX;
			break;
		case CSS_BACKGROUND_ORIGIN_BORDER_BOX:
			value = CSS_BACKGROUND_ORIGIN_BORDER_BOX;
			break;
		case CSS_BACKGROUND_ORIGIN_CONTENT_BOX:
			value = CSS_BACKGROUND_ORIGIN_CONTENT_BOX;
			break;
		}
	}

	if (css__outranks_existing(getOpcode(opv), isImportant(opv), state,
			getFlagValue(opv))) {
		state->computed->i.background_origin = value;
	}

	return CSS_OK;
}

css_error css__set_background_origin_from_hint(const css_hint *hint,
		css_computed_style *style)
{
	style->i.background_origin = hint->status;
	return CSS_OK;
}

css_error css__initial_background_origin(css_select_state *state)
{
	state->computed->i.background_origin = CSS_BACKGROUND_ORIGIN_PADDING_BOX;
	return CSS_OK;
}

css_error css__copy_background_origin(const css_computed_style *from,
		css_computed_style *to)
{
	if (from != to)
		to->i.background_origin = from->i.background_origin;
	return CSS_OK;
}

css_error css__compose_background_origin(const css_computed_style *parent,
		const css_computed_style *child, css_computed_style *result)
{
	int32_t value = child->i.background_origin;

	if (value == CSS_BACKGROUND_ORIGIN_INHERIT)
		value = parent->i.background_origin;
	result->i.background_origin = value;

	return CSS_OK;
}

uint32_t destroy_background_origin(void *bytecode)
{
	UNUSED(bytecode);
	return 0;
}
