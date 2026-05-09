/*
 * Copyright 2005 James Bursa <bursa@users.sourceforge.net>
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
 * GTK implementation of layout handling using pango.
 *
 * Pango is used handle and render fonts.
 */


#include <assert.h>
#include <stdio.h>
#include <gtk/gtk.h>

#include "utils/log.h"
#include "utils/nsoption.h"
#include "netsurf/inttypes.h"
#include "netsurf/layout.h"
#include "netsurf/plot_style.h"

#include "gtk/layout_pango.h"
#include "gtk/plotters.h"

static PangoContext *nsfont_pango_context = NULL;
static PangoLayout *nsfont_pango_layout = NULL;

static inline void nsfont_pango_check(void)
{
	if (nsfont_pango_context == NULL) {
		NSLOG(netsurf, INFO, "Creating nsfont_pango_context.");
		nsfont_pango_context = gdk_pango_context_get();
	}

	if (nsfont_pango_layout == NULL) {
		NSLOG(netsurf, INFO, "Creating nsfont_pango_layout.");
		nsfont_pango_layout = pango_layout_new(nsfont_pango_context);
	}
}

/**
 * Measure the width of a string.
 *
 * \param[in] fstyle plot style for this text
 * \param[in] string UTF-8 string to measure
 * \param[in] length length of string, in bytes
 * \param[out] width updated to width of string[0..length)
 * \return NSERROR_OK and width updated or appropriate error code on faliure
 */
static nserror
nsfont_width(const plot_font_style_t *fstyle,
	     const char *string,
	     size_t length,
	     int *width)
{
	PangoFontDescription *desc;
	PangoRectangle pos;

	if (length == 0) {
		*width = 0;
		return NSERROR_OK;
	}

	nsfont_pango_check();

	desc = nsfont_style_to_description(fstyle);
	pango_layout_set_font_description(nsfont_pango_layout, desc);
	pango_font_description_free(desc);

	pango_layout_set_text(nsfont_pango_layout, string, length);

	pango_layout_index_to_pos(nsfont_pango_layout, length, &pos);
	*width = PANGO_PIXELS(pos.x);

	NSLOG(netsurf, DEEPDEBUG,
	      "fstyle: %p string:\"%.*s\", length: %" PRIsizet ", width: %dpx",
	      fstyle, (int)length, string, length, *width);


	return NSERROR_OK;
}

/**
 * Find the position in a string where an x coordinate falls.
 *
 * \param[in] layout A pango layout with font set
 * \param[in] string UTF-8 string to measure
 * \param[in] length length of string, in bytes
 * \param[in] x coordinate to search for
 * \param[out] string_idx updated to offset in string of actual_x, [0..length]
 * \param[out] actual_x updated to x coordinate of character closest to x or full length if string_idx is 0
 * \return NSERROR_OK and string_idx and actual_x updated or appropriate error code on faliure
 */
static nserror
layout_position(PangoLayout *layout,
		const char *string,
		size_t length,
		int x,
		size_t *string_idx,
		int *actual_x)
{
	int index;
	PangoRectangle pos;

	/* deal with empty string */
	if (length == 0) {
		*string_idx = 0;
		*actual_x = 0;
		return NSERROR_OK;
	}

	x--; /* split x coordinate is exclusive */

	pango_layout_set_text(layout, string, length);

	if (x <= 0) {
		/* deal with negative or zero available width  */
		index = 0;
	} else {
		/* compute index into string */
		if (pango_layout_xy_to_index(layout,
					     x * PANGO_SCALE,
					     0, &index, 0) == FALSE) {
			/* whole string fits */
			index = length;
		}
	}

	*string_idx = index;
	/* if the split is at index 0 return the whole string length */
	if (index == 0) {
		index = length;
	}
	pango_layout_index_to_pos(layout, index, &pos);
	*actual_x = PANGO_PIXELS(pos.x);

	return NSERROR_OK;
}

/**
 * Find the position in a string where an x coordinate falls.
 *
 * \param[in] fstyle style for this text
 * \param[in] string UTF-8 string to measure
 * \param[in] length length of string, in bytes
 * \param[in] x coordinate to search for
 * \param[out] char_offset updated to offset in string of actual_x, [0..length]
 * \param[out] actual_x updated to x coordinate of character closest to x
 * \return NSERROR_OK and char_offset and actual_x updated or appropriate
 *          error code on faliure
 */
static nserror
nsfont_position_in_string(const plot_font_style_t *fstyle,
			  const char *string,
			  size_t length,
			  int x,
			  size_t *char_offset,
			  int *actual_x)
{
	PangoFontDescription *desc;
	nserror res;

	nsfont_pango_check();

	desc = nsfont_style_to_description(fstyle);
	pango_layout_set_font_description(nsfont_pango_layout, desc);
	pango_font_description_free(desc);

	res = layout_position(nsfont_pango_layout,
			      string,
			      length,
			      x,
			      char_offset,
			      actual_x);

	NSLOG(netsurf, DEEPDEBUG,
	      "fstyle: %p string:\"%.*s\", length: %" PRIsizet ", "
	      "search_x: %dpx, offset: %" PRIsizet ", actual_x: %dpx",
	      fstyle, (int)length, string, length, x, *char_offset, *actual_x);
	return res;
}


/**
 * Find where to split a string to make it fit a width.
 *
 * \param[in] fstyle       style for this text
 * \param[in] string       UTF-8 string to measure
 * \param[in] length       length of string, in bytes
 * \param[in] x            width available
 * \param[out] char_offset updated to offset in string of actual_x, [1..length]
 * \param[out] actual_x updated to x coordinate of character closest to x
 * \return NSERROR_OK or appropriate error code on faliure
 *
 * On exit, char_offset indicates first character after split point.
 *
 * \note char_offset of 0 must never be returned.
 *
 *   Returns:
 *     char_offset giving split point closest to x, where actual_x < x
 *   else
 *     char_offset giving split point closest to x, where actual_x >= x
 *
 * Returning char_offset == length means no split possible
 */
