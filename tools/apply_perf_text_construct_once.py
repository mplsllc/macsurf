#!/usr/bin/env python3
from pathlib import Path

p = Path('browser/netsurf/content/handlers/html/box_construct.c')
s = p.read_text()

# Normal/nowrap branch: carry one known length through soft-hyphen handling
# and the final talloc copy. Retire the resolved xf-init substring probe.
s = s.replace('''\t\tchar *text;\n\n\t\ttext = squash_whitespace(dom_string_data(content));\n''', '''\t\tchar *text;\n\t\tsize_t text_len;\n\n\t\ttext = squash_whitespace(dom_string_data(content));\n''', 1)

probe_start = s.find('#ifdef __MACOS9__\n\t\t/* fixes491 diag - trace the source of "data-xf-init" text nodes.')
assert probe_start != -1
probe_end = s.find('#endif\n', probe_start)
assert probe_end != -1
s = s[:probe_start] + '\t\ttext_len = strlen(text);\n\n' + s[probe_end + len('#endif\n'):]

s = s.replace('''\t\tif (text[0] == ' ' && text[1] == 0) {\n''', '''\t\tif (text_len == 1 && text[0] == ' ') {\n''', 1)

old = '''\t\t\tstruct box *prev = props.inline_container->last;\n\t\t\tsize_t tlen = strlen(text);\n\t\t\tsize_t plen = prev->length;\n\t\t\tint new_is_shy = (tlen >= 2 &&\n'''
new = '''\t\t\tstruct box *prev = props.inline_container->last;\n\t\t\tsize_t plen = prev->length;\n\t\t\tint new_is_shy = (text_len >= 2 &&\n'''
assert old in s
s = s.replace(old, new, 1)
s = s.replace('''\t\t\tif ((new_is_shy || prev_shy) && tlen > 0) {\n\t\t\t\tchar *merged = talloc_realloc(ctx->bctx,\n\t\t\t\t\tprev->text, char, plen + tlen + 1);\n\t\t\t\tif (merged != NULL) {\n\t\t\t\t\tmemcpy(merged + plen, text, tlen);\n\t\t\t\t\tmerged[plen + tlen] = '\\0';\n\t\t\t\t\tprev->text = merged;\n\t\t\t\t\tprev->length = plen + tlen;\n''', '''\t\t\tif ((new_is_shy || prev_shy) && text_len > 0) {\n\t\t\t\tchar *merged = talloc_realloc(ctx->bctx,\n\t\t\t\t\tprev->text, char, plen + text_len + 1);\n\t\t\t\tif (merged != NULL) {\n\t\t\t\t\tmemcpy(merged + plen, text, text_len);\n\t\t\t\t\tmerged[plen + text_len] = '\\0';\n\t\t\t\t\tprev->text = merged;\n\t\t\t\t\tprev->length = plen + text_len;\n''', 1)

old = '''\t\tbox->text = talloc_strdup(ctx->bctx, text);\n\t\tfree(text);\n\t\tif (box->text == NULL)\n\t\t\treturn false;\n\n\t\tbox->length = strlen(box->text);\n'''
new = '''\t\tbox->text = talloc_memdup(ctx->bctx, text, text_len + 1);\n\t\tfree(text);\n\t\tif (box->text == NULL)\n\t\t\treturn false;\n\n\t\tbox->length = text_len;\n'''
assert old in s
s = s.replace(old, new, 1)

# Pre/pre-wrap/pre-line: text_len is already maintained exactly. Use it for
# transform instead of strlen, and use each strcspn result as the copy length.
s = s.replace('''\t\t\tbox_text_transform(text, strlen(text),\n''', '''\t\t\tbox_text_transform(text, text_len,\n''', 1)

old = '''\t\t\tbox->text = talloc_strdup(ctx->bctx, current);\n\t\t\tif (box->text == NULL) {\n\t\t\t\tfree(text);\n\t\t\t\treturn false;\n\t\t\t}\n\n\t\t\tbox->length = strlen(box->text);\n'''
new = '''\t\t\tbox->text = talloc_memdup(ctx->bctx, current, len + 1);\n\t\t\tif (box->text == NULL) {\n\t\t\t\tfree(text);\n\t\t\t\treturn false;\n\t\t\t}\n\n\t\t\tbox->length = len;\n'''
assert old in s
s = s.replace(old, new, 1)

p.write_text(s)
