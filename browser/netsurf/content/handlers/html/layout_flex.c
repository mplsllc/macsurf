/*
 * Copyright 2022 Michael Drake <tlsa@netsurf-browser.org>
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
 * HTML layout implementation: display: flex.
 *
 * Layout is carried out in two stages:
 *
 * 1. + calculation of minimum / maximum box widths, and
 *    + determination of whether block level boxes will have >zero height
 *
 * 2. + layout (position and dimensions)
 *
 * In most cases the functions for the two stages are a corresponding pair
 * layout_minmax_X() and layout_X().
 */

#include <string.h>

#include "utils/log.h"
#include "utils/utils.h"

#include "html/box.h"
#include "html/html.h"
#include "html/private.h"
#include "html/box_inspect.h"
#include "html/layout_internal.h"
/* fixes168 — shared sanitizers (layout-wide). flex's own FLEX_SAFE_*
 * helpers stay for back-compat with the fixes167 call sites; new
 * code uses the layout_dim_* shared API. */
#include "html/layout_safe.h"

/* fixes161d — diagnostic-only macsurf_debug_log_writef for the
 * LAYOUTPHASE flex marker. Compiles to a no-op in release builds. */
#include "macsurf_debug.h"

/* fixes167 — flex survival caps. Apple and HuffPost both reach the
 * flex code with degenerate inputs: AUTO main availability on a
 * nested column-direction container, child counts that exceed any
 * realistic page (broken sub-resources), or items the engine cannot
 * base-size cleanly. Spec flexbox is undefined on those inputs.
 *
 * Rather than crash mid-redraw or leave the box tree in a partial
 * state that the document tail (abs/rel/bbox) walks into and crashes
 * on, this round converts any unsafe flex container locally into
 * block-flow: children stack top-to-bottom inside the container,
 * heights resolve concretely, and layout_document continues with
 * abs/rel/bbox running normally over the whole tree. Other (working)
 * flex containers on the same page are untouched.
 *
 * FLEX_SAFE_MAX bounds any sanitized dimension to a value that
 * cannot overflow the Mac32 signed coord space when summed with
 * neighbours; FLEX_MAX_ITEMS / FLEX_MAX_LINES catch runaway counts
 * from a broken DOM. */
#define FLEX_SAFE_MAX  1000000
#define FLEX_MAX_ITEMS 512
#define FLEX_MAX_LINES 512

/* fixes167a — finite-dimension sanitizer. Replace AUTO, negative,
 * or absurdly large with the caller-supplied fallback. After this
 * call the return value is in [0, FLEX_SAFE_MAX]. */
static int flex_safe_dim(int v, int fallback)
{
	if (v == AUTO) return fallback;
	if (v < 0) return fallback;
	if (v > FLEX_SAFE_MAX) return fallback;
	return v;
}

/* fixes167a — fallback that's known to be in range. Used when the
 * caller has nothing better. */
static int flex_safe_fallback_dim(int fallback)
{
	if (fallback < 0) return 0;
	if (fallback > FLEX_SAFE_MAX) return FLEX_SAFE_MAX;
	return fallback;
}

/**
 * Flex item data
 */
struct flex_item_data {
	enum css_flex_basis_e basis;
	css_fixed basis_length;
	css_unit basis_unit;
	struct box *box;

	css_fixed shrink;
	css_fixed grow;

	int min_main;
	int max_main;
	int min_cross;
	int max_cross;

	int target_main_size;
	int base_size;
	int main_size;
	size_t line;

	bool freeze;
	bool min_violation;
	bool max_violation;
};

/**
 * Flex line data
 */
struct flex_line_data {
	int main_size;
	int cross_size;

	int used_main_size;
	int main_auto_margin_count;

	int pos;

	size_t first;
	size_t count;
	size_t frozen;
};

/**
 * Flex layout context
 */
struct flex_ctx {
	html_content *content;
	const struct box *flex;
	const css_unit_ctx *unit_len_ctx;

	int main_size;
	int cross_size;

	int available_main;
	int available_cross;

	bool horizontal;
	bool main_reversed;
	enum css_flex_wrap_e wrap;

	/* fixes148 -- gap between flex items on the main axis and between
	 * wrapped flex lines on the cross axis. Both currently read the
	 * same computed field (column-gap), since MacSurf's libcss does
	 * not yet have independent row-gap storage. See gap.c. */
	int main_gap;
	int cross_gap;

	struct flex_items {
		size_t count;
		struct flex_item_data *data;
	} item;

	struct flex_lines {
		size_t count;
		size_t alloc;
		struct flex_line_data *data;
	} line;
};

/**
 * Destroy a flex layout context
 *
 * \param[in] ctx  Flex layout context
 */
static void layout_flex_ctx__destroy(struct flex_ctx *ctx)
{
	if (ctx != NULL) {
		free(ctx->item.data);
		free(ctx->line.data);
		free(ctx);
	}
}

/**
 * Create a flex layout context
 *
 * \param[in] content  HTML content containing flex box
 * \param[in] flex     Box to create layout context for
 * \return flex layout context or NULL on error
 */
static struct flex_ctx *layout_flex_ctx__create(
		html_content *content,
		const struct box *flex)
{
	struct flex_ctx *ctx;

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		return NULL;
	}
	ctx->line.alloc = 1;

	ctx->item.count = box_count_children(flex);
	ctx->item.data = calloc(ctx->item.count, sizeof(*ctx->item.data));
	if (ctx->item.data == NULL) {
		layout_flex_ctx__destroy(ctx);
		return NULL;
	}

	ctx->line.alloc = 1;
	ctx->line.data = calloc(ctx->line.alloc, sizeof(*ctx->line.data));
	if (ctx->line.data == NULL) {
		layout_flex_ctx__destroy(ctx);
		return NULL;
	}

	ctx->flex = flex;
	ctx->content = content;
	ctx->unit_len_ctx = &content->unit_len_ctx;

	ctx->wrap = css_computed_flex_wrap(flex->style);
	ctx->horizontal = lh__flex_main_is_horizontal(flex);
	ctx->main_reversed = lh__flex_direction_reversed(flex);

	/* fixes148 -- resolve column-gap to device pixels once per
	 * container. CSS_COLUMN_GAP_NORMAL => 0 for flex (per spec).
	 * main_gap and cross_gap are equal for now because row-gap
	 * shares storage with column-gap; when row-gap lands as its
	 * own property, split this into two independent reads. */
	{
		css_fixed gap_len = 0;
		css_unit gap_unit = CSS_UNIT_PX;
		uint8_t gap_type = css_computed_column_gap(flex->style,
				&gap_len, &gap_unit);
		if (gap_type == CSS_COLUMN_GAP_SET) {
			ctx->main_gap = FIXTOINT(css_unit_len2device_px(
					flex->style, ctx->unit_len_ctx,
					gap_len, gap_unit));
			if (ctx->main_gap < 0) {
				ctx->main_gap = 0;
			}
		} else {
			ctx->main_gap = 0;
		}
		ctx->cross_gap = ctx->main_gap;
	}

	return ctx;
}

/**
 * Find box side representing the start of flex container in main direction.
 *
 * \param[in] ctx   Flex layout context.
 * \return the start side.
 */
static enum box_side layout_flex__main_start_side(
		const struct flex_ctx *ctx)
{
	if (ctx->horizontal) {
		return (ctx->main_reversed) ? RIGHT : LEFT;
	} else {
		return (ctx->main_reversed) ? BOTTOM : TOP;
	}
}

