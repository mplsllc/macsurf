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
 */
static void layout_flex_ctx__populate_item_data(
		const struct flex_ctx *ctx,
		const struct box *flex,
		int available_width)
{
	size_t i = 0;
	bool horizontal = ctx->horizontal;
	struct box *b;

	for (b = flex->children; b != NULL; b = b->next) {
		struct flex_item_data *item = &ctx->item.data[i++];

		b->float_container = b->parent;
		layout_find_dimensions(ctx->unit_len_ctx, available_width, -1,
				b, b->style, &b->width, &b->height,
				horizontal ? &item->max_main : &item->max_cross,
				horizontal ? &item->min_main : &item->min_cross,
				horizontal ? &item->max_cross : &item->max_main,
				horizontal ? &item->min_cross : &item->min_main,
				b->margin, b->padding, b->border);
		b->float_container = NULL;

		NSLOG(flex, DEEPDEBUG, "flex-item box: %p: width: %i",
				b, b->width);

		item->box = b;
		item->basis = css_computed_flex_basis(b->style,
				&item->basis_length, &item->basis_unit);

		css_computed_flex_shrink(b->style, &item->shrink);
		css_computed_flex_grow(b->style, &item->grow);

		layout_flex__base_and_main_sizes(ctx, item, available_width);
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
				if (ctx->main_reversed) {
					main_pos -= ctx->main_gap;
				} else {
					main_pos += ctx->main_gap;
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
			if (lh__box_size_cross_is_auto(ctx->horizontal, b)) {
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
	size_t i;

	if (ctx->available_cross != AUTO &&
	    ctx->available_cross > ctx->cross_size &&
	    ctx->line.count > 0) {
		extra = ctx->available_cross - ctx->cross_size;

		extra_remainder = extra % ctx->line.count;
		extra /= ctx->line.count;
	}

	for (i = 0; i < ctx->line.count; i++) {
		struct flex_line_data *line = &ctx->line.data[i];

		line_pos += pre_multiplier * line->cross_size;
		line->pos = line_pos;
		line_pos += post_multiplier * line->cross_size +
				extra + extra_remainder;

		/* fixes148 -- insert cross-axis gap between wrapped lines.
		 * Skip after the last line. Reversed-wrap decrements
		 * line_pos, so flip sign. */
		if (i + 1 < ctx->line.count) {
			if (reversed) {
				line_pos -= ctx->cross_gap;
			} else {
				line_pos += ctx->cross_gap;
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
 * Layout a flex container.
 *
 * \param[in] flex             table to layout
 * \param[in] available_width  width of containing block
 * \param[in] content          memory pool for any new boxes
 * \return  true on success, false on memory exhaustion
 */
bool layout_flex(struct box *flex, int available_width,
		html_content *content)
{
	int max_height, min_height;
	struct flex_ctx *ctx;
	bool success = false;

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

	NSLOG(flex, DEEPDEBUG, "box %p: available_main: %i",
			flex, ctx->available_main);
	NSLOG(flex, DEEPDEBUG, "box %p: available_cross: %i",
			flex, ctx->available_cross);

	layout_flex_ctx__populate_item_data(ctx, flex, available_width);

	/* Place items onto lines. */
	success = layout_flex__collect_items_into_lines(ctx);
	if (!success) {
		goto cleanup;
	}

	layout_flex__place_lines(ctx);

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

cleanup:
	layout_flex_ctx__destroy(ctx);

	NSLOG(flex, DEEPDEBUG, "box %p: %s: w: %i, h: %i", flex,
			success ? "success" : "failure",
			flex->width, flex->height);
	return success;
}
