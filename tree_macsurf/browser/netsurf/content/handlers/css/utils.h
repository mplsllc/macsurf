/*
 * Copyright 2009 John-Mark Bell <jmb@netsurf-browser.org>
 *
 * This file is part of NetSurf, http://www.netsurf-browser.org/
 *
 * NetSurf is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * NetSurf is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef NETSURF_CSS_UTILS_H_
#define NETSURF_CSS_UTILS_H_

#include <libcss/libcss.h>

#include "netsurf/css.h"

/** DPI of the screen, in fixed point units */
extern css_fixed nscss_screen_dpi;

/**
 * Temporary helper wrappers for for libcss computed style getter, while
 * we don't support all values of display.
 */
static inline uint8_t ns_computed_display(
		const css_computed_style *style, bool root)
{
	uint8_t value = css_computed_display(style, root);

	switch (value) {
	case CSS_DISPLAY_GRID:
		return CSS_DISPLAY_BLOCK;

	case CSS_DISPLAY_INLINE_GRID:
		return CSS_DISPLAY_INLINE_BLOCK;

	default:
		break;
	}

	return value;
}

/**
 * Temporary helper wrappers for for libcss computed style getter, while
 * we don't support all values of display.
 */
static inline uint8_t ns_computed_display_static(
		const css_computed_style *style)
{
	uint8_t value = css_computed_display_static(style);

	switch (value) {
	case CSS_DISPLAY_GRID:
		return CSS_DISPLAY_BLOCK;

	case CSS_DISPLAY_INLINE_GRID:
		return CSS_DISPLAY_INLINE_BLOCK;

	default:
		break;
	}

	return value;
}

static inline uint8_t ns_computed_min_height(
		const css_computed_style *style,
		css_fixed *length, css_unit *unit)
{
	uint8_t value = css_computed_min_height(style, length, unit);

	if (value == CSS_MIN_HEIGHT_AUTO) {
		value = CSS_MIN_HEIGHT_SET;
		*length = 0;
		*unit = CSS_UNIT_PX;
	}

	return value;
}


static inline uint8_t ns_computed_min_width(
		const css_computed_style *style,
		css_fixed *length, css_unit *unit)
{
	uint8_t value = css_computed_min_width(style, length, unit);

	if (value == CSS_MIN_WIDTH_AUTO) {
		value = CSS_MIN_WIDTH_SET;
		*length = 0;
		*unit = CSS_UNIT_PX;
	}

	return value;
}

#endif
