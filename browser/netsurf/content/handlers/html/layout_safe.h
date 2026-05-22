/*
 * Copyright 2026 MacSurf
 *
 * This file is part of NetSurf, http://www.netsurf-browser.org/
 *
 * NetSurf is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * fixes168 — Layout-wide dimension sanitizers.
 *
 * The CSS layout engine carries internal sentinels (AUTO == INT_MIN,
 * unresolved-height markers, etc.) through its data structures. These
 * are valid as algorithm states but they must never reach pixel
 * arithmetic, child-layout calls, min/max clamps, or bbox math —
 * once they do, the layout produces INT_MIN x or w values that crash
 * downstream (defensive clamp, plotters, redraw walker) or paint at
 * a single coordinate.
 *
 * Apple's heavy nested flex/grid/table trees were producing exactly
 * this class of corruption. fixes167 added flex-local sanitizers
 * (flex_safe_dim / flex_safe_fallback_dim); fixes168 promotes that
 * pattern to a shared layout-wide header consumed by block, flex,
 * grid, table.
 *
 * Helpers are static so each TU compiles its own copy — CW8 is happy
 * with this and the bodies are tiny. Anyone touching this file:
 * keep the rules cheap (single comparisons, no allocation) because
 * they fire on every dimension before every layout call.
 */

#ifndef NETSURF_HTML_LAYOUT_SAFE_H
#define NETSURF_HTML_LAYOUT_SAFE_H

#include <limits.h>

#ifndef AUTO
#define AUTO INT_MIN
#endif

/* Any dimension outside ±LAYOUT_SAFE_MAX is treated as garbage. The
 * largest realistic page-coordinate today is well under 200000 px
 * (see CLAUDE.md note on the defensive redraw clamp), so 1e6 gives
 * five orders of magnitude of headroom while keeping us inside int32
 * with room for safe addition. */
#define LAYOUT_SAFE_MAX  1000000

/* Hard cap on children iterated by a single fallback path. Mirrors
 * FLEX_MAX_ITEMS from fixes167; grid uses this too. */
#define LAYOUT_MAX_CHILDREN 512

/**
 * Returns 1 if v cannot be used as a pixel value (AUTO/INT_MIN,
 * absurdly large/negative). Otherwise 0.
 */
static int layout_dim_is_auto_or_bad(int v)
{
	if (v == AUTO) return 1;
	if (v == INT_MIN) return 1;
	if (v < -LAYOUT_SAFE_MAX) return 1;
	if (v > LAYOUT_SAFE_MAX) return 1;
	return 0;
}

/**
 * Sanitize a width before arithmetic. If v is auto/bad, fall back to
 * the containing-block width (also sanitized). If even that is bad,
 * return 0 — zero-width is the only universally-safe fallback for
 * width.
 */
static int layout_dim_sanitize_width(int v, int containing_width)
{
	if (!layout_dim_is_auto_or_bad(v)) return v;
	if (!layout_dim_is_auto_or_bad(containing_width))
		return containing_width;
	return 0;
}

/**
 * Sanitize a height for base-size measurement. If v is auto/bad,
 * return 0. Heights are content-driven during measurement, so a
 * 0-height start is correct; layout will resolve the real value
 * from content.
 */
static int layout_dim_sanitize_height_for_measure(int v)
{
	if (layout_dim_is_auto_or_bad(v)) return 0;
	return v;
}

/**
 * General arithmetic sanitizer. Replace any AUTO/bad value with 0
 * before adding it into a running total. Use this anywhere a
 * sentinel would poison a sum.
 */
static int layout_dim_sanitize_for_arithmetic(int v)
{
	if (layout_dim_is_auto_or_bad(v)) return 0;
	return v;
}

/**
 * Saturating add. Returns the clamped sum, never overflows past
 * ±LAYOUT_SAFE_MAX. AUTO/bad inputs are treated as 0.
 */
static int layout_dim_add_safe(int a, int b)
{
	int sa = layout_dim_sanitize_for_arithmetic(a);
	int sb = layout_dim_sanitize_for_arithmetic(b);
	int s;
	/* Overflow check: if same sign and magnitudes large, clamp. */
	if (sa > 0 && sb > 0 && sa > LAYOUT_SAFE_MAX - sb)
		return LAYOUT_SAFE_MAX;
	if (sa < 0 && sb < 0 && sa < -LAYOUT_SAFE_MAX - sb)
		return -LAYOUT_SAFE_MAX;
	s = sa + sb;
	if (s > LAYOUT_SAFE_MAX) return LAYOUT_SAFE_MAX;
	if (s < -LAYOUT_SAFE_MAX) return -LAYOUT_SAFE_MAX;
	return s;
}

