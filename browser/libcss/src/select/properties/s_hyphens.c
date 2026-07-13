/*
 * This file is part of LibCSS
 * Licensed under the MIT License,
 *                http://www.opensource.org/licenses/mit-license.php
 * Copyright 2026 MacSurf (#251)
 */

#include "bytecode/bytecode.h"
#include "bytecode/opcodes.h"
#include "select/propset.h"
#include "select/propget.h"
#include "utils/utils.h"

#include "select/properties/properties.h"
#include "select/properties/helpers.h"

css_error css__cascade_hyphens(uint32_t opv, css_style *style,
		css_select_state *state)
{
	uint16_t value = CSS_HYPHENS_INHERIT;

	UNUSED(style);

	if (hasFlagValue(opv) == false) {
		switch (getValue(opv)) {
		case CSS_HYPHENS_NONE:
			value = CSS_HYPHENS_NONE;
			break;
		case CSS_HYPHENS_MANUAL:
			value = CSS_HYPHENS_MANUAL;
			break;
		case CSS_HYPHENS_AUTO:
			value = CSS_HYPHENS_AUTO;
			break;
		}
	}

	if (css__outranks_existing(getOpcode(opv), isImportant(opv), state,
			getFlagValue(opv))) {
		return set_hyphens(state->computed, (uint8_t)value);
	}

	return CSS_OK;
}

css_error css__set_hyphens_from_hint(const css_hint *hint,
		css_computed_style *style)
{
	return set_hyphens(style, hint->status);
}

css_error css__initial_hyphens(css_select_state *state)
{
	return set_hyphens(state->computed, CSS_HYPHENS_MANUAL);
}

css_error css__copy_hyphens(
		const css_computed_style *from,
		css_computed_style *to)
{
	if (from == to) {
		return CSS_OK;
	}

	return set_hyphens(to, get_hyphens(from));
}

css_error css__compose_hyphens(const css_computed_style *parent,
		const css_computed_style *child,
		css_computed_style *result)
{
	uint8_t type = get_hyphens(child);

	return css__copy_hyphens(
			type == CSS_HYPHENS_INHERIT ? parent : child,
			result);
}

uint32_t destroy_hyphens(void *bytecode)
{
	(void)bytecode;
	return 0;
}
