from pathlib import Path

p = Path('browser/netsurf/frontends/macos9/macos9_font.c')
s = p.read_text()

old = '''        while (i < len && out_len < max_out) {\n                size_t char_len = utf8_char_byte_length(utf8 + i);\n                uint32_t ucs4;\n\n                if (char_len == 0 || i + char_len > len) {\n'''
new = '''        while (i < len && out_len < max_out) {\n                size_t char_len;\n                uint32_t ucs4;\n\n                /* ASCII dominates normal web text. Copy a whole ASCII run\n                 * directly instead of calling the UTF-8 length/decoder helpers\n                 * once per byte. Semantics are identical for all 0x00..0x7f. */\n                if ((unsigned char)utf8[i] < 0x80) {\n                        size_t start = i;\n                        size_t room = max_out - out_len;\n                        while (i < len && i - start < room &&\n                               (unsigned char)utf8[i] < 0x80) {\n                                i++;\n                        }\n                        memcpy(mac_out + out_len, utf8 + start, i - start);\n                        out_len += i - start;\n                        continue;\n                }\n\n                char_len = utf8_char_byte_length(utf8 + i);\n                if (char_len == 0 || i + char_len > len) {\n'''
if s.count(old) != 1:
    raise SystemExit('converter anchor mismatch %d' % s.count(old))
s = s.replace(old, new, 1)

old = '''                int spaces = 0;\n                static short char_locs[4097];\n                int wf;\n'''
new = '''                int spaces = 0;\n                int need_char_sum;\n                static short char_locs[4097];\n                int wf;\n'''
if s.count(old) != 1:
    raise SystemExit('position decl anchor mismatch %d' % s.count(old))
s = s.replace(old, new, 1)

old = '''                char_locs[0] = 0;\n                if (mac_len > 0)\n                        MeasureText((short)mac_len, mac_str, char_locs);\n\n                while (i < length) {\n'''
new = '''                char_locs[0] = 0;\n                if (mac_len > 0)\n                        MeasureText((short)mac_len, mac_str, char_locs);\n\n                need_char_sum = ((face & 1) != 0 ||\n                        (size < 12 && font_id != kFontIDMonaco) ||\n                        (fstyle != NULL &&\n                         (fstyle->letter_spacing != 0 ||\n                          fstyle->word_spacing != 0)));\n\n                while (i < length) {\n'''
if s.count(old) != 1:
    raise SystemExit('MeasureText anchor mismatch %d' % s.count(old))
s = s.replace(old, new, 1)

old = '''                        } else {\n                                char one_mac[4];\n                                delta = macos9_utf8_to_macroman(string + i,\n                                                               next_i - i,\n                                                               one_mac,\n                                                               sizeof(one_mac));\n                                target_mac_i = mac_i + delta;\n'''
new = '''                        } else {\n                                if ((unsigned char)string[i] < 0x80) {\n                                        delta = 1;\n                                } else {\n                                        char one_mac[4];\n                                        delta = macos9_utf8_to_macroman(\n                                                string + i, next_i - i,\n                                                one_mac, sizeof(one_mac));\n                                }\n                                target_mac_i = mac_i + delta;\n'''
if s.count(old) != 1:
    raise SystemExit('delta anchor mismatch %d' % s.count(old))
s = s.replace(old, new, 1)

old = '''                                while (mac_i < target_mac_i) {\n                                        char_sum += (int)CharWidth(mac_str[mac_i]);\n                                        if (mac_str[mac_i] == ' ')\n                                                spaces++;\n                                        mac_i++;\n                                }\n\n                                if (mac_i == 0) {\n                                        w = 0;\n                                } else {\n                                        int eff_ls;\n                                        int eff_ws;\n                                        macos9_run_spacing(fstyle, font_id, face,\n                                                           size, mac_i,\n                                                           &eff_ls, &eff_ws);\n                                        if ((eff_ls == 0 && eff_ws == 0) ||\n                                            mac_i <= 1) {\n                                                w = (int)char_locs[mac_i];\n                                        } else {\n'''
new = '''                                if (!need_char_sum) {\n                                        mac_i = target_mac_i;\n                                } else {\n                                        while (mac_i < target_mac_i) {\n                                                char_sum += (int)CharWidth(\n                                                        mac_str[mac_i]);\n                                                if (mac_str[mac_i] == ' ')\n                                                        spaces++;\n                                                mac_i++;\n                                        }\n                                }\n\n                                if (mac_i == 0) {\n                                        w = 0;\n                                } else if (!need_char_sum) {\n                                        w = (int)char_locs[mac_i];\n                                } else {\n                                        int eff_ls;\n                                        int eff_ws;\n                                        macos9_run_spacing(fstyle, font_id, face,\n                                                           size, mac_i,\n                                                           &eff_ls, &eff_ws);\n                                        if ((eff_ls == 0 && eff_ws == 0) ||\n                                            mac_i <= 1) {\n                                                w = (int)char_locs[mac_i];\n                                        } else {\n'''
if s.count(old) != 1:
    raise SystemExit('CharWidth anchor mismatch %d' % s.count(old))
s = s.replace(old, new, 1)

p.write_text(s)
