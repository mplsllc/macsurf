#!/usr/bin/env python3
from pathlib import Path
p = Path('browser/netsurf/content/handlers/html/box_construct.c')
s = p.read_text()
old = '''static void box_construct_element_after(struct box_construct_ctx *ctx,\n\t\tdom_node *n, html_content *content)\n{\n\tstruct box_construct_props props;\n\tstruct box *box = box_for_node(n);\n\n\tassert(box != NULL);\n\n\tbox_extract_properties(n, &props);\n\n\tif (box->type == BOX_INLINE || box->type == BOX_BR) {\n'''
new = '''static void box_construct_element_after(struct box_construct_ctx *ctx,\n\t\tdom_node *n, html_content *content)\n{\n\tstruct box *box = box_for_node(n);\n\n\tassert(box != NULL);\n\n\tif (box->type == BOX_INLINE || box->type == BOX_BR) {\n\t\tstruct box_construct_props props;\n'''
assert old in s
s=s.replace(old,new,1)

# Insert direct box-tree recovery before child-node query.
old = '''\t\tstruct box *inline_end;\n\t\tbool has_children;\n\t\tdom_exception err;\n\n\t\terr = dom_node_has_child_nodes(n, &has_children);\n'''
new = '''\t\tstruct box *inline_end;\n\t\tbool has_children;\n\t\tdom_exception err;\n\n\t\tmemset(&props, 0, sizeof(props));\n\t\t/* Inline boxes are attached to their INLINE_CONTAINER at element-open\n\t\t * time. Recover close-time placement directly from those box links; the\n\t\t * DOM ancestor walk is only a fallback for an unexpected tree shape. */\n\t\tif (box->parent != NULL &&\n\t\t\t\tbox->parent->type == BOX_INLINE_CONTAINER) {\n\t\t\tprops.inline_container = box->parent;\n\t\t\tprops.containing_block = box->parent->parent;\n\t\t} else {\n\t\t\tbox_extract_properties(n, &props);\n\t\t}\n\n\t\terr = dom_node_has_child_nodes(n, &has_children);\n'''
assert old in s
s=s.replace(old,new,1)
p.write_text(s)
