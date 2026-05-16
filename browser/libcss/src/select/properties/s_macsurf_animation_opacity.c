/*
 * This file is part of LibCSS
 * Licensed under the MIT License,
 *		  http://www.opensource.org/licenses/mit-license.php
 * Copyright 2026 MacSurf (fixes76)
 */

#include "bytecode/bytecode.h"
#include "bytecode/opcodes.h"
#include "select/propset.h"
#include "select/propget.h"
#include "utils/utils.h"

#include "select/properties/properties.h"
#include "select/properties/helpers.h"

css_error css__cascade_macsurf_animation_opacity(uint32_t opv,
		css_style *style, css_select_state *state)
{
	uint16_t value = CSS_MACSURF_ANIMATION_OPACITY_INHERIT;
	int32_t packed = 0;

	if (hasFlagValue(opv) == false) {
		switch (getValue(opv)) {
		case 0x0000: /* NONE */
			value = CSS_MACSURF_ANIMATION_OPACITY_NONE;
			break;
		case 0x0080: /* SET */
			value = CSS_MACSURF_ANIMATION_OPACITY_SET;
			packed = *((css_fixed *) style->bytecode);
			advance_bytecode(style, sizeof(css_fixed));
			break;
		}
	}

	if (css__outranks_existing(getOpcode(opv), isImportant(opv), state,
			getFlagValue(opv))) {
		return set_macsurf_animation_opacity(state->computed, value,
				packed);
	}

	return CSS_OK;
}

css_error css__set_macsurf_animation_opacity_from_hint(
		const css_hint *hint, css_computed_style *style)
{
	return set_macsurf_animation_opacity(style, hint->status,
			hint->data.integer);
}

css_error css__initial_macsurf_animation_opacity(css_select_state *state)
{
	return set_macsurf_animation_opacity(state->computed,
			CSS_MACSURF_ANIMATION_OPACITY_NONE, 0);
}

css_error css__copy_macsurf_animation_opacity(
		const css_computed_style *from,
		css_computed_style *to)
{
	int32_t integer = 0;
	uint8_t type = get_macsurf_animation_opacity(from, &integer);

	if (from == to) {
		return CSS_OK;
	}

	return set_macsurf_animation_opacity(to, type, integer);
}

css_error css__compose_macsurf_animation_opacity(
		const css_computed_style *parent,
		const css_computed_style *child,
		css_computed_style *result)
{
	int32_t integer = 0;
	uint8_t type = get_macsurf_animation_opacity(child, &integer);

	return css__copy_macsurf_animation_opacity(
			type == CSS_MACSURF_ANIMATION_OPACITY_INHERIT ?
				parent : child,
			result);
}

uint32_t destroy_macsurf_animation_opacity(void *bytecode)
{
	(void)bytecode;
	return 0;
}