/**
 * Find box side representing the end of flex container in main direction.
 *
 * \param[in] ctx   Flex layout context.
 * \return the end side.
 */
static enum box_side layout_flex__main_end_side(
		const struct flex_ctx *ctx)
{
	if (ctx->horizontal) {
		return (ctx->main_reversed) ? LEFT : RIGHT;
	} else {
		return (ctx->main_reversed) ? TOP : BOTTOM;
	}
}

/**
 * Perform layout on a flex item
 *
 * \param[in] ctx              Flex layout context
 * \param[in] item             Item to lay out
 * \param[in] available_width  Available width for item in pixels
 * \return true on success false on failure
 */
static bool layout_flex_item(
		const struct flex_ctx *ctx,
		const struct flex_item_data *item,
		int available_width)
{
	bool success;
	struct box *b = item->box;

	switch (b->type) {
	case BOX_BLOCK:
		success = layout_block_context(b, -1, ctx->content);
		break;
	case BOX_TABLE:
		b->float_container = b->parent;
		success = layout_table(b, available_width, ctx->content);
		b->float_container = NULL;
		break;
	case BOX_FLEX:
		b->float_container = b->parent;
		success = layout_flex(b, available_width, ctx->content);
		b->float_container = NULL;
		break;
	default:
		assert(0 && "Bad flex item back type");
		success = false;
		break;
	}

	if (!success) {
		NSLOG(flex, ERROR, "box %p: layout failed", b);
	}

	return success;
}

/**
 * Calculate an item's base and target main sizes.
 *
 * \param[in] ctx              Flex layout context
 * \param[in] item             Item to get sizes of
 * \param[in] available_width  Available width in pixels
 * \return true on success false on failure
 */
static inline bool layout_flex__base_and_main_sizes(
		const struct flex_ctx *ctx,
		struct flex_item_data *item,
		int available_width)
{
	struct box *b = item->box;
	int content_min_width = b->min_width;
	int content_max_width = b->max_width;
	int delta_outer_main = lh__delta_outer_main(ctx->flex, b);

	NSLOG(flex, DEEPDEBUG, "box %p: delta_outer_main: %i",
			b, delta_outer_main);

	/* fixes167b — defensive: caller passes a sanitized available_width
	 * but min/max widths from a degenerate minmax pass can still be
	 * AUTO or absurd. Clamp before they get folded into base_size. */
	if (content_min_width == AUTO || content_min_width < 0 ||
			content_min_width > FLEX_SAFE_MAX) {
		content_min_width = 0;
	}
	if (content_max_width == AUTO || content_max_width < 0 ||
			content_max_width > FLEX_SAFE_MAX) {
		content_max_width = available_width;
	}
	if (delta_outer_main < 0 || delta_outer_main > FLEX_SAFE_MAX) {
		delta_outer_main = 0;
	}

	if (item->basis == CSS_FLEX_BASIS_SET) {
		if (item->basis_unit == CSS_UNIT_PCT) {
			item->base_size = FPCT_OF_INT_TOINT(
					item->basis_length,
					available_width);
		} else {
			item->base_size = FIXTOINT(css_unit_len2device_px(
					b->style, ctx->unit_len_ctx,
					item->basis_length,
					item->basis_unit));
		}

	} else if (item->basis == CSS_FLEX_BASIS_AUTO) {
		item->base_size = ctx->horizontal ? b->width : b->height;
	} else {
		item->base_size = AUTO;
	}

	if (ctx->horizontal == false) {
		if (b->width == AUTO) {
			b->width = min(max(content_min_width, available_width),
					content_max_width);
			b->width -= lh__delta_outer_width(b);
		}

		if (!layout_flex_item(ctx, item, b->width)) {
			return false;
		}
	}

	if (item->base_size == AUTO) {
		if (ctx->horizontal == false) {
			item->base_size = b->height;
		} else {
			item->base_size = content_max_width - delta_outer_main;
		}
	}

	item->base_size += delta_outer_main;

	if (ctx->horizontal) {
		item->base_size = min(item->base_size, available_width);
		item->base_size = max(item->base_size, content_min_width);
	}

	item->target_main_size = item->base_size;
	item->main_size = item->base_size;

	if (item->max_main > 0 &&
	    item->main_size > item->max_main + delta_outer_main) {
		item->main_size = item->max_main + delta_outer_main;
	}

	if (item->main_size < item->min_main + delta_outer_main) {
		item->main_size = item->min_main + delta_outer_main;
	}

	NSLOG(flex, DEEPDEBUG, "flex-item box: %p: base_size: %i, main_size %i",
			b, item->base_size, item->main_size);

	return true;
}

/**
 * Fill out all item's data in a flex container.
 *
 * \param[in] ctx              Flex layout context
 * \param[in] flex             Flex box
 * \param[in] available_width  Available width in pixels
 * \return true on success, false if any child cannot be base-sized
 *         (caller should fall back to block-flow). fixes167b.
 */
static bool layout_flex_ctx__populate_item_data(
		const struct flex_ctx *ctx,
		const struct box *flex,
		int available_width)
{
	size_t i = 0;
	bool horizontal = ctx->horizontal;
	struct box *b;
	int safe_avail;

	/* fixes167b — sanitize available_width once; it feeds every
	 * child's base-size math and percentage flex-basis resolution.
	 * AUTO would propagate as INT_MIN through arithmetic. */
	safe_avail = flex_safe_dim(available_width, 0);

	for (b = flex->children; b != NULL; b = b->next) {
		struct flex_item_data *item;

		/* fixes167b — cap; broken DOM with thousands of flex
		 * children shouldn't bring down the redraw. */
		if (i >= ctx->item.count) {
			macsurf_debug_log_writef(
				"FLEXSAFE child overflow flex=%p i=%d cap=%d",
				(void *)flex, (int)i, (int)ctx->item.count);
			return false;
		}
		if (i >= FLEX_MAX_ITEMS) {
			macsurf_debug_log_writef(
				"FLEXSAFE child cap-reached flex=%p i=%d",
				(void *)flex, (int)i);
			return false;
		}
		if (b->type != BOX_BLOCK && b->type != BOX_TABLE &&
				b->type != BOX_FLEX &&
				b->type != BOX_INLINE_BLOCK &&
				b->type != BOX_INLINE_FLEX) {
			macsurf_debug_log_writef(
				"FLEXSAFE unsupported child type=%d flex=%p",
				(int)b->type, (void *)flex);
			return false;
		}

		item = &ctx->item.data[i++];

		b->float_container = b->parent;
		layout_find_dimensions(ctx->unit_len_ctx, safe_avail, -1,
				b, b->style, &b->width, &b->height,
				horizontal ? &item->max_main : &item->max_cross,
				horizontal ? &item->min_main : &item->min_cross,
				horizontal ? &item->max_cross : &item->max_main,
				horizontal ? &item->min_cross : &item->min_main,
				b->margin, b->padding, b->border);
		b->float_container = NULL;

		/* fixes123: CSS Sizing 3 — for replaced elements where one
		 * dimension is auto and the other is concrete, fill in the
		 * auto dim from the intrinsic aspect ratio BEFORE flex sizing
		 * runs. Without this, the flex algorithm substitutes natural
		 * width for an auto-width image, producing 324x30 squashed
		 * icons instead of aspect-correct ~45x30. */
		if (b->object != NULL) {
			int intrinsic_w = content_get_width(b->object);
			int intrinsic_h = content_get_height(b->object);
			if (intrinsic_w > 0 && intrinsic_h > 0) {
				if (b->width == AUTO && b->height != AUTO) {
					b->width = (b->height * intrinsic_w) /
							intrinsic_h;
				} else if (b->height == AUTO &&
						b->width != AUTO) {
					b->height = (b->width * intrinsic_h) /
							intrinsic_w;
				}
			}
		}

		NSLOG(flex, DEEPDEBUG, "flex-item box: %p: width: %i",
				b, b->width);

		item->box = b;
		item->basis = css_computed_flex_basis(b->style,
				&item->basis_length, &item->basis_unit);

		css_computed_flex_shrink(b->style, &item->shrink);
		css_computed_flex_grow(b->style, &item->grow);

		/* fixes167b — pass the sanitized available width. A
		 * per-child base-size failure (return false from the
		 * recursive layout_flex_item it kicks off for column
		 * direction) is treated as a fallback signal for the
		 * whole container; the caller will rebuild the children
		 * as block flow. */
		if (!layout_flex__base_and_main_sizes(ctx, item, safe_avail)) {
			macsurf_debug_log_writef(
				"FLEXSAFE base-size failed flex=%p item=%d box=%p",
				(void *)flex, (int)(i - 1), (void *)b);
			return false;
		}

		/* fixes167b — sanitize the per-item state that the line
		 * collector reads. main_size and target_main_size feed
		 * arithmetic against available_main; AUTO/negative would
		 * propagate. */
		if (item->base_size == AUTO || item->base_size < 0 ||
				item->base_size > FLEX_SAFE_MAX) {
			item->base_size = 0;
		}
		if (item->main_size == AUTO || item->main_size < 0 ||
				item->main_size > FLEX_SAFE_MAX) {
			item->main_size = item->base_size;
		}
		if (item->target_main_size == AUTO ||
				item->target_main_size < 0 ||
				item->target_main_size > FLEX_SAFE_MAX) {
			item->target_main_size = item->main_size;
		}
	}

	return true;
}

