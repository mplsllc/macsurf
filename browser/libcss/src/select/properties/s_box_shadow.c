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

/* fixes48 -- pack box-shadow h-offset, v-offset, and RGB565 colour
 * into the single int32_t storage slot. css_fixed values stored at
 * scale << PLOT_STYLE_RADIX (=10) are downshifted to integer pixels
 * here, clamped to [-128, 127] so they fit in 8 bits.
 *
 * Packed layout:
 *   bits 31..24: h-offset (8-bit signed, pixels)
 *   bits 23..16: v-offset (8-bit signed, pixels)
 *   bits 15..0:  RGB565 shadow colour (zero == use default grey)
 */
static int32_t box_shadow_pack(css_fixed h, css_fixed v, css_color color)
{
	/* css_fixed is value << 10. Recover signed integer pixels and
	 * clamp to int8_t range. */
	int32_t h_px = (int32_t)(h >> 10);
	int32_t v_px = (int32_t)(v >> 10);
	uint8_t r = (uint8_t)((color >> 16) & 0xff);
	uint8_t g = (uint8_t)((color >>  8) & 0xff);
	uint8_t b = (uint8_t)((color >>  0) & 0xff);
	uint16_t rgb565;
	uint32_t out;
	if (h_px > 127) h_px = 127;
	if (h_px < -128) h_px = -128;
	if (v_px > 127) v_px = 127;
	if (v_px < -128) v_px = -128;
	rgb565 = (uint16_t)((((uint32_t)r >> 3) << 11) |
			(((uint32_t)g >> 2) <<  5) |
			 ((uint32_t)b >> 3));
	out = (((uint32_t)((uint8_t)h_px)) << 24) |
	      (((uint32_t)((uint8_t)v_px)) << 16) |
	       (uint32_t)rgb565;
	return (int32_t)out;
}

css_error css__cascade_box_shadow(uint32_t opv, css_style *style,
		css_select_state *state)
{
	uint16_t value = CSS_BOX_SHADOW_INHERIT;
	css_fixed h = 0, v = 0, blur = 0, spread = 0, inset = 0;
	css_color color = 0;
	int32_t packed = 0;

	if (hasFlagValue(opv) == false) {
		switch (getValue(opv)) {
		case 0x0000: /* NONE */
			value = CSS_BOX_SHADOW_NONE;
			break;
		case 0x0080: /* SET */
			value = CSS_BOX_SHADOW_SET;
			h = *((css_fixed *) style->bytecode);
			advance_bytecode(style, sizeof(css_fixed));
			v = *((css_fixed *) style->bytecode);
			advance_bytecode(style, sizeof(css_fixed));
			blur = *((css_fixed *) style->bytecode);
			advance_bytecode(style, sizeof(css_fixed));
			spread = *((css_fixed *) style->bytecode);
			advance_bytecode(style, sizeof(css_fixed));
			inset = *((css_fixed *) style->bytecode);
			advance_bytecode(style, sizeof(css_fixed));
			color = *((css_color *) style->bytecode);
			advance_bytecode(style, sizeof(css_color));
			packed = box_shadow_pack(h, v, color);
			break;
		}
	}

	if (css__outranks_existing(getOpcode(opv), isImportant(opv), state,
			getFlagValue(opv))) {
		return set_box_shadow(state->computed, value, packed);
	}

	return CSS_OK;
}

css_error css__set_box_shadow_from_hint(const css_hint *hint,
		css_computed_style *style)
{
	return set_box_shadow(style, hint->status, hint->data.integer);
}

css_error css__initial_box_shadow(css_select_state *state)
{
	return set_box_shadow(state->computed, CSS_BOX_SHADOW_NONE, 0);
}

css_error css__copy_box_shadow(
		const css_computed_style *from,
		css_computed_style *to)
{
	int32_t integer = 0;
	uint8_t type = get_box_shadow(from, &integer);

	if (from == to) {
		return CSS_OK;
	}

	return set_box_shadow(to, type, integer);
}

css_error css__compose_box_shadow(const css_computed_style *parent,
		const css_computed_style *child,
		css_computed_style *result)
{
	int32_t integer = 0;
	uint8_t type = get_box_shadow(child, &integer);

	return css__copy_box_shadow(
			type == CSS_BOX_SHADOW_INHERIT ? parent : child,
			result);
}

uint32_t destroy_box_shadow(void *bytecode)
{
	return 0;
}
