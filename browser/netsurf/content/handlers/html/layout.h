/*
 * Copyright 2003 James Bursa <bursa@users.sourceforge.net>
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
 * interface to HTML layout.
 *
 * The main interface to the layout code is layout_document(), which takes a
 * normalized box tree and assigns coordinates and dimensions to the boxes, and
 * also adds boxes to the tree (eg. when formatting lines of text).
 */

#ifndef NETSURF_HTML_LAYOUT_H
#define NETSURF_HTML_LAYOUT_H

#include <stdbool.h>

struct box;
struct html_content;
struct gui_layout_table;

/**
 * Calculate positions of boxes in a document.
 *
 * \param content content of type CONTENT_HTML
 * \param width available width
 * \param height available height
 * \return true on success, false on memory exhaustion
 */
bool layout_document(struct html_content *content, int width, int height);

/* Collapse fix: reset max_width to UNKNOWN_MAX_WIDTH across the whole box
 * tree so the next minmax pass recomputes intrinsic widths that were frozen
 * (at 0) while images/fonts were still settling. */
void html_minmax_invalidate_tree(struct box *box);

unsigned int layout_multicol_segment_count(const struct box *box);
bool layout_multicol_segment_bounds(const struct box *box,
		unsigned int index, int *top, int *bottom);

#endif
