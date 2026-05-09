/*
 * This file is part of LibCSS.
 * Licensed under the MIT License,
 *                http://www.opensource.org/licenses/mit-license.php
 * Copyright 2024 Daniel Silverstone <dsilvers@netsurf-browser.org>
 */

#ifndef css_select_calc_h_
#define css_select_calc_h_

#include <stdint.h>

#include "bytecode/bytecode.h"
#include "libcss/errors.h"
#include "libcss/unit.h"

typedef struct {
	unit unit;
	css_fixed value;
} css_calculator_stack_entry;

typedef struct {
	uint32_t refcount;
	css_calculator_stack_entry *stack;
	size_t stack_alloc;
	size_t stack_ptr;
#ifndef NDEBUG
	lwc_string *canary;
#endif
} css_calculator;

/**
 * Create a new CSS calculator
 *
 * This creates a new CSS calculator with a reference count of one.
 *
 * The caller is responsible for calling \ref css_calculator_unref
 * when it is done with this calculator.
 *
 * \param out The created calculator in saved here
 * \return CSS_OK if no problems were encountered, likely CSS_NOMEM if there
 * were
 */
css_error css_calculator_create(css_calculator **out);

/**
 * Add a ref to a CSS calculator
 *
 * If you are storing a calculator in more than one place you should ref
 * it when storing it into a new place.  You will neeed to call \ref
 * css_calculator_unref to match any call to this.
 *
 * \param calc The calculator to add a reference to
 * \return The same calculator pointer for ease of use
 */
css_calculator *css_calculator_ref(css_calculator *calc);

/**
 * Unref a CSS calculator
 *
 * Every call to \ref css_calculator_create or \ref css_calculator_ref must be
 * matched with a call to this function.  When the last ref is removed then
 * the underlying calculator is freed.
 *
 * \param calc The calculator to unref
 */
void css_calculator_unref(css_calculator *calc);

/**
 * Perform a calculation
 *
 * To resolve the value of a calc() property, call this function.  This will
 * interpret the bytecode in the given expression, with the given display
 * context and available space, and will fill out the computed unit and value.
 *
 * \param calc The calculator to use
 * \param unit_ctx The display context for resolving units
 * \param available The available space (for percentages)
 * \param style The style from which the expression was derived
 * \param expr The expression to compute
 * \param unit_out The computed unit is placed here
 * \param value_out The computed value is placed here
 * \return CSS_OK if computation succeeded, otherwise likely CSS_INVALID or
 * CSS_NOMEM
 */
css_error css_calculator_calculate(css_calculator *calc,
				   const css_unit_ctx *unit_ctx,
				   int32_t available, lwc_string *expr,
				   const css_computed_style *style,
				   css_unit *unit_out, css_fixed *value_out);

#endif /* css_select_calc_h_ */
