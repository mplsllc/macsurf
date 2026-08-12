/*
 * This file is part of LibCSS
 * Licensed under the MIT License,
 *		  http://www.opensource.org/licenses/mit-license.php
 * Copyright 2009 John-Mark Bell <jmb@netsurf-browser.org>
 */

#include <assert.h>

#include "bytecode/bytecode.h"
#include "bytecode/opcodes.h"
#include "select/properties/properties.h"
#include "select/propget.h"
#include "select/propset.h"
#include "utils/utils.h"

#include "select/properties/helpers.h"

/******************************************************************************
 * Utilities below here							      *
 ******************************************************************************/

/* fixes1159b: map a calc-capable length property to its slot number in
 * css_computed_style.macsurf_calc_expr. The five shared cascade helpers
 * below serve many properties; the slot number makes each property's calc
 * expression findable at resolve time (unit.c) without a pointer bit-cast.
 * 0xFF = property has no slot (never happens: only slot-mapped properties
 * carry CALC values, but degrade to 0px if it ever does). */
static uint8_t css__calc_slot_for_prop(uint32_t prop)
{
	switch (prop) {
	case CSS_PROP_BORDER_TOP_WIDTH:		return 0;
	case CSS_PROP_BORDER_RIGHT_WIDTH:	return 1;
	case CSS_PROP_BORDER_BOTTOM_WIDTH:	return 2;
	case CSS_PROP_BORDER_LEFT_WIDTH:	return 3;
	case CSS_PROP_COLUMN_RULE_WIDTH:	return 4;
	case CSS_PROP_OUTLINE_WIDTH:		return 5;
	case CSS_PROP_TOP:			return 6;
	case CSS_PROP_RIGHT:			return 7;
	case CSS_PROP_BOTTOM:			return 8;
	case CSS_PROP_LEFT:			return 9;
	case CSS_PROP_HEIGHT:			return 10;
	case CSS_PROP_MIN_HEIGHT:		return 11;
	case CSS_PROP_MIN_WIDTH:		return 12;
	case CSS_PROP_MARGIN_TOP:		return 13;
	case CSS_PROP_MARGIN_RIGHT:		return 14;
	case CSS_PROP_MARGIN_BOTTOM:		return 15;
	case CSS_PROP_MARGIN_LEFT:		return 16;
	case CSS_PROP_MAX_HEIGHT:		return 17;
	case CSS_PROP_MAX_WIDTH:		return 18;
	case CSS_PROP_COLUMN_GAP:		return 19;
	case CSS_PROP_COLUMN_WIDTH:		return 20;
	case CSS_PROP_LETTER_SPACING:		return 21;
	case CSS_PROP_WORD_SPACING:		return 22;
	case CSS_PROP_PADDING_TOP:		return 23;
	case CSS_PROP_PADDING_RIGHT:		return 24;
	case CSS_PROP_PADDING_BOTTOM:		return 25;
	case CSS_PROP_PADDING_LEFT:		return 26;
	case CSS_PROP_TEXT_INDENT:		return 27;
	case CSS_PROP_ROW_GAP:			return 28;
	default:				return 0xFF;
	}
}
css_error css__cascade_bg_border_color(uint32_t opv, css_style *style,
		css_select_state *state,
		css_error (*fun)(css_computed_style *, uint8_t, css_color))
{
	uint16_t value = CSS_BACKGROUND_COLOR_INHERIT;
	css_color color = 0;

	assert(CSS_BACKGROUND_COLOR_INHERIT ==
	       (enum css_background_color_e)CSS_BORDER_COLOR_INHERIT);
	assert(CSS_BACKGROUND_COLOR_COLOR ==
	       (enum css_background_color_e)CSS_BORDER_COLOR_COLOR);
	assert(CSS_BACKGROUND_COLOR_CURRENT_COLOR ==
	       (enum css_background_color_e)CSS_BORDER_COLOR_CURRENT_COLOR);

	if (hasFlagValue(opv) == false) {
		switch (getValue(opv)) {
		case BACKGROUND_COLOR_TRANSPARENT:
			value = CSS_BACKGROUND_COLOR_COLOR;
			break;
		case BACKGROUND_COLOR_CURRENT_COLOR:
			value = CSS_BACKGROUND_COLOR_CURRENT_COLOR;
			break;
		case BACKGROUND_COLOR_SET:
			value = CSS_BACKGROUND_COLOR_COLOR;
			color = *((css_color *) style->bytecode);
			advance_bytecode(style, sizeof(color));
			break;
		}
	}

	if (css__outranks_existing(getOpcode(opv), isImportant(opv), state,
			getFlagValue(opv))) {
		return fun(state->computed, value, color);
	}

	return CSS_OK;
}

