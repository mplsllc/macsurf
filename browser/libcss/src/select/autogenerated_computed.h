/*
 * This file is part of LibCSS
 * Licensed under the MIT License,
 *                http://www.opensource.org/licenses/mit-license.php
 * Copyright 2017 The NetSurf Project
 */

#ifndef CSS_COMPUTED_COMPUTED_H_
#define CSS_COMPUTED_COMPUTED_H_

#include "calc.h"

typedef union {
	css_fixed value;
	lwc_string *calc;
} css_fixed_or_calc;

/* fixes1159b: number of calc-expression side slots in
 * css_computed_style (see macsurf_calc_expr below).
 * fixes1161b: +1 for row-gap (its own property since fixes1161). */
#define MACSURF_CALC_SLOT_COUNT 29


struct css_computed_style_i {
/*
 * Property                       Size (bits)     Size (bytes)
 * ---                            ---             ---
 * align_content                    3             
 * align_items                      3             
 * align_self                       3             
 * background_attachment            2             
 * background_color                 2               4
 * background_image                 1             sizeof(ptr)
 * background_position              1 + 10          8
 * background_repeat                3             
 * border_bottom_color              2               4
 * border_bottom_style              4             
 * border_bottom_width              3 + 5           4
 * border_collapse                  2             
 * border_left_color                2               4
 * border_left_style                4             
 * border_left_width                3 + 5           4
 * border_radius                    2 + 5           4
 * border_right_color               2               4
 * border_right_style               4             
 * border_right_width               3 + 5           4
 * border_spacing                   1 + 10          8
 * border_top_color                 2               4
 * border_top_style                 4             
 * border_top_width                 3 + 5           4
 * bottom                           2 + 5           4
 * box_shadow                       2               4
 * box_sizing                       2             
 * break_after                      4             
 * break_before                     4             
 * break_inside                     4             
 * caption_side                     2             
 * clear                            3             
 * clip                             6 + 20         16
 * color                            1               4
 * column_count                     2               4
 * column_fill                      2             
 * column_gap                       2 + 5           4
 * column_rule_color                2               4
 * column_rule_style                4             
 * column_rule_width                3 + 5           4
 * column_span                      2             
 * column_width                     2 + 5           4
 * direction                        2             
 * display                          5             
 * empty_cells                      2             
 * fill_opacity                     1               4
 * flex_basis                       2 + 5           4
 * flex_direction                   3             
 * flex_grow                        1               4
 * flex_shrink                      1               4
 * flex_wrap                        2             
 * float                            2             
 * font_size                        4 + 5           4
 * font_style                       2             
 * font_variant                     2             
 * font_weight                      4             
 * height                           2 + 5           4
 * justify_content                  3             
 * left                             2 + 5           4
 * letter_spacing                   2 + 5           4
 * line_height                      2 + 5           4
 * list_style_image                 1             sizeof(ptr)
 * list_style_position              2             
 * list_style_type                  6             
 * macsurf_gradient                 2               4
 * margin_bottom                    2 + 5           4
 * margin_left                      2 + 5           4
 * margin_right                     2 + 5           4
 * margin_top                       2 + 5           4
 * max_height                       2 + 5           4
 * max_width                        2 + 5           4
 * min_height                       2 + 5           4
 * min_width                        2 + 5           4
 * opacity                          1               4
 * order                            1               4
 * orphans                          1               4
 * outline_color                    2               4
 * outline_style                    4             
 * outline_width                    3 + 5           4
 * overflow_x                       3             
 * overflow_y                       3             
 * padding_bottom                   1 + 5           4
 * padding_left                     1 + 5           4
 * padding_right                    1 + 5           4
 * padding_top                      1 + 5           4
 * page_break_after                 3             
 * page_break_before                3             
 * page_break_inside                2             
 * position                         3             
 * right                            2 + 5           4
 * stroke_opacity                   1               4
 * table_layout                     2             
 * text_align                       4             
 * text_decoration                  5             
 * text_indent                      1 + 5           4
 * text_transform                   3             
 * top                              2 + 5           4
 * unicode_bidi                     2             
 * vertical_align                   4 + 5           4
 * visibility                       2             
 * white_space                      3             
 * widows                           1               4
 * width                            2 + 5           4
 * word_spacing                     2 + 5           4
 * writing_mode                     2             
 * z_index                          2               4
 * 
 * Encode content as an array of content items, terminated with a blank entry.
 * 
 * content                          2             sizeof(ptr)
 * 
 * Encode counter_increment as an array of name, value pairs, terminated with a
 * blank entry.
 * 
 * counter_increment                1             sizeof(ptr)
 * 
 * Encode counter_reset as an array of name, value pairs, terminated with a
 * blank entry.
 * 
 * counter_reset                    1             sizeof(ptr)
 * 
 * Encode cursor uri(s) as an array of string objects, terminated with a blank
 * entry
 * 
 * cursor                           5             sizeof(ptr)
 * 
 * Encode font family as an array of string objects, terminated with a blank
 * entry.
 * 
 * font_family                      3             sizeof(ptr)
 * 
 * Encode quotes as an array of string objects, terminated with a blank entry.
 * 
 * quotes                           1             sizeof(ptr)
 * 
 * ---                            ---             ---
 *                                475 bits        248 + 8sizeof(ptr) bytes
 *                                ===================
 *                                308 + 8sizeof(ptr) bytes
 * 
 * Bit allocations:
 * 
 * 0  bbbbbbbboooooooorrrrrrrrdddddddd
 * border_top_width; border_right_width; border_left_width; border_bottom_width
 * 
 * 1  fffffffffooooooooccccccccwwwwwww
 * font_size; outline_width; column_rule_width; word_spacing
 * 
 * 2  cccccccccccccccccccccccccctttttt
 * clip; text_indent
 * 
 * 3  wwwwwwwtttttttrrrrrrrmmmmmmmeeee
 * width; top; right; min_width; text_align
 * 
 * 4  mmmmmmmaaaaaaaxxxxxxxrrrrrrroooo
 * min_height; max_width; max_height; margin_top; outline_style
 * 
 * 5  mmmmmmmaaaaaaarrrrrrrlllllllffff
 * margin_right; margin_left; margin_bottom; line_height; font_weight
 * 
 * 6  llllllleeeeeeehhhhhhhfffffffcccc
 * letter_spacing; left; height; flex_basis; column_rule_style
 * 
 * 7  cccccccooooooobbbbbbbrrrrrrreeee
 * column_width; column_gap; bottom; border_radius; break_inside
 * 
 * 8  bbbboooowwwtttpppaaagggvvveeejjj
 * border_left_style; border_bottom_style; white_space; text_transform;
 * position; page_break_before; page_break_after; overflow_y; overflow_x;
 * justify_content
 * 
 * 9  ppppppaaaaaaddddddiiiiiillllllzz
 * padding_top; padding_right; padding_left; padding_bottom; list_style_type;
 * z_index
 * 
 * 10 oommllffnnaaeeppddccuurriittssbb
 * outline_color; macsurf_gradient; list_style_position; font_variant;
 * font_style; float; flex_wrap; empty_cells; direction; content; column_span;
 * column_rule_color; column_fill; column_count; caption_side; box_sizing
 * 
 * 11 bbbbbbbbbbbaaaaaaaaaaavvvvvvvvvw
 * border_spacing; background_position; vertical_align; widows
 * 
 * 12 tttttdddddcccccbbbbrrrrooooeeees
 * text_decoration; display; cursor; break_before; break_after;
 * border_top_style; border_right_style; stroke_opacity
 * 
 * 13 ffflllcccbbbaaaiiigggwwvvuuttppq
 * font_family; flex_direction; clear; background_repeat; align_self;
 * align_items; align_content; writing_mode; visibility; unicode_bidi;
 * table_layout; page_break_inside; quotes
 * 
 * 14 bboorrddeettaaccpOilfxyunCkGTTSS
 * box_shadow; border_top_color; border_right_color; border_left_color;
 * border_collapse; border_bottom_color; background_color;
 * background_attachment; orphans; order; opacity; list_style_image;
 * flex_shrink; flex_grow; fill_opacity; counter_reset; counter_increment;
 * color; background_image;
 * (bottom 5 bits, FULL): macsurf_grid (bit 4), macsurf_transform (bits 3-2),
 * macsurf_text_shadow (bits 1-0)
 *
 * 15 ...............EEPPPPWWWWXXOOORRA
 * (fixes200)  pointer_events (bits 16-15);
 * (fixes191g) macsurf_object_position (bits 14-11);
 * (fixes136a) overflow_wrap (bits 10-9);
 * (fixes136a) word_break (bits 8-7);
 * (fixes135a) text_overflow (bits 6-5);
 * (fixes116)  object_fit (bits 4-2);
 * (fixes77)   macsurf_animation_rotate (bit 1);
 * (fixes76)   macsurf_animation_opacity (bit 0)
 */
	uint32_t bits[16];
	
