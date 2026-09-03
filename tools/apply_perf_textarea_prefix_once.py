#!/usr/bin/env python3
from pathlib import Path
p = Path('browser/netsurf/desktop/textarea.c')
s = p.read_text()

old = '''struct line_info {\n\tunsigned int b_start;\t\t/**< Byte offset of line start */\n\tunsigned int b_length;\t\t/**< Byte length of line */\n\tint width;\t\t\t/**< Width in pixels of line */\n};\n'''
new = '''struct line_info {\n\tunsigned int b_start;\t\t/**< Byte offset of line start */\n\tunsigned int b_length;\t\t/**< Byte length of line */\n\tint width;\t\t\t/**< Width in pixels of line */\n\tint prefix_max_width;\t\t/**< Max width through this line */\n};\n'''
assert old in s
s = s.replace(old, new, 1)

old = '''\t\tif (line != 0) {\n\t\t\t/* Not starting at the beginning of the textarea, so\n\t\t\t * jump forward, and make sure the horizontal extents\n\t\t\t * accommodate the width of the skipped lines. */\n\t\t\tunsigned int i;\n\t\t\tlen -= ta->lines[line].b_start;\n\t\t\ttext += ta->lines[line].b_start;\n\n\t\t\tfor (i = 0; i < line; i++) {\n\t\t\t\tif (ta->lines[i].width > h_extent) {\n\t\t\t\t\th_extent = ta->lines[i].width;\n\t\t\t\t}\n\t\t\t}\n\t\t}\n'''
new = '''\t\tif (line != 0) {\n\t\t\t/* Not starting at the beginning of the textarea. The prefix\n\t\t\t * maximum is maintained as lines are written, so recovering the\n\t\t\t * unchanged prefix extent is O(1), not a scan from line zero. */\n\t\t\tlen -= ta->lines[line].b_start;\n\t\t\ttext += ta->lines[line].b_start;\n\t\t\tif (ta->lines[line - 1].prefix_max_width > h_extent)\n\t\t\t\th_extent = ta->lines[line - 1].prefix_max_width;\n\t\t}\n'''
assert old in s
s = s.replace(old, new, 1)

# Add helper-like inline assignment after every multiline line width write.
# Empty textarea case.
old = '''\t\t\tta->lines[line].b_start = 0;\n\t\t\tta->lines[line].b_length = 0;\n\t\t\tta->lines[line++].width = 0;\n\t\t\tta->line_count = 1;\n'''
new = '''\t\t\tta->lines[line].b_start = 0;\n\t\t\tta->lines[line].b_length = 0;\n\t\t\tta->lines[line].width = 0;\n\t\t\tta->lines[line].prefix_max_width =\n\t\t\t\t(line == 0) ? 0 : ta->lines[line - 1].prefix_max_width;\n\t\t\tline++;\n\t\t\tta->line_count = 1;\n'''
assert old in s
s = s.replace(old, new, 1)

# Newline-terminated visual line.
old = '''\t\t\t\tta->lines[line].b_start = text - ta->text.data;\n\t\t\t\tta->lines[line].b_length = para_end - text;\n\t\t\t\tta->lines[line++].width = x;\n'''
new = '''\t\t\t\tta->lines[line].b_start = text - ta->text.data;\n\t\t\t\tta->lines[line].b_length = para_end - text;\n\t\t\t\tta->lines[line].width = x;\n\t\t\t\tta->lines[line].prefix_max_width =\n\t\t\t\t\t(line == 0 || x > ta->lines[line - 1].prefix_max_width) ?\n\t\t\t\t\tx : ta->lines[line - 1].prefix_max_width;\n\t\t\t\tline++;\n'''
assert old in s
s = s.replace(old, new, 1)

# Trailing empty line after newline.
old = '''\t\t\t\t\tta->lines[line].b_start = text +\n\t\t\t\t\t\t\tb_off - ta->text.data;\n\t\t\t\t\tta->lines[line].b_length = 0;\n\t\t\t\t\tta->lines[line++].width = x;\n'''
new = '''\t\t\t\t\tta->lines[line].b_start = text +\n\t\t\t\t\t\t\tb_off - ta->text.data;\n\t\t\t\t\tta->lines[line].b_length = 0;\n\t\t\t\t\tta->lines[line].width = x;\n\t\t\t\t\tta->lines[line].prefix_max_width =\n\t\t\t\t\t\t(line == 0 || x > ta->lines[line - 1].prefix_max_width) ?\n\t\t\t\t\t\tx : ta->lines[line - 1].prefix_max_width;\n\t\t\t\t\tline++;\n'''
assert old in s
s = s.replace(old, new, 1)

# Normal/soft-wrapped line.
old = '''\t\t\tta->lines[line].b_start = text - ta->text.data;\n\t\t\tta->lines[line].b_length = b_off;\n\t\t\tta->lines[line++].width = x;\n'''
new = '''\t\t\tta->lines[line].b_start = text - ta->text.data;\n\t\t\tta->lines[line].b_length = b_off;\n\t\t\tta->lines[line].width = x;\n\t\t\tta->lines[line].prefix_max_width =\n\t\t\t\t(line == 0 || x > ta->lines[line - 1].prefix_max_width) ?\n\t\t\t\tx : ta->lines[line - 1].prefix_max_width;\n\t\t\tline++;\n'''
assert old in s
s = s.replace(old, new, 1)

p.write_text(s)