static nserror
nsfont_split(const plot_font_style_t *fstyle,
	     const char *string,
	     size_t length,
	     int x,
	     size_t *string_idx,
	     int *actual_x)
{
	nserror res;
	PangoContext *context;
	PangoLayout *layout;
	PangoFontDescription *desc;
	size_t split_len;
	int split_x;
	size_t str_len;
	PangoRectangle pos;

	context = gdk_pango_context_get();
	layout = pango_layout_new(context);

	desc = nsfont_style_to_description(fstyle);
	pango_layout_set_font_description(layout, desc);
	pango_font_description_free(desc);

	res = layout_position(layout,
			      string,
			      length,
			      x,
			      &split_len,
			      &split_x);
	if (res != NSERROR_OK) {
		goto split_done;
	}

	/* deal with being unable to split */
	if ((split_len < 1) || (split_len >= length)) {
		*string_idx = length;
		*actual_x = split_x;
		goto split_done;
	}

	/* if string broke on boundary do not attempt to adjust */
	if (string[split_len] == ' ') {
		*string_idx = split_len;
		*actual_x = split_x;
		goto split_done;
	}

	/* attempt to break string */
	str_len = split_len;

	/* walk backwards through string looking for space to break on */
	while ((string[str_len] != ' ') &&
	       (str_len > 0)) {
		str_len--;
	}

	/* walk forwards through string looking for space if back failed */
	if (str_len == 0) {
		str_len = split_len;
		while ((str_len < length) &&
		       (string[str_len] != ' ')) {
			str_len++;
		}
	}
	/* include breaking character in match */
	if ((str_len < length) && (string[str_len] == ' ')) {
		str_len++;
	}

	*string_idx = str_len;

	pango_layout_index_to_pos(layout, str_len, &pos);
	*actual_x = PANGO_PIXELS(pos.x);

split_done:
	g_object_unref(layout);
	g_object_unref(context);

	NSLOG(netsurf, DEEPDEBUG,
	      "fstyle: %p string:\"%.*s\" / \"%.*s\", length: %" PRIsizet ", "
	      "split_x: %dpx, offset: %" PRIsizet ", actual_x: %dpx",
	      fstyle, (int)(*string_idx), string, (int)(length - *string_idx),
	      string+*string_idx, length, x, *string_idx, *actual_x);
	return res;
}


/**
 * Render a string.
 *
 * \param  x	   x coordinate
 * \param  y	   y coordinate
 * \param  string  UTF-8 string to measure
 * \param  length  length of string
 * \param  fstyle  plot style for this text
 * \return  true on success, false on error and error reported
 */
nserror nsfont_paint(int x, int y, const char *string, size_t length,
		const plot_font_style_t *fstyle)
{
	PangoFontDescription *desc;
	PangoLayoutLine *line;

	if (length == 0)
		return NSERROR_OK;

	nsfont_pango_check();

	desc = nsfont_style_to_description(fstyle);
	pango_layout_set_font_description(nsfont_pango_layout, desc);
	pango_font_description_free(desc);

	pango_layout_set_text(nsfont_pango_layout, string, length);

	line = pango_layout_get_line_readonly(nsfont_pango_layout, 0);
	cairo_move_to(current_cr, x, y);
	nsgtk_set_colour(fstyle->foreground);
	pango_cairo_show_layout_line(current_cr, line);

	return NSERROR_OK;
}


/* exported interface documented in gtk/layout_pango.h */
PangoFontDescription *
nsfont_style_to_description(const plot_font_style_t *fstyle)
{
	unsigned int size;
	PangoFontDescription *desc;
	PangoStyle style = PANGO_STYLE_NORMAL;

	switch (fstyle->family) {
	case PLOT_FONT_FAMILY_SERIF:
		desc = pango_font_description_from_string(nsoption_charp(font_serif));
		break;
	case PLOT_FONT_FAMILY_MONOSPACE:
		desc = pango_font_description_from_string(nsoption_charp(font_mono));
		break;
	case PLOT_FONT_FAMILY_CURSIVE:
		desc = pango_font_description_from_string(nsoption_charp(font_cursive));
		break;
	case PLOT_FONT_FAMILY_FANTASY:
		desc = pango_font_description_from_string(nsoption_charp(font_fantasy));
		break;
	case PLOT_FONT_FAMILY_SANS_SERIF:
	default:
		desc = pango_font_description_from_string(nsoption_charp(font_sans));
		break;
	}

	size = (fstyle->size * PANGO_SCALE) / PLOT_STYLE_SCALE;

	if (fstyle->flags & FONTF_ITALIC)
		style = PANGO_STYLE_ITALIC;
	else if (fstyle->flags & FONTF_OBLIQUE)
		style = PANGO_STYLE_OBLIQUE;

	pango_font_description_set_style(desc, style);

	pango_font_description_set_weight(desc, (PangoWeight) fstyle->weight);

	pango_font_description_set_size(desc, size);

	if (fstyle->flags & FONTF_SMALLCAPS) {
		pango_font_description_set_variant(desc,
				PANGO_VARIANT_SMALL_CAPS);
	} else {
		pango_font_description_set_variant(desc, PANGO_VARIANT_NORMAL);
	}

	return desc;
}

static struct gui_layout_table layout_table = {
	.width = nsfont_width,
	.position = nsfont_position_in_string,
	.split = nsfont_split,
};

struct gui_layout_table *nsgtk_layout_table = &layout_table;
