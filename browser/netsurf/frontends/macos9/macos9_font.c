/*
 * MacSurf - macos9_font.c
 * gui_layout_table callbacks.
 *
 * v0.5 rewrite: 
 *   - Use FrontWindow() fallback for layout port if initial_win is NULL.
 *   - Linear scan for position/split (more reliable than UTF-8 binary search).
 *   - Robust UTF-8 handling.
 */

#include <string.h>

#include "utils/ns_errors.h"
#include "utils/log.h"
#include "utils/utf8.h"
#include "netsurf/plot_style.h"
#include "netsurf/layout.h"

#include "macos9.h"
#include "macsurf_debug_log.h"
#include "macos9_webfont.h"
#include "netsurf/browser_window.h"
#include "content/hlcache.h"
#include <libwapcaplet/libwapcaplet.h>

#ifdef __MACOS9__
extern struct gui_window *macos9_window_list_head(void);
extern struct browser_window *macos9_gw_bw(struct gui_window *g);

/* First UTF-8 codepoint of a run (0 on empty/invalid). */
static unsigned long macos9_wf_first_cp(const char *s, size_t len)
{
        unsigned char c;
        if (s == NULL || len == 0)
                return 0;
        c = (unsigned char) s[0];
        if (c < 0x80)
                return c;
        if ((c & 0xE0) == 0xC0 && len >= 2)
                return ((unsigned long) (c & 0x1F) << 6) |
                       (unsigned long) (s[1] & 0x3F);
        if ((c & 0xF0) == 0xE0 && len >= 3)
                return ((unsigned long) (c & 0x0F) << 12) |
                       ((unsigned long) (s[1] & 0x3F) << 6) |
                       (unsigned long) (s[2] & 0x3F);
        if ((c & 0xF8) == 0xF0 && len >= 4)
                return ((unsigned long) (c & 0x07) << 18) |
                       ((unsigned long) (s[1] & 0x3F) << 12) |
                       ((unsigned long) (s[2] & 0x3F) << 6) |
                       (unsigned long) (s[3] & 0x3F);
        return 0;
}

/* Content currently in the front window (for @font-face resolution). */
static struct content *macos9_wf_current_content(void)
{
        struct gui_window *gw = macos9_window_list_head();
        struct browser_window *bw = (gw != NULL) ? macos9_gw_bw(gw) : NULL;
        struct hlcache_handle *h = (bw != NULL) ?
                browser_window_get_content(bw) : NULL;
        return (h != NULL) ? hlcache_handle_get_content(h) : NULL;
}

#define MACOS9_WF_IS_PUA(cp) (((cp) >= 0xE000UL && (cp) <= 0xF8FFUL) || \
                              ((cp) >= 0xF0000UL && (cp) <= 0x10FFFDUL))

/* fixes630: if this run is a single Private-Use-Area codepoint in a
 * downloadable @font-face family, return the glyph's advance width in px (also
 * kicks the font fetch on first sight, and re-measures via a reformat once it
 * loads). Returns -1 for ordinary text so normal measurement is untouched  - 
 * ordinary text never starts with a PUA codepoint, so this is a no-op for it. */
