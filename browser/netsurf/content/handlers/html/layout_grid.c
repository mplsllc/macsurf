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
 * fixes158: explicit `grid-column` / `grid-row` placement (V1).
 *   - positive integer line numbers honoured (1-based, like CSS spec)
 *   - `grid-column: A / B` and `grid-row: A / B` shorthands recognised
 *     by the cssh_css preprocessor; longhand `-start` / `-end` too
 *   - explicit placements DO NOT advance the auto-flow cursor — auto
 *     items continue from where the cursor was, which means an auto
 *     item CAN land in the same cell as an explicit item (last-wins
 *     visually; documented V1 behaviour, no collision avoidance)
 *   - row-span > 1 reserves the row visually but row heights are
 *     still per-row tallest-child for V1 (no vertical merging)
 *
 * Out of V1 scope (deferred to V2+):
 *   - grid-template-columns / grid-template-rows full grammar
 *     (fr units, repeat(), auto, minmax)
 *   - grid-area shorthand
 *   - named grid lines
 *   - negative line numbers (`-1` for end-only is supported as the
 *     fill-row sentinel; `-2`, etc. are not)
 *   - grid-template-areas
 *   - grid-auto-flow column/dense
 *   - subgrid
 *   - align-items / justify-items per-cell alignment
 *   - row-gap independent of column-gap (shares column-gap storage)
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

/* fixes158: per-child placement scratch. The two-pass layout assigns
 * (col, row, col_span, row_span) in pass 1 (placement + child layout
 * into the cell width); pass 2 positions each child once row heights
 * are known. Cap at 256 children per grid — anything beyond degrades
 * to fixes151 single-pass auto-flow.
 *
 * Cap at 256 rows so the row_max_height + row_y arrays stay on the
 * stack. Explicit `grid-row: N` beyond this clamps to the cap. */
#define MACSURF_GRID_CHILDREN_MAX 256
#define MACSURF_GRID_ROWS_MAX 256

struct macsurf_grid_slot {
	int col;
	int row;
	int col_span;
	int row_span;
};

/* fixes158a: bit-packed per-row occupancy. One uint8 per row, bit (1 << col)
 * set if that cell is occupied. cols caps at MACSURF_GRID_TRACK_MAX = 8, which
 * fits exactly in 8 bits. Auto-flow skips occupied cells so explicit
 * placements stay visible. */
static int macsurf_grid_cell_free(const unsigned char *occ, int col, int row)
{
	if (row < 0 || row >= MACSURF_GRID_ROWS_MAX) return 0;
	if (col < 0 || col >= MACSURF_GRID_TRACK_MAX) return 0;
	return (occ[row] & (unsigned char)(1u << col)) == 0;
}

static int macsurf_grid_run_free(const unsigned char *occ, int col, int row,
		int col_span, int cols)
{
	int c;
	for (c = col; c < col + col_span && c < cols; c++) {
		if (!macsurf_grid_cell_free(occ, c, row)) return 0;
	}
	return 1;
}

