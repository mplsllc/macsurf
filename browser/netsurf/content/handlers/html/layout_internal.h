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
 * HTML layout private interface.
 */

#ifndef NETSURF_HTML_LAYOUT_INTERNAL_H
#define NETSURF_HTML_LAYOUT_INTERNAL_H

#define AUTO INT_MIN

/**
 * Layout a block formatting context.
 *
 * \param  block            BLOCK, INLINE_BLOCK, or TABLE_CELL to layout
 * \param  viewport_height  Height of viewport in pixels or -ve if unknown
 * \param  content          Memory pool for any new boxes
 * \return  true on success, false on memory exhaustion
 *
 * This function carries out layout of a block and its children, as described
 * in CSS 2.1 9.4.1.
 */
bool layout_block_context(
		struct box *block,
		int viewport_height,
		html_content *content);

/**
 * Layout a table.
 *
 * \param  table            table to layout
 * \param  available_width  width of containing block
 * \param  content          memory pool for any new boxes
 * \return  true on success, false on memory exhaustion
 */
bool layout_table(
		struct box *table,
		int available_width,
		html_content *content);

/**
 * Layout a flex container.
 *
 * \param[in] flex             table to layout
 * \param[in] available_width  width of containing block
 * \param[in] content          memory pool for any new boxes
 * \return  true on success, false on memory exhaustion
 */
bool layout_flex(
		struct box *flex,
		int available_width,
		html_content *content);

/**
 * fixes75: layout a CSS Grid container.
 *
 * V1 reads -macsurf-grid: <cols> [<rows>]. Lays out children row-major
 * in <cols> equal-width columns. Row heights track tallest child in
 * each row. <rows> is currently ignored (auto-rows behaviour).
 *
 * \param[in] grid             grid container to lay out
 * \param[in] available_width  width of containing block
 * \param[in] content          memory pool for any new boxes
 * \return true on success, false on memory exhaustion
 */
bool layout_grid(
		struct box *grid,
		int available_width,
		html_content *content);

typedef uint8_t (*css_len_func)(
		const css_computed_style *style,
		css_fixed *length, css_unit *unit);
typedef uint8_t (*css_border_style_func)(
		const css_computed_style *style);
typedef uint8_t (*css_border_color_func)(
		const css_computed_style *style,
		css_color *color);

/** Array of per-side access functions for computed style margins. */
extern const css_len_func margin_funcs[4];

/** Array of per-side access functions for computed style paddings. */
extern const css_len_func padding_funcs[4];

/** Array of per-side access functions for computed style border_widths. */
extern const css_len_func border_width_funcs[4];

/** Array of per-side access functions for computed style border styles. */
extern const css_border_style_func border_style_funcs[4];

/** Array of per-side access functions for computed style border colors. */
extern const css_border_color_func border_color_funcs[4];

/** Layout helper: Check whether box is a float. */
static inline bool lh__box_is_float_box(const struct box *b)
{
	return b->type == BOX_FLOAT_LEFT ||
	       b->type == BOX_FLOAT_RIGHT;
}

/** Layout helper: Check whether box takes part in inline flow. */
static inline bool lh__box_is_inline_flow(const struct box *b)
{
	return b->type == BOX_INLINE ||
	       b->type == BOX_INLINE_FLEX ||
	       b->type == BOX_INLINE_BLOCK ||
	       b->type == BOX_TEXT ||
	       b->type == BOX_INLINE_END;
}

/** Layout helper: Check whether box takes part in inline flow. */
static inline bool lh__box_is_flex_container(const struct box *b)
{
	return b->type == BOX_FLEX ||
	       b->type == BOX_INLINE_FLEX;
}

/** Layout helper: Check whether box takes part in inline flow. */
static inline bool lh__box_is_flex_item(const struct box *b)
{
	return (b->parent != NULL) && lh__box_is_flex_container(b->parent);
}

/** Layout helper: Check whether box is inline level. (Includes BR.) */
static inline bool lh__box_is_inline_level(const struct box *b)
{
	return lh__box_is_inline_flow(b) ||
	       b->type == BOX_BR;
}

/** Layout helper: Check whether box is inline level. (Includes BR, floats.) */
static inline bool lh__box_is_inline_content(const struct box *b)
{
	return lh__box_is_float_box(b) ||
	       lh__box_is_inline_level(b);
}

