/*
 * MacSurf stub -- libcss/libcss.h
 * Monolithic C89-compatible stub for libcss.
 * Covers: fpmath, errors, types, properties, font_face, computed,
 *         hint, functypes, stylesheet, select, unit.
 *
 * Constraints: C89, no inline keyword, no // comments, no _Bool.
 * Target: CodeWarrior 8 / GCC cross-compile for PowerPC Mac OS 9.
 */

#ifndef MACOS9_LIBCSS_LIBCSS_H
#define MACOS9_LIBCSS_LIBCSS_H

#include <stddef.h>
#include <limits.h>

#include <libwapcaplet/libwapcaplet.h>

/* --- bool compatibility (C89, guarded so libwapcaplet.h wins) --- */
/* libwapcaplet.h already defines bool with the same #ifndef guard; */
/* no redefinition needed here. */

/* --- integer type aliases (C89, no stdint.h) --- */
typedef unsigned long  uint32_t;
typedef long           int32_t;
typedef unsigned short uint16_t;
typedef unsigned char  uint8_t;

/* =========================================================================
 * 2. Fixed-point math (from fpmath.h)
 * ========================================================================= */

#define CSS_RADIX_POINT 10

typedef long css_fixed;

static css_fixed css_add_fixed(const css_fixed x, const css_fixed y) {
    long ux = x, uy = y, res = ux + uy;
    ux = (ux >> 31) + INT_MAX;
    if ((long)((ux ^ uy) | ~(uy ^ res)) >= 0) res = ux;
    return res;
}

static css_fixed css_subtract_fixed(const css_fixed x, const css_fixed y) {
    long ux = x, uy = y, res = ux - uy;
    ux = (ux >> 31) + INT_MAX;
    if ((long)((ux ^ uy) & (ux ^ res)) < 0) res = ux;
    return res;
}

static css_fixed css_divide_fixed(const css_fixed x, const css_fixed y) {
    long res = ((long)x * (1 << CSS_RADIX_POINT)) / y;
    return res;
}

static css_fixed css_multiply_fixed(const css_fixed x, const css_fixed y) {
    long res = ((long)x * (long)y) >> CSS_RADIX_POINT;
    return res;
}

static css_fixed css_int_to_fixed(const int a) {
    long xx = ((long)a) * (1 << CSS_RADIX_POINT);
    if (xx < (-2147483647L - 1)) xx = (-2147483647L - 1);
    if (xx > 2147483647L) xx = 2147483647L;
    return (css_fixed)xx;
}

static css_fixed css_float_to_fixed(const float a) {
    float xx = a * (float)(1 << CSS_RADIX_POINT);
    if (xx < (-2147483647L - 1)) return (css_fixed)(-2147483647L - 1);
    if (xx >= (float)2147483647L) return 2147483647L;
    return (css_fixed)xx;
}

#define FADD(a, b)       (css_add_fixed((a), (b)))
#define FSUB(a, b)       (css_subtract_fixed((a), (b)))
#define FMUL(a, b)       (css_multiply_fixed((a), (b)))
#define FDIV(a, b)       (css_divide_fixed((a), (b)))
#define FLTTOFIX(a)      ((css_fixed)((a) * (float)(1 << CSS_RADIX_POINT)))
#define FIXTOFLT(a)      ((float)(a) / (float)(1 << CSS_RADIX_POINT))
#define INTTOFIX(a)      (css_int_to_fixed(a))
#define FIXTOINT(a)      ((a) >> CSS_RADIX_POINT)
#define TRUNCATEFIX(a)   ((a) & ~((1 << CSS_RADIX_POINT) - 1))
#define FIXFRAC(a)       ((a) & ((1 << CSS_RADIX_POINT) - 1))
#define FPCT_OF_INT_TOINT(p, i) (FIXTOINT(FDIV((p * i), F_100)))

#define F_PI_2  0x00000648
#define F_PI    0x00000c91
#define F_3PI_2 0x000012d9
#define F_2PI   0x00001922
#define F_90    0x00016800
#define F_180   0x0002d000
#define F_270   0x00043800
#define F_360   0x0005a000
#define F_0_5   0x00000200
#define F_1     0x00000400
#define F_10    0x00002800
#define F_72    0x00012000
#define F_96    0x00018000
#define F_100   0x00019000
#define F_200   0x00032000
#define F_255   0x0003FC00
#define F_300   0x0004b000
#define F_400   0x00064000

/* =========================================================================
 * 3. Error enum (from errors.h)
 * ========================================================================= */

typedef enum css_error {
    CSS_OK                = 0,
    CSS_NOMEM             = 1,
    CSS_BADPARM           = 2,
    CSS_INVALID           = 3,
    CSS_FILENOTFOUND      = 4,
    CSS_NEEDDATA          = 5,
    CSS_BADCHARSET        = 6,
    CSS_EOF               = 7,
    CSS_IMPORTS_PENDING   = 8,
    CSS_PROPERTY_NOT_SET  = 9
} css_error;

extern const char *css_error_to_string(css_error error);

/* =========================================================================
 * 4. Type enums (from types.h)
 * ========================================================================= */

typedef enum css_charset_source {
    CSS_CHARSET_DEFAULT  = 0,
    CSS_CHARSET_REFERRED = 1,
    CSS_CHARSET_METADATA = 2,
    CSS_CHARSET_DOCUMENT = 3,
    CSS_CHARSET_DICTATED = 4
} css_charset_source;

typedef enum css_language_level {
    CSS_LEVEL_1       = 0,
    CSS_LEVEL_2       = 1,
    CSS_LEVEL_21      = 2,
    CSS_LEVEL_3       = 3,
    CSS_LEVEL_DEFAULT = CSS_LEVEL_21
} css_language_level;

typedef enum css_media_type {
    CSS_MEDIA_AURAL      = (1 << 0),
    CSS_MEDIA_BRAILLE    = (1 << 1),
    CSS_MEDIA_EMBOSSED   = (1 << 2),
    CSS_MEDIA_HANDHELD   = (1 << 3),
    CSS_MEDIA_PRINT      = (1 << 4),
    CSS_MEDIA_PROJECTION = (1 << 5),
    CSS_MEDIA_SCREEN     = (1 << 6),
    CSS_MEDIA_SPEECH     = (1 << 7),
    CSS_MEDIA_TTY        = (1 << 8),
    CSS_MEDIA_TV         = (1 << 9),
    CSS_MEDIA_ALL        = CSS_MEDIA_AURAL | CSS_MEDIA_BRAILLE |
                           CSS_MEDIA_EMBOSSED | CSS_MEDIA_HANDHELD |
                           CSS_MEDIA_PRINT | CSS_MEDIA_PROJECTION |
                           CSS_MEDIA_SCREEN | CSS_MEDIA_SPEECH |
                           CSS_MEDIA_TTY | CSS_MEDIA_TV
} css_media_type;

typedef enum css_origin {
    CSS_ORIGIN_UA     = 0,
    CSS_ORIGIN_USER   = 1,
    CSS_ORIGIN_AUTHOR = 2
} css_origin;

typedef unsigned long css_color;

typedef enum css_unit {
    CSS_UNIT_PX   = 0x00,
    CSS_UNIT_EX   = 0x01,
    CSS_UNIT_EM   = 0x02,
    CSS_UNIT_IN   = 0x03,
    CSS_UNIT_CM   = 0x04,
    CSS_UNIT_MM   = 0x05,
    CSS_UNIT_PT   = 0x06,
    CSS_UNIT_PC   = 0x07,
    CSS_UNIT_CH   = 0x08,
    CSS_UNIT_REM  = 0x09,
    CSS_UNIT_LH   = 0x0a,
    CSS_UNIT_VH   = 0x0b,
    CSS_UNIT_VW   = 0x0c,
    CSS_UNIT_VI   = 0x0d,
    CSS_UNIT_VB   = 0x0e,
    CSS_UNIT_VMIN = 0x0f,
    CSS_UNIT_VMAX = 0x10,
    CSS_UNIT_Q    = 0x11,
    CSS_UNIT_PCT  = 0x15,
    CSS_UNIT_DEG  = 0x16,
    CSS_UNIT_GRAD = 0x17,
    CSS_UNIT_RAD  = 0x18,
    CSS_UNIT_MS   = 0x19,
    CSS_UNIT_S    = 0x1a,
    CSS_UNIT_HZ   = 0x1b,
    CSS_UNIT_KHZ  = 0x1c,
    CSS_UNIT_CALC = 0x1d
} css_unit;

typedef enum css_media_orientation {
    CSS_MEDIA_ORIENTATION_PORTRAIT  = 0,
    CSS_MEDIA_ORIENTATION_LANDSCAPE = 1
} css_media_orientation;

typedef enum css_media_scan {
    CSS_MEDIA_SCAN_PROGRESSIVE = 0,
    CSS_MEDIA_SCAN_INTERLACE   = 1
} css_media_scan;

typedef enum css_media_update_frequency {
    CSS_MEDIA_UPDATE_FREQUENCY_NORMAL = 0,
    CSS_MEDIA_UPDATE_FREQUENCY_SLOW   = 1,
    CSS_MEDIA_UPDATE_FREQUENCY_NONE   = 2
} css_media_update_frequency;

typedef enum css_media_overflow_block {
    CSS_MEDIA_OVERFLOW_BLOCK_NONE           = 0,
    CSS_MEDIA_OVERFLOW_BLOCK_SCROLL         = 1,
    CSS_MEDIA_OVERFLOW_BLOCK_OPTIONAL_PAGED = 2,
    CSS_MEDIA_OVERFLOW_BLOCK_PAGED          = 3
} css_media_overflow_block;

typedef enum css_media_overflow_inline {
    CSS_MEDIA_OVERFLOW_INLINE_NONE   = 0,
    CSS_MEDIA_OVERFLOW_INLINE_SCROLL = 1
} css_media_overflow_inline;

typedef enum css_media_pointer {
    CSS_MEDIA_POINTER_NONE   = 0,
    CSS_MEDIA_POINTER_COARSE = 1,
    CSS_MEDIA_POINTER_FINE   = 2
} css_media_pointer;

typedef enum css_media_hover {
    CSS_MEDIA_HOVER_NONE      = 0,
    CSS_MEDIA_HOVER_ON_DEMAND = 1,
    CSS_MEDIA_HOVER_HOVER     = 2
} css_media_hover;

typedef enum css_media_light_level {
    CSS_MEDIA_LIGHT_LEVEL_NORMAL = 0,
    CSS_MEDIA_LIGHT_LEVEL_DIM    = 1,
    CSS_MEDIA_LIGHT_LEVEL_WASHED = 2
} css_media_light_level;

typedef enum css_media_scripting {
    CSS_MEDIA_SCRIPTING_NONE         = 0,
    CSS_MEDIA_SCRIPTING_INITIAL_ONLY = 1,
    CSS_MEDIA_SCRIPTING_ENABLED      = 2
} css_media_scripting;