css_error css__cascade_uri_none(uint32_t opv, css_style *style,
		css_select_state *state,
		css_error (*fun)(css_computed_style *, uint8_t,
				lwc_string *))
{
	uint16_t value = CSS_BACKGROUND_IMAGE_INHERIT;
	lwc_string *uri = NULL;
	int outranks;

	if (hasFlagValue(opv) == false) {
		switch (getValue(opv)) {
		case BACKGROUND_IMAGE_NONE:
			value = CSS_BACKGROUND_IMAGE_NONE;
			break;
		case BACKGROUND_IMAGE_URI:
		{
			/* fixes347f - error-check the URI lookup so a failure
			 * (sheet/snumber mismatch, out-of-bounds) downgrades
			 * to NONE rather than silently writing IMAGE with a
			 * NULL string pointer (which presents as kind=IMAGE
			 * uri=null at the box-construct fetch site and looks
			 * like a fetch path bug rather than what it is). */
			css_error gerr = css__stylesheet_string_get(
					style->sheet,
					*((css_code_t *) style->bytecode),
					&uri);
			advance_bytecode(style, sizeof(css_code_t));
			if (gerr != CSS_OK || uri == NULL) {
				value = CSS_BACKGROUND_IMAGE_NONE;
				uri = NULL;
			} else {
				value = CSS_BACKGROUND_IMAGE_IMAGE;
			}
			break;
		}
		}
	}

	/** \todo lose fun != NULL once all properties have set routines */
	outranks = (fun != NULL && css__outranks_existing(getOpcode(opv),
			isImportant(opv), state, getFlagValue(opv))) ? 1 : 0;

#ifdef MACSURF_DEBUG
	if (getOpcode(opv) == 59) {
		extern void macsurf_debug_log_writef(
			const char *fmt, ...);
		macsurf_debug_log_writef(
			"fixes347e uri_none: op=%d val=%d uri=%s outranks=%d pseudo=%d",
			(int)getOpcode(opv), (int)value,
			(uri != NULL) ? lwc_string_data(uri) : "(null)",
			outranks, (int)state->current_pseudo);
	}
#endif

	/* fixes347g - var()-resolved URIs must always land on the pseudo's
	 * computed style. The outranks gate sometimes drops the URI write
	 * because of interaction with the post-cascade set_initial fixup
	 * (css_select.c:1492) - and because CSS_BACKGROUND_IMAGE_NONE and
	 * CSS_BACKGROUND_IMAGE_IMAGE share enum value 0x1 (properties.h),
	 * the post-fixup result presents to the box-construct fetch site
	 * as kind=IMAGE with a NULL URI, looking exactly like a fetch path
	 * bug. When the deferred-decl resolver produces a real URI we want
	 * it to land, so force the write when value=IMAGE+non-null URI and
	 * manually mark prop->set so set_initial doesn't overwrite the URI
	 * in the fixup pass. */
	if (outranks) {
		return fun(state->computed, value, uri);
	}

	if (value == CSS_BACKGROUND_IMAGE_IMAGE && uri != NULL && fun != NULL) {
		state->props[getOpcode(opv)][state->current_pseudo].set = 1;
		return fun(state->computed, value, uri);
	}

	return CSS_OK;
}

