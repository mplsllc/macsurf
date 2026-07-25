/*
 * This file is part of LibCSS
 * Licensed under the MIT License,
 *                http://www.opensource.org/licenses/mit-license.php
 * Copyright 2026 MacSurf (fixes1052)
 *
 * Cascade appearance (#80). Does NOT inherit. Self-aligning int32_t in the
 * scalar tail of css_computed_style_i (see the note on the field itself):
 * keyword-valued, so every cascade path writes a byte-identical value and the
 * arena's memcmp over _i stays correct.
 */

#include <stdlib.h>
#include <string.h>

#include "bytecode/bytecode.h"
#include "bytecode/opcodes.h"
#include "select/propset.h"
#include "select/propget.h"
#include "utils/utils.h"

#include "select/properties/properties.h"
#include "select/properties/helpers.h"

css_error css__cascade_appearance(uint32_t opv, css_style *style,
		css_select_state *state)
{
	int32_t v = CSS_APPEARANCE_INHERIT;
	(void)style;

	if (hasFlagValue(opv) == false) {
		switch (getValue(opv)) {
		case APPEARANCE_AUTO:
			v = CSS_APPEARANCE_AUTO;
			break;
		case APPEARANCE_NONE:
			v = CSS_APPEARANCE_NONE;
			break;
		}
	}

	if (css__outranks_existing(getOpcode(opv), isImportant(opv), state,
			getFlagValue(opv))) {
		state->computed->i.appearance = v;
	}

	return CSS_OK;
}

css_error css__set_appearance_from_hint(const css_hint *hint,
		css_computed_style *style)
{
	style->i.appearance = hint->status;
	return CSS_OK;
}

css_error css__initial_appearance(css_select_state *state)
{
	state->computed->i.appearance = CSS_APPEARANCE_AUTO;
	return CSS_OK;
}

css_error css__copy_appearance(
		const css_computed_style *from,
		css_computed_style *to)
{
	if (from == to) return CSS_OK;
	to->i.appearance = from->i.appearance;
	return CSS_OK;
}

css_error css__compose_appearance(
		const css_computed_style *parent,
		const css_computed_style *child,
		css_computed_style *result)
{
	int32_t v = child->i.appearance;
	/* Non-inherited, but an explicit `inherit` still has to resolve, which
	 * is what the INHERIT sentinel is for -- same shape as every other
	 * non-inheriting property in this directory. */
	if (v == CSS_APPEARANCE_INHERIT) {
		v = parent->i.appearance;
	}
	result->i.appearance = v;
	return CSS_OK;
}

uint32_t destroy_appearance(void *bytecode)
{
	(void)bytecode;
	return 0;
}