typedef struct css_media_resolution {
    css_fixed value;
    css_unit  unit;
} css_media_resolution;

typedef struct css_media {
    css_media_type             type;
    css_fixed                  width;
    css_fixed                  height;
    css_fixed                  aspect_ratio;
    css_media_orientation      orientation;
    css_media_resolution       resolution;
    css_media_scan             scan;
    css_fixed                  grid;
    css_media_update_frequency update;
    css_media_overflow_block   overflow_block;
    css_media_overflow_inline  overflow_inline;
    css_fixed                  color;
    css_fixed                  color_index;
    css_fixed                  monochrome;
    css_fixed                  inverted_colors;
    lwc_string                *prefers_color_scheme;
    css_media_pointer          pointer;
    css_media_pointer          any_pointer;
    css_media_hover            hover;
    css_media_hover            any_hover;
    css_media_light_level      light_level;
    css_media_scripting        scripting;
} css_media;

typedef struct css_qname {
    lwc_string *ns;
    lwc_string *name;
} css_qname;

typedef struct css_stylesheet     css_stylesheet;
typedef struct css_select_ctx     css_select_ctx;
typedef struct css_computed_style css_computed_style;
typedef struct css_font_face      css_font_face;
typedef struct css_font_face_src  css_font_face_src;
typedef struct css_unit_ctx       css_unit_ctx;
typedef struct css_system_font    css_system_font;

/* =========================================================================
 * 6. Property enums (from properties.h)
 * ========================================================================= */

enum css_properties_e {
    CSS_PROP_AZIMUTH                = 0x000,
    CSS_PROP_BACKGROUND_ATTACHMENT  = 0x001,
    CSS_PROP_BACKGROUND_COLOR       = 0x002,
    CSS_PROP_BACKGROUND_IMAGE       = 0x003,
    CSS_PROP_BACKGROUND_POSITION    = 0x004,
    CSS_PROP_BACKGROUND_REPEAT      = 0x005,
    CSS_PROP_BORDER_COLLAPSE        = 0x006,
    CSS_PROP_BORDER_SPACING         = 0x007,
    CSS_PROP_BORDER_TOP_COLOR       = 0x008,
    CSS_PROP_BORDER_RIGHT_COLOR     = 0x009,
    CSS_PROP_BORDER_BOTTOM_COLOR    = 0x00a,
    CSS_PROP_BORDER_LEFT_COLOR      = 0x00b,
    CSS_PROP_BORDER_TOP_STYLE       = 0x00c,
    CSS_PROP_BORDER_RIGHT_STYLE     = 0x00d,
    CSS_PROP_BORDER_BOTTOM_STYLE    = 0x00e,
    CSS_PROP_BORDER_LEFT_STYLE      = 0x00f,
    CSS_PROP_BORDER_TOP_WIDTH       = 0x010,
    CSS_PROP_BORDER_RIGHT_WIDTH     = 0x011,
    CSS_PROP_BORDER_BOTTOM_WIDTH    = 0x012,
    CSS_PROP_BORDER_LEFT_WIDTH      = 0x013,
    CSS_PROP_BOTTOM                 = 0x014,
    CSS_PROP_CAPTION_SIDE           = 0x015,
    CSS_PROP_CLEAR                  = 0x016,
    CSS_PROP_CLIP                   = 0x017,
    CSS_PROP_COLOR                  = 0x018,
    CSS_PROP_CONTENT                = 0x019,
    CSS_PROP_COUNTER_INCREMENT      = 0x01a,
    CSS_PROP_COUNTER_RESET          = 0x01b,
    CSS_PROP_CUE_AFTER              = 0x01c,
    CSS_PROP_CUE_BEFORE             = 0x01d,
    CSS_PROP_CURSOR                 = 0x01e,
    CSS_PROP_DIRECTION              = 0x01f,
    CSS_PROP_DISPLAY                = 0x020,
    CSS_PROP_ELEVATION              = 0x021,
    CSS_PROP_EMPTY_CELLS            = 0x022,
    CSS_PROP_FLOAT                  = 0x023,
    CSS_PROP_FONT_FAMILY            = 0x024,
    CSS_PROP_FONT_SIZE              = 0x025,
    CSS_PROP_FONT_STYLE             = 0x026,
    CSS_PROP_FONT_VARIANT           = 0x027,
    CSS_PROP_FONT_WEIGHT            = 0x028,
    CSS_PROP_HEIGHT                 = 0x029,
    CSS_PROP_LEFT                   = 0x02a,
    CSS_PROP_LETTER_SPACING         = 0x02b,
    CSS_PROP_LINE_HEIGHT            = 0x02c,
    CSS_PROP_LIST_STYLE_IMAGE       = 0x02d,
    CSS_PROP_LIST_STYLE_POSITION    = 0x02e,
    CSS_PROP_LIST_STYLE_TYPE        = 0x02f,
    CSS_PROP_MARGIN_TOP             = 0x030,
    CSS_PROP_MARGIN_RIGHT           = 0x031,
    CSS_PROP_MARGIN_BOTTOM          = 0x032,
    CSS_PROP_MARGIN_LEFT            = 0x033,
    CSS_PROP_MAX_HEIGHT             = 0x034,
    CSS_PROP_MAX_WIDTH              = 0x035,
    CSS_PROP_MIN_HEIGHT             = 0x036,
    CSS_PROP_MIN_WIDTH              = 0x037,
    CSS_PROP_ORPHANS                = 0x038,
    CSS_PROP_OUTLINE_COLOR          = 0x039,
    CSS_PROP_OUTLINE_STYLE          = 0x03a,
    CSS_PROP_OUTLINE_WIDTH          = 0x03b,
    CSS_PROP_OVERFLOW_X             = 0x03c,
    CSS_PROP_PADDING_TOP            = 0x03d,
    CSS_PROP_PADDING_RIGHT          = 0x03e,
    CSS_PROP_PADDING_BOTTOM         = 0x03f,
    CSS_PROP_PADDING_LEFT           = 0x040,
    CSS_PROP_PAGE_BREAK_AFTER       = 0x041,
    CSS_PROP_PAGE_BREAK_BEFORE      = 0x042,
    CSS_PROP_PAGE_BREAK_INSIDE      = 0x043,
    CSS_PROP_PAUSE_AFTER            = 0x044,
    CSS_PROP_PAUSE_BEFORE           = 0x045,
    CSS_PROP_PITCH_RANGE            = 0x046,
    CSS_PROP_PITCH                  = 0x047,
    CSS_PROP_PLAY_DURING            = 0x048,
    CSS_PROP_POSITION               = 0x049,
    CSS_PROP_QUOTES                 = 0x04a,
    CSS_PROP_RICHNESS               = 0x04b,
    CSS_PROP_RIGHT                  = 0x04c,
    CSS_PROP_SPEAK_HEADER           = 0x04d,
    CSS_PROP_SPEAK_NUMERAL          = 0x04e,
    CSS_PROP_SPEAK_PUNCTUATION      = 0x04f,
    CSS_PROP_SPEAK                  = 0x050,
    CSS_PROP_SPEECH_RATE            = 0x051,
    CSS_PROP_STRESS                 = 0x052,
    CSS_PROP_TABLE_LAYOUT           = 0x053,
    CSS_PROP_TEXT_ALIGN             = 0x054,
    CSS_PROP_TEXT_DECORATION        = 0x055,
    CSS_PROP_TEXT_INDENT            = 0x056,
    CSS_PROP_TEXT_TRANSFORM         = 0x057,
    CSS_PROP_TOP                    = 0x058,
    CSS_PROP_UNICODE_BIDI           = 0x059,
    CSS_PROP_VERTICAL_ALIGN         = 0x05a,
    CSS_PROP_VISIBILITY             = 0x05b,
    CSS_PROP_VOICE_FAMILY           = 0x05c,
    CSS_PROP_VOLUME                 = 0x05d,
    CSS_PROP_WHITE_SPACE            = 0x05e,
    CSS_PROP_WIDOWS                 = 0x05f,
    CSS_PROP_WIDTH                  = 0x060,
    CSS_PROP_WORD_SPACING           = 0x061,
    CSS_PROP_Z_INDEX                = 0x062,
    CSS_PROP_OPACITY                = 0x063,
    CSS_PROP_BREAK_AFTER            = 0x064,
    CSS_PROP_BREAK_BEFORE           = 0x065,
    CSS_PROP_BREAK_INSIDE           = 0x066,
    CSS_PROP_COLUMN_COUNT           = 0x067,
    CSS_PROP_COLUMN_FILL            = 0x068,
    CSS_PROP_COLUMN_GAP             = 0x069,
    CSS_PROP_COLUMN_RULE_COLOR      = 0x06a,
    CSS_PROP_COLUMN_RULE_STYLE      = 0x06b,
    CSS_PROP_COLUMN_RULE_WIDTH      = 0x06c,
    CSS_PROP_COLUMN_SPAN            = 0x06d,
    CSS_PROP_COLUMN_WIDTH           = 0x06e,
    CSS_PROP_WRITING_MODE           = 0x06f,
    CSS_PROP_OVERFLOW_Y             = 0x070,
    CSS_PROP_BOX_SIZING             = 0x071,
    CSS_PROP_ALIGN_CONTENT          = 0x072,
    CSS_PROP_ALIGN_ITEMS            = 0x073,
    CSS_PROP_ALIGN_SELF             = 0x074,
    CSS_PROP_FLEX_BASIS             = 0x075,
    CSS_PROP_FLEX_DIRECTION         = 0x076,
    CSS_PROP_FLEX_GROW              = 0x077,
    CSS_PROP_FLEX_SHRINK            = 0x078,
    CSS_PROP_FLEX_WRAP              = 0x079,
    CSS_PROP_JUSTIFY_CONTENT        = 0x07a,
    CSS_PROP_ORDER                  = 0x07b,
    CSS_PROP_FILL_OPACITY           = 0x07c,
    CSS_PROP_STROKE_OPACITY         = 0x07d,
    CSS_N_PROPERTIES
};

enum css_align_content_e {
    CSS_ALIGN_CONTENT_INHERIT        = 0x0,
    CSS_ALIGN_CONTENT_STRETCH        = 0x1,
    CSS_ALIGN_CONTENT_FLEX_START     = 0x2,
    CSS_ALIGN_CONTENT_FLEX_END       = 0x3,
    CSS_ALIGN_CONTENT_CENTER         = 0x4,
    CSS_ALIGN_CONTENT_SPACE_BETWEEN  = 0x5,
    CSS_ALIGN_CONTENT_SPACE_AROUND   = 0x6,
    CSS_ALIGN_CONTENT_SPACE_EVENLY   = 0x7
};

