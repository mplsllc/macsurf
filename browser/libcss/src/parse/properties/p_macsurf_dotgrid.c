/*
 * This file is part of LibCSS.
 * Licensed under the MIT License,
 *                http://www.opensource.org/licenses/mit-license.php
 * Copyright 2026 MacSurf (fixes365c)
 *
 * Parse -macsurf-dotgrid.
 *
 * The cssh_css.c preprocessor rewrites mactrove-style two-layer
 *   `background-image: linear-gradient(c1 1px, transparent 1px),
 *                      linear-gradient(90deg, c2 1px, transparent 1px);
 *    background-size: 2px 2px;`
 * into the same rule with `-macsurf-dotgrid: <c1> <c2>;` appended.
 * The plotter then paints alternating 1px vertical/horizontal stripes
 * on a 2x2 grid, reproducing the dot-grid texture visible behind
 * mactrove's Platinum cards.
 *
 * Accepted form:
 *   -macsurf-dotgrid: <color> <color>
 *
 * Bytecode payload after appendOPV(SET): two css_color words.
 * Storage in css_computed_style_i.macsurf_dotgrid (int32_t),
 * packed as: bit 31 = set flag, bits 15..29 = c2 RGB555,
 * bits 0..14 = c1 RGB555. 0 = unset.
 */

#include <assert.h>
#include <string.h>

#include "bytecode/bytecode.h"
#include "bytecode/opcodes.h"
#include "parse/properties/properties.h"
#include "parse/properties/utils.h"

css_error css__parse_macsurf_dotgrid(css_language *c,
		const parserutils_vector *vector, int32_t *ctx,
		css_style *result)
{
	int32_t orig_ctx = *ctx;
	css_error error;
	uint16_t value_a = 0;
	uint16_t value_b = 0;
	uint32_t color_a = 0;
	uint32_t color_b = 0;

	/* First color (vertical stripe). */
	error = css__parse_colour_specifier(c, vector, ctx,
			&value_a, &color_a);
	if (error != CSS_OK || value_a != COLOR_SET) {
		*ctx = orig_ctx;
		return CSS_INVALID;
	}

	consumeWhitespace(vector, ctx);

	/* Second color (horizontal stripe). */
	error = css__parse_colour_specifier(c, vector, ctx,
			&value_b, &color_b);
	if (error != CSS_OK || value_b != COLOR_SET) {
		*ctx = orig_ctx;
		return CSS_INVALID;
	}

	error = css__stylesheet_style_appendOPV(result,
			CSS_PROP_MACSURF_DOTGRID, 0, 0x0080 /* SET */);
	if (error != CSS_OK) {
		*ctx = orig_ctx;
		return error;
	}

	error = css__stylesheet_style_append(result, color_a);
	if (error != CSS_OK) {
		*ctx = orig_ctx;
		return error;
	}

	error = css__stylesheet_style_append(result, color_b);
	if (error != CSS_OK) {
		*ctx = orig_ctx;
		return error;
	}

	return CSS_OK;
}
