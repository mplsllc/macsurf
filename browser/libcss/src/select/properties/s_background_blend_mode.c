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

css_error css__cascade_background_blend_mode(uint32_t opv, css_style *style,
		css_select_state *state)
{
	uint16_t value = CSS_BACKGROUND_BLEND_MODE_INHERIT;

	UNUSED(style);
	if (hasFlagValue(opv) == false)
		value = getValue(opv);

	if (css__outranks_existing(getOpcode(opv), isImportant(opv), state,
			getFlagValue(opv))) {
		state->computed->i.background_blend_mode = value;
	}
	return CSS_OK;
}

css_error css__set_background_blend_mode_from_hint(const css_hint *hint,
		css_computed_style *style)
{
	style->i.background_blend_mode = hint->status;
	return CSS_OK;
}

css_error css__initial_background_blend_mode(css_select_state *state)
{
	state->computed->i.background_blend_mode =
			CSS_BACKGROUND_BLEND_MODE_NORMAL;
	return CSS_OK;
}

css_error css__copy_background_blend_mode(const css_computed_style *from,
		css_computed_style *to)
{
	if (from != to)
		to->i.background_blend_mode = from->i.background_blend_mode;
	return CSS_OK;
}

css_error css__compose_background_blend_mode(const css_computed_style *parent,
		const css_computed_style *child, css_computed_style *result)
{
	int32_t value = child->i.background_blend_mode;
	if (value == CSS_BACKGROUND_BLEND_MODE_INHERIT)
		value = parent->i.background_blend_mode;
	result->i.background_blend_mode = value;
	return CSS_OK;
}

uint32_t destroy_background_blend_mode(void *bytecode)
{
	UNUSED(bytecode);
	return 0;
}
