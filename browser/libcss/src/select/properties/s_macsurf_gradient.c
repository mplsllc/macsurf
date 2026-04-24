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

css_error css__cascade_macsurf_gradient(uint32_t opv, css_style *style,
		css_select_state *state)
{
	uint16_t value = CSS_MACSURF_GRADIENT_INHERIT;
	css_color c1 = 0, c2 = 0;

	if (hasFlagValue(opv) == false) {
		switch (getValue(opv)) {
		case 0x0000: /* NONE */
			value = CSS_MACSURF_GRADIENT_NONE;
			break;
		case 0x0080: /* SET */
			value = CSS_MACSURF_GRADIENT_SET;
			c1 = *((css_color *) style->bytecode);
			advance_bytecode(style, sizeof(css_color));
			c2 = *((css_color *) style->bytecode);
			advance_bytecode(style, sizeof(css_color));
			break;
		}
	}

	if (css__outranks_existing(getOpcode(opv), isImportant(opv), state,
			getFlagValue(opv))) {
		return set_macsurf_gradient(state->computed, value, c1); /* simplified storage */
	}

	return CSS_OK;
}

css_error css__set_macsurf_gradient_from_hint(const css_hint *hint,
		css_computed_style *style)
{
	return set_macsurf_gradient(style, hint->status, hint->data.color);
}

css_error css__initial_macsurf_gradient(css_select_state *state)
{
	return set_macsurf_gradient(state->computed, CSS_MACSURF_GRADIENT_NONE, 0);
}

css_error css__copy_macsurf_gradient(
		const css_computed_style *from,
		css_computed_style *to)
{
	int32_t color = 0;
	uint8_t type = get_macsurf_gradient(from, &color);

	if (from == to) {
		return CSS_OK;
	}

	return set_macsurf_gradient(to, type, (css_color)color);
}

css_error css__compose_macsurf_gradient(const css_computed_style *parent,
		const css_computed_style *child,
		css_computed_style *result)
{
	int32_t color = 0;
	uint8_t type = get_macsurf_gradient(child, &color);

	return css__copy_macsurf_gradient(
			type == CSS_MACSURF_GRADIENT_INHERIT ? parent : child,
			result);
}

uint32_t destroy_macsurf_gradient(void *bytecode)
{
	return 0;
}