enum css_align_items_e {
    CSS_ALIGN_ITEMS_INHERIT    = 0x0,
    CSS_ALIGN_ITEMS_STRETCH    = 0x1,
    CSS_ALIGN_ITEMS_FLEX_START = 0x2,
    CSS_ALIGN_ITEMS_FLEX_END   = 0x3,
    CSS_ALIGN_ITEMS_CENTER     = 0x4,
    CSS_ALIGN_ITEMS_BASELINE   = 0x5
};

enum css_align_self_e {
    CSS_ALIGN_SELF_INHERIT    = CSS_ALIGN_ITEMS_INHERIT,
    CSS_ALIGN_SELF_STRETCH    = CSS_ALIGN_ITEMS_STRETCH,
    CSS_ALIGN_SELF_FLEX_START = CSS_ALIGN_ITEMS_FLEX_START,
    CSS_ALIGN_SELF_FLEX_END   = CSS_ALIGN_ITEMS_FLEX_END,
    CSS_ALIGN_SELF_CENTER     = CSS_ALIGN_ITEMS_CENTER,
    CSS_ALIGN_SELF_BASELINE   = CSS_ALIGN_ITEMS_BASELINE,
    CSS_ALIGN_SELF_AUTO       = 0x6
};

enum css_background_attachment_e {
    CSS_BACKGROUND_ATTACHMENT_INHERIT = 0x0,
    CSS_BACKGROUND_ATTACHMENT_FIXED   = 0x1,
    CSS_BACKGROUND_ATTACHMENT_SCROLL  = 0x2
};

enum css_background_color_e {
    CSS_BACKGROUND_COLOR_INHERIT       = 0x0,
    CSS_BACKGROUND_COLOR_COLOR         = 0x1,
    CSS_BACKGROUND_COLOR_CURRENT_COLOR = 0x2
};

enum css_background_image_e {
    CSS_BACKGROUND_IMAGE_INHERIT = 0x0,
    CSS_BACKGROUND_IMAGE_NONE    = 0x1,
    CSS_BACKGROUND_IMAGE_IMAGE   = 0x1
};

enum css_background_position_e {
    CSS_BACKGROUND_POSITION_INHERIT = 0x0,
    CSS_BACKGROUND_POSITION_SET     = 0x1
};

enum css_background_repeat_e {
    CSS_BACKGROUND_REPEAT_INHERIT   = 0x0,
    CSS_BACKGROUND_REPEAT_REPEAT_X  = 0x1,
    CSS_BACKGROUND_REPEAT_REPEAT_Y  = 0x2,
    CSS_BACKGROUND_REPEAT_REPEAT    = 0x3,
    CSS_BACKGROUND_REPEAT_NO_REPEAT = 0x4
};

enum css_border_collapse_e {
    CSS_BORDER_COLLAPSE_INHERIT  = 0x0,
    CSS_BORDER_COLLAPSE_SEPARATE = 0x1,
    CSS_BORDER_COLLAPSE_COLLAPSE = 0x2
};

enum css_border_spacing_e {
    CSS_BORDER_SPACING_INHERIT = 0x0,
    CSS_BORDER_SPACING_SET     = 0x1
};

enum css_border_color_e {
    CSS_BORDER_COLOR_INHERIT       = CSS_BACKGROUND_COLOR_INHERIT,
    CSS_BORDER_COLOR_COLOR         = CSS_BACKGROUND_COLOR_COLOR,
    CSS_BORDER_COLOR_CURRENT_COLOR = CSS_BACKGROUND_COLOR_CURRENT_COLOR
};

enum css_border_style_e {
    CSS_BORDER_STYLE_INHERIT = 0x0,
    CSS_BORDER_STYLE_NONE    = 0x1,
    CSS_BORDER_STYLE_HIDDEN  = 0x2,
    CSS_BORDER_STYLE_DOTTED  = 0x3,
    CSS_BORDER_STYLE_DASHED  = 0x4,
    CSS_BORDER_STYLE_SOLID   = 0x5,
    CSS_BORDER_STYLE_DOUBLE  = 0x6,
    CSS_BORDER_STYLE_GROOVE  = 0x7,
    CSS_BORDER_STYLE_RIDGE   = 0x8,
    CSS_BORDER_STYLE_INSET   = 0x9,
    CSS_BORDER_STYLE_OUTSET  = 0xa
};

enum css_border_width_e {
    CSS_BORDER_WIDTH_INHERIT = 0x0,
    CSS_BORDER_WIDTH_THIN    = 0x1,
    CSS_BORDER_WIDTH_MEDIUM  = 0x2,
    CSS_BORDER_WIDTH_THICK   = 0x3,
    CSS_BORDER_WIDTH_WIDTH   = 0x4
};

enum css_bottom_e {
    CSS_BOTTOM_INHERIT = 0x0,
    CSS_BOTTOM_SET     = 0x1,
    CSS_BOTTOM_AUTO    = 0x2
};

enum css_box_sizing_e {
    CSS_BOX_SIZING_INHERIT     = 0x0,
    CSS_BOX_SIZING_CONTENT_BOX = 0x1,
    CSS_BOX_SIZING_BORDER_BOX  = 0x2
};

enum css_break_after_e {
    CSS_BREAK_AFTER_INHERIT       = 0x0,
    CSS_BREAK_AFTER_AUTO          = 0x1,
    CSS_BREAK_AFTER_AVOID         = 0x2,
    CSS_BREAK_AFTER_ALWAYS        = 0x3,
    CSS_BREAK_AFTER_LEFT          = 0x4,
    CSS_BREAK_AFTER_RIGHT         = 0x5,
    CSS_BREAK_AFTER_PAGE          = 0x6,
    CSS_BREAK_AFTER_COLUMN        = 0x7,
    CSS_BREAK_AFTER_AVOID_PAGE    = 0x8,
    CSS_BREAK_AFTER_AVOID_COLUMN  = 0x9
};

enum css_break_before_e {
    CSS_BREAK_BEFORE_INHERIT       = CSS_BREAK_AFTER_INHERIT,
    CSS_BREAK_BEFORE_AUTO          = CSS_BREAK_AFTER_AUTO,
    CSS_BREAK_BEFORE_AVOID         = CSS_BREAK_AFTER_AVOID,
    CSS_BREAK_BEFORE_ALWAYS        = CSS_BREAK_AFTER_ALWAYS,
    CSS_BREAK_BEFORE_LEFT          = CSS_BREAK_AFTER_LEFT,
    CSS_BREAK_BEFORE_RIGHT         = CSS_BREAK_AFTER_RIGHT,
    CSS_BREAK_BEFORE_PAGE          = CSS_BREAK_AFTER_PAGE,
    CSS_BREAK_BEFORE_COLUMN        = CSS_BREAK_AFTER_COLUMN,
    CSS_BREAK_BEFORE_AVOID_PAGE    = CSS_BREAK_AFTER_AVOID_PAGE,
    CSS_BREAK_BEFORE_AVOID_COLUMN  = CSS_BREAK_AFTER_AVOID_COLUMN
};

enum css_break_inside_e {
    CSS_BREAK_INSIDE_INHERIT       = CSS_BREAK_AFTER_INHERIT,
    CSS_BREAK_INSIDE_AUTO          = CSS_BREAK_AFTER_AUTO,
    CSS_BREAK_INSIDE_AVOID         = CSS_BREAK_AFTER_AVOID,
    CSS_BREAK_INSIDE_AVOID_PAGE    = CSS_BREAK_AFTER_AVOID_PAGE,
    CSS_BREAK_INSIDE_AVOID_COLUMN  = CSS_BREAK_AFTER_AVOID_COLUMN
};

enum css_caption_side_e {
    CSS_CAPTION_SIDE_INHERIT = 0x0,
    CSS_CAPTION_SIDE_TOP     = 0x1,
    CSS_CAPTION_SIDE_BOTTOM  = 0x2
};

enum css_clear_e {
    CSS_CLEAR_INHERIT = 0x0,
    CSS_CLEAR_NONE    = 0x1,
    CSS_CLEAR_LEFT    = 0x2,
    CSS_CLEAR_RIGHT   = 0x3,
    CSS_CLEAR_BOTH    = 0x4
};

enum css_clip_e {
    CSS_CLIP_INHERIT = 0x0,
    CSS_CLIP_AUTO    = 0x1,
    CSS_CLIP_RECT    = 0x2
};

enum css_color_e {
    CSS_COLOR_INHERIT = 0x0,
    CSS_COLOR_COLOR   = 0x1
};

enum css_column_count_e {
    CSS_COLUMN_COUNT_INHERIT = 0x0,
    CSS_COLUMN_COUNT_AUTO    = 0x1,
    CSS_COLUMN_COUNT_SET     = 0x2
};

enum css_column_fill_e {
    CSS_COLUMN_FILL_INHERIT  = 0x0,
    CSS_COLUMN_FILL_BALANCE  = 0x1,
    CSS_COLUMN_FILL_AUTO     = 0x2
};

enum css_column_gap_e {
    CSS_COLUMN_GAP_INHERIT = 0x0,
    CSS_COLUMN_GAP_SET     = 0x1,
    CSS_COLUMN_GAP_NORMAL  = 0x2
};

enum css_column_rule_color_e {
    CSS_COLUMN_RULE_COLOR_INHERIT       = CSS_BACKGROUND_COLOR_INHERIT,
    CSS_COLUMN_RULE_COLOR_COLOR         = CSS_BACKGROUND_COLOR_COLOR,
    CSS_COLUMN_RULE_COLOR_CURRENT_COLOR = CSS_BACKGROUND_COLOR_CURRENT_COLOR
};

enum css_column_rule_style_e {
    CSS_COLUMN_RULE_STYLE_INHERIT = CSS_BORDER_STYLE_INHERIT,
    CSS_COLUMN_RULE_STYLE_NONE    = CSS_BORDER_STYLE_NONE,
    CSS_COLUMN_RULE_STYLE_HIDDEN  = CSS_BORDER_STYLE_HIDDEN,
    CSS_COLUMN_RULE_STYLE_DOTTED  = CSS_BORDER_STYLE_DOTTED,
    CSS_COLUMN_RULE_STYLE_DASHED  = CSS_BORDER_STYLE_DASHED,
    CSS_COLUMN_RULE_STYLE_SOLID   = CSS_BORDER_STYLE_SOLID,
    CSS_COLUMN_RULE_STYLE_DOUBLE  = CSS_BORDER_STYLE_DOUBLE,
    CSS_COLUMN_RULE_STYLE_GROOVE  = CSS_BORDER_STYLE_GROOVE,
    CSS_COLUMN_RULE_STYLE_RIDGE   = CSS_BORDER_STYLE_RIDGE,
    CSS_COLUMN_RULE_STYLE_INSET   = CSS_BORDER_STYLE_INSET,
    CSS_COLUMN_RULE_STYLE_OUTSET  = CSS_BORDER_STYLE_OUTSET
};