/** Layout helper: Check whether box is an object. */
static inline bool lh__box_is_object(const struct box *b)
{
	return b->object ||
	       (b->flags & (IFRAME | REPLACE_DIM));
}

/** Layout helper: Check whether box is replaced. */
static inline bool lh__box_is_replace(const struct box *b)
{
	return b->gadget ||
	       lh__box_is_object(b);
}

/** Layout helper: Check for CSS border on given side. */
static inline bool lh__have_border(
		enum box_side side,
		const css_computed_style *style)
{
	return border_style_funcs[side](style) != CSS_BORDER_STYLE_NONE;
}

static inline bool lh__box_is_absolute(const struct box *b)
{
	return css_computed_position(b->style) == CSS_POSITION_ABSOLUTE ||
	       css_computed_position(b->style) == CSS_POSITION_FIXED;
}

static inline bool lh__flex_main_is_horizontal(const struct box *flex)
{
	const css_computed_style *style = flex->style;

	assert(style != NULL);

	switch (css_computed_flex_direction(style)) {
	default:                        /* Fallthrough. */
	case CSS_FLEX_DIRECTION_ROW:    /* Fallthrough. */
	case CSS_FLEX_DIRECTION_ROW_REVERSE:
		return true;
	case CSS_FLEX_DIRECTION_COLUMN: /* Fallthrough. */
	case CSS_FLEX_DIRECTION_COLUMN_REVERSE:
		return false;
	}
}

static inline bool lh__flex_direction_reversed(const struct box *flex)
{
	switch (css_computed_flex_direction(flex->style)) {
	default:                             /* Fallthrough. */
	case CSS_FLEX_DIRECTION_ROW_REVERSE: /* Fallthrough. */
	case CSS_FLEX_DIRECTION_COLUMN_REVERSE:
		return true;
	case CSS_FLEX_DIRECTION_ROW: /* Fallthrough. */
	case CSS_FLEX_DIRECTION_COLUMN:
		return false;
	}
}

static inline int lh__non_auto_margin(const struct box *b, enum box_side side)
{
	return (b->margin[side] == AUTO) ? 0 : b->margin[side];
}

static inline int lh__delta_outer_height(const struct box *b)
{
	return b->padding[TOP] +
	       b->padding[BOTTOM] +
	       b->border[TOP].width +
	       b->border[BOTTOM].width +
	       lh__non_auto_margin(b, TOP) +
	       lh__non_auto_margin(b, BOTTOM);
}

static inline int lh__delta_outer_width(const struct box *b)
{
	return b->padding[LEFT] +
	       b->padding[RIGHT] +
	       b->border[LEFT].width +
	       b->border[RIGHT].width +
	       lh__non_auto_margin(b, LEFT) +
	       lh__non_auto_margin(b, RIGHT);
}

static inline int lh__delta_outer_main(
		const struct box *flex,
		const struct box *b)
{
	if (lh__flex_main_is_horizontal(flex)) {
		return lh__delta_outer_width(b);
	} else {
		return lh__delta_outer_height(b);
	}
}

static inline int lh__delta_outer_cross(
		const struct box *flex,
		const struct box *b)
{
	if (lh__flex_main_is_horizontal(flex) == false) {
		return lh__delta_outer_width(b);
	} else {
		return lh__delta_outer_height(b);
	}
}

static inline int *lh__box_size_main_ptr(
		bool horizontal,
		struct box *b)
{
	return horizontal ? &b->width : &b->height;
}

static inline int *lh__box_size_cross_ptr(
		bool horizontal,
		struct box *b)
{
	return horizontal ? &b->height : &b->width;
}

static inline int lh__box_size_main(
		bool horizontal,
		const struct box *b)
{
	return horizontal ? b->width : b->height;
}

static inline int lh__box_size_cross(
		bool horizontal,
		const struct box *b)
{
	return horizontal ? b->height : b->width;
}

static inline bool lh__box_size_cross_is_auto(
		bool horizontal,
		struct box *b)
{
	css_fixed length;
	css_unit unit;

	if (horizontal) {
		return css_computed_height(b->style,
				&length, &unit) == CSS_HEIGHT_AUTO;
	} else {
		return css_computed_width(b->style,
				&length, &unit) == CSS_WIDTH_AUTO;
	}
}

