/*
 * Copyright 2026 MacSurf
 *
 * This file is part of NetSurf, http://www.netsurf-browser.org/
 *
 * NetSurf is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 */

/**
 * \file
 * HTML layout implementation: display: grid (fixes75 V1).
 *
 * V1 grammar (MacSurf-specific): -macsurf-grid: <cols> [<rows>]
 *
 *   cols : integer 1..255 -- number of equal-width columns
 *   rows : integer 0..255 -- optional; 0 = auto-rows (default)
 *
 * V1 layout: children walked in DOM order, placed row-major into N
 * equal-width columns. Row height = max child height in that row.
 * column-gap (shipped at fixes148) applies between columns.
 *
 * Out of V1 scope (deferred to V2+):
 *   - grid-template-columns / grid-template-rows full grammar
 *     (fr units, repeat(), auto, minmax)
 *   - grid-area / grid-row / grid-column explicit placement
 *   - grid-auto-flow column/dense
 *   - row-gap independent of column-gap
 *   - subgrid
 *   - align-items / justify-items per-cell alignment
 *
 * This V1 covers the most common real-world pattern of "N equal columns,
 * auto rows, children in DOM order". Pages that say `display: grid;
 * grid-template-columns: repeat(3, 1fr)` can use
 * `-macsurf-grid: 3` to opt into the MacSurf grid layout.
 */

#include <string.h>

#include "utils/log.h"
#include "utils/utils.h"

#include "html/box.h"
#include "html/html.h"
#include "html/private.h"
#include "html/layout_internal.h"

#include "css/utils.h"


/**
 * Lay out a single grid item into a cell of given width. Mirrors the
 * dispatch in layout_flex_item but for grid children. The item's height
 * is determined by its own content/layout.
 */
static bool layout_grid_item(
		struct box *item,
		int cell_width,
		html_content *content)
{
	bool success = false;

	item->width = cell_width;

	switch (item->type) {
	case BOX_BLOCK:
	case BOX_INLINE_BLOCK:
		success = layout_block_context(item, -1, content);
		break;
	case BOX_TABLE:
		item->float_container = item->parent;
		success = layout_table(item, cell_width, content);
		item->float_container = NULL;
		break;
	case BOX_FLEX:
	case BOX_INLINE_FLEX:
		item->float_container = item->parent;
		success = layout_flex(item, cell_width, content);
		item->float_container = NULL;
		break;
	case BOX_GRID:
	case BOX_INLINE_GRID:
		item->float_container = item->parent;
		success = layout_grid(item, cell_width, content);
		item->float_container = NULL;
		break;
	default:
		/* Unknown / unsupported item type — treat as zero-size box
		 * rather than failing the whole grid. */
		item->height = 0;
		success = true;
		break;
	}

	return success;
}


bool layout_grid(struct box *grid, int available_width, html_content *content)
{
	int32_t packed = 0;
	int cols = 1;
	int rows = 0;
	int container_width;
	int max_height = -1;
	int min_height = -1;
	int col_gap = 0;
	int row_gap = 0;
	int col_width;
	int child_index = 0;
	int row_y = 0;
	int row_max_height = 0;
	int row_index = 0;
	struct box *child;
	const css_unit_ctx *unit_len_ctx;
	uint8_t grid_status;
	css_fixed gap_len = 0;
	css_unit gap_unit = CSS_UNIT_PX;

	if (grid == NULL || grid->style == NULL) return false;

	unit_len_ctx = &content->unit_len_ctx;

	/* Read -macsurf-grid packed value. cols default 1, rows default 0
	 * (auto). If the property isn't set, fall back to a 1-column
	 * grid which is visually identical to block layout — the
	 * `display: grid` user got what they asked for without crashing. */
	grid_status = css_computed_macsurf_grid(grid->style, &packed);
	if (grid_status == CSS_MACSURF_GRID_SET) {
		cols = (int)(((uint32_t)packed >> 16) & 0xffff);
		rows = (int)(((uint32_t)packed)       & 0xffff);
	}
	if (cols < 1) cols = 1;
	(void)rows;  /* V1: auto-rows only. */

	/* Container dimensions. layout_find_dimensions resolves auto
	 * widths, margins, padding, borders from CSS. */
	layout_find_dimensions(unit_len_ctx, available_width, -1,
			grid, grid->style, NULL, &grid->height,
			NULL, NULL, &max_height, &min_height,
			grid->margin, grid->padding, grid->border);

	container_width = grid->width;
	if (container_width <= 0) {
		container_width = available_width;
		grid->width = available_width;
	}

	/* column-gap applies between adjacent columns. fixes148 already
	 * wired -macsurf-column-gap as the storage; reuse it here. */
	if (css_computed_column_gap(grid->style, &gap_len, &gap_unit) ==
			CSS_COLUMN_GAP_SET) {
		col_gap = (int)FIXTOINT(css_unit_len2device_px(grid->style,
				unit_len_ctx, gap_len, gap_unit));
		if (col_gap < 0) col_gap = 0;
	}
	/* row-gap shares column-gap storage (fixes148). Use the same. */
	row_gap = col_gap;

	if (cols >= 1) {
		int total_gap = col_gap * (cols - 1);
		col_width = (container_width - total_gap) / cols;
		if (col_width < 1) col_width = 1;
	} else {
		col_width = container_width;
	}

	/* Walk children. */
	child = grid->children;
	while (child != NULL) {
		int col;
		int x_pos;
		int child_total_h;

		col = child_index % cols;
		x_pos = col * (col_width + col_gap);

		/* Lay out the child INTO a cell of width col_width. */
		if (!layout_grid_item(child, col_width, content)) {
			return false;
		}

		/* Position the child. */
		child->x = x_pos;
		child->y = row_y;

		/* Track tallest child in current row. */
		child_total_h = child->height;
		if (child->padding[TOP] > 0)    child_total_h += child->padding[TOP];
		if (child->padding[BOTTOM] > 0) child_total_h += child->padding[BOTTOM];
		if (child->border[TOP].width > 0)
			child_total_h += child->border[TOP].width;
		if (child->border[BOTTOM].width > 0)
			child_total_h += child->border[BOTTOM].width;
		if (child->margin[TOP] != AUTO && child->margin[TOP] > 0)
			child_total_h += child->margin[TOP];
		if (child->margin[BOTTOM] != AUTO && child->margin[BOTTOM] > 0)
			child_total_h += child->margin[BOTTOM];

		if (child_total_h > row_max_height) {
			row_max_height = child_total_h;
		}

		child_index++;

		/* End of row — advance row_y by the tallest cell height
		 * plus the row gap. */
		if ((child_index % cols) == 0) {
			row_y += row_max_height + row_gap;
			row_max_height = 0;
			row_index++;
		}

		child = child->next;
	}

	/* Last partial row (e.g. 5 items in 3 cols leaves 2 items in the
	 * final row). */
	if ((child_index % cols) != 0) {
		row_y += row_max_height;
	} else if (row_index > 0) {
		/* Final completed row added a trailing row_gap that has no
		 * row after it; back it out so grid->height is tight. */
		row_y -= row_gap;
	}

	/* Set the grid container's height to fit all rows. Honour
	 * max-height / min-height from CSS. */
	grid->height = row_y;
	if (grid->height < 0) grid->height = 0;
	if (max_height >= 0 && grid->height > max_height) {
		grid->height = max_height;
	}
	if (min_height > 0 && grid->height < min_height) {
		grid->height = min_height;
	}

	return true;
}