enum css_column_rule_width_e {
    CSS_COLUMN_RULE_WIDTH_INHERIT = CSS_BORDER_WIDTH_INHERIT,
    CSS_COLUMN_RULE_WIDTH_THIN    = CSS_BORDER_WIDTH_THIN,
    CSS_COLUMN_RULE_WIDTH_MEDIUM  = CSS_BORDER_WIDTH_MEDIUM,
    CSS_COLUMN_RULE_WIDTH_THICK   = CSS_BORDER_WIDTH_THICK,
    CSS_COLUMN_RULE_WIDTH_WIDTH   = CSS_BORDER_WIDTH_WIDTH
};

enum css_column_span_e {
    CSS_COLUMN_SPAN_INHERIT = 0x0,
    CSS_COLUMN_SPAN_NONE    = 0x1,
    CSS_COLUMN_SPAN_ALL     = 0x2
};

enum css_column_width_e {
    CSS_COLUMN_WIDTH_INHERIT = 0x0,
    CSS_COLUMN_WIDTH_SET     = 0x1,
    CSS_COLUMN_WIDTH_AUTO    = 0x2
};

enum css_content_e {
    CSS_CONTENT_INHERIT = 0x0,
    CSS_CONTENT_NONE    = 0x1,
    CSS_CONTENT_NORMAL  = 0x2,
    CSS_CONTENT_SET     = 0x3
};

enum css_counter_increment_e {
    CSS_COUNTER_INCREMENT_INHERIT = 0x0,
    CSS_COUNTER_INCREMENT_NAMED   = 0x1,
    CSS_COUNTER_INCREMENT_NONE    = 0x1
};

enum css_counter_reset_e {
    CSS_COUNTER_RESET_INHERIT = 0x0,
    CSS_COUNTER_RESET_NAMED   = 0x1,
    CSS_COUNTER_RESET_NONE    = 0x1
};

enum css_cursor_e {
    CSS_CURSOR_INHERIT    = 0x00,
    CSS_CURSOR_AUTO       = 0x01,
    CSS_CURSOR_CROSSHAIR  = 0x02,
    CSS_CURSOR_DEFAULT    = 0x03,
    CSS_CURSOR_POINTER    = 0x04,
    CSS_CURSOR_MOVE       = 0x05,
    CSS_CURSOR_E_RESIZE   = 0x06,
    CSS_CURSOR_NE_RESIZE  = 0x07,
    CSS_CURSOR_NW_RESIZE  = 0x08,
    CSS_CURSOR_N_RESIZE   = 0x09,
    CSS_CURSOR_SE_RESIZE  = 0x0a,
    CSS_CURSOR_SW_RESIZE  = 0x0b,
    CSS_CURSOR_S_RESIZE   = 0x0c,
    CSS_CURSOR_W_RESIZE   = 0x0d,
    CSS_CURSOR_TEXT       = 0x0e,
    CSS_CURSOR_WAIT       = 0x0f,
    CSS_CURSOR_HELP       = 0x10,
    CSS_CURSOR_PROGRESS   = 0x11
};

enum css_direction_e {
    CSS_DIRECTION_INHERIT = 0x0,
    CSS_DIRECTION_LTR     = 0x1,
    CSS_DIRECTION_RTL     = 0x2
};

enum css_display_e {
    CSS_DISPLAY_INHERIT              = 0x00,
    CSS_DISPLAY_INLINE               = 0x01,
    CSS_DISPLAY_BLOCK                = 0x02,
    CSS_DISPLAY_LIST_ITEM            = 0x03,
    CSS_DISPLAY_RUN_IN               = 0x04,
    CSS_DISPLAY_INLINE_BLOCK         = 0x05,
    CSS_DISPLAY_TABLE                = 0x06,
    CSS_DISPLAY_INLINE_TABLE         = 0x07,
    CSS_DISPLAY_TABLE_ROW_GROUP      = 0x08,
    CSS_DISPLAY_TABLE_HEADER_GROUP   = 0x09,
    CSS_DISPLAY_TABLE_FOOTER_GROUP   = 0x0a,
    CSS_DISPLAY_TABLE_ROW            = 0x0b,
    CSS_DISPLAY_TABLE_COLUMN_GROUP   = 0x0c,
    CSS_DISPLAY_TABLE_COLUMN         = 0x0d,
    CSS_DISPLAY_TABLE_CELL           = 0x0e,
    CSS_DISPLAY_TABLE_CAPTION        = 0x0f,
    CSS_DISPLAY_NONE                 = 0x10,
    CSS_DISPLAY_FLEX                 = 0x11,
    CSS_DISPLAY_INLINE_FLEX          = 0x12,
    CSS_DISPLAY_GRID                 = 0x13,
    CSS_DISPLAY_INLINE_GRID          = 0x14
};

enum css_empty_cells_e {
    CSS_EMPTY_CELLS_INHERIT = 0x0,
    CSS_EMPTY_CELLS_SHOW    = 0x1,
    CSS_EMPTY_CELLS_HIDE    = 0x2
};

enum css_fill_opacity_e {
    CSS_FILL_OPACITY_INHERIT = 0x0,
    CSS_FILL_OPACITY_SET     = 0x1
};

enum css_flex_basis_e {
    CSS_FLEX_BASIS_INHERIT = 0x0,
    CSS_FLEX_BASIS_SET     = 0x1,
    CSS_FLEX_BASIS_AUTO    = 0x2,
    CSS_FLEX_BASIS_CONTENT = 0x3
};

enum css_flex_direction_e {
    CSS_FLEX_DIRECTION_INHERIT        = 0x0,
    CSS_FLEX_DIRECTION_ROW            = 0x1,
    CSS_FLEX_DIRECTION_ROW_REVERSE    = 0x2,
    CSS_FLEX_DIRECTION_COLUMN         = 0x3,
    CSS_FLEX_DIRECTION_COLUMN_REVERSE = 0x4
};

enum css_flex_grow_e {
    CSS_FLEX_GROW_INHERIT = 0x0,
    CSS_FLEX_GROW_SET     = 0x1
};

enum css_flex_shrink_e {
    CSS_FLEX_SHRINK_INHERIT = 0x0,
    CSS_FLEX_SHRINK_SET     = 0x1
};

enum css_flex_wrap_e {
    CSS_FLEX_WRAP_INHERIT       = 0x0,
    CSS_FLEX_WRAP_NOWRAP        = 0x1,
    CSS_FLEX_WRAP_WRAP          = 0x2,
    CSS_FLEX_WRAP_WRAP_REVERSE  = 0x3
};

enum css_float_e {
    CSS_FLOAT_INHERIT = 0x0,
    CSS_FLOAT_LEFT    = 0x1,
    CSS_FLOAT_RIGHT   = 0x2,
    CSS_FLOAT_NONE    = 0x3
};

enum css_font_family_e {
    CSS_FONT_FAMILY_INHERIT    = 0x0,
    CSS_FONT_FAMILY_SERIF      = 0x1,
    CSS_FONT_FAMILY_SANS_SERIF = 0x2,
    CSS_FONT_FAMILY_CURSIVE    = 0x3,
    CSS_FONT_FAMILY_FANTASY    = 0x4,
    CSS_FONT_FAMILY_MONOSPACE  = 0x5
};

enum css_font_size_e {
    CSS_FONT_SIZE_INHERIT    = 0x0,
    CSS_FONT_SIZE_XX_SMALL   = 0x1,
    CSS_FONT_SIZE_X_SMALL    = 0x2,
    CSS_FONT_SIZE_SMALL      = 0x3,
    CSS_FONT_SIZE_MEDIUM     = 0x4,
    CSS_FONT_SIZE_LARGE      = 0x5,
    CSS_FONT_SIZE_X_LARGE    = 0x6,
    CSS_FONT_SIZE_XX_LARGE   = 0x7,
    CSS_FONT_SIZE_LARGER     = 0x8,
    CSS_FONT_SIZE_SMALLER    = 0x9,
    CSS_FONT_SIZE_DIMENSION  = 0xa
};

enum css_font_style_e {
    CSS_FONT_STYLE_INHERIT = 0x0,
    CSS_FONT_STYLE_NORMAL  = 0x1,
    CSS_FONT_STYLE_ITALIC  = 0x2,
    CSS_FONT_STYLE_OBLIQUE = 0x3
};

enum css_font_variant_e {
    CSS_FONT_VARIANT_INHERIT    = 0x0,
    CSS_FONT_VARIANT_NORMAL     = 0x1,
    CSS_FONT_VARIANT_SMALL_CAPS = 0x2
};

enum css_font_weight_e {
    CSS_FONT_WEIGHT_INHERIT  = 0x0,
    CSS_FONT_WEIGHT_NORMAL   = 0x1,
    CSS_FONT_WEIGHT_BOLD     = 0x2,
    CSS_FONT_WEIGHT_BOLDER   = 0x3,
    CSS_FONT_WEIGHT_LIGHTER  = 0x4,
    CSS_FONT_WEIGHT_100      = 0x5,
    CSS_FONT_WEIGHT_200      = 0x6,
    CSS_FONT_WEIGHT_300      = 0x7,
    CSS_FONT_WEIGHT_400      = 0x8,
    CSS_FONT_WEIGHT_500      = 0x9,
    CSS_FONT_WEIGHT_600      = 0xa,
    CSS_FONT_WEIGHT_700      = 0xb,
    CSS_FONT_WEIGHT_800      = 0xc,
    CSS_FONT_WEIGHT_900      = 0xd
};

enum css_height_e {
    CSS_HEIGHT_INHERIT = 0x0,
    CSS_HEIGHT_SET     = 0x1,
    CSS_HEIGHT_AUTO    = 0x2
};

enum css_justify_content_e {
    CSS_JUSTIFY_CONTENT_INHERIT       = 0x0,
    CSS_JUSTIFY_CONTENT_FLEX_START    = 0x1,
    CSS_JUSTIFY_CONTENT_FLEX_END      = 0x2,
    CSS_JUSTIFY_CONTENT_CENTER        = 0x3,
    CSS_JUSTIFY_CONTENT_SPACE_BETWEEN = 0x4,
    CSS_JUSTIFY_CONTENT_SPACE_AROUND  = 0x5,
    CSS_JUSTIFY_CONTENT_SPACE_EVENLY  = 0x6
};

enum css_left_e {
    CSS_LEFT_INHERIT = 0x0,
    CSS_LEFT_SET     = 0x1,
    CSS_LEFT_AUTO    = 0x2
};

enum css_letter_spacing_e {
    CSS_LETTER_SPACING_INHERIT = CSS_COLUMN_GAP_INHERIT,
    CSS_LETTER_SPACING_SET     = CSS_COLUMN_GAP_SET,
    CSS_LETTER_SPACING_NORMAL  = CSS_COLUMN_GAP_NORMAL
};

enum css_line_height_e {
    CSS_LINE_HEIGHT_INHERIT   = 0x0,
    CSS_LINE_HEIGHT_NUMBER    = 0x1,
    CSS_LINE_HEIGHT_DIMENSION = 0x2,
    CSS_LINE_HEIGHT_NORMAL    = 0x3
};

