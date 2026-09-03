import re
from pathlib import Path

p = Path('browser/netsurf/content/handlers/html/layout_grid.c')
s = p.read_text()

# Remove diagnostic-only name helpers + logger as one contiguous region.
start = s.find('static const char *layout_grid_justify_name(')
end_marker = '/* fixes168b - Grid local fallback.'
end = s.find(end_marker, start)
if start < 0 or end < 0:
    raise SystemExit('grid justify diagnostic helper region not found')
s = s[:start] + s[end:]

# Remove the size diagnostic call.  Its lh__delta_outer_width argument was
# evaluated solely for logging, so removing the call removes that extra walk.
size_call = '''\tlayout_grid_log_justify("size", item->parent, item,\n\t\t\teffective_justify, cell_width, item->width,\n\t\t\titem->width + lh__delta_outer_width(item), 0, 0);\n\n'''
if s.count(size_call) != 1:
    raise SystemExit('grid size diagnostic call count=%d' % s.count(size_call))
s = s.replace(size_call, '', 1)

# Remove any placement diagnostic call as a statement, preserving surrounding
# layout calculations. The call spans several lines but always terminates with );
s, n = re.subn(r'\n\s*layout_grid_log_justify\("place",.*?\);', '', s,
               count=1, flags=re.S)
if n != 1:
    raise SystemExit('grid place diagnostic call count=%d' % n)

# Remove first-100 GRID marker block. It is pure instrumentation.
marker_start = s.find('\t/* fixes161e - per-call GRID marker capped at first 100 calls per')
if marker_start < 0:
    raise SystemExit('GRID marker start not found')
brace_start = s.find('\t{', marker_start)
if brace_start < 0:
    raise SystemExit('GRID marker brace not found')
depth = 0
marker_end = None
for i in range(brace_start, len(s)):
    if s[i] == '{': depth += 1
    elif s[i] == '}':
        depth -= 1
        if depth == 0:
            marker_end = i + 1
            break
if marker_end is None:
    raise SystemExit('GRID marker end not found')
while marker_end < len(s) and s[marker_end] in '\r\n':
    marker_end += 1
s = s[:marker_start] + s[marker_end:]

p.write_text(s)
