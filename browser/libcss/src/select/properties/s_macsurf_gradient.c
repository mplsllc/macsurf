/*
 * This file is part of LibCSS
 * Licensed under the MIT License,
 *		  http://www.opensource.org/licenses/mit-license.php
 * Copyright 2025 Gemini CLI
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

/* fixes47/48/74 -- pack two css_color endpoints plus a 2-bit direction
 * code into the single int32_t storage slot reserved for the
 * macsurf_gradient property.
 *
 * fixes47 packed RGB565 + RGB565 = 32 bits. fixes48 stole bit 15 for
 * horizontal-vs-vertical (c2 R5 stayed). fixes74 steals bit 14 for the
 * radial flag, dropping c2's red channel from R5 to R4 (16 red levels
 * instead of 32 -- still smooth on 8-bit displays).
 *
 * Format of returned int32_t:
 *   bits 31..16: RGB565 of c1 (start / centre)
 *   bit 15:      horizontal flag (0 = vertical, 1 = horizontal)
 *   bit 14:      radial flag (1 = radial; overrides horizontal)
 *   bits 13..10: R4 of c2
 *   bits 9..4:   G6 of c2
 *   bits 3..0:   B4 of c2
 *
 * macsurf_gradient_unpack in redraw.c knows the same format. */
static uint32_t macsurf_gradient_pack(css_color c1, css_color c2,
		bool horizontal, bool radial)
{
	uint16_t p1;
	uint16_t p2_lo;
	uint8_t r1 = (uint8_t)((c1 >> 16) & 0xff);
	uint8_t g1 = (uint8_t)((c1 >>  8) & 0xff);
	uint8_t b1 = (uint8_t)((c1 >>  0) & 0xff);
	uint8_t r2 = (uint8_t)((c2 >> 16) & 0xff);
	uint8_t g2 = (uint8_t)((c2 >>  8) & 0xff);
	uint8_t b2 = (uint8_t)((c2 >>  0) & 0xff);
	p1 = (uint16_t)((((uint32_t)r1 >> 3) << 11) |
			(((uint32_t)g1 >> 2) <<  5) |
			 ((uint32_t)b1 >> 3));
	/* c2 lives in bits 13..0: R4 G6 B4. */
	p2_lo = (uint16_t)((((uint32_t)r2 >> 4) << 10) |
			(((uint32_t)g2 >> 2) <<  4) |
			 ((uint32_t)b2 >> 4));
	return ((uint32_t)p1 << 16) |
	       (horizontal ? 0x8000U : 0U) |
	       (radial ? 0x4000U : 0U) |
	       (uint32_t)p2_lo;
}

