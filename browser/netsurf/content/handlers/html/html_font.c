/*
 * Copyright 2009 John-Mark Bell <jmb@netsurf-browser.org>
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
 * HTML internal font handling implementation.
 */

#include "utils/nsoption.h"
#include "netsurf/plot_style.h"
#include "css/utils.h"

#include "html/font.h"

/**
 * Map a generic CSS font family to a generic plot font family
 *
 * \param css Generic CSS font family
 * \return Plot font family
 */
static plot_font_generic_family_t
plot_font_generic_family(enum css_font_family_e css)
{
	plot_font_generic_family_t plot;

	switch (css) {
	case CSS_FONT_FAMILY_SERIF:
		plot = PLOT_FONT_FAMILY_SERIF;
		break;
	case CSS_FONT_FAMILY_MONOSPACE:
		plot = PLOT_FONT_FAMILY_MONOSPACE;
		break;
	case CSS_FONT_FAMILY_CURSIVE:
		plot = PLOT_FONT_FAMILY_CURSIVE;
		break;
	case CSS_FONT_FAMILY_FANTASY:
		plot = PLOT_FONT_FAMILY_FANTASY;
		break;
	case CSS_FONT_FAMILY_SANS_SERIF:
	default:
		plot = PLOT_FONT_FAMILY_SANS_SERIF;
		break;
	}

	return plot;
}

/**
 * Map a CSS font weight to a plot weight value
 *
 * \param css  CSS font weight
 * \return Plot weight
 */
static int plot_font_weight(enum css_font_weight_e css)
{
	int weight;

	switch (css) {
	case CSS_FONT_WEIGHT_100:
		weight = 100;
		break;
	case CSS_FONT_WEIGHT_200:
		weight = 200;
		break;
	case CSS_FONT_WEIGHT_300:
		weight = 300;
		break;
	case CSS_FONT_WEIGHT_400:
	case CSS_FONT_WEIGHT_NORMAL:
	default:
		weight = 400;
		break;
	case CSS_FONT_WEIGHT_500:
		weight = 500;
		break;
	case CSS_FONT_WEIGHT_600:
		weight = 600;
		break;
	case CSS_FONT_WEIGHT_700:
	case CSS_FONT_WEIGHT_BOLD:
		weight = 700;
		break;
	case CSS_FONT_WEIGHT_800:
		weight = 800;
		break;
	case CSS_FONT_WEIGHT_900:
		weight = 900;
		break;
	}

	return weight;
}

/**
 * Map a CSS font style and font variant to plot font flags
 *
 * \param style    CSS font style
 * \param variant  CSS font variant
 * \return Computed plot flags
 */
static plot_font_flags_t plot_font_flags(enum css_font_style_e style,
		enum css_font_variant_e variant)
{
	plot_font_flags_t flags = FONTF_NONE;

	if (style == CSS_FONT_STYLE_ITALIC)
		flags |= FONTF_ITALIC;
	else if (style == CSS_FONT_STYLE_OBLIQUE)
		flags |= FONTF_OBLIQUE;

	if (variant == CSS_FONT_VARIANT_SMALL_CAPS)
		flags |= FONTF_SMALLCAPS;

	return flags;
}