static inline enum css_align_self_e lh__box_align_self(
		const struct box *flex,
		const struct box *item)
{
	enum css_align_self_e align_self = css_computed_align_self(item->style);

	if (align_self == CSS_ALIGN_SELF_AUTO) {
		align_self = css_computed_align_items(flex->style);
	}

	return align_self;
}

/**
 * Determine width of margin, borders, and padding on one side of a box.
 *
 * \param unit_len_ctx  CSS length conversion context for document
 * \param style    style to measure
 * \param side     side of box to measure
 * \param margin   whether margin width is required
 * \param border   whether border width is required
 * \param padding  whether padding width is required
 * \param fixed    increased by sum of fixed margin, border, and padding
 * \param frac     increased by sum of fractional margin and padding
 */
static inline void calculate_mbp_width(
		const css_unit_ctx *unit_len_ctx,
		const css_computed_style *style,
		unsigned int side,
		bool margin,
		bool border,
		bool padding,
		int *fixed,
		float *frac)
{
	css_fixed value = 0;
	css_unit unit = CSS_UNIT_PX;

	assert(style);

	/* margin */
	if (margin) {
		enum css_margin_e type;

		type = margin_funcs[side](style, &value, &unit);
		if (type == CSS_MARGIN_SET) {
			if (unit == CSS_UNIT_PCT) {
				*frac += FIXTOFLT(FDIV(value, F_100));
			} else {
				*fixed += FIXTOINT(css_unit_len2device_px(
						style, unit_len_ctx,
						value, unit));
			}
		}
	}

	/* border */
	if (border) {
		if (lh__have_border(side, style)) {
			border_width_funcs[side](style, &value, &unit);

			*fixed += FIXTOINT(css_unit_len2device_px(
					style, unit_len_ctx,
					value, unit));
		}
	}

	/* padding */
	if (padding) {
		padding_funcs[side](style, &value, &unit);
		if (unit == CSS_UNIT_PCT) {
			*frac += FIXTOFLT(FDIV(value, F_100));
		} else {
			*fixed += FIXTOINT(css_unit_len2device_px(
					style, unit_len_ctx,
					value, unit));
		}
	}
}

/**
 * Adjust a specified width or height for the box-sizing property.
 *
 * This turns the specified dimension into a content-box dimension.
 *
 * \param  unit_len_ctx          Length conversion context
 * \param  box		    gadget to adjust dimensions of
 * \param  available_width  width of containing block
 * \param  setwidth	    set true if the dimension to be tweaked is a width,
 *				else set false for a height
 * \param  dimension	    current value for given width/height dimension.
 *				updated to new value after consideration of
 *				gadget properties.
 */
static inline void layout_handle_box_sizing(
		const css_unit_ctx *unit_len_ctx,
		const struct box *box,
		int available_width,
		bool setwidth,
		int *dimension)
{
	enum css_box_sizing_e bs;

	assert(box && box->style);

	bs = css_computed_box_sizing(box->style);

	if (bs == CSS_BOX_SIZING_BORDER_BOX) {
		int orig = *dimension;
		int fixed = 0;
		float frac = 0;

		calculate_mbp_width(unit_len_ctx, box->style,
				setwidth ? LEFT : TOP,
				false, true, true, &fixed, &frac);
		calculate_mbp_width(unit_len_ctx, box->style,
				setwidth ? RIGHT : BOTTOM,
				false, true, true, &fixed, &frac);
		orig -= frac * available_width + fixed;
		*dimension = orig > 0 ? orig : 0;
	}
}

/**
 * Calculate width, height, and thickness of margins, paddings, and borders.
 *
 * \param  unit_len_ctx     Length conversion context
 * \param  available_width  width of containing block
 * \param  viewport_height  height of viewport in pixels or -ve if unknown
 * \param  box              current box
 * \param  style            style giving width, height, margins, paddings,
 *                          and borders
 * \param  width            updated to width, may be NULL
 * \param  height           updated to height, may be NULL
 * \param  max_width        updated to max-width, may be NULL
 * \param  min_width        updated to min-width, may be NULL
 * \param  max_height       updated to max-height, may be NULL
 * \param  min_height       updated to min-height, may be NULL
 * \param  margin           filled with margins, may be NULL
 * \param  padding          filled with paddings, may be NULL
 * \param  border           filled with border widths, may be NULL
 */
