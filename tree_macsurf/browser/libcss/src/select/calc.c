/*
 * This file is part of LibCSS.
 * Licensed under the MIT License,
 *                http://www.opensource.org/licenses/mit-license.php
 * Copyright 2024 Daniel Silverstone <dsilvers@netsurf-browser.org>
 */

#include "libcss/hint.h"

#include "select/calc.h"
#include "select/helpers.h"
#include "select/unit.h"

#include "utils/utils.h"

/** The default number of entries on a calculator stack */
#define DEFAULT_STACK_SIZE 32

#ifndef NDEBUG
#define CANARY "A css_calculator has leaked"
#define CANARY_SLEN SLEN(CANARY)
#endif

/****************************** Allocation **********************************/

/* Exported function, documented in calc.h */
css_error css_calculator_create(css_calculator **out)
{
	*out = calloc(1, sizeof(css_calculator));
	if (*out == NULL) {
		return CSS_NOMEM;
	}

#ifndef NDEBUG
	if (lwc_intern_string(CANARY, CANARY_SLEN, &((*out)->canary)) !=
	    lwc_error_ok) {
		free(*out);
		*out = NULL;
		return CSS_NOMEM;
	}
#endif

	(*out)->stack =
	    calloc(DEFAULT_STACK_SIZE, sizeof(css_calculator_stack_entry));
	if ((*out)->stack == NULL) {
#ifndef NDEBUG
		lwc_string_unref((*out)->canary);
#endif
		free(*out);
		*out = NULL;
		return CSS_NOMEM;
	}

	(*out)->refcount = 1;
	(*out)->stack_alloc = DEFAULT_STACK_SIZE;
	(*out)->stack_ptr = 0;

	return CSS_OK;
}

/* Exported function, documented in calc.h */
css_calculator *css_calculator_ref(css_calculator *calc)
{
	calc->refcount += 1;
	return calc;
}

/* Exported function, documented in calc.h */
void css_calculator_unref(css_calculator *calc)
{
	calc->refcount -= 1;
	if (calc->refcount == 0) {
#ifndef NDEBUG
		lwc_string_unref(calc->canary);
#endif
		free(calc->stack);
		free(calc);
	}
}

/****************************** Helpers ************************************/

static css_error css__calculator_push(css_calculator *calc, unit unit,
				      css_fixed value)
{
	if (calc->stack_ptr == calc->stack_alloc) {
		css_calculator_stack_entry *newstack =
		    realloc(calc->stack, sizeof(css_calculator_stack_entry) *
					     2 * calc->stack_alloc);
		if (newstack == NULL) {
			return CSS_NOMEM;
		}
		calc->stack = newstack;
		calc->stack_alloc *= 2;
	}
	calc->stack[calc->stack_ptr].unit = unit;
	calc->stack[calc->stack_ptr].value = value;
	calc->stack_ptr += 1;

	return CSS_OK;
}

static css_error css__calculator_pop(css_calculator *calc, unit *unit,
				     css_fixed *value)
{
	if (calc->stack_ptr == 0) {
		return CSS_INVALID;
	}

	calc->stack_ptr -= 1;
	*unit = calc->stack[calc->stack_ptr].unit;
	*value = calc->stack[calc->stack_ptr].value;

	return CSS_OK;
}

#define CALC_PUSH(unit, value)                                                 \
	do {                                                                   \
		ret = css__calculator_push(calc, unit, value);                 \
		if (ret != CSS_OK) {                                           \
			return ret;                                            \
		}                                                              \
	} while (0)

#define CALC_POP(unit, value)                                                  \
	do {                                                                   \
		ret = css__calculator_pop(calc, &unit, &value);                \
		if (ret != CSS_OK) {                                           \
			return ret;                                            \
		}                                                              \
	} while (0)

/* Normalise:
 *  - lengths to pixels
 *  - angles to degrees
 *  - time to ms
 *  - freq to hz
 *  - resolution to dpi
 */
