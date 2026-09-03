#!/usr/bin/env python3
from pathlib import Path
import re

p = Path('browser/netsurf/content/handlers/html/box_construct.c')
s = p.read_text()

# The first-150 per-node traces are now compile-gated behind
# MACSURF_VERBOSE_RECONVERT. Keep that optional diagnostic path. Remove the
# remaining unconditional durable FlushVol checkpoints from successful
# reconverts, plus the unused text-box counter.
s = s.replace('/* Diagnostic: count text boxes constructed during DOM->box conversion. */\nlong macos9_box_text_created = 0;\n', '')
s = s.replace('\t\tmacos9_box_text_created++;\n', '')

# One unconditional durable write before the atomic reconvert walk.
s, n = re.subn(r'\n\t/\* fixes901 - durable marker: THIS batch.*?\n\t}\n\n\tdo \{',
               '\n\tdo {', s, count=1, flags=re.S)
assert n == 1, 'batch-enter checkpoint not found'

# Successful completion durable write + freemem log. Keep the Style-B cache
# summary immediately after it.
s, n = re.subn(r'\n\t\t\tif \(macsurf_reconvert_in_progress\) \{\n\t\t\t\t/\* fixes895 - all nodes walked.*?\n\t\t\t}\n\n\t\t\tmacsurf_reconv_style_cache_report',
               '\n\t\t\tmacsurf_reconv_style_cache_report', s, count=1, flags=re.S)
assert n == 1, 'completion checkpoint not found'

# Atomic reconverts cannot reach the cold-load reschedule path, so its reconvert
# branch is dead. Cold-load scheduling itself is untouched.
s, n = re.subn(r'\n\tif \(macsurf_reconvert_in_progress\) \{\n\t\t/\* fixes895 - durable per-batch checkpoint.*?\n\t}\n\tguit->misc->schedule',
               '\n\tguit->misc->schedule', s, count=1, flags=re.S)
assert n == 1, 'batch-yield diagnostic not found'

# The wrapper pre-drain checkpoint may already have been removed by concurrent
# cleanup. If present, retire only the diagnostic block; the actual drain stays.
s = re.sub(r'\n\t/\* fixes901 - durable checkpoint BEFORE.*?\n\tif \(macsurf_reconvert_in_progress\) \{.*?\n\t}\n\tmacos9_content_drain_deferred\(\);',
           '\n\tmacos9_content_drain_deferred();', s, count=1, flags=re.S)

assert 'macos9_box_text_created' not in s
p.write_text(s)