/**
 * Clamp any int dimension into the safe range, replacing AUTO/bad
 * with 0. Use at boundaries where a sentinel must not escape (bbox
 * assignment, paint-rect dispatch).
 */
static int layout_dim_clamp(int v)
{
	if (layout_dim_is_auto_or_bad(v)) return 0;
	return v;
}

/* ============================================================
 * fixes171 — Layout Watchdog
 *
 * Real browsers solve "page X crashes the layout engine" with two
 * pieces of universal infrastructure that don't depend on knowing
 * which specific input triggers the crash:
 *
 *   1. A depth cap on the recursive box-tree walker, so genuinely
 *      runaway recursion (circular tree, malformed nesting, hostile
 *      input) cannot exhaust the stack.
 *
 *   2. An iteration budget on the total layout-function call count
 *      for one html_reformat pass, so genuinely infinite loops
 *      (cycles between layout_block_context / layout_flex /
 *      layout_grid via odd input combinations) cannot hang the
 *      browser.
 *
 * When either cap is exceeded, the offending subtree degrades
 * locally to a zero-height block — same shape fixes167/170's
 * fallbacks use — and a latched `macsurf_layout_aborted` flag is
 * raised so post-layout code can flag the page as degraded if it
 * cares to. Layout continues; the document tail runs; the rest of
 * the page still renders.
 *
 * The watchdog is GLOBAL state. It is reset at the top of
 * html_reformat (macsurf_layout_watchdog_reset). Every layout
 * entry point pairs layout_watchdog_enter() with
 * layout_watchdog_exit(); if enter returns 1 the caller must
 * bail with the zero-height fallback BEFORE calling exit.
 * ============================================================ */

/* CW8 partition is 16MB; deeply-nested modern HTML (Apple, BBC)
 * has been observed in the 60-80 range. 200 leaves comfortable
 * head-room and still catches genuine pathology. */
#define MACSURF_LAYOUT_MAX_DEPTH 200

/* One html_reformat traversal of a typical modern page (mactrove,
 * wikipedia) is in the 5_000-20_000 range. Apple is heavier but
 * realistic measurements top out under 100k. 300_000 is well
 * above that ceiling while still catching infinite cycles. */
#define MACSURF_LAYOUT_MAX_CALLS 300000

/* Globals defined in layout.c. Declared extern here so every
 * layout_*.c file can read/write them without circular includes. */
extern int macsurf_layout_depth;
extern long macsurf_layout_calls;
extern int macsurf_layout_aborted;

/* macsurf_layout_watchdog_reset() and macsurf_layout_breadcrumb()
 * are real functions (defined in layout.c) so they can do I/O and
 * carry mutable state. The inline-static enter/exit helpers below
 * are tiny enough to live in the header. */
extern void macsurf_layout_watchdog_reset(void);
extern void macsurf_layout_breadcrumb(const char *phase, const void *box);

/**
 * Watchdog gate at the entry of every recursive layout function.
 *
 * Returns 1 if either the depth cap or the iteration budget has
 * been exceeded. Callers MUST bail out with the zero-height block
 * fallback in that case (do NOT call layout_watchdog_exit). On a
 * return of 0 the caller has been counted in and must pair with
 * layout_watchdog_exit() before returning.
 */
static int layout_watchdog_enter(const void *box)
{
	(void)box;
	macsurf_layout_calls++;
	if (macsurf_layout_calls > MACSURF_LAYOUT_MAX_CALLS) {
		if (macsurf_layout_aborted == 0) {
			macsurf_layout_aborted = 1;
			/* one-shot log; subsequent trips stay silent
			 * so the log file isn't flooded. */
			macsurf_layout_breadcrumb("WATCHDOG calls", box);
		}
		return 1;
	}
	macsurf_layout_depth++;
	if (macsurf_layout_depth > MACSURF_LAYOUT_MAX_DEPTH) {
		if (macsurf_layout_aborted == 0) {
			macsurf_layout_aborted = 1;
			macsurf_layout_breadcrumb("WATCHDOG depth", box);
		}
		/* don't leave depth elevated — pop ourselves */
		macsurf_layout_depth--;
		return 1;
	}
	return 0;
}

static void layout_watchdog_exit(void)
{
	if (macsurf_layout_depth > 0)
		macsurf_layout_depth--;
}

#endif /* NETSURF_HTML_LAYOUT_SAFE_H */