css_error css__cascade_border_style(uint32_t opv, css_style *style,
		css_select_state *state,
		css_error (*fun)(css_computed_style *, uint8_t))
{
	uint16_t value = CSS_BORDER_STYLE_INHERIT;

	UNUSED(style);

	if (hasFlagValue(opv) == false) {
		switch (getValue(opv)) {
		case BORDER_STYLE_NONE:
			value = CSS_BORDER_STYLE_NONE;
			break;
		case BORDER_STYLE_HIDDEN:
			value = CSS_BORDER_STYLE_HIDDEN;
			break;
		case BORDER_STYLE_DOTTED:
			value = CSS_BORDER_STYLE_DOTTED;
			break;
		case BORDER_STYLE_DASHED:
			value = CSS_BORDER_STYLE_DASHED;
			break;
		case BORDER_STYLE_SOLID:
			value = CSS_BORDER_STYLE_SOLID;
			break;
		case BORDER_STYLE_DOUBLE:
			value = CSS_BORDER_STYLE_DOUBLE;
			break;
		case BORDER_STYLE_GROOVE:
			value = CSS_BORDER_STYLE_GROOVE;
			break;
		case BORDER_STYLE_RIDGE:
			value = CSS_BORDER_STYLE_RIDGE;
			break;
		case BORDER_STYLE_INSET:
			value = CSS_BORDER_STYLE_INSET;
			break;
		case BORDER_STYLE_OUTSET:
			value = CSS_BORDER_STYLE_OUTSET;
			break;
		}
	}

	if (css__outranks_existing(getOpcode(opv), isImportant(opv), state,
			getFlagValue(opv))) {
		return fun(state->computed, value);
	}

	return CSS_OK;
}

css_error css__cascade_border_width(uint32_t opv, css_style *style,
		css_select_state *state,
		css_error (*fun)(css_computed_style *, uint8_t, css_fixed,
				css_unit))
{
	uint16_t value = CSS_BORDER_WIDTH_INHERIT;
	css_fixed length = 0;
	uint32_t unit = UNIT_PX;
	bool is_calc = false;

	if (hasFlagValue(opv) == false) {
		switch (getValue(opv)) {
		case BORDER_WIDTH_SET:
			value = CSS_BORDER_WIDTH_WIDTH;
			length = *((css_fixed *) style->bytecode);
			advance_bytecode(style, sizeof(length));
			unit = *((uint32_t *) style->bytecode);
			advance_bytecode(style, sizeof(unit));
			break;
		case BORDER_WIDTH_THIN:
			value = CSS_BORDER_WIDTH_THIN;
			break;
		case BORDER_WIDTH_MEDIUM:
			value = CSS_BORDER_WIDTH_MEDIUM;
			break;
		case BORDER_WIDTH_THICK:
			value = CSS_BORDER_WIDTH_THICK;
			break;
		case BORDER_WIDTH_CALC:
		{
			uint32_t snum = 0;
			lwc_string *calc_expr = NULL;
			uint8_t slot = css__calc_slot_for_prop(
					getOpcode(opv));

			value = CSS_BORDER_WIDTH_WIDTH;
			advance_bytecode(style, sizeof(unit));
			snum = *((uint32_t *) style->bytecode);
			advance_bytecode(style, sizeof(snum));
			css__stylesheet_string_get(style->sheet, snum, &calc_expr);
			/* fixes1159b: store the expression in the style's
			 * per-property side slot and put the SLOT NUMBER in
			 * the length field. Bit-casting the lwc_string
			 * pointer into css_fixed only fits on 32-bit targets
			 * (truncates to a garbage pointer on Linux 64-bit,
			 * misresolved to 0 on the Mac). */
			if (calc_expr != NULL &&
					slot < MACSURF_CALC_SLOT_COUNT) {
				state->computed->macsurf_calc_expr[slot] =
						calc_expr;
				length = (css_fixed)slot;
				is_calc = true;
			}
			break;
		}
		default:
			assert(0 && "Invalid value");
			break;
		}
	}

	if (is_calc == false) {
		unit = css__to_css_unit(unit);
	} else {
		unit = CSS_UNIT_CALC;
	}

	if (css__outranks_existing(getOpcode(opv), isImportant(opv), state,
			getFlagValue(opv))) {
		return fun(state->computed, value, length, unit);
	}

	return CSS_OK;
}

