#!/usr/bin/env python3
from pathlib import Path
p = Path('browser/netsurf/desktop/textarea.c')
s = p.read_text()

anchor = '''struct line_info {\n\tunsigned int b_start;\t\t/**< Byte offset of line start */\n\tunsigned int b_length;\t\t/**< Byte length of line */\n\tint width;\t\t\t/**< Width in pixels of line */\n\tint prefix_max_width;\t\t/**< Max width through this line */\n};\n'''
helper = anchor + '''\n/* Sorted line starts are queried on every caret move/selection update. */\nstatic int textarea_line_for_offset(const struct line_info *lines,\n\t\tint line_count, unsigned int b_off)\n{\n\tunsigned int lo;\n\tunsigned int hi;\n\tunsigned int mid;\n\n\tif (lines == NULL || line_count <= 1)\n\t\treturn 0;\n\tlo = 0;\n\thi = (unsigned int)line_count;\n\twhile (lo + 1 < hi) {\n\t\tmid = lo + (hi - lo) / 2;\n\t\tif (lines[mid].b_start <= b_off)\n\t\t\tlo = mid;\n\t\telse\n\t\t\thi = mid;\n\t}\n\treturn (int)lo;\n}\n'''
assert anchor in s
s = s.replace(anchor, helper, 1)

# caret: i remains used below, assign from helper instead of scanning.
old = '''\t\t/* Now find line in which byte offset appears */\n\t\tfor (i = 0; i < ta->line_count - 1; i++)\n\t\t\tif (ta->lines[i + 1].b_start > b_off)\n\t\t\t\tbreak;\n'''
new = '''\t\t/* Find line containing byte offset in logarithmic time. */\n\t\ti = textarea_line_for_offset(ta->lines, ta->line_count, b_off);\n'''
assert old in s
s = s.replace(old, new, 1)

# Selection redraw: replace two scans with two binary searches.
old = '''\t\t/* Find redraw start/end lines */\n\t\tfor (line_end = 0; line_end < ta->line_count - 1; line_end++)\n\t\t\tif (ta->lines[line_end + 1].b_start > b_low) {\n\t\t\t\tline_start = line_end;\n\t\t\t\tbreak;\n\t\t\t}\n\t\tfor (; line_end < ta->line_count - 1; line_end++)\n\t\t\tif (ta->lines[line_end + 1].b_start > b_high)\n\t\t\t\tbreak;\n'''
new = '''\t\t/* Find redraw start/end lines without scanning from line zero. */\n\t\tline_start = textarea_line_for_offset(ta->lines,\n\t\t\tta->line_count, b_low);\n\t\tline_end = textarea_line_for_offset(ta->lines,\n\t\t\tta->line_count, b_high);\n'''
assert old in s
s = s.replace(old, new, 1)

# Reflow already has a local binary search; reuse helper and remove local vars.
s = s.replace('''\tunsigned int start;\n\tunsigned int lo, hi, mid;\n''', '''\tunsigned int start;\n''', 1)
old = '''\t/* Get line containing the start of the change. Line starts are sorted,\n\t * so use upper-bound search instead of walking from line zero on every\n\t * keystroke in a large textarea. */\n\tlo = 0;\n\thi = (unsigned int)ta->line_count;\n\twhile (lo + 1 < hi) {\n\t\tmid = lo + (hi - lo) / 2;\n\t\tif (ta->lines[mid].b_start <= b_start)\n\t\t\tlo = mid;\n\t\telse\n\t\t\thi = mid;\n\t}\n\tstart = lo;\n'''
new = '''\t/* Get line containing the start of the change. */\n\tstart = (unsigned int)textarea_line_for_offset(ta->lines,\n\t\tta->line_count, (unsigned int)b_start);\n'''
assert old in s
s = s.replace(old, new, 1)

p.write_text(s)
