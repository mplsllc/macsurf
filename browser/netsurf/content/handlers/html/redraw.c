/*
 * Copyright 2004-2008 James Bursa <bursa@users.sourceforge.net>
 * Copyright 2004-2007 John M Bell <jmb202@ecs.soton.ac.uk>
 * Copyright 2004-2007 Richard Wilson <info@tinct.net>
 * Copyright 2005-2006 Adrian Lees <adrianl@users.sourceforge.net>
 * Copyright 2006 Rob Kendrick <rjek@netsurf-browser.org>
 * Copyright 2008 Michael Drake <tlsa@netsurf-browser.org>
 * Copyright 2009 Paul Blokus <paul_pl@users.sourceforge.net>
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
 *
 * Redrawing CONTENT_HTML implementation.
 */

#include "utils/config.h"
#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <dom/dom.h>

#include "utils/log.h"
#include "utils/messages.h"
#include "utils/utils.h"
#include "utils/nsoption.h"
#include "utils/corestrings.h"
#include "netsurf/content.h"
#include "netsurf/browser_window.h"
#include "netsurf/plotters.h"
#include "netsurf/bitmap.h"
#include "netsurf/layout.h"
#include "content/content.h"
#include "content/content_protected.h"
#include "content/textsearch.h"
#include "css/utils.h"
#include "desktop/selection.h"
#include "desktop/print.h"
#include "desktop/scrollbar.h"
#include "desktop/textarea.h"
#include "desktop/gui_internal.h"

#include "html/box.h"
#include "html/box_inspect.h"
#include "html/box_manipulate.h"
#include "html/font.h"
#include "html/form_internal.h"
#include "html/private.h"
#include "html/layout.h"
#include "html/macsurf_dom_compat.h"
#include "html/layout_safe.h"

/* fixes195 — inline SVG renderer lives in the macos9 frontend so it
 * can use Mac-specific debugging hooks (it dispatches through the
 * portable plotter table; nothing here is Mac-only by design and the
 * file could move to content/handlers/html/ later). */
#ifdef __MACOS9__
#include "frontends/macos9/macos9_svg_inline.h"
/* fixes197 — diagnostic logging hook. Declared extern here because
 * the macsurf_debug.h header is in the macos9 frontend dir and the
 * include path may not reach it from this point in the tree. */
extern void macsurf_debug_log_writef(const char *fmt, ...);

/* fixes366c — diagnostic counters for the fixes365/fixes366b
 * gradient / stripe / dot-grid log lines. Originally function-scoped
 * statics, which capped at 5 per session rather than per navigation.
 * Hoisted to file scope so macsurf_profile_reset() can zero them at
 * the start of each navigation via macos9_redraw_diag_counters_reset(). */
static int hstripe_seen_a = 0;
static int hstripe_seen_b = 0;
static int dotgrid_seen_a = 0;
static int dotgrid_seen_b = 0;
static int grad_seen_a = 0;
static int grad_seen_b = 0;
static int grad_seen_c = 0;
static int grad_seen_d = 0;
static int grad_seen_e = 0;
static int grad_seen_f = 0;

void macos9_redraw_diag_counters_reset(void)
{
	hstripe_seen_a = 0;
	hstripe_seen_b = 0;
	dotgrid_seen_a = 0;
	dotgrid_seen_b = 0;
	grad_seen_a = 0;
	grad_seen_b = 0;
	grad_seen_c = 0;
	grad_seen_d = 0;
	grad_seen_e = 0;
	grad_seen_f = 0;
}
#endif


bool html_redraw_debug = false;

/* fixes76: animation hook implemented in frontends/macos9/macos9_animation.c.
 * Returns ticks (Mac OS 9 60Hz units). Stubbed elsewhere. */
extern uint32_t macos9_animation_now_ticks(void);
extern void macos9_animation_register(void);
extern void macos9_animation_register_rect(int x, int y, int w, int h);

/*
 * fixes76: resolve current opacity for a box that has
 * -macsurf-animation-opacity SET. packed = (duration_ms << 16) |
 * (to << 8) | from, where from / to are 0..255. Returns PLOT_STYLE_SCALE
 * units (Q0.10, 1024 = fully opaque).
 *
 * V1 timing: linear ping-pong from -> to -> from over (2 * duration_ms)
 * with one shared clock (TickCount) for all animated elements on the
 * page. No per-element start offset in this round.
 */
static int32_t macsurf_anim_opacity_resolve_plot_fixed(int32_t packed)
{
	int32_t from_v = packed & 0xff;
	int32_t to_v = (packed >> 8) & 0xff;
	int32_t duration_ms = (packed >> 16) & 0xffff;
	uint32_t now_ticks;
	uint32_t elapsed_ms;
	uint32_t period_ms;
	uint32_t t;
	int32_t cur_byte;
	int32_t result;

	if (duration_ms < 1) duration_ms = 1;
	macos9_animation_register();
	now_ticks = macos9_animation_now_ticks();
	elapsed_ms = (now_ticks * 1000u) / 60u;
	period_ms = (uint32_t)duration_ms * 2u;
	t = elapsed_ms % period_ms;

	if ((int32_t)t < duration_ms) {
		/* from -> to. */
		cur_byte = from_v + (((to_v - from_v) * (int32_t)t) /
				duration_ms);
	} else {
		/* to -> from. */
		int32_t back_t = (int32_t)t - duration_ms;
		cur_byte = to_v + (((from_v - to_v) * back_t) / duration_ms);
	}

	if (cur_byte < 0) cur_byte = 0;
	if (cur_byte > 255) cur_byte = 255;
	/* Map 0..255 to PLOT_STYLE_SCALE (0..1024). */
	result = (cur_byte * PLOT_STYLE_SCALE) / 255;
	return result;
}

/* fixes77: -macsurf-animation-rotate resolver. Same packed layout as
 * opacity (duration_ms<<16 | to<<8 | from) but bytes are degrees scaled
 * by 256/360. Returns current rotation in degrees (0..359). Same linear
 * ping-pong timing. */
static int macsurf_anim_rotate_resolve_degrees(int32_t packed)
{
	int32_t from_b = packed & 0xff;
	int32_t to_b = (packed >> 8) & 0xff;
	int32_t duration_ms = (packed >> 16) & 0xffff;
	uint32_t now_ticks;
	uint32_t elapsed_ms;
	uint32_t period_ms;
	uint32_t t;
	int32_t cur_byte;
	int32_t cur_deg;

	if (duration_ms < 1) duration_ms = 1;
	macos9_animation_register();
	now_ticks = macos9_animation_now_ticks();
	elapsed_ms = (now_ticks * 1000u) / 60u;
	period_ms = (uint32_t)duration_ms * 2u;
	t = elapsed_ms % period_ms;

	if ((int32_t)t < duration_ms) {
		cur_byte = from_b + (((to_b - from_b) * (int32_t)t) /
				duration_ms);
	} else {
		int32_t back_t = (int32_t)t - duration_ms;
		cur_byte = to_b + (((from_b - to_b) * back_t) /
				duration_ms);
	}
	if (cur_byte < 0) cur_byte = 0;
	if (cur_byte > 255) cur_byte = 255;
	/* Map byte back to degrees: 256 steps -> 360 deg. */
	cur_deg = (cur_byte * 360) / 256;
	return cur_deg;
}

/* Diagnostic counters */
long macos9_html_redraw_text_box_calls = 0;
long macos9_text_redraw_plot_calls = 0;
long macos9_hrb_visits = 0;
long macos9_hrb_block = 0;
long macos9_hrb_inlinec = 0;
long macos9_hrb_inline = 0;
long macos9_hrb_text = 0;
long macos9_hrb_other = 0;
long macos9_hrb_clip_skips = 0;
char macos9_skipbox_info[160] = {0};
#include <stdio.h>

/* fixes47/48/74 -- decode a packed-RGB565+dir macsurf_gradient int32_t
 * (the format s_macsurf_gradient.c writes via macsurf_gradient_pack)
 * into two NetSurf-format colour values plus a horizontal flag and a
 * radial flag, ready for the rect plotter.
 *
 * Layout (matches s_macsurf_gradient.c):
 *   bits 31..16: RGB565 of c1
 *   bit 15:      horizontal flag (0 = vertical, 1 = horizontal)
 *   bit 14:      radial flag (1 = radial; overrides horizontal)  fixes74
 *   bits 13..10: R4 of c2 (was R5, fixes74 dropped a bit for radial)
 *   bits 9..4:   G6 of c2
 *   bits 3..0:   B4 of c2 (4-bit blue, fixes48 cost of stealing the
 *                 direction bit; smeared to 8 bits on decode)
 *
 * Returned colours are NetSurf BGR (R in low byte). */
/* fixes74b diagnostic counters — defined in plotters.c. */
extern long macos9_grad_set_count;
extern long macos9_grad_radial_unpack_count;
extern long macos9_grad_linear_unpack_count;

static void macsurf_gradient_unpack(int32_t packed_signed,
		colour *c1_out, colour *c2_out, bool *horizontal_out,
		bool *radial_out)
{
	uint32_t packed = (uint32_t)packed_signed;
	uint16_t p1 = (uint16_t)((packed >> 16) & 0xffff);
	uint16_t p2 = (uint16_t)(packed & 0x3fff);
	uint8_t r1 = (uint8_t)(((p1 >> 11) & 0x1f) << 3);
	uint8_t g1 = (uint8_t)(((p1 >>  5) & 0x3f) << 2);
	uint8_t b1 = (uint8_t)(((p1      ) & 0x1f) << 3);
	uint8_t r2 = (uint8_t)(((p2 >> 10) & 0x0f) << 4);
	uint8_t g2 = (uint8_t)(((p2 >>  4) & 0x3f) << 2);
	uint8_t b2 = (uint8_t)(((p2      ) & 0x0f) << 4);
	macos9_grad_set_count++;
	if (packed & 0x4000U) macos9_grad_radial_unpack_count++;
	else macos9_grad_linear_unpack_count++;
	if (horizontal_out != NULL) {
		*horizontal_out = (packed & 0x8000U) ? 1 : 0;
	}
	if (radial_out != NULL) {
		*radial_out = (packed & 0x4000U) ? 1 : 0;
	}
	/* RGB565 sets the bottom bits to zero on decode; smear the high
	 * bits down so a packed 0xFFFF round-trips to 0xFFFFFF (white)
	 * instead of 0xF8FCF8 (off-white). Same trick for the 4-bit
	 * red/blue: smear top nybble into bottom nybble. */
	r1 = (uint8_t)(r1 | (r1 >> 5));
	g1 = (uint8_t)(g1 | (g1 >> 6));
	b1 = (uint8_t)(b1 | (b1 >> 5));
	r2 = (uint8_t)(r2 | (r2 >> 4));
	g2 = (uint8_t)(g2 | (g2 >> 6));
	b2 = (uint8_t)(b2 | (b2 >> 4));
	/* NetSurf colour: R in low byte, then G, then B, alpha in high. */
	*c1_out = (colour)(((uint32_t)b1 << 16) | ((uint32_t)g1 << 8) |
			(uint32_t)r1);
	*c2_out = (colour)(((uint32_t)b2 << 16) | ((uint32_t)g2 << 8) |
			(uint32_t)r2);
}

/* fixes201 — gradient paint with background-size tiling.
 *
 * The existing path emits one ctx->plot->rectangle call which fills
 * the entire box `r` with the gradient. When background-size is set
 * to a specific tile dimension (not auto / cover / contain), the
 * gradient should repeat at that size — that's how stipple textures
 * built from sharp-stop linear-gradients (mactrove body) get their
 * pattern.
 *
 * V1 strategy: loop plot_rectangle once per tile, capped at
 * MACOS9_GRAD_TILE_MAX (4096) tiles to keep pathological inputs
 * bounded. Above the cap we fall back to a single fill — same as
 * the unset-bg-size behaviour, so body stipple at 2x2 (~hundreds of
 * thousands of tiles) degrades but doesn't slow the page.
 *
 * V2 (deferred) would rasterise the gradient into a small bitmap
 * once and use plot_bitmap with REPEAT flags — one CopyBits per
 * paint, scalable to any tile size.
 */
#define MACOS9_GRAD_TILE_MAX 4096

static nserror html_redraw_paint_gradient_tiled(
		const struct redraw_context *ctx,
		const plot_style_t *pstyle,
		const struct rect *r,
		const css_computed_style *style)
{
	int32_t bgsz;
	int16_t wc;
	int16_t hc;
	int tile_w;
	int tile_h;
	int box_w;
	int box_h;
	int tile_count;
	int tx;
	int ty;
	struct rect tile_r;
	nserror res;

	if (style == NULL) {
		return ctx->plot->rectangle(ctx, pstyle, r);
	}

	bgsz = css_computed_background_size(style);
	if (bgsz == 0) {
		/* unset: paint as single rect (CSS default behaviour). */
		return ctx->plot->rectangle(ctx, pstyle, r);
	}

	wc = (int16_t)((bgsz >> 16) & 0xFFFF);
	hc = (int16_t)(bgsz & 0xFFFF);

	/* cover / contain don't tile (they scale to fit). */
	if (wc < 0 || hc < 0) {
		return ctx->plot->rectangle(ctx, pstyle, r);
	}

	/* auto on either axis → fill once. */
	if (wc == 0 || hc == 0) {
		return ctx->plot->rectangle(ctx, pstyle, r);
	}

	tile_w = (int)wc;
	tile_h = (int)hc;
	if (tile_w < 1 || tile_h < 1) {
		return ctx->plot->rectangle(ctx, pstyle, r);
	}

	box_w = r->x1 - r->x0;
	box_h = r->y1 - r->y0;
	if (box_w <= 0 || box_h <= 0) {
		return NSERROR_OK;
	}

	/* Cap: if we'd emit more than MACOS9_GRAD_TILE_MAX tiles, fall
	 * back to single-rect paint. Body-stipple-style tiny tiles fall
	 * here; they'd render correctly but at unacceptable cost. The
	 * future V2 bitmap-rasterise path is the right fix for them. */
	tile_count = ((box_w + tile_w - 1) / tile_w) *
			((box_h + tile_h - 1) / tile_h);
	if (tile_count > MACOS9_GRAD_TILE_MAX) {
		return ctx->plot->rectangle(ctx, pstyle, r);
	}

	for (ty = r->y0; ty < r->y1; ty += tile_h) {
		for (tx = r->x0; tx < r->x1; tx += tile_w) {
			tile_r.x0 = tx;
			tile_r.y0 = ty;
			tile_r.x1 = tx + tile_w;
			tile_r.y1 = ty + tile_h;
			if (tile_r.x1 > r->x1) tile_r.x1 = r->x1;
			if (tile_r.y1 > r->y1) tile_r.y1 = r->y1;
			res = ctx->plot->rectangle(ctx, pstyle, &tile_r);
			if (res != NSERROR_OK) {
				return res;
			}
		}
	}
	return NSERROR_OK;
}

/**
 * Determine if a box has a background that needs drawing
 *
 * \param box  Box to consider
 * \return True if box has a background, false otherwise.
 */
/**
 * fixes116 — apply CSS `object-fit` to the replaced-element draw rect.
 *
 * Called after the cell's content_redraw_data has been populated with the
 * raw layout dimensions (cell width/height at obj_data->width/height,
 * top-left at obj_data->x/y). Mutates the rect so the image is sized
 * according to its `object-fit` value:
 *
 *   FILL       — leave the rect as-is (default; stretches to cell).
 *   CONTAIN    — scale the image down to fit the cell, preserving
 *                aspect ratio; align within the cell per object-position.
 *   COVER      — scale the image up to cover the cell, preserving
 *                aspect ratio; align and let it overflow. QuickDraw's
 *                clipRgn — already set to the cell rect by html_redraw_box
 *                before content_redraw fires — clips the overflow.
 *   NONE       — render at intrinsic size, aligned in the cell.
 *   SCALE_DOWN — `none` or `contain`, whichever yields a smaller image.
 *
 * Integer math throughout: int64_t intermediates ARE NOT used (CW8 PPC
 * miscompiles them — see CLAUDE.md Known Gotchas). The 32-bit products
 * here (e.g. nat_w * cell_h) cap at ~16k*16k = 256M, which fits.
 */
/* fixes201 — object-position offset resolver.
 *
 * Reads both the keyword and numeric (percent/px) storage slots; when
 * the numeric slot has bit 31 set (the parser's "numeric SET"
 * marker), resolves per CSS spec:
 *
 *   px value:        offset = px
 *   percent value:   offset = (axis_free) * pct / 100
 *
 * Otherwise falls back to the keyword (START/CENTER/END snapped to
 * 0% / 50% / 100% effectively).
 *
 * The fixes191g keyword storage remains for backward compatibility
 * and as the fallback when no numeric value is in the cascade.
 */
static int html_redraw_object_pos_offset(int axis_free, uint8_t kw_pos,
		int packed_xy, int is_horizontal)
{
	int has_value;
	int is_px;
	int q86;
	int real_int;
	int real_frac_64;
	int offset;

	has_value = ((unsigned int)packed_xy >> 31) & 1;
	if (!has_value) {
		/* Fallback to the 4-bit keyword storage. */
		if (kw_pos == CSS_MACSURF_OBJECT_POSITION_START) return 0;
		if (kw_pos == CSS_MACSURF_OBJECT_POSITION_END) return axis_free;
		return axis_free / 2;
	}
	if (is_horizontal) {
		is_px = ((unsigned int)packed_xy >> 30) & 1;
		q86 = ((unsigned int)packed_xy >> 16) & 0x3FFF;
	} else {
		is_px = ((unsigned int)packed_xy >> 14) & 1;
		q86 = (unsigned int)packed_xy & 0x3FFF;
	}
	/* Sign-extend the 14-bit Q8.6 value. */
	if (q86 & 0x2000) q86 |= ~0x3FFF;
	/* q86 is value * 64 in real units. real_int = q86 >> 6 with
	 * arithmetic shift (preserve sign). */
	real_int = q86 >> 6;
	real_frac_64 = q86 & 0x3F;
	if (is_px) {
		/* px: offset = value (real_int is integer pixels, drop
		 * sub-pixel fraction for QuickDraw int-pixel paint). */
		offset = real_int;
	} else {
		/* percent: offset = axis_free * pct / 100. Use the
		 * fractional 1/64-percent bits for sub-percent precision:
		 *   offset = (axis_free * q86) / (64 * 100)
		 *         = (axis_free * q86) / 6400
		 * axis_free up to ~16000 (cell dim), q86 up to ~8191, so
		 * the int32 product caps at 1.3e8 — fits comfortably. */
		(void)real_int;
		(void)real_frac_64;
		offset = (axis_free * q86) / 6400;
	}
	/* Clamp to the slot bounds. CSS allows negative offsets and
	 * over-100% (image hangs off the slot), but for QuickDraw paint
	 * we keep it within the cell so plot_bitmap's clip catches the
	 * rest correctly. */
	if (offset < 0) offset = 0;
	if (offset > axis_free) offset = axis_free;
	return offset;
}

static void html_redraw_apply_object_fit(struct box *box,
		struct content_redraw_data *obj)
{
	uint8_t fit;
	uint8_t pos;
	uint8_t pos_x;
	uint8_t pos_y;
	int32_t pos_xy;
	int nat_w, nat_h;
	int cell_w, cell_h;
	int cell_x, cell_y;
	int new_w, new_h;
	int free_x, free_y;

	if (box == NULL || box->style == NULL || box->object == NULL)
		return;

	fit = css_computed_object_fit(box->style);
	if (fit == CSS_OBJECT_FIT_INHERIT || fit == CSS_OBJECT_FIT_FILL) {
		/* No work: layout already sized rect = cell = fill. */
		return;
	}

	nat_w = content_get_width(box->object);
	nat_h = content_get_height(box->object);
	if (nat_w <= 0 || nat_h <= 0) {
		return;
	}

	cell_w = obj->width;
	cell_h = obj->height;
	cell_x = obj->x;
	cell_y = obj->y;
	if (cell_w <= 0 || cell_h <= 0) {
		return;
	}

	pos = css_computed_macsurf_object_position(box->style);
	pos_xy = css_computed_macsurf_object_position_xy(box->style);
	pos_x = (uint8_t)((pos >> 2) & 0x3);
	pos_y = (uint8_t)(pos & 0x3);
	if (pos_x < CSS_MACSURF_OBJECT_POSITION_START ||
			pos_x > CSS_MACSURF_OBJECT_POSITION_END) {
		pos_x = CSS_MACSURF_OBJECT_POSITION_CENTER;
	}
	if (pos_y < CSS_MACSURF_OBJECT_POSITION_START ||
			pos_y > CSS_MACSURF_OBJECT_POSITION_END) {
		pos_y = CSS_MACSURF_OBJECT_POSITION_CENTER;
	}

/* fixes201: route both axes through the numeric-aware offset
 * resolver. When numeric storage (bit 31 of pos_xy) is clear the
 * resolver falls back to the keyword path automatically, which
 * matches the fixes191g behaviour byte-for-byte. */
#define MACSURF_ALIGN_POS_H(axis_free) \
	html_redraw_object_pos_offset((axis_free), pos_x, pos_xy, 1)
#define MACSURF_ALIGN_POS_V(axis_free) \
	html_redraw_object_pos_offset((axis_free), pos_y, pos_xy, 0)
#define MACSURF_ALIGN_POS(axis_free, axis_pos) \
	(((axis_pos) == pos_x) ? MACSURF_ALIGN_POS_H(axis_free) : \
				  MACSURF_ALIGN_POS_V(axis_free))