static int macos9_wf_icon_advance(const struct plot_font_style *fstyle,
                const char *string, size_t length, int size_px)
{
        unsigned long cp;
        struct content *c;
        lwc_string * const *ff;

        if (fstyle == NULL || fstyle->families == NULL)
                return -1;
        cp = macos9_wf_first_cp(string, length);
        if (!MACOS9_WF_IS_PUA(cp))
                return -1;
        c = macos9_wf_current_content();
        if (c == NULL)
                return -1;
        for (ff = fstyle->families; *ff != NULL; ff++) {
                int a = macos9_webfont_advance(c, *ff, cp, size_px);
                if (a >= 0)
                        return a;
        }
        return -1;
}
#endif /* __MACOS9__ */

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

                if (ucs4 == 0x00AD) {
                        /* #251 soft hyphen: zero-width and invisible in the
                         * middle of a run; only when it is the LAST codepoint
                         * of the run (the line broke right after it) does it
                         * paint a hyphen. This is what makes hyphens:manual
                         * show a '-' at the break and nothing otherwise --
                         * and it fixes the old '?' that U+00AD fell through
                         * to. Applies to measurement and paint alike, since
                         * both go through this converter. */
                        if (i >= len)
                                mac_out[out_len++] = '-';
                        continue;
                }

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
                                case 0x2713: mac_out[out_len++] = (char)0xA1; break; /* Check mark  -  use degree as nearest */
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
                                default:
                                        /* fixes615  -  icon-font glyphs
                                         * (FontAwesome / Material Design) live
                                         * in the Unicode Private Use Area
                                         * (U+E000..U+F8FF). NetSurf has no
                                         * downloadable-webfont support, so
                                         * QuickDraw can't render them and they
                                         * fell back to '?', littering the page
                                         * with question marks. Emit nothing
                                         * (blank) for PUA codepoints so icon
                                         * slots read clean; non-PUA unmapped
                                         * chars still show '?'. */
                                        if ((ucs4 >= 0xE000 && ucs4 <= 0xF8FF) ||
                                            (ucs4 >= 0xF0000 && ucs4 <= 0x10FFFD)) {
                                                /* skip  -  render blank. Covers the
                                                 * BMP Private Use Area (FontAwesome,
                                                 * glyphicons) AND Supplementary PUA
                                                 * planes A/B (U+F0000+), where
                                                 * Material Design Icons v5+ live
                                                 * (e.g. mdi \F0004). Round 1 has no
                                                 * glyph renderer yet, so blank beats
                                                 * a '?' littering every icon slot. */
                                        } else {
                                                mac_out[out_len++] = '?';
                                        }
                                        break;
                        }
                }
        }
        return out_len;
}

/* fixes376  -  MacRoman high half (0x80..0xFF) -> Unicode code point. Standard
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

/* fixes366i  -  per-layout font-measurement profiling counters. Bumped
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

/* fixes609: painted extent of a MacRoman run under the per-char model - the
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

        /* fixes630: divert a downloadable-webfont icon glyph (PUA codepoint in
         * an @font-face family) to its real advance so the box gets sized
         * instead of collapsing to zero (QuickDraw has no glyph for PUA). No
         * port state touched yet, so we can return straight away. */
        {
                int wf = macos9_wf_icon_advance(fstyle, string, length, (int)size);
                if (wf >= 0)
                        return wf;
        }

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
         * block in macos9_plot_text  -  for a multi-segment inline line
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

/* fixes993  -  does a CJK codepoint begin at s[i]? See the long note in
 * layout.c: CJK has no spaces, so without treating every ideograph/kana as a
 * break opportunity a whole paragraph is one unbreakable word. Byte-wise UTF-8
 * test, no decoder needed for the ranges that matter:
 *   E2 BA..  U+2E80-U+2FFF  radicals        E3 ..    U+3000-U+33FF  kana/punct
 *   E4-ED    U+4000-U+D7AF  ideographs/Hangul
 *   EF A4..  U+F900-U+FAFF  compat          EF BC..  U+FF00-U+FFEF  fullwidth
 */
/* fixes994  -  kinsoku shori: the two rules that make Japanese look wrong when
 * they are missing, rather than merely unpolished.
 *
 * UAX #14 forbids a line BEGINNING with closing punctuation (a full stop or a
 * closing bracket stranded alone at the start of a line is the classic
 * giveaway), and forbids a line ENDING with an opening bracket. Small kana
 * (っゃゅょ etc.) and the prolonged sound mark are in the same class as
 * closing punctuation: they modify the character before them and must never
 * lead a line.
 *
 * fixes993 made CJK breakable at all, which is what stopped the page blowing
 * out. This decides WHERE the break lands. Both sets are small and closed, so
 * a table beats a property lookup.
 *
 * Consequence for the minimum-width calculation in layout.c: a no-break-before
 * character is glued to its predecessor, so a true minimum cluster can be two
 * or three characters rather than one. layout.c still counts one character per
 * word, which UNDER-estimates the minimum slightly. That is the safe
 * direction - it permits a marginally narrower shrink-to-fit than is strictly
 * legal, never a wider page - and it keeps the expensive path (run over every
 * text box on every reflow) as cheap as it is today.
 */
static unsigned long macos9_cjk_cp_at(const char *s, size_t len, size_t i)
{
        unsigned char c0, c1, c2;
        if (s == NULL || i + 2 >= len) return 0UL;
        c0 = (unsigned char)s[i];
        if ((c0 & 0xF0) != 0xE0) return 0UL;   /* only the 3-byte plane */
        c1 = (unsigned char)s[i + 1];
        c2 = (unsigned char)s[i + 2];
        return ((unsigned long)(c0 & 0x0F) << 12) |
               ((unsigned long)(c1 & 0x3F) << 6) |
               (unsigned long)(c2 & 0x3F);
}

