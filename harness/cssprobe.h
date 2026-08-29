/*
 * fixes1268a (#167) - libcss private-layout probes for the harness.
 *
 * driver.c cannot include libcss's internal headers: propstrings.h and
 * opcodes.h define enumerators (TOP, LEFT, RIGHT, BOTTOM, CONTENT_NONE,
 * COLUMN_WIDTH_AUTO) that collide with netsurf enums already in scope
 * there. These entry points wrap the private access in a separate
 * translation unit and hand back plain C types.
 *
 * Linux harness only - never compiled into the Mac build.
 */
#ifndef MACSURF_HARNESS_CSSPROBE_H_
#define MACSURF_HARNESS_CSSPROBE_H_

struct css_stylesheet;

/**
 * Collect every RULE-SCOPED definition of custom property `name` in the
 * sheet, descending into @media blocks, in source order.
 *
 * \param sheet  Parsed stylesheet.
 * \param name   Property name WITHOUT leading dashes (e.g. "x").
 * \param vals   Receives the flattened value text of each definition.
 * \param max    Capacity of vals.
 * \return Number of definitions recorded.
 */
int cssprobe_rule_custom_props(struct css_stylesheet *sheet,
		const char *name, char vals[][64], int max);

/**
 * Count definitions of `name` surviving in the LEGACY per-sheet
 * last-write-wins list. Exists so a test can prove the two stores hold
 * different things while 1268a dual-writes.
 */
int cssprobe_sheet_custom_props(struct css_stylesheet *sheet,
		const char *name);

/**
 * fixes1299 (#167) - which calc-expression slot min-height's computed
 * value points at, for testing that a losing calc() candidate never
 * clobbers a winning one's slot (helpers.c split-brain fix). `style` is
 * a css_computed_style* from css_select_style (typed void* here so this
 * header stays includable from driver.c without pulling in
 * libcss/computed.h's opaque-vs-real-struct ambiguity).
 *
 * \param style     The selected computed style to inspect.
 * \param slot_out  Receives the calc slot index if min-height's computed
 *                  unit is CSS_UNIT_CALC.
 * \return true and slot_out filled if min-height is calc-valued; false
 *         (slot_out untouched) otherwise.
 */
bool cssprobe_min_height_calc_slot(void *style, uint8_t *slot_out);

/**
 * fixes1299 (#167) - how many times the given calc slot has been WON
 * (written by a confirmed-winning declaration) this process lifetime,
 * and the specificity of the last such win. Together with
 * cssprobe_min_height_calc_slot, this is the whole regression check:
 * exactly one write, at the expected winning specificity, proves the
 * losing declaration never reached the slot at all -- stronger than
 * inspecting the slot's content, which is compiled calc bytecode, not
 * literal CSS text.
 */
uint32_t cssprobe_calc_slot_write_count(uint8_t slot);
uint32_t cssprobe_calc_slot_write_last_spec(uint8_t slot);

/* Group 2 / Round 2A: Comprehensive CSS Transitions Harness Tests */
bool cssprobe_test_css_transitions(void);

#endif
