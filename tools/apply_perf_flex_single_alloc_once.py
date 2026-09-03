#!/usr/bin/env python3
from pathlib import Path
p = Path('browser/netsurf/content/handlers/html/layout_flex.c')
s = p.read_text()

old = '''static void layout_flex_ctx__destroy(struct flex_ctx *ctx)\n{\n\tif (ctx != NULL) {\n\t\tfree(ctx->item.data);\n\t\tif (ctx->line.data != &ctx->line.first)\n\t\t\tfree(ctx->line.data);\n\t\tfree(ctx);\n\t}\n}\n'''
new = '''static void layout_flex_ctx__destroy(struct flex_ctx *ctx)\n{\n\tif (ctx != NULL) {\n\t\tif (ctx->line.data != &ctx->line.first)\n\t\t\tfree(ctx->line.data);\n\t\t/* item.data is the tail of the ctx allocation. */\n\t\tfree(ctx);\n\t}\n}\n'''
assert old in s
s = s.replace(old, new, 1)

old = '''\tstruct flex_ctx *ctx;\n\n\tctx = calloc(1, sizeof(*ctx));\n\tif (ctx == NULL) {\n\t\treturn NULL;\n\t}\n\tctx->line.alloc = 1;\n\n\tctx->item.count = box_count_children(flex);\n\tctx->item.data = calloc(ctx->item.count, sizeof(*ctx->item.data));\n\tif (ctx->item.data == NULL) {\n\t\tlayout_flex_ctx__destroy(ctx);\n\t\treturn NULL;\n\t}\n\n\tctx->line.alloc = 1;\n'''
new = '''\tstruct flex_ctx *ctx;\n\tsize_t item_count;\n\tsize_t alloc_size;\n\n\titem_count = box_count_children(flex);\n\tif (item_count > (((size_t)-1 - sizeof(*ctx)) /\n\t\t\tsizeof(struct flex_item_data))) {\n\t\treturn NULL;\n\t}\n\talloc_size = sizeof(*ctx) +\n\t\titem_count * sizeof(struct flex_item_data);\n\tctx = calloc(1, alloc_size);\n\tif (ctx == NULL) {\n\t\treturn NULL;\n\t}\n\n\tctx->item.count = item_count;\n\tctx->item.data = (struct flex_item_data *)(ctx + 1);\n\tctx->line.alloc = 1;\n'''
assert old in s
s = s.replace(old, new, 1)
p.write_text(s)
