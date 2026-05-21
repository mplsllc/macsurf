/*
 * This file is part of LibCSS
 * Licensed under the MIT License,
 *		  http://www.opensource.org/licenses/mit-license.php
 * Copyright 2026 MacSurf (fixes159)
 *
 * Cascade -macsurf-justify. Stores a packed int32 in the inner _i
 * struct.
 *
 *   bits 0..3 justify_items (0=unset, 1=start, 2=end, 3=center, 4=stretch)
 *   bits 4..7 justify_self  (same encoding; overrides container default)
 *
 * align_items / align_self read directly via standard libcss getters;
 * only the justify axis needs vendor storage.
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

/* fixes159d: bounded log probe so we can see whether libcss is actually
 * invoking the cascade for `-macsurf-justify` at all. If the
 * preprocessor emits but this log never fires, the property-name
 * lookup or dispatch is broken. If both emit AND cascade fire but the
 * field still reads 0, the issue is on the compose / arena side. */
extern void macsurf_debug_log_writef(const char *fmt, ...);
static int macsurf__justify_cascade_count = 0;

css_error css__cascade_macsurf_justify(uint32_t opv,
		css_style *style, css_select_state *state)
{
	int32_t v = 0;
	bool is_set = false;
	int outranks_int = 0;

	if (hasFlagValue(opv) == false) {
		if (getValue(opv) == 0x0080) { /* SET */
			is_set = true;
			v = *((css_fixed *) style->bytecode);
			advance_bytecode(style, sizeof(css_fixed));
			if (v < 0) v = 0;
		}
	}

	outranks_int = css__outranks_existing(getOpcode(opv),
			isImportant(opv), state, getFlagValue(opv)) ? 1 : 0;
	if (outranks_int != 0) {
		state->computed->i.macsurf_justify = is_set ? v : 0;
	}

	if (macsurf__justify_cascade_count < 16) {
		/* fixes159e: only %d/%ld/%s/%p in this logger — %lx/%x in
		 * 159d would have scrambled the va_list too. */
		macsurf_debug_log_writef(
			"JCASC[%d] opv=%ld val=%d flag=%d is_set=%d v=%ld outranks=%d field_after=%ld",
			macsurf__justify_cascade_count,
			(long)opv,
			(int)getValue(opv),
			(int)hasFlagValue(opv),
			(int)is_set,
			(long)v,
			outranks_int,
			(long)state->computed->i.macsurf_justify);
		macsurf__justify_cascade_count++;
	}

	return CSS_OK;
}

css_error css__set_macsurf_justify_from_hint(const css_hint *hint,
		css_computed_style *style)
{
	(void)hint;
	style->i.macsurf_justify = 0;
	return CSS_OK;
}

css_error css__initial_macsurf_justify(css_select_state *state)
{
	state->computed->i.macsurf_justify = 0;
	return CSS_OK;
}

css_error css__copy_macsurf_justify(
		const css_computed_style *from,
		css_computed_style *to)
{
	if (from == to) return CSS_OK;
	to->i.macsurf_justify = from->i.macsurf_justify;
	return CSS_OK;
}

css_error css__compose_macsurf_justify(
		const css_computed_style *parent,
		const css_computed_style *child,
		css_computed_style *result)
{
	/* fixes159: justify-items / justify-self are not inherited.
	 * Child's value wins outright. Layout reads container vs item
	 * fields separately at grid-walk time. */
	(void)parent;
	result->i.macsurf_justify = child->i.macsurf_justify;
	return CSS_OK;
}

uint32_t destroy_macsurf_justify(void *bytecode)
{
	(void)bytecode;
	return 0;
}
