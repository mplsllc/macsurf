#!/usr/bin/env python3
from pathlib import Path
import re
p = Path('browser/netsurf/content/handlers/html/layout_flex.c')
s = p.read_text()
pattern = re.compile(r'\n\t\{\n\t\textern long macsurf_layout_seq;\n\t\tstatic long macsurf_flexphase(?:3|4|5)?_seq = -1;.*?\n\t\}\n', re.S)
s, n = pattern.subn('\n', s)
assert n == 4, 'expected 4 residual FLEXPHASE blocks, got %d' % n
assert 'FLEXPHASE box=' not in s
p.write_text(s)
