#!/usr/bin/env python3
from pathlib import Path
p = Path('browser/netsurf/content/handlers/html/box_construct.c')
s = p.read_text()
start = s.index('static void\nbox_extract_properties(dom_node *n, struct box_construct_props *props)\n{')
end = s.index('\n\n\n/**\n * Get the style for an element.', start)
new = r'''static void
box_extract_properties(dom_node *n, struct box_construct_props *props)
{
	dom_node *current_node;
	dom_node *parent_node;
	struct box *b;
	dom_exception err;
	int first_parent;
	int have_parent_box;

	memset(props, 0, sizeof(*props));

	/* One ancestor walk supplies both logically distinct answers:
	 *  - the nearest boxed ancestor supplies inherited style/link state;
	 *  - the nearest non-inline/non-contents/non-BR box is the containing
	 *    block.  The old code walked the same DOM chain twice to get them. */
	current_node = n;
	parent_node = NULL;
	first_parent = 1;
	have_parent_box = 0;

	while (true) {
		dom_node_type parent_type;

		err = dom_node_get_parent_node(current_node, &parent_node);
		if (err != DOM_NO_ERR || parent_node == NULL) {
			/* A node with no parent is a root for construction purposes,
			 * matching box_is_root's historical behaviour. */
			if (first_parent)
				props->node_is_root = true;
			break;
		}

		if (first_parent) {
			parent_type = 0;
			err = dom_node_get_node_type(parent_node, &parent_type);
			if (err != DOM_NO_ERR) {
				dom_node_unref(parent_node);
				parent_node = NULL;
				break;
			}
			if (parent_type == DOM_DOCUMENT_NODE) {
				props->node_is_root = true;
				dom_node_unref(parent_node);
				parent_node = NULL;
				break;
			}
			first_parent = 0;
		}

		b = box_for_node(parent_node);
		if (b != NULL) {
			if (!have_parent_box) {
				props->parent_style = b->style;
				props->parent_custom_env = b->custom_env;
				props->href = b->href;
				props->target = b->target;
				props->download = (b->flags & LINK_DOWNLOAD) != 0;
				props->title = b->title;
				have_parent_box = 1;
			}

			if (props->containing_block == NULL &&
					b->type != BOX_INLINE &&
					b->type != BOX_CONTENTS &&
					b->type != BOX_BR) {
				props->containing_block = b;
			}

			if (have_parent_box && props->containing_block != NULL) {
				dom_node_unref(parent_node);
				parent_node = NULL;
				break;
			}
		}

		if (current_node != n)
			dom_node_unref(current_node);
		current_node = parent_node;
		parent_node = NULL;
	}

	if (current_node != n)
		dom_node_unref(current_node);

	/* Compute current inline container, if any */
	if (props->containing_block != NULL &&
			props->containing_block->last != NULL &&
			props->containing_block->last->type ==
				BOX_INLINE_CONTAINER)
		props->inline_container = props->containing_block->last;
}'''
s = s[:start] + new + s[end:]
p.write_text(s)
