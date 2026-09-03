#!/usr/bin/env python3
from pathlib import Path
p=Path('browser/netsurf/content/handlers/html/box_construct.c')
s=p.read_text()
old='''\t/* create box for this element */\n\tcomputed_display = ns_computed_display(style, box_is_root(n));\n'''
new='''\t/* The owning box already identifies the document root. Avoid a DOM\n\t * parent/type probe for generated content and compute display once. */\n\tcomputed_display = ns_computed_display(style, ctx->root_box == box);\n'''
assert old in s
s=s.replace(old,new,1)
old='''\t\t/* set box type from computed display */\n\t\tgen->type = box_map[ns_computed_display(\n\t\t\t\tstyle, box_is_root(n))];\n'''
new='''\t\t/* set box type from the display value already resolved above */\n\t\tgen->type = box_map[computed_display];\n'''
assert old in s
s=s.replace(old,new,1)
p.write_text(s)
