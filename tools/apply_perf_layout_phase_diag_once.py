#!/usr/bin/env python3
from pathlib import Path
import re
p = Path('browser/netsurf/content/handlers/html/layout.c')
s = p.read_text()

# Per-table entry marker.
s, n = re.subn(r'\n\t/\* fixes161e - per-call TABLE marker.*?\n\t\}\n', '\n', s, count=1, flags=re.S)
assert n == 1, 'TABLE marker block not found'

# Pre- and post-distribution table diagnostic blocks.
s, n1 = re.subn(r'\n\t/\* fixes572 TBLDIAG - one-round diagnostic.*?\n\t\}\n', '\n', s, count=1, flags=re.S)
s, n2 = re.subn(r'\n\t/\* fixes572 TBLDIAG - final column widths.*?\n\t\}\n', '\n', s, count=1, flags=re.S)
assert n1 == 1 and n2 == 1, 'TBLDIAG blocks not found'

# Function-scope block-layout phase counter plus entry log.
s, n = re.subn(r'\n\t/\* fixes161f - function-scope phase counters.*?\n\t\}\n', '\n', s, count=1, flags=re.S)
assert n == 1, 'block entry diagnostic not found'

# Block pre-loop and completion markers.
s, n1 = re.subn(r'\n\t/\* fixes161f - pre-loop marker.*?\n\t\}\n', '\n', s, count=1, flags=re.S)
s, n2 = re.subn(r'\n\t/\* fixes161f - exit marker.*?\n\t\}\n', '\n', s, count=1, flags=re.S)
assert n1 == 1 and n2 == 1, 'block pre/exit diagnostic not found'

# Per-child block diagnostic. Match the self-contained scope containing its
# own child counter/sequence; no layout state escapes this scope.
s, n = re.subn(r'\n\t\t\{\n\t\t\tstatic long lbc_child_calls = 0;.*?\n\t\t\}\n', '\n', s, count=1, flags=re.S)
assert n == 1, 'block child diagnostic not found'

# Document entry/exit logs; keep macsurf_layout_seq++ temporarily because the
# residual flex phase probes may still use the generation until their separate
# cleanup commit lands.
s, n1 = re.subn(r'\n\tmacsurf_debug_log_writef\(\n\t\t"LAYOUTPHASE document entry.*?\n\t\twidth, height, \(void \*\)doc\);\n', '\n', s, count=1, flags=re.S)
s, n2 = re.subn(r'\n\t/\* fixes161d - see entry probe\. \*/\n\tmacsurf_debug_log_writef\(\n\t\t"LAYOUTPHASE document exit.*?\n\t\t\(int\)ret, \(int\)doc->width, \(int\)doc->height\);\n', '\n', s, count=1, flags=re.S)
assert n1 == 1 and n2 == 1, 'document diagnostics not found'

for token in ('LAYOUTPHASE table', 'LAYOUTPHASE block', 'LAYOUTPHASE document',
              'TBLDIAG box=', 'TBLPICK box=', 'macsurf_lbc_calls',
              'lbc_child_calls', 'tbl_diag_calls', 'tbl_fin_calls'):
    assert token not in s, token + ' remains'

p.write_text(s)
