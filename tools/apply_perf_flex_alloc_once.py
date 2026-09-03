#!/usr/bin/env python3
from pathlib import Path

p = Path('browser/netsurf/content/handlers/html/layout_flex.c')
s = p.read_text()

old = '''\tstruct flex_lines {\n\t\tsize_t count;\n\t\tsize_t alloc;\n\t\tstruct flex_line_data *data;\n\t} line;\n'''
new = '''\tstruct flex_lines {\n\t\tsize_t count;\n\t\tsize_t alloc;\n\t\tstruct flex_line_data *data;\n\t\tstruct flex_line_data first;\n\t} line;\n'''
assert old in s
s = s.replace(old, new, 1)

old = '''\tif (ctx != NULL) {\n\t\tfree(ctx->item.data);\n\t\tfree(ctx->line.data);\n\t\tfree(ctx);\n\t}\n'''
new = '''\tif (ctx != NULL) {\n\t\tfree(ctx->item.data);\n\t\tif (ctx->line.data != &ctx->line.first)\n\t\t\tfree(ctx->line.data);\n\t\tfree(ctx);\n\t}\n'''
assert old in s
s = s.replace(old, new, 1)

old = '''\tctx->line.alloc = 1;\n\tctx->line.data = calloc(ctx->line.alloc, sizeof(*ctx->line.data));\n\tif (ctx->line.data == NULL) {\n\t\tlayout_flex_ctx__destroy(ctx);\n\t\treturn NULL;\n\t}\n'''
new = '''\tctx->line.alloc = 1;\n\tctx->line.data = &ctx->line.first;\n'''
assert old in s
s = s.replace(old, new, 1)

old = '''\ttemp = realloc(ctx->line.data, sizeof(*ctx->line.data) * line_alloc);\n\tif (temp == NULL) {\n\t\treturn false;\n\t}\n'''
new = '''\tif (ctx->line.data == &ctx->line.first) {\n\t\ttemp = malloc(sizeof(*ctx->line.data) * line_alloc);\n\t\tif (temp != NULL)\n\t\t\ttemp[0] = ctx->line.first;\n\t} else {\n\t\ttemp = realloc(ctx->line.data,\n\t\t\tsizeof(*ctx->line.data) * line_alloc);\n\t}\n\tif (temp == NULL) {\n\t\treturn false;\n\t}\n'''
assert old in s
s = s.replace(old, new, 1)

# Retire the remaining capped FLEXPHASE diagnostic block; it executes on
# every successful flex population and has no behavioral role.
start = s.find('\n\t{\n\t\textern long macsurf_layout_seq;\n\t\tstatic long macsurf_flexphase2_seq = -1;')
assert start != -1
end_marker = '\n\t}\n'
end = s.find(end_marker, start)
assert end != -1
# Find the block's closing brace after the log call, not the inner if.
end = s.find(end_marker, end + len(end_marker))
assert end != -1
s = s[:start] + '\n' + s[end + len(end_marker):]

p.write_text(s)