css_error css__cascade_length_auto(uint32_t opv, css_style *style,
		css_select_state *state,
		css_error (*fun)(css_computed_style *, uint8_t, css_fixed,
				css_unit))
{
	uint16_t value = CSS_BOTTOM_INHERIT;
	css_fixed length = 0;
	uint32_t unit = UNIT_PX;
	bool is_calc = false;

	if (hasFlagValue(opv) == false) {
		switch (getValue(opv)) {
		case BOTTOM_SET:
			value = CSS_BOTTOM_SET;
			length = *((css_fixed *) style->bytecode);
			advance_bytecode(style, sizeof(length));
			unit = *((uint32_t *) style->bytecode);
			advance_bytecode(style, sizeof(unit));
			break;
		case BOTTOM_AUTO:
			value = CSS_BOTTOM_AUTO;
			break;
		case BOTTOM_CALC:
		{
			uint32_t snum = 0;
			lwc_string *calc_expr = NULL;
			uint8_t slot = css__calc_slot_for_prop(
					getOpcode(opv));

			value = CSS_BOTTOM_SET;
			advance_bytecode(style, sizeof(unit));
			snum = *((uint32_t *) style->bytecode);
			advance_bytecode(style, sizeof(snum));
			css__stylesheet_string_get(style->sheet, snum, &calc_expr);
			/* fixes1159b: store the expression in the style's
			 * per-property side slot and put the SLOT NUMBER in
			 * the length field. Bit-casting the lwc_string
			 * pointer into css_fixed only fits on 32-bit targets
			 * (truncates to a garbage pointer on Linux 64-bit,
			 * misresolved to 0 on the Mac). */
			if (calc_expr != NULL &&
					slot < MACSURF_CALC_SLOT_COUNT) {
				state->computed->macsurf_calc_expr[slot] =
						calc_expr;
				length = (css_fixed)slot;
				is_calc = true;
			}
			break;
		}
		default:
			assert(0 && "Invalid value");
			break;
		}
	}

	if (is_calc == false) {
		unit = css__to_css_unit(unit);
	} else {
		unit = CSS_UNIT_CALC;
	}

	if (css__outranks_existing(getOpcode(opv), isImportant(opv), state,
			getFlagValue(opv))) {
		return fun(state->computed, value, length, unit);
	}

	return CSS_OK;
}

css_error css__cascade_length_auto_calc(uint32_t opv, css_style *style,
		css_select_state *state,
		css_error (*fun)(css_computed_style *, uint8_t, css_fixed_or_calc,
				css_unit))
{
	uint16_t value = CSS_BOTTOM_INHERIT;
	css_fixed_or_calc length;
	uint32_t unit = CSS_UNIT_PX;
	uint32_t snum = 0;
	length.value = 0;

	if (hasFlagValue(opv) == false) {
		switch (getValue(opv)) {
		case BOTTOM_SET:
			value = CSS_BOTTOM_SET;
			length.value = *((css_fixed *) style->bytecode);
			advance_bytecode(style, sizeof(length.value));
			unit = css__to_css_unit(*((uint32_t *) style->bytecode));
			advance_bytecode(style, sizeof(unit));
			break;
		case BOTTOM_AUTO:
			value = CSS_BOTTOM_AUTO;
			break;
		case BOTTOM_CALC:
			value = CSS_BOTTOM_SET;
			advance_bytecode(style, sizeof(unit)); /* TODO: Skip unit, not sure what to do */
			snum = *((uint32_t *) style->bytecode);
			advance_bytecode(style, sizeof(snum));
			unit = CSS_UNIT_CALC;
			css__stylesheet_string_get(style->sheet, snum, &length.calc);
			break;
		default:
			assert(0 && "Invalid value");
			break;
		}
	}

	if (css__outranks_existing(getOpcode(opv), isImportant(opv), state,
			getFlagValue(opv))) {
		return fun(state->computed, value, length, unit);
	}

	return CSS_OK;
}