/**
 * fixes41 -- re-sort flex items by computed `order` property.
 * Stable bubble sort: items with equal order retain DOM order.
 * Items typically number < 20 so O(n^2) is fine.
 */
static void layout_flex__order_items(struct flex_ctx *ctx)
{
	size_t n = ctx->item.count;
	size_t i;
	size_t j;
	if (n < 2) return;
	for (i = 0; i + 1 < n; i++) {
		for (j = 0; j + 1 + i < n; j++) {
			int32_t order_a = 0;
			int32_t order_b = 0;
			struct flex_item_data tmp;
			if (ctx->item.data[j].box != NULL &&
			    ctx->item.data[j].box->style != NULL) {
				css_computed_order(
					ctx->item.data[j].box->style,
					&order_a);
			}
			if (ctx->item.data[j + 1].box != NULL &&
			    ctx->item.data[j + 1].box->style != NULL) {
				css_computed_order(
					ctx->item.data[j + 1].box->style,
					&order_b);
			}
			if (order_a > order_b) {
				tmp = ctx->item.data[j];
				ctx->item.data[j] = ctx->item.data[j + 1];
				ctx->item.data[j + 1] = tmp;
			}
		}
	}
}

/**
 * Ensure context's lines array has a free space
 *
 * \param[in] ctx  Flex layout context
 * \return true on success false on out of memory
 */
static bool layout_flex_ctx__ensure_line(struct flex_ctx *ctx)
{
	struct flex_line_data *temp;
	size_t line_alloc = ctx->line.alloc * 2;

	if (ctx->line.alloc > ctx->line.count) {
		return true;
	}

	temp = realloc(ctx->line.data, sizeof(*ctx->line.data) * line_alloc);
	if (temp == NULL) {
		return false;
	}
	ctx->line.data = temp;

	memset(ctx->line.data + ctx->line.alloc, 0,
	       sizeof(*ctx->line.data) * (line_alloc - ctx->line.alloc));
	ctx->line.alloc = line_alloc;

	return true;
}

/**
 * Assigns flex items to the line and returns the line
 *
 * \param[in] ctx         Flex layout context
 * \param[in] item_index  Index to first item to assign to this line
 * \return Pointer to the new line, or NULL on error.
 */
static struct flex_line_data *layout_flex__build_line(struct flex_ctx *ctx,
		size_t item_index)
{
	enum box_side start_side = layout_flex__main_start_side(ctx);
	enum box_side end_side = layout_flex__main_end_side(ctx);
	struct flex_line_data *line;
	int used_main = 0;

	if (!layout_flex_ctx__ensure_line(ctx)) {
		return NULL;
	}

	line = &ctx->line.data[ctx->line.count];
	line->first = item_index;

	NSLOG(flex, DEEPDEBUG, "flex container %p: available main: %i",
			ctx->flex, ctx->available_main);

	while (item_index < ctx->item.count) {
		struct flex_item_data *item = &ctx->item.data[item_index];
		struct box *b = item->box;
		int pos_main;

		pos_main = ctx->horizontal ?
				item->main_size :
				b->height + lh__delta_outer_main(ctx->flex, b);

		/* fixes148 -- include the gap contributed by this item
		 * (only for items that aren't the first on the line) when
		 * deciding whether it fits. */
		{
			int item_main_with_gap = pos_main;
			if (line->count > 0 &&
			    lh__box_is_absolute(item->box) == false) {
				item_main_with_gap += ctx->main_gap;
			}
			if (!(ctx->wrap == CSS_FLEX_WRAP_NOWRAP ||
			    item_main_with_gap + used_main <= ctx->available_main ||
			    lh__box_is_absolute(item->box) ||
			    ctx->available_main == AUTO ||
			    line->count == 0 ||
			    pos_main == 0)) {
				break;
			}
		}

		if (lh__box_is_absolute(item->box) == false) {
			if (line->count > 0) {
				line->main_size += ctx->main_gap;
				used_main += ctx->main_gap;
			}
			line->main_size += item->main_size;
			used_main += pos_main;

			if (b->margin[start_side] == AUTO) {
				line->main_auto_margin_count++;
			}
			if (b->margin[end_side] == AUTO) {
				line->main_auto_margin_count++;
			}
		}
		item->line = ctx->line.count;
		line->count++;
		item_index++;
	}

	if (line->count > 0) {
		ctx->line.count++;
	} else {
		NSLOG(layout, ERROR, "Failed to fit any flex items");
	}

	return line;
}

/**
 * Freeze an item on a line
 *
 * \param[in] line  Line to containing item
 * \param[in] item  Item to freeze
 */
static inline void layout_flex__item_freeze(
		struct flex_line_data *line,
		struct flex_item_data *item)
{
	item->freeze = true;
	line->frozen++;

	if (!lh__box_is_absolute(item->box)){
		line->used_main_size += item->target_main_size;
	}

	NSLOG(flex, DEEPDEBUG, "flex-item box: %p: "
			"Frozen at target_main_size: %i",
			item->box, item->target_main_size);
}

/**
 * Calculate remaining free space and unfrozen item factor sum
 *
 * \param[in]  ctx                  Flex layout context
 * \param[in]  line                 Line to calculate free space on
 * \param[out] unfrozen_factor_sum  Returns sum of unfrozen item's flex factors
 * \param[in]  initial_free_main    Initial free space in main direction
 * \param[in]  available_main       Available space in main direction
 * \param[in]  grow                 Whether to grow or shrink
 * return remaining free space on line
 */
