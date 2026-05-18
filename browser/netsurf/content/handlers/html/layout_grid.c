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
 *
 * fixes75b: resolve margins / padding / borders / height via
 * layout_find_dimensions before dispatching, the way layout_flex's
 * populate_item_data does. Without this the item's padding and borders
 * stay at whatever default the box-construct phase left them at and
 * the rendered cell has no breathing room and no border.
 */
static bool layout_grid_item(
		struct box *item,
		int cell_width,
		html_content *content)
{
	bool success = false;
	int dummy_w = cell_width;
	int dummy_min_w = -1;
	int dummy_max_w = -1;
	int dummy_min_h = -1;
	int dummy_max_h = -1;

	if (item->style != NULL) {
		item->float_container = item->parent;
		/* Pass &item->height so layout_find_dimensions writes the
		 * resolved CSS height (typically AUTO) back into the box.
		 * layout_block_context's height-finalisation step at the
		 * end of the function only fires when block->height == AUTO,
		 * and box_create initialises height to 0 (not AUTO), so
		 * without this the item's height stays 0 and row_max_height
		 * in layout_grid can't track the tallest item -- bug from
		 * fixes75b that manifested in PROBE G4. */
		layout_find_dimensions(&content->unit_len_ctx,
				cell_width, -1, item, item->style,
				&dummy_w, &item->height,
				&dummy_max_w, &dummy_min_w,
				&dummy_max_h, &dummy_min_h,
				item->margin, item->padding, item->border);
		item->float_container = NULL;
	}

	/* fixes114 — only force cell_width if the child's CSS width is AUTO
	 * (i.e. the child didn't set its own width). Replaced elements with
	 * explicit widths — .featured-app__icon { width: 96px; height: 96px; }
	 * being the canonical mactrove case — were getting their CSS width
	 * clobbered by cell_width, which is the full container width when
	 * our V1 grid degenerates to 1 column (because we don't yet parse
	 * grid-template-columns). End result: a 96×96 app icon stretched to
	 * 1771×94 across the whole content column. Preserving the explicit
	 * width — and clamping it to cell_width if the CSS specifies more
	 * than the cell can hold — fixes that without losing the equal-fill
	 * behaviour for items that don't set their own width. */
	if (dummy_w == AUTO) {
		item->width = cell_width;
	} else {
		item->width = dummy_w;
		if (item->width > cell_width) {
			item->width = cell_width;
		}
	}

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


#define MACSURF_GRID_TRACK_MAX 8
#define MACSURF_GRID_TRACK_UNIT_NONE    0
#define MACSURF_GRID_TRACK_UNIT_FR      1
#define MACSURF_GRID_TRACK_UNIT_PX      2
#define MACSURF_GRID_TRACK_UNIT_PERCENT 3

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
	const int32_t *raw_tracks;
	int track_widths[MACSURF_GRID_TRACK_MAX];
	int track_x[MACSURF_GRID_TRACK_MAX];
	int n_tracks = 0;
	bool has_tracks = false;
	int i;

	for (i = 0; i < MACSURF_GRID_TRACK_MAX; i++) {
		track_widths[i] = 0;
		track_x[i] = 0;
	}

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

	/* fixes117: explicit track widths from grid-template-columns
	 * (rewritten by cssh_css.c preprocessor and parsed by
	 * p_macsurf_grid.c V2 grammar). track[i] packs:
	 *   bits 31..28 = unit, bits 27..0 = value (signed 28-bit).
	 *   PX:      value = pixels
	 *   FR:      value = Q20.8 fixed-point ratio
	 *   PERCENT: value = Q20.8 fixed-point of 100%
	 * Distribution: subtract PX + PERCENT widths from container,
	 * divide remainder proportionally across FR units. */
	raw_tracks = css_computed_macsurf_grid_tracks(grid->style);
	if (grid_status == CSS_MACSURF_GRID_SET && raw_tracks != NULL &&
			raw_tracks[0] != 0) {
		int fixed_total = 0;
		int fr_total_q88 = 0;
		int total_gap;
		int remaining;

		for (i = 0; i < MACSURF_GRID_TRACK_MAX; i++) {
			int32_t pk = raw_tracks[i];
			uint8_t unit;
			int32_t value;
			if (pk == 0) break;
			unit = (uint8_t)(((uint32_t)pk >> 28) & 0xF);
			/* Sign-extend the 28-bit value. */
			value = (int32_t)((uint32_t)pk & 0x0FFFFFFFU);
			if (value & 0x08000000) {
				value |= (int32_t)0xF0000000;
			}
			n_tracks++;
			if (unit == MACSURF_GRID_TRACK_UNIT_PX) {
				track_widths[i] = value;
				fixed_total += value;
			} else if (unit ==
					MACSURF_GRID_TRACK_UNIT_PERCENT) {
				/* percent applied to container width. */
				int px = (int)(((long)value *
						(long)container_width) /
						(256L * 100L));
				if (px < 0) px = 0;
				track_widths[i] = px;
				fixed_total += px;
			} else if (unit == MACSURF_GRID_TRACK_UNIT_FR) {
				/* Marker: negative means "fr placeholder";
				 * the real width is filled in below once the
				 * remainder is known. Stash Q8.8 in the
				 * negative-encoded slot so we can read it
				 * back without a second pass. */
				track_widths[i] = -value;
				if (value > 0) fr_total_q88 += value;
			}
		}

		if (n_tracks > 0) {
			has_tracks = true;
			cols = n_tracks;
			total_gap = col_gap * (n_tracks - 1);
			remaining = container_width - total_gap -
					fixed_total;
			if (remaining < 0) remaining = 0;

			if (fr_total_q88 > 0) {
				/* Distribute `remaining` across fr tracks
				 * proportionally to their Q8.8 ratio.
				 * frac = (track_q88 / fr_total_q88) */
				for (i = 0; i < n_tracks; i++) {
					if (track_widths[i] < 0) {
						int q88 = -track_widths[i];
						int w = (int)(((long)remaining *
								(long)q88) /
								(long)fr_total_q88);
						if (w < 1) w = 1;
						track_widths[i] = w;
					}
				}
			} else {
				/* No fr tracks, only fixed + percent.
				 * Any leftover space stays unused at the
				 * right of the grid -- spec behavior. */
				for (i = 0; i < n_tracks; i++) {
					if (track_widths[i] < 0) {
						track_widths[i] = 0;
					}
				}
			}

			/* Compute per-track x offset. */
			{
				int x = 0;
				for (i = 0; i < n_tracks; i++) {
					track_x[i] = x;
					x += track_widths[i] + col_gap;
				}
			}
		}
	}

	if (has_tracks) {
		/* col_width is no longer a single uniform value -- the
		 * walker reads track_widths[col] per child. Keep the
		 * legacy `col_width` variable populated to track_widths[0]
		 * so any downstream code that reads it gets a sensible
		 * default. */
		col_width = track_widths[0];
	} else if (cols >= 1) {
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
		int this_col_width;
		int child_total_h;
		int saved_grid_width;

		col = child_index % cols;
		if (has_tracks) {
			this_col_width = track_widths[col];
			x_pos = track_x[col];
		} else {
			this_col_width = col_width;
			x_pos = col * (col_width + col_gap);
		}

		/* fixes75a: Temporarily narrow the grid container's width to
		 * this_col_width while laying out the child. layout_block_context
		 * resolves the child's auto-width against parent->width, so
		 * without this every child grows to the full container width
		 * and the grid degenerates into a single-column stack.
		 * Restore after the child is laid out so subsequent siblings
		 * see the same starting state, and the final grid->width is
		 * set correctly at the end of the loop. */
		saved_grid_width = grid->width;
		grid->width = this_col_width;

		if (!layout_grid_item(child, this_col_width, content)) {
			grid->width = saved_grid_width;
			return false;
		}

		grid->width = saved_grid_width;

		/* Defensive: if the child still came out wider than the
		 * cell (e.g. an explicit fixed CSS width override), clamp.
		 * Children narrower than the cell are fine -- the cell is
		 * an upper bound, not a forced fill. */
		if (child->width > this_col_width) {
			child->width = this_col_width;
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