	css_color background_color;
	lwc_string *background_image;
	css_fixed background_position_a;
	css_fixed background_position_b;
	css_color border_bottom_color;
	css_fixed border_bottom_width;
	css_color border_left_color;
	css_fixed border_left_width;
	css_fixed border_radius;
	css_color border_right_color;
	css_fixed border_right_width;
	css_fixed border_spacing_a;
	css_fixed border_spacing_b;
	css_color border_top_color;
	css_fixed border_top_width;
	css_fixed bottom;
	int32_t box_shadow;
	css_fixed clip_a;
	css_fixed clip_b;
	css_fixed clip_c;
	css_fixed clip_d;
	css_color color;
	int32_t column_count;
	css_fixed column_gap;
	css_color column_rule_color;
	css_fixed column_rule_width;
	css_fixed column_width;
	css_fixed fill_opacity;
	css_fixed flex_basis;
	css_fixed flex_grow;
	css_fixed flex_shrink;
	css_fixed font_size;
	css_fixed height;
	css_fixed left;
	css_fixed letter_spacing;
	css_fixed line_height;
	lwc_string *list_style_image;
	int32_t macsurf_gradient;
	int32_t macsurf_text_shadow;
	int32_t macsurf_transform;
	/* fixes73: macsurf_transform_b holds scale_x_q88 (bits 31..16)
	 * and scale_y_q88 (bits 15..0). Identity = 0x01000100 (1.0, 1.0).
	 * Zero = author wrote scale(0). */
	int32_t macsurf_transform_b;
	/* fixes75: -macsurf-grid: cols (bits 31..16) and rows (bits 15..0).
	 * rows == 0 means auto-rows. */
	int32_t macsurf_grid;
	/* fixes151 / fixes158: packed grid placement, int32. Originally
	 * (fixes151) just held col-span as a uint8 in the low byte;
	 * fixes158 extends the upper 24 bits to encode start lines and
	 * row-span without adding new libcss properties.
	 *
	 *   bits  0..7  col_span    - 0=unset (treat as 1), 1..254 literal,
	 *                             255 = "fill the rest of the row"
	 *                             sentinel from `grid-column: 1 / -1`
	 *   bits  8..15 col_start   - 0 = auto, 1..254 = explicit line
	 *   bits 16..23 row_start   - 0 = auto, 1..254 = explicit line
	 *   bits 24..31 row_span    - 0 = unset (treat as 1), 1..254 literal,
	 *                             255 = "fill" sentinel
	 *
	 * Backwards compat: existing CSS that emitted just a 1..255 span
	 * value lands in the low byte with all upper bytes zero (= auto
	 * start, single row), which preserves fixes151 behaviour exactly.
	 *
	 * fixes151b: stored as int32_t (not uint8_t) to avoid the
	 * struct-padding trap. A uint8_t between two int32_t fields
	 * creates 3 padding bytes that aren't deterministic across all
	 * cascade paths -- arena interning's memcmp(&a->i, &b->i, ...)
	 * then flags logically-equal styles as different, the intern
	 * table fills with duplicates, and css_computed_style_destroy
	 * eventually walks a freed pointer (same failure mode as
	 * fixes117's inline track array). Using int32_t is self-aligning
	 * and now exactly the size we need for the packed layout. */
	int32_t macsurf_grid_col_span;
	/* fixes152: aspect-ratio.
	 *   bits 31..16: numerator (1..65535)
	 *   bits 15..0:  denominator (1..65535)
	 *   0 = unset / no aspect-ratio constraint
	 * int32 storage is self-aligning (fixes151b padding lesson). */
	int32_t aspect_ratio;
	/* fixes76: -macsurf-animation-opacity.
	 * bits 31..16: duration_ms (uint16, full from->to->from cycle).
	 * bits 15..8:  to_opacity (uint8 0..255).
	 * bits 7..0:   from_opacity (uint8 0..255). */
	int32_t macsurf_animation_opacity;
	/* fixes77: -macsurf-animation-rotate. Same packed layout as
	 * animation_opacity but the bytes are degrees scaled by 256/360. */
	int32_t macsurf_animation_rotate;
	css_fixed margin_bottom;
	css_fixed margin_left;
	css_fixed margin_right;
	css_fixed margin_top;
	css_fixed max_height;
	css_fixed max_width;
	css_fixed min_height;
	css_fixed min_width;
	css_fixed opacity;
	int32_t order;
	int32_t orphans;
	css_color outline_color;
	css_fixed outline_width;
	css_fixed padding_bottom;
	css_fixed padding_left;
	css_fixed padding_right;
	css_fixed padding_top;
	css_fixed right;
	css_fixed stroke_opacity;
	css_fixed text_indent;
	css_fixed top;
	css_fixed vertical_align;
	int32_t widows;
	css_fixed_or_calc width;
	css_fixed word_spacing;
	int32_t z_index;
	/* fixes191b: background-size.
	 *   bits 31..16: w_code  (int16 cast)
	 *   bits 15..0:  h_code  (int16 cast)
	 * Each code:
	 *    0 = auto (use natural image dimension on this axis)
	 *    +N = explicit pixels (0 < N <= 32767)
	 *   -1 = sentinel COVER     (-1 in int16 == 0xFFFF)
	 *   -2 = sentinel CONTAIN
	 * Whole word 0 = unset / treat as `auto auto` (CSS default).
	 * int32 storage is self-aligning (fixes151b padding lesson). */
	int32_t background_size;
	/* fixes201: object-position numeric value (percent or px).
	 *
	 * Encoded as:
	 *   bit  31:    has-value flag (0 = unset, defer to keyword
	 *               field; 1 = use numeric value below)
	 *   bit  30:    h-unit (0 = percentage, 1 = px)
	 *   bit  29:    v-unit (0 = percentage, 1 = px)
	 *   bit  28:    reserved (must be 0)
	 *   bits 27..14 h-value (signed Q8.6 -- enough range for
	 *               -128..127% or -128..127 px in 14 bits, plus
	 *               6 fractional bits for sub-pixel/sub-percent)
	 *   bits 13..0  v-value (signed Q8.6)
	 *
	 * Why both percent and px in one slot: CSS object-position can
	 * mix axes (`object-position: 25% 10px` is valid). The unit
	 * bits per axis avoid two passes.
	 *
	 * Resolution at consumer time: percent values resolve against
	 * (slot_dim - object_dim) per CSS spec; px values are
	 * absolute offsets within the slot. */
	int32_t macsurf_object_position_xy;
	/* fixes275 (#65): grid-auto-flow stored as int32 for self-alignment
	 * (fixes151b padding lesson). Values match enum css_macsurf_grid_flow_e:
	 *   0 = inherit (treat as row at top-of-cascade)
	 *   1 = row (sparse, default)
	 *   2 = column
	 *   3 = row dense
	 *   4 = column dense
	 * Field appended at struct end so existing field offsets in stale CW8
	 * .o files don't shift (per project_libcss_struct_mid_insert_crash). */
	int32_t macsurf_grid_flow;
	/* fixes353 (#73): accent-color status (CSS_ACCENT_COLOR_*) and
	 * resolved css_color. Status carries INHERIT/AUTO/CURRENTCOLOR/
	 * COLOR; the colour word is meaningful only when status == COLOR.
	 * Both fields are int32-wide and self-aligning, appended at end
	 * of _i per the fixes151b padding/mid-insert discipline. */
	int32_t accent_color_status;
	css_color accent_color;
	/* fixes353 (#73): caret-color status + resolved css_color. */
	int32_t caret_color_status;
	css_color caret_color;
	/* fixes354 (#82): box-decoration-break (CSS_BOX_DECORATION_BREAK_*).
	 * Not inherited; initial = SLICE. int32_t for self-alignment per
	 * the fixes151b discipline. */
	int32_t box_decoration_break;
	/* fixes355 (#58): tab-size (integer form).
	 *   0 = unset (consumer uses 8 = CSS default)
	 *   N>0 = explicit integer number of space widths
	 * Inherits. Length-form (px / em) deferred to V2. */
	int32_t tab_size;
	/* fixes356 (#78): image-rendering (CSS_IMAGE_RENDERING_*).
	 * Inherits. Initial = AUTO. When CRISP_EDGES or PIXELATED, the
	 * plotter should skip the fixes203 box-filter pre-downscale so
	 * pixel-art and crisp-edge content renders with pure
	 * nearest-neighbor (plotter wiring queued separately). */
	int32_t image_rendering;
	/* fixes1052 (#80): appearance (CSS_APPEARANCE_*). Scalar tail, not a
	 * bits[] slot -- keyword-valued so it is byte-deterministic across
	 * cascade paths, and appending here shifts no existing field. */
	int32_t appearance;
	/* fixes357 (#44): text-decoration extended sub-properties.
	 *   text_decoration_color_status: CSS_TEXT_DECORATION_COLOR_*
	 *   text_decoration_color: resolved css_color (valid when
	 *      status == COLOR; for CURRENT_COLOR consumer uses the
	 *      element's computed `color`).
	 *   text_decoration_style: CSS_TEXT_DECORATION_STYLE_*
	 *   text_decoration_thickness: 0 = auto/from-font, N>0 = px.
	 * All four self-aligned int32_t per fixes151b discipline. */
	int32_t text_decoration_color_status;
	css_color text_decoration_color;
	int32_t text_decoration_style;
	int32_t text_decoration_thickness;
	/* fixes364 - horizontal stripe background. Two RGB555 colors packed
	 * into one int32: bits 0..14 = c1, bits 15..29 = c2, bit 31 = set
	 * flag. 0 = unset (no stripe). Mapped from
	 * `repeating-linear-gradient(to bottom, ...)` by cssh_css.c so
	 * mactrove's Platinum window title bars get real alternating-row
	 * stripes instead of the fixes281/363 solid #cccccc fallback.
	 * int32_t = self-aligning, memcmp-safe per fixes151b discipline.
	 * Appended at struct end so stale CW8 .o files do not shift the
	 * preceding field offsets, per
	 * project_libcss_struct_mid_insert_crash. */
	int32_t macsurf_hstripe_bg;
	/* -macsurf-dotgrid (fixes365c): two-layer 2x2 background-pattern
	 * stripe colours packed into int32_t (bit31 set flag,
	 * bits15..29 c2 RGB555, bits0..14 c1 RGB555). Appended at struct
	 * end per project_libcss_struct_mid_insert_crash. */
	int32_t macsurf_dotgrid;
	/* row-gap (real row_gap support): the bits[16] array is FULL
	 * (justify-items took the last slot), so row-gap lives in a scalar
	 * tail instead -- self-aligning int32_t + css_fixed per the
	 * fixes151b memcmp/padding discipline, appended at struct end so
	 * no existing field offset shifts.
	 *
	 * row_gap_status packs the same 7 bits column-gap keeps in
	 * bits[7]: bits 1..0 = type (CSS_COLUMN_GAP_INHERIT/SET/NORMAL),
	 * bits 6..2 = unit. row_gap holds the length, meaningful only
	 * when type == CSS_COLUMN_GAP_SET. set_row_gap always writes both
	 * words together, so every cascade/compose/clone/initial path
	 * leaves byte-deterministic memory for the arena interner's
	 * memcmp. */
	int32_t row_gap_status;
	css_fixed row_gap;
};

