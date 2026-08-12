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

/* fixes48/200 -- pack box-shadow h-offset, v-offset, inset flag, and RGB555
 * colour into the single int32_t storage slot.
 *
 * Packed layout:
 *   bits 31..24: h-offset (8-bit signed, pixels)
 *   bits 23..16: v-offset (8-bit signed, pixels)
 *   bit  15:     inset flag (1 = inset, 0 = normal)
 *   bits 14..0:  RGB555 shadow colour
 */
static int32_t box_shadow_pack(css_fixed h, css_fixed v, css_fixed inset, css_color color)
{
        /* css_fixed is value << 10. Recover signed integer pixels and
         * clamp to int8_t range. */
        int32_t h_px = (int32_t)(h >> 10);
        int32_t v_px = (int32_t)(v >> 10);
        uint8_t r = (uint8_t)((color >> 16) & 0xff);
        uint8_t g = (uint8_t)((color >>  8) & 0xff);
        uint8_t b = (uint8_t)((color >>  0) & 0xff);
        uint16_t rgb555;
        uint32_t out;
        if (h_px > 127) h_px = 127;
        if (h_px < -128) h_px = -128;
        if (v_px > 127) v_px = 127;
        if (v_px < -128) v_px = -128;
        /* pack to RGB555 */
        rgb555 = (uint16_t)((((uint32_t)r >> 3) << 10) |
                        (((uint32_t)g >> 3) <<  5) |
                         ((uint32_t)b >> 3));
        out = (((uint32_t)((uint8_t)h_px)) << 24) |
              (((uint32_t)((uint8_t)v_px)) << 16) |
              ((inset != 0 ? 1u : 0u) << 15) |
               (uint32_t)rgb555;
        return (int32_t)out;
}

css_error css__cascade_box_shadow(uint32_t opv, css_style *style,
                css_select_state *state)
{
        uint16_t value = CSS_BOX_SHADOW_INHERIT;
        css_fixed h = 0, v = 0, blur = 0, spread = 0, inset = 0;
        css_color color = 0;
        int32_t packed = 0;
        int32_t packed2 = 0; /* fixes361b second shadow */
        int32_t packed3 = 0; /* fixes362 third shadow */
        css_fixed shadow_count = 0;

        if (hasFlagValue(opv) == false) {
                switch (getValue(opv)) {
                case 0x0000: /* NONE */
                        value = CSS_BOX_SHADOW_NONE;
                        break;
                case 0x0080: /* SET */
                        value = CSS_BOX_SHADOW_SET;
                        /* fixes361b - bytecode now starts with a count
                         * (1 or 2), followed by 6 fields per shadow. */
                        shadow_count = *((css_fixed *) style->bytecode);
                        advance_bytecode(style, sizeof(css_fixed));
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
                        packed = box_shadow_pack(h, v, inset, color);
                        if (shadow_count >= 2) {
                                css_fixed h2, v2, b2, s2, i2;
                                css_color c2;
                                h2 = *((css_fixed *) style->bytecode);
                                advance_bytecode(style, sizeof(css_fixed));
                                v2 = *((css_fixed *) style->bytecode);
                                advance_bytecode(style, sizeof(css_fixed));
                                b2 = *((css_fixed *) style->bytecode);
                                advance_bytecode(style, sizeof(css_fixed));
                                s2 = *((css_fixed *) style->bytecode);
                                advance_bytecode(style, sizeof(css_fixed));
                                i2 = *((css_fixed *) style->bytecode);
                                advance_bytecode(style, sizeof(css_fixed));
                                c2 = *((css_color *) style->bytecode);
                                advance_bytecode(style, sizeof(css_color));
                                (void)b2; (void)s2;
                                packed2 = box_shadow_pack(h2, v2, i2, c2);
                        }
                        if (shadow_count >= 3) {
                                /* fixes362 - third shadow into outer-
                                 * struct box_shadow_3 side channel. */
                                css_fixed h3, v3, b3, s3, i3;
                                css_color c3;
                                h3 = *((css_fixed *) style->bytecode);
                                advance_bytecode(style, sizeof(css_fixed));
                                v3 = *((css_fixed *) style->bytecode);
                                advance_bytecode(style, sizeof(css_fixed));
                                b3 = *((css_fixed *) style->bytecode);
                                advance_bytecode(style, sizeof(css_fixed));
                                s3 = *((css_fixed *) style->bytecode);
                                advance_bytecode(style, sizeof(css_fixed));
                                i3 = *((css_fixed *) style->bytecode);
                                advance_bytecode(style, sizeof(css_fixed));
                                c3 = *((css_color *) style->bytecode);
                                advance_bytecode(style, sizeof(css_color));
                                (void)b3; (void)s3;
                                packed3 = box_shadow_pack(h3, v3, i3, c3);
                        }
                        break;
                }
        }
	if (css__outranks_existing(getOpcode(opv), isImportant(opv), state,
			getFlagValue(opv))) {
		/* Store second + third shadows in outer-struct side channels.
		 * 0 = unset / no shadow at that slot (won't paint). */
		state->computed->box_shadow_2 = packed2;
		state->computed->box_shadow_3 = packed3;
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

	/* fixes361b - propagate second shadow alongside the first.
	 * fixes362 - propagate third shadow too. */
	to->box_shadow_2 = from->box_shadow_2;
	to->box_shadow_3 = from->box_shadow_3;
	return set_box_shadow(to, type, integer);
}

css_error css__compose_box_shadow(const css_computed_style *parent,
		const css_computed_style *child,
		css_computed_style *result)
{
	int32_t integer = 0;
	uint8_t type = get_box_shadow(child, &integer);
	const css_computed_style *src =
		(type == CSS_BOX_SHADOW_INHERIT) ? parent : child;

	result->box_shadow_2 = src->box_shadow_2;
	result->box_shadow_3 = src->box_shadow_3;
	return css__copy_box_shadow(src, result);
}

uint32_t destroy_box_shadow(void *bytecode)
{
	return 0;
}