	switch (fit) {
	case CSS_OBJECT_FIT_CONTAIN:
		/* min(cell_w/nat_w, cell_h/nat_h). Compare ratios via
		 * cross-multiplication to stay in int32 range. */
		if (nat_w * cell_h < nat_h * cell_w) {
			/* Height is the limiting axis. */
			new_h = cell_h;
			new_w = (nat_w * cell_h) / nat_h;
		} else {
			new_w = cell_w;
			new_h = (nat_h * cell_w) / nat_w;
		}
		obj->width = new_w;
		obj->height = new_h;
		free_x = cell_w - new_w;
		free_y = cell_h - new_h;
		obj->x = cell_x + MACSURF_ALIGN_POS(free_x, pos_x);
		obj->y = cell_y + MACSURF_ALIGN_POS(free_y, pos_y);
		break;

	case CSS_OBJECT_FIT_COVER:
		/* max(cell_w/nat_w, cell_h/nat_h). */
		if (nat_w * cell_h > nat_h * cell_w) {
			/* Height is the limiting axis when "covering". */
			new_h = cell_h;
			new_w = (nat_w * cell_h) / nat_h;
		} else {
			new_w = cell_w;
			new_h = (nat_h * cell_w) / nat_w;
		}
		obj->width = new_w;
		obj->height = new_h;
		free_x = cell_w - new_w;
		free_y = cell_h - new_h;
		obj->x = cell_x + MACSURF_ALIGN_POS(free_x, pos_x);
		obj->y = cell_y + MACSURF_ALIGN_POS(free_y, pos_y);
		break;

	case CSS_OBJECT_FIT_NONE:
		obj->width = nat_w;
		obj->height = nat_h;
		free_x = cell_w - nat_w;
		free_y = cell_h - nat_h;
		obj->x = cell_x + MACSURF_ALIGN_POS(free_x, pos_x);
		obj->y = cell_y + MACSURF_ALIGN_POS(free_y, pos_y);
		break;

	case CSS_OBJECT_FIT_SCALE_DOWN:
		/* `none` if intrinsic fits inside the cell, else `contain`. */
		if (nat_w <= cell_w && nat_h <= cell_h) {
			obj->width = nat_w;
			obj->height = nat_h;
			free_x = cell_w - nat_w;
			free_y = cell_h - nat_h;
			obj->x = cell_x + MACSURF_ALIGN_POS(free_x, pos_x);
			obj->y = cell_y + MACSURF_ALIGN_POS(free_y, pos_y);
		} else {
			if (nat_w * cell_h < nat_h * cell_w) {
				new_h = cell_h;
				new_w = (nat_w * cell_h) / nat_h;
			} else {
				new_w = cell_w;
				new_h = (nat_h * cell_w) / nat_w;
			}
			obj->width = new_w;
			obj->height = new_h;
			free_x = cell_w - new_w;
			free_y = cell_h - new_h;
			obj->x = cell_x + MACSURF_ALIGN_POS(free_x, pos_x);
			obj->y = cell_y + MACSURF_ALIGN_POS(free_y, pos_y);
		}
		break;

	default:
		break;
	}

#undef MACSURF_ALIGN_POS
}


/* fixes139a: recursive emptiness test for table cells. A cell is empty
 * (per CSS 2.1 §17.6.1) when it has no visible content -- whitespace
 * counts as empty. Replaced objects, form gadgets, iframes, generated
 * content (object on the box) all count as visible. Non-whitespace
 * text in box->text counts as visible. The walker returns false the
 * moment it sees any visible content. */
static bool html_box_table_cell_is_empty(const struct box *box)
{
	const struct box *c;
	if (box == NULL)
		return true;
	if (box->object != NULL || box->gadget != NULL || box->iframe != NULL)
		return false;
	if (box->text != NULL && box->length > 0) {
		size_t i;
		for (i = 0; i < box->length; i++) {
			unsigned char ch = (unsigned char) box->text[i];
			if (ch != ' ' && ch != '\t' && ch != '\n' &&
					ch != '\r' && ch != '\f' && ch != 0xA0)
				return false;
		}
	}
	for (c = box->children; c != NULL; c = c->next) {
		if (!html_box_table_cell_is_empty(c))
			return false;
	}
	return true;
}

static bool html_redraw_box_has_background(struct box *box)
{
	if (box->background != NULL)
		return true;

	if (box->style != NULL) {
		css_color colour;
		int32_t grad_col;

		css_computed_background_color(box->style, &colour);

		if (nscss_color_is_transparent(colour) == false)
			return true;

		/* fixes74d: -macsurf-gradient: radial/linear-gradient(...)
		 * makes the element render a background even without an
		 * explicit background-color. Without this, every TU that
		 * uses only -macsurf-gradient gets bg_box=NULL and
		 * html_redraw_background never runs -- gradient drops on
		 * the floor. Caught when fixes74's radial-gradient test
		 * page had no background-color on the swatches. */
		if (css_computed_macsurf_gradient(box->style, &grad_col)
				== CSS_MACSURF_GRADIENT_SET)
			return true;

		/* fixes347 — same defect class as fixes74d, but for
		 * `background-image: url(...)`. A box whose only background
		 * declaration is an image (no background-color, no gradient)
		 * was returning false here, meaning html_redraw_find_bg_box
		 * gave bg_box=NULL and html_redraw_background never ran.
		 * Mactrove's header tile pseudo (.page__header--has-tile
		 * ::before) hit this exact path. */
		{
			lwc_string *bgimage_uri = NULL;
			if (css_computed_background_image(box->style,
					&bgimage_uri) ==
					CSS_BACKGROUND_IMAGE_IMAGE &&
					bgimage_uri != NULL) {
				return true;
			}
		}
	}

	return false;
}

/**
 * Find the background box for a box
 *
 * \param box  Box to find background box for
 * \return Pointer to background box, or NULL if there is none
 */
static struct box *html_redraw_find_bg_box(struct box *box)
{
	/* Thanks to backwards compatibility, CSS defines the following:
	 *
	 * + If the box is for the root element and it has a background,
	 *   use that (and then process the body box with no special case)
	 * + If the box is for the root element and it has no background,
	 *   then use the background (if any) from the body element as if
	 *   it were specified on the root. Then, when the box for the body
	 *   element is processed, ignore the background.
	 * + For any other box, just use its own styling.
	 */
	if (box->parent == NULL) {
		/* Root box */
		if (html_redraw_box_has_background(box))
			return box;

		/* No background on root box: consider body box, if any */
		if (box->children != NULL) {
			if (html_redraw_box_has_background(box->children))
				return box->children;
		}
	} else if (box->parent != NULL && box->parent->parent == NULL) {
		/* Body box: only render background if root has its own */
		if (html_redraw_box_has_background(box) &&
				html_redraw_box_has_background(box->parent))
			return box;
	} else {
		/* Any other box */
		if (html_redraw_box_has_background(box))
			return box;
	}

	return NULL;
}

/**
 * Redraw a short text string, complete with highlighting
 * (for selection/search)
 *
 * \param utf8_text pointer to UTF-8 text string
 * \param utf8_len  length of string, in bytes
 * \param offset    byte offset within textual representation
 * \param space     width of space that follows string (0 = no space)
 * \param fstyle    text style to use (pass text size unscaled)
 * \param x         x ordinate at which to plot text
 * \param y         y ordinate at which to plot text
 * \param clip      pointer to current clip rectangle
 * \param height    height of text string
 * \param scale     current display scale (1.0 = 100%)
 * \param excluded  exclude this text string from the selection
 * \param c         Content being redrawn.
 * \param sel       Selection context
 * \param search    Search context
 * \param ctx	    current redraw context
 * \return true iff successful and redraw should proceed
 */

static bool
text_redraw(const char *utf8_text,
	    size_t utf8_len,
	    size_t offset,
	    int space,
	    const plot_font_style_t *fstyle,
	    int x,
	    int y,
	    const struct rect *clip,
	    int height,
	    float scale,
	    bool excluded,
	    struct content *c,
	    const struct selection *sel,
	    const struct redraw_context *ctx)
{
	bool highlighted = false;
	plot_font_style_t plot_fstyle = *fstyle;
	nserror res;

	/* Need scaled text size to pass to plotters */
	plot_fstyle.size *= scale;

	/* is this box part of a selection? */
	if (!excluded && ctx->interactive == true) {
		unsigned len = utf8_len + (space ? 1 : 0);
		unsigned start_idx;
		unsigned end_idx;

		/* first try the browser window's current selection */
		if (selection_highlighted(sel,
					  offset,
					  offset + len,
					  &start_idx,
					  &end_idx)) {
			highlighted = true;
		}

		/* what about the current search operation, if any? */
		if (!highlighted &&
		    (c->textsearch.context != NULL) &&
		    content_textsearch_ishighlighted(c->textsearch.context,
						     offset,
						     offset + len,
						     &start_idx,
						     &end_idx)) {
			highlighted = true;
		}

		/* \todo make search terms visible within selected text */
		if (highlighted) {
			struct rect r;
			unsigned endtxt_idx = end_idx;
			bool clip_changed = false;
			bool text_visible = true;
			int startx, endx;
			plot_style_t pstyle_fill_hback = *plot_style_fill_white;
			plot_font_style_t fstyle_hback = plot_fstyle;

			if (end_idx > utf8_len) {
				/* adjust for trailing space, not present in
				 * utf8_text */
				assert(end_idx == utf8_len + 1);
				endtxt_idx = utf8_len;
			}

			res = guit->layout->width(fstyle,
						  utf8_text, start_idx,
						  &startx);
			if (res != NSERROR_OK) {
				startx = 0;
			}

			res = guit->layout->width(fstyle,
						  utf8_text, endtxt_idx,
						  &endx);
			if (res != NSERROR_OK) {
				endx = 0;
			}

			/* is there a trailing space that should be highlighted
			 * as well? */
			if (end_idx > utf8_len) {
					endx += space;
			}

			if (scale != 1.0) {
				startx *= scale;
				endx *= scale;
			}

			/* draw any text preceding highlighted portion */
			if ((start_idx > 0) &&
			    (ctx->plot->text(ctx,
					     &plot_fstyle,
					     x,
					     y + font_plot_style_baseline(
						     &plot_fstyle,
						     (int)(height * scale)),
					     utf8_text,
					     start_idx) != NSERROR_OK))
				return false;

			pstyle_fill_hback.fill_colour = fstyle->foreground;

			/* highlighted portion */
			r.x0 = x + startx;
			r.y0 = y;
			r.x1 = x + endx;
			r.y1 = y + height * scale;
			res = ctx->plot->rectangle(ctx, &pstyle_fill_hback, &r);
			if (res != NSERROR_OK) {
				return false;
			}

			if (start_idx > 0) {
				int px0 = max(x + startx, clip->x0);
				int px1 = min(x + endx, clip->x1);

				if (px0 < px1) {
					r.x0 = px0;
					r.y0 = clip->y0;
					r.x1 = px1;
					r.y1 = clip->y1;
					res = ctx->plot->clip(ctx, &r);
					if (res != NSERROR_OK) {
						return false;
					}

					clip_changed = true;
				} else {
					text_visible = false;
				}
			}

			fstyle_hback.background =
				pstyle_fill_hback.fill_colour;
			fstyle_hback.foreground = colour_to_bw_furthest(
				pstyle_fill_hback.fill_colour);

			if (text_visible &&
			    (ctx->plot->text(ctx,
					     &fstyle_hback,
					     x,
					     y + font_plot_style_baseline(
						     &fstyle_hback,
						     (int)(height * scale)),
					     utf8_text,
					     endtxt_idx) != NSERROR_OK)) {
				return false;
			}

			/* draw any text succeeding highlighted portion */
			if (endtxt_idx < utf8_len) {
				int px0 = max(x + endx, clip->x0);
				if (px0 < clip->x1) {

					r.x0 = px0;
					r.y0 = clip->y0;
					r.x1 = clip->x1;
					r.y1 = clip->y1;
					res = ctx->plot->clip(ctx, &r);
					if (res != NSERROR_OK) {
						return false;
					}

					clip_changed = true;

					res = ctx->plot->text(ctx,
							      &plot_fstyle,
							      x,
							      y + font_plot_style_baseline(
								      &plot_fstyle,
								      (int)(height * scale)),
							      utf8_text,
							      utf8_len);
					if (res != NSERROR_OK) {
						return false;
					}
				}
			}

			if (clip_changed &&
			    (ctx->plot->clip(ctx, clip) != NSERROR_OK)) {
				return false;
			}
		}
	}

	if (!highlighted) {
		macos9_text_redraw_plot_calls++;
		res = ctx->plot->text(ctx,
				      &plot_fstyle,
				      x,
				      y + font_plot_style_baseline(
					      &plot_fstyle,
					      (int)(height * scale)),
				      utf8_text,
				      utf8_len);
		if (res != NSERROR_OK) {
			return false;
		}
	}
	return true;
}


/**
 * Plot a checkbox.
 *
 * \param  x	     left coordinate
 * \param  y	     top coordinate
 * \param  width     dimensions of checkbox
 * \param  height    dimensions of checkbox
 * \param  selected  the checkbox is selected
 * \param  ctx	     current redraw context
 * \return true if successful, false otherwise
 */

static bool html_redraw_checkbox(int x, int y, int width, int height,
		bool selected, const struct redraw_context *ctx)
{
	double z;
	nserror res;
	struct rect rect;

	z = width * 0.15;
	if (z == 0) {
		z = 1;
	}

	rect.x0 = x;
	rect.y0 = y ;
	rect.x1 = x + width;
	rect.y1 = y + height;
	res = ctx->plot->rectangle(ctx, plot_style_fill_wbasec, &rect);
	if (res != NSERROR_OK) {
		return false;
	}

	/* dark line across top */
	rect.y1 = y;
	res = ctx->plot->line(ctx, plot_style_stroke_darkwbasec, &rect);
	if (res != NSERROR_OK) {
		return false;
	}

	/* dark line across left */
	rect.x1 = x;
	rect.y1 = y + height;
	res = ctx->plot->line(ctx, plot_style_stroke_darkwbasec, &rect);
	if (res != NSERROR_OK) {
		return false;
	}

	/* light line across right */
	rect.x0 = x + width;
	rect.x1 = x + width;
	res = ctx->plot->line(ctx, plot_style_stroke_lightwbasec, &rect);
	if (res != NSERROR_OK) {
		return false;
	}

	/* light line across bottom */
	rect.x0 = x;
	rect.y0 = y + height;
	res = ctx->plot->line(ctx, plot_style_stroke_lightwbasec, &rect);
	if (res != NSERROR_OK) {
		return false;
	}

	if (selected) {
		if (width < 12 || height < 12) {
			/* render a solid box instead of a tick */
			rect.x0 = x + z + z;
			rect.y0 = y + z + z;
			rect.x1 = x + width - z;
			rect.y1 = y + height - z;
			res = ctx->plot->rectangle(ctx, plot_style_fill_wblobc, &rect);
			if (res != NSERROR_OK) {
				return false;
			}
		} else {
			/* render a tick, as it'll fit comfortably */
			rect.x0 = x + width - z;
			rect.y0 = y + z;
			rect.x1 = x + (z * 3);
			rect.y1 = y + height - z;
			res = ctx->plot->line(ctx, plot_style_stroke_wblobc, &rect);
			if (res != NSERROR_OK) {
				return false;
			}

			rect.x0 = x + (z * 3);
			rect.y0 = y + height - z;
			rect.x1 = x + z + z;
			rect.y1 = y + (height / 2);
			res = ctx->plot->line(ctx, plot_style_stroke_wblobc, &rect);
			if (res != NSERROR_OK) {
				return false;
			}
		}
	}
	return true;
}


/**
 * Plot a radio icon.
 *
 * \param  x	     left coordinate
 * \param  y	     top coordinate
 * \param  width     dimensions of radio icon
 * \param  height    dimensions of radio icon
 * \param  selected  the radio icon is selected
 * \param  ctx	     current redraw context
 * \return true if successful, false otherwise
 */
static bool html_redraw_radio(int x, int y, int width, int height,
		bool selected, const struct redraw_context *ctx)
{
	nserror res;

	/* plot background of radio button */
	res = ctx->plot->disc(ctx,
			      plot_style_fill_wbasec,
			      x + width * 0.5,
			      y + height * 0.5,
			      width * 0.5 - 1);
	if (res != NSERROR_OK) {
		return false;
	}

	/* plot dark arc */
	res = ctx->plot->arc(ctx,
			     plot_style_fill_darkwbasec,
			     x + width * 0.5,
			     y + height * 0.5,
			     width * 0.5 - 1,
			     45,
			     225);
	if (res != NSERROR_OK) {
		return false;
	}

	/* plot light arc */
	res = ctx->plot->arc(ctx,
			     plot_style_fill_lightwbasec,
			     x + width * 0.5,
			     y + height * 0.5,
			     width * 0.5 - 1,
			     225,
			     45);
	if (res != NSERROR_OK) {
		return false;
	}

	if (selected) {
		/* plot selection blob */
		res = ctx->plot->disc(ctx,
				      plot_style_fill_wblobc,
				      x + width * 0.5,
				      y + height * 0.5,
				      width * 0.3 - 1);
		if (res != NSERROR_OK) {
			return false;
		}
	}

	return true;
}


/**
 * Plot a file upload input.
 *
 * \param  x	     left coordinate
 * \param  y	     top coordinate
 * \param  width     dimensions of input
 * \param  height    dimensions of input
 * \param  box	     box of input
 * \param  scale     scale for redraw
 * \param  background_colour  current background colour
 * \param  unit_len_ctx   Length conversion context
 * \param  ctx	     current redraw context
 * \return true if successful, false otherwise
 */

static bool html_redraw_file(int x, int y, int width, int height,
		struct box *box, float scale, colour background_colour,
		const css_unit_ctx *unit_len_ctx,
		const struct redraw_context *ctx)
{
	int text_width;
	const char *text;
	size_t length;
	plot_font_style_t fstyle;
	nserror res;

	font_plot_style_from_css(unit_len_ctx, box->style, &fstyle);
	fstyle.background = background_colour;

	if (box->gadget->value) {
		text = box->gadget->value;
	} else {
		text = messages_get("Form_Drop");
	}
	length = strlen(text);

	res = guit->layout->width(&fstyle, text, length, &text_width);
	if (res != NSERROR_OK) {
		return false;
	}
	text_width *= scale;
	if (width < text_width + 8) {
		x = x + width - text_width - 4;
	} else {
		x = x + 4;
	}

	res = ctx->plot->text(ctx, &fstyle, x,
			y + font_plot_style_baseline(&fstyle, height),
			text, length);
	if (res != NSERROR_OK) {
		return false;
	}
	return true;
}


/**
 * Plot background images.
 *
 * The reason for the presence of \a background is the backwards compatibility
 * mess that is backgrounds on &lt;body&gt;. The background will be drawn relative
 * to \a box, using the background information contained within \a background.
 *
 * \param  x	  coordinate of box
 * \param  y	  coordinate of box
 * \param  box	  box to draw background image of
 * \param  scale  scale for redraw
 * \param  clip   current clip rectangle
 * \param  background_colour  current background colour
 * \param  background  box containing background details (usually \a box)
 * \param  unit_len_ctx  Length conversion context
 * \param  ctx      current redraw context
 * \return true if successful, false otherwise
 */

/* fixes137: cross-module helper exposing scroll origin + viewport
 * dimensions on the macos9 frontend. NetSurf core has no notion of
 * scroll position during paint, so html_redraw_background reads them
 * via this extern when background-attachment: fixed is active. On
 * non-macos9 builds the symbol is missing; the #ifdef gate keeps the
 * call site out. */
#ifdef __MACOS9__
extern int macos9_get_bg_fixed_origin(int *out_x, int *out_y,
		int *out_w, int *out_h);
/* fixes361h — second-shadow plumbing via a frontend-side one-shot
 * static instead of a plot_style_t struct extension. fixes361b/g put
 * box_shadow_2_* on plot_style_t, but several NetSurf-core sites
 * declare plot_style_t locals without zero-initialising every field
 * — leaving the new fields as stack garbage and triggering phantom
 * second-shadow paints. With the value living in a macos9-side
 * static, the plotter reads-then-clears: only the immediately
 * preceding macos9_set_box_shadow_2 call can produce a paint.
 *
 * fixes362 — same pattern for the third shadow (the outer drop in the
 * Platinum two-inset + drop convention). */
extern void macos9_set_box_shadow_2(int32_t packed);
extern void macos9_set_box_shadow_3(int32_t packed);
/* fixes364 — horizontal stripe one-shot. Set right before the bg
 * rectangle paint so the plotter overrides the default flat fill with
 * alternating-row stripes. Same read-and-clear lifecycle as the
 * box_shadow_2/3 setters. */
extern void macos9_set_hstripe_bg(int32_t packed);
/* fixes365c — two-layer 2x2 dot-grid background one-shot. Set right
 * before the bg rectangle paint so the plotter overrides the flat fill
 * with a 2x2 grid of alternating 1px vertical (c1) + horizontal (c2)
 * stripes. Same read-and-clear lifecycle as the box_shadow_2/3 and
 * hstripe_bg setters. */
