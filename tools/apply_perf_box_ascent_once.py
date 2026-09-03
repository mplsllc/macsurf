#!/usr/bin/env python3
from pathlib import Path
p = Path('browser/netsurf/content/handlers/html/box_construct.c')
s = p.read_text()
start = s.index('static dom_node *\nnext_node(struct box_construct_ctx *ctx, dom_node *n,')
end = s.index('\n\n\n/**\n * Apply the CSS text-transform property', start)
new = r'''static bool
box_construct_node_is_root(struct box_construct_ctx *ctx, dom_node *n)
{
	/* Once the root element has been constructed, its box owns a reference to
	 * the exact DOM node.  Pointer comparison avoids a parent lookup + parent
	 * node-type query on every ascent step.  Keep box_is_root as a defensive
	 * fallback for any early/abnormal state where the root box is unavailable. */
	if (ctx != NULL && ctx->root_box != NULL && ctx->root_box->node != NULL)
		return ctx->root_box->node == n;
	return box_is_root(n);
}

static dom_node *
next_node(struct box_construct_ctx *ctx, dom_node *n,
		html_content *content, bool convert_children)
{
	dom_node *next = NULL;
	dom_exception err;

	/* get_first_child already returns NULL when there is no child. Avoid a
	 * separate dom_node_has_child_nodes dispatch on every traversal step. */
	if (convert_children) {
		err = dom_node_get_first_child(n, &next);
		if (err != DOM_NO_ERR) {
			dom_node_unref(n);
			return NULL;
		}
		if (next != NULL) {
			dom_node_unref(n);
			return next;
		}
	}

	err = dom_node_get_next_sibling(n, &next);
	if (err != DOM_NO_ERR) {
		dom_node_unref(n);
		return NULL;
	}

	if (next != NULL) {
		if (box_for_node(n) != NULL)
			box_construct_element_after(ctx, n, content);
		dom_node_unref(n);
		return next;
	}

	if (box_for_node(n) != NULL)
		box_construct_element_after(ctx, n, content);

	/* No sibling: climb until an ancestor has one.  The old implementation
	 * asked box_is_root() at each level (parent + type lookup), then when it
	 * found parent_next it discarded BOTH references, broke out, fetched the
	 * same parent again, fetched the same sibling again, and only then closed
	 * the parent.  Carry the references we already own instead. */
	while (!box_construct_node_is_root(ctx, n)) {
		dom_node *parent = NULL;
		dom_node *parent_next = NULL;

		err = dom_node_get_parent_node(n, &parent);
		if (err != DOM_NO_ERR || parent == NULL) {
			dom_node_unref(n);
			return NULL;
		}

		err = dom_node_get_next_sibling(parent, &parent_next);
		if (err != DOM_NO_ERR) {
			dom_node_unref(parent);
			dom_node_unref(n);
			return NULL;
		}

		if (parent_next != NULL) {
			if (box_for_node(parent) != NULL)
				box_construct_element_after(ctx, parent, content);
			dom_node_unref(parent);
			dom_node_unref(n);
			return parent_next;
		}

		/* Parent itself has no next sibling. Promote it to current, close it,
		 * then continue climbing with the already-owned parent reference. */
		dom_node_unref(n);
		n = parent;
		if (box_for_node(n) != NULL)
			box_construct_element_after(ctx, n, content);
	}

	dom_node_unref(n);
	return NULL;
}'''
s = s[:start] + new + s[end:]
p.write_text(s)