enum css_list_style_image_e {
    CSS_LIST_STYLE_IMAGE_INHERIT = 0x0,
    CSS_LIST_STYLE_IMAGE_URI     = 0x1,
    CSS_LIST_STYLE_IMAGE_NONE    = 0x1
};

enum css_list_style_position_e {
    CSS_LIST_STYLE_POSITION_INHERIT = 0x0,
    CSS_LIST_STYLE_POSITION_INSIDE  = 0x1,
    CSS_LIST_STYLE_POSITION_OUTSIDE = 0x2
};

enum css_list_style_type_e {
    CSS_LIST_STYLE_TYPE_INHERIT                  = 0x0,
    CSS_LIST_STYLE_TYPE_DISC                     = 0x1,
    CSS_LIST_STYLE_TYPE_CIRCLE                   = 0x2,
    CSS_LIST_STYLE_TYPE_SQUARE                   = 0x3,
    CSS_LIST_STYLE_TYPE_DECIMAL                  = 0x4,
    CSS_LIST_STYLE_TYPE_DECIMAL_LEADING_ZERO     = 0x5,
    CSS_LIST_STYLE_TYPE_LOWER_ROMAN              = 0x6,
    CSS_LIST_STYLE_TYPE_UPPER_ROMAN              = 0x7,
    CSS_LIST_STYLE_TYPE_LOWER_GREEK              = 0x8,
    CSS_LIST_STYLE_TYPE_LOWER_LATIN              = 0x9,
    CSS_LIST_STYLE_TYPE_UPPER_LATIN              = 0xa,
    CSS_LIST_STYLE_TYPE_ARMENIAN                 = 0xb,
    CSS_LIST_STYLE_TYPE_GEORGIAN                 = 0xc,
    CSS_LIST_STYLE_TYPE_LOWER_ALPHA              = 0xd,
    CSS_LIST_STYLE_TYPE_UPPER_ALPHA              = 0xe,
    CSS_LIST_STYLE_TYPE_NONE                     = 0xf,
    CSS_LIST_STYLE_TYPE_BINARY                   = 0x10,
    CSS_LIST_STYLE_TYPE_OCTAL                    = 0x11,
    CSS_LIST_STYLE_TYPE_LOWER_HEXADECIMAL        = 0x12,
    CSS_LIST_STYLE_TYPE_UPPER_HEXADECIMAL        = 0x13,
    CSS_LIST_STYLE_TYPE_ARABIC_INDIC             = 0x14,
    CSS_LIST_STYLE_TYPE_LOWER_ARMENIAN           = 0x15,
    CSS_LIST_STYLE_TYPE_UPPER_ARMENIAN           = 0x16,
    CSS_LIST_STYLE_TYPE_BENGALI                  = 0x17,
    CSS_LIST_STYLE_TYPE_CAMBODIAN                = 0x18,
    CSS_LIST_STYLE_TYPE_KHMER                    = 0x19,
    CSS_LIST_STYLE_TYPE_CJK_DECIMAL              = 0x1a,
    CSS_LIST_STYLE_TYPE_DEVANAGARI               = 0x1b,
    CSS_LIST_STYLE_TYPE_GUJARATI                 = 0x1c,
    CSS_LIST_STYLE_TYPE_GURMUKHI                 = 0x1d,
    CSS_LIST_STYLE_TYPE_HEBREW                   = 0x1e,
    CSS_LIST_STYLE_TYPE_KANNADA                  = 0x1f,
    CSS_LIST_STYLE_TYPE_LAO                      = 0x20,
    CSS_LIST_STYLE_TYPE_MALAYALAM                = 0x21,
    CSS_LIST_STYLE_TYPE_MONGOLIAN                = 0x22,
    CSS_LIST_STYLE_TYPE_MYANMAR                  = 0x23,
    CSS_LIST_STYLE_TYPE_ORIYA                    = 0x24,
    CSS_LIST_STYLE_TYPE_PERSIAN                  = 0x25,
    CSS_LIST_STYLE_TYPE_TAMIL                    = 0x26,
    CSS_LIST_STYLE_TYPE_TELUGU                   = 0x27,
    CSS_LIST_STYLE_TYPE_THAI                     = 0x28,
    CSS_LIST_STYLE_TYPE_TIBETAN                  = 0x29,
    CSS_LIST_STYLE_TYPE_CJK_EARTHLY_BRANCH      = 0x2a,
    CSS_LIST_STYLE_TYPE_CJK_HEAVENLY_STEM        = 0x2b,
    CSS_LIST_STYLE_TYPE_HIAGANA                  = 0x2c,
    CSS_LIST_STYLE_TYPE_HIAGANA_IROHA            = 0x2d,
    CSS_LIST_STYLE_TYPE_KATAKANA                 = 0x2e,
    CSS_LIST_STYLE_TYPE_KATAKANA_IROHA           = 0x2f,
    CSS_LIST_STYLE_TYPE_JAPANESE_INFORMAL        = 0x30,
    CSS_LIST_STYLE_TYPE_JAPANESE_FORMAL          = 0x31,
    CSS_LIST_STYLE_TYPE_KOREAN_HANGUL_FORMAL     = 0x32,
    CSS_LIST_STYLE_TYPE_KOREAN_HANJA_INFORMAL    = 0x33,
    CSS_LIST_STYLE_TYPE_KOREAN_HANJA_FORMAL      = 0x34
};

enum css_margin_e {
    CSS_MARGIN_INHERIT = 0x0,
    CSS_MARGIN_SET     = 0x1,
    CSS_MARGIN_AUTO    = 0x2
};

enum css_max_height_e {
    CSS_MAX_HEIGHT_INHERIT = 0x0,
    CSS_MAX_HEIGHT_SET     = 0x1,
    CSS_MAX_HEIGHT_NONE    = 0x2
};

enum css_max_width_e {
    CSS_MAX_WIDTH_INHERIT = 0x0,
    CSS_MAX_WIDTH_SET     = 0x1,
    CSS_MAX_WIDTH_NONE    = 0x2
};

enum css_min_height_e {
    CSS_MIN_HEIGHT_INHERIT = 0x0,
    CSS_MIN_HEIGHT_SET     = 0x1,
    CSS_MIN_HEIGHT_AUTO    = 0x2
};

enum css_min_width_e {
    CSS_MIN_WIDTH_INHERIT = 0x0,
    CSS_MIN_WIDTH_SET     = 0x1,
    CSS_MIN_WIDTH_AUTO    = 0x2
};

enum css_opacity_e {
    CSS_OPACITY_INHERIT = 0x0,
    CSS_OPACITY_SET     = 0x1
};

enum css_order_e {
    CSS_ORDER_INHERIT = 0x0,
    CSS_ORDER_SET     = 0x1
};

enum css_outline_color_e {
    CSS_OUTLINE_COLOR_INHERIT       = CSS_BACKGROUND_COLOR_INHERIT,
    CSS_OUTLINE_COLOR_COLOR         = CSS_BACKGROUND_COLOR_COLOR,
    CSS_OUTLINE_COLOR_CURRENT_COLOR = CSS_BACKGROUND_COLOR_CURRENT_COLOR,
    CSS_OUTLINE_COLOR_INVERT        = 0x3
};

enum css_outline_style_e {
    CSS_OUTLINE_STYLE_INHERIT = CSS_BORDER_STYLE_INHERIT,
    CSS_OUTLINE_STYLE_NONE    = CSS_BORDER_STYLE_NONE,
    CSS_OUTLINE_STYLE_DOTTED  = CSS_BORDER_STYLE_DOTTED,
    CSS_OUTLINE_STYLE_DASHED  = CSS_BORDER_STYLE_DASHED,
    CSS_OUTLINE_STYLE_SOLID   = CSS_BORDER_STYLE_SOLID,
    CSS_OUTLINE_STYLE_DOUBLE  = CSS_BORDER_STYLE_DOUBLE,
    CSS_OUTLINE_STYLE_GROOVE  = CSS_BORDER_STYLE_GROOVE,
    CSS_OUTLINE_STYLE_RIDGE   = CSS_BORDER_STYLE_RIDGE,
    CSS_OUTLINE_STYLE_INSET   = CSS_BORDER_STYLE_INSET,
    CSS_OUTLINE_STYLE_OUTSET  = CSS_BORDER_STYLE_OUTSET
};

enum css_outline_width_e {
    CSS_OUTLINE_WIDTH_INHERIT = CSS_BORDER_WIDTH_INHERIT,
    CSS_OUTLINE_WIDTH_THIN    = CSS_BORDER_WIDTH_THIN,
    CSS_OUTLINE_WIDTH_MEDIUM  = CSS_BORDER_WIDTH_MEDIUM,
    CSS_OUTLINE_WIDTH_THICK   = CSS_BORDER_WIDTH_THICK,
    CSS_OUTLINE_WIDTH_WIDTH   = CSS_BORDER_WIDTH_WIDTH
};

enum css_overflow_e {
    CSS_OVERFLOW_INHERIT = 0x0,
    CSS_OVERFLOW_VISIBLE = 0x1,
    CSS_OVERFLOW_HIDDEN  = 0x2,
    CSS_OVERFLOW_SCROLL  = 0x3,
    CSS_OVERFLOW_AUTO    = 0x4
};

enum css_orphans_e {
    CSS_ORPHANS_INHERIT = 0x0,
    CSS_ORPHANS_SET     = 0x1
};

enum css_padding_e {
    CSS_PADDING_INHERIT = 0x0,
    CSS_PADDING_SET     = 0x1
};

enum css_page_break_after_e {
    CSS_PAGE_BREAK_AFTER_INHERIT = CSS_BREAK_AFTER_INHERIT,
    CSS_PAGE_BREAK_AFTER_AUTO    = CSS_BREAK_AFTER_AUTO,
    CSS_PAGE_BREAK_AFTER_AVOID   = CSS_BREAK_AFTER_AVOID,
    CSS_PAGE_BREAK_AFTER_ALWAYS  = CSS_BREAK_AFTER_ALWAYS,
    CSS_PAGE_BREAK_AFTER_LEFT    = CSS_BREAK_AFTER_LEFT,
    CSS_PAGE_BREAK_AFTER_RIGHT   = CSS_BREAK_AFTER_RIGHT
};

enum css_page_break_before_e {
    CSS_PAGE_BREAK_BEFORE_INHERIT = CSS_BREAK_AFTER_INHERIT,
    CSS_PAGE_BREAK_BEFORE_AUTO    = CSS_BREAK_AFTER_AUTO,
    CSS_PAGE_BREAK_BEFORE_AVOID   = CSS_BREAK_AFTER_AVOID,
    CSS_PAGE_BREAK_BEFORE_ALWAYS  = CSS_BREAK_AFTER_ALWAYS,
    CSS_PAGE_BREAK_BEFORE_LEFT    = CSS_BREAK_AFTER_LEFT,
    CSS_PAGE_BREAK_BEFORE_RIGHT   = CSS_BREAK_AFTER_RIGHT
};

