/*
 * This file is part of LibCSS
 * Licensed under the MIT License,
 *                http://www.opensource.org/licenses/mit-license.php
 * Copyright 2009 John-Mark Bell <jmb@netsurf-browser.org>
 */

#ifndef libcss_computed_h_
#define libcss_computed_h_

#ifdef __cplusplus
extern "C"
{
#endif

#include <libwapcaplet/libwapcaplet.h>

#include <libcss/errors.h>
#include <libcss/functypes.h>
#include <libcss/properties.h>
#include <libcss/types.h>
#include <libcss/unit.h>

struct css_hint;
struct css_select_handler;

typedef struct css_computed_counter {
	lwc_string *name;
	css_fixed value;
} css_computed_counter;

typedef struct css_computed_clip_rect {
	css_fixed top;
	css_fixed right;
	css_fixed bottom;
	css_fixed left;

	css_unit tunit;
	css_unit runit;
	css_unit bunit;
	css_unit lunit;

	bool top_auto;
	bool right_auto;
	bool bottom_auto;
	bool left_auto;
} css_computed_clip_rect;

enum css_computed_content_type {
	CSS_COMPUTED_CONTENT_NONE		= 0,
	CSS_COMPUTED_CONTENT_STRING		= 1,
	CSS_COMPUTED_CONTENT_URI		= 2,
	CSS_COMPUTED_CONTENT_COUNTER		= 3,
	CSS_COMPUTED_CONTENT_COUNTERS		= 4,
	CSS_COMPUTED_CONTENT_ATTR		= 5,
	CSS_COMPUTED_CONTENT_OPEN_QUOTE		= 6,
	CSS_COMPUTED_CONTENT_CLOSE_QUOTE	= 7,
	CSS_COMPUTED_CONTENT_NO_OPEN_QUOTE	= 8,
	CSS_COMPUTED_CONTENT_NO_CLOSE_QUOTE	= 9
};

typedef struct css_computed_content_item {
	uint8_t type;
	union {
		lwc_string *string;
		lwc_string *uri;
		lwc_string *attr;
		struct {
			lwc_string *name;
			uint8_t style;
		} counter;
		struct {
			lwc_string *name;
			lwc_string *sep;
			uint8_t style;
		} counters;
	} data;
} css_computed_content_item;

css_error css_computed_style_destroy(css_computed_style *style);

css_error css_computed_style_compose(
		const css_computed_style *restrict parent,
		const css_computed_style *restrict child,
		const css_unit_ctx *unit_ctx,
		css_computed_style **restrict result);

/******************************************************************************
 * speciality formatters                                                      *
 ******************************************************************************/

/**
 * Format a value into a text string controled by a list style
 *
 * \param[in] style The computed style to use for formatting
 * \param[in] value The value to format
 * \param[out] buffer The buffer to recive the formatted result
 * \param[in] buffer_length The length of the buffer
 * \param[out] format_length The complete length of the formatted result which
 *                             may exceed buffer_length in which case the
 *                             buffer data will not be complete.
 * \return CSS_OK on success and the buffer and format_length updated
 */
css_error css_computed_format_list_style(
		const css_computed_style *style,
		int value,
		char *buffer,
		size_t buffer_length,
		size_t *format_length);

/******************************************************************************
 * Property accessors                                                         *
 ******************************************************************************/

uint8_t css_computed_letter_spacing(
		const css_computed_style *style,
		css_fixed *length, css_unit *unit);

uint8_t css_computed_outline_color(
		const css_computed_style *style, css_color *color);

uint8_t css_computed_outline_width(
		const css_computed_style *style,
		css_fixed *length, css_unit *unit);

uint8_t css_computed_border_spacing(
		const css_computed_style *style,
		css_fixed *hlength, css_unit *hunit,
		css_fixed *vlength, css_unit *vunit);

uint8_t css_computed_word_spacing(
		const css_computed_style *style,
		css_fixed *length, css_unit *unit);

uint8_t css_computed_counter_increment(
		const css_computed_style *style,
		const css_computed_counter **counters);

uint8_t css_computed_counter_reset(
		const css_computed_style *style,
		const css_computed_counter **counters);

uint8_t css_computed_cursor(
		const css_computed_style *style,
		lwc_string ***urls);

uint8_t css_computed_clip(
		const css_computed_style *style,
		css_computed_clip_rect *rect);

uint8_t css_computed_content(
		const css_computed_style *style,
		const css_computed_content_item **content);

uint8_t css_computed_vertical_align(
		const css_computed_style *style,
		css_fixed *length, css_unit *unit);

uint8_t css_computed_font_size(
		const css_computed_style *style,
		css_fixed *length, css_unit *unit);

uint8_t css_computed_border_top_width(
		const css_computed_style *style,
		css_fixed *length, css_unit *unit);

uint8_t css_computed_border_right_width(
		const css_computed_style *style,
		css_fixed *length, css_unit *unit);

uint8_t css_computed_border_bottom_width(
		const css_computed_style *style,
		css_fixed *length, css_unit *unit);

uint8_t css_computed_border_left_width(
		const css_computed_style *style,
		css_fixed *length, css_unit *unit);

uint8_t css_computed_border_radius(
		const css_computed_style *style,
		css_fixed *length, css_unit *unit);

uint8_t css_computed_box_shadow(
		const css_computed_style *style,
		int32_t *integer);

/* fixes361b - second box-shadow packed value. Returns 0 if no second
 * shadow set, otherwise the packed bsh int32 in the same format as
 * css_computed_box_shadow's *integer (h<<24 | v<<16 | inset<<15 |
 * rgb555). Used to paint a second inset bevel for Platinum 3D effect. */
int32_t css_computed_box_shadow_2(
		const css_computed_style *style);

/* fixes362 - third box-shadow packed value. Same format as
 * css_computed_box_shadow_2; the Platinum convention is two insets
 * (light + dark bevels) followed by one outer drop shadow. */
int32_t css_computed_box_shadow_3(
		const css_computed_style *style);

/* fixes364 - horizontal stripe background. Two RGB555 colors packed
 * into one int32: bit 31 = set flag, bits 15..29 = c2 (high), bits
 * 0..14 = c1 (low). Mapped from `repeating-linear-gradient(to bottom,
 * ...)` by cssh_css.c so Platinum title bars get real alternating-row
 * stripes. Returns 0 when unset. */
int32_t css_computed_macsurf_hstripe_bg(
		const css_computed_style *style);

/* fixes365c - two-layer 2x2 dot-grid background pattern. Two RGB555
 * colors packed into one int32: bit 31 = set flag, bits 15..29 = c2
 * (horizontal stripe), bits 0..14 = c1 (vertical stripe). Mapped from
 * the two-layer `linear-gradient(c1 1px, transparent 1px),
 * linear-gradient(90deg, c2 1px, transparent 1px)` + `background-size:
 * 2px 2px` pattern by cssh_css.c so mactrove's Platinum body texture
 * paints as alternating 1px stripes on a 2x2 grid. Returns 0 unset. */
int32_t css_computed_macsurf_dotgrid(
		const css_computed_style *style);

uint8_t css_computed_macsurf_gradient(
		const css_computed_style *style,
		int32_t *color);

uint8_t css_computed_macsurf_text_shadow(
		const css_computed_style *style,
		int32_t *packed);

uint8_t css_computed_macsurf_transform(
		const css_computed_style *style,
		int32_t *packed);

/* fixes73: scale companion. Returns the raw scale packing
 * (bits 31..16 scale_x Q8.8, bits 15..0 scale_y Q8.8).
 * Identity = 0x01000100. */
int32_t css_computed_macsurf_transform_b(
		const css_computed_style *style);

/* fixes75: -macsurf-grid: <cols> [<rows>]. packed = (cols << 16) | rows.
 * cols = number of columns in the grid (1..255 reasonable).
 * rows = 0 means auto-rows (size to content). */
uint8_t css_computed_macsurf_grid(
		const css_computed_style *style,
		int32_t *packed);

/* fixes118: pointer to 8-int track-width array, or NULL when no
 * explicit grid-template-columns tracks are set. Each int32 packs
 * (unit << 28) | value. Lifetime tied to the computed style. */
const int32_t *css_computed_macsurf_grid_tracks(
		const css_computed_style *style);

/* fixes150: returns a pointer to an 8-int row-track descriptor array,
 * or NULL when no explicit grid-template-rows tracks are set. Same
 * encoding as macsurf_grid_tracks. Lifetime tied to the computed
 * style. */
const int32_t *css_computed_macsurf_grid_row_tracks(
		const css_computed_style *style);

/* fixes344b: returns a pointer to a 2-element css_color array carrying
 * full ARGB for the gradient's two stops, or NULL when both stops are
 * fully opaque (the painter falls back to the existing RGB565+R4G6B4
 * unpack path in that case). When non-NULL, format is [c1, c2] with
 * alpha in the high byte of each. Used by the gradient plotter to do
 * per-pixel alpha blending against the underlying surface. */
const css_color *css_computed_macsurf_gradient_full(
		const css_computed_style *style);

/* fixes345: returns a pointer to a 4-int radial-gradient size+position
 * array, or NULL when the rule had no explicit size/position prefix.
 * Format: [size_x_px, size_y_px, pos_x_percent_x100, pos_y_percent_x100]
 * with -1 sentinels for any field that wasn't supplied. Painter uses
 * these to place a small concentrated radial ellipse at the author's
 * intended position instead of filling the bounding rect. */
const int32_t *css_computed_macsurf_gradient_radial(
		const css_computed_style *style);

/* fixes365b: returns a pointer to a 7-int extended-linear-gradient
 * descriptor, or NULL when the legacy 2-stop cardinal fast path suffices.
 * Format:
 *   [0] angle in degrees (0..359; full precision)
 *   [1] position 0 as percent×100 (0..10000)
 *   [2] position 1 as percent×100
 *   [3] position 2 as percent×100 (0 when only 2 stops)
 *   [4] colour 0 as css_color (ARGB)
 *   [5] colour 1 as css_color
 *   [6] colour 2 as css_color (0 when only 2 stops)
 * Lifetime tied to the computed style. Painter uses this for diagonal
 * angles (45/135/225/315) and any 3-stop gradient with positions. */
const int32_t *css_computed_macsurf_gradient_stops(
		const css_computed_style *style);

/* fixes151: -macsurf-grid-col-span as a uint8_t. 0 = unset (caller
 * treats as 1). 1..254 = literal span count. 255 = "fill the
 * remainder of the row" sentinel (from `grid-column: 1 / -1`-style
 * preprocessor rewrites). Returns only the low byte of the packed
 * placement storage for backwards compatibility with fixes151 callers. */
uint8_t css_computed_macsurf_grid_col_span(
		const css_computed_style *style);

/* fixes158: full packed grid placement int32. See
 * autogenerated_computed.h for the bit layout:
 *   bits  0..7  col_span    - 0=unset, 1..254 literal, 255 fill row
 *   bits  8..15 col_start   - 0=auto, 1..254 explicit line
 *   bits 16..23 row_start   - 0=auto, 1..254 explicit line
 *   bits 24..31 row_span    - 0=unset, 1..254 literal, 255 fill
 * 0 means "no placement set at all" (auto-flow, span 1, single row). */
int32_t css_computed_macsurf_grid_placement(
		const css_computed_style *style);

/* fixes152: aspect-ratio packed as (num << 16) | denom.
 * 0 = unset (no aspect-ratio constraint). Layout uses width = h * num
 * / denom and height = w * denom / num to derive the missing
 * dimension when one of width/height is set and the other is auto. */
int32_t css_computed_aspect_ratio(
		const css_computed_style *style);

/* fixes191b: background-size packed int32.
 *   bits 31..16: w_code (int16: 0=auto, +N=px, -1=cover, -2=contain)
 *   bits 15..0:  h_code (int16: same encoding)
 *   0 (whole word) = unset (treat as `auto auto`). */
int32_t css_computed_background_size(
		const css_computed_style *style);

uint8_t css_computed_pointer_events(
		const css_computed_style *style);

/* fixes275 (#65): grid-auto-flow value. Returns one of
 * CSS_MACSURF_GRID_FLOW_{ROW,COLUMN,ROW_DENSE,COLUMN_DENSE}.
 * Defaults to ROW (sparse row-major) when unset. */
uint8_t css_computed_macsurf_grid_flow(const css_computed_style *style);

/* fixes353 (#73): accent-color. Returns CSS_ACCENT_COLOR_*; when the
 * return is CSS_ACCENT_COLOR_COLOR, *color receives the css_color.
 * For AUTO / CURRENTCOLOR / INHERIT, *color is 0. */
uint8_t css_computed_accent_color(const css_computed_style *style,
		css_color *color);

/* fixes353 (#73): caret-color. Returns CSS_CARET_COLOR_*; when the
 * return is CSS_CARET_COLOR_COLOR, *color receives the css_color. */
uint8_t css_computed_caret_color(const css_computed_style *style,
		css_color *color);

/* fixes354 (#82): box-decoration-break. Returns
 * CSS_BOX_DECORATION_BREAK_SLICE (default) or _CLONE. */
uint8_t css_computed_box_decoration_break(
		const css_computed_style *style);

/* fixes355 (#58): tab-size. Returns the integer number of space
 * widths; 0 stored = unset, returned as the CSS default of 8. */
int32_t css_computed_tab_size(const css_computed_style *style);

/* fixes356 (#78): image-rendering. Returns CSS_IMAGE_RENDERING_AUTO
 * (default) / CRISP_EDGES / PIXELATED. The plotter consults this to
 * decide whether to skip box-filter pre-downscale. */
uint8_t css_computed_image_rendering(const css_computed_style *style);

/* fixes1052 (#80): appearance. Returns CSS_APPEARANCE_AUTO when unset. */
uint8_t css_computed_appearance(const css_computed_style *style);

/* fixes357 (#44): text-decoration extended.
 * text-decoration-color: returns CSS_TEXT_DECORATION_COLOR_* (default
 * CURRENT_COLOR); *color receives css_color when return == COLOR. */
uint8_t css_computed_text_decoration_color(
		const css_computed_style *style, css_color *color);

/* text-decoration-style: returns CSS_TEXT_DECORATION_STYLE_* (default
 * SOLID). */
uint8_t css_computed_text_decoration_style(
		const css_computed_style *style);

/* text-decoration-thickness: returns 0 = auto/from-font, N>0 = px. */
int32_t css_computed_text_decoration_thickness(
		const css_computed_style *style);

/* fixes76: -macsurf-animation-opacity: <from> <to> <duration_ms>.
 * from, to: opacity 0..255 (255 = opaque).
 * duration_ms: full cycle in ms (1..65535). Cycle is from -> to -> from.
 * packed: bits 0..7 from, bits 8..15 to, bits 16..31 duration_ms. */
uint8_t css_computed_macsurf_animation_opacity(
		const css_computed_style *style,
		int32_t *packed);

/* fixes77: -macsurf-animation-rotate: <from_deg> <to_deg> <duration_ms>.
 * from, to: rotation in 0..255 (each step = ~1.4 deg). Ping-pong cycle.
 * packed: bits 0..7 from_byte, bits 8..15 to_byte, bits 16..31 duration_ms. */
uint8_t css_computed_macsurf_animation_rotate(
		const css_computed_style *style,
		int32_t *packed);

uint8_t css_computed_background_image(
		const css_computed_style *style,
		lwc_string **url);

uint8_t css_computed_color(
		const css_computed_style *style,
		css_color *color);

/* SVG fill V1. Returns CSS_FILL_*; color is valid for CSS_FILL_COLOR. */
uint8_t css_computed_fill(const css_computed_style *style,
		css_color *color);

uint8_t css_computed_list_style_image(
		const css_computed_style *style,
		lwc_string **url);

uint8_t css_computed_quotes(
		const css_computed_style *style,
		lwc_string ***quotes);

uint8_t css_computed_top(
		const css_computed_style *style,
		css_fixed *length, css_unit *unit);

uint8_t css_computed_right(
		const css_computed_style *style,
		css_fixed *length, css_unit *unit);

uint8_t css_computed_bottom(
		const css_computed_style *style,
		css_fixed *length, css_unit *unit);

uint8_t css_computed_left(
		const css_computed_style *style,
		css_fixed *length, css_unit *unit);

uint8_t css_computed_border_top_color(
		const css_computed_style *style,
		css_color *color);

uint8_t css_computed_border_right_color(
		const css_computed_style *style,
		css_color *color);

uint8_t css_computed_border_bottom_color(
		const css_computed_style *style,
		css_color *color);

uint8_t css_computed_border_left_color(
		const css_computed_style *style,
		css_color *color);

uint8_t css_computed_box_sizing(
		const css_computed_style *style);

uint8_t css_computed_object_fit(
		const css_computed_style *style);

uint8_t css_computed_macsurf_object_position(
		const css_computed_style *style);

/* fixes201: numeric (percent / px) object-position. Returns the
 * packed int32 encoded in autogenerated_computed.h next to
 * background_size. Bit 31 == 0 means "unset" - caller should fall
 * back to the keyword accessor above. */
int32_t css_computed_macsurf_object_position_xy(
		const css_computed_style *style);

uint8_t css_computed_text_overflow(
		const css_computed_style *style);

uint8_t css_computed_word_break(
		const css_computed_style *style);

uint8_t css_computed_text_align_last(
		const css_computed_style *style);

uint8_t css_computed_hyphens(
		const css_computed_style *style);

uint8_t css_computed_text_justify(
		const css_computed_style *style);

uint8_t css_computed_hanging_punctuation(
		const css_computed_style *style);

uint8_t css_computed_background_clip(
		const css_computed_style *style);   /* #255 */

uint8_t css_computed_background_origin(
		const css_computed_style *style);

uint8_t css_computed_background_blend_mode(
		const css_computed_style *style);

uint8_t css_computed_justify_items(
		const css_computed_style *style);   /* #279 */

uint8_t css_computed_justify_self(
		const css_computed_style *style);   /* #279 follow-up */

uint8_t css_computed_overflow_wrap(
		const css_computed_style *style);

uint8_t css_computed_height(
		const css_computed_style *style,
		css_fixed *length, css_unit *unit);

uint8_t css_computed_line_height(
		const css_computed_style *style,
		css_fixed *length, css_unit *unit);

uint8_t css_computed_background_color(
		const css_computed_style *style,
		css_color *color);

uint8_t css_computed_z_index(
		const css_computed_style *style,
		int32_t *z_index);

uint8_t css_computed_margin_top(
		const css_computed_style *style,
		css_fixed *length, css_unit *unit);

uint8_t css_computed_margin_right(
		const css_computed_style *style,
		css_fixed *length, css_unit *unit);

uint8_t css_computed_margin_bottom(
		const css_computed_style *style,
		css_fixed *length, css_unit *unit);

uint8_t css_computed_margin_left(
		const css_computed_style *style,
		css_fixed *length, css_unit *unit);

uint8_t css_computed_background_attachment(
		const css_computed_style *style);

uint8_t css_computed_border_collapse(
		const css_computed_style *style);

uint8_t css_computed_caption_side(
		const css_computed_style *style);

uint8_t css_computed_direction(
		const css_computed_style *style);

uint8_t css_computed_max_height(
		const css_computed_style *style,
		css_fixed *length, css_unit *unit);

uint8_t css_computed_max_width(
		const css_computed_style *style,
		css_fixed *length, css_unit *unit);

/**
 * Get the width property value in device pixels.
 *
 * \note  If available_px is set to a negative number (invalid) then,
 *        if the computation would have required a valid available
 *        width, it will return CSS_WIDTH_AUTO.
 *
 * This will resolve `calc()` expressions to used values.
 *
 * \param[in]  style         A computed style.
 * \param[in]  unit_ctx      Unit conversion context.
 * \param[in]  available_px  The available width in pixels.
 * \param[out] px_out        Returns width in pixels if and only if the
 *                           call returns CSS_WIDTH_SET.
 * \return CSS_WIDTH_SET or CSS_WIDTH_AUTO.
 */
uint8_t css_computed_width_px(
		const css_computed_style *style,
		const css_unit_ctx *unit_ctx,
		int available_px,
		int *px_out);

uint8_t css_computed_width(const css_computed_style *style,
		css_fixed *length, css_unit *unit);

uint8_t css_computed_empty_cells(
		const css_computed_style *style);

uint8_t css_computed_float(
		const css_computed_style *style);

uint8_t css_computed_writing_mode(
		const css_computed_style *style);

uint8_t css_computed_font_style(
		const css_computed_style *style);

uint8_t css_computed_min_height(
		const css_computed_style *style,
		css_fixed *length, css_unit *unit);

uint8_t css_computed_min_width(
		const css_computed_style *style,
		css_fixed *length, css_unit *unit);

uint8_t css_computed_background_repeat(
		const css_computed_style *style);

uint8_t css_computed_clear(
		const css_computed_style *style);

uint8_t css_computed_padding_top(
		const css_computed_style *style,
		css_fixed *length, css_unit *unit);

uint8_t css_computed_padding_right(
		const css_computed_style *style,
		css_fixed *length, css_unit *unit);

uint8_t css_computed_padding_bottom(
		const css_computed_style *style,
		css_fixed *length, css_unit *unit);

uint8_t css_computed_padding_left(
		const css_computed_style *style,
		css_fixed *length, css_unit *unit);

uint8_t css_computed_overflow_x(
		const css_computed_style *style);

uint8_t css_computed_overflow_y(
		const css_computed_style *style);

uint8_t css_computed_position(
		const css_computed_style *style);

uint8_t css_computed_opacity(
		const css_computed_style *style,
		css_fixed *opacity);

uint8_t css_computed_fill_opacity(
		const css_computed_style *style,
		css_fixed *fill_opacity);

uint8_t css_computed_stroke_opacity(
		const css_computed_style *style,
		css_fixed *stroke_opacity);

uint8_t css_computed_text_transform(
		const css_computed_style *style);

uint8_t css_computed_text_indent(
		const css_computed_style *style,
		css_fixed *length, css_unit *unit);

uint8_t css_computed_white_space(
		const css_computed_style *style);

uint8_t css_computed_background_position(
		const css_computed_style *style,
		css_fixed *hlength, css_unit *hunit,
		css_fixed *vlength, css_unit *vunit);

uint8_t css_computed_break_after(
		const css_computed_style *style);

uint8_t css_computed_break_before(
		const css_computed_style *style);

uint8_t css_computed_break_inside(
		const css_computed_style *style);

uint8_t css_computed_column_count(
		const css_computed_style *style,
		int32_t *column_count);

uint8_t css_computed_column_fill(
		const css_computed_style *style);

uint8_t css_computed_column_gap(
		const css_computed_style *style,
		css_fixed *length, css_unit *unit);

uint8_t css_computed_row_gap(
		const css_computed_style *style,
		css_fixed *length, css_unit *unit);

uint8_t css_computed_column_rule_color(
		const css_computed_style *style,
		css_color *color);

uint8_t css_computed_column_rule_style(
		const css_computed_style *style);

uint8_t css_computed_column_rule_width(
		const css_computed_style *style,
		css_fixed *length, css_unit *unit);

uint8_t css_computed_column_span(
		const css_computed_style *style);

uint8_t css_computed_column_width(
		const css_computed_style *style,
		css_fixed *length, css_unit *unit);

uint8_t css_computed_display(
		const css_computed_style *style, bool root);

uint8_t css_computed_display_static(
		const css_computed_style *style);

uint8_t css_computed_font_variant(
		const css_computed_style *style);

uint8_t css_computed_text_decoration(
		const css_computed_style *style);

uint8_t css_computed_font_family(
		const css_computed_style *style,
		lwc_string ***names);

uint8_t css_computed_border_top_style(
		const css_computed_style *style);

uint8_t css_computed_border_right_style(
		const css_computed_style *style);

uint8_t css_computed_border_bottom_style(
		const css_computed_style *style);

uint8_t css_computed_border_left_style(
		const css_computed_style *style);

uint8_t css_computed_font_weight(
		const css_computed_style *style);

uint8_t css_computed_list_style_type(
		const css_computed_style *style);

uint8_t css_computed_outline_style(
		const css_computed_style *style);

uint8_t css_computed_table_layout(
		const css_computed_style *style);

uint8_t css_computed_unicode_bidi(
		const css_computed_style *style);

uint8_t css_computed_visibility(
		const css_computed_style *style);

uint8_t css_computed_list_style_position(
		const css_computed_style *style);

uint8_t css_computed_text_align(
		const css_computed_style *style);

uint8_t css_computed_page_break_after(
		const css_computed_style *style);

uint8_t css_computed_page_break_before(
		const css_computed_style *style);

uint8_t css_computed_page_break_inside(
		const css_computed_style *style);

uint8_t css_computed_orphans(
		const css_computed_style *style,
		int32_t *orphans);

uint8_t css_computed_widows(
		const css_computed_style *style,
		int32_t *widows);

uint8_t css_computed_align_content(
		const css_computed_style *style);

uint8_t css_computed_align_items(
		const css_computed_style *style);

uint8_t css_computed_align_self(
		const css_computed_style *style);

uint8_t css_computed_flex_basis(
		const css_computed_style *style,
		css_fixed *length,
		css_unit *unit);

uint8_t css_computed_flex_direction(
		const css_computed_style *style);

uint8_t css_computed_flex_grow(
		const css_computed_style *style,
		css_fixed *number);

uint8_t css_computed_flex_shrink(
		const css_computed_style *style,
		css_fixed *number);

uint8_t css_computed_flex_wrap(
		const css_computed_style *style);

uint8_t css_computed_justify_content(
		const css_computed_style *style);

uint8_t css_computed_order(
		const css_computed_style *style,
		int32_t *order);

#ifdef __cplusplus
}
#endif

#endif
