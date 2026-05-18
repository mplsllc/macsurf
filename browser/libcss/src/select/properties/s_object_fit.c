/*
 * This file is part of LibCSS
 * Licensed under the MIT License,
 *                http://www.opensource.org/licenses/mit-license.php
 * Copyright 2026 MacSurf (fixes116)
 */

#include "bytecode/bytecode.h"
#include "bytecode/opcodes.h"
#include "select/propset.h"
#include "select/propget.h"
#include "utils/utils.h"

#include "select/properties/properties.h"
#include "select/properties/helpers.h"

css_error css__cascade_object_fit(uint32_t opv, css_style *style,
		css_select_state *state)
{
	uint16_t value = CSS_OBJECT_FIT_INHERIT;

	UNUSED(style);

	if (hasFlagValue(opv) == false) {
		switch (getValue(opv)) {
		case CSS_OBJECT_FIT_FILL:
			value = CSS_OBJECT_FIT_FILL;
			break;
		case CSS_OBJECT_FIT_CONTAIN:
			value = CSS_OBJECT_FIT_CONTAIN;
			break;
		case CSS_OBJECT_FIT_COVER:
			value = CSS_OBJECT_FIT_COVER;
			break;
		case CSS_OBJECT_FIT_NONE:
			value = CSS_OBJECT_FIT_NONE;
			break;
		case CSS_OBJECT_FIT_SCALE_DOWN:
			value = CSS_OBJECT_FIT_SCALE_DOWN;
			break;
		}
	}

	if (css__outranks_existing(getOpcode(opv), isImportant(opv), state,
			getFlagValue(opv))) {
		return set_object_fit(state->computed, (uint8_t)value);
	}

	return CSS_OK;
}

css_error css__set_object_fit_from_hint(const css_hint *hint,
		css_computed_style *style)
{
	return set_object_fit(style, hint->status);
}

css_error css__initial_object_fit(css_select_state *state)
{
	return set_object_fit(state->computed, CSS_OBJECT_FIT_FILL);
}

css_error css__copy_object_fit(
		const css_computed_style *from,
		css_computed_style *to)
{
	if (from == to) {
		return CSS_OK;
	}

	return set_object_fit(to, get_object_fit(from));
}

css_error css__compose_object_fit(const css_computed_style *parent,
		const css_computed_style *child,
		css_computed_style *result)
{
	uint8_t type = get_object_fit(child);

	return css__copy_object_fit(
			type == CSS_OBJECT_FIT_INHERIT ? parent : child,
			result);
}

uint32_t destroy_object_fit(void *bytecode)
{
	(void)bytecode;
	return 0;
}
