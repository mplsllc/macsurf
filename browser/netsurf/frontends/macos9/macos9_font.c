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

#include "utils/ns_errors.h"
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

/* fixes376 — MacRoman high half (0x80..0xFF) -> Unicode code point. Standard
 * Apple "Mac OS Roman" table. Index 0 == byte 0x80. C89 positional
 * initializer; 128 entries. Used by macos9_macroman_to_utf8 (clipboard paste). */
static const unsigned short macroman_high_to_ucs[128] = {
	/* 0x80 */ 0x00C4, 0x00C5, 0x00C7, 0x00C9, 0x00D1, 0x00D6, 0x00DC, 0x00E1,
	/* 0x88 */ 0x00E0, 0x00E2, 0x00E4, 0x00E3, 0x00E5, 0x00E7, 0x00E9, 0x00E8,
	/* 0x90 */ 0x00EA, 0x00EB, 0x00ED, 0x00EC, 0x00EE, 0x00EF, 0x00F1, 0x00F3,
	/* 0x98 */ 0x00F2, 0x00F4, 0x00F6, 0x00F5, 0x00FA, 0x00F9, 0x00FB, 0x00FC,
	/* 0xA0 */ 0x2020, 0x00B0, 0x00A2, 0x00A3, 0x00A7, 0x2022, 0x00B6, 0x00DF,
	/* 0xA8 */ 0x00AE, 0x00A9, 0x2122, 0x00B4, 0x00A8, 0x2260, 0x00C6, 0x00D8,
	/* 0xB0 */ 0x221E, 0x00B1, 0x2264, 0x2265, 0x00A5, 0x00B5, 0x2202, 0x2211,
	/* 0xB8 */ 0x220F, 0x03C0, 0x222B, 0x00AA, 0x00BA, 0x03A9, 0x00E6, 0x00F8,
	/* 0xC0 */ 0x00BF, 0x00A1, 0x00AC, 0x221A, 0x0192, 0x2248, 0x2206, 0x00AB,
	/* 0xC8 */ 0x00BB, 0x2026, 0x00A0, 0x00C0, 0x00C3, 0x00D5, 0x0152, 0x0153,
	/* 0xD0 */ 0x2013, 0x2014, 0x201C, 0x201D, 0x2018, 0x2019, 0x00F7, 0x25CA,
	/* 0xD8 */ 0x00FF, 0x0178, 0x2044, 0x20AC, 0x2039, 0x203A, 0xFB01, 0xFB02,
	/* 0xE0 */ 0x2021, 0x00B7, 0x201A, 0x201E, 0x2030, 0x00C2, 0x00CA, 0x00C1,
	/* 0xE8 */ 0x00CB, 0x00C8, 0x00CD, 0x00CE, 0x00CF, 0x00CC, 0x00D3, 0x00D4,
	/* 0xF0 */ 0xF8FF, 0x00D2, 0x00DA, 0x00DB, 0x00D9, 0x0131, 0x02C6, 0x02DC,
	/* 0xF8 */ 0x00AF, 0x02D8, 0x02D9, 0x02DA, 0x00B8, 0x02DD, 0x02DB, 0x02C7
};

/* Convert MacRoman bytes to UTF-8. Inverse of macos9_utf8_to_macroman.
 *
 * mac/len  : input MacRoman bytes (e.g. the desk-scrap 'TEXT' flavor).
 * utf8_out : output buffer (UTF-8).
 * max_out  : capacity of utf8_out in bytes, INCLUDING space for the NUL.
 *
 * Returns the number of UTF-8 bytes written, NOT counting the NUL. A
 * terminating NUL is written when there is room. Never writes more than
 * max_out bytes and never splits a multi-byte UTF-8 sequence across the
 * buffer edge. */