struct css_computed_style {
	struct css_computed_style_i i;

	css_computed_content_item *content;
	css_computed_counter *counter_increment;
	css_computed_counter *counter_reset;
	lwc_string **cursor;
	lwc_string **font_family;
	lwc_string **quotes;
	/* fixes118: heap-allocated 8-int array of grid track descriptors,
	 * or NULL when no explicit tracks are set. Each int32 packs
	 * (unit << 28) | value. Comparison in arena.c via
	 * arena__compare_grid_tracks. Lifetime owned by this style's
	 * destroy path. */
	int32_t *macsurf_grid_tracks;

	/* fixes150: heap-allocated 8-int array of grid ROW track
	 * descriptors, or NULL when no explicit row tracks are set.
	 * Same encoding as macsurf_grid_tracks. Comparison in arena.c
	 * via arena__compare_grid_row_tracks. */
	int32_t *macsurf_grid_row_tracks;

	struct css_computed_style *next;
	uint32_t count;
	uint32_t bin;
	css_calculator *calc;

	/* fixes344b: heap-allocated 2-element css_color array carrying
	 * full ARGB for the gradient stops [c1, c2]. The packed int32
	 * in `_i.macsurf_gradient` stays RGB565+R4G6B4 for the existing
	 * painter's fast path; this side-channel preserves alpha so the
	 * plotter can do real per-pixel alpha blending when the rule
	 * uses rgba(...) or `transparent` stops. NULL when the gradient
	 * is unset or both stops are fully opaque. Compared via raw
	 * memcmp of 8 bytes in arena__compare_macsurf_gradient_full;
	 * lifetime owned by this style's destroy path.
	 *
	 * Field appended at struct end so existing field offsets in
	 * stale CW8 .o files don't shift (per the
	 * project_libcss_struct_mid_insert_crash memory note). */
	css_color *macsurf_gradient_full;