css_error css__cascade_length_normal(uint32_t opv, css_style *style,
		css_select_state *state,
		css_error (*fun)(css_computed_style *, uint8_t, css_fixed,
				css_unit))
{
	uint16_t value = CSS_LETTER_SPACING_INHERIT;
	css_fixed length = 0;
	uint32_t unit = UNIT_PX;
	bool is_calc = false;

	if (hasFlagValue(opv) == false) {
		switch (getValue(opv)) {
		case LETTER_SPACING_SET:
			value = CSS_LETTER_SPACING_SET;
			length = *((css_fixed *) style->bytecode);
			advance_bytecode(style, sizeof(length));
			unit = *((uint32_t *) style->bytecode);
			advance_bytecode(style, sizeof(unit));
			break;
		case LETTER_SPACING_NORMAL:
			value = CSS_LETTER_SPACING_NORMAL;
			break;
		case LETTER_SPACING_CALC:
		{
			uint32_t snum = 0;
			lwc_string *calc_expr = NULL;
			uint8_t slot = css__calc_slot_for_prop(
					getOpcode(opv));

			value = CSS_LETTER_SPACING_SET;
			advance_bytecode(style, sizeof(unit));
			snum = *((uint32_t *) style->bytecode);
			advance_bytecode(style, sizeof(snum));
			css__stylesheet_string_get(style->sheet, snum, &calc_expr);
			/* fixes1159b: store the expression in the style's
			 * per-property side slot and put the SLOT NUMBER in
			 * the length field. Bit-casting the lwc_string
			 * pointer into css_fixed only fits on 32-bit targets
			 * (truncates to a garbage pointer on Linux 64-bit,
			 * misresolved to 0 on the Mac). */
			if (calc_expr != NULL &&
					slot < MACSURF_CALC_SLOT_COUNT) {
				state->computed->macsurf_calc_expr[slot] =
						calc_expr;
				length = (css_fixed)slot;
				is_calc = true;
			}
			break;
		}
		default:
			assert(0 && "Invalid value");
			break;
		}
	}

	if (is_calc == false) {
		unit = css__to_css_unit(unit);
	} else {
		unit = CSS_UNIT_CALC;
	}

	if (css__outranks_existing(getOpcode(opv), isImportant(opv), state,
			getFlagValue(opv))) {
		return fun(state->computed, value, length, unit);
	}

	return CSS_OK;
}

css_error css__cascade_length_none(uint32_t opv, css_style *style,
		css_select_state *state,
		css_error (*fun)(css_computed_style *, uint8_t, css_fixed,
				css_unit))
{
	uint16_t value = CSS_MAX_HEIGHT_INHERIT;
	css_fixed length = 0;
	uint32_t unit = UNIT_PX;
	bool is_calc = false;

	if (hasFlagValue(opv) == false) {
		switch (getValue(opv)) {
		case MAX_HEIGHT_SET:
			value = CSS_MAX_HEIGHT_SET;
			length = *((css_fixed *) style->bytecode);
			advance_bytecode(style, sizeof(length));
			unit = *((uint32_t *) style->bytecode);
			advance_bytecode(style, sizeof(unit));
			break;
		case MAX_HEIGHT_NONE:
			value = CSS_MAX_HEIGHT_NONE;
			break;
		case MAX_HEIGHT_CALC:
		{
			uint32_t snum = 0;
			lwc_string *calc_expr = NULL;
			uint8_t slot = css__calc_slot_for_prop(
					getOpcode(opv));

			value = CSS_MAX_HEIGHT_SET;
			advance_bytecode(style, sizeof(unit));
			snum = *((uint32_t *) style->bytecode);
			advance_bytecode(style, sizeof(snum));
			css__stylesheet_string_get(style->sheet, snum, &calc_expr);
			/* fixes1159b: store the expression in the style's
			 * per-property side slot and put the SLOT NUMBER in
			 * the length field. Bit-casting the lwc_string
			 * pointer into css_fixed only fits on 32-bit targets
			 * (truncates to a garbage pointer on Linux 64-bit,
			 * misresolved to 0 on the Mac). */
			if (calc_expr != NULL &&
					slot < MACSURF_CALC_SLOT_COUNT) {
				state->computed->macsurf_calc_expr[slot] =
						calc_expr;
				length = (css_fixed)slot;
				is_calc = true;
			}
			break;
		}
		default:
			assert(0 && "Invalid value");
			break;
		}
	}

	if (is_calc == false) {
		unit = css__to_css_unit(unit);
	} else {
		unit = CSS_UNIT_CALC;
	}

	if (css__outranks_existing(getOpcode(opv), isImportant(opv), state,
			getFlagValue(opv))) {
		return fun(state->computed, value, length, unit);
	}

	return CSS_OK;
}