/* Must not START a line. */
static int macos9_cjk_no_break_before(unsigned long cp)
{
        switch (cp) {
        /* full stops, commas, middle dot */
        case 0x3001UL: case 0x3002UL: case 0x30FBUL:
        case 0xFF0CUL: case 0xFF0EUL: case 0xFF61UL: case 0xFF64UL:
        /* closing brackets and quotes */
        case 0x300DUL: case 0x300FUL: case 0x3011UL: case 0x3015UL:
        case 0x3017UL: case 0x3019UL: case 0x301BUL:
        case 0x3009UL: case 0x300BUL:
        case 0xFF09UL: case 0xFF3DUL: case 0xFF5DUL: case 0xFF63UL:
        /* sentence-final marks */
        case 0xFF01UL: case 0xFF1FUL: case 0xFF1AUL: case 0xFF1BUL:
        /* prolonged sound mark, iteration marks */
        case 0x30FCUL: case 0x3005UL: case 0x303BUL:
        /* small kana (hiragana) */
        case 0x3041UL: case 0x3043UL: case 0x3045UL: case 0x3047UL:
        case 0x3049UL: case 0x3063UL: case 0x3083UL: case 0x3085UL:
        case 0x3087UL: case 0x308EUL: case 0x3095UL: case 0x3096UL:
        /* small kana (katakana) */
        case 0x30A1UL: case 0x30A3UL: case 0x30A5UL: case 0x30A7UL:
        case 0x30A9UL: case 0x30C3UL: case 0x30E3UL: case 0x30E5UL:
        case 0x30E7UL: case 0x30EEUL: case 0x30F5UL: case 0x30F6UL:
                return 1;
        default:
                return 0;
        }
}

/* Must not END a line. */
static int macos9_cjk_no_break_after(unsigned long cp)
{
        switch (cp) {
        case 0x300CUL: case 0x300EUL: case 0x3010UL: case 0x3014UL:
        case 0x3016UL: case 0x3018UL: case 0x301AUL:
        case 0x3008UL: case 0x300AUL:
        case 0xFF08UL: case 0xFF3BUL: case 0xFF5BUL: case 0xFF62UL:
                return 1;
        default:
                return 0;
        }
}