static inline void layout_find_dimensions(
		const css_unit_ctx *unit_len_ctx,
		int available_width,
		int viewport_height,
		const struct box *box,
		const css_computed_style *style,
		int *width,
		int *height,
		int *max_width,
		int *min_width,
		int *max_height,
		int *min_height,
		int margin[4],
		int padding[4],
		struct box_border border[4])
{
	struct box *containing_block = NULL;
	unsigned int i;

	if (width) {
		if (css_computed_width_px(style, unit_len_ctx,
				available_width, width) == CSS_WIDTH_SET) {
			layout_handle_box_sizing(unit_len_ctx, box,
					available_width, true, width);
		} else {
			*width = AUTO;
		}
	}

	if (height) {
		enum css_height_e htype;
		css_fixed value = 0;
		css_unit unit = CSS_UNIT_PX;

		htype = css_computed_height(style, &value, &unit);

		if (htype == CSS_HEIGHT_SET) {
			if (unit == CSS_UNIT_PCT) {
				enum css_height_e cbhtype;

				if (css_computed_position(box->style) ==
						CSS_POSITION_ABSOLUTE &&
						box->parent) {
					/* Box is absolutely positioned */
					assert(box->float_container);
					containing_block = box->float_container;
				} else if (box->float_container &&
					css_computed_position(box->style) !=
						CSS_POSITION_ABSOLUTE &&
					(css_computed_float(box->style) ==
						CSS_FLOAT_LEFT ||
					 css_computed_float(box->style) ==
						CSS_FLOAT_RIGHT)) {
					/* Box is a float */
					assert(box->parent &&
						box->parent->parent &&
						box->parent->parent->parent);

					containing_block =
						box->parent->parent->parent;
				} else if (box->parent && box->parent->type !=
						BOX_INLINE_CONTAINER) {
					/* Box is a block level element */
					containing_block = box->parent;
				} else if (box->parent && box->parent->type ==
						BOX_INLINE_CONTAINER) {
					/* Box is an inline block */
					assert(box->parent->parent);
					containing_block = box->parent->parent;
				}

				if (containing_block) {
					css_fixed f = 0;
					css_unit u = CSS_UNIT_PX;

					cbhtype = css_computed_height(
							containing_block->style,
							&f, &u);
				}

				if (containing_block &&
					containing_block->height != AUTO &&
					(css_computed_position(box->style) ==
							CSS_POSITION_ABSOLUTE ||
						cbhtype == CSS_HEIGHT_SET)) {
					/* Box is absolutely positioned or its
					 * containing block has a valid
					 * specified height.
					 * (CSS 2.1 Section 10.5) */
					*height = FPCT_OF_INT_TOINT(value,
						containing_block->height);
				} else if ((!box->parent ||
						!box->parent->parent) &&
						viewport_height >= 0) {
					/* If root element or it's child
					 * (HTML or BODY) */
					*height = FPCT_OF_INT_TOINT(value,
							viewport_height);
				} else {
					/* precentage height not permissible
					 * treat height as auto */
					*height = AUTO;
				}
			} else {
				*height = FIXTOINT(css_unit_len2device_px(
						style, unit_len_ctx,
						value, unit));
			}
		} else {
			*height = AUTO;
		}

		if (*height != AUTO) {
			layout_handle_box_sizing(unit_len_ctx, box,
					available_width, false, height);
		}
	}

	if (max_width) {
		enum css_max_width_e type;
		css_fixed value = 0;
		css_unit unit = CSS_UNIT_PX;

		type = css_computed_max_width(style, &value, &unit);

		if (type == CSS_MAX_WIDTH_SET) {
			if (unit == CSS_UNIT_PCT) {
				*max_width = FPCT_OF_INT_TOINT(value,
						available_width);
			} else {
				*max_width = FIXTOINT(css_unit_len2device_px(
						style, unit_len_ctx,
						value, unit));
			}
		} else {
			/* Inadmissible */
			*max_width = -1;
		}

		if (*max_width != -1) {
			layout_handle_box_sizing(unit_len_ctx, box,
					available_width, true, max_width);
		}
	}

	if (min_width) {
		enum css_min_width_e type;
		css_fixed value = 0;
		css_unit unit = CSS_UNIT_PX;

		type = ns_computed_min_width(style, &value, &unit);

		if (type == CSS_MIN_WIDTH_SET) {
			if (unit == CSS_UNIT_PCT) {
				*min_width = FPCT_OF_INT_TOINT(value,
						available_width);
			} else {
				*min_width = FIXTOINT(css_unit_len2device_px(
						style, unit_len_ctx,
						value, unit));
			}
		} else {
			/* Inadmissible */
			*min_width = 0;
		}

		if (*min_width != 0) {
			layout_handle_box_sizing(unit_len_ctx, box,
					available_width, true, min_width);
		}
	}

