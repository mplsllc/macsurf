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
 * Interface to GTK layout handling using pango.
 */

#ifndef _NETSURF_GTK_LAYOUT_PANGO_H_
#define _NETSURF_GTK_LAYOUT_PANGO_H_

#include <stdbool.h>

struct plot_font_style;

extern struct gui_layout_table *nsgtk_layout_table;

nserror nsfont_paint(int x, int y, const char *string, size_t length, const struct plot_font_style *fstyle);

/**
 * Convert a plot style to a PangoFontDescription.
 *
 * \param fstyle plot style for this text
 * \return A new Pango font description
 */
PangoFontDescription *nsfont_style_to_description(const struct plot_font_style *fstyle);

#endif
