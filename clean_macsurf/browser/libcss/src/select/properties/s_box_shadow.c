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

css_error css__cascade_box_shadow(uint32_t opv, css_style *style,
		css_select_state *state)
{
	uint16_t value = CSS_BOX_SHADOW_INHERIT;
	css_fixed h = 0, v = 0, blur = 0, spread = 0, inset = 0;
	css_color color = 0;

	if (hasFlagValue(opv) == false) {
		switch (getValue(opv)) {
		case 0x0000: /* NONE */
			value = CSS_BOX_SHADOW_NONE;
			break;
		case 0x0080: /* SET */
			value = CSS_BOX_SHADOW_SET;
			h = *((css_fixed *) style->bytecode);
			advance_bytecode(style, sizeof(css_fixed));
			v = *((css_fixed *) style->bytecode);
			advance_bytecode(style, sizeof(css_fixed));
			blur = *((css_fixed *) style->bytecode);
			advance_bytecode(style, sizeof(css_fixed));
			spread = *((css_fixed *) style->bytecode);
			advance_bytecode(style, sizeof(css_fixed));
			inset = *((css_fixed *) style->bytecode);
			advance_bytecode(style, sizeof(css_fixed));
			color = *((css_color *) style->bytecode);
			advance_bytecode(style, sizeof(css_color));
			break;
		}
	}

	if (css__outranks_existing(getOpcode(opv), isImportant(opv), state,
			getFlagValue(opv))) {
		return set_box_shadow(state->computed, value, (int32_t)h); /* simplified storage */
	}

	return CSS_OK;
}

css_error css__set_box_shadow_from_hint(const css_hint *hint,
		css_computed_style *style)
{
	return set_box_shadow(style, hint->status, hint->data.integer);
}

css_error css__initial_box_shadow(css_select_state *state)
{
	return set_box_shadow(state->computed, CSS_BOX_SHADOW_NONE, 0);
}

css_error css__copy_box_shadow(
		const css_computed_style *from,
		css_computed_style *to)
{
	int32_t integer = 0;
	uint8_t type = get_box_shadow(from, &integer);

	if (from == to) {
		return CSS_OK;
	}

	return set_box_shadow(to, type, integer);
}

css_error css__compose_box_shadow(const css_computed_style *parent,
		const css_computed_style *child,
		css_computed_style *result)
{
	int32_t integer = 0;
	uint8_t type = get_box_shadow(child, &integer);

	return css__copy_box_shadow(
			type == CSS_BOX_SHADOW_INHERIT ? parent : child,
			result);
}

uint32_t destroy_box_shadow(void *bytecode)
{
	return 0;
}
