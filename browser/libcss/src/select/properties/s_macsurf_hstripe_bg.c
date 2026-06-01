/*
 * This file is part of LibCSS
 * Licensed under the MIT License,
 *                http://www.opensource.org/licenses/mit-license.php
 * Copyright 2026 MacSurf (fixes364)
 *
 * Cascade -macsurf-hstripe-bg. Reads two css_color words from the
 * bytecode and packs them into a single int32_t for storage in the
 * inner _i struct.
 *
 * Packed format (storage int32_t):
 *   bit 31      = set flag (1 = stripe pattern present)
 *   bits 15..29 = color2 RGB555  (R5 G5 B5, from low to high)
 *   bits 0..14  = color1 RGB555
 *
 * 0 = unset. Per-container, NOT inherited; compose returns the child
 * value verbatim. Self-aligning int32_t per fixes151b discipline.
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

static int32_t pack_hstripe(uint32_t c1, uint32_t c2)
{
	/* css_color is 0xAARRGGBB. Extract RGB and quantise to 5 bits. */
	uint32_t r1 = (c1 >> 16) & 0xff;
	uint32_t g1 = (c1 >> 8)  & 0xff;
	uint32_t b1 =  c1        & 0xff;
	uint32_t r2 = (c2 >> 16) & 0xff;
	uint32_t g2 = (c2 >> 8)  & 0xff;
	uint32_t b2 =  c2        & 0xff;
	uint32_t rgb1 = ((r1 >> 3) << 10) | ((g1 >> 3) << 5) | (b1 >> 3);
	uint32_t rgb2 = ((r2 >> 3) << 10) | ((g2 >> 3) << 5) | (b2 >> 3);
	uint32_t packed = (uint32_t)0x80000000 |
			((rgb2 & 0x7fff) << 15) |
			 (rgb1 & 0x7fff);
	return (int32_t)packed;
}

css_error css__cascade_macsurf_hstripe_bg(uint32_t opv,
		css_style *style, css_select_state *state)
{
	int32_t packed = 0;
	bool is_set = false;

	if (hasFlagValue(opv) == false) {
		if (getValue(opv) == 0x0080) { /* SET */
			uint32_t c1, c2;
			c1 = *((uint32_t *) style->bytecode);
			advance_bytecode(style, sizeof(uint32_t));
			c2 = *((uint32_t *) style->bytecode);
			advance_bytecode(style, sizeof(uint32_t));
			packed = pack_hstripe(c1, c2);
			is_set = true;
		}
	}

	if (css__outranks_existing(getOpcode(opv), isImportant(opv), state,
			getFlagValue(opv))) {
		state->computed->i.macsurf_hstripe_bg = is_set ? packed : 0;
	}

	return CSS_OK;
}

css_error css__set_macsurf_hstripe_bg_from_hint(const css_hint *hint,
		css_computed_style *style)
{
	(void)hint;
	style->i.macsurf_hstripe_bg = 0;
	return CSS_OK;
}

css_error css__initial_macsurf_hstripe_bg(css_select_state *state)
{
	state->computed->i.macsurf_hstripe_bg = 0;
	return CSS_OK;
}

css_error css__copy_macsurf_hstripe_bg(
		const css_computed_style *from,
		css_computed_style *to)
{
	if (from == to) return CSS_OK;
	to->i.macsurf_hstripe_bg = from->i.macsurf_hstripe_bg;
	return CSS_OK;
}

css_error css__compose_macsurf_hstripe_bg(
		const css_computed_style *parent,
		const css_computed_style *child,
		css_computed_style *result)
{
	(void)parent;
	result->i.macsurf_hstripe_bg = child->i.macsurf_hstripe_bg;
	return CSS_OK;
}

uint32_t destroy_macsurf_hstripe_bg(void *bytecode)
{
	(void)bytecode;
	return 0;
}