	/* fixes345: heap-allocated 4-int array carrying the radial-
	 * gradient size + position prefix the parser extracted from
	 * `radial-gradient(<W>px <H>px at <X>% <Y>%, ...)`. Format:
	 *   [0] size_x in px (-1 = unset / use bounding-rect width)
	 *   [1] size_y in px (-1 = unset / use bounding-rect height)
	 *   [2] pos_x percent × 100 (e.g. 78% = 7800; -1 = center)
	 *   [3] pos_y percent × 100 (e.g. -4% = -400; -1 = center)
	 *
	 * NULL when the radial-gradient had no size or position prefix
	 * (then painter falls back to centered, fill-bounding-rect).
	 * Compared in arena.c via arena__compare_macsurf_gradient_radial;
	 * lifetime owned by this style's destroy path. Appended at end
	 * per the same struct-mid-insert gotcha as macsurf_gradient_full. */
	int32_t *macsurf_gradient_radial;

	/* fixes361b - second box-shadow packed value. Same format as
	 * the inner `box_shadow` slot: h<<24 | v<<16 | inset<<15 | rgb555.
	 * 0 = unset (no second shadow). Appended at outer-struct end so
	 * existing field offsets in stale CW8 .o files don't shift, same
	 * discipline as macsurf_gradient_full / macsurf_gradient_radial. */
	int32_t box_shadow_2;
	/* fixes362 - third box-shadow packed value. Common Platinum
	 * pattern is two inset bevels + one outer drop shadow; this
	 * holds the drop. Same packing format as the first two slots. */
	int32_t box_shadow_3;