extern void macos9_set_dotgrid(int32_t packed);
/* fixes365b — diagonal / 3-stop linear-gradient one-shots. Set right
 * before the bg rectangle paint so the plotter routes through the
 * per-pixel interpolation branch with the author's full angle and
 * stop palette. Same read-and-clear lifecycle as the box_shadow_2/3
 * and hstripe_bg setters. The pointer must remain valid until the
 * immediately following plot_rectangle call, which is the case here
 * because the array is owned by the computed style and the redraw
 * holds the style ref through the entire box paint. */
extern void macos9_set_gradient_stops(const int32_t *stops);
extern void macos9_set_gradient_angle(uint16_t angle);
#endif

static bool html_redraw_background(int x, int y, struct box *box, float scale,
		const struct rect *clip, colour *background_colour,
		struct box *background,
		const css_unit_ctx *unit_len_ctx,
		const struct redraw_context *ctx)
{
	bool repeat_x = false;
	bool repeat_y = false;
	bool plot_colour = true;
	bool plot_content;
	bool bg_fixed = false;
	bool clip_to_children = false;
	struct box *clip_box = box;
	int ox = x, oy = y;
	int width, height;
	css_fixed hpos = 0, vpos = 0;
	css_unit hunit = CSS_UNIT_PX, vunit = CSS_UNIT_PX;
	struct box *parent;
	struct rect r = *clip;
	css_color bgcol;
	/* MacSurf: positional init for CW8 C89.
	 * plot_style_t order: stroke_type, stroke_width, stroke_colour,
	 *                     fill_type, fill_colour. */
	plot_style_t pstyle_fill_bg = {
	        PLOT_OP_TYPE_NONE, 0, 0,
	        PLOT_OP_TYPE_SOLID, 0,
	        0, 0
	};
	nserror res;
	css_fixed br_len = 0;
	css_unit br_unit = CSS_UNIT_PX;

	pstyle_fill_bg.fill_colour = *background_colour;

	if (background && background->style && css_computed_border_radius(background->style, &br_len, &br_unit) == CSS_BORDER_RADIUS_SET) {
	        pstyle_fill_bg.border_radius = br_len * scale;
	        if (pstyle_fill_bg.border_radius > 0) {
	                css_color bcol;
	                css_fixed bw_len = 0;
	                css_unit bw_unit = CSS_UNIT_PX;
	                if (css_computed_border_top_style(background->style) != CSS_BORDER_STYLE_NONE) {
	                        css_computed_border_top_color(background->style, &bcol);
	                        pstyle_fill_bg.stroke_type = PLOT_OP_TYPE_SOLID; /* simplify */
	                        pstyle_fill_bg.stroke_colour = nscss_color_to_ns(bcol);
	                        pstyle_fill_bg.stroke_width = bw_len * scale;
	                }
	        }
	}
	if (background && background->style) {
	        int32_t bsh;
	        int32_t grad_col;
	        css_fixed op_fixed = 0;
	        /* fixes49 -- opacity passes through to the rectangle plotter
	         * so it can switch to a stipple pattern when < ~0.85.
	         * fixes76 -- if -macsurf-animation-opacity is SET, the
	         * animated value overrides the static one. */
	        if (css_computed_opacity(background->style, &op_fixed) ==
	                        CSS_OPACITY_SET) {
	                pstyle_fill_bg.opacity = (plot_style_fixed)op_fixed;
	        } else {
	                pstyle_fill_bg.opacity = (plot_style_fixed)PLOT_STYLE_SCALE;
	        }
	        {
	                int32_t anim_packed = 0;
	                if (css_computed_macsurf_animation_opacity(
	                                background->style, &anim_packed) ==
	                                CSS_MACSURF_ANIMATION_OPACITY_SET) {
	                        int rw, rh;
	                        pstyle_fill_bg.opacity = (plot_style_fixed)
	                                macsurf_anim_opacity_resolve_plot_fixed(
	                                        anim_packed);
	                        /* fixes76b -- queue a per-box rect invalidate
	                         * so the tick refreshes only this badge, not
	                         * the whole page. */
	                        rw = box->padding[LEFT] + box->width +
	                                        box->padding[RIGHT];
	                        rh = box->padding[TOP] + box->height +
	                                        box->padding[BOTTOM];
	                        macos9_animation_register_rect(x, y, rw, rh);
	                }
	        }
	        /* fixes71 -- -macsurf-transform packed value to the plotter.
	         * fixes73 -- scale companion in transform_b. */
	        {
	                int32_t tfm = 0;
	                if (css_computed_macsurf_transform(background->style, &tfm) ==
	                                CSS_MACSURF_TRANSFORM_SET) {
	                        pstyle_fill_bg.transform = (int)tfm;
	                        pstyle_fill_bg.transform_b =
	                                (int)css_computed_macsurf_transform_b(
	                                        background->style);
	                } else {
	                        pstyle_fill_bg.transform = 0;
	                        pstyle_fill_bg.transform_b = (int)0x01000100;
	                }
	        }
	        /* fixes77 -- -macsurf-animation-rotate overrides the rotation
	         * field of transform with the current tick's interpolated
	         * angle. Preserves tx/ty/scale from any static transform. */
	        {
	                int32_t anim_rot = 0;
	                if (css_computed_macsurf_animation_rotate(
	                                background->style, &anim_rot) ==
	                                CSS_MACSURF_ANIMATION_ROTATE_SET) {
	                        int cur_deg =
	                                macsurf_anim_rotate_resolve_degrees(
	                                        anim_rot);
	                        uint32_t cur_tfm =
	                                (uint32_t)pstyle_fill_bg.transform;
	                        int rw_box, rh_box, inflate;
	                        /* Q10.6: integer degrees * 64 in upper 16 bits. */
	                        cur_tfm = (cur_tfm & 0x0000ffffu) |
	                                ((((uint32_t)cur_deg) << 6) << 16);
	                        pstyle_fill_bg.transform = (int)cur_tfm;
	                        if (pstyle_fill_bg.transform_b == 0) {
	                                pstyle_fill_bg.transform_b =
	                                        (int)0x01000100;
	                        }
	                        /* Inflate invalidate rect by half max dim so a
	                         * 45-deg rotation's corners stay covered. */
	                        rw_box = box->padding[LEFT] + box->width +
	                                        box->padding[RIGHT];
	                        rh_box = box->padding[TOP] + box->height +
	                                        box->padding[BOTTOM];
	                        inflate = (rw_box > rh_box ? rw_box : rh_box)
	                                        / 2 + 4;
	                        macos9_animation_register_rect(
	                                x - inflate, y - inflate,
	                                rw_box + 2 * inflate,
	                                rh_box + 2 * inflate);
	                }
	        }
	        if (css_computed_box_shadow(background->style, &bsh) == CSS_BOX_SHADOW_SET) {
	                /* MacSurf fixes48/200 -- packed v2: h/v/inset/rgb555. */
	                int8_t hoff_px = (int8_t)((((uint32_t)bsh) >> 24) & 0xff);
	                int8_t voff_px = (int8_t)((((uint32_t)bsh) >> 16) & 0xff);
	                bool inset = (((uint32_t)bsh) & 0x8000) != 0;
	                uint16_t rgb555 = (uint16_t)(((uint32_t)bsh) & 0x7fff);
	                pstyle_fill_bg.box_shadow =
	                        ((plot_style_fixed)hoff_px) << PLOT_STYLE_RADIX;
	                pstyle_fill_bg.box_shadow_y =
	                        ((plot_style_fixed)voff_px) << PLOT_STYLE_RADIX;
	                pstyle_fill_bg.box_shadow_inset = inset;
	                if (rgb555 != 0) {
	                        uint8_t r5 = (uint8_t)((rgb555 >> 10) & 0x1f);
	                        uint8_t g5 = (uint8_t)((rgb555 >>  5) & 0x1f);
	                        uint8_t b5 = (uint8_t)((rgb555      ) & 0x1f);
	                        uint8_t r = (uint8_t)((r5 << 3) | (r5 >> 2));
	                        uint8_t g = (uint8_t)((g5 << 3) | (g5 >> 2));
	                        uint8_t b = (uint8_t)((b5 << 3) | (b5 >> 2));
	                        /* NetSurf colour: R in low byte. */
	                        pstyle_fill_bg.box_shadow_color =
	                                (colour)(((uint32_t)b << 16) |
	                                         ((uint32_t)g <<  8) |
	                                          (uint32_t)r);
	                } else {
	                        pstyle_fill_bg.box_shadow_color = 0;
	                }
	                /* fixes361h / 362 — extra shadows go through the
	                 * macos9_set_box_shadow_{2,3} one-shot statics;
	                 * plotter reads-then-clears. Avoids the stack-
	                 * garbage class that fixes361b/g hit when
	                 * extending plot_style_t. */
#ifdef __MACOS9__
	                macos9_set_box_shadow_2(css_computed_box_shadow_2(
	                                background->style));
	                macos9_set_box_shadow_3(css_computed_box_shadow_3(
	                                background->style));
	                /* fixes364 — alternating-row stripes from cssh_css.c
	                 * rewrite of `repeating-linear-gradient(to bottom,
	                 * c1, c2, ...)`. */
	                {
	                        int32_t hstripe_val =
	                                css_computed_macsurf_hstripe_bg(
	                                        background->style);
	                        /* fixes366f — only call the setter when the
	                         * value is set. Calling with 0 wipes any
	                         * earlier non-zero value before the actual
	                         * paint consumes it. Mirrors the existing
	                         * gradient_stops gating pattern. */
	                        if (hstripe_val != 0) {
	                                if (hstripe_seen_a < 5) {
	                                        macsurf_debug_log_writef(
	                                          "redraw: hstripe_bg=%p set on box (bg-path)",
	                                          (void *)background);
	                                        hstripe_seen_a++;
	                                }
	                                macos9_set_hstripe_bg(hstripe_val);
	                        }
	                }
	                /* fixes365c — 2x2 dot-grid texture from cssh_css.c
	                 * rewrite of two-layer 1px linear-gradient pattern. */
	                {
	                        int32_t dotgrid_val =
	                                css_computed_macsurf_dotgrid(
	                                        background->style);
	                        if (dotgrid_val != 0) {
	                                if (dotgrid_seen_a < 5) {
	                                        macsurf_debug_log_writef(
	                                          "redraw: dotgrid=%p set on box (bg-path)",
	                                          (void *)background);
	                                        dotgrid_seen_a++;
	                                }
	                                macos9_set_dotgrid(dotgrid_val);
	                        }
	                }
#endif
	                (void)scale;  /* scale baked into offset->fixed shift */
	        }
	        /* MacSurf fixes47/48 -- the gradient slot stores BOTH
	         * endpoint colours plus a direction bit. Unpack and pick
	         * vertical vs horizontal fill_type accordingly. */
	        if (css_computed_macsurf_gradient(background->style, &grad_col) ==
	                        CSS_MACSURF_GRADIENT_SET) {
	                colour gc1, gc2;
	                bool grad_h = false;
	                bool grad_r = false;
	                const css_color *full_stops;
	                macsurf_gradient_unpack(grad_col, &gc1, &gc2,
	                                        &grad_h, &grad_r);
	                /* fixes344b — when the rule used rgba(.../X) or
	                 * `transparent` stops, libcss stored the full
	                 * ARGB in the outer style side-channel. Pre-
	                 * blend toward the body's background-color so
	                 * the painter (which has no alpha pipeline) gets
	                 * a colour that visually approximates the
	                 * intended alpha compositing. Assumed surface =
	                 * the box's effective background-color when
	                 * resolvable, else white. */
	                full_stops = css_computed_macsurf_gradient_full(
	                        background->style);
	                if (full_stops != NULL) {
	                        css_color bg_argb = 0xFFFFFFFFu; /* white */
	                        css_color css_bg = 0;
	                        if (css_computed_background_color(
	                                        background->style, &css_bg) ==
	                                        CSS_BACKGROUND_COLOR_COLOR) {
	                                if (((css_bg >> 24) & 0xff) != 0)
	                                        bg_argb = css_bg;
	                        }
	                        {
	                                int i;
	                                css_color stops[2];
	                                stops[0] = full_stops[0];
	                                stops[1] = full_stops[1];
	                                for (i = 0; i < 2; i++) {
	                                        css_color v = stops[i];
	                                        unsigned int a =
	                                                (v >> 24) & 0xff;
	                                        unsigned int r =
	                                                (v >> 16) & 0xff;
	                                        unsigned int g =
	                                                (v >>  8) & 0xff;
	                                        unsigned int b =
	                                                (v >>  0) & 0xff;
	                                        unsigned int br =
	                                                (bg_argb >> 16) & 0xff;
	                                        unsigned int bgn =
	                                                (bg_argb >>  8) & 0xff;
	                                        unsigned int bb =
	                                                (bg_argb >>  0) & 0xff;
	                                        unsigned int inv = 255 - a;
	                                        unsigned int blR =
	                                                (r * a + br * inv) / 255;
	                                        unsigned int blG =
	                                                (g * a + bgn * inv) / 255;
	                                        unsigned int blB =
	                                                (b * a + bb * inv) / 255;
	                                        /* Repack to NetSurf
	                                         * colour (BGR low to high). */
	                                        stops[i] =
	                                                (blB << 16) |
	                                                (blG <<  8) |
	                                                 blR;
	                                }
	                                gc1 = (colour)stops[0];
	                                gc2 = (colour)stops[1];
	                        }
	                }
	                pstyle_fill_bg.fill_type = grad_r ?
	                        PLOT_OP_TYPE_RADIAL_GRADIENT :
	                        (grad_h ? PLOT_OP_TYPE_LINEAR_GRADIENT_H :
	                                  PLOT_OP_TYPE_LINEAR_GRADIENT);
	                pstyle_fill_bg.fill_colour  = gc1;
	                pstyle_fill_bg.fill_colour2 = gc2;
	                /* fixes345 — plumb the radial size+position prefix
	                 * through to the painter. radial_set=false means
	                 * fall back to existing centered-fill behaviour. */
	                pstyle_fill_bg.radial_set = false;
	                pstyle_fill_bg.radial_sx = -1;
	                pstyle_fill_bg.radial_sy = -1;
	                pstyle_fill_bg.radial_px = -1;
	                pstyle_fill_bg.radial_py = -1;
	                if (grad_r) {
	                        const int32_t *rad =
	                                css_computed_macsurf_gradient_radial(
	                                        background->style);
	                        if (rad != NULL) {
	                                pstyle_fill_bg.radial_set = true;
	                                pstyle_fill_bg.radial_sx = (int)rad[0];
	                                pstyle_fill_bg.radial_sy = (int)rad[1];
	                                pstyle_fill_bg.radial_px = (int)rad[2];
	                                pstyle_fill_bg.radial_py = (int)rad[3];
	                        }
	                }
	                /* fixes365b — diagonal / 3-stop side-channel. When
	                 * the cascade allocated the extended descriptor, push
	                 * the angle + stops array to the plotter via the
	                 * one-shot statics. For cardinal angles (0/90/180/270)
	                 * we keep the existing horizontal/vertical fast path
	                 * so opacity/output is byte-for-byte preserved. */
#ifdef __MACOS9__
	                if (!grad_r) {
	                        const int32_t *grad_ext =
	                                css_computed_macsurf_gradient_stops(
	                                        background->style);
	                        if (grad_ext != NULL) {
	                                int a = (int)((uint32_t)grad_ext[0] & 0xffffu);
	                                while (a < 0) a += 360;
	                                a = a % 360;
	                                if (a != 0 && a != 90 &&
	                                    a != 180 && a != 270) {
	                                        pstyle_fill_bg.fill_type =
	                                                PLOT_OP_TYPE_LINEAR_GRADIENT;
	                                }
	                                if (grad_seen_a < 5) {
	                                        macsurf_debug_log_writef(
	                                          "redraw: gradient_stops=%p set on box (bg-path)",
	                                          (void *)grad_ext);
	                                        grad_seen_a++;
	                                }
	                                macos9_set_gradient_stops(grad_ext);
	                                macos9_set_gradient_angle((uint16_t)a);
	                        }
	                }
#endif
	        }
	}	if (ctx->background_images == false)
		return true;

	plot_content = (background->background != NULL);

	if (plot_content) {
		if (!box->parent) {
			/* Root element, special case:
			 * background origin calc. is based on margin box */
			x -= box->margin[LEFT] * scale;
			y -= box->margin[TOP] * scale;
			width = box->margin[LEFT] + box->padding[LEFT] +
					box->width + box->padding[RIGHT] +
					box->margin[RIGHT];
			height = box->margin[TOP] + box->padding[TOP] +
					box->height + box->padding[BOTTOM] +
					box->margin[BOTTOM];
		} else {
			width = box->padding[LEFT] + box->width +
					box->padding[RIGHT];
			height = box->padding[TOP] + box->height +
					box->padding[BOTTOM];
		}

		/* fixes137: background-attachment: fixed.
		 *
		 * When fixed, the image anchors to the viewport instead of
		 * the element's box: as the page scrolls, the image stays
		 * still on screen. We override (x, y) to the viewport's
		 * top-left in page-coords (= scroll offset), and use
		 * viewport dims as the position-percent context so the
		 * 50%/50% case lands at the centre of the visible window,
		 * not the centre of the element.
		 *
		 * The box's clip rect `r` stays unchanged below, so the
		 * fixed image is still only visible inside the element's
		 * bounds -- the rest is clipped by the active QD clipRgn.
		 *
		 * The image-clip-narrowing further down (for non-repeating
		 * images) is skipped when bg_fixed is true, because that
		 * code narrows `r` to (image_at_viewport_x, image_at_
		 * viewport_y, +width, +height), which would land far
		 * outside the box when scrolled. */
		if (background->style &&
				css_computed_background_attachment(
					background->style) ==
				CSS_BACKGROUND_ATTACHMENT_FIXED) {
#ifdef __MACOS9__
			int fx = 0, fy = 0, fw = 0, fh = 0;
			(void) macos9_get_bg_fixed_origin(&fx, &fy, &fw, &fh);
			if (fw > 0 && fh > 0) {
				x = fx;
				y = fy;
				width = fw;
				height = fh;
				bg_fixed = true;
			}
#endif
		}

		/* handle background-repeat */
		switch (css_computed_background_repeat(background->style)) {
		case CSS_BACKGROUND_REPEAT_REPEAT:
			repeat_x = repeat_y = true;
			/* optimisation: only plot the colour if
			 * bitmap is not opaque */
			plot_colour = !content_get_opaque(background->background);
			break;

		case CSS_BACKGROUND_REPEAT_REPEAT_X:
			repeat_x = true;
			break;

		case CSS_BACKGROUND_REPEAT_REPEAT_Y:
			repeat_y = true;
			break;

		case CSS_BACKGROUND_REPEAT_NO_REPEAT:
			break;

		default:
			break;
		}

		/* handle background-position */
		css_computed_background_position(background->style,
				&hpos, &hunit, &vpos, &vunit);
		if (hunit == CSS_UNIT_PCT) {
			x += (width -
				content_get_width(background->background)) *
				scale * FIXTOFLT(hpos) / 100.;
		} else {
			x += (int) (FIXTOFLT(css_unit_len2device_px(
					background->style, unit_len_ctx,
					hpos, hunit)) * scale);
		}

		if (vunit == CSS_UNIT_PCT) {
			y += (height -
				content_get_height(background->background)) *
				scale * FIXTOFLT(vpos) / 100.;
		} else {
			y += (int) (FIXTOFLT(css_unit_len2device_px(
					background->style, unit_len_ctx,
					vpos, vunit)) * scale);
		}
	}

	/* special case for table rows as their background needs
	 * to be clipped to all the cells */
	if (box->type == BOX_TABLE_ROW) {
		css_fixed h = 0, v = 0;
		css_unit hu = CSS_UNIT_PX, vu = CSS_UNIT_PX;

		for (parent = box->parent;
			((parent) && (parent->type != BOX_TABLE));
				parent = parent->parent);
		assert(parent && (parent->style));

		css_computed_border_spacing(parent->style, &h, &hu, &v, &vu);

		clip_to_children = (h > 0) || (v > 0);

		if (clip_to_children)
			clip_box = box->children;
	}

	for (; clip_box; clip_box = clip_box->next) {
		/* clip to child boxes if needed */
		if (clip_to_children) {
			assert(clip_box->type == BOX_TABLE_CELL);

			/* update clip.* to the child cell */
			r.x0 = ox + (clip_box->x * scale);
			r.y0 = oy + (clip_box->y * scale);
			r.x1 = r.x0 + (clip_box->padding[LEFT] +
					clip_box->width +
					clip_box->padding[RIGHT]) * scale;
			r.y1 = r.y0 + (clip_box->padding[TOP] +
					clip_box->height +
					clip_box->padding[BOTTOM]) * scale;

			if (r.x0 < clip->x0) r.x0 = clip->x0;
			if (r.y0 < clip->y0) r.y0 = clip->y0;
			if (r.x1 > clip->x1) r.x1 = clip->x1;
			if (r.y1 > clip->y1) r.y1 = clip->y1;

			css_computed_background_color(clip_box->style, &bgcol);

			/* <td> attributes override <tr> */
			/* if the background content is opaque there
			 * is no need to plot underneath it.
			 */
			if ((r.x0 >= r.x1) ||
			    (r.y0 >= r.y1) ||
			    (nscss_color_is_transparent(bgcol) == false) ||
			    ((clip_box->background != NULL) &&
			     content_get_opaque(clip_box->background)))
				continue;
		}

		/* plot the background colour */
		css_computed_background_color(background->style, &bgcol);

		/* fixes306 (#41) — background-attachment: fixed for gradients.
		 * Anchor the gradient stops to the viewport (not the box) by
		 * substituting the viewport rect for `r` before the paint. The
		 * QD clipRgn still narrows the visible pixels to the box, so
		 * the gradient appears to scroll with the viewport behind the
		 * box's content. Mirrors the fixes137 raster-image override.
		 * Solid backgrounds are unaffected: a solid colour fills the
		 * same visible region regardless of whether `r` is sized to
		 * the box or the viewport (the QD clip narrows it either way),
		 * so we override unconditionally on attachment:fixed. */
#ifdef __MACOS9__
		if (background && background->style &&
		    css_computed_background_attachment(background->style) ==
		    CSS_BACKGROUND_ATTACHMENT_FIXED) {
			int fx = 0, fy = 0, fw = 0, fh = 0;
			(void) macos9_get_bg_fixed_origin(&fx, &fy, &fw, &fh);
			if (fw > 0 && fh > 0) {
				r.x0 = fx;
				r.y0 = fy;
				r.x1 = fx + fw;
				r.y1 = fy + fh;
			}
		}
#endif

		if (nscss_color_is_transparent(bgcol) == false) {
			int32_t grad_col_late = 0;
			*background_colour = nscss_color_to_ns(bgcol);
			pstyle_fill_bg.fill_colour = *background_colour;
			/* MacSurf fixes40+45 -- -macsurf-gradient takes
			 * precedence over background-color when the rule
			 * also carries a `background: linear-gradient(...)`
			 * shorthand. Route the captured css_color through
			 * nscss_color_to_ns so byte order matches what the
			 * rectangle plotter expects (R in low byte). */
			if (background && background->style &&
			    css_computed_macsurf_gradient(background->style,
			        &grad_col_late) == CSS_MACSURF_GRADIENT_SET) {
				colour gc1, gc2;
				bool grad_h = false;
				bool grad_r = false;
				macsurf_gradient_unpack(grad_col_late,
						&gc1, &gc2, &grad_h, &grad_r);
				pstyle_fill_bg.fill_type = grad_r ?
						PLOT_OP_TYPE_RADIAL_GRADIENT :
						(grad_h ? PLOT_OP_TYPE_LINEAR_GRADIENT_H :
							  PLOT_OP_TYPE_LINEAR_GRADIENT);
				pstyle_fill_bg.fill_colour  = gc1;
				pstyle_fill_bg.fill_colour2 = gc2;
				/* fixes365b — extended (diagonal / 3-stop). */
#ifdef __MACOS9__
				if (!grad_r) {
					const int32_t *grad_ext =
						css_computed_macsurf_gradient_stops(
							background->style);
					if (grad_ext != NULL) {
						int a = (int)((uint32_t)grad_ext[0] & 0xffffu);
						while (a < 0) a += 360;
						a = a % 360;
						if (a != 0 && a != 90 &&
						    a != 180 && a != 270) {
							pstyle_fill_bg.fill_type =
								PLOT_OP_TYPE_LINEAR_GRADIENT;
						}
						if (grad_seen_b < 5) {
							macsurf_debug_log_writef(
							  "redraw: gradient_stops=%p set on box (bg-color-set)",
							  (void *)grad_ext);
							grad_seen_b++;
						}
						macos9_set_gradient_stops(grad_ext);
						macos9_set_gradient_angle((uint16_t)a);
					}
				}
#endif
			}
			if (plot_colour) {
				/* fixes201: route through the bg-size tiling
				 * helper so gradient backgrounds repeat at
				 * the requested tile size when bg-size is set
				 * to a specific px value. */
				res = html_redraw_paint_gradient_tiled(ctx,
						&pstyle_fill_bg, &r,
						background ? background->style : NULL);
				if (res != NSERROR_OK) {
					return false;
				}
			}
		} else {
			int32_t grad_col_late;
			/* MacSurf: even when background-color is transparent,
			 * a -macsurf-gradient: SET should still paint. */
			if (background && background->style &&
			    css_computed_macsurf_gradient(background->style,
			        &grad_col_late) == CSS_MACSURF_GRADIENT_SET) {
				colour gc1, gc2;
				bool grad_h = false;
				bool grad_r = false;
				macsurf_gradient_unpack(grad_col_late,
						&gc1, &gc2, &grad_h, &grad_r);
				pstyle_fill_bg.fill_type = grad_r ?
						PLOT_OP_TYPE_RADIAL_GRADIENT :
						(grad_h ? PLOT_OP_TYPE_LINEAR_GRADIENT_H :
							  PLOT_OP_TYPE_LINEAR_GRADIENT);
				pstyle_fill_bg.fill_colour  = gc1;
				pstyle_fill_bg.fill_colour2 = gc2;
				/* fixes365b — extended (diagonal / 3-stop). */
#ifdef __MACOS9__
				if (!grad_r) {
					const int32_t *grad_ext =
						css_computed_macsurf_gradient_stops(
							background->style);
					if (grad_ext != NULL) {
						int a = (int)((uint32_t)grad_ext[0] & 0xffffu);
						while (a < 0) a += 360;
						a = a % 360;
						if (a != 0 && a != 90 &&
						    a != 180 && a != 270) {
							pstyle_fill_bg.fill_type =
								PLOT_OP_TYPE_LINEAR_GRADIENT;
						}
						if (grad_seen_c < 5) {
							macsurf_debug_log_writef(
							  "redraw: gradient_stops=%p set on box (bg-transparent)",
							  (void *)grad_ext);
							grad_seen_c++;
						}
						macos9_set_gradient_stops(grad_ext);
						macos9_set_gradient_angle((uint16_t)a);
					}
				}
#endif
				res = html_redraw_paint_gradient_tiled(ctx,
						&pstyle_fill_bg, &r,
						background->style);
				if (res != NSERROR_OK) {
					return false;
				}
			}
		}
		/* and plot the image */
		if (plot_content) {
			width = content_get_width(background->background);
			height = content_get_height(background->background);

			/* ensure clip area only as large as required.
			 * fixes137: skip narrowing when bg-attachment is
			 * fixed -- the image is anchored to the viewport,
			 * not the box, so (x, +width) may lie far outside
			 * the box and would null the clip. Let the QD
			 * clipRgn (already box-bound) handle the natural
			 * clipping instead. */
			if (!repeat_x && !bg_fixed) {
				if (r.x0 < x)
					r.x0 = x;
				if (r.x1 > x + width * scale)
					r.x1 = x + width * scale;
			}
			if (!repeat_y && !bg_fixed) {
				if (r.y0 < y)
					r.y0 = y;
				if (r.y1 > y + height * scale)
					r.y1 = y + height * scale;
			}
			/* valid clipping rectangles only */
			if ((r.x0 < r.x1) && (r.y0 < r.y1)) {
				struct content_redraw_data bg_data;

				res = ctx->plot->clip(ctx, &r);
				if (res != NSERROR_OK) {
					return false;
				}

				bg_data.x = x;
				bg_data.y = y;
				bg_data.width = ceilf(width * scale);
				bg_data.height = ceilf(height * scale);
				bg_data.background_colour = *background_colour;
				bg_data.scale = scale;
				bg_data.repeat_x = repeat_x;
				bg_data.repeat_y = repeat_y;

				/* fixes191b -- background-size consumer.
				 *
				 * The default (unset / 0) keeps the historical
				 * MacSurf behaviour where each "tile" is the
				 * box size (one tile fills the box). When set,
				 * resolve to a tile dimension per axis:
				 *
				 *   auto (0)  - use natural image dimension
				 *               (or aspect-preserved when the
				 *                other axis has an explicit size)
				 *   +N px     - tile that many pixels (scaled)
				 *   cover (-1)- scale to MAX(box/nat ratios)
				 *               so the image covers the box.
				 *   contain (-2)- scale to MIN ratio so the
				 *               image fits inside the box.
				 *
				 * Integer math (no int64) per CW8 PPC long-long
				 * codegen gotcha. */
				if (background->style != NULL) {
					int32_t bgsz = css_computed_background_size(
							background->style);
					if (bgsz != 0) {
						int16_t wc = (int16_t)(
							(bgsz >> 16) & 0xFFFF);
						int16_t hc = (int16_t)(
							bgsz & 0xFFFF);
						int nat_w =
							content_get_width(
							background->background);
						int nat_h =
							content_get_height(
							background->background);
						int box_w = (int)ceilf(
							width * scale);
						int box_h = (int)ceilf(
							height * scale);
						int tile_w = box_w;
						int tile_h = box_h;
						if (nat_w < 1) nat_w = 1;
						if (nat_h < 1) nat_h = 1;
						if (wc == -1 || hc == -1) {
							/* cover */
							if (nat_w * box_h >
							  nat_h * box_w) {
								tile_h = box_h;
								tile_w = (nat_w *
								  box_h) / nat_h;
							} else {
								tile_w = box_w;
								tile_h = (nat_h *
								  box_w) / nat_w;
							}
						} else if (wc == -2 ||
								hc == -2) {
							/* contain */
							if (nat_w * box_h <
							  nat_h * box_w) {
								tile_h = box_h;
								tile_w = (nat_w *
								  box_h) / nat_h;
							} else {
								tile_w = box_w;
								tile_h = (nat_h *
								  box_w) / nat_w;
							}
						} else {
							/* per-axis explicit
							 * or auto. */
							if (wc > 0) {
								tile_w = (int)(
								(float)wc *
								scale);
							} else if (hc > 0) {
								tile_w = (int)(
								(float)hc *
								(float)nat_w /
								(float)nat_h *
								scale);
							} else {
								tile_w = nat_w;
							}
							if (hc > 0) {
								tile_h = (int)(
								(float)hc *
								scale);
							} else if (wc > 0) {
								tile_h = (int)(
								(float)wc *
								(float)nat_h /
								(float)nat_w *
								scale);
							} else {
								tile_h = nat_h;
							}
						}
						if (tile_w < 1) tile_w = 1;
						if (tile_h < 1) tile_h = 1;
						bg_data.width = tile_w;
						bg_data.height = tile_h;
					}
				}

				/* We just continue if redraw fails */
				content_redraw(background->background,
						&bg_data, &r, ctx);
			}
		}

		/* only <tr> rows being clipped to child boxes loop */
		if (!clip_to_children)
			return true;
	}
	return true;
}


