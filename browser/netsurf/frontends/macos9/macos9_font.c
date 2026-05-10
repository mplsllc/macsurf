/*
 * MacSurf - macos9_font.c
 * gui_layout_table callbacks.
 *
 * v0.5 rewrite: 
 *   - Use FrontWindow() fallback for layout port if initial_win is NULL.
 *   - Linear scan for position/split (more reliable than UTF-8 binary search).
 *   - Robust UTF-8 handling.
 */

#include <stdlib.h>
#include <string.h>

#include "utils/errors.h"
#include "utils/log.h"
#include "utils/utf8.h"
#include "netsurf/plot_style.h"
#include "netsurf/layout.h"

#include "macos9.h"
#include <libwapcaplet/libwapcaplet.h>

/* Heuristic constants for non-Mac builds */
#define CW_MONO_NUM  614   /* 0.60 * 1024 */
#define CW_PROP_NUM  533   /* 0.52 * 1024 */
#define CW_DEN       1024

static int
mac_char_width_heuristic(const struct plot_font_style *fstyle)
{
        int size_px;
        int num;
        int w;

        if (fstyle == NULL)
                size_px = 12;
        else
                size_px = (int)(fstyle->size >> PLOT_STYLE_RADIX);
        if (size_px <= 0) size_px = 12;

        if (fstyle != NULL &&
            fstyle->family == PLOT_FONT_FAMILY_MONOSPACE)
                num = CW_MONO_NUM;
        else
                num = CW_PROP_NUM;

        /* Add 15% for bold if we can't measure it */
        if (fstyle != NULL && fstyle->weight >= 600)
                num = (num * 1177 + 512) / 1024;

        w = (size_px * num + CW_DEN / 2) / CW_DEN;
        if (w < 1) w = 1;
        return w;
}

size_t
macos9_utf8_to_macroman(const char *utf8, size_t len, char *mac_out, size_t max_out)
{
        size_t out_len = 0;
        size_t i = 0;

        while (i < len && out_len < max_out) {
                size_t char_len = utf8_char_byte_length(utf8 + i);
                uint32_t ucs4;

                if (char_len == 0 || i + char_len > len) {
                        mac_out[out_len++] = '?';
                        i++;
                        continue;
                }
                
                ucs4 = utf8_to_ucs4(utf8 + i, char_len);
                i += char_len;
                
                if (ucs4 < 0x80) {
                        mac_out[out_len++] = (char)ucs4;
                } else {
                        /* Basic MacRoman mapping for common characters */
                        switch (ucs4) {
                                case 0x00A0: mac_out[out_len++] = (char)0xCA; break; /* NBSP */
                                case 0x00A3: mac_out[out_len++] = (char)0xA3; break; /* Pound */
                                case 0x00A9: mac_out[out_len++] = (char)0xA9; break; /* Copyright */
                                case 0x00AE: mac_out[out_len++] = (char)0xA8; break; /* Registered */
                                case 0x2013: mac_out[out_len++] = (char)0xD0; break; /* En dash */
                                case 0x2014: mac_out[out_len++] = (char)0xD1; break; /* Em dash */
                                case 0x2018: mac_out[out_len++] = (char)0xD4; break; /* Left single quote */
                                case 0x2019: mac_out[out_len++] = (char)0xD5; break; /* Right single quote */
                                case 0x201C: mac_out[out_len++] = (char)0xD2; break; /* Left double quote */
                                case 0x201D: mac_out[out_len++] = (char)0xD3; break; /* Right double quote */
                                case 0x2022: mac_out[out_len++] = (char)0xA5; break; /* Bullet */
                                case 0x2026: mac_out[out_len++] = (char)0xC9; break; /* Ellipsis */
                                case 0x20AC: mac_out[out_len++] = (char)0xDB; break; /* Euro */
                                case 0x2122: mac_out[out_len++] = (char)0xAA; break; /* Trademark */
                                default:     mac_out[out_len++] = '?'; break;
                        }
                }
        }
        return out_len;
}

#ifdef __MACOS9__
static int
macos9_font_measure(const struct plot_font_style *fstyle,
                    const char *string,
                    size_t length)
{
        short font_id;
        short size;
        short face;
        GrafPtr old_port;
        int width;
        char mac_str[4096];
        size_t mac_len;
        Boolean changed_port = false;

        if (string == NULL || length == 0)
                return 0;

        mac_len = macos9_utf8_to_macroman(string, length, mac_str, sizeof(mac_str));

        font_id = macos9_font_id_from_style(fstyle);
        face = macos9_face_from_style(fstyle);
        size = (short)(fstyle->size >> PLOT_STYLE_RADIX);
        if (size <= 0) size = 12;

        GetPort(&old_port);
        /* Only switch ports if the current one isn't a window we can use. 
         * This avoids thrashing the port state during redraw. */
        if (old_port == NULL) {
                if (initial_win != NULL && initial_win->window != NULL) {
                        SetPortWindowPort(initial_win->window);
                        changed_port = true;
                } else if (FrontWindow() != NULL) {
                        SetPortWindowPort(FrontWindow());
                        changed_port = true;
                }
        }

        TextFont(font_id);
        TextSize(size);
        TextFace(face);

        width = TextWidth(mac_str, 0, (short)mac_len);

        if (changed_port)
                SetPort(old_port);
        return width;
}
#endif