static int macos9_cjk_starts_at(const char *s, size_t len, size_t i)
{
        unsigned char c0, c1;
        if (s == NULL || i >= len) return 0;
        c0 = (unsigned char)s[i];
        if (c0 < 0xE2 || c0 > 0xEF) return 0;      /* not a CJK-range lead */
        if (i + 2 >= len) return 0;                /* truncated: leave alone */
        c1 = (unsigned char)s[i + 1];
        if (c0 == 0xE2) return (c1 >= 0xBA) ? 1 : 0;          /* U+2E80+   */
        if (c0 == 0xE3) return 1;                              /* U+3000+   */
        if (c0 >= 0xE4 && c0 <= 0xED) return 1;                /* U+4000+   */
        if (c0 == 0xEF) {
                if (c1 >= 0xA4 && c1 <= 0xAB) return 1;        /* U+F900+   */
                if (c1 >= 0xBC && c1 <= 0xBD) return 1;        /* U+FF00+   */
        }
        return 0;
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
        /* #251 soft-hyphen (U+00AD = 0xC2 0xAD) break opportunity. */
        size_t last_shy = 0;   /* byte offset AFTER a soft hyphen that fits */
        int have_shy = 0;
        /* fixes993  -  byte offset of the LAST CJK character that fits. UAX #14
         * permits a break between two ideographs/kana, and without it a
         * space-free Japanese paragraph never splits, so the line runs off. */
        size_t last_cjk = 0;
        int have_cjk = 0;
        /* CSS_HYPHENS_NONE == 1 (css_hyphens_e); allow shy breaks otherwise
         * (inherit/manual/auto). Avoids a libcss include in the font code. */
        int allow_shy = (fstyle != NULL && fstyle->hyphens != 1);

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

        /* The string overflows. Find the last space - and, when hyphens
         * allows, the last soft hyphen - that FITS. */
        for (i = 0; i < fit_offset; i++) {
                if (string[i] == ' ') {
                        last_space = i;
                        have_space = 1;
                }
                if (allow_shy && i + 1 < fit_offset &&
                                (unsigned char)string[i] == 0xC2 &&
                                (unsigned char)string[i + 1] == 0xAD) {
                        last_shy = i + 2;   /* break AFTER the soft hyphen */
                        have_shy = 1;
                }
                /* fixes993  -  a CJK character here means a break is permitted
                 * BEFORE it. Only i > 0: offset 0 reads to core as "cannot
                 * split here" (fixes788), which would loop. */
                if (i > 0 && macos9_cjk_starts_at(string, fit_offset, i)) {
                        /* fixes994  -  kinsoku shori. Breaking HERE would put
                         * string[i] at the start of the next line and the
                         * character before it at the end of this one, so
                         * reject the pair the rules forbid. i >= 3 because
                         * every character in both sets is 3-byte UTF-8. */
                        int ok = 1;
                        if (macos9_cjk_no_break_before(
                                        macos9_cjk_cp_at(string, fit_offset, i)))
                                ok = 0;
                        if (ok && i >= 3 &&
                            macos9_cjk_no_break_after(macos9_cjk_cp_at(
                                        string, fit_offset, i - 3)))
                                ok = 0;
                        if (ok) {
                                last_cjk = i;
                                have_cjk = 1;
                        }
                }
        }

        /* #251: prefer whichever break fits the most content (rightmost). A
         * soft-hyphen break returns the offset AFTER the U+00AD so the first
         * fragment ends with it and paints a trailing '-' (macos9_utf8_to_
         * macroman); core sees a non-space at that offset so reserves no
         * box->space, exactly right for a hyphenation break. */
        /* fixes993  -  CJK wins when it is the rightmost break that fits, so a
         * mixed sentence still breaks nearest the edge. char_offset lands ON
         * the character and the byte there is not a space, so core reserves no
         * box->space - correct for a CJK break, as for a hyphenation one. */
        if (have_cjk && (have_space == 0 || last_cjk > last_space) &&
                        (have_shy == 0 || last_cjk > last_shy)) {
                *char_offset = last_cjk;
                macos9_font_width(fstyle, string, last_cjk, actual_x);
                return NSERROR_OK;
        }
        if (have_shy && (have_space == 0 || last_shy > last_space)) {
                *char_offset = last_shy;
                macos9_font_width(fstyle, string, last_shy, actual_x);
        } else if (have_space) {
                /* fixes636: return the offset OF the space itself, not the
                 * char after it. NetSurf core's layout_text_box_split
                 * (layout.c:3512) does
                 *     bool space = (split_box->text[new_length] == ' ');
                 * i.e. it requires char_offset to land ON the space so core
                 * can detect it, reserve box->space, and consume exactly one
                 * space itself (used_length = new_length + (space?1:0)).
                 * Returning last_space+1 made core see a letter at that
                 * offset, so it treated the break as spaceless: box->space
                 * was forced to 0 and the next fragment packed flush against
                 * the previous one with no gap - "software and" rendered as
                 * "softwareand". actual_x is unchanged: it already measures
                 * width up to (excluding) the space, which is exactly what
                 * core expects paired with char_offset == last_space. */
                *char_offset = last_space;
                macos9_font_width(fstyle, string, last_space, actual_x);
        } else {
                /* #234: no word-boundary (space) break fits within the
                 * available width x. Return char_offset == 0 - the standard
                 * NetSurf "text can't be split here" signal - and let core
                 * decide (layout.c:4462): if this is the FIRST box on the line
                 * it force-fits the word (or char-breaks it when the author set
                 * word-break/overflow-wrap, handled at layout.c:4414);
                 * otherwise the whole word wraps to the next line.
                 *
                 * The previous code returned `fit_offset` here - a mid-word
                 * offset - so on a nearly-full line (small remaining x) core
                 * accepted a mid-word break: "remain" rendered as "remai"+"n"
                 * on reflow. That fixes136a-era aggressive char-break (a
                 * documented deviation from spec) was itself the cause of the
                 * word-merge/mid-wrap symptoms. Standard behaviour: long
                 * unbreakable words overflow unless overflow-wrap:break-word. */
                *char_offset = 0;
                *actual_x = 0;
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
 * fixes144a - QuickDraw font-metric diagnostic probe.
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
 * TextWidth(full) under-counts - this is the symptom behind
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
         * FrontWindow() as the port source instead - same pattern as
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

                                /* Zero out the struct first - FontInfo
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