static inline int layout_flex__remaining_free_main(
		struct flex_ctx *ctx,
		struct flex_line_data *line,
		css_fixed *unfrozen_factor_sum,
		int initial_free_main,
		int available_main,
		bool grow)
{
	int remaining_free_main = available_main;
	size_t item_count = line->first + line->count;
	size_t i;

	*unfrozen_factor_sum = 0;

	for (i = line->first; i < item_count; i++) {
		struct flex_item_data *item = &ctx->item.data[i];

		if (item->freeze) {
			remaining_free_main -= item->target_main_size;
		} else {
			remaining_free_main -= item->base_size;

			*unfrozen_factor_sum += grow ?
					item->grow : item->shrink;
		}
	}

	if (*unfrozen_factor_sum < F_1) {
		int free_space = FIXTOINT(FMUL(INTTOFIX(initial_free_main),
				*unfrozen_factor_sum));

		if (free_space < remaining_free_main) {
			remaining_free_main = free_space;
		}
	}

	NSLOG(flex, DEEPDEBUG, "Remaining free space: %i",
			remaining_free_main);

	return remaining_free_main;
}

/**
 * Clamp flex item target main size and get min/max violations
 *
 * \param[in] ctx   Flex layout context
 * \param[in] line  Line to align items on
 * return total violation in pixels
 */
static inline int layout_flex__get_min_max_violations(
		struct flex_ctx *ctx,
		struct flex_line_data *line)
{

	int total_violation = 0;
	size_t item_count = line->first + line->count;
	size_t i;

	for (i = line->first; i < item_count; i++) {
		struct flex_item_data *item = &ctx->item.data[i];
		int target_main_size = item->target_main_size;

		NSLOG(flex, DEEPDEBUG, "item %p: target_main_size: %i",
					item->box, target_main_size);

		if (item->freeze) {
			continue;
		}

		if (item->max_main > 0 &&
		    target_main_size > item->max_main) {
			target_main_size = item->max_main;
			item->max_violation = true;
			NSLOG(flex, DEEPDEBUG, "Violation: max_main: %i",
					item->max_main);
		}

		if (target_main_size < item->min_main) {
			target_main_size = item->min_main;
			item->min_violation = true;
			NSLOG(flex, DEEPDEBUG, "Violation: min_main: %i",
					item->min_main);
		}

		if (target_main_size < item->box->min_width) {
			target_main_size = item->box->min_width;
			item->min_violation = true;
			NSLOG(flex, DEEPDEBUG, "Violation: box min_width: %i",
					item->box->min_width);
		}

		if (target_main_size < 0) {
			target_main_size = 0;
			item->min_violation = true;
			NSLOG(flex, DEEPDEBUG, "Violation: less than 0");
		}

		total_violation += target_main_size - item->target_main_size;
		item->target_main_size = target_main_size;
	}

	NSLOG(flex, DEEPDEBUG, "Total violation: %i", total_violation);

	return total_violation;
}

/**
 * Distribute remaining free space proportional to the flex factors.
 *
 * Remaining free space may be negative.
 *
 * \param[in] ctx                  Flex layout context
 * \param[in] line                 Line to distribute free space on
 * \param[in] unfrozen_factor_sum  Sum of unfrozen item's flex factors
 * \param[in] remaining_free_main  Remaining free space in main direction
 * \param[in] grow                 Whether to grow or shrink
 */
static inline void layout_flex__distribute_free_main(
		struct flex_ctx *ctx,
		struct flex_line_data *line,
		css_fixed unfrozen_factor_sum,
		int remaining_free_main,
		bool grow)
{
	size_t item_count = line->first + line->count;
	size_t i;

	if (grow) {
		css_fixed remainder = 0;
		for (i = line->first; i < item_count; i++) {
			struct flex_item_data *item = &ctx->item.data[i];
			css_fixed result;
			css_fixed ratio;

			if (item->freeze) {
				continue;
			}

			ratio = FDIV(item->grow, unfrozen_factor_sum);
			result = FMUL(INTTOFIX(remaining_free_main), ratio) +
					remainder;

			item->target_main_size = item->base_size +
					FIXTOINT(result);
			remainder = FIXFRAC(result);
		}
	} else {
		css_fixed scaled_shrink_factor_sum = 0;
		css_fixed remainder = 0;

		for (i = line->first; i < item_count; i++) {
			struct flex_item_data *item = &ctx->item.data[i];
			css_fixed scaled_shrink_factor;

			if (item->freeze) {
				continue;
			}

			scaled_shrink_factor = FMUL(
					item->shrink,
					INTTOFIX(item->base_size));
			scaled_shrink_factor_sum += scaled_shrink_factor;
		}

		for (i = line->first; i < item_count; i++) {
			struct flex_item_data *item = &ctx->item.data[i];
			css_fixed scaled_shrink_factor;
			css_fixed result;
			css_fixed ratio;

			if (item->freeze) {
				continue;
			} else if (scaled_shrink_factor_sum == 0) {
				item->target_main_size = item->main_size;
				layout_flex__item_freeze(line, item);
				continue;
			}

			scaled_shrink_factor = FMUL(
					item->shrink,
					INTTOFIX(item->base_size));
			ratio = FDIV(scaled_shrink_factor,
			             scaled_shrink_factor_sum);
			result = FMUL(INTTOFIX(abs(remaining_free_main)),
			              ratio) + remainder;

			item->target_main_size = item->base_size -
					FIXTOINT(result);
			remainder = FIXFRAC(result);
		}
	}
}

/**
 * Resolve flexible item lengths along a line.
 *
 * See 9.7 of Tests CSS Flexible Box Layout Module Level 1.
 *
 * \param[in] ctx   Flex layout context
 * \param[in] line  Line to resolve
 * \return true on success, false on failure.
 */
static bool layout_flex__resolve_line(
		struct flex_ctx *ctx,
		struct flex_line_data *line)
{
	size_t item_count = line->first + line->count;
	int available_main = ctx->available_main;
	int initial_free_main;
	bool grow;
	size_t i;

	if (available_main == AUTO) {
		available_main = INT_MAX;
	}

	grow = (line->main_size < available_main);
	initial_free_main = available_main;

	NSLOG(flex, DEEPDEBUG, "box %p: line %zu: first: %zu, count: %zu",
			ctx->flex, line - ctx->line.data,
			line->first, line->count);
	NSLOG(flex, DEEPDEBUG, "Line main_size: %i, available_main: %i",
			line->main_size, available_main);

	for (i = line->first; i < item_count; i++) {
		struct flex_item_data *item = &ctx->item.data[i];

		/* 3. Size inflexible items */
		if (grow) {
			if (item->grow == 0 ||
			    item->base_size > item->main_size) {
				item->target_main_size = item->main_size;
				layout_flex__item_freeze(line, item);
			}
		} else {
			if (item->shrink == 0 ||
			    item->base_size < item->main_size) {
				item->target_main_size = item->main_size;
				layout_flex__item_freeze(line, item);
			}
		}

		/* 4. Calculate initial free space */
		if (item->freeze) {
			initial_free_main -= item->target_main_size;
		} else {
			initial_free_main -= item->base_size;
		}
	}