enum css_page_break_inside_e {
    CSS_PAGE_BREAK_INSIDE_INHERIT = CSS_BREAK_AFTER_INHERIT,
    CSS_PAGE_BREAK_INSIDE_AUTO    = CSS_BREAK_AFTER_AUTO,
    CSS_PAGE_BREAK_INSIDE_AVOID   = CSS_BREAK_AFTER_AVOID
};

enum css_position_e {
    CSS_POSITION_INHERIT  = 0x0,
    CSS_POSITION_STATIC   = 0x1,
    CSS_POSITION_RELATIVE = 0x2,
    CSS_POSITION_ABSOLUTE = 0x3,
    CSS_POSITION_FIXED    = 0x4,
    CSS_POSITION_STICKY   = 0x5
};

enum css_quotes_e {
    CSS_QUOTES_INHERIT = 0x0,
    CSS_QUOTES_STRING  = 0x1,
    CSS_QUOTES_NONE    = 0x1
};

enum css_right_e {
    CSS_RIGHT_INHERIT = 0x0,
    CSS_RIGHT_SET     = 0x1,
    CSS_RIGHT_AUTO    = 0x2
};

enum css_stroke_opacity_e {
    CSS_STROKE_OPACITY_INHERIT = 0x0,
    CSS_STROKE_OPACITY_SET     = 0x1
};

enum css_table_layout_e {
    CSS_TABLE_LAYOUT_INHERIT = 0x0,
    CSS_TABLE_LAYOUT_AUTO    = 0x1,
    CSS_TABLE_LAYOUT_FIXED   = 0x2
};

enum css_text_align_e {
    CSS_TEXT_ALIGN_INHERIT             = 0x0,
    CSS_TEXT_ALIGN_INHERIT_IF_NON_MAGIC = 0x1,
    CSS_TEXT_ALIGN_LEFT                = 0x2,
    CSS_TEXT_ALIGN_RIGHT               = 0x3,
    CSS_TEXT_ALIGN_CENTER              = 0x4,
    CSS_TEXT_ALIGN_JUSTIFY             = 0x5,
    CSS_TEXT_ALIGN_DEFAULT             = 0x6,
    CSS_TEXT_ALIGN_LIBCSS_LEFT         = 0x7,
    CSS_TEXT_ALIGN_LIBCSS_CENTER       = 0x8,
    CSS_TEXT_ALIGN_LIBCSS_RIGHT        = 0x9
};

enum css_text_decoration_e {
    CSS_TEXT_DECORATION_INHERIT      = 0x00,
    CSS_TEXT_DECORATION_NONE         = 0x10,
    CSS_TEXT_DECORATION_BLINK        = (1<<3),
    CSS_TEXT_DECORATION_LINE_THROUGH = (1<<2),
    CSS_TEXT_DECORATION_OVERLINE     = (1<<1),
    CSS_TEXT_DECORATION_UNDERLINE    = (1<<0)
};

enum css_text_indent_e {
    CSS_TEXT_INDENT_INHERIT = 0x0,
    CSS_TEXT_INDENT_SET     = 0x1
};

enum css_text_transform_e {
    CSS_TEXT_TRANSFORM_INHERIT    = 0x0,
    CSS_TEXT_TRANSFORM_CAPITALIZE = 0x1,
    CSS_TEXT_TRANSFORM_UPPERCASE  = 0x2,
    CSS_TEXT_TRANSFORM_LOWERCASE  = 0x3,
    CSS_TEXT_TRANSFORM_NONE       = 0x4
};

enum css_top_e {
    CSS_TOP_INHERIT = 0x0,
    CSS_TOP_SET     = 0x1,
    CSS_TOP_AUTO    = 0x2
};

enum css_unicode_bidi_e {
    CSS_UNICODE_BIDI_INHERIT       = 0x0,
    CSS_UNICODE_BIDI_NORMAL        = 0x1,
    CSS_UNICODE_BIDI_EMBED         = 0x2,
    CSS_UNICODE_BIDI_BIDI_OVERRIDE = 0x3
};

enum css_vertical_align_e {
    CSS_VERTICAL_ALIGN_INHERIT     = 0x0,
    CSS_VERTICAL_ALIGN_BASELINE    = 0x1,
    CSS_VERTICAL_ALIGN_SUB         = 0x2,
    CSS_VERTICAL_ALIGN_SUPER       = 0x3,
    CSS_VERTICAL_ALIGN_TOP         = 0x4,
    CSS_VERTICAL_ALIGN_TEXT_TOP    = 0x5,
    CSS_VERTICAL_ALIGN_MIDDLE      = 0x6,
    CSS_VERTICAL_ALIGN_BOTTOM      = 0x7,
    CSS_VERTICAL_ALIGN_TEXT_BOTTOM = 0x8,
    CSS_VERTICAL_ALIGN_SET         = 0x9
};

enum css_visibility_e {
    CSS_VISIBILITY_INHERIT  = 0x0,
    CSS_VISIBILITY_VISIBLE  = 0x1,
    CSS_VISIBILITY_HIDDEN   = 0x2,
    CSS_VISIBILITY_COLLAPSE = 0x3
};

enum css_white_space_e {
    CSS_WHITE_SPACE_INHERIT   = 0x0,
    CSS_WHITE_SPACE_NORMAL    = 0x1,
    CSS_WHITE_SPACE_PRE       = 0x2,
    CSS_WHITE_SPACE_NOWRAP    = 0x3,
    CSS_WHITE_SPACE_PRE_WRAP  = 0x4,
    CSS_WHITE_SPACE_PRE_LINE  = 0x5
};

enum css_widows_e {
    CSS_WIDOWS_INHERIT = 0x0,
    CSS_WIDOWS_SET     = 0x1
};

enum css_width_e {
    CSS_WIDTH_INHERIT = 0x0,
    CSS_WIDTH_SET     = 0x1,
    CSS_WIDTH_AUTO    = 0x2
};

enum css_word_spacing_e {
    CSS_WORD_SPACING_INHERIT = CSS_COLUMN_GAP_INHERIT,
    CSS_WORD_SPACING_SET     = CSS_COLUMN_GAP_SET,
    CSS_WORD_SPACING_NORMAL  = CSS_COLUMN_GAP_NORMAL
};

enum css_writing_mode_e {
    CSS_WRITING_MODE_INHERIT       = 0x0,
    CSS_WRITING_MODE_HORIZONTAL_TB = 0x1,
    CSS_WRITING_MODE_VERTICAL_RL   = 0x2,
    CSS_WRITING_MODE_VERTICAL_LR   = 0x3
};

enum css_z_index_e {
    CSS_Z_INDEX_INHERIT = 0x0,
    CSS_Z_INDEX_SET     = 0x1,
    CSS_Z_INDEX_AUTO    = 0x2
};

/* =========================================================================
 * 7. Font face types (from font_face.h)
 * ========================================================================= */

typedef enum css_font_face_format {
    CSS_FONT_FACE_FORMAT_UNSPECIFIED        = 0x00,
    CSS_FONT_FACE_FORMAT_WOFF               = 0x01,
    CSS_FONT_FACE_FORMAT_OPENTYPE           = 0x02,
    CSS_FONT_FACE_FORMAT_EMBEDDED_OPENTYPE  = 0x04,
    CSS_FONT_FACE_FORMAT_SVG                = 0x08,
    CSS_FONT_FACE_FORMAT_UNKNOWN            = 0x10
} css_font_face_format;

typedef enum css_font_face_location_type {
    CSS_FONT_FACE_LOCATION_TYPE_UNSPECIFIED = 0,
    CSS_FONT_FACE_LOCATION_TYPE_LOCAL       = 1,
    CSS_FONT_FACE_LOCATION_TYPE_URI         = 2
} css_font_face_location_type;

extern css_error css_font_face_get_font_family(
        const css_font_face *font_face,
        lwc_string **font_family);

extern css_error css_font_face_count_srcs(const css_font_face *font_face,
        uint32_t *count);
extern css_error css_font_face_get_src(const css_font_face *font_face,
        uint32_t index, const css_font_face_src **src);

extern css_error css_font_face_src_get_location(const css_font_face_src *src,
        lwc_string **location);

extern css_font_face_location_type css_font_face_src_location_type(
        const css_font_face_src *src);
extern css_font_face_format css_font_face_src_format(
        const css_font_face_src *src);

extern uint8_t css_font_face_font_style(const css_font_face *font_face);
extern uint8_t css_font_face_font_weight(const css_font_face *font_face);

/* =========================================================================
 * 8. Computed style types (from computed.h)
 * ========================================================================= */

typedef struct css_computed_counter {
    lwc_string *name;
    css_fixed   value;
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
    CSS_COMPUTED_CONTENT_NONE          = 0,
    CSS_COMPUTED_CONTENT_STRING        = 1,
    CSS_COMPUTED_CONTENT_URI           = 2,
    CSS_COMPUTED_CONTENT_COUNTER       = 3,
    CSS_COMPUTED_CONTENT_COUNTERS      = 4,
    CSS_COMPUTED_CONTENT_ATTR          = 5,
    CSS_COMPUTED_CONTENT_OPEN_QUOTE    = 6,
    CSS_COMPUTED_CONTENT_CLOSE_QUOTE   = 7,
    CSS_COMPUTED_CONTENT_NO_OPEN_QUOTE = 8,
    CSS_COMPUTED_CONTENT_NO_CLOSE_QUOTE = 9
};

typedef struct css_computed_content_item {
    uint8_t type;
    union {
        lwc_string *string;
        lwc_string *uri;
        lwc_string *attr;
        struct {
            lwc_string *name;
            uint8_t     style;
        } counter;
        struct {
            lwc_string *name;
            lwc_string *sep;
            uint8_t     style;
        } counters;
    } data;
} css_computed_content_item;

/* =========================================================================
 * 9. Computed style functions (from computed.h)
 * ========================================================================= */

extern css_error css_computed_style_destroy(css_computed_style *style);

extern css_error css_computed_style_compose(
        const css_computed_style *parent,
        const css_computed_style *child,
        const css_unit_ctx *unit_ctx,
        css_computed_style **result);

extern css_error css_computed_format_list_style(
        const css_computed_style *style,
        int value,
        char *buffer,
        size_t buffer_length,
        size_t *format_length);

extern uint8_t css_computed_letter_spacing(
        const css_computed_style *style,
        css_fixed *length, css_unit *unit);

extern uint8_t css_computed_outline_color(
        const css_computed_style *style, css_color *color);

extern uint8_t css_computed_outline_width(
        const css_computed_style *style,
        css_fixed *length, css_unit *unit);

extern uint8_t css_computed_border_spacing(
        const css_computed_style *style,
        css_fixed *hlength, css_unit *hunit,
        css_fixed *vlength, css_unit *vunit);

