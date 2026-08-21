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

#endif
