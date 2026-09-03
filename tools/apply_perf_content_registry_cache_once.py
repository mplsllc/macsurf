#!/usr/bin/env python3
from pathlib import Path
p = Path('browser/netsurf/frontends/macos9/macos9_content_registry.c')
s = p.read_text()

old = '''/**\n * Find the slot index holding c, or -1 if absent.\n */\nstatic int\nmacos9_content_find(struct content *c)\n{\n\tint i;\n\n\tif (c == NULL)\n\t\treturn -1;\n\n\tfor (i = 0; i < MACOS9_CONTENT_REGISTRY_CAP; i++) {\n\t\tif (macos9_content_table[i].c == c)\n\t\t\treturn i;\n\t}\n\n\treturn -1;\n}\n'''
new = '''/* Small epoch-tagged lookup cache.  The live table can change only when\n * macos9_content_registry_epoch changes, so a cached slot/absence from the\n * current epoch is exact.  Box construction asks about the same html_content\n * before and after every element; this turns those 256-slot scans into one\n * pointer/hash check after the first lookup in a build. */\n#define MACOS9_CONTENT_FIND_CACHE 8\nstruct macos9_content_find_cache_entry {\n\tstruct content *c;\n\tunsigned long epoch;\n\tint idx;\n};\nstatic struct macos9_content_find_cache_entry\n\tmacos9_content_find_cache[MACOS9_CONTENT_FIND_CACHE];\n\n/**\n * Find the slot index holding c, or -1 if absent.\n */\nstatic int\nmacos9_content_find(struct content *c)\n{\n\tunsigned long h;\n\tstruct macos9_content_find_cache_entry *ce;\n\tint i;\n\n\tif (c == NULL)\n\t\treturn -1;\n\n\th = (((unsigned long)c) >> 4) & (MACOS9_CONTENT_FIND_CACHE - 1);\n\tce = &macos9_content_find_cache[h];\n\tif (ce->c == c && ce->epoch == macos9_content_registry_epoch)\n\t\treturn ce->idx;\n\n\tfor (i = 0; i < MACOS9_CONTENT_REGISTRY_CAP; i++) {\n\t\tif (macos9_content_table[i].c == c) {\n\t\t\tce->c = c;\n\t\t\tce->epoch = macos9_content_registry_epoch;\n\t\t\tce->idx = i;\n\t\t\treturn i;\n\t\t}\n\t}\n\n\tce->c = c;\n\tce->epoch = macos9_content_registry_epoch;\n\tce->idx = -1;\n\treturn -1;\n}\n'''
assert old in s
s = s.replace(old, new, 1)

# Update stale file header claim that all operations are necessarily O(N).
s = s.replace(''' * (content pointer, generation) slots.  No malloc; all operations are O(N)\n * linear scans over a small table, which is cheap because the live-content\n * count on OS 9 is tiny (a page plus its sub-resources - tens, not\n * thousands).  C89 / CW8-clean: all declarations at the top of their block,\n''', ''' * (content pointer, generation) slots.  No malloc. The authoritative table\n * remains a linear array, with an epoch-tagged pointer cache for repeated\n * lookups on hot paths such as box construction and JS geometry.\n * C89 / CW8-clean: all declarations at the top of their block,\n''', 1)
p.write_text(s)