	/* fixes365b - extended linear-gradient side-channel: diagonal angle
	 * (45/135/225/315) and up to 3 colour stops with explicit positions.
	 *
	 * The existing 2-stop cardinal fast path (vertical / horizontal,
	 * packed RGB565+R4G6B4 in `i.macsurf_gradient`) is left untouched;
	 * this side-channel is only allocated when the rule needs more -
	 * either a non-cardinal angle or a third stop. NULL = use the legacy
	 * path.
	 *
	 * Format (heap-allocated int32_t[7], all words self-aligning for
	 * arena memcmp safety):
	 *   [0] angle in degrees (0..359; full precision, low 16 bits used)
	 *   [1] position 0 as percent×100 (0..10000)
	 *   [2] position 1 as percent×100
	 *   [3] position 2 as percent×100 (0 when only 2 stops, in which
	 *       case [6] is also 0 and consumer reads only 2)
	 *   [4] colour 0 as css_color (ARGB)
	 *   [5] colour 1 as css_color
	 *   [6] colour 2 as css_color (0 when only 2 stops)
	 *
	 * Compared in arena.c via arena__compare_macsurf_gradient_stops;
	 * lifetime owned by this style's destroy path. Appended at struct
	 * end per project_libcss_struct_mid_insert_crash. */
	int32_t *macsurf_gradient_stops;

	/* fixes1159b: calc-expression side slots, one per calc-capable
	 * length property (fixed property->slot mapping in helpers.c
	 * css__calc_slot_for_prop). The cascade stores the SLOT NUMBER in
	 * the property's css_fixed length field with unit CSS_UNIT_CALC;
	 * unit.c resolves the expression from this table. Never bit-cast
	 * an lwc_string pointer into css_fixed: css_fixed is 32 bits, so
	 * the pointer truncates on 64-bit targets (the original fixes1159
	 * design - ASan SEGV in the Linux harness) and misresolved on the
	 * Mac. The slot table travels with the style through compose and
	 * clone (computed.c) and is compared in css__arena_style_is_equal
	 * (arena.c). The strings are owned by the sheet's string vector,
	 * so the style holds no reference and destroy frees nothing.
	 * Inline fixed array - zeroed by calloc, no heap management.
	 * Appended at struct end per
	 * project_libcss_struct_mid_insert_crash. */
	lwc_string *macsurf_calc_expr[MACSURF_CALC_SLOT_COUNT];
};

#endif
