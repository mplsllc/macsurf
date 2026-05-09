/*
 * This file is part of LibCSS
 * Licensed under the MIT License,
 *		  http://www.opensource.org/licenses/mit-license.php
 * Copyright 2025 Gemini CLI
 */

#include "bytecode/bytecode.h"
#include "bytecode/opcodes.h"
#include "select/propset.h"
#include "select/propget.h"
#include "utils/utils.h"

#include "select/properties/properties.h"
#include "select/properties/helpers.h"

css_error css__cascade_border_radius(uint32_t opv, css_style *style,
		css_select_state *state)
{
	uint16_t value = CSS_BORDER_RADIUS_INHERIT;
	css_fixed length = 0;
	uint32_t unit = UNIT_PX;

	if (hasFlagValue(opv) == false) {
		switch (getValue(opv)) {
		case 0x0080: /* BORDER_RADIUS_SET */
			value = CSS_BORDER_RADIUS_SET;
			length = *((css_fixed *) style->bytecode);
			advance_bytecode(style, sizeof(css_fixed));
			unit = *((uint32_t *) style->bytecode);
			advance_bytecode(style, sizeof(uint32_t));
			break;
		}
	}

	if (css__outranks_existing(getOpcode(opv), isImportant(opv), state,
			getFlagValue(opv))) {
		return set_border_radius(state->computed, value, length, unit);
	}

	return CSS_OK;
}

css_error css__set_border_radius_from_hint(const css_hint *hint,
		css_computed_style *style)
{
	return set_border_radius(style, hint->status,
			hint->data.length.value, hint->data.length.unit);
}

css_error css__initial_border_radius(css_select_state *state)
{
	return set_border_radius(state->computed, CSS_BORDER_RADIUS_SET,
			0, CSS_UNIT_PX);
}

css_error css__copy_border_radius(
		const css_computed_style *from,
		css_computed_style *to)
{
	css_fixed length = 0;
	css_unit unit = CSS_UNIT_PX;
	uint8_t type = get_border_radius(from, &length, &unit);

	if (from == to) {
		return CSS_OK;
	}

	return set_border_radius(to, type, length, unit);
}

css_error css__compose_border_radius(const css_computed_style *parent,
		const css_computed_style *child,
		css_computed_style *result)
{
	css_fixed length = 0;
	css_unit unit = CSS_UNIT_PX;
	uint8_t type = get_border_radius(child, &length, &unit);

	return css__copy_border_radius(
			type == CSS_BORDER_RADIUS_INHERIT ? parent : child,
			result);
}

uint32_t destroy_border_radius(void *bytecode)
{
	return 0;
}
