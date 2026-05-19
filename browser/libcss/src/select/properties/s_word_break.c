/*
 * This file is part of LibCSS
 * Licensed under the MIT License,
 *                http://www.opensource.org/licenses/mit-license.php
 * Copyright 2026 MacSurf (fixes136a)
 */

#include "bytecode/bytecode.h"
#include "bytecode/opcodes.h"
#include "select/propset.h"
#include "select/propget.h"
#include "utils/utils.h"

#include "select/properties/properties.h"
#include "select/properties/helpers.h"

css_error css__cascade_word_break(uint32_t opv, css_style *style,
		css_select_state *state)
{
	uint16_t value = CSS_WORD_BREAK_INHERIT;

	UNUSED(style);

	if (hasFlagValue(opv) == false) {
		switch (getValue(opv)) {
		case CSS_WORD_BREAK_NORMAL:
			value = CSS_WORD_BREAK_NORMAL;
			break;
		case CSS_WORD_BREAK_BREAK_ALL:
			value = CSS_WORD_BREAK_BREAK_ALL;
			break;
		case CSS_WORD_BREAK_KEEP_ALL:
			value = CSS_WORD_BREAK_KEEP_ALL;
			break;
		}
	}

	if (css__outranks_existing(getOpcode(opv), isImportant(opv), state,
			getFlagValue(opv))) {
		return set_word_break(state->computed, (uint8_t)value);
	}

	return CSS_OK;
}

css_error css__set_word_break_from_hint(const css_hint *hint,
		css_computed_style *style)
{
	return set_word_break(style, hint->status);
}

css_error css__initial_word_break(css_select_state *state)
{
	return set_word_break(state->computed, CSS_WORD_BREAK_NORMAL);
}

css_error css__copy_word_break(
		const css_computed_style *from,
		css_computed_style *to)
{
	if (from == to) {
		return CSS_OK;
	}

	return set_word_break(to, get_word_break(from));
}

css_error css__compose_word_break(const css_computed_style *parent,
		const css_computed_style *child,
		css_computed_style *result)
{
	uint8_t type = get_word_break(child);

	return css__copy_word_break(
			type == CSS_WORD_BREAK_INHERIT ? parent : child,
			result);
}

uint32_t destroy_word_break(void *bytecode)
{
	(void)bytecode;
	return 0;
}
