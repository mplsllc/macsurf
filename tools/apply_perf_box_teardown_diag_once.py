#!/usr/bin/env python3
from pathlib import Path
p = Path('browser/netsurf/content/handlers/html/box_manipulate.c')
s = p.read_text()

old = '''/* fixes908 -- recently-freed-box ring for the reconvert double-free hunt.\n * box_talloc_destructor is the single choke point every box passes through on\n * its FIRST free (via box_free_box, or via talloc_free(tree) recursion -- talloc\n * runs the destructor per chunk). Recording (box, during-label) here lets the\n * talloc double-free abort in talloc.c report WHERE the box was first freed --\n * the missing half of the picture (the second-free path is the live during=\n * label). 4096 slots so a whole heavy-page tree-free does not evict the target\n * before the double-free lands. Diagnostic only. */\n#define MACSURF_BOXFREE_RING 4096\nstatic void *g_boxfree_ptr[MACSURF_BOXFREE_RING];\nstatic const char *g_boxfree_ctx[MACSURF_BOXFREE_RING];\nstatic int g_boxfree_ix = 0;\n\nvoid macsurf_box_freed_note(void *box, const char *ctx)\n{\n\tg_boxfree_ptr[g_boxfree_ix] = box;\n\tg_boxfree_ctx[g_boxfree_ix] = (ctx != NULL) ? ctx : "(null)";\n\tg_boxfree_ix++;\n\tif (g_boxfree_ix >= MACSURF_BOXFREE_RING) {\n\t\tg_boxfree_ix = 0;\n\t}\n}\n\nconst char *macsurf_box_freed_lookup(void *box)\n{\n\tint i;\n\tfor (i = 0; i < MACSURF_BOXFREE_RING; i++) {\n\t\tif (g_boxfree_ptr[i] == box) {\n\t\t\treturn g_boxfree_ctx[i];\n\t\t}\n\t}\n\treturn "(not-in-ring)";\n}\n'''
new = '''/* The recently-freed-box ring was diagnostic scaffolding for the resolved\n * reconvert double-free hunt. Keeping it active costs global writes for every\n * box destructor and permanently reserves the static ring. Retain the exported\n * hooks because talloc's fatal diagnostic references them, but make normal\n * teardown allocation- and write-free. */\nvoid macsurf_box_freed_note(void *box, const char *ctx)\n{\n\t(void)box;\n\t(void)ctx;\n}\n\nconst char *macsurf_box_freed_lookup(void *box)\n{\n\t(void)box;\n\treturn "(boxfree-diag-disabled)";\n}\n'''
assert old in s
s = s.replace(old, new, 1)

old = '''\t/* fixes908 -- note this box's first free + the free path in flight. */\n\t{\n\t\textern const char *macsurf_talloc_free_ctx;\n\t\tmacsurf_box_freed_note(b, macsurf_talloc_free_ctx);\n\t}\n\n'''
assert old in s
s = s.replace(old, '', 1)

# Keep the externally referenced counter symbol, but retire its per-box\n# diagnostic read/modify/write from the destructor hot path.\nassert '\t\t\tmacsurf_box_backlink_cleared++;\n' in s
s = s.replace('\t\t\tmacsurf_box_backlink_cleared++;\n', '', 1)

p.write_text(s)
