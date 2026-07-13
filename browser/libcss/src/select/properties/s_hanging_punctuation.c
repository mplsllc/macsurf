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

css_error css__cascade_hanging_punctuation(uint32_t opv, css_style *style,
		css_select_state *state)
{
	uint16_t value = CSS_HANGING_PUNCTUATION_INHERIT;

	UNUSED(style);

	if (hasFlagValue(opv) == false) {
		switch (getValue(opv)) {
		case CSS_HANGING_PUNCTUATION_NONE:
			value = CSS_HANGING_PUNCTUATION_NONE;
			break;
		case CSS_HANGING_PUNCTUATION_FIRST:
			value = CSS_HANGING_PUNCTUATION_FIRST;
			break;
		case CSS_HANGING_PUNCTUATION_LAST:
			value = CSS_HANGING_PUNCTUATION_LAST;
			break;
		case CSS_HANGING_PUNCTUATION_FORCE_END:
			value = CSS_HANGING_PUNCTUATION_FORCE_END;
			break;
		case CSS_HANGING_PUNCTUATION_ALLOW_END:
			value = CSS_HANGING_PUNCTUATION_ALLOW_END;
			break;
		}
	}

	if (css__outranks_existing(getOpcode(opv), isImportant(opv), state,
			getFlagValue(opv))) {
		return set_hanging_punctuation(state->computed, (uint8_t)value);
	}

	return CSS_OK;
}

css_error css__set_hanging_punctuation_from_hint(const css_hint *hint,
		css_computed_style *style)
{
	return set_hanging_punctuation(style, hint->status);
}

css_error css__initial_hanging_punctuation(css_select_state *state)
{
	return set_hanging_punctuation(state->computed, CSS_HANGING_PUNCTUATION_NONE);
}

css_error css__copy_hanging_punctuation(
		const css_computed_style *from,
		css_computed_style *to)
{
	if (from == to) {
		return CSS_OK;
	}

	return set_hanging_punctuation(to, get_hanging_punctuation(from));
}

css_error css__compose_hanging_punctuation(const css_computed_style *parent,
		const css_computed_style *child,
		css_computed_style *result)
{
	uint8_t type = get_hanging_punctuation(child);

	return css__copy_hanging_punctuation(
			type == CSS_HANGING_PUNCTUATION_INHERIT ? parent : child,
			result);
}

uint32_t destroy_hanging_punctuation(void *bytecode)
{
	(void)bytecode;
	return 0;
}