/**
 * Plot an inline's background and/or background image.
 *
 * \param  x	  coordinate of box
 * \param  y	  coordinate of box
 * \param  box	  BOX_INLINE which created the background
 * \param  scale  scale for redraw
 * \param  clip	  coordinates of clip rectangle
 * \param  b	  coordinates of border edge rectangle
 * \param  first  true if this is the first rectangle associated with the inline
 * \param  last   true if this is the last rectangle associated with the inline
 * \param  background_colour  updated to current background colour if plotted
 * \param  unit_len_ctx  Length conversion context
 * \param  ctx      current redraw context
 * \return true if successful, false otherwise
 */

static bool html_redraw_inline_background(int x, int y, struct box *box,
		float scale, const struct rect *clip, struct rect b,
		bool first, bool last, colour *background_colour,
		const css_unit_ctx *unit_len_ctx,
		const struct redraw_context *ctx)
{
	struct rect r = *clip;
	bool repeat_x = false;
	bool repeat_y = false;
	bool plot_colour = true;
	bool plot_content;
	css_fixed hpos = 0, vpos = 0;
	css_unit hunit = CSS_UNIT_PX, vunit = CSS_UNIT_PX;
	css_color bgcol;
	/* MacSurf: positional init for CW8 C89.
	 * plot_style_t order: stroke_type, stroke_width, stroke_colour,
	 *                     fill_type, fill_colour. */
	plot_style_t pstyle_fill_bg = {
	        PLOT_OP_TYPE_NONE, 0, 0,
	        PLOT_OP_TYPE_SOLID, 0,
	        0, 0
	};
	nserror res;
	css_fixed br_len = 0;
	css_unit br_unit = CSS_UNIT_PX;

	pstyle_fill_bg.fill_colour = *background_colour;

	if (box && box->style && css_computed_border_radius(box->style, &br_len, &br_unit) == CSS_BORDER_RADIUS_SET) {
	        pstyle_fill_bg.border_radius = br_len * scale;
	        if (pstyle_fill_bg.border_radius > 0) {
	                css_color bcol;
	                css_fixed bw_len = 0;
	                css_unit bw_unit = CSS_UNIT_PX;
	                if (css_computed_border_top_style(box->style) != CSS_BORDER_STYLE_NONE) {
	                        css_computed_border_top_color(box->style, &bcol);
	                        pstyle_fill_bg.stroke_type = PLOT_OP_TYPE_SOLID;
	                        pstyle_fill_bg.stroke_colour = nscss_color_to_ns(bcol);
	                        pstyle_fill_bg.stroke_width = bw_len * scale;
	                }
	        }
	}
	if (box && box->style) {
	        int32_t bsh;
	        int32_t grad_col_inline;
	        css_fixed op_fixed = 0;
	        /* fixes49 -- opacity mirror for inline path.
	         * fixes76 -- animation override (inline path). */
	        if (css_computed_opacity(box->style, &op_fixed) ==
	                        CSS_OPACITY_SET) {
	                pstyle_fill_bg.opacity = (plot_style_fixed)op_fixed;
	        } else {
	                pstyle_fill_bg.opacity = (plot_style_fixed)PLOT_STYLE_SCALE;
	        }
	        {
	                int32_t anim_packed_il = 0;
	                if (css_computed_macsurf_animation_opacity(
	                                box->style, &anim_packed_il) ==
	                                CSS_MACSURF_ANIMATION_OPACITY_SET) {
	                        pstyle_fill_bg.opacity = (plot_style_fixed)
	                                macsurf_anim_opacity_resolve_plot_fixed(
	                                        anim_packed_il);
	                        /* fixes76b -- queue a per-box rect invalidate.
	                         * `b` is the border edge rect in page coords. */
	                        macos9_animation_register_rect(b.x0, b.y0,
	                                        b.x1 - b.x0, b.y1 - b.y0);
	                }
	        }
	        /* fixes71 -- -macsurf-transform mirror for inline path.
	         * fixes73 -- scale companion in transform_b. */
	        {
	                int32_t tfm_inl = 0;
	                if (css_computed_macsurf_transform(box->style, &tfm_inl) ==
	                                CSS_MACSURF_TRANSFORM_SET) {
	                        pstyle_fill_bg.transform = (int)tfm_inl;
	                        pstyle_fill_bg.transform_b =
	                                (int)css_computed_macsurf_transform_b(box->style);
	                } else {
	                        pstyle_fill_bg.transform = 0;
	                        pstyle_fill_bg.transform_b = (int)0x01000100;
	                }
	        }
	        /* fixes77 -- animation-rotate override for inline path. */
	        {
	                int32_t anim_rot_il = 0;
	                if (css_computed_macsurf_animation_rotate(
	                                box->style, &anim_rot_il) ==
	                                CSS_MACSURF_ANIMATION_ROTATE_SET) {
	                        int cur_deg =
	                                macsurf_anim_rotate_resolve_degrees(
	                                        anim_rot_il);
	                        uint32_t cur_tfm =
	                                (uint32_t)pstyle_fill_bg.transform;
	                        int bw_in, bh_in, inflate_in;
	                        cur_tfm = (cur_tfm & 0x0000ffffu) |
	                                ((((uint32_t)cur_deg) << 6) << 16);
	                        pstyle_fill_bg.transform = (int)cur_tfm;
	                        if (pstyle_fill_bg.transform_b == 0) {
	                                pstyle_fill_bg.transform_b =
	                                        (int)0x01000100;
	                        }
	                        bw_in = b.x1 - b.x0;
	                        bh_in = b.y1 - b.y0;
	                        inflate_in = (bw_in > bh_in ? bw_in : bh_in)
	                                        / 2 + 4;
	                        macos9_animation_register_rect(
	                                b.x0 - inflate_in,
	                                b.y0 - inflate_in,
	                                bw_in + 2 * inflate_in,
	                                bh_in + 2 * inflate_in);
	                }
	        }
	        if (css_computed_box_shadow(box->style, &bsh) == CSS_BOX_SHADOW_SET) {
	                /* MacSurf fixes48/200 -- packed v2: h/v/inset/rgb555. */
	                int8_t hoff_px = (int8_t)((((uint32_t)bsh) >> 24) & 0xff);
	                int8_t voff_px = (int8_t)((((uint32_t)bsh) >> 16) & 0xff);
	                bool inset = (((uint32_t)bsh) & 0x8000) != 0;
	                uint16_t rgb555 = (uint16_t)(((uint32_t)bsh) & 0x7fff);
	                pstyle_fill_bg.box_shadow =
	                        ((plot_style_fixed)hoff_px) << PLOT_STYLE_RADIX;
	                pstyle_fill_bg.box_shadow_y =
	                        ((plot_style_fixed)voff_px) << PLOT_STYLE_RADIX;
	                pstyle_fill_bg.box_shadow_inset = inset;
	                if (rgb555 != 0) {
	                        uint8_t r5 = (uint8_t)((rgb555 >> 10) & 0x1f);
	                        uint8_t g5 = (uint8_t)((rgb555 >>  5) & 0x1f);
	                        uint8_t b5 = (uint8_t)((rgb555      ) & 0x1f);
	                        uint8_t r = (uint8_t)((r5 << 3) | (r5 >> 2));
	                        uint8_t g = (uint8_t)((g5 << 3) | (g5 >> 2));
	                        uint8_t b = (uint8_t)((b5 << 3) | (b5 >> 2));
	                        pstyle_fill_bg.box_shadow_color =
	                                (colour)(((uint32_t)b << 16) |
	                                         ((uint32_t)g <<  8) |
	                                          (uint32_t)r);
	                } else {
	                        pstyle_fill_bg.box_shadow_color = 0;
	                }
	                /* fixes361h / 362 — extra shadows via one-shot
	                 * statics. fixes364 — stripe pattern via same
	                 * pattern. */
#ifdef __MACOS9__
	                macos9_set_box_shadow_2(css_computed_box_shadow_2(
	                                box->style));
	                macos9_set_box_shadow_3(css_computed_box_shadow_3(
	                                box->style));
	                {
	                        int32_t hstripe_val =
	                                css_computed_macsurf_hstripe_bg(box->style);
	                        /* fixes366f — gate setter same as bg-path. */
	                        if (hstripe_val != 0) {
	                                if (hstripe_seen_b < 5) {
	                                        macsurf_debug_log_writef(
	                                          "redraw: hstripe_bg=%p set on box (inline-path)",
	                                          (void *)box);
	                                        hstripe_seen_b++;
	                                }
	                                macos9_set_hstripe_bg(hstripe_val);
	                        }
	                }
	                /* fixes365c — 2x2 dot-grid pattern setter. */
	                {
	                        int32_t dotgrid_val =
	                                css_computed_macsurf_dotgrid(box->style);
	                        if (dotgrid_val != 0) {
	                                if (dotgrid_seen_b < 5) {
	                                        macsurf_debug_log_writef(
	                                          "redraw: dotgrid=%p set on box (inline-path)",
	                                          (void *)box);
	                                        dotgrid_seen_b++;
	                                }
	                                macos9_set_dotgrid(dotgrid_val);
	                        }
	                }
#endif
	                (void)scale;
	        }
	        /* MacSurf: mirror the html_redraw_background gradient override
	         * for the inline-background path. fixes40/47/48. */
	        if (css_computed_macsurf_gradient(box->style, &grad_col_inline) ==
	                        CSS_MACSURF_GRADIENT_SET) {
	                colour gc1, gc2;
	                bool grad_h = false;
	                bool grad_r = false;
	                macsurf_gradient_unpack(grad_col_inline,
	                                &gc1, &gc2, &grad_h, &grad_r);
	                pstyle_fill_bg.fill_type = grad_r ?
	                                PLOT_OP_TYPE_RADIAL_GRADIENT :
	                                (grad_h ? PLOT_OP_TYPE_LINEAR_GRADIENT_H :
	                                          PLOT_OP_TYPE_LINEAR_GRADIENT);
	                pstyle_fill_bg.fill_colour  = gc1;
	                pstyle_fill_bg.fill_colour2 = gc2;
	                /* fixes365b — extended (diagonal / 3-stop). */
#ifdef __MACOS9__
	                if (!grad_r) {
	                        const int32_t *grad_ext =
	                                css_computed_macsurf_gradient_stops(
	                                        box->style);
	                        if (grad_ext != NULL) {
	                                int a = (int)((uint32_t)grad_ext[0] & 0xffffu);
	                                while (a < 0) a += 360;
	                                a = a % 360;
	                                if (a != 0 && a != 90 &&
	                                    a != 180 && a != 270) {
	                                        pstyle_fill_bg.fill_type =
	                                                PLOT_OP_TYPE_LINEAR_GRADIENT;
	                                }
	                                if (grad_seen_d < 5) {
	                                        macsurf_debug_log_writef(
	                                          "redraw: gradient_stops=%p set on box (inline-path)",
	                                          (void *)grad_ext);
	                                        grad_seen_d++;
	                                }
	                                macos9_set_gradient_stops(grad_ext);
	                                macos9_set_gradient_angle((uint16_t)a);
	                        }
	                }
#endif
	        }
	}	plot_content = (box->background != NULL);
	if (html_redraw_printing && nsoption_bool(remove_backgrounds))
		return true;

	if (plot_content) {
		/* handle background-repeat */
		switch (css_computed_background_repeat(box->style)) {
		case CSS_BACKGROUND_REPEAT_REPEAT:
			repeat_x = repeat_y = true;
			/* optimisation: only plot the colour if
			 * bitmap is not opaque
			 */
			plot_colour = !content_get_opaque(box->background);
			break;

		case CSS_BACKGROUND_REPEAT_REPEAT_X:
			repeat_x = true;
			break;

		case CSS_BACKGROUND_REPEAT_REPEAT_Y:
			repeat_y = true;
			break;

		case CSS_BACKGROUND_REPEAT_NO_REPEAT:
			break;

		default:
			break;
		}

		/* handle background-position */
		css_computed_background_position(box->style,
				&hpos, &hunit, &vpos, &vunit);
		if (hunit == CSS_UNIT_PCT) {
			x += (b.x1 - b.x0 -
					content_get_width(box->background) *
					scale) * FIXTOFLT(hpos) / 100.;

			if (!repeat_x && ((hpos < 2 && !first) ||
					(hpos > 98 && !last))){
				plot_content = false;
			}
		} else {
			x += (int) (FIXTOFLT(css_unit_len2device_px(
					box->style, unit_len_ctx,
					hpos, hunit)) * scale);
		}

		if (vunit == CSS_UNIT_PCT) {
			y += (b.y1 - b.y0 -
					content_get_height(box->background) *
					scale) * FIXTOFLT(vpos) / 100.;
		} else {
			y += (int) (FIXTOFLT(css_unit_len2device_px(
					box->style, unit_len_ctx,
					vpos, vunit)) * scale);
		}
	}

	/* plot the background colour */
	css_computed_background_color(box->style, &bgcol);

	if (nscss_color_is_transparent(bgcol) == false) {
		int32_t grad_col_late;
		*background_colour = nscss_color_to_ns(bgcol);
		pstyle_fill_bg.fill_colour = *background_colour;
		/* MacSurf fixes40/47/48 — gradient overrides bg-color. */
		if (box && box->style &&
		    css_computed_macsurf_gradient(box->style,
		        &grad_col_late) == CSS_MACSURF_GRADIENT_SET) {
			colour gc1, gc2;
			bool grad_h = false;
			bool grad_r = false;
			macsurf_gradient_unpack(grad_col_late,
					&gc1, &gc2, &grad_h, &grad_r);
			pstyle_fill_bg.fill_type = grad_r ?
					PLOT_OP_TYPE_RADIAL_GRADIENT :
					(grad_h ? PLOT_OP_TYPE_LINEAR_GRADIENT_H :
						  PLOT_OP_TYPE_LINEAR_GRADIENT);
			pstyle_fill_bg.fill_colour  = gc1;
			pstyle_fill_bg.fill_colour2 = gc2;
			/* fixes365b — extended (diagonal / 3-stop). */
#ifdef __MACOS9__
			if (!grad_r) {
				const int32_t *grad_ext =
					css_computed_macsurf_gradient_stops(
						box->style);
				if (grad_ext != NULL) {
					int a = (int)((uint32_t)grad_ext[0] & 0xffffu);
					while (a < 0) a += 360;
					a = a % 360;
					if (a != 0 && a != 90 &&
					    a != 180 && a != 270) {
						pstyle_fill_bg.fill_type =
							PLOT_OP_TYPE_LINEAR_GRADIENT;
					}
					if (grad_seen_e < 5) {
						macsurf_debug_log_writef(
						  "redraw: gradient_stops=%p set on box (box-color-set)",
						  (void *)grad_ext);
						grad_seen_e++;
					}
					macos9_set_gradient_stops(grad_ext);
					macos9_set_gradient_angle((uint16_t)a);
				}
			}
#endif
		}

		if (plot_colour) {
			/* fixes201: gradient bg-size tile loop (inline path). */
			res = html_redraw_paint_gradient_tiled(ctx,
					&pstyle_fill_bg, &r,
					box ? box->style : NULL);
			if (res != NSERROR_OK) {
				return false;
			}
		}
	} else {
		int32_t grad_col_late;
		/* MacSurf: transparent bg + -macsurf-gradient SET still paints. */
		if (box && box->style &&
		    css_computed_macsurf_gradient(box->style,
		        &grad_col_late) == CSS_MACSURF_GRADIENT_SET) {
			colour gc1, gc2;
			bool grad_h = false;
			bool grad_r = false;
			macsurf_gradient_unpack(grad_col_late,
					&gc1, &gc2, &grad_h, &grad_r);
			pstyle_fill_bg.fill_type = grad_r ?
					PLOT_OP_TYPE_RADIAL_GRADIENT :
					(grad_h ? PLOT_OP_TYPE_LINEAR_GRADIENT_H :
						  PLOT_OP_TYPE_LINEAR_GRADIENT);
			pstyle_fill_bg.fill_colour  = gc1;
			pstyle_fill_bg.fill_colour2 = gc2;
			/* fixes365b — extended (diagonal / 3-stop). */
#ifdef __MACOS9__
			if (!grad_r) {
				const int32_t *grad_ext =
					css_computed_macsurf_gradient_stops(
						box->style);
				if (grad_ext != NULL) {
					int a = (int)((uint32_t)grad_ext[0] & 0xffffu);
					while (a < 0) a += 360;
					a = a % 360;
					if (a != 0 && a != 90 &&
					    a != 180 && a != 270) {
						pstyle_fill_bg.fill_type =
							PLOT_OP_TYPE_LINEAR_GRADIENT;
					}
					if (grad_seen_f < 5) {
						macsurf_debug_log_writef(
						  "redraw: gradient_stops=%p set on box (box-transparent)",
						  (void *)grad_ext);
						grad_seen_f++;
					}
					macos9_set_gradient_stops(grad_ext);
					macos9_set_gradient_angle((uint16_t)a);
				}
			}
#endif
			res = html_redraw_paint_gradient_tiled(ctx,
					&pstyle_fill_bg, &r,
					box->style);
			if (res != NSERROR_OK) {
				return false;
			}
		}
	}
	/* and plot the image */
	if (plot_content) {
		int width = content_get_width(box->background);
		int height = content_get_height(box->background);

		if (!repeat_x) {
			if (r.x0 < x)
				r.x0 = x;
			if (r.x1 > x + width * scale)
				r.x1 = x + width * scale;
		}
		if (!repeat_y) {
			if (r.y0 < y)
				r.y0 = y;
			if (r.y1 > y + height * scale)
				r.y1 = y + height * scale;
		}
		/* valid clipping rectangles only */
		if ((r.x0 < r.x1) && (r.y0 < r.y1)) {
			struct content_redraw_data bg_data;

			res = ctx->plot->clip(ctx, &r);
			if (res != NSERROR_OK) {
				return false;
			}

			bg_data.x = x;
			bg_data.y = y;
			bg_data.width = ceilf(width * scale);
			bg_data.height = ceilf(height * scale);
			bg_data.background_colour = *background_colour;
			bg_data.scale = scale;
			bg_data.repeat_x = repeat_x;
			bg_data.repeat_y = repeat_y;

			/* We just continue if redraw fails */
			content_redraw(box->background, &bg_data, &r, ctx);
		}
	}

	return true;
}


