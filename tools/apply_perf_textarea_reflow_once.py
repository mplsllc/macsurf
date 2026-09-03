#!/usr/bin/env python3
from pathlib import Path
p = Path('browser/netsurf/desktop/textarea.c')
s = p.read_text()

old = '''#ifdef __MACOS9__\n/* fixes758 (#212) - DIAGNOSTIC ONLY: per-reflow timing + scope, to find why\n * typing in a big multiline textarea crawls. Externs kept local so no Mac\n * headers leak into core textarea.c. Remove with the log lines once diagnosed. */\nextern void macsurf_debug_log_writef(const char *fmt, ...);\nextern void Microseconds(UnsignedWide *microTickCount);\n#endif\n\n'''
assert old in s
s = s.replace(old, '', 1)

old = '''\tchar *text;\n\tunsigned int len;\n\tunsigned int start;\n\tsize_t b_off;\n\tsize_t b_start_line_end;\n\tint x;\n\tchar *space, *para_end;\n\tunsigned int line; /* line count */\n'''
new = '''\tchar *text;\n\tunsigned int len;\n\tunsigned int start;\n\tunsigned int lo, hi, mid;\n\tsize_t b_off;\n\tsize_t b_start_line_end;\n\tint x;\n\tchar *space, *para_end;\n\tchar *para_end_cache;\n\tunsigned int line; /* line count */\n'''
assert old in s
s = s.replace(old, new, 1)

old = '''\tbool restart = false;\n\tbool skip_line = false;\n#ifdef __MACOS9__\n\tUnsignedWide _rt0, _rt1; int _restart_n = 0; unsigned int _start0 = 0;\n\tMicroseconds(&_rt0);\n#endif\n\n'''
new = '''\tbool restart = false;\n\tbool skip_line = false;\n\n'''
assert old in s
s = s.replace(old, new, 1)

old = '''\t/* Get line of start of changes */\n\tfor (start = 0; (signed) start < ta->line_count - 1; start++)\n\t\tif (ta->lines[start + 1].b_start > b_start)\n\t\t\tbreak;\n'''
new = '''\t/* Get line containing the start of the change. Line starts are sorted,\n\t * so use upper-bound search instead of walking from line zero on every\n\t * keystroke in a large textarea. */\n\tlo = 0;\n\thi = (unsigned int)ta->line_count;\n\twhile (lo + 1 < hi) {\n\t\tmid = lo + (hi - lo) / 2;\n\t\tif (ta->lines[mid].b_start <= b_start)\n\t\t\tlo = mid;\n\t\telse\n\t\t\thi = mid;\n\t}\n\tstart = lo;\n'''
assert old in s
s = s.replace(old, new, 1)

old = '''\t/* Record original end pos of start line */\n\tb_start_line_end = ta->lines[start].b_start + ta->lines[start].b_length;\n#ifdef __MACOS9__\n\t_start0 = start;\n#endif\n\n'''
new = '''\t/* Record original end pos of start line */\n\tb_start_line_end = ta->lines[start].b_start + ta->lines[start].b_length;\n\n'''
assert old in s
s = s.replace(old, new, 1)

# Initialize the absolute next-newline cache before the restartable layout loop.
old = '''\t/* During layout we may decide we need to restart again from the\n\t * textarea's first line. */\n\tdo {\n'''
new = '''\t/* During layout we may decide we need to restart again from the\n\t * textarea's first line. para_end_cache is an absolute pointer into the\n\t * stable text buffer; soft wraps reuse it instead of rescanning the rest\n\t * of the same paragraph for '\\n' on every visual line. */\n\tpara_end_cache = NULL;\n\tdo {\n'''
assert old in s
s = s.replace(old, new, 1)

old = '''\t\tfor (; len > 0; len -= b_off, text += b_off) {\n\t\t\t/* Find end of paragraph */\n\t\t\tfor (para_end = text; para_end < text + len;\n\t\t\t\t\tpara_end++) {\n\t\t\t\tif (*para_end == '\\n')\n\t\t\t\t\tbreak;\n\t\t\t}\n\n'''
new = '''\t\tfor (; len > 0; len -= b_off, text += b_off) {\n\t\t\t/* Find end of paragraph once. While soft-wrapping the same\n\t\t\t * paragraph, text advances but the newline pointer does not. */\n\t\t\tif (para_end_cache == NULL || para_end_cache < text) {\n\t\t\t\tfor (para_end = text; para_end < text + len;\n\t\t\t\t\t\tpara_end++) {\n\t\t\t\t\tif (*para_end == '\\n')\n\t\t\t\t\t\tbreak;\n\t\t\t\t}\n\t\t\t\tpara_end_cache = para_end;\n\t\t\t} else {\n\t\t\t\tpara_end = para_end_cache;\n\t\t\t}\n\n'''
assert old in s
s = s.replace(old, new, 1)

# Restart changes visible width and resets text/start, so invalidate paragraph cache.
old = '''\t\tif (restart)\n\t\t\tstart = 0;\n\n\t\t/* Set current line to the starting line */\n'''
new = '''\t\tif (restart) {\n\t\t\tstart = 0;\n\t\t\tpara_end_cache = NULL;\n\t\t}\n\n\t\t/* Set current line to the starting line */\n'''
assert old in s
s = s.replace(old, new, 1)

# Remove retired restart counters.
s = s.replace('''\t\t\trestart = true;\n#ifdef __MACOS9__\n\t\t\t_restart_n++;\n#endif\n''', '''\t\t\trestart = true;\n''')

old = '''\tta->h_extent = h_extent;\n\tta->v_extent = v_extent;\n\tta->line_count = line;\n#ifdef __MACOS9__\n\tMicroseconds(&_rt1);\n\tmacsurf_debug_log_writef(\n\t\t"RECON TAref len=%ld start=%ld lines=%ld restart=%d dt=%ldus",\n\t\t(long)ta->text.len, (long)_start0, (long)line, _restart_n,\n\t\t(long)(_rt1.lo - _rt0.lo));\n#endif\n\n'''
new = '''\tta->h_extent = h_extent;\n\tta->v_extent = v_extent;\n\tta->line_count = line;\n\n'''
assert old in s
s = s.replace(old, new, 1)

p.write_text(s)