	if (max_height) {
		enum css_max_height_e type;
		css_fixed value = 0;
		css_unit unit = CSS_UNIT_PX;

		type = css_computed_max_height(style, &value, &unit);

		if (type == CSS_MAX_HEIGHT_SET) {
			if (unit == CSS_UNIT_PCT) {
				/* TODO: handle percentage */
				*max_height = -1;
			} else {
				*max_height = FIXTOINT(css_unit_len2device_px(
						style, unit_len_ctx,
						value, unit));
			}
		} else {
			/* Inadmissible */
			*max_height = -1;
		}
	}

	if (min_height) {
		enum css_min_height_e type;
		css_fixed value = 0;
		css_unit unit = CSS_UNIT_PX;

		type = ns_computed_min_height(style, &value, &unit);

		if (type == CSS_MIN_HEIGHT_SET) {
			if (unit == CSS_UNIT_PCT) {
				/* TODO: handle percentage */
				*min_height = 0;
			} else {
				*min_height = FIXTOINT(css_unit_len2device_px(
						style, unit_len_ctx,
						value, unit));
			}
		} else {
			/* Inadmissible */
			*min_height = 0;
		}
	}

	for (i = 0; i != 4; i++) {
		if (margin) {
			enum css_margin_e type = CSS_MARGIN_AUTO;
			css_fixed value = 0;
			css_unit unit = CSS_UNIT_PX;

			type = margin_funcs[i](style, &value, &unit);

			if (type == CSS_MARGIN_SET) {
				if (unit == CSS_UNIT_PCT) {
					margin[i] = FPCT_OF_INT_TOINT(value,
							available_width);
				} else {
					margin[i] = FIXTOINT(css_unit_len2device_px(
							style, unit_len_ctx,
							value, unit));
				}
			} else {
				margin[i] = AUTO;
			}
		}

		if (padding) {
			css_fixed value = 0;
			css_unit unit = CSS_UNIT_PX;

			padding_funcs[i](style, &value, &unit);

			if (unit == CSS_UNIT_PCT) {
				padding[i] = FPCT_OF_INT_TOINT(value,
						available_width);
			} else {
				padding[i] = FIXTOINT(css_unit_len2device_px(
						style, unit_len_ctx,
						value, unit));
			}
		}

		/* Table cell borders are populated in table.c */
		if (border && box->type != BOX_TABLE_CELL) {
			enum css_border_style_e bstyle = CSS_BORDER_STYLE_NONE;
			css_color color = 0;
			css_fixed value = 0;
			css_unit unit = CSS_UNIT_PX;

			border_width_funcs[i](style, &value, &unit);
			bstyle = border_style_funcs[i](style);
			border_color_funcs[i](style, &color);

			border[i].style = bstyle;
			border[i].c = color;

			if (bstyle == CSS_BORDER_STYLE_HIDDEN ||
					bstyle == CSS_BORDER_STYLE_NONE)
				/* spec unclear: following Mozilla */
				border[i].width = 0;
			else {
				border[i].width = FIXTOINT(css_unit_len2device_px(
						style, unit_len_ctx,
						value, unit));
				/* Sanity clamp: if computed style / unit ctx is
				 * incompletely initialised, css_unit_len2device_px
				 * can return a huge garbage value and border width
				 * then feeds back into layout_get_box_bbox as
				 * *desc_y0 = -border[TOP].width, producing things
				 * like desc_y0 = -39845888 that trick the render
				 * walker's clip test. Clamp to a sane range. */
				if (border[i].width < 0 ||
						border[i].width > 1000)
					border[i].width = 0;
			}

			/* Special case for border-collapse: make all borders
			 * on table/table-row-group/table-row zero width. */
			if (css_computed_border_collapse(style) ==
					CSS_BORDER_COLLAPSE_COLLAPSE &&
					(box->type == BOX_TABLE ||
					 box->type == BOX_TABLE_ROW_GROUP ||
					 box->type == BOX_TABLE_ROW))
				border[i].width = 0;
		}
	}
}

#endif