static void macsurf_grid_mark(unsigned char *occ, int col, int row,
		int col_span, int row_span, int cols)
{
	int r;
	int c;
	int end_r = row + row_span;
	int end_c = col + col_span;
	if (end_r > MACSURF_GRID_ROWS_MAX) end_r = MACSURF_GRID_ROWS_MAX;
	if (end_c > cols) end_c = cols;
	for (r = row; r < end_r; r++) {
		if (r < 0) continue;
		for (c = col; c < end_c; c++) {
			if (c < 0 || c >= MACSURF_GRID_TRACK_MAX) continue;
			occ[r] |= (unsigned char)(1u << c);
		}
	}
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
	const int32_t *raw_tracks;
	int track_widths[MACSURF_GRID_TRACK_MAX];
	int track_x[MACSURF_GRID_TRACK_MAX];
	int n_tracks = 0;
	bool has_tracks = false;
	/* fixes150 -- row tracks (px-only V1; fr/percent degrade to
	 * tallest-child sizing when no definite container height). */
	const int32_t *raw_row_tracks;
	int row_track_h[MACSURF_GRID_TRACK_MAX];
	int n_row_tracks = 0;
	bool has_row_tracks = false;
	int i;

	for (i = 0; i < MACSURF_GRID_TRACK_MAX; i++) {
		track_widths[i] = 0;
		track_x[i] = 0;
		row_track_h[i] = 0;
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

	/* fixes150 -- read row tracks. V1 honours PX heights; FR and
	 * PERCENT row tracks degrade to tallest-child sizing (the
	 * existing auto-row behaviour) because we don't have a definite
	 * container height to distribute against in the auto case. */
	raw_row_tracks = css_computed_macsurf_grid_row_tracks(grid->style);
	if (raw_row_tracks != NULL && raw_row_tracks[0] != 0) {
		for (i = 0; i < MACSURF_GRID_TRACK_MAX; i++) {
			int32_t pk = raw_row_tracks[i];
			uint8_t unit;
			int32_t value;
			if (pk == 0) break;
			unit = (uint8_t)(((uint32_t)pk >> 28) & 0xF);
			value = (int32_t)((uint32_t)pk & 0x0FFFFFFFU);
			if (value & 0x08000000) {
				value |= (int32_t)0xF0000000;
			}
			n_row_tracks++;
			if (unit == MACSURF_GRID_TRACK_UNIT_PX) {
				row_track_h[i] = value;
			} else {
				/* FR / PERCENT placeholder: 0 means "use
				 * tallest-child for this row" in the walker
				 * below. */
				row_track_h[i] = 0;
			}
		}
		has_row_tracks = (n_row_tracks > 0);
	}

	/* fixes158: three-pass placement.
	 *
	 * Pass 0 — pre-mark cells occupied by every child that has an
	 *          explicit placement (col_start AND row_start both > 0).
	 *          Pass 1's auto-flow then skips those cells so the
	 *          explicit items stay visible.
	 * Pass 1 — assign each child a (col, row, col_span, row_span).
	 *          Auto-flow advances row-major through unoccupied cells.
	 * Pass 2 — lay out each child into its cell width; track per-row
	 *          max heights.
	 * Pass 3 — compute row_y once heights are known; position each
	 *          child.
	 *
	 * Backwards compat: a child with all-zero placement falls into
	 * the auto-flow branch and behaves exactly like the original
	 * fixes151 walker (modulo skipping cells now occupied by
	 * explicit siblings). */
	{
		struct macsurf_grid_slot slots[MACSURF_GRID_CHILDREN_MAX];
		int row_max_h[MACSURF_GRID_ROWS_MAX];
		int row_y_arr[MACSURF_GRID_ROWS_MAX];
		unsigned char occupancy[MACSURF_GRID_ROWS_MAX];
		int n_children = 0;
		int cur_col = 0;
		int cur_row = 0;
		int max_row_used = -1;
		int slot_index;

		memset(row_max_h, 0, sizeof(row_max_h));
		memset(row_y_arr, 0, sizeof(row_y_arr));
		memset(occupancy, 0, sizeof(occupancy));

		/* --- Pass 0: reserve cells for explicit-both placements.
		 * We only pre-reserve when BOTH axes are explicit because
		 * that's the only case whose cell is knowable without
		 * walking auto-flow. col-only / row-only explicit items
		 * are still cursor-dependent on the other axis and get
		 * resolved during pass 1; their cells are marked there. */
		child = grid->children;
		while (child != NULL && n_children <
				MACSURF_GRID_CHILDREN_MAX) {
			int32_t packed = 0;
			int col_start;
			int row_start;
			int col_span;
			int row_span;

			if (child->style != NULL) {
				packed = css_computed_macsurf_grid_placement(
						child->style);
			}
			col_span  = (int)((uint32_t)packed         & 0xff);
			col_start = (int)(((uint32_t)packed >>  8) & 0xff);
			row_start = (int)(((uint32_t)packed >> 16) & 0xff);
			row_span  = (int)(((uint32_t)packed >> 24) & 0xff);

			if (col_start > 0 && row_start > 0) {
				int sc = col_start - 1;
				int sr = row_start - 1;
				int span_c;
				int span_r;
				if (col_span == 0) col_span = 1;
				if (col_span == 255) {
					span_c = cols - sc;
				} else {
					span_c = col_span;
				}
				if (row_span == 0) row_span = 1;
				span_r = (row_span == 255) ? 1 : row_span;
				if (sc < 0) sc = 0;
				if (sc >= cols) sc = cols - 1;
				if (sc + span_c > cols) span_c = cols - sc;
				if (span_c < 1) span_c = 1;
				if (sr < 0) sr = 0;
				if (sr >= MACSURF_GRID_ROWS_MAX)
					sr = MACSURF_GRID_ROWS_MAX - 1;
				if (sr + span_r > MACSURF_GRID_ROWS_MAX)
					span_r = MACSURF_GRID_ROWS_MAX - sr;
				if (span_r < 1) span_r = 1;
				macsurf_grid_mark(occupancy, sc, sr,
						span_c, span_r, cols);
			}

			n_children++;
			child = child->next;
		}

		/* --- Pass 1: assign every child a slot. */
		n_children = 0;
		cur_col = 0;
		cur_row = 0;
		child = grid->children;
		while (child != NULL && n_children <
				MACSURF_GRID_CHILDREN_MAX) {
			int32_t packed = 0;
			int col_start;
			int col_span;
			int row_start;
			int row_span;
			int slot_col;
			int slot_row;

			if (child->style != NULL) {
				packed = css_computed_macsurf_grid_placement(
						child->style);
			}
			col_span  = (int)((uint32_t)packed         & 0xff);
			col_start = (int)(((uint32_t)packed >>  8) & 0xff);
			row_start = (int)(((uint32_t)packed >> 16) & 0xff);
			row_span  = (int)(((uint32_t)packed >> 24) & 0xff);

			/* Resolve span sentinels and zero defaults. */
			if (col_span == 0) col_span = 1;
			if (col_span == 255) {
				int from = (col_start > 0) ?
						(col_start - 1) : cur_col;
				col_span = cols - from;
				if (col_span < 1) col_span = 1;
			}
			if (row_span == 0) row_span = 1;
			if (row_span == 255) row_span = 1; /* V1: degrade */

			/* Resolve cell origin. */
			if (col_start > 0 && row_start > 0) {
				/* Both explicit — cells were marked in
				 * pass 0. Auto-cursor unaffected. */
				slot_col = col_start - 1;
				slot_row = row_start - 1;
			} else if (col_start > 0) {
				/* Explicit col, auto row. Find first row
				 * from cur_row where this col-run is free. */
				int sr = cur_row;
				slot_col = col_start - 1;
				if (slot_col < 0) slot_col = 0;
				if (slot_col >= cols) slot_col = cols - 1;
				while (sr < MACSURF_GRID_ROWS_MAX &&
						!macsurf_grid_run_free(
							occupancy, slot_col,
							sr, col_span, cols)) {
					sr++;
				}
				slot_row = sr;
				macsurf_grid_mark(occupancy, slot_col,
						slot_row, col_span, row_span,
						cols);
			} else if (row_start > 0) {
				/* Explicit row, auto col. Find first col
				 * from cur_col in row_start-1 with a free
				 * col-run. */
				int sc = cur_col;
				slot_row = row_start - 1;
				if (slot_row < 0) slot_row = 0;
				if (slot_row >= MACSURF_GRID_ROWS_MAX)
					slot_row = MACSURF_GRID_ROWS_MAX - 1;
				while (sc + col_span <= cols &&
						!macsurf_grid_run_free(
							occupancy, sc, slot_row,
							col_span, cols)) {
					sc++;
				}
				if (sc + col_span > cols) sc = 0;
				slot_col = sc;
				macsurf_grid_mark(occupancy, slot_col,
						slot_row, col_span, row_span,
						cols);
			} else {
				/* Pure auto-flow. Advance cursor row-major
				 * past any occupied cells until a free
				 * col-run of col_span cells is found. */
				int safety = MACSURF_GRID_ROWS_MAX *
						MACSURF_GRID_TRACK_MAX + 4;
				while (safety-- > 0) {
					if (cur_col + col_span > cols) {
						cur_col = 0;
						cur_row++;
					}
					if (cur_row >= MACSURF_GRID_ROWS_MAX)
						break;
					if (macsurf_grid_run_free(occupancy,
							cur_col, cur_row,
							col_span, cols)) {
						break;
					}
					cur_col++;
				}
				slot_col = cur_col;
				slot_row = cur_row;
				macsurf_grid_mark(occupancy, slot_col,
						slot_row, col_span, row_span,
						cols);
				cur_col += col_span;
				if (cur_col >= cols) {
					cur_col = 0;
					cur_row++;
				}
			}

			/* Final clamps (re-derive col_span / row_span in case
			 * the chosen origin pushes us out of bounds). */
			if (slot_col < 0) slot_col = 0;
			if (slot_col >= cols) slot_col = cols - 1;
			if (slot_col + col_span > cols)
				col_span = cols - slot_col;
			if (col_span < 1) col_span = 1;
			if (slot_row < 0) slot_row = 0;
			if (slot_row >= MACSURF_GRID_ROWS_MAX)
				slot_row = MACSURF_GRID_ROWS_MAX - 1;
			if (slot_row + row_span > MACSURF_GRID_ROWS_MAX)
				row_span = MACSURF_GRID_ROWS_MAX - slot_row;
			if (row_span < 1) row_span = 1;

			slots[n_children].col = slot_col;
			slots[n_children].row = slot_row;
			slots[n_children].col_span = col_span;
			slots[n_children].row_span = row_span;

			if (slot_row > max_row_used)
				max_row_used = slot_row;
			if (slot_row + row_span - 1 > max_row_used)
				max_row_used = slot_row + row_span - 1;

			n_children++;
			child = child->next;
		}

		/* --- Pass 1b: layout each child into its cell width. */
		child = grid->children;
		slot_index = 0;
		while (child != NULL && slot_index < n_children) {
			int slot_col = slots[slot_index].col;
			int slot_col_span = slots[slot_index].col_span;
			int slot_row = slots[slot_index].row;
			int this_col_width;
			int saved_grid_width;
			int child_total_h;
			int k;
			int end_col;

			if (has_tracks) {
				this_col_width = track_widths[slot_col];
			} else {
				this_col_width = col_width;
			}
			end_col = slot_col + slot_col_span;
			if (end_col > cols) end_col = cols;
			for (k = slot_col + 1; k < end_col; k++) {
				if (has_tracks) {
					this_col_width += col_gap +
							track_widths[k];
				} else {
					this_col_width += col_gap + col_width;
				}
			}

			/* fixes75a: narrow grid->width while laying out so
			 * child auto-width resolves against the cell, not the
			 * full container. */
			saved_grid_width = grid->width;
			grid->width = this_col_width;

			if (!layout_grid_item(child, this_col_width, content)) {
				grid->width = saved_grid_width;
				return false;
			}

			grid->width = saved_grid_width;

			if (child->width > this_col_width) {
				child->width = this_col_width;
			}

			/* Total cell height contribution. */
			child_total_h = child->height;
			if (child->padding[TOP] > 0)
				child_total_h += child->padding[TOP];
			if (child->padding[BOTTOM] > 0)
				child_total_h += child->padding[BOTTOM];
			if (child->border[TOP].width > 0)
				child_total_h += child->border[TOP].width;
			if (child->border[BOTTOM].width > 0)
				child_total_h += child->border[BOTTOM].width;
			if (child->margin[TOP] != AUTO &&
					child->margin[TOP] > 0)
				child_total_h += child->margin[TOP];
			if (child->margin[BOTTOM] != AUTO &&
					child->margin[BOTTOM] > 0)
				child_total_h += child->margin[BOTTOM];

			/* Credit this child's height to the row it occupies.
			 * Multi-row spans only credit the first row in V1 —
			 * full distribution waits for V2. */
			if (slot_row >= 0 && slot_row < MACSURF_GRID_ROWS_MAX) {
				if (child_total_h > row_max_h[slot_row])
					row_max_h[slot_row] = child_total_h;
			}

			slot_index++;
			child = child->next;
		}

		/* --- Pass 2: compute row_y per row. fixes150 row tracks
		 * give explicit px heights when present; otherwise use the
		 * tallest-child height we tracked above. */
		{
			int r;
			int y = 0;
			int last_h = 0;
			for (r = 0; r <= max_row_used &&
					r < MACSURF_GRID_ROWS_MAX; r++) {
				int h = row_max_h[r];
				if (has_row_tracks && r < n_row_tracks &&
						row_track_h[r] > 0) {
					h = row_track_h[r];
				}
				row_y_arr[r] = y;
				y += h + row_gap;
				last_h = h;
			}
			/* The total grid height is y minus the trailing
			 * row_gap (no row after the last row). */
			row_y = y;
			if (max_row_used >= 0) {
				row_y -= row_gap;
			}
			(void)last_h;
		}

		/* --- Pass 3: position each child. */
		child = grid->children;
		slot_index = 0;
		while (child != NULL && slot_index < n_children) {
			int slot_col = slots[slot_index].col;
			int slot_row = slots[slot_index].row;
			int x_pos;

			if (has_tracks) {
				x_pos = track_x[slot_col];
			} else {
				x_pos = slot_col * (col_width + col_gap);
			}
			child->x = x_pos;
			if (slot_row >= 0 && slot_row < MACSURF_GRID_ROWS_MAX)
				child->y = row_y_arr[slot_row];
			else
				child->y = 0;

			slot_index++;
			child = child->next;
		}

		/* Suppress unused-variable warnings from the legacy
		 * single-pass scaffolding the old loop relied on. */
		(void)child_index;
		(void)row_max_height;
		(void)row_index;
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
