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
#include "macsurf_debug_log.h"
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
                        /* Basic MacRoman mapping for common characters.
                         * MacRoman has no native circle or square outline
                         * glyphs, so we fall back to ASCII approximations
                         * for list-style-type markers (circle, square). */
                        switch (ucs4) {
                                case 0x00A0: mac_out[out_len++] = (char)0xCA; break; /* NBSP */
                                case 0x00A3: mac_out[out_len++] = (char)0xA3; break; /* Pound */
                                case 0x00A9: mac_out[out_len++] = (char)0xA9; break; /* Copyright */
                                case 0x00AE: mac_out[out_len++] = (char)0xA8; break; /* Registered */
                                case 0x00B0: mac_out[out_len++] = (char)0xA1; break; /* Degree */
                                case 0x00B1: mac_out[out_len++] = (char)0xB1; break; /* Plus-minus */
                                case 0x00B7: mac_out[out_len++] = (char)0xE1; break; /* Middle dot */
                                case 0x00BC: mac_out[out_len++] = '1';
                                             if (out_len < max_out) mac_out[out_len++] = '/';
                                             if (out_len < max_out) mac_out[out_len++] = '4';
                                             break; /* 1/4 */
                                case 0x00BD: mac_out[out_len++] = '1';
                                             if (out_len < max_out) mac_out[out_len++] = '/';
                                             if (out_len < max_out) mac_out[out_len++] = '2';
                                             break; /* 1/2 */
                                case 0x00BE: mac_out[out_len++] = '3';
                                             if (out_len < max_out) mac_out[out_len++] = '/';
                                             if (out_len < max_out) mac_out[out_len++] = '4';
                                             break; /* 3/4 */
                                case 0x2013: mac_out[out_len++] = (char)0xD0; break; /* En dash */
                                case 0x2014: mac_out[out_len++] = (char)0xD1; break; /* Em dash */
                                case 0x2018: mac_out[out_len++] = (char)0xD4; break; /* Left single quote */
                                case 0x2019: mac_out[out_len++] = (char)0xD5; break; /* Right single quote */
                                case 0x201C: mac_out[out_len++] = (char)0xD2; break; /* Left double quote */
                                case 0x201D: mac_out[out_len++] = (char)0xD3; break; /* Right double quote */
                                case 0x2022: mac_out[out_len++] = (char)0xA5; break; /* Bullet (disc) */
                                case 0x2026: mac_out[out_len++] = (char)0xC9; break; /* Ellipsis */
                                case 0x2122: mac_out[out_len++] = (char)0xAA; break; /* Trademark */
                                case 0x20AC: mac_out[out_len++] = (char)0xDB; break; /* Euro */
                                /* Arrows */
                                case 0x2190: mac_out[out_len++] = '<'; break; /* Left arrow */
                                case 0x2192: mac_out[out_len++] = '>'; break; /* Right arrow */
                                case 0x2191: mac_out[out_len++] = '^'; break; /* Up arrow */
                                case 0x2193: mac_out[out_len++] = 'v'; break; /* Down arrow */
                                /* Box drawing / list markers (no MacRoman glyph, ASCII fallback) */
                                case 0x25A0: mac_out[out_len++] = (char)0xA5; break; /* Black square -> use bullet glyph */
                                case 0x25AA: mac_out[out_len++] = (char)0xA5; break; /* Small square -> use bullet glyph */
                                case 0x25CB: mac_out[out_len++] = 'o';        break; /* White circle */
                                case 0x25CF: mac_out[out_len++] = (char)0xA5; break; /* Black circle -> bullet */
                                case 0x25E6: mac_out[out_len++] = 'o';        break; /* White bullet (circle) */
                                /* Check / cross */
                                case 0x2713: mac_out[out_len++] = (char)0xA1; break; /* Check mark — use degree as nearest */
                                case 0x2717: mac_out[out_len++] = 'x';        break; /* Ballot X */
                                /* Common math */
                                case 0x00D7: mac_out[out_len++] = 'x';        break; /* Multiplication */
                                case 0x00F7: mac_out[out_len++] = (char)0xD6; break; /* Division */
                                case 0x2260: mac_out[out_len++] = (char)0xAD; break; /* Not equal */
                                case 0x2264: mac_out[out_len++] = (char)0xB2; break; /* Less or equal */
                                case 0x2265: mac_out[out_len++] = (char)0xB3; break; /* Greater or equal */
                                /* Currency */
                                case 0x00A5: mac_out[out_len++] = (char)0xB4; break; /* Yen */
                                case 0x00A2: mac_out[out_len++] = (char)0xA2; break; /* Cent */
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

        /* fixes42: letter-spacing inserted between glyph pairs.
         * mac_len characters => mac_len - 1 gaps. */
        if (fstyle != NULL && fstyle->letter_spacing != 0 && mac_len > 1) {
                width += (int)(mac_len - 1) * fstyle->letter_spacing;
                if (width < 0) width = 0;
        }

        /* fixes139b: word-spacing inserted after each ASCII space in
         * the MacRoman string. CSS word-spacing only affects literal
         * spaces; the per-space accounting here keeps measure and
         * paint in lockstep so wrap decisions match the painted
         * glyph positions. */
        if (fstyle != NULL && fstyle->word_spacing != 0 && mac_len > 0) {
                size_t k;
                int sc = 0;
                for (k = 0; k < (size_t) mac_len; k++) {
                        if (mac_str[k] == ' ') sc++;
                }
                if (sc > 0) {
                        width += sc * fstyle->word_spacing;
                        if (width < 0) width = 0;
                }
        }

        /* fixes51a -- anti-aliased TrueType glyphs (fixes51) can paint
         * a fringe pixel past the integer-pixel TextWidth value. NetSurf's
         * inline layout uses font_width to choose line breaks, and any
         * width underestimate cascades into subsequent lines being placed
         * too close to or even on top of each other. Add a small slop
         * proportional to character count so the layout always reserves
         * at least the true painted width. 1 px per 24 chars + 1 floor
         * is invisible at full-line widths but enough to absorb AA bleed
         * and the occasional fractional-pixel drift. */
        if (mac_len > 0) {
                width += (int)(mac_len / 24) + 1;
        }

        /* fixes69: bold-specific extra slop. QuickDraw fakes bold via
         * smear — each glyph is rendered twice with a 1-pixel right
         * shift to thicken the strokes. TextWidth(bold) returns the
         * smeared total width, but the smear from glyph N still bleeds
         * 1 pixel into glyph N+1's slot, causing letter pairs like "BE"
         * or "OB" to look fused in tight bold runs (visible on PROBE
         * card headings). Add 1 extra px per glyph-pair gap so the
         * smear has breathing room. Only applies when bold face is
         * active. */
        if ((face & 1) && mac_len > 1) {
                width += (int)(mac_len - 1);
        }

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

#ifdef __MACOS9__
/* ============================================================
 * fixes144a -- QuickDraw font-metric diagnostic probe.
 *
 * Fires once after window creation. Walks 4 fonts x 3 sizes x
 * 2 faces x 9 probe strings, logging TextWidth(full string)
 * vs sum-of-per-char-TextWidth to MacSurf Debug.log.
 *
 * Output line format:
 *   [METRIC] font=NAME id=N size=S face=F str="X" tw=A sum=B delta=C
 *
 * Where delta = A - B. Positive delta means TextWidth(full)
 * over-counts vs the per-char sum (rare). Negative delta means
 * TextWidth(full) under-counts -- this is the symptom behind
 * the "Di" overlap: full-string width is less than the sum of
 * the individual glyphs' painted widths, so DrawText advances
 * the pen into the next glyph's territory.
 *
 * Probe bypasses macos9_font_id_from_style entirely (which
 * currently force-collapses every CSS family to Helvetica per
 * fixes52). Real QuickDraw font selection is exercised here.
 *
 * NO BEHAVIOUR CHANGE. Diagnostic-only. */

typedef struct {
        short       font_id;
        const char *font_name;
} macos9_metric_probe_font;

static const macos9_metric_probe_font macos9_metric_probe_fonts[] = {
        {  3, "Geneva"    },
        { 21, "Helvetica" },
        {  0, "Chicago"   }, /* System font; Chicago on classic Mac OS. */
        {  4, "Monaco"    }
};

static const short macos9_metric_probe_sizes[] = { 9, 10, 12 };

static const short       macos9_metric_probe_faces[]      = { 0, 1 };
static const char *const macos9_metric_probe_faces_name[] = { "plain", "bold" };

static const char *const macos9_metric_probe_strings[] = {
        "Di", "Da", "Do", "Dill", "Disc",
        "The", "AV", "To", "fi"
};

void
macos9_font_metric_probe_run(void)
{
        static int has_run = 0;
        GrafPtr old_port;
        size_t fi, si, ff, st;
        size_t n_fonts;
        size_t n_sizes;
        size_t n_faces;
        size_t n_strings;

        if (has_run) {
                return;
        }
        has_run = 1;

        if (initial_win == NULL || initial_win->window == NULL) {
                return;
        }

        n_fonts   = sizeof(macos9_metric_probe_fonts) /
                    sizeof(macos9_metric_probe_fonts[0]);
        n_sizes   = sizeof(macos9_metric_probe_sizes) /
                    sizeof(macos9_metric_probe_sizes[0]);
        n_faces   = sizeof(macos9_metric_probe_faces) /
                    sizeof(macos9_metric_probe_faces[0]);
        n_strings = sizeof(macos9_metric_probe_strings) /
                    sizeof(macos9_metric_probe_strings[0]);

        GetPort(&old_port);
        SetPortWindowPort(initial_win->window);

        macsurf_debug_log_write("=== FONT METRIC PROBE BEGIN (fixes144a) ===");

        for (fi = 0; fi < n_fonts; fi++) {
                short font_id = macos9_metric_probe_fonts[fi].font_id;
                const char *font_name = macos9_metric_probe_fonts[fi].font_name;

                for (si = 0; si < n_sizes; si++) {
                        short sz = macos9_metric_probe_sizes[si];

                        for (ff = 0; ff < n_faces; ff++) {
                                short face = macos9_metric_probe_faces[ff];
                                const char *face_name =
                                    macos9_metric_probe_faces_name[ff];

                                TextFont(font_id);
                                TextSize(sz);
                                TextFace(face);

                                for (st = 0; st < n_strings; st++) {
                                        const char *s =
                                            macos9_metric_probe_strings[st];
                                        size_t k;
                                        size_t n;
                                        int sum;
                                        short tw_full;

                                        n = strlen(s);
                                        tw_full = TextWidth((char *)s, 0,
                                                            (short)n);

                                        sum = 0;
                                        for (k = 0; k < n; k++) {
                                                short cw;
                                                cw = TextWidth((char *)s,
                                                               (short)k, 1);
                                                sum += (int)cw;
                                        }

                                        macsurf_debug_log_writef(
                                            "[METRIC] font=%s id=%d size=%d "
                                            "face=%s str=\"%s\" tw=%d sum=%d "
                                            "delta=%d",
                                            font_name, (int)font_id,
                                            (int)sz, face_name, s,
                                            (int)tw_full, sum,
                                            (int)tw_full - sum);
                                }
                        }
                }
        }

        macsurf_debug_log_write("=== FONT METRIC PROBE END ===");

        SetPort(old_port);
}
#endif /* __MACOS9__ */
