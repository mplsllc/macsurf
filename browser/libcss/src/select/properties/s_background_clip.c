/*
 * This file is part of LibCSS
 * Licensed under the MIT License,
 *                http://www.opensource.org/licenses/mit-license.php
 * Copyright 2026 MacSurf (#255)
 *
 * background-clip cascade -- box-model values (border/padding/content-box).
 * Cloned from s_object_fit.c.
 */

#include "bytecode/bytecode.h"
#include "bytecode/opcodes.h"
#include "select/propset.h"
#include "select/propget.h"
#include "utils/utils.h"

#include "select/properties/properties.h"
#include "select/properties/helpers.h"

css_error css__cascade_background_clip(uint32_t opv, css_style *style,
		css_select_state *state)
{
	uint16_t value = CSS_BACKGROUND_CLIP_INHERIT;

	UNUSED(style);

	if (hasFlagValue(opv) == false) {
		switch (getValue(opv)) {
		case CSS_BACKGROUND_CLIP_BORDER_BOX:
			value = CSS_BACKGROUND_CLIP_BORDER_BOX;
			break;
		case CSS_BACKGROUND_CLIP_PADDING_BOX:
			value = CSS_BACKGROUND_CLIP_PADDING_BOX;
			break;
		case CSS_BACKGROUND_CLIP_CONTENT_BOX:
			value = CSS_BACKGROUND_CLIP_CONTENT_BOX;
			break;
		}
	}

	if (css__outranks_existing(getOpcode(opv), isImportant(opv), state,
			getFlagValue(opv))) {
		return set_background_clip(state->computed, (uint8_t)value);
	}

	return CSS_OK;
}

css_error css__set_background_clip_from_hint(const css_hint *hint,
		css_computed_style *style)
{
	return set_background_clip(style, hint->status);
}

css_error css__initial_background_clip(css_select_state *state)
{
	return set_background_clip(state->computed,
			CSS_BACKGROUND_CLIP_BORDER_BOX);
}

css_error css__copy_background_clip(
		const css_computed_style *from,
		css_computed_style *to)
{
	if (from == to) {
		return CSS_OK;
	}

	return set_background_clip(to, get_background_clip(from));
}

css_error css__compose_background_clip(const css_computed_style *parent,
		const css_computed_style *child,
		css_computed_style *result)
{
	uint8_t type = get_background_clip(child);

	return css__copy_background_clip(
			type == CSS_BACKGROUND_CLIP_INHERIT ? parent : child,
			result);
}

uint32_t destroy_background_clip(void *bytecode)
{
	(void)bytecode;
	return 0;
}
