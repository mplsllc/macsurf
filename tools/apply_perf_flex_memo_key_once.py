#!/usr/bin/env python3
from pathlib import Path

boxp = Path('browser/netsurf/content/handlers/html/box.h')
flexp = Path('browser/netsurf/content/handlers/html/layout_flex.c')

s = boxp.read_text()
old = '''\tunsigned flex_layout_gen;\n\tint flex_layout_width;\n\tint flex_layout_height;\n'''
new = '''\tunsigned flex_layout_gen;\n\tint flex_layout_available_width;\n\tint flex_layout_width;\n\tint flex_layout_height;\n'''
if old not in s:
    raise SystemExit('box.h memo fields anchor not found')
s = s.replace(old, new, 1)
boxp.write_text(s)

s = flexp.read_text()
old = '''\tif (macsurf_flex_layout_cache_enabled &&\n\t\t\tb->flex_layout_gen == macsurf_layout_pass_gen &&\n\t\t\tb->flex_layout_width == available_width &&\n\t\t\tb->flex_layout_width == b->width &&\n\t\t\tb->flex_layout_height == b->height) {\n\t\treturn true;\n\t}\n'''
new = '''\tif (macsurf_flex_layout_cache_enabled &&\n\t\t\tb->flex_layout_gen == macsurf_layout_pass_gen &&\n\t\t\tb->flex_layout_available_width == available_width &&\n\t\t\tb->flex_layout_width == b->width &&\n\t\t\tb->flex_layout_height == b->height) {\n\t\treturn true;\n\t}\n'''
if old not in s:
    raise SystemExit('flex memo hit anchor not found')
s = s.replace(old, new, 1)

old = '''\tif (success) {\n\t\tb->flex_layout_gen = macsurf_layout_pass_gen;\n\t\tb->flex_layout_width = b->width;\n\t\tb->flex_layout_height = b->height;\n\t}\n'''
new = '''\tif (success) {\n\t\tb->flex_layout_gen = macsurf_layout_pass_gen;\n\t\tb->flex_layout_available_width = available_width;\n\t\tb->flex_layout_width = b->width;\n\t\tb->flex_layout_height = b->height;\n\t}\n'''
if old not in s:
    raise SystemExit('flex memo store anchor not found')
s = s.replace(old, new, 1)

# Update stale nearby comments so the invariant is explicit.
s = s.replace('''\t * Correctness invariant for the skip: a box laid out is a\n\t * deterministic function of (b->width, b->style, content) -- its\n''', '''\t * Correctness invariant for the skip: a box laid out is a\n\t * deterministic function of (available_width, b->width, b->style, content) -- its\n''', 1)

flexp.write_text(s)
