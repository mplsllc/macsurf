/*
 * Copyright 2020 Michael Drake <tlsa@netsurf-browser.org>
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

/**
 * \file
 * NetSurf UI colours (implementation).
 *
 * Builds common colours used throughout NetSurf's interface.
 */

#include <stdio.h>
#include <assert.h>
#include <stddef.h>
#include <stdbool.h>

#include "netsurf/inttypes.h"
#include "netsurf/plot_style.h"

#include "utils/errors.h"
#include "utils/nscolour.h"
#include "desktop/system_colour.h"

colour nscolours[NSCOLOUR__COUNT];

/**
 * Set some colours up from a couple of system colour entries.
 *
 * \param[in]  name_bg    Name of choices string for background colour lookup.
 * \param[in]  name_fg    Name of choices string for foreground colour lookup.
 * \param[in]  bg_num     Numerator for background adjustment ratio.
 * \param[in]  bg_den     Denominator for backfground adjustment ratio.
 * \param[out] bg         Returns the background colour.
 * \param[out] bg_hover   Returns the hovered background colour.
 * \param[out] fg         Returns the foreground colour.
 * \param[out] fg_subtle  Returns the subtle foreground colour.
 * \param[out] fg_faded   Returns the faded foreground colour.
 * \param[out] fg_good    Returns the good foreground colour.
 * \param[out] fg_bad     Returns the bad foreground colour.
 * \param[out] border     Returns the border colour.
 */
static nserror nscolour__get(
		const char *name_bg,
		const char *name_fg,
		unsigned bg_num,
		unsigned bg_den,
		colour *bg,
		colour *bg_hover,
		colour *fg,
		colour *fg_subtle,
		colour *fg_faded,
		colour *fg_good,
		colour *fg_bad,
		colour *border)
{
	nserror res;
	bool dark_mode;
	colour bg_sys;

	assert(name_bg != NULL);
	assert(name_fg != NULL);
	assert(bg != NULL);
	assert(fg != NULL);

	/* user configured background colour */
	res = ns_system_colour_char(name_bg, &bg_sys);
	if (res != NSERROR_OK) {
		return res;
	}

	/* user configured foreground colour */
	res = ns_system_colour_char(name_fg, fg);
	if (res != NSERROR_OK) {
		return res;
	}

	/* if there is a valid background fraction apply it */
	if (bg_num < bg_den) {
		*bg = mix_colour(bg_sys, *fg, 255 * bg_num / bg_den);
	} else {
		*bg = bg_sys;
	}

	dark_mode = colour_lightness(*fg) > colour_lightness(*bg);

	if (bg_hover != NULL) {
		*bg_hover = dark_mode ?
				half_lighten_colour(*bg) :
				half_darken_colour(*bg);
	}

	if (fg_subtle != NULL) {
		*fg_subtle = mix_colour(*fg, *bg, 255 * 25 / 32);
	}

	if (fg_faded != NULL) {
		*fg_faded = mix_colour(*fg, *bg, 255 * 20 / 32);
	}

	if (fg_good != NULL) {
		*fg_good = colour_engorge_component(*fg, !dark_mode,
				PLOT_COLOUR_COMPONENT_GREEN);
	}

	if (fg_bad != NULL) {
		*fg_bad = colour_engorge_component(*fg, !dark_mode,
				PLOT_COLOUR_COMPONENT_RED);
	}

	if (border != NULL) {
		*border = mix_colour(*fg, bg_sys, 255 * 8 / 32);
	}

	return NSERROR_OK;
}

/* Exported interface, documented in utils/nscolour.h */
nserror nscolour_update(void)
{
	nserror res;

	res = nscolour__get("Canvas", "CanvasText", 16, 16,
			&nscolours[NSCOLOUR_WIN_EVEN_BG],
			&nscolours[NSCOLOUR_WIN_EVEN_BG_HOVER],
			&nscolours[NSCOLOUR_WIN_EVEN_FG],
			&nscolours[NSCOLOUR_WIN_EVEN_FG_SUBTLE],
			&nscolours[NSCOLOUR_WIN_EVEN_FG_FADED],
			&nscolours[NSCOLOUR_WIN_EVEN_FG_GOOD],
			&nscolours[NSCOLOUR_WIN_EVEN_FG_BAD],
			&nscolours[NSCOLOUR_WIN_EVEN_BORDER]);
	if (res != NSERROR_OK) {
		return res;
	}

	res = nscolour__get("Canvas", "CanvasText", 15, 16,
			&nscolours[NSCOLOUR_WIN_ODD_BG],
			&nscolours[NSCOLOUR_WIN_ODD_BG_HOVER],
			&nscolours[NSCOLOUR_WIN_ODD_FG],
			&nscolours[NSCOLOUR_WIN_ODD_FG_SUBTLE],
			&nscolours[NSCOLOUR_WIN_ODD_FG_FADED],
			&nscolours[NSCOLOUR_WIN_ODD_FG_GOOD],
			&nscolours[NSCOLOUR_WIN_ODD_FG_BAD],
			&nscolours[NSCOLOUR_WIN_ODD_BORDER]);
	if (res != NSERROR_OK) {
		return res;
	}

	res = nscolour__get("Highlight", "HighlightText", 16, 16,
			&nscolours[NSCOLOUR_SEL_BG],
			NULL,
			&nscolours[NSCOLOUR_SEL_FG],
			&nscolours[NSCOLOUR_SEL_FG_SUBTLE],
			NULL,
			NULL,
			NULL,
			NULL);
	if (res != NSERROR_OK) {
		return res;
	}

	res = ns_system_colour_char("Canvas",
			&nscolours[NSCOLOUR_SCROLL_WELL]);
	if (res != NSERROR_OK) {
		return res;
	}

	res = nscolour__get("ButtonFace", "ButtonText", 16, 16,
			&nscolours[NSCOLOUR_BUTTON_BG],
			NULL,
			&nscolours[NSCOLOUR_BUTTON_FG],
			NULL,
			NULL,
			NULL,
			NULL,
			NULL);
	if (res != NSERROR_OK) {
		return res;
	}

	nscolours[NSCOLOUR_TEXT_INPUT_BG] =
			colour_to_bw_nearest(nscolours[NSCOLOUR_WIN_EVEN_BG]);
	nscolours[NSCOLOUR_TEXT_INPUT_FG] =
			colour_to_bw_nearest(nscolours[NSCOLOUR_WIN_EVEN_FG]);
	nscolours[NSCOLOUR_TEXT_INPUT_FG_SUBTLE] =
			blend_colour(nscolours[NSCOLOUR_TEXT_INPUT_BG],
			             nscolours[NSCOLOUR_TEXT_INPUT_FG]);

	return NSERROR_OK;
}