	/* 5. Loop */
	while (line->frozen < line->count) {
		css_fixed unfrozen_factor_sum;
		int remaining_free_main;
		int total_violation;

		NSLOG(flex, DEEPDEBUG, "flex-container: %p: Resolver pass",
				ctx->flex);

		/* b */
		remaining_free_main = layout_flex__remaining_free_main(ctx,
				line, &unfrozen_factor_sum, initial_free_main,
				available_main, grow);

		/* c */
		if (remaining_free_main != 0) {
			layout_flex__distribute_free_main(ctx,
					line, unfrozen_factor_sum,
					remaining_free_main, grow);
		}

		/* d */
		total_violation = layout_flex__get_min_max_violations(
				ctx, line);

		/* e */
		for (i = line->first; i < item_count; i++) {
			struct flex_item_data *item = &ctx->item.data[i];

			if (item->freeze) {
				continue;
			}

			if (total_violation == 0 ||
			    (total_violation > 0 && item->min_violation) ||
			    (total_violation < 0 && item->max_violation)) {
				layout_flex__item_freeze(line, item);
			}
		}
	}

	return true;
}

/**
 * Position items along a line
 *
 * \param[in] ctx   Flex layout context
 * \param[in] line  Line to resolve
 * \return true on success, false on failure.
 */
static bool layout_flex__place_line_items_main(
		struct flex_ctx *ctx,
		struct flex_line_data *line)
{
	int main_pos = ctx->flex->padding[layout_flex__main_start_side(ctx)];
	int post_multiplier = ctx->main_reversed ? 0 : 1;
	int pre_multiplier = ctx->main_reversed ? -1 : 0;
	size_t item_count = line->first + line->count;
	int extra_remainder = 0;
	int extra = 0;
	/* fixes41 -- justify-content distribution. */
	int jc_free = 0;
	int jc_between = 0;
	uint8_t jc_v = CSS_JUSTIFY_CONTENT_FLEX_START;
	size_t i;

	if (ctx->main_reversed) {
		main_pos = lh__box_size_main(ctx->horizontal, ctx->flex) -
				main_pos;
	}

	if (ctx->available_main != AUTO &&
	    ctx->available_main != UNKNOWN_WIDTH &&
	    ctx->available_main > line->used_main_size) {
		if (line->main_auto_margin_count > 0) {
			extra = ctx->available_main - line->used_main_size;

			extra_remainder = extra % line->main_auto_margin_count;
			extra /= line->main_auto_margin_count;
		} else {
			/* fixes41 -- no auto-margin consumers, free main
			 * space goes to justify-content. */
			jc_free = ctx->available_main - line->used_main_size;
		}
	}

	/* fixes41 -- read justify-content and compute initial offset +
	 * between-item gap. flex-start is the default and adds nothing. */
	if (ctx->flex->style != NULL) {
		jc_v = css_computed_justify_content(ctx->flex->style);
	}
	if (jc_free > 0 && line->count > 0) {
		int pre_gap = 0;
		switch (jc_v) {
		case CSS_JUSTIFY_CONTENT_FLEX_END:
			pre_gap = jc_free;
			break;
		case CSS_JUSTIFY_CONTENT_CENTER:
			pre_gap = jc_free / 2;
			break;
		case CSS_JUSTIFY_CONTENT_SPACE_BETWEEN:
			if (line->count > 1) {
				jc_between = jc_free /
					(int)(line->count - 1);
			}
			break;
		case CSS_JUSTIFY_CONTENT_SPACE_AROUND:
			jc_between = jc_free / (int)line->count;
			pre_gap = jc_between / 2;
			break;
		case CSS_JUSTIFY_CONTENT_SPACE_EVENLY:
			jc_between = jc_free /
				(int)(line->count + 1);
			pre_gap = jc_between;
			break;
		default:
			break;
		}
		if (ctx->main_reversed) {
			main_pos -= pre_gap;
		} else {
			main_pos += pre_gap;
		}
	}

	for (i = line->first; i < item_count; i++) {
		enum box_side main_end = ctx->horizontal ? RIGHT : BOTTOM;
		enum box_side main_start = ctx->horizontal ? LEFT : TOP;
		struct flex_item_data *item = &ctx->item.data[i];
		struct box *b = item->box;
		int extra_total = 0;
		int extra_post = 0;
		int extra_pre = 0;
		int box_size_main;
		int *box_pos_main;

		if (ctx->horizontal) {
			b->width = item->target_main_size -
					lh__delta_outer_width(b);

			if (!layout_flex_item(ctx, item, b->width)) {
				return false;
			}
		}

		box_size_main = lh__box_size_main(ctx->horizontal, b);
		box_pos_main = ctx->horizontal ? &b->x : &b->y;

		if (!lh__box_is_absolute(b)) {
			if (b->margin[main_start] == AUTO) {
				extra_pre = extra + extra_remainder;
			}
			if (b->margin[main_end] == AUTO) {
				extra_post = extra + extra_remainder;
			}
			extra_total = extra_pre + extra_post;

			main_pos += pre_multiplier *
					(extra_total + box_size_main +
					 lh__delta_outer_main(ctx->flex, b));
		}

		*box_pos_main = main_pos + lh__non_auto_margin(b, main_start) +
				extra_pre + b->border[main_start].width;

		if (!lh__box_is_absolute(b)) {
			int cross_size;
			int box_size_cross = lh__box_size_cross(
					ctx->horizontal, b);

			main_pos += post_multiplier *
					(extra_total + box_size_main +
					 lh__delta_outer_main(ctx->flex, b));

			/* fixes148 -- add main-axis gap between consecutive
			 * items. Direction-aware: reversed lines walk
			 * main_pos backwards, so the gap flips sign. Skip
			 * the gap after the last item on the line. */
			if (i + 1 < item_count) {
				int between_total = ctx->main_gap + jc_between;
				if (ctx->main_reversed) {
					main_pos -= between_total;
				} else {
					main_pos += between_total;
				}
			}

			cross_size = box_size_cross + lh__delta_outer_cross(
					ctx->flex, b);
			if (line->cross_size < cross_size) {
				line->cross_size = cross_size;
			}
		}
	}

	return true;
}

/**
 * Collect items onto lines and place items along the lines
 *
 * \param[in] ctx   Flex layout context
 * \return true on success, false on failure.
 */
static bool layout_flex__collect_items_into_lines(
		struct flex_ctx *ctx)
{
	size_t pos = 0;

	while (pos < ctx->item.count) {
		struct flex_line_data *line;

		line = layout_flex__build_line(ctx, pos);
		if (line == NULL) {
			return false;
		}

		pos += line->count;

		NSLOG(flex, DEEPDEBUG, "flex-container: %p: "
				"fitted: %zu (total: %zu/%zu)",
				ctx->flex, line->count,
				pos, ctx->item.count);

		if (!layout_flex__resolve_line(ctx, line)) {
			return false;
		}

		if (!layout_flex__place_line_items_main(ctx, line)) {
			return false;
		}

		/* fixes148 -- include cross-axis gap between wrapped lines
		 * in the total cross size so available_cross math is
		 * accurate. The actual line positioning adds the same
		 * gap in layout_flex__place_lines below. */
		if (ctx->line.count > 1) {
			ctx->cross_size += ctx->cross_gap;
		}

		ctx->cross_size += line->cross_size;
		if (ctx->main_size < line->main_size) {
			ctx->main_size = line->main_size;
		}
	}

	return true;
}

