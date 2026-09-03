/* SVG fill V1. Inherited; initial value is black. */
#include "bytecode/bytecode.h"
#include "bytecode/opcodes.h"
#include "select/propset.h"
#include "select/propget.h"
#include "utils/utils.h"

#include "select/properties/properties.h"
#include "select/properties/helpers.h"

css_error css__cascade_fill(uint32_t opv, css_style *style,
		css_select_state *state)
{
	int32_t status = CSS_FILL_INHERIT;
	css_color color = 0;

	if (hasFlagValue(opv) == false) {
		switch (getValue(opv)) {
		case FILL_NONE: status = CSS_FILL_NONE; break;
		case FILL_CURRENT_COLOR: status = CSS_FILL_CURRENT_COLOR; break;
		case FILL_SET:
			status = CSS_FILL_COLOR;
			color = *((css_color *)style->bytecode);
			advance_bytecode(style, sizeof(color));
			break;
		}
	}

	if (css__outranks_existing(getOpcode(opv), isImportant(opv), state,
			getFlagValue(opv))) {
		state->computed->i.fill_status = status;
		state->computed->i.fill_color = color;
	}
	return CSS_OK;
}

css_error css__set_fill_from_hint(const css_hint *hint,
		css_computed_style *style)
{
	style->i.fill_status = hint->status;
	style->i.fill_color = hint->data.color;
	return CSS_OK;
}

css_error css__initial_fill(css_select_state *state)
{
	state->computed->i.fill_status = CSS_FILL_COLOR;
	state->computed->i.fill_color = 0xff000000;
	return CSS_OK;
}

css_error css__copy_fill(const css_computed_style *from,
		css_computed_style *to)
{
	if (from != to) {
		to->i.fill_status = from->i.fill_status;
		to->i.fill_color = from->i.fill_color;
	}
	return CSS_OK;
}

css_error css__compose_fill(const css_computed_style *parent,
		const css_computed_style *child, css_computed_style *result)
{
	int32_t status = child->i.fill_status;
	css_color color = child->i.fill_color;
	if (status == CSS_FILL_INHERIT) {
		status = parent->i.fill_status;
		color = parent->i.fill_color;
	}
	result->i.fill_status = status;
	result->i.fill_color = color;
	return CSS_OK;
}

uint32_t destroy_fill(void *bytecode)
{
	(void)bytecode;
	return 0;
}