/* exported function documented in html/font.h */
void font_plot_style_from_css(
		const css_unit_ctx *unit_len_ctx,
		const css_computed_style *css,
		plot_font_style_t *fstyle)
{
	lwc_string **families;
	css_fixed length = 0;
	css_unit unit = CSS_UNIT_PX;
	css_color col;

	fstyle->family = plot_font_generic_family(
			css_computed_font_family(css, &families));
	fstyle->families = families;

	css_computed_font_size(css, &length, &unit);
	/* fixes859 (#287) — DEVICE PIXELS, not points.
	 *
	 * This used css_unit_font_size_len2pt(), and the macos9 plotter feeds
	 * fstyle->size straight to QuickDraw's TextSize() -- which is a POINT
	 * size that QuickDraw renders at 72dpi, i.e. 1 pt = 1 device pixel.  But
	 * nscss_screen_dpi is 96 (cssh_css.c:61; browser_set_dpi() is never
	 * called), so the cascade says 1 CSS px == 1 device px.  The pt hop threw
	 * away that 96/72: EVERY author `font-size:14px` was measured and drawn
	 * at 14*72/96 = 10.5 device px -- 25% too small, browser-wide.  That is
	 * why small text has always looked cramped here, and it is what fixes830
	 * was really compensating for when it inflated the DEFAULT font 128->160
	 * to make the default look right (leaving every explicit px size short,
	 * and every em/rem/@media resolving against a 21.33px default instead of
	 * 16 -- hackaday's `@media (min-width:59.5em)` wanted 1269px instead of
	 * 952, so its whole desktop branch, nav included, never applied).
	 *
	 * len2device_px honours ctx->device_dpi, so the value lands in the same
	 * space the plotter and macos9_font.c already treat it as (they even name
	 * the local `size_px`).  Paired with font_size 160->120 in options.h, the
	 * DEFAULT is unchanged on screen -- was: medium = 21.33 CSS px -> 16 pt ->
	 * size=16; now: medium = 16 CSS px -> 16 device px -> size=16, the same
	 * number -- while author px sizes become correct (14px: 10 -> 14) and
	 * em/rem/@media finally match Chrome.  Both must land together: this alone
	 * would scale all text 1.33x, and the option alone would shrink it 0.75x.
	 *
	 * The min clamp below is unit-agnostic and keeps the same effective floor
	 * (8.5 -> 8.5 device px), so it needs no change. */
	fstyle->size = FIXTOINT(FMUL(css_unit_len2device_px(css,
				      unit_len_ctx, length, unit),
				      INTTOFIX(PLOT_STYLE_SCALE)));

	/* Clamp font size to configured minimum */
	if (fstyle->size < (nsoption_int(font_min_size) * PLOT_STYLE_SCALE) / 10)
		fstyle->size = (nsoption_int(font_min_size) * PLOT_STYLE_SCALE) / 10;

	fstyle->weight = plot_font_weight(css_computed_font_weight(css));
	fstyle->flags = plot_font_flags(css_computed_font_style(css),
			css_computed_font_variant(css));

	css_computed_color(css, &col);
	fstyle->foreground = nscss_color_to_ns(col);
	fstyle->background = 0;
	/* fixes42: letter-spacing. NORMAL or INHERIT => 0. SET emits a
	 * pixel value the macos9 plotter inserts between glyphs. */
	{
		css_fixed ls_len = 0;
		css_unit ls_unit = CSS_UNIT_PX;
		uint8_t ls_status = css_computed_letter_spacing(css,
				&ls_len, &ls_unit);
		if (ls_status == CSS_LETTER_SPACING_SET) {
			fstyle->letter_spacing = (int)FIXTOINT(
				css_unit_len2device_px(css, unit_len_ctx,
					ls_len, ls_unit));
		} else {
			fstyle->letter_spacing = 0;
		}
	}
	/* fixes139b: word-spacing. Resolves like letter-spacing but only
	 * affects ASCII spaces (0x20) at paint and measure time. NORMAL or
	 * INHERIT => 0. */
	{
		css_fixed ws_len = 0;
		css_unit ws_unit = CSS_UNIT_PX;
		uint8_t ws_status = css_computed_word_spacing(css,
				&ws_len, &ws_unit);
		if (ws_status == CSS_WORD_SPACING_SET) {
			fstyle->word_spacing = (int)FIXTOINT(
				css_unit_len2device_px(css, unit_len_ctx,
					ws_len, ws_unit));
		} else {
			fstyle->word_spacing = 0;
		}
	}

	/* #251: carry the CSS hyphens value so macos9_font_split only takes
	 * soft-hyphen break opportunities when hyphens != none. */
	fstyle->hyphens = (int)css_computed_hyphens(css);
	/* fixes50/200: -macsurf-text-shadow packed value.
	 *   bits 31..26 h-offset px (6-bit signed, -32..31)
	 *   bits 25..20 v-offset px (6-bit signed, -32..31)
	 *   bits 19..16 blur radius px (4-bit unsigned, 0..15)
	 *   bits 15..0  RGB565 colour (smear high bits down on decode) */
	{
		int32_t ts_packed = 0;
		uint8_t ts_status = css_computed_macsurf_text_shadow(css,
				&ts_packed);
		if (ts_status == CSS_MACSURF_TEXT_SHADOW_SET) {
			uint32_t u = (uint32_t)ts_packed;
			int8_t hp = (int8_t)((u >> 26) & 0x3f);
			int8_t vp = (int8_t)((u >> 20) & 0x3f);
			uint8_t blur = (uint8_t)((u >> 16) & 0x0f);
			uint16_t rgb565 = (uint16_t)(u & 0xffff);
			uint8_t r5 = (uint8_t)((rgb565 >> 11) & 0x1f);
			uint8_t g6 = (uint8_t)((rgb565 >>  5) & 0x3f);
			uint8_t b5 = (uint8_t)((rgb565      ) & 0x1f);
			uint8_t r = (uint8_t)((r5 << 3) | (r5 >> 2));
			uint8_t g = (uint8_t)((g6 << 2) | (g6 >> 4));
			uint8_t b = (uint8_t)((b5 << 3) | (b5 >> 2));

			/* sign extend 6-bit to 8-bit */
			if (hp & 0x20) hp |= 0xc0;
			if (vp & 0x20) vp |= 0xc0;

			fstyle->shadow_x = (int)hp;
			fstyle->shadow_y = (int)vp;
			fstyle->shadow_blur = (int)blur;
			fstyle->shadow_color =
				(colour)(((uint32_t)b << 16) |
				         ((uint32_t)g <<  8) |
				          (uint32_t)r);
		} else {
			fstyle->shadow_x = 0;
			fstyle->shadow_y = 0;
			fstyle->shadow_blur = 0;
			fstyle->shadow_color = 0;
		}
	}
	/* fixes71: -macsurf-transform packed value flows into plot_font_style
	 * so plot_text can honour 90/180/270 rotations. Layout below is
	 *   bits 31..16 rotation Q10.6 deg
	 *   bits 15..8  translate-x int8 px
	 *   bits 7..0   translate-y int8 px
	 * fixes73: scale companion in transform_b. */
	{
		int32_t tfm_packed = 0;
		uint8_t tfm_status = css_computed_macsurf_transform(css,
				&tfm_packed);
		if (tfm_status == CSS_MACSURF_TRANSFORM_SET) {
			fstyle->transform = (int)tfm_packed;
			fstyle->transform_b =
				(int)css_computed_macsurf_transform_b(css);
		} else {
			fstyle->transform = 0;
			fstyle->transform_b = (int)0x01000100;
		}
	}
	/* fixes857 (#286) — the white-to-black clamp is GONE.  It used to read:
	 *     rgb = fstyle->foreground & 0x00ffffff;
	 *     if (rgb == 0x00000000 || rgb == 0x00ffffff)
	 *             fstyle->foreground = 0x00000000;
	 * i.e. any text the cascade computed as pure WHITE was repainted opaque
	 * BLACK.  Its own comment called it "a diagnostic fallback; real CSS text
	 * colour support lands once the cascade is sound" -- a v0.4-era guard from
	 * when the UA sheet still had body{background:#fff} and a mis-cascaded
	 * white would vanish into it.  fixes629 fixed that for real (dropped the
	 * UA body background, moved the default text colour to an inheritable,
	 * author-overridable html{color:#000}), but the clamp was never removed --
	 * so it kept silently mugging the single most common colour on the modern
	 * web: white text on a dark theme.
	 *
	 * HW-observed on hackaday.com: `h1,h1>a,h2,h2>a{color:#fff}` (style.css:344)
	 * rendered BLACK on the #1a1a1a body, while `<p>` under
	 * `body{color:#ddd}` (style.css:310) rendered correctly light -- because
	 * #dddddd is not pure white and slipped past the == test.  That split is
	 * the clamp's fingerprint, and it also proves the cascade is sound: the
	 * exact same sheet, element and inheritance chain deliver #ddd fine.  Only
	 * #fff was ever broken.
	 *
	 * There is nothing to replace it with.  `color: #fff` is valid CSS and the
	 * renderer's job is to draw it; a page that really does put white on white
	 * is broken in every browser, and guessing on the author's behalf is what
	 * caused this bug.  Deliberately no clamp, no heuristic, no contrast
	 * fixup -- if text is illegible, fix the cascade or the backdrop, not the
	 * colour at the point of use. */
}

int font_plot_style_baseline(const plot_font_style_t *fstyle, int line_height)
{
	int size = 12;
	int baseline;
	int family_floor;

	if (fstyle != NULL) {
		size = (int)(fstyle->size >> PLOT_STYLE_RADIX);
		if (size <= 0) {
			size = 12;
		}
	}

	if (line_height <= 0) {
		line_height = size;
	}

	baseline = (line_height * 3 + 2) / 4;

	if (fstyle != NULL &&
			fstyle->family == PLOT_FONT_FAMILY_MONOSPACE) {
		family_floor = size;
	} else {
		family_floor = (size * 3 + 3) / 4;
	}

	if (baseline < family_floor) {
		baseline = family_floor;
	}
	if (baseline < 1) {
		baseline = 1;
	}

	return baseline;
}
