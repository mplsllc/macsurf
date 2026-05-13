/*
 * This file is part of LibCSS
 * Licensed under the MIT License,
 *		  http://www.opensource.org/licenses/mit-license.php
 * Copyright 2025 Gemini CLI
 */

#include "bytecode/bytecode.h"
#include "bytecode/opcodes.h"
#include "select/propset.h"
#include "select/propget.h"
#include "utils/utils.h"

#include "select/properties/properties.h"
#include "select/properties/helpers.h"

/* fixes47 -- pack two css_color endpoints into the single int32_t
 * storage slot reserved for the macsurf_gradient property. Each
 * color is squashed to RGB565 (16 bits); high half = first color,
 * low half = second color. Lossy but adequate for gradient
 * endpoints on 8-bit and 16-bit Mac displays.
 *
 * Format of returned int32_t:
 *   bits 31..16: RGB565 of c1 (start colour, top of vertical gradient)
 *   bits 15..0:  RGB565 of c2 (end colour, bottom of vertical gradient)
 *
 * Helpers macsurf_gradient_pack / unpack are mirrored in redraw.c so
 * the renderer can decode without pulling libcss internals. */
static uint32_t macsurf_gradient_pack(css_color c1, css_color c2)
{
	uint16_t p1;
	uint16_t p2;
	uint8_t r1 = (uint8_t)((c1 >> 16) & 0xff);
	uint8_t g1 = (uint8_t)((c1 >>  8) & 0xff);
	uint8_t b1 = (uint8_t)((c1 >>  0) & 0xff);
	uint8_t r2 = (uint8_t)((c2 >> 16) & 0xff);
	uint8_t g2 = (uint8_t)((c2 >>  8) & 0xff);
	uint8_t b2 = (uint8_t)((c2 >>  0) & 0xff);
	p1 = (uint16_t)((((uint32_t)r1 >> 3) << 11) |
			(((uint32_t)g1 >> 2) <<  5) |
			 ((uint32_t)b1 >> 3));
	p2 = (uint16_t)((((uint32_t)r2 >> 3) << 11) |
			(((uint32_t)g2 >> 2) <<  5) |
			 ((uint32_t)b2 >> 3));
	return ((uint32_t)p1 << 16) | (uint32_t)p2;
}

css_error css__cascade_macsurf_gradient(uint32_t opv, css_style *style,
		css_select_state *state)
{
	uint16_t value = CSS_MACSURF_GRADIENT_INHERIT;
	css_color c1 = 0, c2 = 0;
	uint32_t packed = 0;

	if (hasFlagValue(opv) == false) {
		switch (getValue(opv)) {
		case 0x0000: /* NONE */
			value = CSS_MACSURF_GRADIENT_NONE;
			break;
		case 0x0080: /* SET */
			value = CSS_MACSURF_GRADIENT_SET;
			c1 = *((css_color *) style->bytecode);
			advance_bytecode(style, sizeof(css_color));
			c2 = *((css_color *) style->bytecode);
			advance_bytecode(style, sizeof(css_color));
			packed = macsurf_gradient_pack(c1, c2);
			break;
		}
	}

	if (css__outranks_existing(getOpcode(opv), isImportant(opv), state,
			getFlagValue(opv))) {
		return set_macsurf_gradient(state->computed, value,
				(int32_t)packed);
	}

	return CSS_OK;
}

css_error css__set_macsurf_gradient_from_hint(const css_hint *hint,
		css_computed_style *style)
{
	return set_macsurf_gradient(style, hint->status, hint->data.color);
}

css_error css__initial_macsurf_gradient(css_select_state *state)
{
	return set_macsurf_gradient(state->computed, CSS_MACSURF_GRADIENT_NONE, 0);
}

css_error css__copy_macsurf_gradient(
		const css_computed_style *from,
		css_computed_style *to)
{
	int32_t color = 0;
	uint8_t type = get_macsurf_gradient(from, &color);

	if (from == to) {
		return CSS_OK;
	}

	return set_macsurf_gradient(to, type, (css_color)color);
}

css_error css__compose_macsurf_gradient(const css_computed_style *parent,
		const css_computed_style *child,
		css_computed_style *result)
{
	int32_t color = 0;
	uint8_t type = get_macsurf_gradient(child, &color);

	return css__copy_macsurf_gradient(
			type == CSS_MACSURF_GRADIENT_INHERIT ? parent : child,
			result);
}

uint32_t destroy_macsurf_gradient(void *bytecode)
{
	return 0;
}
