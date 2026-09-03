#!/usr/bin/env python3
from pathlib import Path

p = Path('browser/netsurf/content/handlers/html/box_construct.c')
s = p.read_text()

marker = '''/**\n * Construct the box tree for an XML text node.\n'''
assert marker in s
helper = '''/* Box-construction variant of squash_whitespace. The generic helper returns\n * a malloc buffer which this path immediately copied into bctx; allocate in\n * bctx up front so the collapsed buffer can become box->text directly. */\nstatic char *\nbox_squash_whitespace(void *bctx, const char *s, size_t *out_len)\n{\n\tchar *c;\n\tsize_t i = 0;\n\tsize_t j = 0;\n\tsize_t n;\n\n\tassert(s != NULL);\n\tassert(out_len != NULL);\n\tn = strlen(s);\n\tc = talloc_array(bctx, char, n + 1);\n\tif (c == NULL)\n\t\treturn NULL;\n\tdo {\n\t\tif (s[i] == ' ' || s[i] == '\\n' ||\n\t\t\t\ts[i] == '\\r' || s[i] == '\\t') {\n\t\t\tc[j++] = ' ';\n\t\t\twhile (s[i] == ' ' || s[i] == '\\n' ||\n\t\t\t\t\ts[i] == '\\r' || s[i] == '\\t')\n\t\t\t\ti++;\n\t\t}\n\t\tc[j++] = s[i++];\n\t} while (s[i - 1] != 0);\n\t*out_len = (j == 0) ? 0 : j - 1;\n\treturn c;\n}\n\n\n'''
if 'box_squash_whitespace(' not in s:
    s = s.replace(marker, helper + marker, 1)

old = '''\t\ttext = squash_whitespace(dom_string_data(content));\n\n\t\tdom_string_unref(content);\n\n\t\tif (text == NULL)\n\t\t\treturn false;\n\n\t\ttext_len = strlen(text);\n'''
new = '''\t\ttext = box_squash_whitespace(ctx->bctx,\n\t\t\tdom_string_data(content), &text_len);\n\n\t\tdom_string_unref(content);\n\n\t\tif (text == NULL)\n\t\t\treturn false;\n'''
assert old in s
s = s.replace(old, new, 1)

# All temporary-buffer release paths in this branch now release a talloc child.
# Limit replacements to the function region before the PRE/PRE_WRAP branch.
start = s.index('static bool box_construct_text(')
branch_end = s.index('\t} else {', start)
prefix = s[:start]
body = s[start:branch_end]
suffix = s[branch_end:]
body = body.replace('free(text);', 'talloc_free(text);')

old_final = '''\t\tbox->text = talloc_memdup(ctx->bctx, text, text_len + 1);\n\t\ttalloc_free(text);\n\t\tif (box->text == NULL)\n\t\t\treturn false;\n\n\t\tbox->length = text_len;\n'''
new_final = '''\t\t/* text is already a bctx-owned allocation; transfer it directly. */\n\t\tbox->text = text;\n\t\tbox->length = text_len;\n'''
assert old_final in body
body = body.replace(old_final, new_final, 1)
s = prefix + body + suffix

# Ensure this normal branch no longer uses the generic two-allocation helper.
normal = s[start:s.index('\t} else {', start)]
assert 'squash_whitespace(dom_string_data(content))' not in normal
assert 'talloc_memdup(ctx->bctx, text, text_len + 1)' not in normal
assert 'free(text);' not in normal

p.write_text(s)
