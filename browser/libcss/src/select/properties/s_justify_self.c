/*
 * This file is part of LibCSS
 * Licensed under the MIT License,
 *                http://www.opensource.org/licenses/mit-license.php
 * Copyright 2026 MacSurf (#279)
 *
 * justify-self cascade -- scalar-tail storage because bits[] is full.
 */

#include "bytecode/bytecode.h"
#include "bytecode/opcodes.h"
#include "select/propset.h"
#include "select/propget.h"
#include "utils/utils.h"

#include "select/properties/properties.h"
#include "select/properties/helpers.h"

css_error css__cascade_justify_self(uint32_t opv, css_style *style,
		css_select_state *state)
{
	uint16_t value = CSS_JUSTIFY_SELF_INHERIT;

	(void)style;

	if (hasFlagValue(opv) == false) {
		switch (getValue(opv)) {
		case CSS_JUSTIFY_SELF_AUTO:
			value = CSS_JUSTIFY_SELF_AUTO;
			break;
		case CSS_JUSTIFY_SELF_STRETCH:
			value = CSS_JUSTIFY_SELF_STRETCH;
			break;
		case CSS_JUSTIFY_SELF_START:
			value = CSS_JUSTIFY_SELF_START;
			break;
		case CSS_JUSTIFY_SELF_CENTER:
			value = CSS_JUSTIFY_SELF_CENTER;
			break;
		}
	}

	if (css__outranks_existing(getOpcode(opv), isImportant(opv), state,
			getFlagValue(opv))) {
		state->computed->i.justify_self = value;
	}

	return CSS_OK;
}

css_error css__set_justify_self_from_hint(const css_hint *hint,
		css_computed_style *style)
{
	style->i.justify_self = hint->status;
	return CSS_OK;
}

css_error css__initial_justify_self(css_select_state *state)
{
	state->computed->i.justify_self = CSS_JUSTIFY_SELF_AUTO;
	return CSS_OK;
}

css_error css__copy_justify_self(
		const css_computed_style *from,
		css_computed_style *to)
{
	if (from != to) {
		to->i.justify_self = from->i.justify_self;
	}
	return CSS_OK;
}

css_error css__compose_justify_self(const css_computed_style *parent,
		const css_computed_style *child,
		css_computed_style *result)
{
	int32_t v = child->i.justify_self;

	if (v == CSS_JUSTIFY_SELF_INHERIT)
		v = parent->i.justify_self;
	result->i.justify_self = v;

	return CSS_OK;
}

uint32_t destroy_justify_self(void *bytecode)
{
	(void)bytecode;
	return 0;
}
