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
	fstyle->size = FIXTOINT(FMUL(css_unit_font_size_len2pt(css,
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
	/* fixes50: -macsurf-text-shadow packed value.
	 *   bits 31..24 h-offset px (int8)
	 *   bits 23..16 v-offset px (int8)
	 *   bits 15..0  RGB565 colour (smear high bits down on decode) */
	{
		int32_t ts_packed = 0;
		uint8_t ts_status = css_computed_macsurf_text_shadow(css,
				&ts_packed);
		if (ts_status == CSS_MACSURF_TEXT_SHADOW_SET) {
			uint32_t u = (uint32_t)ts_packed;
			int8_t hp = (int8_t)((u >> 24) & 0xff);
			int8_t vp = (int8_t)((u >> 16) & 0xff);
			uint16_t rgb565 = (uint16_t)(u & 0xffff);
			uint8_t r5 = (uint8_t)((rgb565 >> 11) & 0x1f);
			uint8_t g6 = (uint8_t)((rgb565 >>  5) & 0x3f);
			uint8_t b5 = (uint8_t)((rgb565      ) & 0x1f);
			uint8_t r = (uint8_t)((r5 << 3) | (r5 >> 2));
			uint8_t g = (uint8_t)((g6 << 2) | (g6 >> 4));
			uint8_t b = (uint8_t)((b5 << 3) | (b5 >> 2));
			fstyle->shadow_x = (int)hp;
			fstyle->shadow_y = (int)vp;
			fstyle->shadow_color =
				(colour)(((uint32_t)b << 16) |
				         ((uint32_t)g <<  8) |
				          (uint32_t)r);
		} else {
			fstyle->shadow_x = 0;
			fstyle->shadow_y = 0;
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
	/* Safety: when the CSS cascade produces a suspicious colour (white,
	 * transparent, or otherwise garbage from an incomplete computed
	 * style), force foreground to opaque black so text is always
	 * legible. This is a diagnostic fallback; real CSS text colour
	 * support lands once the cascade is sound. NetSurf colour is XBGR
	 * so 0x00000000 = opaque black. */
	{
		uint32_t rgb = fstyle->foreground & 0x00ffffff;
		if (rgb == 0x00000000 || rgb == 0x00ffffff)
			fstyle->foreground = 0x00000000;
	}
}