static css_error css__normalise_unit(const css_unit_ctx *unit_ctx,
				     const css_computed_style *style,
				     int32_t available, unit *u, css_fixed *v)
{
	if (*u & UNIT_LENGTH) {
		css_fixed px = css_unit_len2css_px(style, unit_ctx, *v,
						   css__to_css_unit(*u));
		*v = px;
		*u = UNIT_PX;
		return CSS_OK;
	} else if (*u & UNIT_ANGLE) {
		css_fixed deg = css_unit_angle2deg(css__to_css_unit(*u), *v);
		*v = deg;
		*u = UNIT_DEG;
		return CSS_OK;
	} else if (*u & UNIT_TIME) {
		if (*u == UNIT_S) {
			*v = css_multiply_fixed(*v, INTTOFIX(1000));
		}
		*u = UNIT_MS;
		return CSS_OK;
	} else if (*u & UNIT_FREQ) {
		if (*u == UNIT_KHZ) {
			*v = css_multiply_fixed(*v, INTTOFIX(1000));
		}
		*u = UNIT_HZ;
		return CSS_OK;
	} else if (*u & UNIT_RESOLUTION) {
		if (*u == UNIT_DPCM) {
			*v = css_multiply_fixed(*v, FLTTOFIX(2.54));
		} else if (*u == UNIT_DPPX) {
			*v = css_multiply_fixed(*v, INTTOFIX(96));
		}
		*u = UNIT_DPI;
		return CSS_OK;
	} else if (*u == UNIT_PCT) {
		css_fixed pct100 = css_unit_device2css_px(INTTOFIX(available),
							  unit_ctx->device_dpi);
		if (available < 0) {
			return CSS_INVALID;
		}
		*v = css_multiply_fixed(*v, pct100);
		*v = css_divide_fixed(*v, F_100);
		*u = UNIT_PX;
		return CSS_OK;
	} else if (*u == UNIT_CALC_NUMBER) {
		/* Nothing to do to normalise numbers */
		return CSS_OK;
	}
	return CSS_INVALID;
}

/****************************** Compute ************************************/

/* Exported function, documented in calc.h */
css_error css_calculator_calculate(css_calculator *calc,
				   const css_unit_ctx *unit_ctx,
				   int32_t available, lwc_string *expr,
				   const css_computed_style *style,
				   css_unit *unit_out, css_fixed *value_out)
{
	css_error ret = CSS_OK;
	/* Alignment note: lwc string data is always very well aligned */
	css_code_t *codeptr = (css_code_t *)(void *)lwc_string_data(expr);

	/* Reset the stack before we begin, just in case */
	calc->stack_ptr = 0;

	/* We are trusting that the bytecode is sane */
	while (*codeptr != CALC_FINISH) {
		css_code_t op = *codeptr++;
		switch (op) {
		case CALC_PUSH_VALUE: {
			css_fixed v = (css_fixed)(*codeptr++);
			unit u = (unit)(*codeptr++);
			ret = css__normalise_unit(unit_ctx, style, available,
						  &u, &v);
			if (ret != CSS_OK) {
				return ret;
			}
			CALC_PUSH(u, v);
			break;
		}
		case CALC_PUSH_NUMBER: {
			css_fixed v = (css_fixed)(*codeptr++);
			CALC_PUSH(UNIT_CALC_NUMBER, v);
			break;
		}
		case CALC_ADD: /* fallthrough */
		case CALC_SUBTRACT: {
			unit u_left, u_right;
			css_fixed v_left, v_right;
			CALC_POP(u_right, v_right);
			CALC_POP(u_left, v_left);
			if (op == CALC_ADD) {
				v_left = css_add_fixed(v_left, v_right);
			} else {
				v_left = css_subtract_fixed(v_left, v_right);
			}
			CALC_PUSH(u_left, v_left);
			break;
		}
		case CALC_MULTIPLY: /* fallthrough */
		case CALC_DIVIDE: {
			unit u_left, u_right;
			css_fixed v_left, v_right;
			CALC_POP(u_right, v_right);
			CALC_POP(u_left, v_left);

			if (op == CALC_MULTIPLY && u_left == UNIT_CALC_NUMBER) {
				unit u_tmp = u_left;
				css_fixed v_tmp = v_left;
				u_left = u_right;
				v_left = v_right;
				u_right = u_tmp;
				v_right = v_tmp;
			}

			if (u_right != UNIT_CALC_NUMBER) {
				return CSS_INVALID;
			}

			if (op == CALC_MULTIPLY) {
				v_left = css_multiply_fixed(v_left, v_right);
			} else {
				v_left = css_divide_fixed(v_left, v_right);
			}

			CALC_PUSH(u_left, v_left);
			break;
		}
		case CALC_FINISH: /* Should not happen */
		default:
			return CSS_INVALID;
		}
	}

	if (calc->stack_ptr != 1) {
		return CSS_INVALID;
	}

	*unit_out = css__to_css_unit(calc->stack[0].unit);
	*value_out = calc->stack[0].value;

	return CSS_OK;
}