/**
 * Plot text decoration for an inline box.
 *
 * \param  box     box to plot decorations for, of type BOX_INLINE
 * \param  x       x coordinate of parent of box
 * \param  y       y coordinate of parent of box
 * \param  scale   scale for redraw
 * \param  colour  colour for decorations
 * \param  ratio   position of line as a ratio of line height
 * \param  ctx	   current redraw context
 * \return true if successful, false otherwise
 */

static bool
html_redraw_text_decoration_inline(struct box *box,
				   int x, int y,
				   float scale,
				   colour colour,
				   float ratio,
				   const struct redraw_context *ctx)
{
	struct box *c;
	nserror res;
	struct rect rect;
	/* MacSurf: positional init (CW8 C89). plot_style_t order:
	 * stroke_type, stroke_width, stroke_colour, fill_type, fill_colour. */
	plot_style_t plot_style_box = {
		PLOT_OP_TYPE_SOLID, 0, 0,
		PLOT_OP_TYPE_NONE, 0
	};
	plot_style_box.stroke_colour = colour;

	for (c = box->next;
	     c && c != box->inline_end;
	     c = c->next) {
		if (c->type != BOX_TEXT) {
			continue;
		}
		rect.x0 = (x + c->x) * scale;
		rect.y0 = (y + c->y + c->height * ratio) * scale;
		rect.x1 = (x + c->x + c->width) * scale;
		rect.y1 = (y + c->y + c->height * ratio) * scale;
		res = ctx->plot->line(ctx, &plot_style_box, &rect);
		if (res != NSERROR_OK) {
			return false;
		}
	}
	return true;
}


/**
 * Plot text decoration for an non-inline box.
 *
 * \param  box     box to plot decorations for, of type other than BOX_INLINE
 * \param  x       x coordinate of box
 * \param  y       y coordinate of box
 * \param  scale   scale for redraw
 * \param  colour  colour for decorations
 * \param  ratio   position of line as a ratio of line height
 * \param  ctx	   current redraw context
 * \return true if successful, false otherwise
 */

static bool
html_redraw_text_decoration_block(struct box *box,
				  int x, int y,
				  float scale,
				  colour colour,
				  float ratio,
				  const struct redraw_context *ctx)
{
	struct box *c;
	nserror res;
	struct rect rect;
	/* MacSurf: positional init (CW8 C89). plot_style_t order:
	 * stroke_type, stroke_width, stroke_colour, fill_type, fill_colour. */
	plot_style_t plot_style_box = {
		PLOT_OP_TYPE_SOLID, 0, 0,
		PLOT_OP_TYPE_NONE, 0
	};
	plot_style_box.stroke_colour = colour;

	/* draw through text descendants */
	for (c = box->children; c; c = c->next) {
		if (c->type == BOX_TEXT) {
			rect.x0 = (x + c->x) * scale;
			rect.y0 = (y + c->y + c->height * ratio) * scale;
			rect.x1 = (x + c->x + c->width) * scale;
			rect.y1 = (y + c->y + c->height * ratio) * scale;
			res = ctx->plot->line(ctx, &plot_style_box, &rect);
			if (res != NSERROR_OK) {
				return false;
			}
		} else if ((c->type == BOX_INLINE_CONTAINER) || (c->type == BOX_BLOCK)) {
			if (!html_redraw_text_decoration_block(c,
					x + c->x, y + c->y,
					scale, colour, ratio, ctx))
				return false;
		}
	}
	return true;
}


/**
 * Plot text decoration for a box.
 *
 * \param  box       box to plot decorations for
 * \param  x_parent  x coordinate of parent of box
 * \param  y_parent  y coordinate of parent of box
 * \param  scale     scale for redraw
 * \param  background_colour  current background colour
 * \param  ctx	     current redraw context
 * \return true if successful, false otherwise
 */

static bool html_redraw_text_decoration(struct box *box,
		int x_parent, int y_parent, float scale,
		colour background_colour, const struct redraw_context *ctx)
{
	static const enum css_text_decoration_e decoration[] = {
		CSS_TEXT_DECORATION_UNDERLINE, CSS_TEXT_DECORATION_OVERLINE,
		CSS_TEXT_DECORATION_LINE_THROUGH };
	static const float line_ratio[] = { 0.9, 0.1, 0.5 };
	colour fgcol;
	unsigned int i;
	css_color col;

	css_computed_color(box->style, &col);
	fgcol = nscss_color_to_ns(col);

	/* antialias colour for under/overline */
	if (html_redraw_printing == false)
		fgcol = blend_colour(background_colour, fgcol);

	if (box->type == BOX_INLINE) {
		if (!box->inline_end)
			return true;
		for (i = 0; i != NOF_ELEMENTS(decoration); i++)
			if (css_computed_text_decoration(box->style) &
					decoration[i])
				if (!html_redraw_text_decoration_inline(box,
						x_parent, y_parent, scale,
						fgcol, line_ratio[i], ctx))
					return false;
	} else {
		for (i = 0; i != NOF_ELEMENTS(decoration); i++)
			if (css_computed_text_decoration(box->style) &
					decoration[i])
				if (!html_redraw_text_decoration_block(box,
						x_parent + box->x,
						y_parent + box->y,
						scale,
						fgcol, line_ratio[i], ctx))
					return false;
	}

	return true;
}


/**
 * Redraw the text content of a box, possibly partially highlighted
 * because the text has been selected, or matches a search operation.
 *
 * \param html The html content to redraw text within.
 * \param  box      box with text content
 * \param  x        x co-ord of box
 * \param  y        y co-ord of box
 * \param  clip     current clip rectangle
 * \param  scale    current scale setting (1.0 = 100%)
 * \param  current_background_color
 * \param  ctx	    current redraw context
 * \return true iff successful and redraw should proceed
 */

/* fixes135b/135c: locate the nearest styled ancestor and check whether
 * it specifies text-overflow: ellipsis on a clipped (overflow != visible)
 * inline axis. Returns true when an ellipsis paint should fire.
 *
 * Synthetic INLINE_CONTAINER / BOX_TEXT nodes have NULL style; the walk
 * skips them naturally. We stop at the first styled ancestor: the
 * containing block carries the text-overflow declaration. */
static bool
html_redraw_text_overflow_ellipsis_active(const struct box *box)
{
	const struct box *p;

	for (p = box->parent; p != NULL; p = p->parent) {
		if (p->style == NULL) continue;
		if (css_computed_text_overflow(p->style) !=
				CSS_TEXT_OVERFLOW_ELLIPSIS) {
			return false;
		}
		if (css_computed_overflow_x(p->style) ==
				CSS_OVERFLOW_VISIBLE) {
			return false;
		}
		return true;
	}
	return false;
}

static bool html_redraw_text_box(const html_content *html, struct box *box,
		int x, int y, const struct rect *clip, float scale,
		colour current_background_color,
		const struct redraw_context *ctx)
{
	bool excluded = (box->object != NULL);
	plot_font_style_t fstyle;

	macos9_html_redraw_text_box_calls++;

	font_plot_style_from_css(&html->unit_len_ctx, box->style, &fstyle);
	fstyle.background = current_background_color;

	if (!text_redraw(box->text,
			 box->length,
			 box->byte_offset,
			 box->space,
			 &fstyle,
			 x, y,
			 clip,
			 box->height,
			 scale,
			 excluded,
			 (struct content *)html,
			 html->sel,
			 ctx))
		return false;

	/* fixes135c: paint-after-truncate ellipsis.
	 *
	 * fixes135b mutated box->text into a stack-buffer truncated copy
	 * before passing it to text_redraw; that produced corrupted output
	 * I couldn't pin from code review (text starting mid-word with
	 * non-deterministic byte patterns). 135c sidesteps the whole
	 * text-buffer-mutation class of bug: the original text draws
	 * normally (clipped at clip->x1 by overflow:hidden), then we paint
	 * an ellipsis on top of the rightmost slice. Two extra plot calls
	 * per truncated box. No binary search, no UTF-8 byte-boundary
	 * arithmetic, no temporary buffers.
	 *
	 * Trigger: text-overflow:ellipsis ancestor with overflow != visible
	 * AND the text box would paint past clip->x1 (so overflow:hidden
	 * has actually clipped something) AND x is still within the clip
	 * (so there's room to paint the marker). */
	if (box->text != NULL && box->length > 0 && box->width > 0 &&
			(x + box->width > clip->x1) && (x < clip->x1) &&
			html_redraw_text_overflow_ellipsis_active(box)) {
		const char *marker = "\xE2\x80\xA6"; /* U+2026 -> MacRoman 0xC9 */
		size_t marker_len = 3;
		int marker_w = 0;
		nserror res;

		res = guit->layout->width(&fstyle, marker, marker_len,
				&marker_w);
		if (res == NSERROR_OK && marker_w > 0 &&
				marker_w < (clip->x1 - x)) {
			plot_style_t fill_style = {
				PLOT_OP_TYPE_NONE
			}; /* fixes361g — zero-init ALL fields so plotters.c
			    * doesn't read garbage in box_shadow_2_* etc. */
			struct rect r;
			int ell_x = clip->x1 - marker_w;

			/* Clear the rightmost marker_w pixels with the
			 * container's background colour so the partial glyph
			 * the original text painted there doesn't bleed
			 * through. CW8 C89 positional init order:
			 *   stroke_type, stroke_width, stroke_colour,
			 *   fill_type, fill_colour. */
			fill_style.stroke_type = PLOT_OP_TYPE_NONE;
			fill_style.stroke_width = 0;
			fill_style.stroke_colour = 0;
			fill_style.fill_type = PLOT_OP_TYPE_SOLID;
			fill_style.fill_colour = current_background_color;

			r.x0 = ell_x;
			r.y0 = y;
			r.x1 = clip->x1;
			r.y1 = y + box->height;
			(void) ctx->plot->rectangle(ctx, &fill_style, &r);

			/* Paint the ellipsis at the cleared slot. Use the
			 * same baseline math text_redraw uses. */
			(void) ctx->plot->text(ctx, &fstyle,
					ell_x,
					y + font_plot_style_baseline(
						&fstyle,
						(int)(box->height * scale)),
					marker, marker_len);
		}
	}

	return true;
}

static int html_redraw_multicol_default_gap(
		struct box *box,
		const css_unit_ctx *unit_len_ctx)
{
	css_fixed one_em;
	int gap;

	one_em = INTTOFIX(1);
	gap = FIXTOINT(css_unit_len2device_px(box->style,
			unit_len_ctx, one_em, CSS_UNIT_EM));
	if (gap < 0)
		gap = 0;
	return gap;
}

static bool html_redraw_multicol_resolve(
		struct box *box,
		const css_unit_ctx *unit_len_ctx,
		int *count_out,
		int *gap_out,
		int *column_width_out)
{
	uint8_t count_type;
	uint8_t gap_type;
	uint8_t width_type;
	int32_t count_value;
	css_fixed gap_len;
	css_fixed width_len;
	css_unit gap_unit;
	css_unit width_unit;
	int available_width;
	int gap;
	int count;
	int preferred_width;
	int fit_count;
	int total_gap;

	if (box == NULL || box->style == NULL)
		return false;

	available_width = box->width;
	if (available_width <= 0)
		return false;

	count_value = 0;
	gap_len = 0;
	width_len = 0;
	gap_unit = CSS_UNIT_PX;
	width_unit = CSS_UNIT_PX;
	count = 0;
	preferred_width = 0;

	count_type = css_computed_column_count(box->style, &count_value);
	width_type = css_computed_column_width(box->style, &width_len,
			&width_unit);
	gap_type = css_computed_column_gap(box->style, &gap_len, &gap_unit);

	if (gap_type == CSS_COLUMN_GAP_NORMAL) {
		gap = html_redraw_multicol_default_gap(box, unit_len_ctx);
	} else if (gap_type == CSS_COLUMN_GAP_SET) {
		gap = FIXTOINT(css_unit_len2device_px(box->style,
				unit_len_ctx, gap_len, gap_unit));
	} else {
		gap = 0;
	}
	if (gap < 0)
		gap = 0;

	if (width_type == CSS_COLUMN_WIDTH_SET) {
		preferred_width = FIXTOINT(css_unit_len2device_px(box->style,
				unit_len_ctx, width_len, width_unit));
		if (preferred_width < 0)
			preferred_width = 0;
	}

	if (count_type == CSS_COLUMN_COUNT_SET && count_value > 1)
		count = (int) count_value;

	if (preferred_width > 0) {
		fit_count = (available_width + gap) / (preferred_width + gap);
		if (fit_count < 1)
			fit_count = 1;
		if (count > 0) {
			if (fit_count < count)
				count = fit_count;
		} else {
			count = fit_count;
		}
	}

	if (count < 2)
		return false;
	if (count > 8)
		count = 8;

	total_gap = gap * (count - 1);
	if (available_width <= total_gap)
		return false;

	*column_width_out = (available_width - total_gap) / count;
	if (*column_width_out < 24)
		return false;

	*count_out = count;
	*gap_out = gap;
	return true;
}

static bool html_redraw_multicol_rule_segment(
		const struct box *box,
		int x,
		int y,
		float scale,
		const struct rect *clip,
		const struct redraw_context *ctx,
		const plot_style_t *rule_style,
		int count,
		int gap,
		int column_width,
		int top,
		int bottom)
{
	struct rect line_rect;
	int i;

	if (bottom <= top)
		return true;

	for (i = 1; i < count; i++) {
		int line_x;

		line_x = x + box->padding[LEFT] +
				i * column_width + (i - 1) * gap + gap / 2;
		line_rect.x0 = line_x * scale;
		line_rect.x1 = line_rect.x0;
		line_rect.y0 = (y + top) * scale;
		line_rect.y1 = (y + bottom) * scale;

		if (line_rect.x0 < clip->x0 || line_rect.x0 > clip->x1)
			continue;
		if (line_rect.y1 < clip->y0 || line_rect.y0 > clip->y1)
			continue;

		if (ctx->plot->line(ctx, rule_style, &line_rect) != NSERROR_OK)
			return false;
	}

	return true;
}

