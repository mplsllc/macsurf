#!/usr/bin/env python3
from pathlib import Path
p = Path('browser/netsurf/content/handlers/html/box_construct.c')
s = p.read_text()
old = '''static dom_node *\nnext_node(struct box_construct_ctx *ctx, dom_node *n,\n\t\thtml_content *content, bool convert_children)\n{\n\tdom_node *next = NULL;\n\tbool has_children;\n\tdom_exception err;\n\n\terr = dom_node_has_child_nodes(n, &has_children);\n\tif (err != DOM_NO_ERR) {\n\t\tdom_node_unref(n);\n\t\treturn NULL;\n\t}\n\n\tif (convert_children && has_children) {\n\t\terr = dom_node_get_first_child(n, &next);\n\t\tif (err != DOM_NO_ERR) {\n\t\t\tdom_node_unref(n);\n\t\t\treturn NULL;\n\t\t}\n\t\tdom_node_unref(n);\n\t} else {\n\t\terr = dom_node_get_next_sibling(n, &next);\n'''
new = '''static dom_node *\nnext_node(struct box_construct_ctx *ctx, dom_node *n,\n\t\thtml_content *content, bool convert_children)\n{\n\tdom_node *next = NULL;\n\tdom_exception err;\n\n\t/* get_first_child already returns NULL when there is no child. Avoid a\n\t * separate dom_node_has_child_nodes dispatch on every traversal step. */\n\tif (convert_children) {\n\t\terr = dom_node_get_first_child(n, &next);\n\t\tif (err != DOM_NO_ERR) {\n\t\t\tdom_node_unref(n);\n\t\t\treturn NULL;\n\t\t}\n\t\tif (next != NULL) {\n\t\t\tdom_node_unref(n);\n\t\t\treturn next;\n\t\t}\n\t}\n\n\t{\n\t\terr = dom_node_get_next_sibling(n, &next);\n'''
assert old in s
s=s.replace(old,new,1)
# Function's original else closes just before return; change that matching closing brace to our scope is structurally same.
p.write_text(s)