static nserror
macos9_font_width(const struct plot_font_style *fstyle,
                  const char *string,
                  size_t length,
                  int *width)
{
        if (string == NULL || length == 0) {
                *width = 0;
                return NSERROR_OK;
        }

#ifdef __MACOS9__
        *width = macos9_font_measure(fstyle, string, length);
#else
        {
                int cw = mac_char_width_heuristic(fstyle);
                *width = (int)length * cw;
        }
#endif
        return NSERROR_OK;
}

static nserror
macos9_font_position(const struct plot_font_style *fstyle,
                     const char *string,
                     size_t length,
                     int x,
                     size_t *char_offset,
                     int *actual_x)
{
        int last_x = 0;
        size_t i = 0;

        if (string == NULL || length == 0) {
                *char_offset = 0;
                *actual_x = 0;
                return NSERROR_OK;
        }

        if (x <= 0) {
                *char_offset = 0;
                *actual_x = 0;
                return NSERROR_OK;
        }

#ifdef __MACOS9__
        while (i < length) {
                size_t next_i = utf8_next(string, length, i);
                int w;
                if (next_i == i) break;

                w = macos9_font_measure(fstyle, string, next_i);
                if (w > x) {
                        /* This char made it too wide. Return last good one. */
                        if (i == 0) {
                                /* Even the first char doesn't fit, but we must return >= 1 
                                 * per NetSurf specs if we are in 'split'. For 'position' 
                                 * we can return 0. */
                                *char_offset = 0;
                                *actual_x = 0;
                        } else {
                                *char_offset = i;
                                *actual_x = last_x;
                        }
                        return NSERROR_OK;
                }
                last_x = w;
                i = next_i;
        }
        *char_offset = i;
        *actual_x = last_x;
#else
        {
                int cw = mac_char_width_heuristic(fstyle);
                size_t n;
                if (cw <= 0) cw = 1;
                n = (size_t)(x / cw);
                if (n > length) n = length;
                *char_offset = n;
                *actual_x = (int)n * cw;
        }
#endif
        return NSERROR_OK;
}

static nserror
macos9_font_split(const struct plot_font_style *fstyle,
                  const char *string,
                  size_t length,
                  int x,
                  size_t *char_offset,
                  int *actual_x)
{
        size_t fit_offset;
        int fit_x;
        size_t last_space = 0;
        int have_space = 0;
        size_t i;

        if (string == NULL || length == 0) {
                *char_offset = 0;
                *actual_x = 0;
                return NSERROR_OK;
        }

        macos9_font_position(fstyle, string, length, x, &fit_offset, &fit_x);

        /* If the entire string fits within the width, do not split it at all. */
        if (fit_offset == length) {
                *char_offset = length;
                *actual_x = fit_x;
                return NSERROR_OK;
        }

        /* The string overflows. Find the last space character that FITS. */
        for (i = 0; i < fit_offset; i++) {
                if (string[i] == ' ') {
                        last_space = i;
                        have_space = 1;
                }
        }

        if (have_space) {
                /* Return the character AFTER the space as the start of the next line.
                 * This "consumes" the space at the end of the current line. */
                *char_offset = last_space + 1;
                macos9_font_width(fstyle, string, last_space, actual_x);
        } else {
                /* No space found in the part that fits. Force a hard break. */
                if (fit_offset == 0) {
                        /* Force at least one character even if it doesn't fit. */
                        size_t first_char_len = utf8_next(string, length, 0);
                        if (first_char_len == 0 || first_char_len > length) 
                                first_char_len = 1;
                        *char_offset = first_char_len;
                        macos9_font_width(fstyle, string, first_char_len, actual_x);
                } else {
                        *char_offset = fit_offset;
                        *actual_x = fit_x;
                }
        }

        return NSERROR_OK;
}

/* Field order: width, position, split (see include/netsurf/layout.h) */
static struct gui_layout_table layout_table = {
        macos9_font_width,
        macos9_font_position,
        macos9_font_split
};

struct gui_layout_table *macos9_layout_table = &layout_table;
