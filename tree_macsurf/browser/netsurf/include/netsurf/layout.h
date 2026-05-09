/*
 * Copyright 2016 Vincent Sanders <vince@netsurf-browser.org>
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
 *
 * Interface to platform-specific layout operation table.
 *
 * This table is part of the layout used to measure glyphs before
 * rendering, previously referred to as font functions.
 *
 * \note This is an old interface within the browser, it has been
 * broken out purely to make the API obvious not as an indication this
 * is the correct approach.
 */

#ifndef _NETSURF_LAYOUT_H_
#define _NETSURF_LAYOUT_H_

struct plot_font_style;

struct gui_layout_table
{
	/**
	 * Measure the width of a string.
	 *
	 * \param[in] fstyle plot style for this text
	 * \param[in] string UTF-8 string to measure
	 * \param[in] length length of string, in bytes
	 * \param[out] width updated to width of string[0..length)
	 * \return NSERROR_OK and width updated or appropriate error
	 *          code on faliure
	 */
	nserror (*width)(const struct plot_font_style *fstyle, const char *string, size_t length, int *width);


	/**
	 * Find the position in a string where an x coordinate falls.
	 *
	 * \param[in] fstyle style for this text
	 * \param[in] string UTF-8 string to measure
	 * \param[in] length length of string, in bytes
	 * \param[in] x coordinate to search for
	 * \param[out] char_offset updated to offset in string of actual_x, [0..length]
	 * \param[out] actual_x updated to x coordinate of character closest to x
	 * \return NSERROR_OK and char_offset and actual_x updated or appropriate error code on faliure
	 */
	nserror (*position)(const struct plot_font_style *fstyle, const char *string, size_t length, int x, size_t *char_offset, int *actual_x);


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
	 *     char_offset giving split point closest to x, where actual_x <= x
	 *   else
	 *     char_offset giving split point closest to x, where actual_x > x
	 *
	 * Returning char_offset == length means no split possible
	 */
	nserror (*split)(const struct plot_font_style *fstyle, const char *string, size_t length, int x, size_t *char_offset, int *actual_x);
};

#endif