size_t
macos9_macroman_to_utf8(const unsigned char *mac, size_t len,
                        char *utf8_out, size_t max_out)
{
        size_t in_i = 0;
        size_t out_len = 0;

        if (utf8_out == NULL || max_out == 0)
                return 0;

        while (in_i < len) {
                unsigned char b = mac[in_i];
                unsigned int  cp;
                size_t        need;

                if (b < 0x80) {
                        cp = (unsigned int)b;
                } else {
                        cp = (unsigned int)macroman_high_to_ucs[b - 0x80];
                }

                if (cp < 0x80) {
                        need = 1;
                } else if (cp < 0x800) {
                        need = 2;
                } else {
                        need = 3;
                }

                /* Room for this sequence AND the terminating NUL. */
                if (out_len + need + 1 > max_out)
                        break;

                if (need == 1) {
                        utf8_out[out_len++] = (char)cp;
                } else if (need == 2) {
                        utf8_out[out_len++] = (char)(0xC0 | (cp >> 6));
                        utf8_out[out_len++] = (char)(0x80 | (cp & 0x3F));
                } else {
                        utf8_out[out_len++] = (char)(0xE0 | (cp >> 12));
                        utf8_out[out_len++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        utf8_out[out_len++] = (char)(0x80 | (cp & 0x3F));
                }

                in_i++;
        }

        utf8_out[out_len] = '\0';
        return out_len;
}

/* fixes366i — per-layout font-measurement profiling counters. Bumped
 * on every macos9_font_measure call (each does a real QuickDraw
 * TextWidth). html_reformat resets these before layout_document and
 * emits a FONTPROF summary after, so one clean number per layout pass
 * tells us whether text measurement is the per-pass bottleneck. Plain
 * longs, no timing traps, so the instrumentation itself is ~free. */
long macos9_font_measure_calls = 0;
long macos9_font_measure_chars = 0;

#ifdef __MACOS9__
/* fixes609: shared effective-spacing computation. The measure path (here)
 * and the paint path (plotters.c macos9_plot_text) MUST derive letter/word
 * spacing identically, or reserved width drifts from painted width and
 * inter-word spaces collapse ("newadventure"). Bold-smear breathing room
 * (fixes70/69) and the sub-12pt bitmap glyph-gap correction (fixes144b/146)
 * are folded into the effective letter-spacing here so both paths pick the
 * same value AND the same bulk-vs-per-char branch. Exported for plotters.c. */
void
macos9_run_spacing(const struct plot_font_style *fstyle,
                   short font_id, short face, short size, size_t mac_len,
                   int *out_ls, int *out_ws)
{
        int ls = (fstyle != NULL) ? fstyle->letter_spacing : 0;
        int ws = (fstyle != NULL) ? fstyle->word_spacing : 0;
        if ((face & 1) && mac_len > 1)
                ls += 1;                /* bold smear breathing room */
        if (size < 12 && font_id != kFontIDMonaco && mac_len > 1)
                ls += 1;                /* sub-12 bitmap glyph gap */
        *out_ls = ls;
        *out_ws = ws;
}

/* fixes609: painted extent of a MacRoman run under the per-char model -- the
 * exact right edge the paint path's per-glyph pen walk produces (sum of
 * CharWidth plus the inter-glyph gaps between, but not after, the last
 * glyph). QuickDraw's TextWidth(full) UNDER-counts vs this per-char sum
 * (macos9_font.c:561 metric probe), which is the bold/sub-12 overlap source;
 * measuring this way makes measure and paint agree exactly. Requires the QD
 * port's TextFont/TextSize/TextFace to already be set. */
static int
macos9_run_extent(const char *mac_str, size_t mac_len, int ls, int ws)
{
        int total = 0;
        size_t i;
        for (i = 0; i < mac_len; i++) {
                total += (int)CharWidth(mac_str[i]);
                if (i + 1 < mac_len) {
                        total += ls;
                        if (mac_str[i] == ' ')
                                total += ws;
                }
        }
        if (total < 0)
                total = 0;
        return total;
}

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

        macos9_font_measure_calls++;
        macos9_font_measure_chars += (long)length;

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

        /* fixes609: measure via the SAME model the painter advances by, so
         * reserved width == painted width by construction. This replaces the
         * accreted TextWidth + slop stack (fixes42/139b/51a/69/146): the AA
         * slop over-reserved (opening gaps and skewing wrap points), while
         * TextWidth still under-counted vs the per-char pen walk for
         * bold/sub-12/spaced runs, so paint overran the reservation and
         * inter-word spaces collapsed ("newadventure"). macos9_run_spacing
         * folds bold-smear + sub-12 gaps into the effective letter-spacing
         * exactly as the paint path does; the branch below mirrors the
         * painter's bulk-vs-per-char decision so the two never drift. */
        {
                int eff_ls;
                int eff_ws;
                macos9_run_spacing(fstyle, font_id, face, size, mac_len,
                                   &eff_ls, &eff_ws);
                if ((eff_ls == 0 && eff_ws == 0) || mac_len <= 1) {
                        /* Bulk model: matches paint's single DrawText, which
                         * advances by exactly TextWidth. */
                        width = TextWidth(mac_str, 0, (short)mac_len);
                } else {
                        /* Per-char model: matches paint's per-glyph pen walk
                         * (sum of CharWidth + inter-glyph gaps). */
                        width = macos9_run_extent(mac_str, mac_len,
                                                  eff_ls, eff_ws);
                }
        }

#if MACSURF_FONT_ALIAS_DIAG
        /* fixes157: log measure-side dispatch. Paired with the matching
         * block in macos9_plot_text — for a multi-segment inline line
         * (e.g. body Helvetica + <code> Monaco), pulling the two adjacent
         * FONTDIAG entries from the log shows whether width and paint
         * resolved the same font_id/size/face for the same fstyle. SMART
         * filter (in macos9.h) skips the SANS_SERIF firehose. */
        {
                int log_this = 1;
#if MACSURF_FONT_ALIAS_DIAG_SMART
                if (fstyle != NULL &&
                    fstyle->family == PLOT_FONT_FAMILY_SANS_SERIF) {
                        log_this = 0;
                }
#endif
                if (log_this) {
                        char preview[24];
                        size_t pv = (mac_len < 16) ? mac_len : 16;
                        size_t k;
                        for (k = 0; k < pv; k++) {
                                char c = mac_str[k];
                                preview[k] = (c >= 0x20 && c < 0x7f) ? c : '.';
                        }
                        preview[pv] = '\0';
                        macsurf_debug_log_writef(
                            "[FONTDIAG] op=measure fam=%d id=%d sz=%d face=%d "
                            "ls=%d ws=%d mac=%d w=%d str=\"%s\"",
                            (int)(fstyle ? fstyle->family : -1),
                            (int)font_id, (int)size, (int)face,
                            (int)(fstyle ? fstyle->letter_spacing : 0),
                            (int)(fstyle ? fstyle->word_spacing : 0),
                            (int)mac_len, width, preview);
                }
        }
#endif

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
        {  4, "Monaco"    },
        { 20, "Times"     }, /* fixes153: probe for serif alias retry. */
        { 16, "Palatino"  }, /* fixes153: alternative serif. */
        { 22, "Courier"   }  /* fixes153: alternative monospace. */
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

        n_fonts   = sizeof(macos9_metric_probe_fonts) /
                    sizeof(macos9_metric_probe_fonts[0]);
        n_sizes   = sizeof(macos9_metric_probe_sizes) /
                    sizeof(macos9_metric_probe_sizes[0]);
        n_faces   = sizeof(macos9_metric_probe_faces) /
                    sizeof(macos9_metric_probe_faces[0]);
        n_strings = sizeof(macos9_metric_probe_strings) /
                    sizeof(macos9_metric_probe_strings[0]);

        /* fixes144a2: initial_win is only set by macos9_create_initial_window
         * (the fallback path); the normal BW_CREATE path leaves it NULL. Use
         * FrontWindow() as the port source instead -- same pattern as
         * macos9_font_measure. */
        GetPort(&old_port);
        if (initial_win != NULL && initial_win->window != NULL) {
                SetPortWindowPort(initial_win->window);
        } else if (FrontWindow() != NULL) {
                SetPortWindowPort(FrontWindow());
        } else {
                /* No window yet; can't measure. Drop the run silently;
                 * has_run stays true so we don't try again. */
                macsurf_debug_log_write(
                    "FONT METRIC PROBE: no window available, skipping");
                return;
        }

        macsurf_debug_log_write("=== FONT METRIC PROBE BEGIN (fixes144a2) ===");

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

/* fixes153: vertical-metric probe. Calls GetFontInfo for each
 * font x size x face combination in the probe matrix and dumps
 * ascent / descent / widMax / leading. Ground-truth values for
 * the gui_layout_table per-font work and the font-family alias
 * retry deferred from fixes145b. Like fixes144a2 this is single-
 * shot at startup, gated by a static has_run, no work after the
 * first call. */
void
macos9_font_vmetric_probe_run(void)
{
        static int has_run = 0;
        GrafPtr old_port;
        size_t fi, si, ff;
        size_t n_fonts;
        size_t n_sizes;
        size_t n_faces;

        if (has_run) {
                return;
        }
        has_run = 1;

        n_fonts   = sizeof(macos9_metric_probe_fonts) /
                    sizeof(macos9_metric_probe_fonts[0]);
        n_sizes   = sizeof(macos9_metric_probe_sizes) /
                    sizeof(macos9_metric_probe_sizes[0]);
        n_faces   = sizeof(macos9_metric_probe_faces) /
                    sizeof(macos9_metric_probe_faces[0]);

        GetPort(&old_port);
        if (initial_win != NULL && initial_win->window != NULL) {
                SetPortWindowPort(initial_win->window);
        } else if (FrontWindow() != NULL) {
                SetPortWindowPort(FrontWindow());
        } else {
                macsurf_debug_log_write(
                    "FONT VMETRIC PROBE: no window available, skipping");
                return;
        }

        macsurf_debug_log_write(
            "=== FONT VMETRIC PROBE BEGIN (fixes153) ===");

        for (fi = 0; fi < n_fonts; fi++) {
                short font_id = macos9_metric_probe_fonts[fi].font_id;
                const char *font_name = macos9_metric_probe_fonts[fi].font_name;

                for (si = 0; si < n_sizes; si++) {
                        short sz = macos9_metric_probe_sizes[si];

                        for (ff = 0; ff < n_faces; ff++) {
                                short face = macos9_metric_probe_faces[ff];
                                const char *face_name =
                                    macos9_metric_probe_faces_name[ff];
                                FontInfo fi_info;

                                TextFont(font_id);
                                TextSize(sz);
                                TextFace(face);

                                /* Zero out the struct first -- FontInfo
                                 * fields are filled by GetFontInfo but
                                 * defensive zero catches the case where
                                 * the toolbox call fails silently. */
                                fi_info.ascent  = 0;
                                fi_info.descent = 0;
                                fi_info.widMax  = 0;
                                fi_info.leading = 0;

                                GetFontInfo(&fi_info);

                                macsurf_debug_log_writef(
                                    "[VMETRIC] font=%s id=%d size=%d "
                                    "face=%s ascent=%d descent=%d "
                                    "widMax=%d leading=%d lineheight=%d",
                                    font_name, (int)font_id,
                                    (int)sz, face_name,
                                    (int)fi_info.ascent,
                                    (int)fi_info.descent,
                                    (int)fi_info.widMax,
                                    (int)fi_info.leading,
                                    (int)(fi_info.ascent +
                                          fi_info.descent +
                                          fi_info.leading));
                        }
                }
        }

        macsurf_debug_log_write(
            "=== FONT VMETRIC PROBE END ===");

        SetPort(old_port);
}
#endif /* __MACOS9__ */