/**
 * Align items on a line.
 *
 * \param[in] ctx    Flex layout context
 * \param[in] line   Line to align items on
 * \param[in] extra  Extra line width in pixels
 */
static void layout_flex__place_line_items_cross(struct flex_ctx *ctx,
		struct flex_line_data *line, int extra)
{
	enum box_side cross_start = ctx->horizontal ? TOP : LEFT;
	size_t item_count = line->first + line->count;
	size_t i;

	for (i = line->first; i < item_count; i++) {
		struct flex_item_data *item = &ctx->item.data[i];
		struct box *b = item->box;
		int cross_free_space;
		int *box_size_cross;
		int *box_pos_cross;

		box_pos_cross = ctx->horizontal ? &b->y : &b->x;
		box_size_cross = lh__box_size_cross_ptr(ctx->horizontal, b);

		cross_free_space = line->cross_size + extra - *box_size_cross -
				lh__delta_outer_cross(ctx->flex, b);

		switch (lh__box_align_self(ctx->flex, b)) {
		default:
		case CSS_ALIGN_SELF_STRETCH:
			if (lh__box_size_cross_is_auto(ctx->horizontal, b) &&
					b->object == NULL) {
				/* fixes119 — CSS Sizing Level 3: replaced
				 * elements with intrinsic aspect ratio are
				 * NOT stretched on the cross axis even when
				 * align-self defaults to stretch. Without
				 * this guard, an <img> with CSS height: auto
				 * inside a flex line gets its height forced
				 * to the line height — visible on mactrove
				 * as the 1058x245 logo crushed to 1058x28
				 * because the flex header line is 28px tall
				 * (cross_free_space = 28-245 = -217 added to
				 * box_size_cross). Skipping the stretch
				 * leaves the image at its intrinsic
				 * aspect-preserving size; the FLEX_START
				 * positioning below still places it. */
				*box_size_cross += cross_free_space;

				/* Relayout children for stretch. */
				if (!layout_flex_item(ctx, item, b->width)) {
					return;
				}
			}
			fallthrough;
		case CSS_ALIGN_SELF_FLEX_START:
			*box_pos_cross = ctx->flex->padding[cross_start] +
					line->pos +
					lh__non_auto_margin(b, cross_start) +
					b->border[cross_start].width;
			break;

		case CSS_ALIGN_SELF_FLEX_END:
			*box_pos_cross = ctx->flex->padding[cross_start] +
					line->pos + cross_free_space +
					lh__non_auto_margin(b, cross_start) +
					b->border[cross_start].width;
			break;

		case CSS_ALIGN_SELF_BASELINE:
		case CSS_ALIGN_SELF_CENTER:
			*box_pos_cross = ctx->flex->padding[cross_start] +
					line->pos + cross_free_space / 2 +
					lh__non_auto_margin(b, cross_start) +
					b->border[cross_start].width;
			break;
		}
	}
}

/**
 * Place the lines and align the items on the line.
 *
 * \param[in] ctx  Flex layout context
 */
static void layout_flex__place_lines(struct flex_ctx *ctx)
{
	bool reversed = ctx->wrap == CSS_FLEX_WRAP_WRAP_REVERSE;
	int line_pos = reversed ? ctx->cross_size : 0;
	int post_multiplier = reversed ? 0 : 1;
	int pre_multiplier = reversed ? -1 : 0;
	int extra_remainder = 0;
	int extra = 0;
	/* fixes41 -- align-content distribution. */
	int ac_between = 0;
	int ac_free = 0;
	uint8_t ac_v = CSS_ALIGN_CONTENT_STRETCH;
	size_t i;

	if (ctx->flex->style != NULL) {
		ac_v = css_computed_align_content(ctx->flex->style);
	}

	if (ctx->available_cross != AUTO &&
	    ctx->available_cross > ctx->cross_size &&
	    ctx->line.count > 0) {
		ac_free = ctx->available_cross - ctx->cross_size;
		/* STRETCH (default) distributes evenly across all lines —
		 * the existing extra/extra_remainder path. fixes41 only
		 * overrides this when align-content is not STRETCH. */
		if (ac_v == CSS_ALIGN_CONTENT_STRETCH ||
		    ac_v == CSS_ALIGN_CONTENT_INHERIT) {
			extra = ac_free;
			extra_remainder = extra % ctx->line.count;
			extra /= ctx->line.count;
		} else {
			int pre_offset = 0;
			switch (ac_v) {
			case CSS_ALIGN_CONTENT_FLEX_END:
				pre_offset = ac_free;
				break;
			case CSS_ALIGN_CONTENT_CENTER:
				pre_offset = ac_free / 2;
				break;
			case CSS_ALIGN_CONTENT_SPACE_BETWEEN:
				if (ctx->line.count > 1) {
					ac_between = ac_free /
						(int)(ctx->line.count - 1);
				}
				break;
			case CSS_ALIGN_CONTENT_SPACE_AROUND:
				ac_between = ac_free /
						(int)ctx->line.count;
				pre_offset = ac_between / 2;
				break;
			case CSS_ALIGN_CONTENT_SPACE_EVENLY:
				ac_between = ac_free /
						(int)(ctx->line.count + 1);
				pre_offset = ac_between;
				break;
			default:
				break;
			}
			if (reversed) {
				line_pos -= pre_offset;
			} else {
				line_pos += pre_offset;
			}
		}
	}

	for (i = 0; i < ctx->line.count; i++) {
		struct flex_line_data *line = &ctx->line.data[i];

		line_pos += pre_multiplier * line->cross_size;
		line->pos = line_pos;
		line_pos += post_multiplier * line->cross_size +
				extra + extra_remainder;

		/* fixes148 -- insert cross-axis gap between wrapped lines.
		 * Skip after the last line. Reversed-wrap decrements
		 * line_pos, so flip sign. fixes41 stacks align-content
		 * between-line gap on top of the cross-axis gap. */
		if (i + 1 < ctx->line.count) {
			int line_gap_total = ctx->cross_gap + ac_between;
			if (reversed) {
				line_pos -= line_gap_total;
			} else {
				line_pos += line_gap_total;
			}
		}

		layout_flex__place_line_items_cross(ctx, line,
				extra + extra_remainder);

		if (extra_remainder > 0) {
			extra_remainder--;
		}
	}
}

/**
 * fixes167c — block-flow fallback for an unsafe flex container.
 *
 * Stacks the flex container's children top-to-bottom as a vertical
 * block flow. Used when the spec flex algorithm cannot produce a
 * valid layout (AUTO main-axis availability with content-dependent
 * children, item-count overflow, mid-pass failure of a child's base
 * size). Sets each child's x/y and the container's height so the
 * tree is consistent and walkable by the document tail (abs/rel/bbox).
 *
 * Guaranteed not to dereference NULL boxes, not to recurse beyond
 * FLEX_MAX_ITEMS children, not to assign a height larger than
 * FLEX_SAFE_MAX. Per-child layout failures are tolerated; the child
 * is left at height 0 and the next child stacks below.
 *
 * Returns true unconditionally (the caller should use the return
 * value of layout_flex itself).
 */
