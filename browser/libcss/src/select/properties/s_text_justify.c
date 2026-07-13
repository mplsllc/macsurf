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

css_error css__cascade_text_justify(uint32_t opv, css_style *style,
		css_select_state *state)
{
	uint16_t value = CSS_TEXT_JUSTIFY_INHERIT;

	UNUSED(style);

	if (hasFlagValue(opv) == false) {
		switch (getValue(opv)) {
		case CSS_TEXT_JUSTIFY_AUTO:
			value = CSS_TEXT_JUSTIFY_AUTO;
			break;
		case CSS_TEXT_JUSTIFY_NONE:
			value = CSS_TEXT_JUSTIFY_NONE;
			break;
		case CSS_TEXT_JUSTIFY_INTER_WORD:
			value = CSS_TEXT_JUSTIFY_INTER_WORD;
			break;
		case CSS_TEXT_JUSTIFY_INTER_CHARACTER:
			value = CSS_TEXT_JUSTIFY_INTER_CHARACTER;
			break;
		}
	}

	if (css__outranks_existing(getOpcode(opv), isImportant(opv), state,
			getFlagValue(opv))) {
		return set_text_justify(state->computed, (uint8_t)value);
	}

	return CSS_OK;
}

css_error css__set_text_justify_from_hint(const css_hint *hint,
		css_computed_style *style)
{
	return set_text_justify(style, hint->status);
}

css_error css__initial_text_justify(css_select_state *state)
{
	return set_text_justify(state->computed, CSS_TEXT_JUSTIFY_AUTO);
}

css_error css__copy_text_justify(
		const css_computed_style *from,
		css_computed_style *to)
{
	if (from == to) {
		return CSS_OK;
	}

	return set_text_justify(to, get_text_justify(from));
}

css_error css__compose_text_justify(const css_computed_style *parent,
		const css_computed_style *child,
		css_computed_style *result)
{
	uint8_t type = get_text_justify(child);

	return css__copy_text_justify(
			type == CSS_TEXT_JUSTIFY_INHERIT ? parent : child,
			result);
}

uint32_t destroy_text_justify(void *bytecode)
{
	(void)bytecode;
	return 0;
}