extern uint8_t css_computed_word_spacing(
        const css_computed_style *style,
        css_fixed *length, css_unit *unit);

extern uint8_t css_computed_counter_increment(
        const css_computed_style *style,
        const css_computed_counter **counters);

extern uint8_t css_computed_counter_reset(
        const css_computed_style *style,
        const css_computed_counter **counters);

extern uint8_t css_computed_cursor(
        const css_computed_style *style,
        lwc_string ***urls);

extern uint8_t css_computed_clip(
        const css_computed_style *style,
        css_computed_clip_rect *rect);

extern uint8_t css_computed_content(
        const css_computed_style *style,
        const css_computed_content_item **content);

extern uint8_t css_computed_vertical_align(
        const css_computed_style *style,
        css_fixed *length, css_unit *unit);

extern uint8_t css_computed_font_size(
        const css_computed_style *style,
        css_fixed *length, css_unit *unit);

extern uint8_t css_computed_border_top_width(
        const css_computed_style *style,
        css_fixed *length, css_unit *unit);

extern uint8_t css_computed_border_right_width(
        const css_computed_style *style,
        css_fixed *length, css_unit *unit);

extern uint8_t css_computed_border_bottom_width(
        const css_computed_style *style,
        css_fixed *length, css_unit *unit);

extern uint8_t css_computed_border_left_width(
        const css_computed_style *style,
        css_fixed *length, css_unit *unit);

extern uint8_t css_computed_background_image(
        const css_computed_style *style,
        lwc_string **url);

extern uint8_t css_computed_color(
        const css_computed_style *style,
        css_color *color);

extern uint8_t css_computed_list_style_image(
        const css_computed_style *style,
        lwc_string **url);

extern uint8_t css_computed_quotes(
        const css_computed_style *style,
        lwc_string ***quotes);

extern uint8_t css_computed_top(
        const css_computed_style *style,
        css_fixed *length, css_unit *unit);

extern uint8_t css_computed_right(
        const css_computed_style *style,
        css_fixed *length, css_unit *unit);

extern uint8_t css_computed_bottom(
        const css_computed_style *style,
        css_fixed *length, css_unit *unit);

extern uint8_t css_computed_left(
        const css_computed_style *style,
        css_fixed *length, css_unit *unit);

extern uint8_t css_computed_border_top_color(
        const css_computed_style *style,
        css_color *color);

extern uint8_t css_computed_border_right_color(
        const css_computed_style *style,
        css_color *color);

extern uint8_t css_computed_border_bottom_color(
        const css_computed_style *style,
        css_color *color);

extern uint8_t css_computed_border_left_color(
        const css_computed_style *style,
        css_color *color);

extern uint8_t css_computed_box_sizing(
        const css_computed_style *style);

extern uint8_t css_computed_height(
        const css_computed_style *style,
        css_fixed *length, css_unit *unit);

extern uint8_t css_computed_line_height(
        const css_computed_style *style,
        css_fixed *length, css_unit *unit);

extern uint8_t css_computed_background_color(
        const css_computed_style *style,
        css_color *color);

extern uint8_t css_computed_z_index(
        const css_computed_style *style,
        int32_t *z_index);

extern uint8_t css_computed_margin_top(
        const css_computed_style *style,
        css_fixed *length, css_unit *unit);

extern uint8_t css_computed_margin_right(
        const css_computed_style *style,
        css_fixed *length, css_unit *unit);

extern uint8_t css_computed_margin_bottom(
        const css_computed_style *style,
        css_fixed *length, css_unit *unit);

extern uint8_t css_computed_margin_left(
        const css_computed_style *style,
        css_fixed *length, css_unit *unit);

extern uint8_t css_computed_background_attachment(
        const css_computed_style *style);

extern uint8_t css_computed_border_collapse(
        const css_computed_style *style);

extern uint8_t css_computed_caption_side(
        const css_computed_style *style);

extern uint8_t css_computed_direction(
        const css_computed_style *style);

extern uint8_t css_computed_max_height(
        const css_computed_style *style,
        css_fixed *length, css_unit *unit);

extern uint8_t css_computed_max_width(
        const css_computed_style *style,
        css_fixed *length, css_unit *unit);

extern uint8_t css_computed_width_px(
        const css_computed_style *style,
        const css_unit_ctx *unit_ctx,
        int available_px,
        int *px_out);

extern uint8_t css_computed_width(const css_computed_style *style,
        css_fixed *length, css_unit *unit);

extern uint8_t css_computed_empty_cells(
        const css_computed_style *style);

extern uint8_t css_computed_float(
        const css_computed_style *style);

extern uint8_t css_computed_writing_mode(
        const css_computed_style *style);

extern uint8_t css_computed_font_style(
        const css_computed_style *style);

extern uint8_t css_computed_min_height(
        const css_computed_style *style,
        css_fixed *length, css_unit *unit);

extern uint8_t css_computed_min_width(
        const css_computed_style *style,
        css_fixed *length, css_unit *unit);

extern uint8_t css_computed_background_repeat(
        const css_computed_style *style);

extern uint8_t css_computed_clear(
        const css_computed_style *style);

extern uint8_t css_computed_padding_top(
        const css_computed_style *style,
        css_fixed *length, css_unit *unit);

extern uint8_t css_computed_padding_right(
        const css_computed_style *style,
        css_fixed *length, css_unit *unit);

extern uint8_t css_computed_padding_bottom(
        const css_computed_style *style,
        css_fixed *length, css_unit *unit);

extern uint8_t css_computed_padding_left(
        const css_computed_style *style,
        css_fixed *length, css_unit *unit);

extern uint8_t css_computed_overflow_x(
        const css_computed_style *style);

extern uint8_t css_computed_overflow_y(
        const css_computed_style *style);

extern uint8_t css_computed_position(
        const css_computed_style *style);

extern uint8_t css_computed_opacity(
        const css_computed_style *style,
        css_fixed *opacity);

extern uint8_t css_computed_fill_opacity(
        const css_computed_style *style,
        css_fixed *fill_opacity);

extern uint8_t css_computed_stroke_opacity(
        const css_computed_style *style,
        css_fixed *stroke_opacity);

extern uint8_t css_computed_text_transform(
        const css_computed_style *style);

extern uint8_t css_computed_text_indent(
        const css_computed_style *style,
        css_fixed *length, css_unit *unit);

extern uint8_t css_computed_white_space(
        const css_computed_style *style);

extern uint8_t css_computed_background_position(
        const css_computed_style *style,
        css_fixed *hlength, css_unit *hunit,
        css_fixed *vlength, css_unit *vunit);

extern uint8_t css_computed_break_after(
        const css_computed_style *style);

extern uint8_t css_computed_break_before(
        const css_computed_style *style);

extern uint8_t css_computed_break_inside(
        const css_computed_style *style);

extern uint8_t css_computed_column_count(
        const css_computed_style *style,
        int32_t *column_count);

extern uint8_t css_computed_column_fill(
        const css_computed_style *style);

extern uint8_t css_computed_column_gap(
        const css_computed_style *style,
        css_fixed *length, css_unit *unit);

extern uint8_t css_computed_column_rule_color(
        const css_computed_style *style,
        css_color *color);

extern uint8_t css_computed_column_rule_style(
        const css_computed_style *style);

extern uint8_t css_computed_column_rule_width(
        const css_computed_style *style,
        css_fixed *length, css_unit *unit);

extern uint8_t css_computed_column_span(
        const css_computed_style *style);

extern uint8_t css_computed_column_width(
        const css_computed_style *style,
        css_fixed *length, css_unit *unit);

extern uint8_t css_computed_display(
        const css_computed_style *style, bool root);

extern uint8_t css_computed_display_static(
        const css_computed_style *style);

extern uint8_t css_computed_font_variant(
        const css_computed_style *style);

extern uint8_t css_computed_text_decoration(
        const css_computed_style *style);

extern uint8_t css_computed_font_family(
        const css_computed_style *style,
        lwc_string ***names);

extern uint8_t css_computed_border_top_style(
        const css_computed_style *style);

extern uint8_t css_computed_border_right_style(
        const css_computed_style *style);

extern uint8_t css_computed_border_bottom_style(
        const css_computed_style *style);

extern uint8_t css_computed_border_left_style(
        const css_computed_style *style);

extern uint8_t css_computed_font_weight(
        const css_computed_style *style);

extern uint8_t css_computed_list_style_type(
        const css_computed_style *style);

extern uint8_t css_computed_outline_style(
        const css_computed_style *style);

extern uint8_t css_computed_table_layout(
        const css_computed_style *style);

extern uint8_t css_computed_unicode_bidi(
        const css_computed_style *style);

extern uint8_t css_computed_visibility(
        const css_computed_style *style);

extern uint8_t css_computed_list_style_position(
        const css_computed_style *style);

extern uint8_t css_computed_text_align(
        const css_computed_style *style);

extern uint8_t css_computed_page_break_after(
        const css_computed_style *style);

extern uint8_t css_computed_page_break_before(
        const css_computed_style *style);

extern uint8_t css_computed_page_break_inside(
        const css_computed_style *style);

extern uint8_t css_computed_orphans(
        const css_computed_style *style,
        int32_t *orphans);

extern uint8_t css_computed_widows(
        const css_computed_style *style,
        int32_t *widows);

extern uint8_t css_computed_align_content(
        const css_computed_style *style);

extern uint8_t css_computed_align_items(
        const css_computed_style *style);

extern uint8_t css_computed_align_self(
        const css_computed_style *style);

extern uint8_t css_computed_flex_basis(
        const css_computed_style *style,
        css_fixed *length,
        css_unit *unit);

extern uint8_t css_computed_flex_direction(
        const css_computed_style *style);

extern uint8_t css_computed_flex_grow(
        const css_computed_style *style,
        css_fixed *number);

extern uint8_t css_computed_flex_shrink(
        const css_computed_style *style,
        css_fixed *number);

extern uint8_t css_computed_flex_wrap(
        const css_computed_style *style);

extern uint8_t css_computed_justify_content(
        const css_computed_style *style);

extern uint8_t css_computed_order(
        const css_computed_style *style,
        int32_t *order);

/* =========================================================================
 * 10. css_hint struct types (from hint.h)
 * ========================================================================= */

typedef struct css_hint_length {
    css_fixed value;
    css_unit  unit;
} css_hint_length;

typedef struct css_hint {
    union {
        css_computed_clip_rect    *clip;
        css_color                  color;
        css_computed_content_item *content;
        css_computed_counter      *counter;
        css_fixed                  fixed;
        int32_t                    integer;
        css_hint_length            length;
        struct {
            css_hint_length h;
            css_hint_length v;
        } position;
        lwc_string  *string;
        lwc_string **strings;
    } data;

    uint32_t prop;
    uint8_t  status;
} css_hint;

/* =========================================================================
 * 11. Function pointer typedefs (from stylesheet.h / functypes.h)
 * ========================================================================= */