static bool layout_flex_fallback_block(struct box *flex, int available_width,
		html_content *content)
{
	struct box *c;
	int y;
	int avail;
	int top_offset;
	int n = 0;

	if (flex == NULL) {
		return false;
	}

	/* Sanitize the container's width so children don't inherit AUTO. */
	avail = flex_safe_dim(available_width, 0);
	if (flex->width == AUTO || flex->width <= 0 ||
			flex->width > FLEX_SAFE_MAX) {
		flex->width = avail;
	}

	top_offset = flex->padding[TOP] + flex->border[TOP].width;
	y = top_offset;

	for (c = flex->children; c != NULL; c = c->next) {
		int child_avail;
		bool ok = true;

		n++;
		if (n > FLEX_MAX_ITEMS) break;

		/* Drop flex-item residue so the child is a plain block of
		 * the flex container. */
		c->float_container = NULL;

		child_avail = flex->width;
		if (child_avail < 0) child_avail = 0;
		if (child_avail > FLEX_SAFE_MAX) child_avail = FLEX_SAFE_MAX;

		if (c->width == AUTO || c->width < 0 ||
				c->width > FLEX_SAFE_MAX) {
			c->width = child_avail;
		}

		switch (c->type) {
		case BOX_BLOCK:
		case BOX_INLINE_BLOCK:
			ok = layout_block_context(c, -1, content);
			break;
		case BOX_TABLE:
			c->float_container = c->parent;
			ok = layout_table(c, child_avail, content);
			c->float_container = NULL;
			break;
		case BOX_FLEX:
		case BOX_INLINE_FLEX:
			c->float_container = c->parent;
			ok = layout_flex(c, child_avail, content);
			c->float_container = NULL;
			break;
		case BOX_GRID:
		case BOX_INLINE_GRID:
			/* fixes168c — flex fallback now dispatches grid
			 * children through layout_grid, which has its own
			 * fallback (fixes168b). Previously these went into
			 * the default case and stayed unlaid-out. */
			c->float_container = c->parent;
			ok = layout_grid(c, child_avail, content);
			c->float_container = NULL;
			break;
		default:
			/* Unsupported child type — give it a zero-height
			 * slot, don't crash the flex fallback. */
			c->height = 0;
			ok = true;
			break;
		}
		if (!ok) {
			/* fixes168c — child layout itself failed inside the
			 * flex fallback. Convert to a zero-height continuation
			 * rather than abort the whole flex subtree. */
			c->height = 0;
			ok = true;
		}
		(void)ok;

		if (c->height == AUTO || c->height < 0) c->height = 0;
		if (c->height > FLEX_SAFE_MAX) c->height = FLEX_SAFE_MAX;
		if (c->width == AUTO || c->width < 0) c->width = 0;
		if (c->width > FLEX_SAFE_MAX) c->width = FLEX_SAFE_MAX;

		c->x = flex->padding[LEFT] + flex->border[LEFT].width +
				lh__non_auto_margin(c, LEFT) +
				c->border[LEFT].width;
		c->y = y + lh__non_auto_margin(c, TOP) +
				c->border[TOP].width;

		y = c->y + c->height + c->padding[BOTTOM] +
				c->border[BOTTOM].width +
				lh__non_auto_margin(c, BOTTOM);
		if (y < 0) y = 0;
		if (y > FLEX_SAFE_MAX) y = FLEX_SAFE_MAX;
	}

	if (flex->height == AUTO || flex->height < 0) {
		flex->height = y - top_offset;
		if (flex->height < 0) flex->height = 0;
	}
	if (flex->height > FLEX_SAFE_MAX) flex->height = FLEX_SAFE_MAX;

	macsurf_debug_log_writef(
		"FLEXSAFE fallback box=%p children=%d w=%d h=%d",
		(void *)flex, n, (int)flex->width, (int)flex->height);

	return true;
}

/**
 * Layout a flex container.
 *
 * \param[in] flex             table to layout
 * \param[in] available_width  width of containing block
 * \param[in] content          memory pool for any new boxes
 * \return  true on success, false on memory exhaustion
 */
/* fixes171 — Watchdog wrapper for layout_flex. The original body
 * is renamed to layout_flex_inner; this wrapper applies the depth
 * + iteration gate and pairs the exit. */
static bool layout_flex_inner(struct box *flex, int available_width,
		html_content *content);

bool layout_flex(struct box *flex, int available_width,
		html_content *content)
{
	bool ret;
	if (flex == NULL) return false;
	if (layout_watchdog_enter(flex)) {
		flex->height = 0;
		return true;
	}
	ret = layout_flex_inner(flex, available_width, content);
	layout_watchdog_exit();
	return ret;
}