css_error css__cascade_length(uint32_t opv, css_style *style,
		css_select_state *state,
		css_error (*fun)(css_computed_style *, uint8_t, css_fixed,
				css_unit))
{
	uint16_t value = CSS_MIN_HEIGHT_INHERIT;
	css_fixed length = 0;
	uint32_t unit = UNIT_PX;
	bool is_calc = false;

	if (hasFlagValue(opv) == false) {
		switch (getValue(opv)) {
		case MIN_HEIGHT_SET:
			value = CSS_MIN_HEIGHT_SET;
			length = *((css_fixed *) style->bytecode);
			advance_bytecode(style, sizeof(length));
			unit = *((uint32_t *) style->bytecode);
			advance_bytecode(style, sizeof(unit));
			break;
		case MIN_HEIGHT_CALC:
		{
			uint32_t snum = 0;
			lwc_string *calc_expr = NULL;
			uint8_t slot = css__calc_slot_for_prop(
					getOpcode(opv));

			value = CSS_MIN_HEIGHT_SET;
			advance_bytecode(style, sizeof(unit));
			snum = *((uint32_t *) style->bytecode);
			advance_bytecode(style, sizeof(snum));
			css__stylesheet_string_get(style->sheet, snum, &calc_expr);
			/* fixes1159b: store the expression in the style's
			 * per-property side slot and put the SLOT NUMBER in
			 * the length field. Bit-casting the lwc_string
			 * pointer into css_fixed only fits on 32-bit targets
			 * (truncates to a garbage pointer on Linux 64-bit,
			 * misresolved to 0 on the Mac). */
			if (calc_expr != NULL &&
					slot < MACSURF_CALC_SLOT_COUNT) {
				state->computed->macsurf_calc_expr[slot] =
						calc_expr;
				length = (css_fixed)slot;
				is_calc = true;
			}
			break;
		}
		default:
			assert(0 && "Invalid value");
			break;
		}
	}

	if (is_calc == false) {
		unit = css__to_css_unit(unit);
	} else {
		unit = CSS_UNIT_CALC;
	}

	/** \todo lose fun != NULL once all properties have set routines */
	if (fun != NULL && css__outranks_existing(getOpcode(opv),
			isImportant(opv), state, getFlagValue(opv))) {
		return fun(state->computed, value, length, unit);
	}

	return CSS_OK;
}

css_error css__cascade_number(uint32_t opv, css_style *style,
		css_select_state *state,
		css_error (*fun)(css_computed_style *, uint8_t, css_fixed))
{
	uint16_t value = 0;
	css_fixed length = 0;

	/** \todo values */

	if (hasFlagValue(opv) == false) {
		switch (getValue(opv)) {
		case ORPHANS_SET:
			value = 0;
			length = *((css_fixed *) style->bytecode);
			advance_bytecode(style, sizeof(length));
			break;
		case ORPHANS_CALC:
			advance_bytecode(style, sizeof(unit));
			advance_bytecode(style, sizeof(unit)); /* TODO */
			return CSS_OK;
		default:
			assert(0 && "Invalid value");
			break;
		}
	}

	/** \todo lose fun != NULL once all properties have set routines */
	if (fun != NULL && css__outranks_existing(getOpcode(opv),
			isImportant(opv), state, getFlagValue(opv))) {
		return fun(state->computed, value, length);
	}

	return CSS_OK;
}

css_error css__cascade_page_break_after_before_inside(uint32_t opv,
		css_style *style, css_select_state *state,
		css_error (*fun)(css_computed_style *, uint8_t))
{
	uint16_t value = CSS_PAGE_BREAK_AFTER_INHERIT;

	UNUSED(style);

	if (hasFlagValue(opv) == false) {
		switch (getValue(opv)) {
		case PAGE_BREAK_AFTER_AUTO:
			value = CSS_PAGE_BREAK_AFTER_AUTO;
			break;
		case PAGE_BREAK_AFTER_ALWAYS:
			value = CSS_PAGE_BREAK_AFTER_ALWAYS;
			break;
		case PAGE_BREAK_AFTER_AVOID:
			value = CSS_PAGE_BREAK_AFTER_AVOID;
			break;
		case PAGE_BREAK_AFTER_LEFT:
			value = CSS_PAGE_BREAK_AFTER_LEFT;
			break;
		case PAGE_BREAK_AFTER_RIGHT:
			value = CSS_PAGE_BREAK_AFTER_RIGHT;
			break;
		}
	}

	/** \todo lose fun != NULL */
	if (fun != NULL && css__outranks_existing(getOpcode(opv),
			isImportant(opv), state, getFlagValue(opv))) {
		return fun(state->computed, value);
	}

	return CSS_OK;
}