static bool html_redraw_multicol_rules(
		const html_content *html,
		struct box *box,
		int x,
		int y,
		float scale,
		const struct rect *clip,
		const struct redraw_context *ctx)
{
	uint8_t rule_style_type;
	uint8_t rule_width_type;
	uint8_t rule_color_type;
	css_fixed rule_len;
	css_unit rule_unit;
	css_color rule_color;
	plot_style_t rule_style = {
		PLOT_OP_TYPE_NONE
	}; /* fixes361g — zero-init all fields to keep box_shadow_2_*
	    * (plot_style.h extension) from leaking stack garbage into
	    * the plotter's second-inset paint path. */
	struct box *child;
	int count;
	int gap;
	int column_width;
	int rule_width_px;
	unsigned int segment_index;

	if (!html_redraw_multicol_resolve(box, &html->unit_len_ctx, &count,
			&gap, &column_width)) {
		return true;
	}

	rule_style_type = css_computed_column_rule_style(box->style);
	if (rule_style_type == CSS_COLUMN_RULE_STYLE_NONE ||
			rule_style_type == CSS_COLUMN_RULE_STYLE_HIDDEN ||
			rule_style_type == CSS_COLUMN_RULE_STYLE_INHERIT) {
		return true;
	}

	rule_len = 0;
	rule_unit = CSS_UNIT_PX;
	rule_width_type = css_computed_column_rule_width(box->style,
			&rule_len, &rule_unit);
	switch (rule_width_type) {
	case CSS_COLUMN_RULE_WIDTH_THIN:
		rule_width_px = 1;
		break;
	case CSS_COLUMN_RULE_WIDTH_MEDIUM:
		rule_width_px = 3;
		break;
	case CSS_COLUMN_RULE_WIDTH_THICK:
		rule_width_px = 5;
		break;
	case CSS_COLUMN_RULE_WIDTH_WIDTH:
		rule_width_px = FIXTOINT(css_unit_len2device_px(box->style,
				&html->unit_len_ctx, rule_len, rule_unit));
		break;
	default:
		rule_width_px = 0;
		break;
	}
	if (rule_width_px < 1)
		return true;

	rule_color = 0;
	rule_color_type = css_computed_column_rule_color(box->style,
			&rule_color);
	if (rule_color_type == CSS_COLUMN_RULE_COLOR_CURRENT_COLOR ||
			rule_color_type == CSS_COLUMN_RULE_COLOR_INHERIT) {
		css_computed_color(box->style, &rule_color);
	}

	rule_style.stroke_type = PLOT_OP_TYPE_SOLID;
	if (rule_style_type == CSS_COLUMN_RULE_STYLE_DASHED) {
		rule_style.stroke_type = PLOT_OP_TYPE_DASH;
	} else if (rule_style_type == CSS_COLUMN_RULE_STYLE_DOTTED) {
		rule_style.stroke_type = PLOT_OP_TYPE_DOT;
	}
	rule_style.stroke_width =
			(rule_width_px * scale) * PLOT_STYLE_SCALE;
	rule_style.stroke_colour = nscss_color_to_ns(rule_color);
	rule_style.fill_type = PLOT_OP_TYPE_NONE;
	rule_style.fill_colour = 0;
	rule_style.border_radius = 0;
	rule_style.box_shadow = 0;
	rule_style.fill_colour2 = 0;
	rule_style.box_shadow_y = 0;
	rule_style.box_shadow_color = 0;
	rule_style.opacity = PLOT_STYLE_SCALE;
	rule_style.transform = 0;
	rule_style.transform_b = 0;

	if (layout_multicol_segment_count(box) > 0) {
		int segment_top;
		int segment_bottom;

		for (segment_index = 0;
				layout_multicol_segment_bounds(box, segment_index,
				&segment_top, &segment_bottom);
				segment_index++) {
			if (!html_redraw_multicol_rule_segment(box, x, y, scale,
					clip, ctx, &rule_style, count, gap,
					column_width, segment_top, segment_bottom)) {
				return false;
			}
		}
		return true;
	}

	if (!html_redraw_multicol_rule_segment(box, x, y, scale, clip,
			ctx, &rule_style, count, gap, column_width,
			box->padding[TOP], box->padding[TOP] + box->height))
		return false;

	return true;
}

bool html_redraw_box(const html_content *html, struct box *box,
		int x_parent, int y_parent,
		const struct rect *clip, float scale,
		colour current_background_color,
		const struct redraw_context *ctx);

/**
 * Draw the various children of a box.
 *
 * \param  html	     html content
 * \param  box	     box to draw children of
 * \param  x_parent  coordinate of parent box
 * \param  y_parent  coordinate of parent box
 * \param  clip      clip rectangle
 * \param  scale     scale for redraw
 * \param  current_background_color  background colour under this box
 * \param  ctx	     current redraw context
 * \return true if successful, false otherwise
 */

/*
 * fixes147: CSS 2.1 §9.9 / Appendix E painting order at the sibling level.
 *
 * Supersedes fixes133's 3-pass model. Per CSS 2.1 within a stacking context
 * the paint order is:
 *   (a) background/border of the stacking context root (handled by caller)
 *   (b) child stacking contexts with NEGATIVE z-index (most negative first)
 *   (c) in-flow non-inline-level non-positioned descendants
 *   (d) non-positioned floats
 *   (e) in-flow inline-level non-positioned descendants
 *   (f) child stacking contexts with z-index: auto AND positioned
 *       descendants with z-index: auto
 *   (g) child stacking contexts with POSITIVE z-index (least positive first)
 *
 * At the sibling level (the level this function operates on) we bucket the
 * direct children into:
 *   - HRBSC_NEG  : stacking context, z < 0  -> paint phase (b)
 *   - HRBSC_NONE : non-stacking-context, non-positioned -> phase (c)+(e)
 *                   mixed in DOM order; this matches what real browsers
 *                   render for sibling-level interleaving
 *   - HRBSC_ZERO : stacking context, z == 0 OR positioned + z:auto -> (f)
 *   - HRBSC_POS  : stacking context, z > 0 -> (g)
 * Floats paint between phase (c)/(e) and phase (f), per spec.
 *
 * Stacking context creation triggers covered:
 *   - position != static AND z-index != auto (any value, incl. 0)
 *   - position: fixed (creates SC regardless of z-index)
 *   - opacity < 1
 *   - non-identity -macsurf-transform / transform (via fixes73)
 * Not yet covered: filter, mix-blend-mode, isolation, will-change.
 *
 * Cross-level limitation: positioned descendants of a non-SC parent
 * stay within that parent's flow rather than escaping to the nearest
 * ancestor stacking context. Full CSS 2.1 would require a recursive
 * descendant walk that classifies every box in the subtree of the
 * containing SC. Per-sibling bucketing handles ~90% of real cases
 * (modals as direct children of body, dropdowns inside positioned
 * parents that already create their own SC, etc.).
 */
#define MACOS9_Z_BUF 64

typedef enum {
	HRBSC_NONE = 0,    /* paints in flow */
	HRBSC_NEG,         /* stacking context, z < 0 */
	HRBSC_ZERO,        /* stacking context, z == 0 (or position+z:auto) */
	HRBSC_POS          /* stacking context, z > 0 */
} html_redraw_sc_class;

static html_redraw_sc_class html_redraw_box_classify(const struct box *c,
		int32_t *zout)
{
	int32_t z = 0;
	int has_explicit_z;
	css_fixed op_fixed = 0;
	int32_t tfm = 0;
	uint8_t pos;

	*zout = 0;
	if (c->style == NULL) return HRBSC_NONE;

	pos = css_computed_position(c->style);
	has_explicit_z =
		(css_computed_z_index(c->style, &z) == CSS_Z_INDEX_SET);

	/* position: fixed creates a stacking context regardless of z. */
	if (pos == CSS_POSITION_FIXED) {
		*zout = has_explicit_z ? z : 0;
		if (has_explicit_z && z < 0) return HRBSC_NEG;
		if (has_explicit_z && z > 0) return HRBSC_POS;
		return HRBSC_ZERO;
	}

	/* position != static AND explicit z-index creates a stacking context.
	 * The z value classifies neg / zero / pos. */
	if (pos != CSS_POSITION_STATIC && has_explicit_z) {
		*zout = z;
		if (z < 0) return HRBSC_NEG;
		if (z > 0) return HRBSC_POS;
		return HRBSC_ZERO;
	}

	/* opacity < 1 creates a stacking context (z effectively 0).
	 * INTTOFIX(1) is libcss fixed-point 1.0 (1 << 10 in 22.10). */
	if (css_computed_opacity(c->style, &op_fixed) == CSS_OPACITY_SET &&
			op_fixed < INTTOFIX(1)) {
		return HRBSC_ZERO;
	}

	/* Non-identity -macsurf-transform / transform creates a stacking
	 * context (fixes73 stores non-identity values as non-zero packed
	 * int32; identity is 0). */
	if (css_computed_macsurf_transform(c->style, &tfm) ==
			CSS_MACSURF_TRANSFORM_SET && tfm != 0) {
		return HRBSC_ZERO;
	}

	return HRBSC_NONE;
}

static bool html_redraw_box_children(const html_content *html, struct box *box,
		int x_parent, int y_parent,
		const struct rect *clip, float scale,
		colour current_background_color,
		const struct redraw_context *ctx)
{
	struct box *c;
	struct box *flow_buf[MACOS9_Z_BUF * 2];   /* non-SC in DOM order */
	struct box *neg_buf[MACOS9_Z_BUF];
	int32_t neg_z[MACOS9_Z_BUF];
	struct box *zero_buf[MACOS9_Z_BUF];
	struct box *pos_buf[MACOS9_Z_BUF];
	int32_t pos_z[MACOS9_Z_BUF];
	int flow_n = 0, neg_n = 0, zero_n = 0, pos_n = 0;
	int32_t zval = 0;
	int i, j;
	int x_off, y_off;

	x_off = x_parent + box->x - scrollbar_get_offset(box->scroll_x);
	y_off = y_parent + box->y - scrollbar_get_offset(box->scroll_y);

	/* Pass 1: classify all non-float children into four buckets.
	 * Defer all paint so negative-z paints first per CSS 2.1 (b). */
	for (c = box->children; c; c = c->next) {
		html_redraw_sc_class cls;

		if (c->type == BOX_FLOAT_LEFT || c->type == BOX_FLOAT_RIGHT)
			continue;

		cls = html_redraw_box_classify(c, &zval);

		if (cls == HRBSC_NEG) {
			if (neg_n < MACOS9_Z_BUF) {
				neg_buf[neg_n] = c;
				neg_z[neg_n] = zval;
				neg_n++;
				continue;
			}
		} else if (cls == HRBSC_ZERO) {
			if (zero_n < MACOS9_Z_BUF) {
				zero_buf[zero_n] = c;
				zero_n++;
				continue;
			}
		} else if (cls == HRBSC_POS) {
			if (pos_n < MACOS9_Z_BUF) {
				pos_buf[pos_n] = c;
				pos_z[pos_n] = zval;
				pos_n++;
				continue;
			}
		}

		/* HRBSC_NONE (or bucket overflow): in-flow / phase (c)+(e). */
		if (flow_n < (int)(sizeof(flow_buf) / sizeof(flow_buf[0]))) {
			flow_buf[flow_n++] = c;
		} else {
			/* Out of all buckets — paint immediately as last resort. */
			if (!html_redraw_box(html, c, x_off, y_off,
					clip, scale,
					current_background_color, ctx))
				return false;
		}
	}

	/* Pass 2-a: stable bubble sort negatives by z ascending. */
	for (i = 0; i < neg_n - 1; i++) {
		for (j = 0; j < neg_n - 1 - i; j++) {
			if (neg_z[j] > neg_z[j + 1]) {
				int32_t tv = neg_z[j];
				struct box *tb = neg_buf[j];
				neg_z[j] = neg_z[j + 1];
				neg_buf[j] = neg_buf[j + 1];
				neg_z[j + 1] = tv;
				neg_buf[j + 1] = tb;
			}
		}
	}
	/* Pass 2-b: stable bubble sort positives by z ascending. */
	for (i = 0; i < pos_n - 1; i++) {
		for (j = 0; j < pos_n - 1 - i; j++) {
			if (pos_z[j] > pos_z[j + 1]) {
				int32_t tv = pos_z[j];
				struct box *tb = pos_buf[j];
				pos_z[j] = pos_z[j + 1];
				pos_buf[j] = pos_buf[j + 1];
				pos_z[j + 1] = tv;
				pos_buf[j + 1] = tb;
			}
		}
	}

	/* CSS 2.1 Appendix E paint order at the sibling level:
	 *   (b) negative-z stacking contexts (most negative first)
	 *   (c)+(d)+(e) non-SC children in DOM order, then floats
	 *   (f) zero-z stacking contexts (DOM order)
	 *   (g) positive-z stacking contexts (least positive first)
	 */

	for (i = 0; i < neg_n; i++) {
		if (!html_redraw_box(html, neg_buf[i], x_off, y_off,
				clip, scale, current_background_color, ctx))
			return false;
	}

	for (i = 0; i < flow_n; i++) {
		if (!html_redraw_box(html, flow_buf[i], x_off, y_off,
				clip, scale, current_background_color, ctx))
			return false;
	}

	for (c = box->float_children; c; c = c->next_float) {
		if (!html_redraw_box(html, c, x_off, y_off,
				clip, scale, current_background_color, ctx))
			return false;
	}

	for (i = 0; i < zero_n; i++) {
		if (!html_redraw_box(html, zero_buf[i], x_off, y_off,
				clip, scale, current_background_color, ctx))
			return false;
	}

	for (i = 0; i < pos_n; i++) {
		if (!html_redraw_box(html, pos_buf[i], x_off, y_off,
				clip, scale, current_background_color, ctx))
			return false;
	}

	return true;
}

/**
 * Recursively draw a box.
 *
 * \param  html	     html content
 * \param  box	     box to draw
 * \param  x_parent  coordinate of parent box
 * \param  y_parent  coordinate of parent box
 * \param  clip      clip rectangle
 * \param  scale     scale for redraw
 * \param  current_background_color  background colour under this box
 * \param  ctx	     current redraw context
 * \return true if successful, false otherwise
 *
 * x, y, clip_[xy][01] are in target coordinates.
 */

/* fixes650 (Track C2): the ONLY consumer of a box's element tag-type in
 * html_redraw_box is the canvas branch, yet the lookup (a DOM vtable dispatch +
 * dom_string refcount + up to 26 strcasecmp) used to run for EVERY element box
 * on EVERY redraw/scroll. Resolve it lazily here, called only after the cheap
 * REPLACE_DIM gate, so ~99.9% of boxes skip it each frame. */
static bool macsurf_box_node_is_canvas(struct box *box)
{
	dom_html_element_type tt = DOM_HTML_ELEMENT_TYPE__UNKNOWN;
	dom_exception e;
	if (box == NULL || box->node == NULL) return false;
	e = macsurf_html_element_get_tag_type(box->node, &tt);
	return (e == DOM_NO_ERR && tt == DOM_HTML_ELEMENT_TYPE_CANVAS);
}