static bool layout_flex_inner(struct box *flex, int available_width,
		html_content *content)
{
	int max_height, min_height;
	struct flex_ctx *ctx;
	bool success = false;

	/* fixes161e — per-call FLEX marker capped at first 200 calls per
	 * redraw. Counter resets when macsurf_layout_seq changes
	 * (incremented in layout_document). Child count walks flex->children
	 * once; capped at 999 as a safety. Prime suspect for both apple and
	 * huffpost; per-call granularity will pin the exact box. */
	{
		extern long macsurf_layout_seq;
		static long macsurf_flex_calls = 0;
		static long macsurf_flex_seq = -1;
		int macsurf_flex_children = 0;
		struct box *macsurf_flex_c;
		if (macsurf_flex_seq != macsurf_layout_seq) {
			macsurf_flex_calls = 0;
			macsurf_flex_seq = macsurf_layout_seq;
		}
		macsurf_flex_calls++;
		if (macsurf_flex_calls <= 200) {
			for (macsurf_flex_c = flex->children;
			     macsurf_flex_c != NULL;
			     macsurf_flex_c = macsurf_flex_c->next) {
				macsurf_flex_children++;
				if (macsurf_flex_children > 999)
					break;
			}
			macsurf_debug_log_writef(
				"LAYOUTPHASE flex #%ld box=%p type=%d w=%d h=%d children=%d",
				macsurf_flex_calls, (void *)flex,
				(int)flex->type,
				(int)flex->width, (int)flex->height,
				macsurf_flex_children);
		}
	}

	ctx = layout_flex_ctx__create(content, flex);
	if (ctx == NULL) {
		return false;
	}

	NSLOG(flex, DEEPDEBUG, "box %p: %s, available_width %i, width: %i",
			flex, ctx->horizontal ? "horizontal" : "vertical",
			available_width, flex->width);

	layout_find_dimensions(
			ctx->unit_len_ctx, available_width, -1,
			flex, flex->style, NULL, &flex->height,
			NULL, NULL, &max_height, &min_height,
			flex->margin, flex->padding, flex->border);

	available_width = min(available_width, flex->width);

	if (ctx->horizontal) {
		ctx->available_main = available_width;
		ctx->available_cross = ctx->flex->height;
	} else {
		ctx->available_main = ctx->flex->height;
		ctx->available_cross = available_width;
	}

	/* fixes167a — sanitize available_main and available_cross before
	 * they reach the base-size pass. Apple's nested column-direction
	 * flex container reaches this point with available_main = AUTO
	 * (INT_MIN); subsequent arithmetic against it overflows or
	 * produces garbage offsets. A finite fallback derived from the
	 * cross dimension and the containing block lets the pass
	 * complete; if the spec layout still can't finish, the caller
	 * routes to layout_flex_fallback_block. The 'AUTO' state itself
	 * is still meaningful to layout_flex__build_line (it disables
	 * wrap-test arithmetic), so we preserve it via a separate flag
	 * only inside that function and not by mutating ctx here.
	 *
	 * We DO sanitize negatives and absurdly-large values eagerly;
	 * AUTO is treated specially by the consumers and only converted
	 * to a finite value at use sites where it cannot be tolerated
	 * (base_size pass uses safe_avail). */
	if (ctx->available_main != AUTO) {
		if (ctx->available_main < 0 ||
				ctx->available_main > FLEX_SAFE_MAX) {
			ctx->available_main = flex_safe_fallback_dim(
				available_width);
		}
	}
	if (ctx->available_cross != AUTO) {
		if (ctx->available_cross < 0 ||
				ctx->available_cross > FLEX_SAFE_MAX) {
			ctx->available_cross = flex_safe_fallback_dim(
				available_width);
		}
	}

	NSLOG(flex, DEEPDEBUG, "box %p: available_main: %i",
			flex, ctx->available_main);
	NSLOG(flex, DEEPDEBUG, "box %p: available_cross: %i",
			flex, ctx->available_cross);

	/* fixes167b — cap on per-container item count. If the DOM
	 * produced more children than FLEX_MAX_ITEMS we cannot safely
	 * base-size them all; fall back to block flow immediately. */
	if (ctx->item.count > FLEX_MAX_ITEMS) {
		macsurf_debug_log_writef(
			"FLEXSAFE item-count over cap flex=%p count=%d",
			(void *)flex, (int)ctx->item.count);
		layout_flex_ctx__destroy(ctx);
		return layout_flex_fallback_block(flex, available_width,
				content);
	}

	/* fixes166 -- shared FLEXPHASE probes capped at first 200 flex calls
	 * per redraw (same cap as the entry-FLEX marker). Tags each phase
	 * with the flex box pointer so apple's crash site can be localized. */
	{
		extern long macsurf_layout_seq;
		static long macsurf_flexphase_seq = -1;
		static long macsurf_flexphase_calls = 0;
		if (macsurf_flexphase_seq != macsurf_layout_seq) {
			macsurf_flexphase_calls = 0;
			macsurf_flexphase_seq = macsurf_layout_seq;
		}
		macsurf_flexphase_calls++;
		if (macsurf_flexphase_calls <= 200) {
			macsurf_debug_log_writef(
				"FLEXPHASE box=%p pre-populate avail_main=%d cross=%d",
				(void *)flex, (int)ctx->available_main,
				(int)ctx->available_cross);
		}
	}

	/* fixes167b — populate now returns bool. A failing child kicks
	 * the whole container into block-flow fallback. */
	if (!layout_flex_ctx__populate_item_data(ctx, flex, available_width)) {
		layout_flex_ctx__destroy(ctx);
		macsurf_debug_log_writef(
			"FLEXSAFE populate failed flex=%p -> block fallback",
			(void *)flex);
		return layout_flex_fallback_block(flex, available_width,
				content);
	}

	{
		extern long macsurf_layout_seq;
		static long macsurf_flexphase2_seq = -1;
		static long macsurf_flexphase2_calls = 0;
		if (macsurf_flexphase2_seq != macsurf_layout_seq) {
			macsurf_flexphase2_calls = 0;
			macsurf_flexphase2_seq = macsurf_layout_seq;
		}
		macsurf_flexphase2_calls++;
		if (macsurf_flexphase2_calls <= 200) {
			macsurf_debug_log_writef(
				"FLEXPHASE box=%p post-populate items=%d",
				(void *)flex, (int)ctx->item.count);
		}
	}

	/* fixes41 -- re-order items by computed `order` before they go
	 * onto lines. Stable so equal-order items keep DOM order. */
	layout_flex__order_items(ctx);

	/* Place items onto lines. */
	success = layout_flex__collect_items_into_lines(ctx);
	if (!success) {
		layout_flex_ctx__destroy(ctx);
		macsurf_debug_log_writef(
			"FLEXSAFE collect failed flex=%p -> block fallback",
			(void *)flex);
		return layout_flex_fallback_block(flex, available_width,
				content);
	}

	/* fixes167b — runaway line count guard. The collector is bounded
	 * by item count, but a degenerate wrap pattern can still produce
	 * one line per item. Cap at FLEX_MAX_LINES; over the cap, fall
	 * back to block. */
	if (ctx->line.count > FLEX_MAX_LINES) {
		layout_flex_ctx__destroy(ctx);
		macsurf_debug_log_writef(
			"FLEXSAFE line-count over cap flex=%p lines=%d",
			(void *)flex, (int)ctx->line.count);
		return layout_flex_fallback_block(flex, available_width,
				content);
	}

	{
		extern long macsurf_layout_seq;
		static long macsurf_flexphase3_seq = -1;
		static long macsurf_flexphase3_calls = 0;
		if (macsurf_flexphase3_seq != macsurf_layout_seq) {
			macsurf_flexphase3_calls = 0;
			macsurf_flexphase3_seq = macsurf_layout_seq;
		}
		macsurf_flexphase3_calls++;
		if (macsurf_flexphase3_calls <= 200) {
			macsurf_debug_log_writef(
				"FLEXPHASE box=%p post-collect lines=%d main=%d cross=%d",
				(void *)flex, (int)ctx->line.count,
				(int)ctx->main_size, (int)ctx->cross_size);
		}
	}

	layout_flex__place_lines(ctx);

	{
		extern long macsurf_layout_seq;
		static long macsurf_flexphase4_seq = -1;
		static long macsurf_flexphase4_calls = 0;
		if (macsurf_flexphase4_seq != macsurf_layout_seq) {
			macsurf_flexphase4_calls = 0;
			macsurf_flexphase4_seq = macsurf_layout_seq;
		}
		macsurf_flexphase4_calls++;
		if (macsurf_flexphase4_calls <= 200) {
			macsurf_debug_log_writef(
				"FLEXPHASE box=%p post-place main=%d cross=%d h=%d",
				(void *)flex, (int)ctx->main_size,
				(int)ctx->cross_size, (int)flex->height);
		}
	}

	if (flex->height == AUTO) {
		flex->height = ctx->horizontal ?
				ctx->cross_size :
				ctx->main_size;
	}

	if (flex->height != AUTO) {
		if (max_height >= 0 && flex->height > max_height) {
			flex->height = max_height;
		}
		if (min_height >  0 && flex->height < min_height) {
			flex->height = min_height;
		}
	}

	success = true;

	/* fixes167b — note: failure paths above destroy ctx and return
	 * through layout_flex_fallback_block; control only reaches here
	 * on the success path, so 'cleanup:' is no longer needed as a
	 * label. The exit FLEXPHASE probe still fires for the success
	 * case. */
	{
		extern long macsurf_layout_seq;
		static long macsurf_flexphase5_seq = -1;
		static long macsurf_flexphase5_calls = 0;
		if (macsurf_flexphase5_seq != macsurf_layout_seq) {
			macsurf_flexphase5_calls = 0;
			macsurf_flexphase5_seq = macsurf_layout_seq;
		}
		macsurf_flexphase5_calls++;
		if (macsurf_flexphase5_calls <= 200) {
			macsurf_debug_log_writef(
				"FLEXPHASE box=%p exit success=%d w=%d h=%d",
				(void *)flex, (int)success,
				(int)flex->width, (int)flex->height);
		}
	}
	layout_flex_ctx__destroy(ctx);

	NSLOG(flex, DEEPDEBUG, "box %p: %s: w: %i, h: %i", flex,
			success ? "success" : "failure",
			flex->width, flex->height);
	return success;
}
