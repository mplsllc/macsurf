/*
 * This file is part of LibCSS
 * Licensed under the MIT License,
 *		  http://www.opensource.org/licenses/mit-license.php
 * Copyright 2026 MacSurf (fixes151)
 *
 * Cascade -macsurf-grid-col-span. Stores a uint8_t directly in the
 * inner _i struct (memcmp-safe scalar -- no bit packing). Default 0
 * means "unset, treat as 1 in layout".
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

css_error css__cascade_macsurf_grid_col_span(uint32_t opv,
		css_style *style, css_select_state *state)
{
	int32_t span = 0;
	bool is_set = false;

	if (hasFlagValue(opv) == false) {
		if (getValue(opv) == 0x0080) { /* SET */
			is_set = true;
			span = *((css_fixed *) style->bytecode);
			advance_bytecode(style, sizeof(css_fixed));
			if (span < 0) span = 0;
			if (span > 255) span = 255;
		}
	}

	if (css__outranks_existing(getOpcode(opv), isImportant(opv), state,
			getFlagValue(opv))) {
		state->computed->i.macsurf_grid_col_span =
				is_set ? span : 0;
	}

	return CSS_OK;
}

css_error css__set_macsurf_grid_col_span_from_hint(const css_hint *hint,
		css_computed_style *style)
{
	(void)hint;
	style->i.macsurf_grid_col_span = 0;
	return CSS_OK;
}

css_error css__initial_macsurf_grid_col_span(css_select_state *state)
{
	state->computed->i.macsurf_grid_col_span = 0;
	return CSS_OK;
}

css_error css__copy_macsurf_grid_col_span(
		const css_computed_style *from,
		css_computed_style *to)
{
	if (from == to) return CSS_OK;
	to->i.macsurf_grid_col_span = from->i.macsurf_grid_col_span;
	return CSS_OK;
}

css_error css__compose_macsurf_grid_col_span(
		const css_computed_style *parent,
		const css_computed_style *child,
		css_computed_style *result)
{
	/* col-span is not inherited; child wins unless child is unset
	 * (0) and parent is set, in which case fall back to parent.
	 * Matches the pattern used by macsurf_grid_rows. */
	int32_t v = child->i.macsurf_grid_col_span;
	if (v == 0) v = parent->i.macsurf_grid_col_span;
	result->i.macsurf_grid_col_span = v;
	return CSS_OK;
}

uint32_t destroy_macsurf_grid_col_span(void *bytecode)
{
	(void)bytecode;
	return 0;
}