css_error css__cascade_macsurf_gradient(uint32_t opv, css_style *style,
		css_select_state *state)
{
	uint16_t value = CSS_MACSURF_GRADIENT_INHERIT;
	css_color c1 = 0, c2 = 0;
	uint32_t packed = 0;
	bool horizontal = false;
	bool radial = false;
	/* fixes345 - radial size+position tail. */
	bool rad_set = false;
	int32_t rad_sx = -1, rad_sy = -1, rad_px = -1, rad_py = -1;
	/* fixes365b - extended-linear (diagonal / 3-stop) descriptor. */
	bool ext_set = false;
	int32_t ext_angle = 0;
	int32_t ext_pos0 = 0, ext_pos1 = 0, ext_pos2 = 0;
	css_color ext_col0 = 0, ext_col1 = 0, ext_col2 = 0;

	if (hasFlagValue(opv) == false) {
		switch (getValue(opv)) {
		case 0x0000: /* NONE */
			value = CSS_MACSURF_GRADIENT_NONE;
			break;
		case 0x0100: /* SET radial (fixes74) */
			radial = true;
			value = CSS_MACSURF_GRADIENT_SET;
			c1 = *((css_color *) style->bytecode);
			advance_bytecode(style, sizeof(css_color));
			c2 = *((css_color *) style->bytecode);
			advance_bytecode(style, sizeof(css_color));
			packed = macsurf_gradient_pack(c1, c2, false, true);
#ifdef MACSURF_DEBUG
			{
				extern void macsurf_debug_log_writef(
					const char *fmt, ...);
				macsurf_debug_log_writef(
					"fixes345 RADIAL cascade firing c1=%ld c2=%ld",
					(long)c1, (long)c2);
			}
#endif
			/* fixes345 - radial bytecode tail: 5 uint32 with
			 * [set_flag, sx, sy, px, py]. */
			{
				uint32_t rad_flag =
					*((uint32_t *) style->bytecode);
				advance_bytecode(style, sizeof(uint32_t));
				rad_sx = (int32_t)(*((uint32_t *)
					style->bytecode));
				advance_bytecode(style, sizeof(uint32_t));
				rad_sy = (int32_t)(*((uint32_t *)
					style->bytecode));
				advance_bytecode(style, sizeof(uint32_t));
				rad_px = (int32_t)(*((uint32_t *)
					style->bytecode));
				advance_bytecode(style, sizeof(uint32_t));
				rad_py = (int32_t)(*((uint32_t *)
					style->bytecode));
				advance_bytecode(style, sizeof(uint32_t));
				rad_set = (rad_flag != 0);
			}
			/* fixes360 - extend fixes348's alpha-aware downgrade
			 * to the radial-gradient case. The same painter
			 * limitation applies to radial as to linear: alpha is
			 * ignored, so a radial like
			 *   radial-gradient(rgba(220,184,94,.20),
			 *                   transparent 62%)
			 * (macsurf.org's body::before glow) paints as opaque
			 * gold blending to opaque BLACK - exactly the gold-to-
			 * black bands visible in the regression screenshots
			 * (june2.jpg, june5.jpg). Same drop conditions as the
			 * linear case: either stop alpha < 0xC0, OR both <
			 * 0xFF with one fully transparent. */
			{
				uint8_t a1 = (uint8_t)((c1 >> 24) & 0xff);
				uint8_t a2 = (uint8_t)((c2 >> 24) & 0xff);
				bool drop = false;
				if (a1 < 0xC0 || a2 < 0xC0) drop = true;
				if ((a1 < 0xFF && a2 == 0) ||
				    (a2 < 0xFF && a1 == 0)) drop = true;
				if (drop) {
					value = CSS_MACSURF_GRADIENT_NONE;
					packed = 0;
					radial = false;
					rad_set = false;
				}
			}
#ifdef MACSURF_DEBUG
			{
				extern void macsurf_debug_log_writef(
					const char *fmt, ...);
				macsurf_debug_log_writef(
					"fixes360 RADIAL alpha-check c1=%ld c2=%ld val=%d",
					(long)c1, (long)c2, (int)value);
			}
#endif
			break;
		case 0x0180: /* fixes365b - extended linear (diagonal /
		              * 3-stop with positions). Tail layout:
		              *   [c1, last, angle, p0, p1, p2, col0,
		              *    col1, col2] = 9 words. */
			value = CSS_MACSURF_GRADIENT_SET;
			c1 = *((css_color *) style->bytecode);
			advance_bytecode(style, sizeof(css_color));
			c2 = *((css_color *) style->bytecode);
			advance_bytecode(style, sizeof(css_color));
			ext_angle = (int32_t)(*((uint32_t *)
					style->bytecode));
			advance_bytecode(style, sizeof(uint32_t));
			ext_pos0 = (int32_t)(*((uint32_t *)
					style->bytecode));
			advance_bytecode(style, sizeof(uint32_t));
			ext_pos1 = (int32_t)(*((uint32_t *)
					style->bytecode));
			advance_bytecode(style, sizeof(uint32_t));
			ext_pos2 = (int32_t)(*((uint32_t *)
					style->bytecode));
			advance_bytecode(style, sizeof(uint32_t));
			ext_col0 = *((css_color *) style->bytecode);
			advance_bytecode(style, sizeof(css_color));
			ext_col1 = *((css_color *) style->bytecode);
			advance_bytecode(style, sizeof(css_color));
			ext_col2 = *((css_color *) style->bytecode);
			advance_bytecode(style, sizeof(css_color));
			ext_set = true;
			/* Map to a cardinal horizontal/vertical flag for
			 * the legacy packed slot so existing callers that
			 * only read the packed int still get a usable
			 * fallback when the extended pointer is ignored.
			 * Mask out the high-bits stop-count encoding the
			 * parser packs into ext_angle. */
			{
				int a = (int)((uint32_t)ext_angle & 0xffffu);
				while (a < 0) a += 360;
				a = a % 360;
				if ((a >= 45 && a < 135) ||
				    (a >= 225 && a < 315)) {
					horizontal = true;
				}
			}
			packed = macsurf_gradient_pack(c1, c2, horizontal,
					false);
			/* Apply the same alpha-aware downgrade as the
			 * cardinal SET case so semi-transparent
			 * extended gradients drop gracefully. */
			{
				uint8_t a1 = (uint8_t)((c1 >> 24) & 0xff);
				uint8_t a2 = (uint8_t)((c2 >> 24) & 0xff);
				bool drop = false;
				if (a1 < 0xC0 || a2 < 0xC0) drop = true;
				if ((a1 < 0xFF && a2 == 0) ||
				    (a2 < 0xFF && a1 == 0)) drop = true;
				if (drop) {
					value = CSS_MACSURF_GRADIENT_NONE;
					packed = 0;
					ext_set = false;
				}
			}
			break;
		case 0x00C0: /* SET horizontal (fixes48) */
			horizontal = true;
			/* fallthrough */
		case 0x0080: /* SET vertical (default) */
			value = CSS_MACSURF_GRADIENT_SET;
			c1 = *((css_color *) style->bytecode);
			advance_bytecode(style, sizeof(css_color));
			c2 = *((css_color *) style->bytecode);
			advance_bytecode(style, sizeof(css_color));
			packed = macsurf_gradient_pack(c1, c2, horizontal,
					false);
			/* fixes348 - alpha-aware downgrade.
			 *
			 * The gradient painter in plotters.c ignores alpha
			 * (it interpolates RGB only). When the source CSS is
			 * a pinstripe / overlay like
			 *   linear-gradient(rgba(255,255,255,.5) 1px,
			 *                   transparent 1px)
			 * (mactrove's platinum.css line 39-40), c1's alpha is
			 * 0x7F-0x80 and c2's alpha is 0x00. Painted opaquely
			 * this becomes a HARSH BLACK-TO-WHITE gradient because
			 * transparent's rgb is 0x000000 and the white stop's
			 * rgb is 0xFFFFFF - exactly the black-to-white box
			 * artefact users see "where images should be".
			 *
			 * Until the painter learns to alpha-composite over the
			 * page background, the correct visual answer is to
			 * SKIP these gradients entirely so the underlying
			 * background-color (Platinum-grey for mactrove) shows
			 * through. Conditions for skip:
			 *
			 *   - either stop has alpha < 0xC0 (less than ~75%
			 *     opaque). Authors don't write decorative semi-
			 *     transparent gradients expecting them to render
			 *     as opaque; if they wanted opaque they'd write
			 *     opaque colours.
			 *
			 *   - OR both stops have alpha < 0xFF and one is fully
			 *     transparent (alpha = 0) - the classic pinstripe
			 *     overlay shape.
			 *
			 * Fully-opaque gradients are unaffected; this only
			 * intercepts the alpha-overlay class. */
			{
				uint8_t a1 = (uint8_t)((c1 >> 24) & 0xff);
				uint8_t a2 = (uint8_t)((c2 >> 24) & 0xff);
				bool drop = false;
				if (a1 < 0xC0 || a2 < 0xC0) drop = true;
				if ((a1 < 0xFF && a2 == 0) ||
				    (a2 < 0xFF && a1 == 0)) drop = true;
				if (drop) {
					value = CSS_MACSURF_GRADIENT_NONE;
					packed = 0;
				}
			}
#ifdef MACSURF_DEBUG
			/* fixes318 (#147) probe - log raw bytecode values so
			 * we can see on hardware whether c1 / c2 actually
			 * differ, or whether one slot reads as the other.
			 * Custom logger only supports %d %ld %p %s %% - emit
			 * as decimal longs and decode visually. */
			{
				extern void macsurf_debug_log_writef(
					const char *fmt, ...);
				macsurf_debug_log_writef(
					"grad cascade: opv=%ld c1=%ld c2=%ld packed=%ld h=%d val=%d",
					(long)opv, (long)c1, (long)c2,
					(long)packed, horizontal ? 1 : 0,
					(int)value);
			}
#endif
			break;
		}
	}
	(void)radial;

	if (css__outranks_existing(getOpcode(opv), isImportant(opv), state,
			getFlagValue(opv))) {
		/* fixes344b - capture the full ARGB of both stops into the
		 * outer struct's macsurf_gradient_full side-channel so the
		 * painter can do per-pixel alpha blending. Only allocates
		 * when at least one stop is non-opaque (alpha < 0xFF);
		 * fully-opaque gradients take the fast existing path with
		 * full == NULL. */
		if (value == CSS_MACSURF_GRADIENT_SET) {
			uint8_t a1 = (uint8_t)((c1 >> 24) & 0xff);
			uint8_t a2 = (uint8_t)((c2 >> 24) & 0xff);
			if (a1 != 0xff || a2 != 0xff) {
				css_color *full = (css_color *)malloc(
					2 * sizeof(css_color));
				if (full != NULL) {
					full[0] = c1;
					full[1] = c2;
					if (state->computed->macsurf_gradient_full
							!= NULL) {
						free(state->computed->macsurf_gradient_full);
					}
					state->computed->macsurf_gradient_full = full;
				}
			} else {
				if (state->computed->macsurf_gradient_full
						!= NULL) {
					free(state->computed->macsurf_gradient_full);
					state->computed->macsurf_gradient_full = NULL;
				}
			}
		}
		/* fixes345 - stash radial size+position when set. */
		if (radial && rad_set) {
			int32_t *rad = (int32_t *)malloc(4 * sizeof(int32_t));
			if (rad != NULL) {
				rad[0] = rad_sx;
				rad[1] = rad_sy;
				rad[2] = rad_px;
				rad[3] = rad_py;
				if (state->computed->macsurf_gradient_radial
						!= NULL) {
					free(state->computed->macsurf_gradient_radial);
				}
				state->computed->macsurf_gradient_radial = rad;
			}
		} else if (value == CSS_MACSURF_GRADIENT_SET) {
			if (state->computed->macsurf_gradient_radial != NULL) {
				free(state->computed->macsurf_gradient_radial);
				state->computed->macsurf_gradient_radial = NULL;
			}
		}
		/* fixes365b - extended-linear side-channel allocation. */
		if (ext_set) {
			int32_t *ext = (int32_t *)malloc(7 * sizeof(int32_t));
			if (ext != NULL) {
				ext[0] = ext_angle;
				ext[1] = ext_pos0;
				ext[2] = ext_pos1;
				ext[3] = ext_pos2;
				ext[4] = (int32_t)ext_col0;
				ext[5] = (int32_t)ext_col1;
				ext[6] = (int32_t)ext_col2;
				if (state->computed->macsurf_gradient_stops
						!= NULL) {
					free(state->computed->macsurf_gradient_stops);
				}
				state->computed->macsurf_gradient_stops = ext;
			}
		} else if (value == CSS_MACSURF_GRADIENT_SET ||
		           value == CSS_MACSURF_GRADIENT_NONE) {
			if (state->computed->macsurf_gradient_stops != NULL) {
				free(state->computed->macsurf_gradient_stops);
				state->computed->macsurf_gradient_stops = NULL;
			}
		}
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
	css_error err;

	if (from == to) {
		return CSS_OK;
	}

	err = set_macsurf_gradient(to, type, (css_color)color);
	if (err != CSS_OK) return err;

	/* fixes344b - also propagate the full-ARGB side-channel. */
	if (to->macsurf_gradient_full != NULL) {
		free(to->macsurf_gradient_full);
		to->macsurf_gradient_full = NULL;
	}
	if (from->macsurf_gradient_full != NULL) {
		css_color *copy = (css_color *)malloc(
			2 * sizeof(css_color));
		if (copy != NULL) {
			memcpy(copy, from->macsurf_gradient_full,
				2 * sizeof(css_color));
			to->macsurf_gradient_full = copy;
		}
	}

	/* fixes345 - also propagate the radial size+position. */
	if (to->macsurf_gradient_radial != NULL) {
		free(to->macsurf_gradient_radial);
		to->macsurf_gradient_radial = NULL;
	}
	if (from->macsurf_gradient_radial != NULL) {
		int32_t *copy = (int32_t *)malloc(4 * sizeof(int32_t));
		if (copy != NULL) {
			memcpy(copy, from->macsurf_gradient_radial,
				4 * sizeof(int32_t));
			to->macsurf_gradient_radial = copy;
		}
	}

	/* fixes365b - propagate the extended-linear descriptor. */
	if (to->macsurf_gradient_stops != NULL) {
		free(to->macsurf_gradient_stops);
		to->macsurf_gradient_stops = NULL;
	}
	if (from->macsurf_gradient_stops != NULL) {
		int32_t *copy = (int32_t *)malloc(7 * sizeof(int32_t));
		if (copy != NULL) {
			memcpy(copy, from->macsurf_gradient_stops,
				7 * sizeof(int32_t));
			to->macsurf_gradient_stops = copy;
		}
	}
	return CSS_OK;
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