bool html_redraw_box(const html_content *html, struct box *box,
		int x_parent, int y_parent,
		const struct rect *clip, const float scale,
		colour current_background_color,
		const struct redraw_context *ctx)
{
	const struct plotter_table *plot = ctx->plot;
	int x, y;
	int width, height;
	int padding_left, padding_top, padding_width, padding_height;
	int border_left, border_top, border_right, border_bottom;
	struct rect r;
	struct rect rect;
	int x_scrolled, y_scrolled;
	struct box *bg_box = NULL;
	css_computed_clip_rect css_rect;
	enum css_overflow_e overflow_x = CSS_OVERFLOW_VISIBLE;
	enum css_overflow_e overflow_y = CSS_OVERFLOW_VISIBLE;
	dom_exception exc;

	macos9_hrb_visits++;
	switch (box->type) {
	case BOX_BLOCK: macos9_hrb_block++; break;
	case BOX_INLINE_CONTAINER: macos9_hrb_inlinec++; break;
	case BOX_INLINE: macos9_hrb_inline++; break;
	case BOX_TEXT: macos9_hrb_text++; break;
	default: macos9_hrb_other++; break;
	}
#ifdef __MACOS9__
	/* fixes620: publish the backdrop the Mac plotter composites
	 * semi-transparent (rgba) fills, borders and text against. On entry
	 * this is the background beneath this box -- the correct backdrop for
	 * this box's own translucent background fill. Refreshed to the box's
	 * resolved background before the border pass below. */
	{
		extern colour macos9_plot_backdrop;
		macos9_plot_backdrop = current_background_color;
	}
#endif

	if (html_redraw_printing && (box->flags & PRINTED))
		return true;

	if (box->style != NULL) {
		overflow_x = css_computed_overflow_x(box->style);
		overflow_y = css_computed_overflow_y(box->style);
	}

	/* Defensive sanity clamp: layout / CSS engine can leave box fields
	 * with garbage values when computed style is incompletely initialised.
	 * Those garbage values trick the clip test into skipping real content.
	 * Observed garbage: box->x = 30728, box->descendant_y0 = -39845888
	 * on macos9. The original clamps used ±10000 across the board, which
	 * worked when advanced.html was shorter — but once probe cards FF1–FF5
	 * (fixes154), C1–C5, R1–R5, V1–V4, A1–A4, ZS1–ZS6 etc. pushed total
	 * page height past 10000 px (current c_h = 10035), the root box's
	 * height + descendant_y1 got reset to 0 EVERY redraw, and the walker
	 * collapsed at the first child's clip intersection. fixes156:
	 * heights/y-coords clamped at ±200000 (catches the -39845888 garbage
	 * with 4 orders of magnitude headroom, allows up to 200000 px tall
	 * real content); x-coords / widths stay at ±10000 (no real page goes
	 * wider). */
	{
		if (box->x < -10000 || box->x > 10000) box->x = 0;
		if (box->y < -200000 || box->y > 200000) box->y = 0;
		if (box->width < 0 || box->width > 10000) box->width = 0;
		if (box->height < 0 || box->height > 200000) box->height = 0;
		if (box->padding[LEFT] < 0 || box->padding[LEFT] > 10000) box->padding[LEFT] = 0;
		if (box->padding[TOP] < 0 || box->padding[TOP] > 10000) box->padding[TOP] = 0;
		if (box->padding[RIGHT] < 0 || box->padding[RIGHT] > 10000) box->padding[RIGHT] = 0;
		if (box->padding[BOTTOM] < 0 || box->padding[BOTTOM] > 10000) box->padding[BOTTOM] = 0;
		if (box->border[LEFT].width < 0 || box->border[LEFT].width > 1000) box->border[LEFT].width = 0;
		if (box->border[TOP].width < 0 || box->border[TOP].width > 1000) box->border[TOP].width = 0;
		if (box->border[RIGHT].width < 0 || box->border[RIGHT].width > 1000) box->border[RIGHT].width = 0;
		if (box->border[BOTTOM].width < 0 || box->border[BOTTOM].width > 1000) box->border[BOTTOM].width = 0;
		if (box->descendant_x0 < -10000 || box->descendant_x0 > 10000) box->descendant_x0 = 0;
		if (box->descendant_y0 < -200000 || box->descendant_y0 > 200000) box->descendant_y0 = 0;
		if (box->descendant_x1 < -10000 || box->descendant_x1 > 10000) box->descendant_x1 = box->width;
		if (box->descendant_y1 < -200000 || box->descendant_y1 > 200000) box->descendant_y1 = box->height;
		/* Expand descendants if collapsed — happens when layout hasn't
		 * fully run but the box has real text children. Skip this for
		 * boxes that can carry an element scrollbar (overflow auto/scroll):
		 * the inflated descendant extent would trick box_[hv]scrollbar_present
		 * into manufacturing a spurious mid-page scrollbar (the ScrollV bar
		 * splitting XenForo pages). */
		if (box->descendant_x1 <= box->descendant_x0 &&
				overflow_x != CSS_OVERFLOW_AUTO &&
				overflow_x != CSS_OVERFLOW_SCROLL)
			box->descendant_x1 = box->descendant_x0 + 10000;
		if (box->descendant_y1 <= box->descendant_y0 &&
				overflow_y != CSS_OVERFLOW_AUTO &&
				overflow_y != CSS_OVERFLOW_SCROLL)
			box->descendant_y1 = box->descendant_y0 + 200000;
	}

	/* fixes191c + fixes201 — position: sticky clamp (V2).
	 *
	 * Sticky lays out exactly like position: relative, then at paint
	 * time we shift the box (and all descendants, by adjusting
	 * x_parent / y_parent BEFORE the x/y computation below) so the
	 * painted position respects the `top` / `bottom` / `left` / `right`
	 * offset that names a viewport edge to pin against.
	 *
	 * NetSurf calls browser_window_redraw with x_offset/y_offset =
	 * -scroll_x/-scroll_y; by the time we reach this function,
	 * (x_parent + box->x, y_parent + box->y) is already in VIEWPORT
	 * coordinates (0,0 = first painted pixel of the content area).
	 * top:32px wants y = max(normal_y, 32). bottom:8px wants
	 * (y + box->height) <= (viewport_h - 8). left/right are
	 * symmetric on x.
	 *
	 * V2 limits remaining:
	 *   - no containing-block-bottom clamp (sticky never "lets go" at
	 *     its parent's bottom edge — it pins to the viewport for the
	 *     full document height). For sidebars/headers in modern themes
	 *     the parent is usually tall enough that this is invisible.
	 *   - no nested-scroll-container support (sticky always pins to
	 *     the page viewport, not the closest scrolling ancestor).
	 *   - hit-testing uses the shifted painted position because the
	 *     downstream walker takes x_parent/y_parent into account.
	 */
	if (box->style != NULL &&
			css_computed_position(box->style) ==
				CSS_POSITION_STICKY) {
		css_fixed off_v = 0;
		css_unit off_u = CSS_UNIT_PX;
		uint8_t off_type;
		int viewport_w = 0, viewport_h = 0;
		int normal_x, normal_y;
		int box_w, box_h;

		/* Resolve viewport dimensions for bottom/right anchors.
		 * unit_len_ctx stores viewport_w/h as css_fixed (value <<
		 * 10); convert to integer pixels for comparison with the
		 * normal_x/y already in pixel coords. */
		viewport_w = FIXTOINT(html->unit_len_ctx.viewport_width);
		viewport_h = FIXTOINT(html->unit_len_ctx.viewport_height);

		normal_x = x_parent + box->x;
		normal_y = y_parent + box->y;
		box_w = box->width + box->padding[LEFT] + box->padding[RIGHT];
		box_h = box->height + box->padding[TOP] + box->padding[BOTTOM];

		/* top: */
		off_type = css_computed_top(box->style, &off_v, &off_u);
		if (off_type == CSS_TOP_SET) {
			int top_px;
			if (off_u == CSS_UNIT_PCT) {
				top_px = 0;
			} else {
				top_px = FIXTOINT(css_unit_len2device_px(
						box->style,
						&html->unit_len_ctx,
						off_v, off_u));
			}
			if (normal_y < top_px) {
				y_parent += (top_px - normal_y);
				normal_y = y_parent + box->y;
			}
		}

		/* bottom: pin so (normal_y + box_h) <= viewport_h - bottom_px.
		 * If the natural position is already inside that bound, no
		 * shift; if it has scrolled past, pull upward. */
		off_type = css_computed_bottom(box->style, &off_v, &off_u);
		if (off_type == CSS_BOTTOM_SET && viewport_h > 0) {
			int bot_px;
			int max_y;
			if (off_u == CSS_UNIT_PCT) {
				bot_px = 0;
			} else {
				bot_px = FIXTOINT(css_unit_len2device_px(
						box->style,
						&html->unit_len_ctx,
						off_v, off_u));
			}
			max_y = viewport_h - bot_px - box_h;
			if (normal_y > max_y) {
				y_parent -= (normal_y - max_y);
				normal_y = y_parent + box->y;
			}
		}

		/* left: */
		off_type = css_computed_left(box->style, &off_v, &off_u);
		if (off_type == CSS_LEFT_SET) {
			int left_px;
			if (off_u == CSS_UNIT_PCT) {
				left_px = 0;
			} else {
				left_px = FIXTOINT(css_unit_len2device_px(
						box->style,
						&html->unit_len_ctx,
						off_v, off_u));
			}
			if (normal_x < left_px) {
				x_parent += (left_px - normal_x);
				normal_x = x_parent + box->x;
			}
		}

		/* right: */
		off_type = css_computed_right(box->style, &off_v, &off_u);
		if (off_type == CSS_RIGHT_SET && viewport_w > 0) {
			int right_px;
			int max_x;
			if (off_u == CSS_UNIT_PCT) {
				right_px = 0;
			} else {
				right_px = FIXTOINT(css_unit_len2device_px(
						box->style,
						&html->unit_len_ctx,
						off_v, off_u));
			}
			max_x = viewport_w - right_px - box_w;
			if (normal_x > max_x) {
				x_parent -= (normal_x - max_x);
			}
		}
	}

	/* fixes610 — box-level percent translate (e.g. the off-canvas
	 * sidebar's transform:translateX(-100%)). A % translate resolves
	 * against the box's OWN padding-box size, known only here at paint
	 * time; shifting x_parent/y_parent moves the box AND its whole subtree
	 * (same mechanism as the sticky block above). Only the percent case
	 * (packed bit 31, emitted by the 0x0081 cascade) is handled here —
	 * pixel translate stays on the background/text transform path — so
	 * existing transforms never double-apply. */
	if (box->style != NULL) {
		int32_t tfm_packed = 0;
		uint8_t tfm_type = css_computed_macsurf_transform(box->style,
				&tfm_packed);
		if (tfm_type == CSS_MACSURF_TRANSFORM_SET &&
				(((uint32_t)tfm_packed >> 31) & 1)) {
			int tx_pct = (int)(int8_t)
					(((uint32_t)tfm_packed >> 8) & 0xff);
			int ty_pct = (int)(int8_t)
					(((uint32_t)tfm_packed) & 0xff);
			int box_w = box->width + box->padding[LEFT] +
					box->padding[RIGHT];
			int box_h = box->height + box->padding[TOP] +
					box->padding[BOTTOM];
			x_parent += (tx_pct * box_w) / 100;
			y_parent += (ty_pct * box_h) / 100;
		}
	}

	/* avoid trivial FP maths */
	if (scale == 1.0) {
		x = x_parent + box->x;
		y = y_parent + box->y;
		width = box->width;
		height = box->height;
		padding_left = box->padding[LEFT];
		padding_top = box->padding[TOP];
		padding_width = padding_left + box->width + box->padding[RIGHT];
		padding_height = padding_top + box->height +
				box->padding[BOTTOM];
		border_left = box->border[LEFT].width;
		border_top = box->border[TOP].width;
		border_right = box->border[RIGHT].width;
		border_bottom = box->border[BOTTOM].width;
	} else {
		x = (x_parent + box->x) * scale;
		y = (y_parent + box->y) * scale;
		width = box->width * scale;
		height = box->height * scale;
		/* left and top padding values are normally zero,
		 * so avoid trivial FP maths */
		padding_left = box->padding[LEFT] ? box->padding[LEFT] * scale
				: 0;
		padding_top = box->padding[TOP] ? box->padding[TOP] * scale
				: 0;
		padding_width = (box->padding[LEFT] + box->width +
				box->padding[RIGHT]) * scale;
		padding_height = (box->padding[TOP] + box->height +
				box->padding[BOTTOM]) * scale;
		border_left = box->border[LEFT].width * scale;
		border_top = box->border[TOP].width * scale;
		border_right = box->border[RIGHT].width * scale;
		border_bottom = box->border[BOTTOM].width * scale;
	}

	/* calculate rectangle covering this box and descendants */
	if (box->style && overflow_x != CSS_OVERFLOW_VISIBLE &&
			box->parent != NULL) {
		/* box contents clipped to box size */
		r.x0 = x - border_left;
		r.x1 = x + padding_width + border_right;
	} else {
		/* box contents can hang out of the box; use descendant box */
		if (scale == 1.0) {
			r.x0 = x + box->descendant_x0;
			r.x1 = x + box->descendant_x1 + 1;
		} else {
			r.x0 = x + box->descendant_x0 * scale;
			r.x1 = x + box->descendant_x1 * scale + 1;
		}
		if (!box->parent) {
			/* root element */
			int margin_left, margin_right;
			if (scale == 1.0) {
				margin_left = box->margin[LEFT];
				margin_right = box->margin[RIGHT];
			} else {
				margin_left = box->margin[LEFT] * scale;
				margin_right = box->margin[RIGHT] * scale;
			}
			r.x0 = x - border_left - margin_left < r.x0 ?
					x - border_left - margin_left : r.x0;
			r.x1 = x + padding_width + border_right +
					margin_right > r.x1 ?
					x + padding_width + border_right +
					margin_right : r.x1;
		}
	}

	/* calculate rectangle covering this box and descendants */
	if (box->style && overflow_y != CSS_OVERFLOW_VISIBLE &&
			box->parent != NULL) {
		/* box contents clipped to box size */
		r.y0 = y - border_top;
		r.y1 = y + padding_height + border_bottom;
	} else {
		/* box contents can hang out of the box; use descendant box */
		if (scale == 1.0) {
			r.y0 = y + box->descendant_y0;
			r.y1 = y + box->descendant_y1 + 1;
		} else {
			r.y0 = y + box->descendant_y0 * scale;
			r.y1 = y + box->descendant_y1 * scale + 1;
		}
		if (!box->parent) {
			/* root element */
			int margin_top, margin_bottom;
			if (scale == 1.0) {
				margin_top = box->margin[TOP];
				margin_bottom = box->margin[BOTTOM];
			} else {
				margin_top = box->margin[TOP] * scale;
				margin_bottom = box->margin[BOTTOM] * scale;
			}
			r.y0 = y - border_top - margin_top < r.y0 ?
					y - border_top - margin_top : r.y0;
			r.y1 = y + padding_height + border_bottom +
					margin_bottom > r.y1 ?
					y + padding_height + border_bottom +
					margin_bottom : r.y1;
		}
	}

	/* return if the rectangle is completely outside the clip rectangle */
	if (clip->y1 < r.y0 || r.y1 < clip->y0 ||
			clip->x1 < r.x0 || r.x1 < clip->x0) {
		macos9_hrb_clip_skips++;
		/* Do NOT return early — recurse children even when this box is
		 * outside the clip rect.  With partially-initialised layout
		 * coordinates some content lands at y=0 and would be wrongly
		 * skipped otherwise.  Matches pre-regression fixes169 behaviour. */
		/* return true; */
	}

	/*if the rectangle is under the page bottom but it can fit in a page,
	don't print it now*/
	if (html_redraw_printing) {
		if (r.y1 > html_redraw_printing_border) {
			if (r.y1 - r.y0 <= html_redraw_printing_border &&
					(box->type == BOX_TEXT ||
					box->type == BOX_TABLE_CELL
					|| box->object || box->gadget)) {
				/*remember the highest of all points from the
				not printed elements*/
				if (r.y0 < html_redraw_printing_top_cropped)
					html_redraw_printing_top_cropped = r.y0;
				return true;
			}
		}
		else box->flags |= PRINTED; /*it won't be printed anymore*/
	}

	/* if visibility is hidden render children only */
	if (box->style && css_computed_visibility(box->style) ==
			CSS_VISIBILITY_HIDDEN) {
		if ((ctx->plot->group_start) &&
		    (ctx->plot->group_start(ctx, "hidden box") != NSERROR_OK))
			return false;
		if (!html_redraw_box_children(html, box, x_parent, y_parent,
				&r, scale, current_background_color, ctx))
			return false;
		return ((!ctx->plot->group_end) || (ctx->plot->group_end(ctx) == NSERROR_OK));
	}

	if ((ctx->plot->group_start) &&
	    (ctx->plot->group_start(ctx,"vis box") != NSERROR_OK)) {
		return false;
	}

	if (box->style != NULL &&
			css_computed_position(box->style) ==
					CSS_POSITION_ABSOLUTE &&
			css_computed_clip(box->style, &css_rect) ==
					CSS_CLIP_RECT) {
		/* We have an absolutly positioned box with a clip rect */
		if (css_rect.left_auto == false)
			r.x0 = x - border_left + FIXTOINT(css_unit_len2device_px(
					box->style, &html->unit_len_ctx,
					css_rect.left, css_rect.lunit));

		if (css_rect.top_auto == false)
			r.y0 = y - border_top + FIXTOINT(css_unit_len2device_px(
					box->style, &html->unit_len_ctx,
					css_rect.top, css_rect.tunit));

		if (css_rect.right_auto == false)
			r.x1 = x - border_left + FIXTOINT(css_unit_len2device_px(
					box->style, &html->unit_len_ctx,
					css_rect.right, css_rect.runit));

		if (css_rect.bottom_auto == false)
			r.y1 = y - border_top + FIXTOINT(css_unit_len2device_px(
					box->style, &html->unit_len_ctx,
					css_rect.bottom, css_rect.bunit));

		/* find intersection of clip rectangle and box */
		if (r.x0 < clip->x0) r.x0 = clip->x0;
		if (r.y0 < clip->y0) r.y0 = clip->y0;
		if (clip->x1 < r.x1) r.x1 = clip->x1;
		if (clip->y1 < r.y1) r.y1 = clip->y1;
		/* Nothing to do for invalid rectangles */
		if (r.x0 >= r.x1 || r.y0 >= r.y1)
			/* not an error */
			return ((!ctx->plot->group_end) ||
				(ctx->plot->group_end(ctx) == NSERROR_OK));
		/* clip to it */
		if (ctx->plot->clip(ctx, &r) != NSERROR_OK)
			return false;

	} else if (box->type == BOX_BLOCK || box->type == BOX_INLINE_BLOCK ||
			box->type == BOX_TABLE_CELL || box->object) {
		/* find intersection of clip rectangle and box */
		if (r.x0 < clip->x0) r.x0 = clip->x0;
		if (r.y0 < clip->y0) r.y0 = clip->y0;
		if (clip->x1 < r.x1) r.x1 = clip->x1;
		if (clip->y1 < r.y1) r.y1 = clip->y1;
		/* fixes158 -- no point trying to draw 0-width/height OR inverted
		 * boxes. Previous check was r.x0 == r.x1 || r.y0 == r.y1 which
		 * only caught zero-sized intersections. When the box sits entirely
		 * above the clip (descendants all ABOVE clip.y0), the intersection
		 * produces r.y0 = clip.y0 and r.y1 = box_bottom < clip.y0, i.e.
		 * an inverted rect. The == check missed this, the walker set an
		 * empty clipRgn via plot_clip, recursed into children with the
		 * inverted rect as their clip, and each child's intersection
		 * cascaded into more inverted clips. Eventually the walker's
		 * ctx->plot->clip failed somewhere and propagated false back up,
		 * aborting body's children loop after 3 visits. Signature in log:
		 * visits=3 block=3 plot_text=0 at any offset below the first
		 * viewport. The >= check catches both zero-sized and inverted. */
		if (r.x0 >= r.x1 || r.y0 >= r.y1)
			/* not an error */
			return ((!ctx->plot->group_end) ||
				(ctx->plot->group_end(ctx) == NSERROR_OK));
		/* clip to it */
		if (ctx->plot->clip(ctx, &r) != NSERROR_OK)
			return false;
	} else {
		/* clip box is fine, clip to it */
		r = *clip;
		if (ctx->plot->clip(ctx, &r) != NSERROR_OK)
			return false;
	}

	/* background colour and image for block level content and replaced
	 * inlines */

	bg_box = html_redraw_find_bg_box(box);

	/* fixes139a: CSS 2.1 empty-cells: hide -- a table cell with no
	 * visible content should not paint its background or border. Both
	 * paint blocks below short-circuit when this flag is true. Layout
	 * is unaffected; the cell still occupies its allocated space. */
	{
		bool empty_cell_hide = false;
		if (box->type == BOX_TABLE_CELL && box->style != NULL &&
				css_computed_empty_cells(box->style) ==
				CSS_EMPTY_CELLS_HIDE &&
				html_box_table_cell_is_empty(box)) {
			empty_cell_hide = true;
		}

	/* bg_box == NULL implies that this box should not have
	* its background rendered. Otherwise filter out linebreaks,
	* optimize away non-differing inlines, only plot background
	* for BOX_TEXT it's in an inline */
	if (!empty_cell_hide && bg_box && bg_box->type != BOX_BR &&
			bg_box->type != BOX_TEXT &&
			bg_box->type != BOX_INLINE_END &&
			(bg_box->type != BOX_INLINE || bg_box->object ||
			bg_box->flags & IFRAME || box->flags & REPLACE_DIM ||
			(bg_box->gadget != NULL &&
			(bg_box->gadget->type == GADGET_TEXTAREA ||
			bg_box->gadget->type == GADGET_TEXTBOX ||
			bg_box->gadget->type == GADGET_PASSWORD)))) {
		/* find intersection of clip box and border edge */
		struct rect p;
		p.x0 = x - border_left < r.x0 ? r.x0 : x - border_left;
		p.y0 = y - border_top < r.y0 ? r.y0 : y - border_top;
		p.x1 = x + padding_width + border_right < r.x1 ?
				x + padding_width + border_right : r.x1;
		p.y1 = y + padding_height + border_bottom < r.y1 ?
				y + padding_height + border_bottom : r.y1;
		if (!box->parent) {
			/* Root element, special case:
			 * background covers margins too */
			int m_left, m_top, m_right, m_bottom;
			if (scale == 1.0) {
				m_left = box->margin[LEFT];
				m_top = box->margin[TOP];
				m_right = box->margin[RIGHT];
				m_bottom = box->margin[BOTTOM];
			} else {
				m_left = box->margin[LEFT] * scale;
				m_top = box->margin[TOP] * scale;
				m_right = box->margin[RIGHT] * scale;
				m_bottom = box->margin[BOTTOM] * scale;
			}
			p.x0 = p.x0 - m_left < r.x0 ? r.x0 : p.x0 - m_left;
			p.y0 = p.y0 - m_top < r.y0 ? r.y0 : p.y0 - m_top;
			p.x1 = p.x1 + m_right < r.x1 ? p.x1 + m_right : r.x1;
			p.y1 = p.y1 + m_bottom < r.y1 ? p.y1 + m_bottom : r.y1;
		}
		/* valid clipping rectangles only */
		if ((p.x0 < p.x1) && (p.y0 < p.y1)) {
			/* plot background */
			if (!html_redraw_background(x, y, box, scale, &p,
					&current_background_color, bg_box,
					&html->unit_len_ctx, ctx))
				return false;
			/* restore previous graphics window */
			if (ctx->plot->clip(ctx, &r) != NSERROR_OK)
				return false;
		}
	}

	/* borders for block level content and replaced inlines */
	if (!empty_cell_hide && box->style &&
	    box->type != BOX_TEXT &&
	    box->type != BOX_INLINE_END &&
	    (box->type != BOX_INLINE || box->object ||
	     box->flags & IFRAME || box->flags & REPLACE_DIM ||
	     (box->gadget != NULL &&
	      (box->gadget->type == GADGET_TEXTAREA ||
	       box->gadget->type == GADGET_TEXTBOX ||
	       box->gadget->type == GADGET_PASSWORD))) &&
	    (border_top || border_right || border_bottom || border_left)) {
#ifdef __MACOS9__
		/* fixes620: html_redraw_background (above) updated
		 * current_background_color to this box's own resolved
		 * background, so refresh the plotter backdrop before borders
		 * -- rgba hairlines blend over the box's background, not the
		 * page. */
		{
			extern colour macos9_plot_backdrop;
			macos9_plot_backdrop = current_background_color;
		}
#endif
		if (!html_redraw_borders(box, x_parent, y_parent,
				padding_width, padding_height, &r,
				scale, ctx))
			return false;
	}
	}  /* end empty_cell_hide scope (fixes139a) */

	/* backgrounds and borders for non-replaced inlines */
	if (box->style && box->type == BOX_INLINE && box->inline_end &&
			(html_redraw_box_has_background(box) ||
			border_top || border_right ||
			border_bottom || border_left)) {
		/* inline backgrounds and borders span other boxes and may
		 * wrap onto separate lines */
		struct box *ib;
		struct rect b; /* border edge rectangle */
		struct rect p; /* clipped rect */
		bool first = true;
		int ib_x;
		int ib_y = y;
		int ib_p_width;
		int ib_b_left, ib_b_right;

		b.x0 = x - border_left;
		b.x1 = x + padding_width + border_right;
		b.y0 = y - border_top;
		b.y1 = y + padding_height + border_bottom;

		p.x0 = b.x0 < r.x0 ? r.x0 : b.x0;
		p.x1 = b.x1 < r.x1 ? b.x1 : r.x1;
		p.y0 = b.y0 < r.y0 ? r.y0 : b.y0;
		p.y1 = b.y1 < r.y1 ? b.y1 : r.y1;
		for (ib = box; ib; ib = ib->next) {
			/* to get extents of rectangle(s) associated with
			 * inline, cycle though all boxes in inline, skipping
			 * over floats */
			if (ib->type == BOX_FLOAT_LEFT ||
					ib->type == BOX_FLOAT_RIGHT)
				continue;
			if (scale == 1.0) {
				ib_x = x_parent + ib->x;
				ib_y = y_parent + ib->y;
				ib_p_width = ib->padding[LEFT] + ib->width +
						ib->padding[RIGHT];
				ib_b_left = ib->border[LEFT].width;
				ib_b_right = ib->border[RIGHT].width;
			} else {
				ib_x = (x_parent + ib->x) * scale;
				ib_y = (y_parent + ib->y) * scale;
				ib_p_width = (ib->padding[LEFT] + ib->width +
						ib->padding[RIGHT]) * scale;
				ib_b_left = ib->border[LEFT].width * scale;
				ib_b_right = ib->border[RIGHT].width * scale;
			}

			if ((ib->flags & NEW_LINE) && ib != box) {
				/* inline element has wrapped, plot background
				 * and borders */
				if (!html_redraw_inline_background(
						x, y, box, scale, &p, b,
						first, false,
						&current_background_color,
						&html->unit_len_ctx, ctx))
					return false;
				/* restore previous graphics window */
				if (ctx->plot->clip(ctx, &r) != NSERROR_OK)
					return false;
				if (!html_redraw_inline_borders(box, b, &r,
						scale, first, false, ctx))
					return false;
				/* reset coords */
				b.x0 = ib_x - ib_b_left;
				b.y0 = ib_y - border_top - padding_top;
				b.y1 = ib_y + padding_height - padding_top +
						border_bottom;

				p.x0 = b.x0 < r.x0 ? r.x0 : b.x0;
				p.y0 = b.y0 < r.y0 ? r.y0 : b.y0;
				p.y1 = b.y1 < r.y1 ? b.y1 : r.y1;

				first = false;
			}

			/* increase width for current box */
			b.x1 = ib_x + ib_p_width + ib_b_right;
			p.x1 = b.x1 < r.x1 ? b.x1 : r.x1;

			if (ib == box->inline_end)
				/* reached end of BOX_INLINE span */
				break;
		}
		/* plot background and borders for last rectangle of
		 * the inline */
		if (!html_redraw_inline_background(x, ib_y, box, scale, &p, b,
				first, true, &current_background_color,
				&html->unit_len_ctx, ctx))
			return false;
		/* restore previous graphics window */
		if (ctx->plot->clip(ctx, &r) != NSERROR_OK)
			return false;
		if (!html_redraw_inline_borders(box, b, &r, scale, first, true,
				ctx))
			return false;

	}

	/* MacSurf: native CSS outline rendering. libcss exposes
	 * outline-color/outline-style/outline-width; NetSurf core never
	 * wired them. We FrameRect just outside the border box. The
	 * stroke_width path goes through QuickDraw PenSize (fixes170)
	 * so widths > 1px render correctly. fixes40. */
	if (box->style != NULL) {
		uint8_t ostyle_v = css_computed_outline_style(box->style);
		if (ostyle_v != CSS_OUTLINE_STYLE_NONE &&
		    ostyle_v != CSS_OUTLINE_STYLE_INHERIT) {
			css_color ocol;
			css_fixed olen = 0;
			css_unit ounit = CSS_UNIT_PX;
			int owidth_px;
			plot_style_t plot_style_outline = {
				PLOT_OP_TYPE_SOLID, 0, 0,
				PLOT_OP_TYPE_NONE, 0,
				0, 0
			};
			struct rect orect;
			uint8_t ocol_status;
			/* fixes365a: honour outline-style at paint time.
			 * libcss parses + cascades the style into ostyle_v
			 * but the paint path used to hard-code SOLID. Map
			 * SOLID/DOTTED/DASHED to the matching PLOT_OP_TYPE_*
			 * enums; complex 3D styles (DOUBLE/GROOVE/RIDGE/
			 * INSET/OUTSET) fall back to SOLID. NB: the macos9
			 * plotter currently rasterises DOT/DASH the same as
			 * SOLID (no QuickDraw pen pattern wired yet), so on
			 * Mac OS 9 this is a forward-looking emission; other
			 * frontends honour it today. */
			switch (ostyle_v) {
			case CSS_OUTLINE_STYLE_DOTTED:
				plot_style_outline.stroke_type =
						PLOT_OP_TYPE_DOT;
				break;
			case CSS_OUTLINE_STYLE_DASHED:
				plot_style_outline.stroke_type =
						PLOT_OP_TYPE_DASH;
				break;
			case CSS_OUTLINE_STYLE_SOLID:
			case CSS_OUTLINE_STYLE_DOUBLE:
			case CSS_OUTLINE_STYLE_GROOVE:
			case CSS_OUTLINE_STYLE_RIDGE:
			case CSS_OUTLINE_STYLE_INSET:
			case CSS_OUTLINE_STYLE_OUTSET:
			default:
				plot_style_outline.stroke_type =
						PLOT_OP_TYPE_SOLID;
				break;
			}
			ocol_status = css_computed_outline_color(box->style, &ocol);
			if (ocol_status == CSS_OUTLINE_COLOR_INVERT ||
			    ocol_status == CSS_OUTLINE_COLOR_CURRENT_COLOR) {
				css_computed_color(box->style, &ocol);
			}
			css_computed_outline_width(box->style, &olen, &ounit);
			owidth_px = (int)FIXTOINT(css_unit_len2device_px(
					box->style, &html->unit_len_ctx,
					olen, ounit));
			if (owidth_px < 1) owidth_px = 1;
			if (owidth_px > 16) owidth_px = 16;
			plot_style_outline.stroke_width =
					(owidth_px << PLOT_STYLE_RADIX);
			plot_style_outline.stroke_colour = nscss_color_to_ns(ocol);
			orect.x0 = (x - border_left - owidth_px) * scale;
			orect.y0 = (y - border_top - owidth_px) * scale;
			orect.x1 = (x + padding_width + border_right + owidth_px)
					* scale;
			orect.y1 = (y + padding_height + border_bottom +
					owidth_px) * scale;
			if (ctx->plot->rectangle(ctx, &plot_style_outline,
					&orect) != NSERROR_OK)
				return false;
		}
	}

	if (!html_redraw_multicol_rules(html, box, x, y, scale, &r, ctx))
		return false;

	/* Debug outlines */
	if (html_redraw_debug) {
		int margin_left, margin_right;
		int margin_top, margin_bottom;
		if (scale == 1.0) {
			/* avoid trivial fp maths */
			margin_left = box->margin[LEFT];
			margin_top = box->margin[TOP];
			margin_right = box->margin[RIGHT];
			margin_bottom = box->margin[BOTTOM];
		} else {
			margin_left = box->margin[LEFT] * scale;
			margin_top = box->margin[TOP] * scale;
			margin_right = box->margin[RIGHT] * scale;
			margin_bottom = box->margin[BOTTOM] * scale;
		}
		/* Content edge -- blue */
		rect.x0 = x + padding_left;
		rect.y0 = y + padding_top;
		rect.x1 = x + padding_left + width;
		rect.y1 = y + padding_top + height;
		if (ctx->plot->rectangle(ctx, plot_style_content_edge, &rect) != NSERROR_OK)
			return false;

		/* Padding edge -- red */
		rect.x0 = x;
		rect.y0 = y;
		rect.x1 = x + padding_width;
		rect.y1 = y + padding_height;
		if (ctx->plot->rectangle(ctx, plot_style_padding_edge, &rect) != NSERROR_OK)
			return false;

		/* Margin edge -- yellow */
		rect.x0 = x - border_left - margin_left;
		rect.y0 = y - border_top - margin_top;
		rect.x1 = x + padding_width + border_right + margin_right;
		rect.y1 = y + padding_height + border_bottom + margin_bottom;
		if (ctx->plot->rectangle(ctx, plot_style_margin_edge, &rect) != NSERROR_OK)
			return false;
	}

	/* clip to the padding edge for objects, or boxes with overflow hidden
	 * or scroll, unless it's the root element */
	if (box->parent != NULL) {
		bool need_clip = false;
		if (box->object || box->flags & IFRAME ||
				(overflow_x != CSS_OVERFLOW_VISIBLE &&
				 overflow_y != CSS_OVERFLOW_VISIBLE)) {
			r.x0 = x;
			r.y0 = y;
			r.x1 = x + padding_width;
			r.y1 = y + padding_height;
			if (r.x0 < clip->x0) r.x0 = clip->x0;
			if (r.y0 < clip->y0) r.y0 = clip->y0;
			if (clip->x1 < r.x1) r.x1 = clip->x1;
			if (clip->y1 < r.y1) r.y1 = clip->y1;
			if (r.x1 <= r.x0 || r.y1 <= r.y0) {
				return (!ctx->plot->group_end ||
					(ctx->plot->group_end(ctx) == NSERROR_OK));
			}
			need_clip = true;

		} else if (overflow_x != CSS_OVERFLOW_VISIBLE) {
			r.x0 = x;
			r.y0 = clip->y0;
			r.x1 = x + padding_width;
			r.y1 = clip->y1;
			if (r.x0 < clip->x0) r.x0 = clip->x0;
			if (clip->x1 < r.x1) r.x1 = clip->x1;
			if (r.x1 <= r.x0) {
				return (!ctx->plot->group_end ||
					(ctx->plot->group_end(ctx) == NSERROR_OK));
			}
			need_clip = true;

		} else if (overflow_y != CSS_OVERFLOW_VISIBLE) {
			r.x0 = clip->x0;
			r.y0 = y;
			r.x1 = clip->x1;
			r.y1 = y + padding_height;
			if (r.y0 < clip->y0) r.y0 = clip->y0;
			if (clip->y1 < r.y1) r.y1 = clip->y1;
			if (r.y1 <= r.y0) {
				return (!ctx->plot->group_end ||
					(ctx->plot->group_end(ctx) == NSERROR_OK));
			}
			need_clip = true;
		}

		if (need_clip &&
		    (box->type == BOX_BLOCK ||
		     box->type == BOX_INLINE_BLOCK ||
		     box->type == BOX_TABLE_CELL ||
		     box->type == BOX_FLEX ||
		     box->type == BOX_INLINE_FLEX ||
		     box->type == BOX_GRID ||
		     box->object)) {
			if (ctx->plot->clip(ctx, &r) != NSERROR_OK)
				return false;
		}
	}

	/* text decoration */
	if ((box->type != BOX_TEXT) &&
	    box->style &&
	    css_computed_text_decoration(box->style) !=	CSS_TEXT_DECORATION_NONE) {
		if (!html_redraw_text_decoration(box, x_parent, y_parent,
				scale, current_background_color, ctx))
			return false;
	}

	/* fixes650 (Track C2): the per-box tag-type lookup that used to run here
	 * on every frame is now deferred into the canvas branch below (behind the
	 * cheap REPLACE_DIM gate) via macsurf_box_node_is_canvas(). */

	if (box->object && width != 0 && height != 0) {
		struct content_redraw_data obj_data;

		x_scrolled = x - scrollbar_get_offset(box->scroll_x) * scale;
		y_scrolled = y - scrollbar_get_offset(box->scroll_y) * scale;

		obj_data.x = x_scrolled + padding_left;
		obj_data.y = y_scrolled + padding_top;
		obj_data.width = width;
		obj_data.height = height;
		obj_data.background_colour = current_background_color;
		obj_data.scale = scale;
		obj_data.repeat_x = false;
		obj_data.repeat_y = false;

		if (content_get_type(box->object) == CONTENT_HTML) {
			obj_data.x /= scale;
			obj_data.y /= scale;
		}

		/* fixes116: apply CSS object-fit. The cell clip is already set
		 * (line ~2393 above), so a `cover` rect that overflows the
		 * cell is hardware-clipped by QuickDraw on draw. */
		html_redraw_apply_object_fit(box, &obj_data);

		if (!content_redraw(box->object, &obj_data, &r, ctx)) {
			/* fixes291 (#101): prefer the <img> `alt` text as the
			 * fallback when image content fails to decode/fetch.
			 * box_special.c box_image already captures the alt
			 * attribute into box->text at parse; when present,
			 * render it inside the placeholder rect rather than
			 * the generic OBJECT REPLACEMENT CHARACTER glyph.
			 *
			 * V1 paint is small italic grey (plot_fstyle_broken_object)
			 * centered inside the box's reserved replaced-element
			 * dimensions, with a left-edge clamp so long alt text
			 * doesn't render at a negative x offset.  Real V2
			 * relaxes the IS_REPLACED layout when content is
			 * permanently failed and lets alt flow as inline text. */
			const char *fallback;
			size_t fallback_len;
			int obj_width;
			int obj_x = x + padding_left;
			nserror res;

			rect.x0 = x + padding_left;
			rect.y0 = y + padding_top;
			rect.x1 = x + padding_left + width - 1;
			rect.y1 = y + padding_top + height - 1;
			res = ctx->plot->rectangle(ctx, plot_style_broken_object, &rect);
			if (res != NSERROR_OK) {
				return false;
			}

			if (box->text != NULL && box->length > 0) {
				fallback = box->text;
				fallback_len = box->length;
			} else {
				/* Unicode (U+FFFC) 'OBJECT REPLACEMENT CHARACTER' */
				fallback = "\xef\xbf\xbc";
				fallback_len = 3;
			}

			res = guit->layout->width(plot_fstyle_broken_object,
						  fallback,
						  fallback_len,
						  &obj_width);
			if (res != NSERROR_OK) {
				obj_x += 1;
			} else {
				obj_x += width / 2 - obj_width / 2;
				if (obj_x < x + padding_left)
					obj_x = x + padding_left + 2;
			}

			if (ctx->plot->text(ctx,
					    plot_fstyle_broken_object,
					    obj_x, y + padding_top +
						    font_plot_style_baseline(
							    plot_fstyle_broken_object,
							    height),
					    fallback, fallback_len) != NSERROR_OK)
				return false;
		}
	} else if ((box->flags & REPLACE_DIM) && box->node != NULL &&
		   macsurf_box_node_is_canvas(box)) {
		/* Canvas to draw */
		struct bitmap *bitmap = NULL;
		exc = dom_node_get_user_data(box->node,
					     corestring_dom___ns_key_canvas_node_data,
					     &bitmap);
		if (exc != DOM_NO_ERR) {
			bitmap = NULL;
		}
		if (bitmap != NULL &&
		    ctx->plot->bitmap(ctx, bitmap, x + padding_left, y + padding_top,
				      width, height, current_background_color,
				      BITMAPF_NONE) != NSERROR_OK)
			return false;
	} else if (box->iframe) {
		/* Offset is passed to browser window redraw unscaled */
		browser_window_redraw(box->iframe,
				x + padding_left,
				y + padding_top, &r, ctx);

	} else if (box->gadget && box->gadget->type == GADGET_CHECKBOX) {
		if (!html_redraw_checkbox(x + padding_left, y + padding_top,
				width, height, box->gadget->selected, ctx))
			return false;

	} else if (box->gadget && box->gadget->type == GADGET_RADIO) {
		if (!html_redraw_radio(x + padding_left, y + padding_top,
				width, height, box->gadget->selected, ctx))
			return false;

	} else if (box->gadget && box->gadget->type == GADGET_FILE) {
		if (!html_redraw_file(x + padding_left, y + padding_top,
				width, height, box, scale,
				current_background_color, &html->unit_len_ctx, ctx))
			return false;

	} else if (box->gadget &&
			(box->gadget->type == GADGET_TEXTAREA ||
			box->gadget->type == GADGET_PASSWORD ||
			box->gadget->type == GADGET_TEXTBOX)) {
		textarea_redraw(box->gadget->data.text.ta, x, y,
				current_background_color, scale, &r, ctx);

	} else if (box->text) {
		if (!html_redraw_text_box(html, box, x, y, &r, scale,
				current_background_color, ctx))
			return false;

	} else if (box->flags & SVG_INLINE) {
		/* fixes195 — inline <svg> root. The SVG DOM children are
		 * not in the box tree (skipped at box_construct time);
		 * the macos9 SVG painter walks the DOM directly and
		 * issues plotter calls for each shape. The padded
		 * content rect (x + padding_left, y + padding_top,
		 * width x height) is the viewBox target. */
#ifdef __MACOS9__
		macsurf_debug_log_writef(
			"svg_paint: box=%p x=%d y=%d w=%d h=%d node=%p",
			(void *)box, (int)(x + padding_left),
			(int)(y + padding_top),
			(int)width, (int)height,
			(void *)(box ? box->node : NULL));
		(void)macos9_svg_paint_inline(box,
				x + padding_left,
				y + padding_top,
				width,
				height,
				ctx,
				html->base_url);
#endif

	} else {
		if (!html_redraw_box_children(html, box, x_parent, y_parent, &r,
				scale, current_background_color, ctx))
			return false;
	}

	if (box->type == BOX_BLOCK || box->type == BOX_INLINE_BLOCK ||
			box->type == BOX_TABLE_CELL || box->type == BOX_INLINE ||
			box->type == BOX_FLEX || box->type == BOX_INLINE_FLEX ||
			box->type == BOX_GRID)
		if (ctx->plot->clip(ctx, clip) != NSERROR_OK)
			return false;

	/* list marker */
	if (box->list_marker) {
		if (!html_redraw_box(html, box->list_marker,
				x_parent + box->x -
				scrollbar_get_offset(box->scroll_x),
				y_parent + box->y -
				scrollbar_get_offset(box->scroll_y),
				clip, scale, current_background_color, ctx))
			return false;
	}

	/* scrollbars */
	if (((box->style && box->type != BOX_BR &&
	      box->type != BOX_TABLE && box->type != BOX_INLINE &&
	      (box->gadget == NULL || box->gadget->type != GADGET_TEXTAREA) &&
	      (overflow_x == CSS_OVERFLOW_SCROLL ||
	       overflow_x == CSS_OVERFLOW_AUTO ||
	       overflow_y == CSS_OVERFLOW_SCROLL ||
	       overflow_y == CSS_OVERFLOW_AUTO)) ||
	     (box->object && content_get_type(box->object) ==
	      CONTENT_HTML)) && box->parent != NULL) {
		nserror res;
		bool has_x_scroll = (overflow_x == CSS_OVERFLOW_SCROLL);
		bool has_y_scroll = (overflow_y == CSS_OVERFLOW_SCROLL);

		has_x_scroll |= (overflow_x == CSS_OVERFLOW_AUTO) &&
				box_hscrollbar_present(box);
		has_y_scroll |= (overflow_y == CSS_OVERFLOW_AUTO) &&
				box_vscrollbar_present(box);

		res = box_handle_scrollbars((struct content *)html,
					    box, has_x_scroll, has_y_scroll);
		if (res != NSERROR_OK) {
			NSLOG(netsurf, INFO, "%s", messages_get_errorcode(res));
			return false;
		}

		if (box->scroll_x != NULL)
			scrollbar_redraw(box->scroll_x,
					x_parent + box->x,
					y_parent + box->y + box->padding[TOP] +
					box->height + box->padding[BOTTOM] -
					SCROLLBAR_WIDTH, clip, scale, ctx);
		if (box->scroll_y != NULL)
			scrollbar_redraw(box->scroll_y,
					x_parent + box->x + box->padding[LEFT] +
					box->width + box->padding[RIGHT] -
					SCROLLBAR_WIDTH,
					y_parent + box->y, clip, scale, ctx);
	}

	if (box->type == BOX_BLOCK || box->type == BOX_INLINE_BLOCK ||
	    box->type == BOX_TABLE_CELL || box->type == BOX_INLINE) {
		if (ctx->plot->clip(ctx, clip) != NSERROR_OK)
			return false;
	}

	return ((!plot->group_end) || (ctx->plot->group_end(ctx) == NSERROR_OK));
}

