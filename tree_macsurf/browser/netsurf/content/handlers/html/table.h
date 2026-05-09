/*
 * Copyright 2005 James Bursa <bursa@users.sourceforge.net>
 * Copyright 2005 Richard Wilson <info@tinct.net>
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
 * Interface to HTML table processing and layout.
 */

#ifndef NETSURF_HTML_TABLE_H
#define NETSURF_HTML_TABLE_H

#include <stdbool.h>

struct box;


/**
 * Determine the column width types for a table.
 *
 * \param unit_len_ctx Length conversion context
 * \param table box of type BOX_TABLE
 * \return true on success, false on memory exhaustion
 *
 * The table->col array is allocated and type and width are filled in for each
 * column.
 */
bool table_calculate_column_types(const css_unit_ctx *unit_len_ctx,	struct box *table);


/**
 * Calculate used values of border-{trbl}-{style,color,width} for table cells.
 *
 * \param unit_len_ctx Length conversion context
 * \param cell Table cell to consider
 *
 * \post \a cell's border array is populated
 */
void table_used_border_for_cell(const css_unit_ctx *unit_len_ctx, struct box *cell);

#endif
