/*
 * MacSurf stub -- css/utils.h
 * C89-compatible stub for CodeWarrior 8 / GCC PPC Mac OS 9.
 *
 * Provides the same symbols as content/handlers/css/utils.h:
 *   nscss_screen_dpi, ns_computed_display, ns_computed_display_static,
 *   ns_computed_min_height, ns_computed_min_width.
 *
 * Licensed under GPL v2.
 */

#ifndef MACOS9_CSS_UTILS_H
#define MACOS9_CSS_UTILS_H

#include <libcss/libcss.h>

/* netsurf/css.h macros -- inlined here to avoid search-path issues */
#ifndef nscss_color_to_ns
#define nscss_color_to_ns(c) \
        ( ((~(c)) & 0xff000000)        | \
         ((( (c)) & 0xff0000  ) >> 16) | \
          (( (c)) & 0xff00    )        | \
         ((( (c)) & 0xff      ) << 16))
#endif

#ifndef ns_color_to_nscss
#define ns_color_to_nscss(c) \
        ( ((~(c)) & 0xff000000)        | \
         ((( (c)) & 0xff0000  ) >> 16) | \
          (( (c)) & 0xff00    )        | \
         ((( (c)) & 0xff      ) << 16))
#endif

#ifndef nscss_color_is_transparent
#define nscss_color_is_transparent(color) \
        (((color) >> 24) == 0)
#endif

/* DPI of the screen, in fixed-point units */
extern css_fixed nscss_screen_dpi;

/*
 * Wrapper for css_computed_display -- maps unsupported grid values
 * to block/inline-block equivalents.
 */
static uint8_t ns_computed_display(
        const css_computed_style *style, bool root)
{
    uint8_t value = css_computed_display(style, root);

    switch (value) {
    case CSS_DISPLAY_GRID:
        return CSS_DISPLAY_BLOCK;
    case CSS_DISPLAY_INLINE_GRID:
        return CSS_DISPLAY_INLINE_BLOCK;
    default:
        break;
    }
    return value;
}

/*
 * Static variant -- calls css_computed_display_static instead.
 */
static uint8_t ns_computed_display_static(
        const css_computed_style *style)
{
    uint8_t value = css_computed_display_static(style);

    switch (value) {
    case CSS_DISPLAY_GRID:
        return CSS_DISPLAY_BLOCK;
    case CSS_DISPLAY_INLINE_GRID:
        return CSS_DISPLAY_INLINE_BLOCK;
    default:
        break;
    }
    return value;
}

/*
 * Wrapper for css_computed_min_height -- converts AUTO to SET 0px.
 */
static uint8_t ns_computed_min_height(
        const css_computed_style *style,
        css_fixed *length, css_unit *unit)
{
    uint8_t value = css_computed_min_height(style, length, unit);

    if (value == CSS_MIN_HEIGHT_AUTO) {
        value = CSS_MIN_HEIGHT_SET;
        *length = 0;
        *unit = CSS_UNIT_PX;
    }
    return value;
}

/*
 * Wrapper for css_computed_min_width -- converts AUTO to SET 0px.
 */
static uint8_t ns_computed_min_width(
        const css_computed_style *style,
        css_fixed *length, css_unit *unit)
{
    uint8_t value = css_computed_min_width(style, length, unit);

    if (value == CSS_MIN_WIDTH_AUTO) {
        value = CSS_MIN_WIDTH_SET;
        *length = 0;
        *unit = CSS_UNIT_PX;
    }
    return value;
}

#endif /* MACOS9_CSS_UTILS_H */