/**
 * Draw a CONTENT_HTML using the current set of plotters (plot).
 *
 * \param  c	 content of type CONTENT_HTML
 * \param  data	 redraw data for this content redraw
 * \param  clip	 current clip region
 * \param  ctx	 current redraw context
 * \return true if successful, false otherwise
 *
 * x, y, clip_[xy][01] are in target coordinates.
 */

bool html_redraw(struct content *c, struct content_redraw_data *data,
		const struct rect *clip, const struct redraw_context *ctx)
{
	html_content *html = (html_content *) c;
	struct box *box;
	bool result = true;
	bool select, select_only;
	/* MacSurf: positional init (CW8 C89). plot_style_t order:
	 * stroke_type, stroke_width, stroke_colour, fill_type, fill_colour. */
	plot_style_t pstyle_fill_bg = {
		PLOT_OP_TYPE_NONE, 0, 0,
		PLOT_OP_TYPE_SOLID, 0
	};
	pstyle_fill_bg.fill_colour = data->background_colour;

	box = html->layout;
	assert(box);

	/* The select menu needs special treating because, when opened, it
	 * reaches beyond its layout box.
	 */
	select = false;
	select_only = false;
	if (ctx->interactive && html->visible_select_menu != NULL) {
		struct form_control *control = html->visible_select_menu;
		select = true;
		/* check if the redraw rectangle is completely inside of the
		   select menu */
		select_only = form_clip_inside_select_menu(control,
				data->scale, clip);
	}

	if (!select_only) {
		/* clear to background colour */
		result = (ctx->plot->clip(ctx, clip) == NSERROR_OK);

		if (html->background_colour != NS_TRANSPARENT)
			pstyle_fill_bg.fill_colour = html->background_colour;

		result &= (ctx->plot->rectangle(ctx, &pstyle_fill_bg, clip) == NSERROR_OK);

		result &= html_redraw_box(html, box, data->x, data->y, clip,
				data->scale, pstyle_fill_bg.fill_colour, ctx);
	}

	if (select) {
		int menu_x, menu_y;
		box = html->visible_select_menu->box;
		box_coords(box, &menu_x, &menu_y);

		menu_x -= box->border[LEFT].width;
		menu_y += box->height + box->border[BOTTOM].width +
				box->padding[BOTTOM] + box->padding[TOP];
		result &= form_redraw_select_menu(html->visible_select_menu,
				data->x + menu_x, data->y + menu_y,
				data->scale, clip, ctx);
	}

	return result;

}