/* Exported interface, documented in utils/nscolour.h */
nserror nscolour_get_stylesheet(const char **stylesheet_out)
{
	static char buffer[640];
	int ret;

	assert(stylesheet_out != NULL);

	ret = snprintf(buffer, sizeof(buffer),
			".ns-odd-bg {\n"
			"\tbackground-color: #%06"PRIx32";\n"
			"}\n"
			".ns-odd-bg-hover {\n"
			"\tbackground-color: #%06"PRIx32";\n"
			"}\n"
			".ns-odd-fg {\n"
			"\tcolor: #%06"PRIx32";\n"
			"}\n"
			".ns-odd-fg-subtle {\n"
			"\tcolor: #%06"PRIx32";\n"
			"}\n"
			".ns-odd-fg-faded {\n"
			"\tcolor: #%06"PRIx32";\n"
			"}\n"
			".ns-odd-fg-good {\n"
			"\tcolor: #%06"PRIx32";\n"
			"}\n"
			".ns-odd-fg-bad {\n"
			"\tcolor: #%06"PRIx32";\n"
			"}\n"
			".ns-even-bg {\n"
			"\tbackground-color: #%06"PRIx32";\n"
			"}\n"
			".ns-even-bg-hover {\n"
			"\tbackground-color: #%06"PRIx32";\n"
			"}\n"
			".ns-even-fg {\n"
			"\tcolor: #%06"PRIx32";\n"
			"}\n"
			".ns-even-fg-subtle {\n"
			"\tcolor: #%06"PRIx32";\n"
			"}\n"
			".ns-even-fg-faded {\n"
			"\tcolor: #%06"PRIx32";\n"
			"}\n"
			".ns-even-fg-good {\n"
			"\tcolor: #%06"PRIx32";\n"
			"}\n"
			".ns-even-fg-bad {\n"
			"\tcolor: #%06"PRIx32";\n"
			"}\n"
			".ns-border {\n"
			"\tborder-color: #%06"PRIx32";\n"
			"}\n",
			colour_rb_swap(nscolours[NSCOLOUR_WIN_ODD_BG]),
			colour_rb_swap(nscolours[NSCOLOUR_WIN_ODD_BG_HOVER]),
			colour_rb_swap(nscolours[NSCOLOUR_WIN_ODD_FG]),
			colour_rb_swap(nscolours[NSCOLOUR_WIN_ODD_FG_SUBTLE]),
			colour_rb_swap(nscolours[NSCOLOUR_WIN_ODD_FG_FADED]),
			colour_rb_swap(nscolours[NSCOLOUR_WIN_ODD_FG_GOOD]),
			colour_rb_swap(nscolours[NSCOLOUR_WIN_ODD_FG_BAD]),
			colour_rb_swap(nscolours[NSCOLOUR_WIN_EVEN_BG]),
			colour_rb_swap(nscolours[NSCOLOUR_WIN_EVEN_BG_HOVER]),
			colour_rb_swap(nscolours[NSCOLOUR_WIN_EVEN_FG]),
			colour_rb_swap(nscolours[NSCOLOUR_WIN_EVEN_FG_SUBTLE]),
			colour_rb_swap(nscolours[NSCOLOUR_WIN_EVEN_FG_FADED]),
			colour_rb_swap(nscolours[NSCOLOUR_WIN_EVEN_FG_GOOD]),
			colour_rb_swap(nscolours[NSCOLOUR_WIN_EVEN_FG_BAD]),
			colour_rb_swap(nscolours[NSCOLOUR_WIN_EVEN_BORDER]));
	assert(ret > 0 && (size_t)ret < sizeof(buffer));
	if (ret < 0 || (size_t)ret >= sizeof(buffer)) {
		/* Error or buffer too small */
		return NSERROR_NOSPACE;
	}

	*stylesheet_out = buffer;
	return NSERROR_OK;
}