css_error css__cascade_break_after_before_inside(uint32_t opv,
		css_style *style, css_select_state *state,
		css_error (*fun)(css_computed_style *, uint8_t))
{
	uint16_t value = CSS_BREAK_AFTER_AUTO;

	UNUSED(style);

	if (hasFlagValue(opv) == false) {
		switch (getValue(opv)) {
		case BREAK_AFTER_AUTO:
			value = CSS_BREAK_AFTER_AUTO;
			break;
		case BREAK_AFTER_ALWAYS:
			value = CSS_BREAK_AFTER_ALWAYS;
			break;
		case BREAK_AFTER_AVOID:
			value = CSS_BREAK_AFTER_AVOID;
			break;
		case BREAK_AFTER_LEFT:
			value = CSS_BREAK_AFTER_LEFT;
			break;
		case BREAK_AFTER_RIGHT:
			value = CSS_BREAK_AFTER_RIGHT;
			break;
		case BREAK_AFTER_PAGE:
			value = CSS_BREAK_AFTER_PAGE;
			break;
		case BREAK_AFTER_COLUMN:
			value = CSS_BREAK_AFTER_COLUMN;
			break;
		case BREAK_AFTER_AVOID_PAGE:
			value = CSS_BREAK_AFTER_AVOID_PAGE;
			break;
		case BREAK_AFTER_AVOID_COLUMN:
			value = CSS_BREAK_AFTER_AVOID_COLUMN;
			break;
		}
	}

	/** \todo lose fun != NULL */
	if (fun != NULL && css__outranks_existing(getOpcode(opv),
			isImportant(opv), state, getFlagValue(opv))) {
		return fun(state->computed, value);
	}

	return CSS_OK;
}

css_error css__cascade_counter_increment_reset(uint32_t opv, css_style *style,
		css_select_state *state,
		css_error (*fun)(css_computed_style *, uint8_t,
				css_computed_counter *))
{
	uint16_t value = CSS_COUNTER_INCREMENT_INHERIT;
	css_computed_counter *counters = NULL;
	uint32_t n_counters = 0;

	if (hasFlagValue(opv) == false) {
		switch (getValue(opv)) {
		case COUNTER_INCREMENT_NAMED:
		{
			uint32_t v = getValue(opv);

			while (v != COUNTER_INCREMENT_NONE) {
				css_computed_counter *temp;
				lwc_string *name;
				css_fixed val = 0;

				css__stylesheet_string_get(style->sheet, *((css_code_t *) style->bytecode), &name);
				advance_bytecode(style, sizeof(css_code_t));

				val = *((css_fixed *) style->bytecode);
				advance_bytecode(style, sizeof(css_code_t));

				temp = realloc(counters,
						(n_counters + 1) *
						sizeof(css_computed_counter));
				if (temp == NULL) {
					if (counters != NULL) {
						free(counters);
					}
					return CSS_NOMEM;
				}

				counters = temp;

				counters[n_counters].name = name;
				counters[n_counters].value = val;

				n_counters++;

				v = *((uint32_t *) style->bytecode);
				advance_bytecode(style, sizeof(css_code_t));
			}

			/* fixes134d: upstream libcss bug. The NAMED branch
			 * builds the counters array but never sets `value`,
			 * leaving it at the default CSS_COUNTER_INCREMENT_
			 * INHERIT (0). The setter then writes bit=0 to the
			 * 1-bit type field even though counter_arr is non-
			 * NULL. The compose pass at counter_increment.c:85
			 * (and counter_reset.c:85) reads the bit, sees
			 * INHERIT, copies from PARENT instead of child --
			 * and parent has the initial (NULL) array. The
			 * counter list the cascade just built is discarded.
			 * One-line fix: declare the bit NAMED. */
			value = CSS_COUNTER_INCREMENT_NAMED;
		}
			break;
		case COUNTER_INCREMENT_NONE:
			value = CSS_COUNTER_INCREMENT_NONE;
			break;
		}
	}

	/* If we have some counters, terminate the array with a blank entry */
	if (n_counters > 0) {
		css_computed_counter *temp;

		temp = realloc(counters, (n_counters + 1) *
				sizeof(css_computed_counter));
		if (temp == NULL) {
			free(counters);
			return CSS_NOMEM;
		}

		counters = temp;

		counters[n_counters].name = NULL;
		counters[n_counters].value = 0;
	}

	if (css__outranks_existing(getOpcode(opv), isImportant(opv), state,
			getFlagValue(opv))) {
		css_error error;

		error = fun(state->computed, value, counters);
		if (error != CSS_OK && n_counters > 0)
			free(counters);

		return error;
	} else if (n_counters > 0) {
		free(counters);
	}

	return CSS_OK;
}