typedef css_error (*css_url_resolution_fn)(void *pw,
        const char *base, lwc_string *rel, lwc_string **abs);

typedef css_error (*css_import_notification_fn)(void *pw,
        css_stylesheet *parent, lwc_string *url);

typedef css_error (*css_color_resolution_fn)(void *pw,
        lwc_string *name, css_color *color);

typedef css_fixed (*css_unit_len_measure)(
        void *pw,
        const css_computed_style *style,
        const css_unit unit);

typedef css_error (*css_font_resolution_fn)(void *pw,
        lwc_string *name, css_system_font *system_font);

/* =========================================================================
 * 12. css_system_font, stylesheet params (from stylesheet.h)
 * ========================================================================= */

struct css_system_font {
    enum css_font_style_e   style;
    enum css_font_variant_e variant;
    enum css_font_weight_e  weight;
    struct {
        css_fixed size;
        css_unit  unit;
    } size;
    struct {
        css_fixed size;
        css_unit  unit;
    } line_height;
    lwc_string *family;
};

typedef enum css_stylesheet_params_version {
    CSS_STYLESHEET_PARAMS_VERSION_1 = 1
} css_stylesheet_params_version;

typedef struct css_stylesheet_params {
    uint32_t             params_version;
    css_language_level   level;
    const char          *charset;
    const char          *url;
    const char          *title;
    bool                 allow_quirks;
    bool                 inline_style;
    css_url_resolution_fn   resolve;
    void                    *resolve_pw;
    css_import_notification_fn import;
    void                       *import_pw;
    css_color_resolution_fn  color;
    void                    *color_pw;
    css_font_resolution_fn   font;
    void                    *font_pw;
} css_stylesheet_params;

/* =========================================================================
 * 13. Stylesheet API functions (from stylesheet.h)
 * ========================================================================= */

extern css_error css_stylesheet_create(const css_stylesheet_params *params,
        css_stylesheet **stylesheet);
extern css_error css_stylesheet_destroy(css_stylesheet *sheet);

extern css_error css_stylesheet_append_data(css_stylesheet *sheet,
        const uint8_t *data, size_t len);
extern css_error css_stylesheet_data_done(css_stylesheet *sheet);

extern css_error css_stylesheet_next_pending_import(css_stylesheet *parent,
        lwc_string **url);
extern css_error css_stylesheet_register_import(css_stylesheet *parent,
        css_stylesheet *child);

extern css_error css_stylesheet_get_language_level(css_stylesheet *sheet,
        css_language_level *level);
extern css_error css_stylesheet_get_url(css_stylesheet *sheet,
        const char **url);
extern css_error css_stylesheet_get_title(css_stylesheet *sheet,
        const char **title);
extern css_error css_stylesheet_quirks_allowed(css_stylesheet *sheet,
        bool *allowed);
extern css_error css_stylesheet_used_quirks(css_stylesheet *sheet,
        bool *quirks);

extern css_error css_stylesheet_get_disabled(css_stylesheet *sheet,
        bool *disabled);
extern css_error css_stylesheet_set_disabled(css_stylesheet *sheet,
        bool disabled);

extern css_error css_stylesheet_size(css_stylesheet *sheet, size_t *size);

/* =========================================================================
 * 14. Select API types and functions (from select.h)
 * ========================================================================= */

typedef enum css_pseudo_element {
    CSS_PSEUDO_ELEMENT_NONE         = 0,
    CSS_PSEUDO_ELEMENT_FIRST_LINE   = 1,
    CSS_PSEUDO_ELEMENT_FIRST_LETTER = 2,
    CSS_PSEUDO_ELEMENT_BEFORE       = 3,
    CSS_PSEUDO_ELEMENT_AFTER        = 4,
    CSS_PSEUDO_ELEMENT_COUNT        = 5
} css_pseudo_element;

typedef struct css_select_results {
    css_computed_style *styles[CSS_PSEUDO_ELEMENT_COUNT];
} css_select_results;

typedef enum css_select_handler_version {
    CSS_SELECT_HANDLER_VERSION_1 = 1
} css_select_handler_version;

typedef struct css_select_handler {
    uint32_t handler_version;

    css_error (*node_name)(void *pw, void *node, css_qname *qname);
    css_error (*node_classes)(void *pw, void *node,
            lwc_string ***classes, uint32_t *n_classes);
    css_error (*node_id)(void *pw, void *node, lwc_string **id);

    css_error (*named_ancestor_node)(void *pw, void *node,
            const css_qname *qname, void **ancestor);
    css_error (*named_parent_node)(void *pw, void *node,
            const css_qname *qname, void **parent);
    css_error (*named_sibling_node)(void *pw, void *node,
            const css_qname *qname, void **sibling);
    css_error (*named_generic_sibling_node)(void *pw, void *node,
            const css_qname *qname, void **sibling);

    css_error (*parent_node)(void *pw, void *node, void **parent);
    css_error (*sibling_node)(void *pw, void *node, void **sibling);

    css_error (*node_has_name)(void *pw, void *node,
            const css_qname *qname, bool *match);
    css_error (*node_has_class)(void *pw, void *node,
            lwc_string *name, bool *match);
    css_error (*node_has_id)(void *pw, void *node,
            lwc_string *name, bool *match);
    css_error (*node_has_attribute)(void *pw, void *node,
            const css_qname *qname, bool *match);
    css_error (*node_has_attribute_equal)(void *pw, void *node,
            const css_qname *qname, lwc_string *value, bool *match);
    css_error (*node_has_attribute_dashmatch)(void *pw, void *node,
            const css_qname *qname, lwc_string *value, bool *match);
    css_error (*node_has_attribute_includes)(void *pw, void *node,
            const css_qname *qname, lwc_string *value, bool *match);
    css_error (*node_has_attribute_prefix)(void *pw, void *node,
            const css_qname *qname, lwc_string *value, bool *match);
    css_error (*node_has_attribute_suffix)(void *pw, void *node,
            const css_qname *qname, lwc_string *value, bool *match);
    css_error (*node_has_attribute_substring)(void *pw, void *node,
            const css_qname *qname, lwc_string *value, bool *match);

    css_error (*node_is_root)(void *pw, void *node, bool *match);
    css_error (*node_count_siblings)(void *pw, void *node,
            bool same_name, bool after, int32_t *count);
    css_error (*node_is_empty)(void *pw, void *node, bool *match);

    css_error (*node_is_link)(void *pw, void *node, bool *match);
    css_error (*node_is_visited)(void *pw, void *node, bool *match);
    css_error (*node_is_hover)(void *pw, void *node, bool *match);
    css_error (*node_is_active)(void *pw, void *node, bool *match);
    css_error (*node_is_focus)(void *pw, void *node, bool *match);

    css_error (*node_is_enabled)(void *pw, void *node, bool *match);
    css_error (*node_is_disabled)(void *pw, void *node, bool *match);
    css_error (*node_is_checked)(void *pw, void *node, bool *match);

    css_error (*node_is_target)(void *pw, void *node, bool *match);
    css_error (*node_is_lang)(void *pw, void *node,
            lwc_string *lang, bool *match);

    css_error (*node_presentational_hint)(void *pw, void *node,
            uint32_t *nhints, css_hint **hints);

    css_error (*ua_default_for_property)(void *pw, uint32_t property,
            css_hint *hint);

    css_error (*set_libcss_node_data)(void *pw, void *node,
            void *libcss_node_data);
    css_error (*get_libcss_node_data)(void *pw, void *node,
            void **libcss_node_data);
} css_select_handler;

typedef struct css_select_font_faces_results {
    css_font_face **font_faces;
    uint32_t        n_font_faces;
} css_select_font_faces_results;

typedef enum {
    CSS_NODE_DELETED,
    CSS_NODE_MODIFIED,
    CSS_NODE_ANCESTORS_MODIFIED,
    CSS_NODE_CLONED
} css_node_data_action;

extern css_error css_libcss_node_data_handler(css_select_handler *handler,
        css_node_data_action action, void *pw, void *node,
        void *clone_node, void *libcss_node_data);

extern css_error css_select_ctx_create(css_select_ctx **result);
extern css_error css_select_ctx_destroy(css_select_ctx *ctx);

extern css_error css_select_ctx_append_sheet(css_select_ctx *ctx,
        const css_stylesheet *sheet,
        css_origin origin, const char *media);
extern css_error css_select_ctx_insert_sheet(css_select_ctx *ctx,
        const css_stylesheet *sheet, uint32_t index,
        css_origin origin, const char *media);
extern css_error css_select_ctx_remove_sheet(css_select_ctx *ctx,
        const css_stylesheet *sheet);

extern css_error css_select_ctx_count_sheets(css_select_ctx *ctx,
        uint32_t *count);
extern css_error css_select_ctx_get_sheet(css_select_ctx *ctx,
        uint32_t index, const css_stylesheet **sheet);

extern css_error css_select_default_style(css_select_ctx *ctx,
        css_select_handler *handler, void *pw,
        css_computed_style **style);
extern css_error css_select_style(css_select_ctx *ctx, void *node,
        const css_unit_ctx *unit_ctx,
        const css_media *media, const css_stylesheet *inline_style,
        css_select_handler *handler, void *pw,
        css_select_results **result);
extern css_error css_select_results_destroy(css_select_results *results);

extern css_error css_select_font_faces(css_select_ctx *ctx,
        const css_media *media,
        const css_unit_ctx *unit_ctx,
        lwc_string *font_family,
        css_select_font_faces_results **result);
extern css_error css_select_font_faces_results_destroy(
        css_select_font_faces_results *results);

/* =========================================================================
 * 15. Unit API (from unit.h)
 * ========================================================================= */

struct css_unit_ctx {
    css_fixed                  viewport_width;
    css_fixed                  viewport_height;
    css_fixed                  font_size_default;
    css_fixed                  font_size_minimum;
    css_fixed                  device_dpi;
    const css_computed_style  *root_style;
    void                      *pw;
    const css_unit_len_measure  measure;
};

static css_fixed css_unit_css2device_px(
        const css_fixed css_pixels,
        const css_fixed device_dpi)
{
    return FDIV(FMUL(css_pixels, device_dpi), F_96);
}

static css_fixed css_unit_device2css_px(
        const css_fixed device_pixels,
        const css_fixed device_dpi)
{
    return FDIV(FMUL(device_pixels, F_96), device_dpi);
}

extern css_fixed css_unit_font_size_len2pt(
        const css_computed_style *style,
        const css_unit_ctx *ctx,
        const css_fixed length,
        const css_unit unit);

extern css_fixed css_unit_len2css_px(
        const css_computed_style *style,
        const css_unit_ctx *ctx,
        const css_fixed length,
        const css_unit unit);

extern css_fixed css_unit_len2device_px(
        const css_computed_style *style,
        const css_unit_ctx *ctx,
        const css_fixed length,
        const css_unit unit);

#endif /* MACOS9_LIBCSS_LIBCSS_H */
