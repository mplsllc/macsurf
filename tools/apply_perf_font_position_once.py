from pathlib import Path

path = Path('browser/netsurf/frontends/macos9/macos9_font.c')
s = path.read_text()
old = r'''static nserror
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
'''
new = r'''static nserror
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
        {
                short font_id;
                short size;
                short face;
                GrafPtr old_port;
                Boolean changed_port = false;
                char mac_str[4096];
                size_t mac_len;
                size_t mac_i = 0;
                int char_sum = 0;
                int spaces = 0;
                static short char_locs[4097];
                int wf;

                font_id = macos9_font_id_from_style(fstyle);
                face = macos9_face_from_style(fstyle);
                size = (short)(fstyle->size >> PLOT_STYLE_RADIX);
                if (size <= 0) size = 12;

                /* fixes-perf: macos9_font_measure() used to run once for every
                 * UTF-8 prefix below.  That made position O(n^2): every step
                 * reconverted the same prefix and entered QuickDraw again.
                 * Downloadable PUA runs are a special case whose old measure
                 * result depends only on the first codepoint/family, so resolve
                 * it once and preserve that behaviour for every boundary. */
                wf = macos9_wf_icon_advance(fstyle, string, length, (int)size);
                if (wf >= 0) {
                        if (wf > x) {
                                *char_offset = 0;
                                *actual_x = 0;
                        } else {
                                *char_offset = length;
                                *actual_x = wf;
                        }
                        return NSERROR_OK;
                }

                mac_len = macos9_utf8_to_macroman(string, length,
                                                  mac_str, sizeof(mac_str));

                GetPort(&old_port);
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

                /* MeasureText gives the exact TextWidth-compatible cumulative
                 * positions for all MacRoman byte boundaries in one QuickDraw
                 * call.  Keep the per-glyph cumulative sum beside it because
                 * fixes609 deliberately uses CharWidth + synthetic spacing for
                 * bold/sub-12/letter/word-spaced runs. */
                char_locs[0] = 0;
                if (mac_len > 0)
                        MeasureText((short)mac_len, mac_str, char_locs);

                while (i < length) {
                        size_t next_i = utf8_next(string, length, i);
                        size_t delta = 0;
                        size_t target_mac_i;
                        int w;
                        int is_shy;

                        if (next_i == i)
                                break;

                        is_shy = (next_i - i == 2 &&
                                  (unsigned char)string[i] == 0xC2 &&
                                  (unsigned char)string[i + 1] == 0xAD);

                        if (is_shy && next_i < length) {
                                /* U+00AD is the one prefix-sensitive mapping:
                                 * internal SHY emits zero bytes in the full run,
                                 * but a prefix ending exactly here paints '-'.
                                 * Keep the exact legacy measurement for these
                                 * rare boundaries; complexity is O(n + shy). */
                                w = macos9_font_measure(fstyle, string, next_i);
                        } else {
                                char one_mac[4];
                                delta = macos9_utf8_to_macroman(string + i,
                                                               next_i - i,
                                                               one_mac,
                                                               sizeof(one_mac));
                                target_mac_i = mac_i + delta;
                                if (target_mac_i > mac_len)
                                        target_mac_i = mac_len;

                                while (mac_i < target_mac_i) {
                                        char_sum += (int)CharWidth(mac_str[mac_i]);
                                        if (mac_str[mac_i] == ' ')
                                                spaces++;
                                        mac_i++;
                                }

                                if (mac_i == 0) {
                                        w = 0;
                                } else {
                                        int eff_ls;
                                        int eff_ws;
                                        macos9_run_spacing(fstyle, font_id, face,
                                                           size, mac_i,
                                                           &eff_ls, &eff_ws);
                                        if ((eff_ls == 0 && eff_ws == 0) ||
                                            mac_i <= 1) {
                                                w = (int)char_locs[mac_i];
                                        } else {
                                                int gap_spaces = spaces;
                                                if (mac_str[mac_i - 1] == ' ')
                                                        gap_spaces--;
                                                w = char_sum +
                                                    (int)(mac_i - 1) * eff_ls +
                                                    gap_spaces * eff_ws;
                                                if (w < 0) w = 0;
                                        }
                                }
                        }

                        if (w > x) {
                                if (changed_port)
                                        SetPort(old_port);
                                if (i == 0) {
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

                if (changed_port)
                        SetPort(old_port);
                *char_offset = i;
                *actual_x = last_x;
        }
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
'''
if s.count(old) != 1:
    raise SystemExit('macos9_font_position source block mismatch: %d' % s.count(old))
path.write_text(s.replace(old, new, 1))
