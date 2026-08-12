/*
 * This file is part of LibCSS
 * Licensed under the MIT License,
 *                http://www.opensource.org/licenses/mit-license.php
 * Copyright 2026 MacSurf (fixes355)
 *
 * Cascade tab-size (#58). Inherited. Self-aligning int32_t scalar.
 *
 * 0 = unset / use spec default of 8 at consume time.
 * N>0 = explicit integer width.
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

css_error css__cascade_tab_size(uint32_t opv, css_style *style,
		css_select_state *state)
{
	int32_t n = 0;

	if (hasFlagValue(opv) == false) {
		if (getValue(opv) == 0x0080) { /* SET */
			n = *((css_fixed *) style->bytecode);
			advance_bytecode(style, sizeof(css_fixed));
		}
	}

	if (css__outranks_existing(getOpcode(opv), isImportant(opv), state,
			getFlagValue(opv))) {
		state->computed->i.tab_size = n;
	}

	return CSS_OK;
}

css_error css__set_tab_size_from_hint(const css_hint *hint,
		css_computed_style *style)
{
	(void)hint;
	style->i.tab_size = 0;
	return CSS_OK;
}

css_error css__initial_tab_size(css_select_state *state)
{
	state->computed->i.tab_size = 0; /* 0 => consumer uses 8 */
	return CSS_OK;
}

css_error css__copy_tab_size(
		const css_computed_style *from,
		css_computed_style *to)
{
	if (from == to) return CSS_OK;
	to->i.tab_size = from->i.tab_size;
	return CSS_OK;
}

css_error css__compose_tab_size(
		const css_computed_style *parent,
		const css_computed_style *child,
		css_computed_style *result)
{
	/* Inherited - child wins, fall back to parent when child unset. */
	int32_t v = child->i.tab_size;
	if (v == 0) v = parent->i.tab_size;
	result->i.tab_size = v;
	return CSS_OK;
}

uint32_t destroy_tab_size(void *bytecode)
{
	(void)bytecode;
	return 0;
}
