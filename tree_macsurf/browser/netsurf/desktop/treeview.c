/*
 * Copyright 2012 - 2013 Michael Drake <tlsa@netsurf-browser.org>
 *
 * This file is part of NetSurf, http://www.netsurf-browser.org/
 *
 * NetSurf is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * NetSurf is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * \file
 *
 * Treeview handling implementation.
 */

#include "utils/config.h"

#include <string.h>

#include "utils/utils.h"
#include "utils/log.h"
#include "utils/nsurl.h"
#include "utils/nscolour.h"
#include "utils/nsoption.h"
#include "netsurf/bitmap.h"
#include "netsurf/content.h"
#include "netsurf/plotters.h"
#include "netsurf/clipboard.h"
#include "netsurf/layout.h"
#include "netsurf/keypress.h"
#include "netsurf/core_window.h"
#include "content/hlcache.h"
#include "css/utils.h"

#include "desktop/bitmap.h"
#include "desktop/knockout.h"
#include "desktop/textarea.h"
#include "desktop/treeview.h"
#include "desktop/cw_helper.h"
#include "desktop/gui_internal.h"
#include "desktop/system_colour.h"

/**
 * The maximum horizontal size a treeview can possibly be.
 *
 * \todo get rid of REDRAW_MAX -- need to be able to know window size
 */
#define REDRAW_MAX 8000


/**
 * Treeview handling global context
 */
struct treeview_globals {
	unsigned initialised;
	int line_height;
	int furniture_width;
	int step_width;
	int window_padding;
	int icon_size;
	int icon_step;
	int move_offset;
} tree_g;


/**
 * Section type of a treeview at a point
 */
enum treeview_node_part {
	TV_NODE_PART_TOGGLE,	/**< Expansion toggle */
	TV_NODE_PART_ON_NODE,	/**< Node content (text, icon) */
	TV_NODE_PART_NONE	/**< Empty area */
};


/**
 * Text within a treeview field or node
 */
struct treeview_text {
	const char *data;	/**< Text string */
	uint32_t len;		/**< Length of string in bytes */
	int width;		/**< Width of text in px */
};


/**
 * a treeview field
 */
struct treeview_field {
	/** flags controlling how field is interpreted */
	enum treeview_field_flags flags;

	lwc_string *field; /**< field contents */
	struct treeview_text value; /**< field text */
};


/**
 * flags indicating render state of node.
 */
enum treeview_node_flags {
	TV_NFLAGS_NONE     = 0,		/**< No node flags set */
	TV_NFLAGS_EXPANDED = (1 << 0),	/**< Whether node is expanded */
	TV_NFLAGS_SELECTED = (1 << 1),	/**< Whether node is selected */
	TV_NFLAGS_SPECIAL  = (1 << 2),	/**< Render as special node */
	TV_NFLAGS_MATCHED  = (1 << 3),	/**< Whether node matches search */
};


/**
 * Treeview target position
 */
enum treeview_target_pos {
	TV_TARGET_ABOVE,
	TV_TARGET_INSIDE,
	TV_TARGET_BELOW,
	TV_TARGET_NONE
};


/**
 * Treeview node
 */
struct treeview_node {
	enum treeview_node_flags flags;	/**< Node flags */
	enum treeview_node_type type;	/**< Node type */

	int height;	/**< Includes height of any descendants (pixels) */
	int inset;	/**< Node's inset depending on tree depth (pixels) */

	treeview_node *parent; /**< parent node */
	treeview_node *prev_sib; /**< previous sibling node */
	treeview_node *next_sib; /**< next sibling node */
	treeview_node *children; /**< first child node */

	void *client_data;  /**< Passed to client on node event msg callback */

	struct treeview_text text; /** Text to show for node (default field) */
};


/**
 * Node entry
 *
 * node entry contains a base node at the beginning allowing for
 * trivial containerof by cast and some number of fields.
 */
struct treeview_node_entry {
	treeview_node base; /**< Entry class inherits node base class */
	struct treeview_field fields[FLEX_ARRAY_LEN_DECL];
};


/**
 * A mouse position wrt treeview
 */
struct treeview_pos {
	int x;		/**< Mouse X coordinate */
	int y;		/**< Mouse Y coordinate */
	int node_y;	/**< Top of node at y */
	int node_h;	/**< Height of node at y */
};


/**
 * Treeview drag state
 */
struct treeview_drag {
	enum {
		TV_DRAG_NONE,
		TV_DRAG_SELECTION,
		TV_DRAG_MOVE,
		TV_DRAG_TEXTAREA,
		TV_DRAG_SEARCH,
	} type;	/**< Drag type */
	treeview_node *start_node;	/**< Start node */
	bool selected;			/**< Start node is selected */
	enum treeview_node_part part;	/**< Node part at start */
	struct treeview_pos start;	/**< Start pos */
	struct treeview_pos prev;	/**< Previous pos */
};


/**
 * Treeview node move details
 */
struct treeview_move {
	treeview_node *root;		/**< Head of yanked node list */
	treeview_node *target;		/**< Move target */
	struct rect target_area;	/**< Pos/size of target indicator */
	enum treeview_target_pos target_pos;	/**< Pos wrt render node */
};


/**
 * Treeview node edit details
 */
struct treeview_edit {
	treeview_node *node;		/**< Node being edited, or NULL */
	struct textarea *textarea;	/**< Textarea for edit, or NULL */
	lwc_string *field;		/**< The field being edited, or NULL */
	int x;		/**< Textarea x position */
	int y;		/**< Textarea y position */
	int w;		/**< Textarea width */
	int h;		/**< Textarea height */
};


/**
 * Treeview search box details
 */
struct treeview_search {
	struct textarea *textarea;  /**< Search box. */
	bool active;                /**< Whether the search box has focus. */
	bool search;                /**< Whether we have a search term. */
	int height;                 /**< Current search display height. */
};


/**
 * The treeview context
 */
struct treeview {
	uint32_t view_width; /**< Viewport horizontal size */

	treeview_flags flags; /**< Treeview behaviour settings */

	treeview_node *root; /**< Root node */

	struct treeview_field *fields;	/**< Array of fields */
	int n_fields; /**< fields[n_fields] is folder, lower are entry fields */
	int field_width; /**< Max width of shown field names */

	struct treeview_drag drag; /**< Drag state */
	struct treeview_move move; /**< Move drag details */
	struct treeview_edit edit; /**< Edit details */

	struct treeview_search search; /**< Treeview search box */

	const struct treeview_callback_table *callbacks; /**< For node events */

	struct core_window *cw_h; /**< Core window handle */
};


/**
 * Treeview furniture states.
 */
enum treeview_furniture_id {
	TREE_FURN_EXPAND = 0,
	TREE_FURN_CONTRACT,
	TREE_FURN_LAST
};


/**
 * style for a node
 */
struct treeview_node_style {
	plot_style_t bg;	  /**< Background */
	plot_font_style_t text;	  /**< Text */
	plot_font_style_t itext;  /**< Entry field text */

	plot_style_t sbg;	  /**< Selected background */
	plot_font_style_t stext;  /**< Selected text */
	plot_font_style_t sitext; /**< Selected entry field text */

	struct {
		int size;
		struct bitmap *bmp;
		struct bitmap *sel;
	} furn[TREE_FURN_LAST];
};


/**
 * Plot style for odd rows
 */
struct treeview_node_style plot_style_odd;


/**
 * Plot style for even rows
 */
struct treeview_node_style plot_style_even;


/**
 * Treeview content resource data
 */
struct treeview_resource {
	const char *url;
	struct hlcache_handle *c;
	int height;
	bool ready;
};


/**
 * treeview resource indexes
 */
enum treeview_resource_id {
	TREE_RES_ARROW = 0,
	TREE_RES_CONTENT,
	TREE_RES_FOLDER,
	TREE_RES_FOLDER_SPECIAL,
	TREE_RES_SEARCH,
	TREE_RES_LAST
};


/**
 * Treeview content resources
 */
static struct treeview_resource treeview_res[TREE_RES_LAST] = {
	{ "resource:icons/arrow-l.png", NULL, 0, false },
	{ "resource:icons/content.png", NULL, 0, false },
	{ "resource:icons/directory.png", NULL, 0, false },
	{ "resource:icons/directory2.png", NULL, 0, false },
	{ "resource:icons/search.png", NULL, 0, false }
};


/**
 * Get the display height of the treeview data component of the display.
 *
 * \param[in] tree  Treeview to get the height of.
 * \return the display height in pixels.
 */
static inline int treeview__get_display_height(const treeview *tree)
{
	return (tree->search.search == false) ?
			tree->root->height :
			tree->search.height;
}


/**
 * Corewindow callback wrapper: Request a redraw of the window
 *
 * \param[in] tree The treeview to request redraw on.
 * \param[in] r rectangle to redraw
 */
static inline void treeview__cw_invalidate_area(
		const struct treeview *tree,
		const struct rect *r)
{
	if (tree->cw_h != NULL) {
		guit->corewindow->invalidate(tree->cw_h, r);
	}
}


/**
 * Corewindow callback wrapper: Request a full redraw of the window
 *
 * \param[in] tree The treeview to request redraw on.
 */
static inline void treeview__cw_full_redraw(
		const struct treeview *tree)
{
	if (tree->cw_h != NULL) {
		static const struct rect r = {
			.x0 = 0,
			.y0 = 0,
			.x1 = REDRAW_MAX,
			.y1 = REDRAW_MAX,
		};
		guit->corewindow->invalidate(tree->cw_h, &r);
	}
}


/**
 * Get height used by treeview's search bar (or 0 if not present).
 *
 * \param tree    Treeview object to check.
 * \return height used by search bar in pixels.
 */
static inline unsigned treeview__get_search_height(
		const treeview *tree)
{
	return (tree->flags & TREEVIEW_SEARCHABLE) ?
			tree_g.line_height : 0;
}


/**
 * Corewindow callback wrapper: Update the limits of the window
 *
 * \param[in] tree The treeview to update size for.
 * \param[in] width the width in px, or negative if don't care
 * \param[in] height the height in px, or negative if don't care
 */
static inline void treeview__cw_update_size(
	const struct treeview *tree,
	int width, int height)
{
	if (tree->cw_h != NULL) {
		guit->corewindow->set_extent(tree->cw_h,
				     width,
				     height + treeview__get_search_height(tree));
	}
}


/**
 * Corewindow callback_wrapper: Scroll to top of window.
 *
 * \param[in] tree  The treeview to scroll.
 */
static inline void treeview__cw_scroll_top(
		const struct treeview *tree)
{
	struct rect r = {
		.x0 = 0,
		.y0 = 0,
		.x1 = tree_g.window_padding,
		.y1 = tree_g.line_height,
	};

	cw_helper_scroll_visible(tree->cw_h, &r);
}

/**
 * Corewindow callback wrapper: Get window viewport dimensions
 *
 * \param[in] tree The treeview to get dimensions for.
 * \param[out] width to be set to viewport width in px
 * \param[out] height to be set to viewport height in px
 */
static inline void treeview__cw_get_window_dimensions(
	const struct treeview *tree,
	int *width, int *height)
{
	if (tree->cw_h != NULL) {
		guit->corewindow->get_dimensions(tree->cw_h, width, height);
	}
}


/**
 * Corewindow callback wrapper: Inform corewindow owner of drag status
 *
 * \param[in] tree The treeview to report status on.
 * \param[in] ds the current drag status
 */
static inline void treeview__cw_drag_status(
	const struct treeview *tree,
	core_window_drag_status ds)
{
	if (tree->cw_h != NULL) {
		guit->corewindow->drag_status(tree->cw_h, ds);
	}
}


/**
 * Helper function to access the given field of a node
 *
 * \param tree Treeview that node belongs to
 * \param n Node to get field from
 * \param i Index of field of interest
 * \return text entry for field or NULL.
 */
static inline struct treeview_text *
treeview_get_text_for_field(treeview *tree, treeview_node *n, int i)
{
	if (i == 0) {
		return &n->text;

	} else if (i < tree->n_fields && n->type == TREE_NODE_ENTRY) {
		struct treeview_node_entry *e = (struct treeview_node_entry *)n;
		return &e->fields[i - 1].value;
	}

	assert(0 && "Bad field index for node");
	return NULL;
}


/**
 * Find the next node in depth first tree order
 *
 * \param node Start node
 * \param full Iff true, visit children of collapsed nodes
 * \return next node, or NULL if \a node is last node
 */
static inline treeview_node * treeview_node_next(treeview_node *node, bool full)
{
	assert(node != NULL);

	if ((full || (node->flags & TV_NFLAGS_EXPANDED)) &&
	    node->children != NULL) {
		/* Next node is child */
		node = node->children;
	} else {
		/* No children.  As long as we're not at the root,
		 * go to next sibling if present, or nearest ancestor
		 * with a next sibling. */

		while (node->parent != NULL && node->next_sib == NULL) {
			node = node->parent;
		}

		if (node->type == TREE_NODE_ROOT) {
			node = NULL;

		} else {
			node = node->next_sib;
		}
	}

	return node;
}


/**
 * Find node at given y-position
 *
 * \param tree Treeview object to delete node from
 * \param target_y Target y-position
 * \return node at y_target
 */
static treeview_node * treeview_y_node(treeview *tree, int target_y)
{
	int y = treeview__get_search_height(tree);
	treeview_node *n;

	assert(tree != NULL);
	assert(tree->root != NULL);

	n = treeview_node_next(tree->root, false);

	while (n != NULL) {
		int h = (n->type == TREE_NODE_ENTRY) ?
			n->height : tree_g.line_height;
		if (target_y >= y && target_y < y + h)
			return n;
		y += h;

		n = treeview_node_next(n, false);
	}

	return NULL;
}


/**
 * Find y position of the top of a node
 *
 * \param tree Treeview object to delete node from
 * \param node Node to get position of
 * \return node's y position
 */
static int treeview_node_y(
		const treeview *tree,
		const treeview_node *node)
{
	treeview_node *n;
	int y = treeview__get_search_height(tree);

	assert(tree != NULL);
	assert(tree->root != NULL);

	n = treeview_node_next(tree->root, false);

	while (n != NULL && n != node) {
		y += (n->type == TREE_NODE_ENTRY) ?
			n->height : tree_g.line_height;

		n = treeview_node_next(n, false);
	}

	return y;
}


/**
 * Corewindow callback_wrapper: Scroll to make node visible
 *
 * \param[in] tree  The treeview to scroll.
 * \param[in] node  The treeview node to scroll to visibility.
 */
static inline void treeview__cw_scroll_to_node(
		const struct treeview *tree,
		const struct treeview_node *node)
{
	struct rect r = {
		.x0 = 0,
		.y0 = treeview_node_y(tree, node),
		.x1 = 1,
		.y1 = (((node != NULL) && (node->type == TREE_NODE_ENTRY)) ?
		       node->height : tree_g.line_height),
	};

	r.y1 += r.y0; /* Apply the Y offset to the second Y coordinate */

	cw_helper_scroll_visible(tree->cw_h, &r);
}


/**
 * Redraw tree from given node to the bottom.
 *
 * \param[in] tree  Tree to redraw from node in.
 * \param[in] node  Node to redraw from.
 */
static void treeview__redraw_from_node(
		const treeview *tree,
		const treeview_node *node)
{
	struct rect r = {
		.x0 = 0,
		.y0 = treeview_node_y(tree, node),
		.x1 = REDRAW_MAX,
		.y1 = treeview__get_display_height(tree) +
				treeview__get_search_height(tree),
	};

	assert(tree != NULL);

	treeview__cw_invalidate_area(tree, &r);
}


/**
 * The treeview walk mode.  Controls which nodes are visited in a walk.
 */
enum treeview_walk_mode {
	/**
	 * Walk to all nodes in the (sub)tree.
	 */
	TREEVIEW_WALK_MODE_LOGICAL_COMPLETE,

	/**
	 * Walk to expanded nodes in the (sub)tree only.  Children of
	 * collapsed nodes are not visited.
	 */
	TREEVIEW_WALK_MODE_LOGICAL_EXPANDED,

	/**
	 * Walk displayed nodes.  This differs from the
	 * `TREEVIEW_WALK_MODE_LOGICAL_EXPANDED` mode when there is
	 * an active search filter display.
	 */
	TREEVIEW_WALK_MODE_DISPLAY,
};


/**
 * Walk a treeview subtree, calling a callback at each node (depth first)
 *
 * \param tree		Treeview being walked.
 * \param root		Root to walk tree from (doesn't get a callback call)
 * \param mode		The treeview walk mode to use.
 * \param callback_bwd	Function to call on each node in backwards order
 * \param callback_fwd	Function to call on each node in forwards order
 * \param ctx		Context to pass to callback
 * \return NSERROR_OK on success, or appropriate error otherwise
 *
 * \note Any node deletion must happen in callback_bwd.
 */
static nserror treeview_walk_internal(
		treeview *tree,
		treeview_node *root,
		enum treeview_walk_mode mode,
		nserror (*callback_bwd)(
				treeview_node *n,
				void *ctx,
				bool *end),
		nserror (*callback_fwd)(
				treeview_node *n,
				void *ctx,
				bool *skip_children,
				bool *end),
		void *ctx)
{
	treeview_node *node, *child, *parent, *next_sibling;
	bool walking_search = (mode == TREEVIEW_WALK_MODE_DISPLAY &&
			tree->search.search == true);
	bool skip_children = false;
	bool abort = false;
	bool full = false;
	nserror err;
	bool entry;

	assert(root != NULL);

	if (mode == TREEVIEW_WALK_MODE_LOGICAL_COMPLETE || walking_search) {
		/* We need to visit children of collapsed folders. */
		full = true;
	}

	node = root;
	parent = node->parent;
	next_sibling = node->next_sib;
	child = (full || (node->flags & TV_NFLAGS_EXPANDED)) ?
		node->children : NULL;

	while (node != NULL) {

		if (child != NULL && !skip_children) {
			/* Down to children */
			node = child;
		} else {
			/* No children.  As long as we're not at the root,
			 * go to next sibling if present, or nearest ancestor
			 * with a next sibling. */

			while (node != root && next_sibling == NULL) {
				entry = (node->type == TREE_NODE_ENTRY);
				if (callback_bwd != NULL &&
						(entry || !walking_search)) {
					/* Backwards callback */
					err = callback_bwd(node, ctx, &abort);

					if (err != NSERROR_OK) {
						return err;

					} else if (abort) {
						/* callback requested early
						 * termination */
						return NSERROR_OK;
					}
				}
				node = parent;
				parent = node->parent;
				next_sibling = node->next_sib;
			}

			if (node == root)
				break;

			if (callback_bwd != NULL) {
				/* Backwards callback */
				err = callback_bwd(node, ctx, &abort);

				if (err != NSERROR_OK) {
					return err;

				} else if (abort) {
					/* callback requested early
					 * termination */
					return NSERROR_OK;
				}
			}
			node = next_sibling;
		}

		assert(node != NULL);
		assert(node != root);

		entry = (node->type == TREE_NODE_ENTRY);

		parent = node->parent;
		next_sibling = node->next_sib;
		child = (full || (node->flags & TV_NFLAGS_EXPANDED)) ?
			node->children : NULL;

		if (walking_search && (!entry ||
				!(node->flags & TV_NFLAGS_MATCHED))) {
			continue;
		}

		if (callback_fwd != NULL) {
			/* Forwards callback */
			err = callback_fwd(node, ctx, &skip_children, &abort);

			if (err != NSERROR_OK) {
				return err;

			} else if (abort) {
				/* callback requested early termination */
				return NSERROR_OK;
			}
		}
	}
	return NSERROR_OK;
}


/**
 * Data used when doing a treeview walk for search.
 */
struct treeview_search_walk_data {
	treeview *tree;          /**< The treeview to search. */
	const char *text;        /**< The string being searched for. */
	const unsigned int len;  /**< Length of string being searched for. */
	int window_height;       /**< Accumulate height for matching entries. */
};


/**
 * Treewalk node callback for handling search.
 *
 * \param[in]     n              Current node.
 * \param[in]     ctx            Treeview search context.
 * \param[in,out] skip_children  Flag to allow children to be skipped.
 * \param[in,out] end            Flag to allow iteration to be finished early.
 * \return NSERROR_OK on success else error code.
 */
static nserror treeview__search_walk_cb(
		treeview_node *n,
		void *ctx,
		bool *skip_children,
		bool *end)
{
	struct treeview_search_walk_data *sw = ctx;

	/* only entry nodes can be searched */
	if (n->type != TREE_NODE_ENTRY) {
		return NSERROR_OK;
	}

	/* empty string will never match */
	if (sw->len == 0) {
		n->flags &= ~TV_NFLAGS_MATCHED;
		return NSERROR_OK;
	}

	struct treeview_node_entry *entry = (struct treeview_node_entry *)n;
	bool matched = false;

	for (int i = 0; i < sw->tree->n_fields; i++) {
		struct treeview_field *ef = &(sw->tree->fields[i + 1]);
		if (ef->flags & TREE_FLAG_SEARCHABLE) {
			if (strcasestr(entry->fields[i].value.data,
				       sw->text) != NULL) {
				matched = true;
				break;
			}
		}
	}

	if (!matched && strcasestr(n->text.data, sw->text) != NULL) {
		matched = true;
	}

	if (matched) {
		n->flags |= TV_NFLAGS_MATCHED;
		sw->window_height += n->height;
	} else {
		n->flags &= ~TV_NFLAGS_MATCHED;
	}

	return NSERROR_OK;
}


/**
 * Search treeview for text.
 *
 * \param[in] tree  Treeview to search.
 * \param[in] text  UTF-8 string to search for.  (NULL-terminated.)
 * \param[in] len   Byte length of UTF-8 string.
 * \return NSERROR_OK on success, appropriate error otherwise.
 */
static nserror treeview__search(
		treeview *tree,
		const char *text,
		unsigned int len)
{
	nserror err;
	uint32_t height;
	uint32_t prev_height = treeview__get_display_height(tree);
	int search_height = treeview__get_search_height(tree);
	struct treeview_search_walk_data sw = {
		.len = len,
		.text = text,
		.tree = tree,
		.window_height = 0,
	};
	struct rect r = {
		.x0 = 0,
		.y0 = search_height,
		.x1 = REDRAW_MAX,
	};

	assert(text[len] == '\0');

	if (tree->root == NULL) {
		return NSERROR_OK;
	}

	err = treeview_walk_internal(tree, tree->root,
			TREEVIEW_WALK_MODE_LOGICAL_COMPLETE, NULL,
			treeview__search_walk_cb, &sw);
	if (err != NSERROR_OK) {
		return err;
	}

	if (len > 0) {
		tree->search.height = sw.window_height;
		tree->search.search = true;
		height = sw.window_height;
	} else {
		tree->search.search = false;
		height = tree->root->height;
	}

	r.y1 = ((height > prev_height) ? height : prev_height) + search_height;
	treeview__cw_invalidate_area(tree, &r);
	treeview__cw_update_size(tree, -1, height);
	treeview__cw_scroll_top(tree);

	return NSERROR_OK;
}


/**
 * Cancel a treeview search, optionally droping focus from search widget.
 *
 * \param[in] tree        Treeview to cancel search in.
 * \param[in] drop_focus  Iff true, drop input focus from search widget.
 */
static void treeview__search_cancel(treeview *tree, bool drop_focus)
{
	struct rect r = {
		.x0 = tree_g.window_padding + tree_g.icon_size,
		.x1 = 600,
		.y0 = 0,
		.y1 = tree_g.line_height,
	};

	tree->search.search = false;
	if (tree->search.active == false) {
		return;
	}

	if (textarea_get_text(tree->search.textarea, NULL, 0) == 1) {
		// If there's no text in the search box, we drop focus on a
		// cancel.  Note '1' because it includes the trailing \0
		drop_focus = true;
	}

	if (drop_focus) {
		tree->search.active = false;
		textarea_set_caret(tree->search.textarea, -1);
	} else {
		textarea_set_caret(tree->search.textarea, 0);
	}

	textarea_set_text(tree->search.textarea, "");
	treeview__cw_invalidate_area(tree, &r);
}

/**
 * Convert from treeview drag to core window drag type.
 *
 * \param[in]  tree  A treeview.
 * \return Core window drag type.
 */
static core_window_drag_status treeview__get_cw_drag_type(
		const treeview *tree)
{
	assert(tree != NULL);

	switch (tree->drag.type) {
	case TV_DRAG_NONE:
		return CORE_WINDOW_DRAG_NONE;

	case TV_DRAG_SELECTION:
		return CORE_WINDOW_DRAG_SELECTION;

	case TV_DRAG_TEXTAREA: /* Fall through.*/
	case TV_DRAG_SEARCH:
		return CORE_WINDOW_DRAG_TEXT_SELECTION;

	case TV_DRAG_MOVE:
		return CORE_WINDOW_DRAG_MOVE;
	}

	return CORE_WINDOW_DRAG_NONE;
}

/**
 * Callback for textarea_create, in desktop/treeview.h
 *
 * \param data treeview context
 * \param msg textarea message
 */
static void treeview_textarea_search_callback(void *data,
		struct textarea_msg *msg)
{
	treeview *tree = data;
	struct rect *r;

	if (tree->search.active == false || tree->root == NULL) {
		return;
	}

	switch (msg->type) {
	case TEXTAREA_MSG_DRAG_REPORT:
		if (msg->data.drag == TEXTAREA_DRAG_NONE) {
			/* Textarea drag finished */
			tree->drag.type = TV_DRAG_NONE;
		} else {
			/* Textarea drag started */
			tree->drag.type = TV_DRAG_SEARCH;
		}
		treeview__cw_drag_status(tree,
				treeview__get_cw_drag_type(tree));
		break;

	case TEXTAREA_MSG_REDRAW_REQUEST:
		r = &msg->data.redraw;
		r->x0 += tree_g.window_padding + tree_g.icon_size;
		r->y0 += 0;
		r->x1 += 600;
		r->y1 += tree_g.line_height;

		/* Redraw the textarea */
		treeview__cw_invalidate_area(tree, r);
		break;

	case TEXTAREA_MSG_TEXT_MODIFIED:
		/* Textarea length includes trailing NULL, so subtract it. */
		treeview__search(tree,
				msg->data.modified.text,
				msg->data.modified.len - 1);
		break;

	default:
		break;
	}
}


/**
 * Update the layout for any active search.
 *
 * \param[in] tree  The tree to update.
 */
static void treeview__search_update_display(
		treeview *tree)
{
	const char *string;
	unsigned int len;

	if (tree->search.search == false) {
		/* No active search to update view for. */
		return;
	}

	string = textarea_data(tree->search.textarea, &len);
	if (string == NULL || len == 0) {
		return;
	}

	treeview__search(tree, string, len - 1);
}


/**
 * Create treeview's root node
 *
 * \param[out] root Returns root node
 * \return NSERROR_OK on success, appropriate error otherwise
 */
static nserror treeview_create_node_root(treeview_node **root)
{
	treeview_node *n;

	n = malloc(sizeof(struct treeview_node));
	if (n == NULL) {
		return NSERROR_NOMEM;
	}

	n->flags = TV_NFLAGS_EXPANDED;
	n->type = TREE_NODE_ROOT;

	n->height = 0;
	n->inset = tree_g.window_padding - tree_g.step_width;

	n->text.data = NULL;
	n->text.len = 0;
	n->text.width = 0;

	n->parent = NULL;
	n->next_sib = NULL;
	n->prev_sib = NULL;
	n->children = NULL;

	n->client_data = NULL;

	*root = n;

	return NSERROR_OK;
}


/**
 * Set a node's inset from its parent
 *
 * This may be used as treeview walk callback
 *
 * \param[in] n node to set inset on
 * \param[in] ctx context unused
 * \param[out] skip_children set to false so child nodes are not skipped.
 * \param[out] end unused flag so treewalk in not terminated early.
 */
static nserror
treeview_set_inset_from_parent(treeview_node *n,
			       void *ctx,
			       bool *skip_children,
			       bool *end)
{
	if (n->parent != NULL)
		n->inset = n->parent->inset + tree_g.step_width;

	*skip_children = false;
	return NSERROR_OK;
}


/**
 * Insert a treeview node into a treeview
 *
 * \param tree  the treeview to insert node into.
 * \param a     parentless node to insert
 * \param b     tree node to insert a as a relation of
 * \param rel The relationship between \a a and \a b
 */
static inline void
treeview_insert_node(
		treeview *tree,
		treeview_node *a,
		treeview_node *b,
		enum treeview_relationship rel)
{
	assert(a != NULL);
	assert(a->parent == NULL);
	assert(b != NULL);

	switch (rel) {
	case TREE_REL_FIRST_CHILD:
		assert(b->type != TREE_NODE_ENTRY);
		a->parent = b;
		a->next_sib = b->children;
		if (a->next_sib)
			a->next_sib->prev_sib = a;
		b->children = a;
		break;

	case TREE_REL_NEXT_SIBLING:
		assert(b->type != TREE_NODE_ROOT);
		a->prev_sib = b;
		a->next_sib = b->next_sib;
		a->parent = b->parent;
		b->next_sib = a;
		if (a->next_sib)
			a->next_sib->prev_sib = a;
		break;

	default:
		assert(0);
		break;
	}

	assert(a->parent != NULL);

	a->inset = a->parent->inset + tree_g.step_width;
	if (a->children != NULL) {
		treeview_walk_internal(tree, a,
				TREEVIEW_WALK_MODE_LOGICAL_COMPLETE, NULL,
				treeview_set_inset_from_parent, NULL);
	}

	if (a->parent->flags & TV_NFLAGS_EXPANDED) {
		int height = a->height;
		/* Parent is expanded, so inserted node will be visible and
		 * affect layout */
		if (a->text.width == 0) {
			guit->layout->width(&plot_style_odd.text,
					    a->text.data,
					    a->text.len,
					    &(a->text.width));
		}

		do {
			a->parent->height += height;
			a = a->parent;
		} while (a->parent != NULL);
	}
}


/* Exported interface, documented in treeview.h */
nserror
treeview_create_node_folder(treeview *tree,
			    treeview_node **folder,
			    treeview_node *relation,
			    enum treeview_relationship rel,
			    const struct treeview_field_data *field,
			    void *data,
			    treeview_node_options_flags flags)
{
	treeview_node *n;

	assert(data != NULL);
	assert(tree != NULL);
	assert(tree->root != NULL);

	if (relation == NULL) {
		relation = tree->root;
		rel = TREE_REL_FIRST_CHILD;
	}

	n = malloc(sizeof(struct treeview_node));
	if (n == NULL) {
		return NSERROR_NOMEM;
	}

	n->flags = (flags & TREE_OPTION_SPECIAL_DIR) ?
		TV_NFLAGS_SPECIAL : TV_NFLAGS_NONE;
	n->type = TREE_NODE_FOLDER;

	n->height = tree_g.line_height;

	n->text.data = field->value;
	n->text.len = field->value_len;
	n->text.width = 0;

	n->parent = NULL;
	n->next_sib = NULL;
	n->prev_sib = NULL;
	n->children = NULL;

	n->client_data = data;

	treeview_insert_node(tree, n, relation, rel);

	if (n->parent->flags & TV_NFLAGS_EXPANDED) {
		/* Inform front end of change in dimensions */
		if (!(flags & TREE_OPTION_SUPPRESS_RESIZE))
			treeview__cw_update_size(tree, -1,
						 tree->root->height);

		/* Redraw */
		if (!(flags & TREE_OPTION_SUPPRESS_REDRAW)) {
			struct rect r;
			r.x0 = 0;
			r.y0 = treeview_node_y(tree, n);
			r.x1 = REDRAW_MAX;
			r.y1 = tree->root->height;
			treeview__cw_invalidate_area(tree, &r);
		}
	}

	*folder = n;

	return NSERROR_OK;
}


/* Exported interface, documented in treeview.h */
nserror
treeview_update_node_folder(treeview *tree,
			    treeview_node *folder,
			    const struct treeview_field_data *field,
			    void *data)
{
	bool match;

	assert(data != NULL);
	assert(tree != NULL);
	assert(folder != NULL);
	assert(data == folder->client_data);
	assert(folder->parent != NULL);

	assert(field != NULL);
	assert(lwc_string_isequal(tree->fields[tree->n_fields].field,
				  field->field, &match) == lwc_error_ok &&
	       match == true);
	folder->text.data = field->value;
	folder->text.len = field->value_len;
	folder->text.width = 0;

	if (folder->parent->flags & TV_NFLAGS_EXPANDED) {
		/* Text will be seen, get its width */
		guit->layout->width(&plot_style_odd.text,
				    folder->text.data,
				    folder->text.len,
				    &(folder->text.width));
	} else {
		/* Just invalidate the width, since it's not needed now */
		folder->text.width = 0;
	}

	/* Redraw */
	if (folder->parent->flags & TV_NFLAGS_EXPANDED) {
		struct rect r;
		r.x0 = 0;
		r.y0 = treeview_node_y(tree, folder);
		r.x1 = REDRAW_MAX;
		r.y1 = r.y0 + tree_g.line_height;
		treeview__cw_invalidate_area(tree, &r);
	}

	return NSERROR_OK;
}


/* Exported interface, documented in treeview.h */
nserror
treeview_update_node_entry(treeview *tree,
			   treeview_node *entry,
			   const struct treeview_field_data fields[],
			   void *data)
{
	bool match;
	struct treeview_node_entry *e = (struct treeview_node_entry *)entry;
	int i;

	assert(data != NULL);
	assert(tree != NULL);
	assert(entry != NULL);
	assert(data == entry->client_data);
	assert(entry->parent != NULL);

	assert(fields != NULL);
	assert(fields[0].field != NULL);
	assert(lwc_string_isequal(tree->fields[0].field,
				  fields[0].field, &match) == lwc_error_ok &&
	       match == true);
	entry->text.data = fields[0].value;
	entry->text.len = fields[0].value_len;
	entry->text.width = 0;

	if (entry->parent->flags & TV_NFLAGS_EXPANDED) {
		/* Text will be seen, get its width */
		guit->layout->width(&plot_style_odd.text,
				    entry->text.data,
				    entry->text.len,
				    &(entry->text.width));
	} else {
		/* Just invalidate the width, since it's not needed now */
		entry->text.width = 0;
	}

	for (i = 1; i < tree->n_fields; i++) {
		assert(fields[i].field != NULL);
		assert(lwc_string_isequal(tree->fields[i].field,
					  fields[i].field, &match) == lwc_error_ok &&
		       match == true);

		e->fields[i - 1].value.data = fields[i].value;
		e->fields[i - 1].value.len = fields[i].value_len;

		if (entry->flags & TV_NFLAGS_EXPANDED) {
			/* Text will be seen, get its width */
			guit->layout->width(&plot_style_odd.text,
					    e->fields[i - 1].value.data,
					    e->fields[i - 1].value.len,
					    &(e->fields[i - 1].value.width));
		} else {
			/* Invalidate the width, since it's not needed yet */
			e->fields[i - 1].value.width = 0;
		}
	}

	treeview__search_update_display(tree);

	/* Redraw */
	if (entry->parent->flags & TV_NFLAGS_EXPANDED) {
		struct rect r;
		r.x0 = 0;
		r.y0 = treeview_node_y(tree, entry);
		r.x1 = REDRAW_MAX;
		r.y1 = r.y0 + entry->height;
		treeview__cw_invalidate_area(tree, &r);
	}

	return NSERROR_OK;
}


/* Exported interface, documented in treeview.h */
nserror
treeview_create_node_entry(treeview *tree,
			   treeview_node **entry,
			   treeview_node *relation,
			   enum treeview_relationship rel,
			   const struct treeview_field_data fields[],
			   void *data,
			   treeview_node_options_flags flags)
{
	bool match;
	struct treeview_node_entry *e;
	treeview_node *n;
	int i;

	assert(data != NULL);
	assert(tree != NULL);
	assert(tree->root != NULL);

	if (relation == NULL) {
		relation = tree->root;
		rel = TREE_REL_FIRST_CHILD;
	}

	e = malloc(sizeof(struct treeview_node_entry) +
			(tree->n_fields - 1) * sizeof(struct treeview_field));
	if (e == NULL) {
		return NSERROR_NOMEM;
	}


	n = (treeview_node *) e;

	n->flags = TV_NFLAGS_NONE;
	n->type = TREE_NODE_ENTRY;

	n->height = tree_g.line_height;

	assert(fields != NULL);
	assert(fields[0].field != NULL);
	assert(lwc_string_isequal(tree->fields[0].field,
			fields[0].field, &match) == lwc_error_ok &&
			match == true);
	n->text.data = fields[0].value;
	n->text.len = fields[0].value_len;
	n->text.width = 0;

	n->parent = NULL;
	n->next_sib = NULL;
	n->prev_sib = NULL;
	n->children = NULL;

	n->client_data = data;

	for (i = 1; i < tree->n_fields; i++) {
		assert(fields[i].field != NULL);
		assert(lwc_string_isequal(tree->fields[i].field,
				fields[i].field, &match) == lwc_error_ok &&
				match == true);

		e->fields[i - 1].value.data = fields[i].value;
		e->fields[i - 1].value.len = fields[i].value_len;
		e->fields[i - 1].value.width = 0;
	}

	treeview_insert_node(tree, n, relation, rel);

	if (n->parent->flags & TV_NFLAGS_EXPANDED) {
		/* Inform front end of change in dimensions */
		if (!(flags & TREE_OPTION_SUPPRESS_RESIZE))
			treeview__cw_update_size(tree, -1,
						 tree->root->height);

		/* Redraw */
		if (!(flags & TREE_OPTION_SUPPRESS_REDRAW)) {
			struct rect r;
			r.x0 = 0;
			r.y0 = treeview_node_y(tree, n);
			r.x1 = REDRAW_MAX;
			r.y1 = tree->root->height;
			treeview__cw_invalidate_area(tree, &r);
		}
	}

	treeview__search_update_display(tree);

	*entry = n;

	return NSERROR_OK;
}


/**
 * Treewalk iterator context
 */
struct treeview_walk_ctx {
	treeview_walk_cb enter_cb;
	treeview_walk_cb leave_cb;
	void *ctx;
	enum treeview_node_type type;
};


/**
 * Treewalk node enter callback.
 *
 * \param n current node
 * \param ctx treewalk context
 * \param skip_children set if child nodes should be skipped
 * \param end set if iteration should end early
 */
static nserror
treeview_walk_fwd_cb(treeview_node *n,
		     void *ctx,
		     bool *skip_children,
		     bool *end)
{
	struct treeview_walk_ctx *tw = ctx;

	if (n->type & tw->type) {
		return tw->enter_cb(tw->ctx, n->client_data, n->type, end);
	}

	return NSERROR_OK;
}


/**
 * Treewalk node leave callback.
 *
 * \param n current node
 * \param ctx treewalk context
 * \param end set if iteration should end early
 */
static nserror treeview_walk_bwd_cb(treeview_node *n, void *ctx, bool *end)
{
	struct treeview_walk_ctx *tw = ctx;

	if (n->type & tw->type) {
		return tw->leave_cb(tw->ctx, n->client_data, n->type, end);
	}

	return NSERROR_OK;
}


/* Exported interface, documented in treeview.h */
nserror
treeview_walk(treeview *tree,
	      treeview_node *root,
	      treeview_walk_cb enter_cb,
	      treeview_walk_cb leave_cb,
	      void *ctx,
	      enum treeview_node_type type)
{
	struct treeview_walk_ctx tw = {
		.enter_cb = enter_cb,
		.leave_cb = leave_cb,
		.ctx = ctx,
		.type = type
	};

	assert(tree != NULL);
	assert(tree->root != NULL);

	if (root == NULL)
		root = tree->root;

	return treeview_walk_internal(tree, root,
			TREEVIEW_WALK_MODE_LOGICAL_COMPLETE,
			(leave_cb != NULL) ? treeview_walk_bwd_cb : NULL,
			(enter_cb != NULL) ? treeview_walk_fwd_cb : NULL,
			&tw);
}


/**
 * Unlink a treeview node
 *
 * \param n Node to unlink
 * \return true iff ancestor heights need to be reduced
 */
static inline bool treeview_unlink_node(treeview_node *n)
{
	/* Unlink node from tree */
	if (n->parent != NULL && n->parent->children == n) {
		/* Node is a first child */
		n->parent->children = n->next_sib;

	} else if (n->prev_sib != NULL) {
		/* Node is not first child */
		n->prev_sib->next_sib = n->next_sib;
	}

	if (n->next_sib != NULL) {
		/* Always need to do this */
		n->next_sib->prev_sib = n->prev_sib;
	}

	/* Reduce ancestor heights */
	if ((n->parent != NULL) &&
	    (n->parent->flags & TV_NFLAGS_EXPANDED)) {
		return true;
	}

	return false;
}


/**
 * Cancel the editing of a treeview node
 *
 * \param tree Treeview object to cancel node editing in
 * \param redraw Set true iff redraw of removed textarea area required
 */
static void treeview_edit_cancel(treeview *tree, bool redraw)
{
	struct rect r;

	if (tree->edit.textarea == NULL)
		return;

	textarea_destroy(tree->edit.textarea);

	tree->edit.textarea = NULL;
	tree->edit.node = NULL;

	if (tree->drag.type == TV_DRAG_TEXTAREA)
		tree->drag.type = TV_DRAG_NONE;

	if (redraw) {
		r.x0 = tree->edit.x;
		r.y0 = tree->edit.y;
		r.x1 = tree->edit.x + tree->edit.w;
		r.y1 = tree->edit.y + tree->edit.h;
		treeview__cw_invalidate_area(tree, &r);
	}
}


/**
 * Complete a treeview edit
 *
 * Complete edit by informing the client with a change request msg
 *
 * \param tree Treeview object to complete edit in
 */
static void treeview_edit_done(treeview *tree)
{
	int len, error;
	char* new_text;
	treeview_node *n = tree->edit.node;
	struct treeview_node_msg msg;
	msg.msg = TREE_MSG_NODE_EDIT;

	if (tree->edit.textarea == NULL) {
		return;
	}

	assert(n != NULL);

	/* Get new text length */
	len = textarea_get_text(tree->edit.textarea, NULL, 0);

	new_text = malloc(len);
	if (new_text == NULL) {
		/* TODO: don't just silently ignore */
		return;
	}

	/* Get the new text from textarea */
	error = textarea_get_text(tree->edit.textarea, new_text, len);
	if (error == -1) {
		/* TODO: don't just silently ignore */
		free(new_text);
		return;
	}

	/* Inform the treeview client with change request message */
	msg.data.node_edit.field = tree->edit.field;
	msg.data.node_edit.text = new_text;

	switch (n->type) {
	case TREE_NODE_ENTRY:
		tree->callbacks->entry(msg, n->client_data);
		break;
	case TREE_NODE_FOLDER:
		tree->callbacks->folder(msg, n->client_data);
		break;
	case TREE_NODE_ROOT:
		break;
	default:
		break;
	}

	/* Finished with the new text */
	free(new_text);

	/* Finally, destroy the treeview, and redraw */
	treeview_edit_cancel(tree, true);
}


/**
 * context for treeview node deletion iterator
 */
struct treeview_node_delete {
	treeview *tree;
	int h_reduction;
	bool user_interaction;
};


/**
 * Treewalk node callback deleting nodes.
 */
static nserror
treeview_delete_node_walk_cb(treeview_node *n, void *ctx, bool *end)
{
	struct treeview_node_delete *nd = (struct treeview_node_delete *)ctx;
	struct treeview_node_msg msg;

	msg.msg = TREE_MSG_NODE_DELETE;
	msg.data.delete.user = nd->user_interaction;

	assert(n->children == NULL);

	if (treeview_unlink_node(n))
		nd->h_reduction += (n->type == TREE_NODE_ENTRY) ?
			n->height : tree_g.line_height;

	/* Handle any special treatment */
	switch (n->type) {
	case TREE_NODE_ENTRY:
		nd->tree->callbacks->entry(msg, n->client_data);
		break;

	case TREE_NODE_FOLDER:
		nd->tree->callbacks->folder(msg, n->client_data);
		break;

	case TREE_NODE_ROOT:
		break;

	default:
		return NSERROR_BAD_PARAMETER;
	}

	/* Cancel any edit of this node */
	if (nd->tree->edit.textarea != NULL &&
	    nd->tree->edit.node == n) {
		treeview_edit_cancel(nd->tree, false);
	}

	/* Free the node */
	free(n);

	return NSERROR_OK;
}


/**
 * Delete a treeview node
 *
 * Will emit folder or entry deletion msg callback.
 *
 * \note this can be called from inside a treeview_walk fwd callback.
 * For example walking the tree and calling this for any node that's selected.
 *
 * This function does not delete empty nodes, so if TREEVIEW_DEL_EMPTY_DIRS is
 * set, caller must also call treeview_delete_empty.
 *
 * \param tree		Treeview object to delete node from
 * \param n		Node to delete
 * \param interaction	Delete is result of user interaction with treeview
 * \param flags		Treeview node options flags
 * \return NSERROR_OK on success, appropriate error otherwise
 */
static nserror
treeview_delete_node_internal(treeview *tree,
			      treeview_node *n,
			      bool interaction,
			      treeview_node_options_flags flags)
{
	nserror err;
	treeview_node *p = n->parent;
	struct treeview_node_delete nd = {
		.tree = tree,
		.h_reduction = 0,
		.user_interaction = interaction
	};

	if (interaction && (tree->flags & TREEVIEW_NO_DELETES)) {
		return NSERROR_OK;
	}

	/* Delete any children first */
	err = treeview_walk_internal(tree, n,
			TREEVIEW_WALK_MODE_LOGICAL_COMPLETE,
			treeview_delete_node_walk_cb, NULL, &nd);
	if (err != NSERROR_OK) {
		return err;
	}

	/* Now delete node */
	if (n == tree->root)
		tree->root = NULL;
	err = treeview_delete_node_walk_cb(n, &nd, false);
	if (err != NSERROR_OK) {
		return err;
	}

	n = p;
	/* Reduce ancestor heights */
	while (n != NULL && n->flags & TV_NFLAGS_EXPANDED) {
		n->height -= nd.h_reduction;
		n = n->parent;
	}

	/* Inform front end of change in dimensions */
	if (tree->root != NULL && p != NULL && p->flags & TV_NFLAGS_EXPANDED &&
	    nd.h_reduction > 0 &&
	    !(flags & TREE_OPTION_SUPPRESS_RESIZE)) {
		treeview__cw_update_size(tree, -1,
					 tree->root->height);
	}

	treeview__search_update_display(tree);

	return NSERROR_OK;
}


/**
 * Delete any empty treeview folder nodes
 *
 * \param tree Treeview object to delete empty nodes from
 * \param interaction Delete is result of user interaction with treeview
 * \return NSERROR_OK on success, appropriate error otherwise
 *
 * Note this must not be called within a treeview_walk.  It may delete the
 * walker's 'current' node, making it impossible to move on without invalid
 * reads.
 */
static nserror treeview_delete_empty_nodes(treeview *tree, bool interaction)
{
	treeview_node *node, *child, *parent, *next_sibling, *p;
	bool abort = false;
	nserror err;
	struct treeview_node_delete nd = {
		.tree = tree,
		.h_reduction = 0,
		.user_interaction = interaction
	};

	assert(tree != NULL);
	assert(tree->root != NULL);

	node = tree->root;
	parent = node->parent;
	next_sibling = node->next_sib;
	child = (node->flags & TV_NFLAGS_EXPANDED) ? node->children : NULL;

	while (node != NULL) {

		if (child != NULL) {
			/* Down to children */
			node = child;
		} else {
			/* No children.  As long as we're not at the root,
			 * go to next sibling if present, or nearest ancestor
			 * with a next sibling. */

			while (node->parent != NULL &&
			       next_sibling == NULL) {
				if (node->type == TREE_NODE_FOLDER &&
				    node->children == NULL) {
					/* Delete node */
					p = node->parent;
					err = treeview_delete_node_walk_cb(
						node, &nd, &abort);
					if (err != NSERROR_OK) {
						return err;
					}

					/* Reduce ancestor heights */
					while (p != NULL &&
					       p->flags &
					       TV_NFLAGS_EXPANDED) {
						p->height -= nd.h_reduction;
						p = p->parent;
					}
					nd.h_reduction = 0;
				}
				node = parent;
				parent = node->parent;
				next_sibling = node->next_sib;
			}

			if (node->parent == NULL)
				break;

			if (node->type == TREE_NODE_FOLDER &&
			    node->children == NULL) {
				/* Delete node */
				p = node->parent;
				err = treeview_delete_node_walk_cb(
					node, &nd, &abort);
				if (err != NSERROR_OK) {
					return err;
				}

				/* Reduce ancestor heights */
				while (p != NULL &&
				       p->flags & TV_NFLAGS_EXPANDED) {
					p->height -= nd.h_reduction;
					p = p->parent;
				}
				nd.h_reduction = 0;
			}
			node = next_sibling;
		}

		assert(node != NULL);
		assert(node->parent != NULL);

		parent = node->parent;
		next_sibling = node->next_sib;
		child = (node->flags & TV_NFLAGS_EXPANDED) ?
			node->children : NULL;
	}

	return NSERROR_OK;
}


/* Exported interface, documented in treeview.h */
nserror
treeview_delete_node(treeview *tree,
		     treeview_node *n,
		     treeview_node_options_flags flags)
{
	nserror err;
	struct rect r;
	bool visible;

	assert(tree != NULL);
	assert(n != NULL);
	assert(n->parent != NULL);

	visible = n->parent->flags & TV_NFLAGS_EXPANDED;

	r.y0 = treeview_node_y(tree, n);
	r.y1 = tree->root->height;

	err = treeview_delete_node_internal(tree, n, false, flags);
	if (err != NSERROR_OK)
		return err;

	if (tree->flags & TREEVIEW_DEL_EMPTY_DIRS) {
		int h = tree->root->height;
		/* Delete any empty nodes */
		err = treeview_delete_empty_nodes(tree, false);
		if (err != NSERROR_OK)
			return err;

		/* Inform front end of change in dimensions */
		if (tree->root->height != h) {
			r.y0 = 0;
			if (!(flags & TREE_OPTION_SUPPRESS_RESIZE)) {
				treeview__cw_update_size(tree, -1,
							 tree->root->height);
			}
		}
	}

	/* Redraw */
	if (visible && !(flags & TREE_OPTION_SUPPRESS_REDRAW)) {
		r.x0 = 0;
		r.x1 = REDRAW_MAX;
		treeview__cw_invalidate_area(tree, &r);
	}

	return NSERROR_OK;
}


/**
 * Helper to create a textarea.
 *
 * \param[in] tree         The treeview we're creating the textarea for.
 * \param[in] width        The width of the textarea.
 * \param[in] height       The height of the textarea.
 * \param[in] border       The border colour to use.
 * \param[in] background   The background colour to use.
 * \param[in] foreground   The foreground colour to use.
 * \param[in] text         The text style to use for the text area.
 * \param[in] ta_callback  The textarea callback function to give the textarea.
 * \return the textarea pointer on success, or NULL on failure.
 */
static struct textarea *treeview__create_textarea(
		treeview *tree,
		int width,
		int height,
		colour border,
		colour background,
		colour foreground,
		plot_font_style_t text,
		textarea_client_callback ta_callback)
{
	/* Configure the textarea */
	textarea_flags ta_flags = TEXTAREA_INTERNAL_CARET;
	textarea_setup ta_setup = {
		.text = text,
		.width = width,
		.height = height,
		.pad_top = 0,
		.pad_left = 2,
		.pad_right = 2,
		.pad_bottom = 0,
		.border_width = 1,
		.border_col = border,
		.selected_bg = foreground,
		.selected_text = background,
	};

	ta_setup.text.foreground = foreground;
	ta_setup.text.background = background;

	/* Create text area */
	return textarea_create(ta_flags, &ta_setup, ta_callback, tree);
}


/* Exported interface, documented in treeview.h */
nserror
treeview_create(treeview **treeout,
		const struct treeview_callback_table *callbacks,
		int n_fields,
		struct treeview_field_desc fields[],
		struct core_window *cw,
		treeview_flags flags)
{
	treeview *tree;
	nserror error;
	int fldidx;

	assert(callbacks != NULL);

	assert(fields != NULL);
	assert(fields[0].flags & TREE_FLAG_DEFAULT);
	assert(fields[n_fields - 1].flags & TREE_FLAG_DEFAULT);
	assert(n_fields >= 2);

	tree = malloc(sizeof(struct treeview));
	if (tree == NULL) {
		return NSERROR_NOMEM;
	}

	tree->fields = malloc(sizeof(struct treeview_field) * n_fields);
	if (tree->fields == NULL) {
		free(tree);
		return NSERROR_NOMEM;
	}

	error = treeview_create_node_root(&(tree->root));
	if (error != NSERROR_OK) {
		free(tree->fields);
		free(tree);
		return error;
	}

	tree->field_width = 0;
	for (fldidx = 0; fldidx < n_fields; fldidx++) {
		struct treeview_field *f = &(tree->fields[fldidx]);

		f->flags = fields[fldidx].flags;
		f->field = lwc_string_ref(fields[fldidx].field);
		f->value.data = lwc_string_data(fields[fldidx].field);
		f->value.len = lwc_string_length(fields[fldidx].field);

		guit->layout->width(&plot_style_odd.text,
				    f->value.data,
				    f->value.len,
				    &(f->value.width));

		if (f->flags & TREE_FLAG_SHOW_NAME) {
			if (tree->field_width < f->value.width) {
				tree->field_width = f->value.width;
			}
		}
	}

	tree->field_width += tree_g.step_width;

	tree->callbacks = callbacks;
	tree->n_fields = n_fields - 1;

	tree->drag.type = TV_DRAG_NONE;
	tree->drag.start_node = NULL;
	tree->drag.start.x = 0;
	tree->drag.start.y = 0;
	tree->drag.start.node_y = 0;
	tree->drag.start.node_h = 0;
	tree->drag.prev.x = 0;
	tree->drag.prev.y = 0;
	tree->drag.prev.node_y = 0;
	tree->drag.prev.node_h = 0;

	tree->move.root = NULL;
	tree->move.target = NULL;
	tree->move.target_pos = TV_TARGET_NONE;

	tree->edit.textarea = NULL;
	tree->edit.node = NULL;

	if (flags & TREEVIEW_SEARCHABLE) {
		tree->search.textarea = treeview__create_textarea(
				tree, 600, tree_g.line_height,
				nscolours[NSCOLOUR_TEXT_INPUT_BG],
				nscolours[NSCOLOUR_TEXT_INPUT_BG],
				nscolours[NSCOLOUR_TEXT_INPUT_FG],
				plot_style_odd.text,
				treeview_textarea_search_callback);
		if (tree->search.textarea == NULL) {
			treeview_destroy(tree);
			return NSERROR_NOMEM;
		}
	} else {
		tree->search.textarea = NULL;
	}
	tree->search.active = false;
	tree->search.search = false;

	tree->flags = flags;

	tree->cw_h = cw;

	*treeout = tree;

	return NSERROR_OK;
}


/* Exported interface, documented in treeview.h */
nserror
treeview_cw_attach(treeview *tree, struct core_window *cw)
{
	assert(cw != NULL);

	if (tree->cw_h != NULL) {
		NSLOG(netsurf, INFO, "Treeview already attached.");
		return NSERROR_UNKNOWN;
	}
	tree->cw_h = cw;

	return NSERROR_OK;
}


/* Exported interface, documented in treeview.h */
nserror treeview_cw_detach(treeview *tree)
{
	tree->cw_h = NULL;

	treeview__search_cancel(tree, true);

	return NSERROR_OK;
}


/* Exported interface, documented in treeview.h */
nserror treeview_destroy(treeview *tree)
{
	int f;

	assert(tree != NULL);

	if (tree->search.textarea != NULL) {
		tree->search.active = false;
		tree->search.search = false;
		textarea_destroy(tree->search.textarea);
	}

	/* Destroy nodes */
	treeview_delete_node_internal(tree, tree->root, false,
				      TREE_OPTION_SUPPRESS_RESIZE |
				      TREE_OPTION_SUPPRESS_REDRAW);

	/* Destroy feilds */
	for (f = 0; f <= tree->n_fields; f++) {
		lwc_string_unref(tree->fields[f].field);
	}
	free(tree->fields);

	/* Free treeview */
	free(tree);

	return NSERROR_OK;
}


/**
 * Expand a treeview's nodes
 *
 * \param tree Treeview object to expand nodes in
 * \param node The node to expand.
 * \return NSERROR_OK on success, appropriate error otherwise.
 */
static nserror
treeview_node_expand_internal(treeview *tree, treeview_node *node)
{
	treeview_node *child;
	struct treeview_node_entry *e;
	int additional_height_folders = 0;
	int additional_height_entries = 0;
	int i;

	assert(tree != NULL);
	assert(node != NULL);

	if (node->flags & TV_NFLAGS_EXPANDED) {
		/* What madness is this? */
		NSLOG(netsurf, INFO, "Tried to expand an expanded node.");
		return NSERROR_OK;
	}

	switch (node->type) {
	case TREE_NODE_FOLDER:
		child = node->children;
		if (child == NULL) {
			/* Allow expansion of empty folders */
			break;
		}

		do {
			if (child->text.width == 0) {
				guit->layout->width(&plot_style_odd.text,
						    child->text.data,
						    child->text.len,
						    &(child->text.width));
			}

			additional_height_folders += child->height;

			child = child->next_sib;
		} while (child != NULL);

		break;

	case TREE_NODE_ENTRY:
		assert(node->children == NULL);

		e = (struct treeview_node_entry *)node;

		for (i = 0; i < tree->n_fields - 1; i++) {

			if (e->fields[i].value.width == 0) {
				guit->layout->width(&plot_style_odd.text,
						    e->fields[i].value.data,
						    e->fields[i].value.len,
						    &(e->fields[i].value.width));
			}

			/* Add height for field */
			additional_height_entries += tree_g.line_height;
		}

		break;

	case TREE_NODE_ROOT:
	case TREE_NODE_NONE:
		assert(node->type != TREE_NODE_ROOT);
		assert(node->type != TREE_NODE_NONE);
		break;
	}

	/* Update the node */
	node->flags |= TV_NFLAGS_EXPANDED;

	/* And node heights */
	for (struct treeview_node *n = node;
			(n != NULL) && (n->flags & TV_NFLAGS_EXPANDED);
			n = n->parent) {
		n->height += additional_height_entries +
				additional_height_folders;
	}

	if (tree->search.search &&
			node->type == TREE_NODE_ENTRY &&
			node->flags & TV_NFLAGS_MATCHED) {
		tree->search.height += additional_height_entries;
	}

	/* Inform front end of change in dimensions */
	if (additional_height_entries + additional_height_folders != 0) {
		treeview__cw_update_size(tree, -1,
				treeview__get_display_height(tree));
	}

	return NSERROR_OK;
}


/* Exported interface, documented in treeview.h */
nserror treeview_node_expand(treeview *tree, treeview_node *node)
{
	nserror res;

	res = treeview_node_expand_internal(tree, node);
	NSLOG(netsurf, INFO, "Expanding!");
	if (res == NSERROR_OK) {
		/* expansion was successful, attempt redraw */
		treeview__redraw_from_node(tree, node);
		NSLOG(netsurf, INFO, "Expanded!");
	}

	return res;
}


/**
 * context for treeview contraction callback
 */
struct treeview_contract_data {
	treeview *tree;
	bool only_entries;
};


/**
 * Treewalk node callback for handling node contraction.
 *
 * \param n node
 * \param ctx contract iterator context
 * \param end flag to end iteration now
 * \return NSERROR_OK on success else appropriate error code
 */
static nserror treeview_node_contract_cb(treeview_node *n, void *ctx, bool *end)
{
	struct treeview_contract_data *data = ctx;
	int h_reduction_folder = 0;
	int h_reduction_entry = 0;

	assert(n != NULL);
	assert(n->type != TREE_NODE_ROOT);

	n->flags &= ~TV_NFLAGS_SELECTED;

	if ((n->flags & TV_NFLAGS_EXPANDED) == false ||
	    (n->type == TREE_NODE_FOLDER && data->only_entries)) {
		/* Nothing to do. */
		return NSERROR_OK;
	}


	switch (n->type) {
	case TREE_NODE_FOLDER:
		h_reduction_folder = n->height - tree_g.line_height;
		break;

	case TREE_NODE_ENTRY:
		h_reduction_entry = n->height - tree_g.line_height;
		break;

	default:
		break;
	}


	assert(h_reduction_folder + h_reduction_entry >= 0);
	for (struct treeview_node *node = n;
			(node != NULL) && (node->flags & TV_NFLAGS_EXPANDED);
			node = node->parent) {
		node->height -= h_reduction_folder + h_reduction_entry;
	}

	if (data->tree->search.search) {
		data->tree->search.height -= h_reduction_entry;
	}

	n->flags ^= TV_NFLAGS_EXPANDED;

	return NSERROR_OK;
}


/**
 * Contract a treeview node
 *
 * \param tree Treeview object to contract node in
 * \param node Node to contract
 * \return NSERROR_OK on success, appropriate error otherwise
 */
static nserror
treeview_node_contract_internal(treeview *tree, treeview_node *node)
{
	struct treeview_contract_data data;
	bool selected;
	assert(node != NULL);

	if ((node->flags & TV_NFLAGS_EXPANDED) == false) {
		/* What madness is this? */
		NSLOG(netsurf, INFO, "Tried to contract a contracted node.");
		return NSERROR_OK;
	}

	data.tree = tree;
	data.only_entries = false;
	selected = node->flags & TV_NFLAGS_SELECTED;

	/* Contract children. */
	treeview_walk_internal(tree, node, TREEVIEW_WALK_MODE_LOGICAL_EXPANDED,
			treeview_node_contract_cb, NULL, &data);

	/* Contract node */
	treeview_node_contract_cb(node, &data, false);

	if (selected)
		node->flags |= TV_NFLAGS_SELECTED;

	/* Inform front end of change in dimensions */
	treeview__cw_update_size(tree, -1, treeview__get_display_height(tree));

	return NSERROR_OK;
}


/* Exported interface, documented in treeview.h */
nserror treeview_node_contract(treeview *tree, treeview_node *node)
{
	nserror res;

	assert(tree != NULL);

	res = treeview_node_contract_internal(tree, node);
	NSLOG(netsurf, INFO, "Contracting!");
	if (res == NSERROR_OK) {
		/* successful contraction, request redraw */
		treeview__redraw_from_node(tree, node);
		NSLOG(netsurf, INFO, "Contracted!");
	}

	return res;
}


/* Exported interface, documented in treeview.h */
nserror treeview_contract(treeview *tree, bool all)
{
	int search_height = treeview__get_search_height(tree);
	struct treeview_contract_data data;
	bool selected;
	treeview_node *n;
	struct rect r;

	assert(tree != NULL);
	assert(tree->root != NULL);

	r.x0 = 0;
	r.y0 = 0;
	r.x1 = REDRAW_MAX;
	r.y1 = tree->root->height + search_height;

	data.tree = tree;
	data.only_entries = !all;

	for (n = tree->root->children; n != NULL; n = n->next_sib) {
		if ((n->flags & TV_NFLAGS_EXPANDED) == false) {
			continue;
		}

		selected = n->flags & TV_NFLAGS_SELECTED;

		/* Contract children. */
		treeview_walk_internal(tree, n,
				TREEVIEW_WALK_MODE_LOGICAL_EXPANDED,
				treeview_node_contract_cb, NULL, &data);

		/* Contract node */
		treeview_node_contract_cb(n, &data, false);

		if (selected)
			n->flags |= TV_NFLAGS_SELECTED;
	}

	/* Inform front end of change in dimensions */
	treeview__cw_update_size(tree, -1, tree->root->height);

	/* Redraw */
	treeview__cw_invalidate_area(tree, &r);

	return NSERROR_OK;
}


/**
 * context data for treeview expansion
 */
struct treeview_expand_data {
	treeview *tree;
	bool only_folders;
};


/**
 * Treewalk node callback for handling recursive node expansion.
 *
 * \param n current node
 * \param ctx node expansion context
 * \param skip_children flag to allow children to be skipped
 * \param end flag to allow iteration to be finished early.
 * \return NSERROR_OK on success else error code.
 */
static nserror
treeview_expand_cb(treeview_node *n,
		   void *ctx,
		   bool *skip_children,
		   bool *end)
{
	struct treeview_expand_data *data = ctx;
	nserror err;

	assert(n != NULL);
	assert(n->type != TREE_NODE_ROOT);

	if (n->flags & TV_NFLAGS_EXPANDED ||
	    (data->only_folders && n->type != TREE_NODE_FOLDER)) {
		/* Nothing to do. */
		return NSERROR_OK;
	}

	err = treeview_node_expand_internal(data->tree, n);

	return err;
}


/* Exported interface, documented in treeview.h */
nserror treeview_expand(treeview *tree, bool only_folders)
{
	struct treeview_expand_data data;
	nserror res;
	struct rect r;

	assert(tree != NULL);
	assert(tree->root != NULL);

	data.tree = tree;
	data.only_folders = only_folders;

	res = treeview_walk_internal(tree, tree->root,
			TREEVIEW_WALK_MODE_LOGICAL_COMPLETE,
			NULL, treeview_expand_cb, &data);
	if (res == NSERROR_OK) {
		/* expansion succeeded, schedule redraw */

		r.x0 = 0;
		r.y0 = 0;
		r.x1 = REDRAW_MAX;
		r.y1 = tree->root->height;

		treeview__cw_invalidate_area(tree, &r);
	}
	return res;
}


/**
 * Draw a treeview normally, in tree mode.
 *
 * \param[in]     tree      The treeview we're rendering.
 * \param[in]     x         X coordinate we're rendering the treeview at.
 * \param[in]     y         Y coordinate we're rendering the treeview at.
 * \param[in,out] render_y  Current vertical position in tree, updated on exit.
 * \param[in]     r         Clip rectangle.
 * \param[in]     data      Redraw data for rendering contents.
 * \param[in]     ctx       Current render context.
 */
static void treeview_redraw_tree(
		treeview *tree,
		const int x,
		const int y,
		int *render_y_in_out,
		const struct rect *r,
		struct content_redraw_data *data,
		const struct redraw_context *ctx)
{
	struct treeview_node_style *style = &plot_style_odd;
	enum treeview_resource_id res = TREE_RES_CONTENT;
	int baseline = (tree_g.line_height * 3 + 2) / 4;
	plot_font_style_t *infotext_style;
	treeview_node *root = tree->root;
	treeview_node *node = tree->root;
	int render_y = *render_y_in_out;
	plot_font_style_t *text_style;
	plot_style_t *bg_style;
	int sel_min, sel_max;
	uint32_t count = 0;
	struct rect rect;
	int inset;
	int x0;

	if (tree->drag.start.y > tree->drag.prev.y) {
		sel_min = tree->drag.prev.y;
		sel_max = tree->drag.start.y;
	} else {
		sel_min = tree->drag.start.y;
		sel_max = tree->drag.prev.y;
	}

	while (node != NULL) {
		struct treeview_node_entry *entry;
		struct bitmap *furniture;
		bool invert_selection;
		treeview_node *next;
		int height;
		int i;

		next = (node->flags & TV_NFLAGS_EXPANDED) ?
			node->children : NULL;

		if (next != NULL) {
			/* down to children */
			node = next;
		} else {
			/* No children.  As long as we're not at the root,
			 * go to next sibling if present, or nearest ancestor
			 * with a next sibling. */

			while (node != root &&
			       node->next_sib == NULL) {
				node = node->parent;
			}

			if (node == root)
				break;

			node = node->next_sib;
		}

		assert(node != NULL);
		assert(node != root);
		assert(node->type == TREE_NODE_FOLDER ||
		       node->type == TREE_NODE_ENTRY);

		count++;
		inset = x + node->inset;
		height = (node->type == TREE_NODE_ENTRY) ? node->height :
			tree_g.line_height;

		if ((render_y + height) < r->y0) {
			/* This node's line is above clip region */
			render_y += height;
			continue;
		}

		style = (count & 0x1) ? &plot_style_odd : &plot_style_even;
		if (tree->drag.type == TV_DRAG_SELECTION &&
		    (render_y + height >= sel_min &&
		     render_y < sel_max)) {
			invert_selection = true;
		} else {
			invert_selection = false;
		}
		if ((node->flags & TV_NFLAGS_SELECTED && !invert_selection) ||
		    (!(node->flags & TV_NFLAGS_SELECTED) &&
		     invert_selection)) {
			bg_style = &style->sbg;
			text_style = &style->stext;
			infotext_style = &style->sitext;
			furniture = (node->flags & TV_NFLAGS_EXPANDED) ?
				style->furn[TREE_FURN_CONTRACT].sel :
				style->furn[TREE_FURN_EXPAND].sel;
		} else {
			bg_style = &style->bg;
			text_style = &style->text;
			infotext_style = &style->itext;
			furniture = (node->flags & TV_NFLAGS_EXPANDED) ?
				style->furn[TREE_FURN_CONTRACT].bmp :
				style->furn[TREE_FURN_EXPAND].bmp;
		}

		/* Render background */
		rect.x0 = r->x0;
		rect.y0 = render_y;
		rect.x1 = r->x1;
		rect.y1 = render_y + height;
		ctx->plot->rectangle(ctx, bg_style, &rect);

		/* Render toggle */
		ctx->plot->bitmap(ctx,
				furniture,
				inset,
				render_y + tree_g.line_height / 4,
				style->furn[TREE_FURN_EXPAND].size,
				style->furn[TREE_FURN_EXPAND].size,
				bg_style->fill_colour,
				BITMAPF_NONE);

		/* Render icon */
		if (node->type == TREE_NODE_ENTRY) {
			res = TREE_RES_CONTENT;
		} else if (node->flags & TV_NFLAGS_SPECIAL) {
			res = TREE_RES_FOLDER_SPECIAL;
		} else {
			res = TREE_RES_FOLDER;
		}

		if (treeview_res[res].ready) {
			/* Icon resource is available */
			data->x = inset + tree_g.step_width;
			data->y = render_y + ((tree_g.line_height -
					      treeview_res[res].height + 1) / 2);
			data->background_colour = bg_style->fill_colour;

			content_redraw(treeview_res[res].c, data, r, ctx);
		}

		/* Render text */
		x0 = inset + tree_g.step_width + tree_g.icon_step;
		ctx->plot->text(ctx,
				text_style,
				x0, render_y + baseline,
				node->text.data,
				node->text.len);

		/* Rendered the node */
		render_y += tree_g.line_height;
		if (render_y > r->y1) {
			/* Passed the bottom of what's in the clip region.
			 * Done. */
			break;
		}


		if (node->type != TREE_NODE_ENTRY ||
		    !(node->flags & TV_NFLAGS_EXPANDED))
			/* Done everything for this node */
			continue;

		/* Render expanded entry fields */
		entry = (struct treeview_node_entry *)node;
		for (i = 0; i < tree->n_fields - 1; i++) {
			struct treeview_field *ef = &(tree->fields[i + 1]);

			if (ef->flags & TREE_FLAG_SHOW_NAME) {
				int max_width = tree->field_width;

				ctx->plot->text(ctx,
						infotext_style,
						x0 + max_width - ef->value.width - tree_g.step_width,
						render_y + baseline,
						ef->value.data,
						ef->value.len);

				ctx->plot->text(ctx,
						infotext_style,
						x0 + max_width,
						render_y + baseline,
						entry->fields[i].value.data,
						entry->fields[i].value.len);
			} else {
				ctx->plot->text(ctx,
						infotext_style,
						x0, render_y + baseline,
						entry->fields[i].value.data,
						entry->fields[i].value.len);
			}

			/* Rendered the expanded entry field */
			render_y += tree_g.line_height;
		}

		/* Finished rendering expanded entry */

		if (render_y > r->y1) {
			/* Passed the bottom of what's in the clip region.
			 * Done. */
			break;
		}
	}

	*render_y_in_out = render_y;
}


/**
 * Draw a treeview normally, in tree mode.
 *
 * \param[in]     tree      The treeview we're rendering.
 * \param[in]     x         X coordinate we're rendering the treeview at.
 * \param[in]     y         Y coordinate we're rendering the treeview at.
 * \param[in,out] render_y  Current vertical position in tree, updated on exit.
 * \param[in]     r         Clip rectangle.
 * \param[in]     data      Redraw data for rendering contents.
 * \param[in]     ctx       Current render context.
 */
static void treeview_redraw_search(
		treeview *tree,
		const int x,
		const int y,
		int *render_y_in_out,
		const struct rect *r,
		struct content_redraw_data *data,
		const struct redraw_context *ctx)
{
	struct treeview_node_style *style = &plot_style_odd;
	enum treeview_resource_id res = TREE_RES_CONTENT;
	int baseline = (tree_g.line_height * 3 + 2) / 4;
	plot_font_style_t *infotext_style;
	treeview_node *root = tree->root;
	treeview_node *node = tree->root;
	int render_y = *render_y_in_out;
	plot_font_style_t *text_style;
	plot_style_t *bg_style;
	int sel_min, sel_max;
	uint32_t count = 0;
	struct rect rect;
	int inset;
	int x0;

	if (tree->drag.start.y > tree->drag.prev.y) {
		sel_min = tree->drag.prev.y;
		sel_max = tree->drag.start.y;
	} else {
		sel_min = tree->drag.start.y;
		sel_max = tree->drag.prev.y;
	}

	while (node != NULL) {
		struct treeview_node_entry *entry;
		struct bitmap *furniture;
		bool invert_selection;
		treeview_node *next;
		int height;
		int i;

		next = node->children;

		if (next != NULL) {
			/* down to children */
			node = next;
		} else {
			/* No children.  As long as we're not at the root,
			 * go to next sibling if present, or nearest ancestor
			 * with a next sibling. */

			while (node != root &&
			       node->next_sib == NULL) {
				node = node->parent;
			}

			if (node == root)
				break;

			node = node->next_sib;
		}

		assert(node != NULL);
		assert(node != root);
		assert(node->type == TREE_NODE_FOLDER ||
		       node->type == TREE_NODE_ENTRY);

		if (node->type == TREE_NODE_FOLDER ||
				!(node->flags & TV_NFLAGS_MATCHED)) {
			continue;
		}

		count++;
		inset = x + tree_g.window_padding;
		height = node->height;

		if ((render_y + height) < r->y0) {
			/* This node's line is above clip region */
			render_y += height;
			continue;
		}

		style = (count & 0x1) ? &plot_style_odd : &plot_style_even;
		if (tree->drag.type == TV_DRAG_SELECTION &&
		    (render_y + height >= sel_min &&
		     render_y < sel_max)) {
			invert_selection = true;
		} else {
			invert_selection = false;
		}
		if ((node->flags & TV_NFLAGS_SELECTED && !invert_selection) ||
		    (!(node->flags & TV_NFLAGS_SELECTED) &&
		     invert_selection)) {
			bg_style = &style->sbg;
			text_style = &style->stext;
			infotext_style = &style->sitext;
			furniture = (node->flags & TV_NFLAGS_EXPANDED) ?
				style->furn[TREE_FURN_CONTRACT].sel :
				style->furn[TREE_FURN_EXPAND].sel;
		} else {
			bg_style = &style->bg;
			text_style = &style->text;
			infotext_style = &style->itext;
			furniture = (node->flags & TV_NFLAGS_EXPANDED) ?
				style->furn[TREE_FURN_CONTRACT].bmp :
				style->furn[TREE_FURN_EXPAND].bmp;
		}

		/* Render background */
		rect.x0 = r->x0;
		rect.y0 = render_y;
		rect.x1 = r->x1;
		rect.y1 = render_y + height;
		ctx->plot->rectangle(ctx, bg_style, &rect);

		/* Render toggle */
		ctx->plot->bitmap(ctx,
				furniture,
				inset,
				render_y + tree_g.line_height / 4,
				style->furn[TREE_FURN_EXPAND].size,
				style->furn[TREE_FURN_EXPAND].size,
				bg_style->fill_colour,
				BITMAPF_NONE);

		/* Render icon */
		if (node->type == TREE_NODE_ENTRY) {
			res = TREE_RES_CONTENT;
		} else if (node->flags & TV_NFLAGS_SPECIAL) {
			res = TREE_RES_FOLDER_SPECIAL;
		} else {
			res = TREE_RES_FOLDER;
		}

		if (treeview_res[res].ready) {
			/* Icon resource is available */
			data->x = inset + tree_g.step_width;
			data->y = render_y + ((tree_g.line_height -
					      treeview_res[res].height + 1) / 2);
			data->background_colour = bg_style->fill_colour;

			content_redraw(treeview_res[res].c, data, r, ctx);
		}

		/* Render text */
		x0 = inset + tree_g.step_width + tree_g.icon_step;
		ctx->plot->text(ctx,
				text_style,
				x0, render_y + baseline,
				node->text.data,
				node->text.len);

		/* Rendered the node */
		render_y += tree_g.line_height;
		if (render_y > r->y1) {
			/* Passed the bottom of what's in the clip region.
			 * Done. */
			break;
		}


		if (node->type != TREE_NODE_ENTRY ||
		    !(node->flags & TV_NFLAGS_EXPANDED))
			/* Done everything for this node */
			continue;

		/* Render expanded entry fields */
		entry = (struct treeview_node_entry *)node;
		for (i = 0; i < tree->n_fields - 1; i++) {
			struct treeview_field *ef = &(tree->fields[i + 1]);

			if (ef->flags & TREE_FLAG_SHOW_NAME) {
				int max_width = tree->field_width;

				ctx->plot->text(ctx,
						infotext_style,
						x0 + max_width - ef->value.width - tree_g.step_width,
						render_y + baseline,
						ef->value.data,
						ef->value.len);

				ctx->plot->text(ctx,
						infotext_style,
						x0 + max_width,
						render_y + baseline,
						entry->fields[i].value.data,
						entry->fields[i].value.len);
			} else {
				ctx->plot->text(ctx,
						infotext_style,
						x0, render_y + baseline,
						entry->fields[i].value.data,
						entry->fields[i].value.len);
			}

			/* Rendered the expanded entry field */
			render_y += tree_g.line_height;
		}

		/* Finished rendering expanded entry */

		if (render_y > r->y1) {
			/* Passed the bottom of what's in the clip region.
			 * Done. */
			break;
		}
	}

	*render_y_in_out = render_y;
}


/* Exported interface, documented in treeview.h */
void
treeview_redraw(treeview *tree,
		const int x,
		const int y,
		struct rect *clip,
		const struct redraw_context *ctx)
{
	struct redraw_context new_ctx = *ctx;
	struct content_redraw_data data;
	struct rect r;
	struct rect rect;
	int render_y = y;

	assert(tree != NULL);
	assert(tree->root != NULL);
	assert(tree->root->flags & TV_NFLAGS_EXPANDED);

	/* Start knockout rendering if it's available for this plotter */
	if (ctx->plot->option_knockout) {
		knockout_plot_start(ctx, &new_ctx);
	}

	/* Set up clip rectangle */
	r.x0 = clip->x0 + x;
	r.y0 = clip->y0 + y;
	r.x1 = clip->x1 + x;
	r.y1 = clip->y1 + y;
	new_ctx.plot->clip(&new_ctx, &r);

	/* Setup common content redraw data */
	data.width = tree_g.icon_size;
	data.height = tree_g.icon_size;
	data.scale = 1;
	data.repeat_x = false;
	data.repeat_y = false;

	if (tree->flags & TREEVIEW_SEARCHABLE) {
		if (render_y < r.y1) {
			enum treeview_resource_id icon = TREE_RES_SEARCH;

			/* Fill the blank area at the bottom */
			rect.x0 = r.x0;
			rect.y0 = render_y;
			rect.x1 = r.x1;
			rect.y1 = render_y + tree_g.line_height;
			new_ctx.plot->rectangle(&new_ctx, &plot_style_even.bg,
					&rect);

			if (treeview_res[icon].ready) {
				/* Icon resource is available */
				data.x = tree_g.window_padding;
				data.y = render_y + ((tree_g.line_height -
						treeview_res[icon].height + 1) /
						2);
				data.background_colour = plot_style_even.bg.
						fill_colour;

				content_redraw(treeview_res[icon].c,
						&data, &r, &new_ctx);
			}

			textarea_redraw(tree->search.textarea,
					x + tree_g.window_padding +
							tree_g.icon_step, y,
					plot_style_even.bg.fill_colour, 1.0,
					&r, &new_ctx);
		}
		render_y += tree_g.line_height;
	}

	/* Render the treeview data */
	if (tree->search.search == true) {
		treeview_redraw_search(tree, x, y,
				&render_y, &r, &data, &new_ctx);
	} else {
		treeview_redraw_tree(tree, x, y,
				&render_y, &r, &data, &new_ctx);
	}

	if (render_y < r.y1) {
		/* Fill the blank area at the bottom */
		rect.x0 = r.x0;
		rect.y0 = render_y;
		rect.x1 = r.x1;
		rect.y1 = r.y1;
		new_ctx.plot->rectangle(&new_ctx, &plot_style_even.bg, &rect);
	}

	/* All normal treeview rendering is done; render any overlays */
	if ((tree->move.target_pos != TV_TARGET_NONE) &&
	    (treeview_res[TREE_RES_ARROW].ready)) {
		/* Got a MOVE drag; render move indicator arrow */
		data.x = tree->move.target_area.x0 + x;
		data.y = tree->move.target_area.y0 + y;
		data.background_colour = plot_style_even.bg.fill_colour;

		content_redraw(treeview_res[TREE_RES_ARROW].c, &data, &r, &new_ctx);

	} else if (tree->edit.textarea != NULL) {
		/* Edit in progress; render textarea */
		textarea_redraw(tree->edit.textarea,
				tree->edit.x + x, tree->edit.y + y,
				plot_style_even.bg.fill_colour, 1.0,
				&r, &new_ctx);
	}

	/* Rendering complete */
	if (ctx->plot->option_knockout) {
		knockout_plot_end(ctx);
	}
}


/**
 * context for treeview selection
 */
struct treeview_selection_walk_data {
	enum {
		TREEVIEW_WALK_HAS_SELECTION,
		TREEVIEW_WALK_GET_FIRST_SELECTED,
		TREEVIEW_WALK_CLEAR_SELECTION,
		TREEVIEW_WALK_SELECT_ALL,
		TREEVIEW_WALK_COMMIT_SELECT_DRAG,
		TREEVIEW_WALK_DELETE_SELECTION,
		TREEVIEW_WALK_PROPAGATE_SELECTION,
		TREEVIEW_WALK_YANK_SELECTION,
		TREEVIEW_WALK_COPY_SELECTION
	} purpose;
	union {
		bool has_selection;
		struct {
			bool required;
			struct rect *rect;
		} redraw;
		struct {
			int sel_min;
			int sel_max;
		} drag;
		struct {
			treeview_node *prev;
			treeview_node *fixed;
		} yank;
		struct {
			treeview_node *n;
		} first;
		struct {
			char *text;
			uint32_t len;
		} copy;
	} data;
	int current_y;
	treeview *tree;
};


/**
 * Treewalk node callback for handling selection related actions.
 *
 * \param n current node
 * \param ctx node selection context
 * \param skip_children flag to allow children to be skipped
 * \param end flag to allow iteration to be finished early.
 * \return NSERROR_OK on success else error code.
 */
static nserror
treeview_node_selection_walk_cb(treeview_node *n,
				void *ctx,
				bool *skip_children,
				bool *end)
{
	struct treeview_selection_walk_data *sw = ctx;
	int height;
	bool changed = false;
	nserror err;

	height = (n->type == TREE_NODE_ENTRY) ? n->height : tree_g.line_height;
	sw->current_y += height;

	switch (sw->purpose) {
	case TREEVIEW_WALK_HAS_SELECTION:
		if (n->flags & TV_NFLAGS_SELECTED) {
			sw->data.has_selection = true;
			*end = true; /* Can abort tree walk */
			return NSERROR_OK;
		}
		break;

	case TREEVIEW_WALK_GET_FIRST_SELECTED:
		if (n->flags & TV_NFLAGS_SELECTED) {
			sw->data.first.n = n;
			*end = true; /* Can abort tree walk */
			return NSERROR_OK;
		}
		break;

	case TREEVIEW_WALK_DELETE_SELECTION:
		if (n->flags & TV_NFLAGS_SELECTED) {
			err = treeview_delete_node_internal(sw->tree, n, true,
							    TREE_OPTION_NONE);
			if (err != NSERROR_OK) {
				return err;
			}
			*skip_children = true;
			changed = true;
		}
		break;

	case TREEVIEW_WALK_PROPAGATE_SELECTION:
		if (n->parent != NULL &&
		    n->parent->flags & TV_NFLAGS_SELECTED &&
		    !(n->flags & TV_NFLAGS_SELECTED)) {
			n->flags ^= TV_NFLAGS_SELECTED;
			changed = true;
		}
		break;

	case TREEVIEW_WALK_CLEAR_SELECTION:
		if (n->flags & TV_NFLAGS_SELECTED) {
			n->flags ^= TV_NFLAGS_SELECTED;
			changed = true;
		}
		break;

	case TREEVIEW_WALK_SELECT_ALL:
		if (!(n->flags & TV_NFLAGS_SELECTED)) {
			n->flags ^= TV_NFLAGS_SELECTED;
			changed = true;
		}
		break;

	case TREEVIEW_WALK_COMMIT_SELECT_DRAG:
		if (sw->current_y >= sw->data.drag.sel_min &&
		    sw->current_y - height <
		    sw->data.drag.sel_max) {
			n->flags ^= TV_NFLAGS_SELECTED;
		}
		return NSERROR_OK;

	case TREEVIEW_WALK_YANK_SELECTION:
		if (n->flags & TV_NFLAGS_SELECTED) {
			treeview_node *p = n->parent;
			int h = 0;

			if (n == sw->data.yank.fixed) {
				break;
			}

			if (treeview_unlink_node(n))
				h = n->height;

			/* Reduce ancestor heights */
			while (p != NULL && p->flags & TV_NFLAGS_EXPANDED) {
				p->height -= h;
				p = p->parent;
			}
			if (sw->data.yank.prev == NULL) {
				sw->tree->move.root = n;
				n->parent = NULL;
				n->prev_sib = NULL;
				n->next_sib = NULL;
			} else {
				n->parent = NULL;
				n->prev_sib = sw->data.yank.prev;
				n->next_sib = NULL;
				sw->data.yank.prev->next_sib = n;
			}
			sw->data.yank.prev = n;

			*skip_children = true;
		}
		break;

	case TREEVIEW_WALK_COPY_SELECTION:
		if (n->flags & TV_NFLAGS_SELECTED &&
		    n->type == TREE_NODE_ENTRY) {
			int i;
			char *temp;
			uint32_t len;
			const char *text;
			struct treeview_field *ef;
			struct treeview_text *val;

			for (i = 0; i < sw->tree->n_fields; i++) {
				ef = &(sw->tree->fields[i]);

				if (!(ef->flags & TREE_FLAG_COPY_TEXT)) {
					continue;
				}
				val = treeview_get_text_for_field(sw->tree,
								  n, i);
				text = val->data;
				len  = val->len;

				temp = realloc(sw->data.copy.text,
					       sw->data.copy.len + len + 1);
				if (temp == NULL) {
					free(sw->data.copy.text);
					sw->data.copy.text = NULL;
					sw->data.copy.len = 0;
					return NSERROR_NOMEM;
				}

				if (sw->data.copy.len != 0) {
					temp[sw->data.copy.len - 1] = '\n';
				}
				memcpy(temp + sw->data.copy.len, text, len);
				temp[sw->data.copy.len + len] = '\0';
				sw->data.copy.len += len + 1;
				sw->data.copy.text = temp;
			}
		}
		break;
	}

	if (changed) {
		if (sw->data.redraw.required == false) {
			sw->data.redraw.required = true;
			sw->data.redraw.rect->y0 = sw->current_y - height;
		}

		if (sw->current_y > sw->data.redraw.rect->y1) {
			sw->data.redraw.rect->y1 = sw->current_y;
		}
	}

	return NSERROR_OK;
}


/* Exported interface, documented in treeview.h */
bool treeview_has_selection(treeview *tree)
{
	struct treeview_selection_walk_data sw;

	sw.purpose = TREEVIEW_WALK_HAS_SELECTION;
	sw.data.has_selection = false;

	treeview_walk_internal(tree, tree->root,
			TREEVIEW_WALK_MODE_DISPLAY, NULL,
			treeview_node_selection_walk_cb, &sw);

	return sw.data.has_selection;
}


/**
 * Get first selected node (in any)
 *
 * \param tree Treeview object in which to create folder
 * \return the first selected treeview node, or NULL
 */
static treeview_node * treeview_get_first_selected(treeview *tree)
{
	struct treeview_selection_walk_data sw;

	sw.purpose = TREEVIEW_WALK_GET_FIRST_SELECTED;
	sw.data.first.n = NULL;

	treeview_walk_internal(tree, tree->root,
			TREEVIEW_WALK_MODE_DISPLAY, NULL,
			treeview_node_selection_walk_cb, &sw);

	return sw.data.first.n;
}


/* Exported interface, documented in treeview.h */
enum treeview_node_type treeview_get_selection(treeview *tree,
					       void **node_data)
{
	treeview_node *n;

	assert(tree != NULL);

	n = treeview_get_first_selected(tree);

	if (n != NULL && n->type & (TREE_NODE_ENTRY | TREE_NODE_FOLDER)) {
		*node_data = n->client_data;
		return n->type;
	}

	*node_data = NULL;
	return TREE_NODE_NONE;
}


/**
 * Clear any selection in a treeview
 *
 * \param tree Treeview object to clear selection in
 * \param rect Redraw rectangle (if redraw required)
 * \return true iff redraw required
 */
static bool treeview_clear_selection(treeview *tree, struct rect *rect)
{
	struct treeview_selection_walk_data sw;

	rect->x0 = 0;
	rect->y0 = 0;
	rect->x1 = REDRAW_MAX;
	rect->y1 = 0;

	sw.purpose = TREEVIEW_WALK_CLEAR_SELECTION;
	sw.data.redraw.required = false;
	sw.data.redraw.rect = rect;
	sw.current_y = treeview__get_search_height(tree);

	treeview_walk_internal(tree, tree->root,
			TREEVIEW_WALK_MODE_DISPLAY, NULL,
			treeview_node_selection_walk_cb, &sw);

	return sw.data.redraw.required;
}


/**
 * Select all in a treeview
 *
 * \param tree Treeview object to select all in
 * \param rect Redraw rectangle (if redraw required)
 * \return true iff redraw required
 */
static bool treeview_select_all(treeview *tree, struct rect *rect)
{
	struct treeview_selection_walk_data sw;

	rect->x0 = 0;
	rect->y0 = 0;
	rect->x1 = REDRAW_MAX;
	rect->y1 = 0;

	sw.purpose = TREEVIEW_WALK_SELECT_ALL;
	sw.data.redraw.required = false;
	sw.data.redraw.rect = rect;
	sw.current_y = treeview__get_search_height(tree);

	treeview_walk_internal(tree, tree->root,
			TREEVIEW_WALK_MODE_DISPLAY, NULL,
			treeview_node_selection_walk_cb, &sw);

	return sw.data.redraw.required;
}


/**
 * Commit a current selection drag, modifying the node's selection state.
 *
 * \param tree Treeview object to commit drag selection in
 */
static void treeview_commit_selection_drag(treeview *tree)
{
	struct treeview_selection_walk_data sw;

	sw.purpose = TREEVIEW_WALK_COMMIT_SELECT_DRAG;
	sw.current_y = treeview__get_search_height(tree);

	if (tree->drag.start.y > tree->drag.prev.y) {
		sw.data.drag.sel_min = tree->drag.prev.y;
		sw.data.drag.sel_max = tree->drag.start.y;
	} else {
		sw.data.drag.sel_min = tree->drag.start.y;
		sw.data.drag.sel_max = tree->drag.prev.y;
	}

	treeview_walk_internal(tree, tree->root,
			TREEVIEW_WALK_MODE_DISPLAY, NULL,
			treeview_node_selection_walk_cb, &sw);
}


/**
 * Yank a selection to the node move list.
 *
 * \param tree   Treeview object to yank selection from
 * \param fixed  Treeview node that should not be yanked
 */
static void treeview_move_yank_selection(treeview *tree, treeview_node *fixed)
{
	struct treeview_selection_walk_data sw;

	sw.purpose = TREEVIEW_WALK_YANK_SELECTION;
	sw.data.yank.fixed = fixed;
	sw.data.yank.prev = NULL;
	sw.tree = tree;

	treeview_walk_internal(tree, tree->root,
			TREEVIEW_WALK_MODE_DISPLAY, NULL,
			treeview_node_selection_walk_cb, &sw);
}


/**
 * Copy a selection to the clipboard.
 *
 * \param tree Treeview object to yank selection from
 */
static void treeview_copy_selection(treeview *tree)
{
	struct treeview_selection_walk_data sw;
	nserror err;

	sw.purpose = TREEVIEW_WALK_COPY_SELECTION;
	sw.data.copy.text = NULL;
	sw.data.copy.len = 0;
	sw.tree = tree;

	err = treeview_walk_internal(tree, tree->root,
			TREEVIEW_WALK_MODE_DISPLAY, NULL,
			treeview_node_selection_walk_cb, &sw);
	if (err != NSERROR_OK) {
		return;
	}

	if (sw.data.copy.text != NULL) {
		guit->clipboard->set(sw.data.copy.text,
				     sw.data.copy.len - 1, NULL, 0);
		free(sw.data.copy.text);
	}
}


/**
 * Delete a selection.
 *
 * \param tree Treeview object to delete selected nodes from
 * \param rect Updated to redraw rectangle
 * \return true iff redraw required.
 */
static bool treeview_delete_selection(treeview *tree, struct rect *rect)
{
	struct treeview_selection_walk_data sw;

	assert(tree != NULL);
	assert(tree->root != NULL);

	rect->x0 = 0;
	rect->y0 = 0;
	rect->x1 = REDRAW_MAX;
	rect->y1 = treeview__get_display_height(tree);

	sw.purpose = TREEVIEW_WALK_DELETE_SELECTION;
	sw.data.redraw.required = false;
	sw.data.redraw.rect = rect;
	sw.current_y = treeview__get_search_height(tree);
	sw.tree = tree;

	treeview_walk_internal(tree, tree->root,
			TREEVIEW_WALK_MODE_DISPLAY, NULL,
			treeview_node_selection_walk_cb, &sw);

	return sw.data.redraw.required;
}


/**
 * Propagate selection to visible descendants of selected nodes.
 *
 * \param tree Treeview object to propagate selection in
 * \param rect Redraw rectangle (if redraw required)
 * \return true iff redraw required
 */
static bool treeview_propagate_selection(treeview *tree, struct rect *rect)
{
	struct treeview_selection_walk_data sw;

	assert(tree != NULL);
	assert(tree->root != NULL);

	rect->x0 = 0;
	rect->y0 = 0;
	rect->x1 = REDRAW_MAX;
	rect->y1 = 0;

	sw.purpose = TREEVIEW_WALK_PROPAGATE_SELECTION;
	sw.data.redraw.required = false;
	sw.data.redraw.rect = rect;
	sw.current_y = treeview__get_search_height(tree);
	sw.tree = tree;

	treeview_walk_internal(tree, tree->root,
			TREEVIEW_WALK_MODE_DISPLAY, NULL,
			treeview_node_selection_walk_cb, &sw);

	return sw.data.redraw.required;
}


/**
 * Move a selection according to the current move drag.
 *
 * \param tree Treeview object to move selected nodes in
 * \param rect Redraw rectangle
 * \return NSERROR_OK on success else appropriate error code
 */
static nserror treeview_move_selection(treeview *tree, struct rect *rect)
{
	treeview_node *node, *next, *parent;
	treeview_node *relation;
	enum treeview_relationship relationship;
	int height;

	assert(tree != NULL);
	assert(tree->root != NULL);
	assert(tree->root->children != NULL);
	assert(tree->move.target_pos != TV_TARGET_NONE);

	height = tree->root->height;

	/* Identify target location */
	switch (tree->move.target_pos) {
	case TV_TARGET_ABOVE:
		if (tree->move.target == NULL) {
			/* Target: After last child of root */
			relation = tree->root->children;
			while (relation->next_sib != NULL) {
				relation = relation->next_sib;
			}
			relationship = TREE_REL_NEXT_SIBLING;

		} else if (tree->move.target->prev_sib != NULL) {
			/* Target: After previous sibling */
			relation = tree->move.target->prev_sib;
			relationship = TREE_REL_NEXT_SIBLING;

		} else {
			/* Target: Target: First child of parent */
			assert(tree->move.target->parent != NULL);
			relation = tree->move.target->parent;
			relationship = TREE_REL_FIRST_CHILD;
		}
		break;

	case TV_TARGET_INSIDE:
		assert(tree->move.target != NULL);
		relation = tree->move.target;
		relationship = TREE_REL_FIRST_CHILD;
		break;

	case TV_TARGET_BELOW:
		assert(tree->move.target != NULL);
		relation = tree->move.target;
		relationship = TREE_REL_NEXT_SIBLING;
		break;

	default:
		NSLOG(netsurf, INFO, "Bad drop target for move.");
		return NSERROR_BAD_PARAMETER;
	}

	if (relationship == TREE_REL_FIRST_CHILD) {
		parent = relation;
	} else {
		parent = relation->parent;
	}

	/* Move all selected nodes from treeview to tree->move.root */
	treeview_move_yank_selection(tree, relation);

	/* Move all nodes on tree->move.root to target location */
	for (node = tree->move.root; node != NULL; node = next) {
		next = node->next_sib;

		if (node == relation) {
			continue;
		}

		if (!(parent->flags & TV_NFLAGS_EXPANDED)) {
			if (node->flags & TV_NFLAGS_EXPANDED)
				treeview_node_contract_internal(tree, node);
			node->flags &= ~TV_NFLAGS_SELECTED;
		}

		treeview_insert_node(tree, node, relation, relationship);

		relation = node;
		relationship = TREE_REL_NEXT_SIBLING;
	}
	tree->move.root = NULL;

	/* Tell window, if height has changed */
	if (height != tree->root->height)
		treeview__cw_update_size(tree, -1, tree->root->height);

	/* TODO: Deal with redraw area properly */
	rect->x0 = 0;
	rect->y0 = 0;
	rect->x1 = REDRAW_MAX;
	rect->y1 = REDRAW_MAX;

	return NSERROR_OK;
}


/**
 * context for treeview launch action
 */
struct treeview_launch_walk_data {
	int selected_depth;
	treeview *tree;
};


/**
 * Treewalk node walk backward callback for tracking folder selection.
 */
static nserror
treeview_node_launch_walk_bwd_cb(treeview_node *n, void *ctx, bool *end)
{
	struct treeview_launch_walk_data *lw = ctx;

	if (n->type == TREE_NODE_FOLDER && n->flags == TV_NFLAGS_SELECTED) {
		lw->selected_depth--;
	}

	return NSERROR_OK;
}


/**
 * Treewalk node walk forward callback for launching nodes.
 *
 * \param n current node
 * \param ctx node launch context
 * \param skip_children flag to allow children to be skipped
 * \param end flag to allow iteration to be finished early.
 * \return NSERROR_OK on success else error code.
 */
static nserror
treeview_node_launch_walk_fwd_cb(treeview_node *n,
				 void *ctx,
				 bool *skip_children,
				 bool *end)
{
	struct treeview_launch_walk_data *lw = ctx;
	nserror ret = NSERROR_OK;

	if (n->type == TREE_NODE_FOLDER && n->flags & TV_NFLAGS_SELECTED) {
		lw->selected_depth++;

	} else if (n->type == TREE_NODE_ENTRY &&
		   (n->flags & TV_NFLAGS_SELECTED ||
		    lw->selected_depth > 0)) {
		struct treeview_node_msg msg;
		msg.msg = TREE_MSG_NODE_LAUNCH;
		msg.data.node_launch.mouse = BROWSER_MOUSE_HOVER;
		ret = lw->tree->callbacks->entry(msg, n->client_data);
	}

	return ret;
}


/**
 * Launch a selection.
 *
 * \note Selected entries are launched. Entries that are descendants
 *        of selected folders are also launched.
 *
 * \param tree Treeview object to launch selected nodes in
 * \return NSERROR_OK on success, appropriate error otherwise
 */
static nserror treeview_launch_selection(treeview *tree)
{
	struct treeview_launch_walk_data lw;

	assert(tree != NULL);
	assert(tree->root != NULL);

	lw.selected_depth = 0;
	lw.tree = tree;

	return treeview_walk_internal(tree, tree->root,
			TREEVIEW_WALK_MODE_LOGICAL_COMPLETE,
			treeview_node_launch_walk_bwd_cb,
			treeview_node_launch_walk_fwd_cb, &lw);
}


/* Exported interface, documented in treeview.h */
nserror
treeview_get_relation(treeview *tree,
		      treeview_node **relation,
		      enum treeview_relationship *rel,
		      bool at_y,
		      int y)
{
	treeview_node *n;

	assert(tree != NULL);

	if (at_y) {
		n = treeview_y_node(tree, y);

	} else {
		n = treeview_get_first_selected(tree);
	}

	if (n != NULL && n->parent != NULL) {
		if (n == n->parent->children) {
			/* First child */
			*relation = n->parent;
			*rel = TREE_REL_FIRST_CHILD;
		} else {
			/* Not first child */
			*relation = n->prev_sib;
			*rel = TREE_REL_NEXT_SIBLING;
		}
	} else {
		if (tree->root->children == NULL) {
			/* First child of root */
			*relation = tree->root;
			*rel = TREE_REL_FIRST_CHILD;
		} else {
			/* Last child of root */
			n = tree->root->children;
			while (n->next_sib != NULL)
				n = n->next_sib;
			*relation = n;
			*rel = TREE_REL_NEXT_SIBLING;
		}
	}

	return NSERROR_OK;
}


/**
 * context for treeview keyboard action
 */
struct treeview_nav_state {
	treeview *tree;
	treeview_node *prev;
	treeview_node *curr;
	treeview_node *next;
	treeview_node *last;
	int n_selected;
	int prev_n_selected;
};


/**
 * Treewalk node callback for handling mouse action.
 *
 * \param node current node
 * \param ctx node context
 * \param skip_children flag to allow children to be skipped
 * \param end flag to allow iteration to be finished early.
 * \return NSERROR_OK on success else error code.
 */
static nserror
treeview_node_nav_cb(treeview_node *node,
		     void *ctx,
		     bool *skip_children,
		     bool *end)
{
	struct treeview_nav_state *ns = ctx;

	if (node == ns->tree->root)
		return NSERROR_OK;

	if (node->flags & TV_NFLAGS_SELECTED) {
		ns->n_selected++;
		if (ns->curr == NULL) {
			ns->curr = node;
		}

	} else {
		if (ns->n_selected == 0) {
			ns->prev = node;

		} else if (ns->prev_n_selected < ns->n_selected) {
			ns->next = node;
			ns->prev_n_selected = ns->n_selected;
		}
	}
	ns->last = node;

	return NSERROR_OK;
}


/**
 * Handle keyboard navigation.
 *
 * \note Selected entries are launched.
 *       Entries that are descendants of selected folders are also launched.
 *
 * \param tree Treeview object to launch selected nodes in
 * \param key The ucs4 character codepoint
 * \param rect Updated to redraw rectangle
 * \return true if treeview needs redraw, false otherwise
 */
static bool
treeview_keyboard_navigation(treeview *tree, uint32_t key, struct rect *rect)
{
	struct treeview_nav_state ns = {
		.tree = tree,
		.prev = NULL,
		.curr = NULL,
		.next = NULL,
		.last = NULL,
		.n_selected = 0,
		.prev_n_selected = 0
	};
	int search_height = treeview__get_search_height(tree);
	int h = treeview__get_display_height(tree) + search_height;
	bool redraw = false;
	struct treeview_node *scroll_to_node = NULL;

	/* Fill out the nav. state struct, by examining the current selection
	 * state */
	treeview_walk_internal(tree, tree->root,
			TREEVIEW_WALK_MODE_DISPLAY, NULL,
			treeview_node_nav_cb, &ns);

	scroll_to_node = ns.curr;

	if (tree->search.search == false) {
		if (ns.next == NULL)
			ns.next = tree->root->children;
		if (ns.prev == NULL)
			ns.prev = ns.last;
	}

	/* Clear any existing selection */
	redraw = treeview_clear_selection(tree, rect);

	switch (key) {
	case NS_KEY_LEFT:
		if (tree->search.search == true) {
			break;
		}
		if (ns.curr != NULL &&
		    ns.curr->parent != NULL &&
		    ns.curr->parent->type != TREE_NODE_ROOT) {
			/* Step to parent */
			ns.curr->parent->flags |= TV_NFLAGS_SELECTED;
			scroll_to_node = ns.curr->parent;

		} else if (ns.curr != NULL && tree->root->children != NULL) {
			/* Select first node in tree */
			tree->root->children->flags |= TV_NFLAGS_SELECTED;
			scroll_to_node = tree->root->children;
		}
		break;

	case NS_KEY_RIGHT:
		if (ns.curr != NULL) {
			if (!(ns.curr->flags & TV_NFLAGS_EXPANDED)) {
				/* Toggle node to expanded */
				treeview_node_expand_internal(tree, ns.curr);
				if (ns.curr->children != NULL) {
					/* Step to first child */
					ns.curr->children->flags |=
						TV_NFLAGS_SELECTED;
					scroll_to_node = ns.curr->children;
				} else {
					/* Retain current node selection */
					ns.curr->flags |= TV_NFLAGS_SELECTED;
				}
			} else {
				/* Toggle node to contracted */
				treeview_node_contract_internal(tree, ns.curr);
				/* Retain current node selection */
				ns.curr->flags |= TV_NFLAGS_SELECTED;
			}

		} else if (ns.curr != NULL) {
			/* Retain current node selection */
			ns.curr->flags |= TV_NFLAGS_SELECTED;
		}
		break;

	case NS_KEY_UP:
		if (ns.prev != NULL) {
			/* Step to previous node */
			ns.prev->flags |= TV_NFLAGS_SELECTED;
			scroll_to_node = ns.prev;
		}
		break;

	case NS_KEY_DOWN:
		if (ns.next != NULL) {
			/* Step to next node */
			ns.next->flags |= TV_NFLAGS_SELECTED;
			scroll_to_node = ns.next;
		}
		break;

	default:
		break;
	}

	treeview__cw_scroll_to_node(tree, scroll_to_node);

	/* TODO: Deal with redraw area properly */
	rect->x0 = 0;
	rect->y0 = 0;
	rect->x1 = REDRAW_MAX;
	if (treeview__get_display_height(tree) + search_height > h)
		rect->y1 = treeview__get_display_height(tree) + search_height;
	else
		rect->y1 = h;
	redraw = true;

	return redraw;
}


/* Exported interface, documented in treeview.h */
bool treeview_keypress(treeview *tree, uint32_t key)
{
	struct rect r;	/**< Redraw rectangle */
	bool redraw = false;

	assert(tree != NULL);

	/* Pass to any textarea, if editing in progress */
	if (tree->edit.textarea != NULL) {
		switch (key) {
		case NS_KEY_ESCAPE:
			treeview_edit_cancel(tree, true);
			return true;
		case NS_KEY_NL:
		case NS_KEY_CR:
			treeview_edit_done(tree);
			return true;
		default:
			return textarea_keypress(tree->edit.textarea, key);
		}
	} else if (tree->search.active == true) {
		switch (key) {
		case NS_KEY_ESCAPE:
			treeview__search_cancel(tree, false);
			return true;
		case NS_KEY_NL:
		case NS_KEY_CR:
			return true;
		default:
			return textarea_keypress(tree->search.textarea, key);
		}
	}

	/* Keypress to be handled by treeview */
	switch (key) {
	case NS_KEY_SELECT_ALL:
		redraw = treeview_select_all(tree, &r);
		break;
	case NS_KEY_COPY_SELECTION:
		treeview_copy_selection(tree);
		break;
	case NS_KEY_DELETE_LEFT:
	case NS_KEY_DELETE_RIGHT:
		redraw = treeview_delete_selection(tree, &r);

		if (tree->flags & TREEVIEW_DEL_EMPTY_DIRS) {
			int h = tree->root->height;
			/* Delete any empty nodes */
			treeview_delete_empty_nodes(tree, false);

			/* Inform front end of change in dimensions */
			if (tree->root->height != h) {
				r.y0 = 0;
				treeview__cw_update_size(tree, -1,
							 tree->root->height);
			}
		}
		break;
	case NS_KEY_CR:
	case NS_KEY_NL:
		treeview_launch_selection(tree);
		break;
	case NS_KEY_ESCAPE:
	case NS_KEY_CLEAR_SELECTION:
		redraw = treeview_clear_selection(tree, &r);
		break;
	case NS_KEY_LEFT:
	case NS_KEY_RIGHT:
	case NS_KEY_UP:
	case NS_KEY_DOWN:
		redraw = treeview_keyboard_navigation(tree, key, &r);
		break;
	default:
		return false;
	}

	if (redraw) {
		treeview__cw_invalidate_area(tree, &r);
	}

	return true;
}


/**
 * Set the drag&drop drop indicator
 *
 * \param tree		Treeview object to set node indicator in
 * \param need_redraw	True iff we already have a redraw region
 * \param target	The treeview node with mouse pointer over it
 * \param node_height	The height of node
 * \param node_y	The Y coord of the top of target node
 * \param mouse_y	Y coord of mouse position
 * \param rect		Redraw rectangle (if redraw required)
 * \return true iff redraw required
 */
static bool
treeview_set_move_indicator(treeview *tree,
			    bool need_redraw,
			    treeview_node *target,
			    int node_height,
			    int node_y,
			    int mouse_y,
			    struct rect *rect)
{
	treeview_node *orig = target;
	enum treeview_target_pos target_pos;
	int mouse_pos = mouse_y - node_y;
	int x;

	assert(tree != NULL);
	assert(tree->root != NULL);
	assert(tree->root->children != NULL);
	assert(target != NULL);

	if (target->flags & TV_NFLAGS_SELECTED) {
		/* Find top selected ancestor */
		while (target->parent &&
		       target->parent->flags & TV_NFLAGS_SELECTED) {
			target = target->parent;
		}

		/* Find top adjacent selected sibling */
		while (target->prev_sib &&
		       target->prev_sib->flags & TV_NFLAGS_SELECTED) {
			target = target->prev_sib;
		}
		target_pos = TV_TARGET_ABOVE;

	} else switch (target->type) {
		case TREE_NODE_FOLDER:
			if (mouse_pos <= node_height / 4) {
				target_pos = TV_TARGET_ABOVE;
			} else if (mouse_pos <= (3 * node_height) / 4 ||
				   target->flags & TV_NFLAGS_EXPANDED) {
				target_pos = TV_TARGET_INSIDE;
			} else {
				target_pos = TV_TARGET_BELOW;
			}
			break;

		case TREE_NODE_ENTRY:
			if (mouse_pos <= node_height / 2) {
				target_pos = TV_TARGET_ABOVE;
			} else {
				target_pos = TV_TARGET_BELOW;
			}
			break;

		default:
			assert(target->type != TREE_NODE_ROOT);
			return false;
		}

	if (target_pos == tree->move.target_pos &&
	    target == tree->move.target) {
		/* No change */
		return need_redraw;
	}

	if (tree->move.target_pos != TV_TARGET_NONE) {
		/* Need to clear old indicator position */
		if (need_redraw) {
			if (rect->x0 > tree->move.target_area.x0)
				rect->x0 = tree->move.target_area.x0;
			if (tree->move.target_area.x1 > rect->x1)
				rect->x1 = tree->move.target_area.x1;
			if (rect->y0 > tree->move.target_area.y0)
				rect->y0 = tree->move.target_area.y0;
			if (tree->move.target_area.y1 > rect->y1)
				rect->y1 = tree->move.target_area.y1;
		} else {
			*rect = tree->move.target_area;
			need_redraw = true;
		}
	}

	/* Offset for ABOVE / BELOW */
	if (target_pos == TV_TARGET_ABOVE) {
		if (target != orig) {
			node_y = treeview_node_y(tree, target);
		}
		node_y -= (tree_g.line_height + 1) / 2;
	} else if (target_pos == TV_TARGET_BELOW) {
		node_y += node_height - (tree_g.line_height + 1) / 2;
	}

	/* Oftsets are all relative to centred (INSIDE) */
	node_y += (tree_g.line_height -
		   treeview_res[TREE_RES_ARROW].height + 1) / 2;

	x = target->inset + tree_g.move_offset;

	/* Update target details */
	tree->move.target = target;
	tree->move.target_pos = target_pos;
	tree->move.target_area.x0 = x;
	tree->move.target_area.y0 = node_y;
	tree->move.target_area.x1 = tree_g.icon_size + x;
	tree->move.target_area.y1 = tree_g.icon_size + node_y;

	if (target_pos != TV_TARGET_NONE) {
		/* Need to draw new indicator position */
		if (need_redraw) {
			if (rect->x0 > tree->move.target_area.x0)
				rect->x0 = tree->move.target_area.x0;
			if (tree->move.target_area.x1 > rect->x1)
				rect->x1 = tree->move.target_area.x1;
			if (rect->y0 > tree->move.target_area.y0)
				rect->y0 = tree->move.target_area.y0;
			if (tree->move.target_area.y1 > rect->y1)
				rect->y1 = tree->move.target_area.y1;
		} else {
			*rect = tree->move.target_area;
			need_redraw = true;
		}
	}

	return need_redraw;
}


/**
 * Callback for textarea_create, in desktop/treeview.h
 *
 * \param data treeview context
 * \param msg textarea message
 */
static void treeview_textarea_callback(void *data, struct textarea_msg *msg)
{
	treeview *tree = data;
	struct rect *r;

	switch (msg->type) {
	case TEXTAREA_MSG_DRAG_REPORT:
		if (msg->data.drag == TEXTAREA_DRAG_NONE) {
			/* Textarea drag finished */
			tree->drag.type = TV_DRAG_NONE;
		} else {
			/* Textarea drag started */
			tree->drag.type = TV_DRAG_TEXTAREA;
		}
		treeview__cw_drag_status(tree,
				treeview__get_cw_drag_type(tree));
		break;

	case TEXTAREA_MSG_REDRAW_REQUEST:
		r = &msg->data.redraw;
		r->x0 += tree->edit.x;
		r->y0 += tree->edit.y;
		r->x1 += tree->edit.x;
		r->y1 += tree->edit.y;

		/* Redraw the textarea */
		treeview__cw_invalidate_area(tree, r);
		break;

	default:
		break;
	}
}


/**
 * Start edit of node field, at given y-coord, if editable
 *
 * \param tree Treeview object to consider editing in
 * \param n The treeview node to try editing
 * \param node_y The Y coord of the top of n
 * \param mouse_x X coord of mouse position
 * \param mouse_y Y coord of mouse position
 * \param rect Redraw rectangle (if redraw required)
 * \return true iff redraw required
 */
static bool
treeview_edit_node_at_point(treeview *tree,
			    treeview_node *n,
			    int node_y,
			    int mouse_x,
			    int mouse_y,
			    struct rect *rect)
{
	struct treeview_text *field_data = NULL;
	struct treeview_field *ef, *field_desc = NULL;
	int pos = node_y + tree_g.line_height;
	int field_y = node_y;
	int field_x;
	int width, height;
	bool success;

	/* If the main field is editable, make field_data point to it */
	if (n->type == TREE_NODE_ENTRY)
		ef = &(tree->fields[0]);
	else
		ef = &(tree->fields[tree->n_fields]);
	if (ef->flags & TREE_FLAG_ALLOW_EDIT) {
		field_data = &n->text;
		field_desc = ef;
		field_y = node_y;
	}

	/* Check for editable entry fields */
	if (n->type == TREE_NODE_ENTRY && n->height != tree_g.line_height) {
		struct treeview_node_entry *e = (struct treeview_node_entry *)n;
		int i;

		for (i = 0; i < tree->n_fields - 1; i++) {
			if (mouse_y <= pos)
				continue;

			ef = &(tree->fields[i + 1]);
			pos += tree_g.line_height;
			if (mouse_y <= pos && (ef->flags &
					       TREE_FLAG_ALLOW_EDIT)) {
				field_data = &e->fields[i].value;
				field_desc = ef;
				field_y = pos - tree_g.line_height;
			}
		}
	}

	if (field_data == NULL || field_desc == NULL) {
		/* No editable field */
		return false;
	}

	/* Get window width/height */
	treeview__cw_get_window_dimensions(tree, &width, &height);

	/* Calculate textarea width/height */
	field_x = n->inset + tree_g.step_width + tree_g.icon_step - 3;
	width -= field_x;
	height = tree_g.line_height;

	/* Create text area */
	tree->edit.textarea = treeview__create_textarea(tree, width, height,
			0x000000, 0xffffff, 0x000000, plot_style_odd.text,
			treeview_textarea_callback);
	if (tree->edit.textarea == NULL) {
		return false;
	}

	success = textarea_set_text(tree->edit.textarea, field_data->data);
	if (!success) {
		textarea_destroy(tree->edit.textarea);
		return false;
	}

	tree->edit.node = n;
	tree->edit.field = field_desc->field;

	/* Position the caret */
	mouse_x -= field_x;
	if (mouse_x < 0)
		mouse_x = 0;
	else if (mouse_x >= width)
		mouse_x = width - 1;

	textarea_mouse_action(tree->edit.textarea,
			      BROWSER_MOUSE_PRESS_1 | BROWSER_MOUSE_CLICK_1,
			      mouse_x, tree_g.line_height / 2);

	/* Position the textarea */
	tree->edit.x = field_x;
	tree->edit.y = field_y;
	tree->edit.w = width;
	tree->edit.h = height;

	/* Setup redraw rectangle */
	if (rect->x0 > field_x)
		rect->x0 = field_x;
	if (rect->y0 > field_y)
		rect->y0 = field_y;
	if (rect->x1 < field_x + width)
		rect->x1 = field_x + width;
	if (rect->y1 < field_y + height)
		rect->y1 = field_y + height;

	return true;
}


/* Exported interface, documented in treeview.h */
void treeview_edit_selection(treeview *tree)
{
	struct rect rect;
	treeview_node *n;
	bool redraw;
	int y;

	assert(tree != NULL);
	assert(tree->root != NULL);

	/* Get first selected node */
	n = treeview_get_first_selected(tree);

	if (n == NULL)
		return;

	/* Get node's y-position */
	y = treeview_node_y(tree, n);

	/* Edit node at y */
	redraw = treeview_edit_node_at_point(tree, n, y,
					     0, y + tree_g.line_height / 2, &rect);

	if (redraw == false)
		return;

	/* Redraw */
	rect.x0 = 0;
	rect.y0 = y;
	rect.x1 = REDRAW_MAX;
	rect.y1 = y + tree_g.line_height;
	treeview__cw_invalidate_area(tree, &rect);
}


/**
 * context for treeview mouse handling
 */
struct treeview_mouse_action {
	treeview *tree;
	browser_mouse_state mouse;
	int x;
	int y;
	int current_y;	/* Y coordinate value of top of current node */
	int search_height;
};


/**
 * Treewalk node callback for handling mouse action.
 *
 * \param node current node
 * \param ctx node context
 * \param skip_children flag to allow children to be skipped
 * \param end flag to allow iteration to be finished early.
 * \return NSERROR_OK on success else error code.
 */
static nserror
treeview_node_mouse_action_cb(treeview_node *node,
			      void *ctx,
			      bool *skip_children,
			      bool *end)
{
	struct treeview_mouse_action *ma = ctx;
	struct rect r;
	bool redraw = false;
	bool click;
	int height;
	enum {
		TV_NODE_ACTION_NONE		= 0,
		TV_NODE_ACTION_SELECTION	= (1 << 0)
	} action = TV_NODE_ACTION_NONE;
	enum treeview_node_part part = TV_NODE_PART_NONE;
	nserror err;

	r.x0 = 0;
	r.x1 = REDRAW_MAX;

	height = (node->type == TREE_NODE_ENTRY) ? node->height :
		tree_g.line_height;

	/* Skip line if we've not reached mouse y */
	if (ma->y > ma->current_y + height) {
		ma->current_y += height;
		return NSERROR_OK; /* Don't want to abort tree walk */
	}

	/* Find where the mouse is */
	if (ma->y <= ma->current_y + tree_g.line_height) {
		int inset = node->inset;
		if (ma->tree->search.search == true) {
			inset = tree_g.window_padding;
		}
		if (ma->x >= inset - 1 &&
				ma->x < inset + tree_g.step_width) {
			/* Over expansion toggle */
			part = TV_NODE_PART_TOGGLE;

		} else if (ma->x >= inset + tree_g.step_width &&
				ma->x < inset + tree_g.step_width +
				tree_g.icon_step + node->text.width) {
			/* On node */
			part = TV_NODE_PART_ON_NODE;
		}
	} else if (node->type == TREE_NODE_ENTRY &&
		   height > tree_g.line_height) {
		/* Expanded entries */
		int x = node->inset + tree_g.step_width + tree_g.icon_step;
		int y = ma->current_y + tree_g.line_height;
		int i;
		struct treeview_node_entry *entry =
			(struct treeview_node_entry *)node;
		for (i = 0; i < ma->tree->n_fields - 1; i++) {
			struct treeview_field *ef = &(ma->tree->fields[i + 1]);

			if (ma->y > y + tree_g.line_height) {
				y += tree_g.line_height;
				continue;
			}

			if (ef->flags & TREE_FLAG_SHOW_NAME) {
				int max_width = ma->tree->field_width;

				if (ma->x >= x + max_width - ef->value.width -
				    tree_g.step_width &&
				    ma->x < x + max_width -
				    tree_g.step_width) {
					/* On a field name */
					part = TV_NODE_PART_ON_NODE;

				} else if (ma->x >= x + max_width &&
					   ma->x < x + max_width +
					   entry->fields[i].value.width) {
					/* On a field value */
					part = TV_NODE_PART_ON_NODE;
				}
			} else {
				if (ma->x >= x && ma->x < x +
				    entry->fields[i].value.width) {
					/* On a field value */
					part = TV_NODE_PART_ON_NODE;
				}
			}

			break;
		}
	}

	/* Record what position / part a drag started on */
	if (ma->mouse & (BROWSER_MOUSE_PRESS_1 | BROWSER_MOUSE_PRESS_2) &&
	    ma->tree->drag.type == TV_DRAG_NONE) {
		ma->tree->drag.selected = node->flags & TV_NFLAGS_SELECTED;
		ma->tree->drag.start_node = node;
		ma->tree->drag.part = part;
		ma->tree->drag.start.x = ma->x;
		ma->tree->drag.start.y = ma->y;
		ma->tree->drag.start.node_y = ma->current_y;
		ma->tree->drag.start.node_h = height;

		ma->tree->drag.prev.x = ma->x;
		ma->tree->drag.prev.y = ma->y;
		ma->tree->drag.prev.node_y = ma->current_y;
		ma->tree->drag.prev.node_h = height;
	}

	/* Handle drag start */
	if (ma->tree->drag.type == TV_DRAG_NONE) {
		if (ma->mouse & BROWSER_MOUSE_DRAG_1 &&
		    ma->tree->drag.selected == false &&
		    ma->tree->drag.part == TV_NODE_PART_NONE) {
			ma->tree->drag.type = TV_DRAG_SELECTION;
			treeview__cw_drag_status(ma->tree,
						 CORE_WINDOW_DRAG_SELECTION);

		} else if (ma->tree->search.search == false &&
			   !(ma->tree->flags & TREEVIEW_NO_MOVES) &&
			   ma->mouse & BROWSER_MOUSE_DRAG_1 &&
			   (ma->tree->drag.selected == true ||
			    ma->tree->drag.part == TV_NODE_PART_ON_NODE)) {
			ma->tree->drag.type = TV_DRAG_MOVE;
			treeview__cw_drag_status(ma->tree,
						 CORE_WINDOW_DRAG_MOVE);
			redraw |= treeview_propagate_selection(ma->tree, &r);

		} else if (ma->mouse & BROWSER_MOUSE_DRAG_2) {
			ma->tree->drag.type = TV_DRAG_SELECTION;
			treeview__cw_drag_status(ma->tree,
						 CORE_WINDOW_DRAG_SELECTION);
		}

		if (ma->tree->drag.start_node != NULL &&
		    ma->tree->drag.type == TV_DRAG_SELECTION) {
			ma->tree->drag.start_node->flags ^= TV_NFLAGS_SELECTED;
		}
	}

	/* Handle active drags */
	switch (ma->tree->drag.type) {
	case TV_DRAG_SELECTION:
	{
		int curr_y1 = ma->current_y + height;
		int prev_y1 = ma->tree->drag.prev.node_y +
			ma->tree->drag.prev.node_h;

		r.y0 = (ma->current_y < ma->tree->drag.prev.node_y) ?
			ma->current_y : ma->tree->drag.prev.node_y;
		r.y1 = (curr_y1 > prev_y1) ? curr_y1 : prev_y1;

		redraw = true;

		ma->tree->drag.prev.x = ma->x;
		ma->tree->drag.prev.y = ma->y;
		ma->tree->drag.prev.node_y = ma->current_y;
		ma->tree->drag.prev.node_h = height;
	}
		break;

	case TV_DRAG_MOVE:
		redraw |= treeview_set_move_indicator(ma->tree, redraw,
						      node, height,
						      ma->current_y, ma->y, &r);
		break;

	default:
		break;
	}

	click = ma->mouse & (BROWSER_MOUSE_CLICK_1 | BROWSER_MOUSE_CLICK_2);

	if (((node->type == TREE_NODE_FOLDER) &&
	     (ma->mouse & BROWSER_MOUSE_DOUBLE_CLICK) && click) ||
	    (part == TV_NODE_PART_TOGGLE && click)) {
		int h = treeview__get_display_height(ma->tree) +
				ma->search_height;

		/* Clear any existing selection */
		redraw |= treeview_clear_selection(ma->tree, &r);

		/* Toggle node expansion */
		if (node->flags & TV_NFLAGS_EXPANDED) {
			err = treeview_node_contract_internal(ma->tree, node);
		} else {
			err = treeview_node_expand_internal(ma->tree, node);
		}
		if (err != NSERROR_OK) {
			return err;
		}

		/* Set up redraw */
		if (!redraw || r.y0 > ma->current_y)
			r.y0 = ma->current_y;
		if (h > treeview__get_display_height(ma->tree) +
				ma->search_height) {
			r.y1 = h;
		} else {
			r.y1 = treeview__get_display_height(ma->tree) +
				ma->search_height;
		}
		redraw = true;

	} else if ((node->type == TREE_NODE_ENTRY) &&
		   (ma->mouse & BROWSER_MOUSE_DOUBLE_CLICK) && click) {
		struct treeview_node_msg msg;
		msg.msg = TREE_MSG_NODE_LAUNCH;
		msg.data.node_launch.mouse = ma->mouse;

		/* Clear any existing selection */
		redraw |= treeview_clear_selection(ma->tree, &r);

		/* Tell client an entry was launched */
		ma->tree->callbacks->entry(msg, node->client_data);

	} else if (ma->mouse & BROWSER_MOUSE_PRESS_2 ||
		   (ma->mouse & BROWSER_MOUSE_PRESS_1 &&
		    ma->mouse & BROWSER_MOUSE_MOD_2)) {
		/* Toggle selection of node */
		action |= TV_NODE_ACTION_SELECTION;

	} else if (ma->mouse & BROWSER_MOUSE_CLICK_1 &&
		   ma->mouse &
		   (BROWSER_MOUSE_MOD_1 | BROWSER_MOUSE_MOD_3) &&
		   part != TV_NODE_PART_TOGGLE) {

		/* Clear any existing selection */
		redraw |= treeview_clear_selection(ma->tree, &r);

		/* Edit node */
		redraw |= treeview_edit_node_at_point(ma->tree, node,
						      ma->current_y, ma->x,
						      ma->y, &r);

	} else if (ma->mouse & BROWSER_MOUSE_PRESS_1 &&
		   !(ma->mouse &
		     (BROWSER_MOUSE_MOD_1 | BROWSER_MOUSE_MOD_3)) &&
		   !(node->flags & TV_NFLAGS_SELECTED) &&
		   part != TV_NODE_PART_TOGGLE) {
		/* Clear any existing selection */
		redraw |= treeview_clear_selection(ma->tree, &r);

		/* Select node */
		action |= TV_NODE_ACTION_SELECTION;

	}

	if (action & TV_NODE_ACTION_SELECTION) {
		/* Handle change in selection */
		node->flags ^= TV_NFLAGS_SELECTED;

		/* Redraw */
		if (!redraw) {
			r.y0 = ma->current_y;
			r.y1 = ma->current_y + height;
			redraw = true;
		} else {
			if (r.y0 > ma->current_y) {
				r.y0 = ma->current_y;
			}
			if (r.y1 < ma->current_y + height) {
				r.y1 = ma->current_y + height;
			}
		}
	}

	if (redraw) {
		treeview__cw_invalidate_area(ma->tree, &r);
	}

	*end = true; /* Reached line with click; stop walking tree */
	return NSERROR_OK;
}


/* Exported interface, documented in treeview.h */
void
treeview_mouse_action(treeview *tree, browser_mouse_state mouse, int x, int y)
{
	struct rect r;
	bool redraw = false;
	int search_height = treeview__get_search_height(tree);

	assert(tree != NULL);
	assert(tree->root != NULL);

	/* Not interested in whether mouse leaves window. */
	if (mouse == BROWSER_MOUSE_LEAVE) {
		return;
	}

	/* Handle mouse drag captured by textarea */
	if (tree->drag.type == TV_DRAG_TEXTAREA) {
		textarea_mouse_action(tree->edit.textarea, mouse,
				      x - tree->edit.x, y - tree->edit.y);
		return;
	} else if (tree->drag.type == TV_DRAG_SEARCH) {
		if (tree->search.active == false) {
			tree->search.active = true;
			if (treeview_clear_selection(tree, &r)) {
				treeview__cw_invalidate_area(tree, &r);
			}
		}
		textarea_mouse_action(tree->search.textarea, mouse,
				x - tree_g.window_padding - tree_g.icon_size,
				y);
		return;
	} else if (mouse & (BROWSER_MOUSE_PRESS_1 | BROWSER_MOUSE_PRESS_2) &&
		   y < search_height && tree->search.active == false) {
		tree->search.active = true;
		if (treeview_clear_selection(tree, &r)) {
			treeview__cw_invalidate_area(tree, &r);
		}
		textarea_mouse_action(tree->search.textarea, mouse,
				x - tree_g.window_padding - tree_g.icon_size,
				y);
		return;
	} else if (mouse & (BROWSER_MOUSE_PRESS_1 | BROWSER_MOUSE_PRESS_2) &&
			tree->search.active == true) {

		tree->search.active = false;
		textarea_set_caret(tree->search.textarea, -1);
		r.x0 = 0;
		r.y0 = 0;
		r.x1 = REDRAW_MAX;
		r.y1 = tree_g.line_height;
		treeview__cw_invalidate_area(tree, &r);
	}

	/* Handle textarea related mouse action */
	if (tree->edit.textarea != NULL) {
		int ta_x = x - tree->edit.x;
		int ta_y = y - tree->edit.y;

		if (ta_x > 0 && ta_x < tree->edit.w &&
		    ta_y > 0 && ta_y < tree->edit.h) {
			/* Inside textarea */
			textarea_mouse_action(tree->edit.textarea, mouse,
					      ta_x, ta_y);
			return;

		} else if (mouse != BROWSER_MOUSE_HOVER) {
			/* Action outside textarea */
			treeview_edit_cancel(tree, true);
		}
	}

	/* Handle drag ends */
	if (mouse == BROWSER_MOUSE_HOVER) {
		switch (tree->drag.type) {
		case TV_DRAG_SELECTION:
			treeview_commit_selection_drag(tree);
			tree->drag.type = TV_DRAG_NONE;
			tree->drag.start_node = NULL;

			treeview__cw_drag_status(tree, CORE_WINDOW_DRAG_NONE);
			return;
		case TV_DRAG_MOVE:
			treeview_move_selection(tree, &r);
			tree->drag.type = TV_DRAG_NONE;
			tree->drag.start_node = NULL;

			tree->move.target = NULL;
			tree->move.target_pos = TV_TARGET_NONE;

			treeview__cw_drag_status(tree, CORE_WINDOW_DRAG_NONE);
			treeview__cw_invalidate_area(tree, &r);
			return;
		default:
			/* No drag to end */
			break;
		}
	}

	if (y > treeview__get_display_height(tree) + search_height) {
		/* Below tree */

		r.x0 = 0;
		r.x1 = REDRAW_MAX;

		/* Record what position / part a drag started on */
		if (mouse & (BROWSER_MOUSE_PRESS_1 | BROWSER_MOUSE_PRESS_2) &&
		    tree->drag.type == TV_DRAG_NONE) {
			tree->drag.selected = false;
			tree->drag.start_node = NULL;
			tree->drag.part = TV_NODE_PART_NONE;
			tree->drag.start.x = x;
			tree->drag.start.y = y;
			tree->drag.start.node_y = tree->root->height;
			tree->drag.start.node_h = 0;

			tree->drag.prev.x = x;
			tree->drag.prev.y = y;
			tree->drag.prev.node_y = tree->root->height;
			tree->drag.prev.node_h = 0;
		}

		/* Handle drag start */
		if (tree->drag.type == TV_DRAG_NONE) {
			if (mouse & BROWSER_MOUSE_DRAG_1 &&
			    tree->drag.selected == false &&
			    tree->drag.part == TV_NODE_PART_NONE) {
				tree->drag.type = TV_DRAG_SELECTION;
				treeview__cw_drag_status(tree,
						CORE_WINDOW_DRAG_SELECTION);
			} else if (mouse & BROWSER_MOUSE_DRAG_2) {
				tree->drag.type = TV_DRAG_SELECTION;
				treeview__cw_drag_status(tree,
						CORE_WINDOW_DRAG_SELECTION);
			}

			if (tree->drag.start_node != NULL &&
			    tree->drag.type == TV_DRAG_SELECTION) {
				tree->drag.start_node->flags ^=
					TV_NFLAGS_SELECTED;
			}
		}

		/* Handle selection drags */
		if (tree->drag.type == TV_DRAG_SELECTION) {
			int curr_y1 = tree->root->height;
			int prev_y1 = tree->drag.prev.node_y +
				tree->drag.prev.node_h;

			r.y0 = tree->drag.prev.node_y;
			r.y1 = (curr_y1 > prev_y1) ? curr_y1 : prev_y1;

			redraw = true;

			tree->drag.prev.x = x;
			tree->drag.prev.y = y;
			tree->drag.prev.node_y = curr_y1;
			tree->drag.prev.node_h = 0;
		}

		if (mouse & BROWSER_MOUSE_PRESS_1) {
			/* Clear any existing selection */
			redraw |= treeview_clear_selection(tree, &r);
		}

		if (redraw) {
			treeview__cw_invalidate_area(tree, &r);
		}

	} else {
		/* On tree */
		struct treeview_mouse_action ma = {
			.tree = tree,
			.mouse = mouse,
			.x = x,
			.y = y,
			.current_y = search_height,
			.search_height = search_height,
		};

		treeview_walk_internal(tree, tree->root,
				TREEVIEW_WALK_MODE_DISPLAY, NULL,
				treeview_node_mouse_action_cb, &ma);
	}
}

/* Exported interface, documented in treeview.h */
int treeview_get_height(treeview *tree)
{
	int search_height = treeview__get_search_height(tree);
	int height = treeview__get_display_height(tree);

	assert(tree != NULL);
	assert(tree->root != NULL);

	treeview__cw_update_size(tree, -1, height);

	return height + search_height;
}

/* Exported interface, documented in treeview.h */
nserror treeview_set_search_string(
		treeview *tree,
		const char *string)
{
	if (!(tree->flags & TREEVIEW_SEARCHABLE)) {
		return NSERROR_BAD_PARAMETER;
	}

	if (string == NULL || strlen(string) == 0) {
		tree->search.active = false;
		tree->search.search = false;
		if (!textarea_set_text(tree->search.textarea, "")) {
			return NSERROR_UNKNOWN;
		}
		return treeview__search(tree, "", 0);
	}

	tree->search.active = true;
	tree->search.search = true;
	if (!textarea_set_text(tree->search.textarea, string)) {
		return NSERROR_UNKNOWN;
	}

	treeview__cw_full_redraw(tree);

	return NSERROR_OK;
}

/**
 * Initialise the plot styles from CSS system colour values.
 *
 * \param font_pt_size font size to use
 * \return NSERROR_OK on success else appropriate error code
 */
static nserror treeview_init_plot_styles(int font_pt_size)
{
	/* Background colour */
	plot_style_even.bg.stroke_type = PLOT_OP_TYPE_NONE;
	plot_style_even.bg.stroke_width = 0;
	plot_style_even.bg.stroke_colour = 0;
	plot_style_even.bg.fill_type = PLOT_OP_TYPE_SOLID;
	plot_style_even.bg.fill_colour = nscolours[NSCOLOUR_WIN_EVEN_BG];

	/* Text colour */
	plot_style_even.text.family = PLOT_FONT_FAMILY_SANS_SERIF;
	plot_style_even.text.size = font_pt_size;
	plot_style_even.text.weight = 400;
	plot_style_even.text.flags = FONTF_NONE;
	plot_style_even.text.foreground = nscolours[NSCOLOUR_WIN_EVEN_FG];
	plot_style_even.text.background = nscolours[NSCOLOUR_WIN_EVEN_BG];

	/* Entry field text colour */
	plot_style_even.itext = plot_style_even.text;
	plot_style_even.itext.foreground = nscolours[NSCOLOUR_WIN_EVEN_FG_FADED];

	/* Selected background colour */
	plot_style_even.sbg = plot_style_even.bg;
	plot_style_even.sbg.fill_colour = nscolours[NSCOLOUR_SEL_BG];

	/* Selected text colour */
	plot_style_even.stext = plot_style_even.text;
	plot_style_even.stext.foreground = nscolours[NSCOLOUR_SEL_FG];
	plot_style_even.stext.background = nscolours[NSCOLOUR_SEL_BG];

	/* Selected entry field text colour */
	plot_style_even.sitext = plot_style_even.stext;
	plot_style_even.sitext.foreground = nscolours[NSCOLOUR_SEL_FG_SUBTLE];

	/* Odd numbered node styles */
	plot_style_odd.bg = plot_style_even.bg;
	plot_style_odd.bg.fill_colour = nscolours[NSCOLOUR_WIN_ODD_BG];
	plot_style_odd.text = plot_style_even.text;
	plot_style_odd.text.background = plot_style_odd.bg.fill_colour;
	plot_style_odd.itext = plot_style_odd.text;
	plot_style_odd.itext.foreground = nscolours[NSCOLOUR_WIN_EVEN_FG_FADED];

	plot_style_odd.sbg = plot_style_even.sbg;
	plot_style_odd.stext = plot_style_even.stext;
	plot_style_odd.sitext = plot_style_even.sitext;

	return NSERROR_OK;
}


/**
 * Callback for hlcache retrieving resources.
 *
 * \param handle content hlcache handle
 * \param event The event that occurred on the content
 * \param pw treeview resource context
 */
static nserror
treeview_res_cb(struct hlcache_handle *handle,
		const hlcache_event *event,
		void *pw)
{
	struct treeview_resource *r = pw;

	switch (event->type) {
	case CONTENT_MSG_READY:
	case CONTENT_MSG_DONE:
		r->ready = true;
		r->height = content_get_height(handle);
		break;

	default:
		break;
	}

	return NSERROR_OK;
}


/**
 * Fetch content resources used by treeview.
 */
static void treeview_init_resources(void)
{
	int i;

	for (i = 0; i < TREE_RES_LAST; i++) {
		nsurl *url;
		treeview_res[i].ready = false;
		treeview_res[i].height = 0;
		if (nsurl_create(treeview_res[i].url, &url) == NSERROR_OK) {
			hlcache_handle_retrieve(url, 0, NULL, NULL,
						treeview_res_cb,
						&(treeview_res[i]), NULL,
						CONTENT_IMAGE,
						&(treeview_res[i].c));
			nsurl_unref(url);
		}
	}
}


/**
 * Create a right-pointing anti-aliased triangle bitmap
 *
 * \param bg background colour
 * \param fg foreground colour
 * \param size required bitmap size
 */
static struct bitmap *
treeview_generate_triangle_bitmap(colour bg, colour fg, int size)
{
	struct bitmap *b = NULL;
	int x, y;
	unsigned char *rpos;
	unsigned char *pos;
	size_t stride;

	/* Set up required colour graduations.  Ignores screen gamma. */
	colour colour0 = bg;
	colour colour1 = mix_colour(bg, fg, 255 * 3 / 4);
	colour colour2 = blend_colour(bg, fg);
	colour colour3 = mix_colour(bg, fg, 255 * 1 / 4);
	colour colour4 = fg;

	/* Create the bitmap */
	b = guit->bitmap->create(size, size, BITMAP_OPAQUE);
	if (b == NULL)
		return NULL;

	rpos = guit->bitmap->get_buffer(b);
	stride = guit->bitmap->get_rowstride(b);

	/* Draw the triangle */
	for (y = 0; y < size; y++) {
		pos = rpos;

		if (y < size / 2) {
			/* Top half */
			for (x = 0; x < y * 2; x++) {
				pos[bitmap_layout.r] = red_from_colour(colour4);
				pos[bitmap_layout.g] = green_from_colour(colour4);
				pos[bitmap_layout.b] = blue_from_colour(colour4);
				pos[bitmap_layout.a] = 0xff;
				pos += 4;
			}
			pos[bitmap_layout.r] = red_from_colour(colour3);
			pos[bitmap_layout.g] = green_from_colour(colour3);
			pos[bitmap_layout.b] = blue_from_colour(colour3);
			pos[bitmap_layout.a] = 0xff;
			pos += 4;
			pos[bitmap_layout.r] = red_from_colour(colour1);
			pos[bitmap_layout.g] = green_from_colour(colour1);
			pos[bitmap_layout.b] = blue_from_colour(colour1);
			pos[bitmap_layout.a] = 0xff;
			pos += 4;
			for (x = y * 2 + 2; x < size ; x++) {
				pos[bitmap_layout.r] = red_from_colour(colour0);
				pos[bitmap_layout.g] = green_from_colour(colour0);
				pos[bitmap_layout.b] = blue_from_colour(colour0);
				pos[bitmap_layout.a] = 0xff;
				pos += 4;
			}
		} else if ((y == size / 2) && (size & 0x1)) {
			/* Middle row */
			for (x = 0; x < size - 1; x++) {
				pos[bitmap_layout.r] = red_from_colour(colour4);
				pos[bitmap_layout.g] = green_from_colour(colour4);
				pos[bitmap_layout.b] = blue_from_colour(colour4);
				pos[bitmap_layout.a] = 0xff;
				pos += 4;
			}
			pos[bitmap_layout.r] = red_from_colour(colour2);
			pos[bitmap_layout.g] = green_from_colour(colour2);
			pos[bitmap_layout.b] = blue_from_colour(colour2);
			pos[bitmap_layout.a] = 0xff;
			pos += 4;
		} else {
			/* Bottom half */
			for (x = 0; x < (size - y - 1) * 2; x++) {
				pos[bitmap_layout.r] = red_from_colour(colour4);
				pos[bitmap_layout.g] = green_from_colour(colour4);
				pos[bitmap_layout.b] = blue_from_colour(colour4);
				pos[bitmap_layout.a] = 0xff;
				pos += 4;
			}
			pos[bitmap_layout.r] = red_from_colour(colour3);
			pos[bitmap_layout.g] = green_from_colour(colour3);
			pos[bitmap_layout.b] = blue_from_colour(colour3);
			pos[bitmap_layout.a] = 0xff;
			pos += 4;
			pos[bitmap_layout.r] = red_from_colour(colour1);
			pos[bitmap_layout.g] = green_from_colour(colour1);
			pos[bitmap_layout.b] = blue_from_colour(colour1);
			pos[bitmap_layout.a] = 0xff;
			pos += 4;
			for (x = (size - y) * 2; x < size ; x++) {
				pos[bitmap_layout.r] = red_from_colour(colour0);
				pos[bitmap_layout.g] = green_from_colour(colour0);
				pos[bitmap_layout.b] = blue_from_colour(colour0);
				pos[bitmap_layout.a] = 0xff;
				pos += 4;
			}
		}

		rpos += stride;
	}

	guit->bitmap->modified(b);

	return b;
}


/**
 * Create bitmap copy of another bitmap
 *
 * \param orig bitmap to copy
 * \param size required bitmap size
 */
static struct bitmap *
treeview_generate_copy_bitmap(struct bitmap *orig, int size)
{
	struct bitmap *b = NULL;
	unsigned char *data;
	unsigned char *orig_data;
	size_t stride;

	if (orig == NULL)
		return NULL;

	assert(size == guit->bitmap->get_width(orig));
	assert(size == guit->bitmap->get_height(orig));

	/* Create the bitmap */
	b = guit->bitmap->create(size, size, BITMAP_OPAQUE);
	if (b == NULL)
		return NULL;

	stride = guit->bitmap->get_rowstride(b);
	assert(stride == guit->bitmap->get_rowstride(orig));

	data = guit->bitmap->get_buffer(b);
	orig_data = guit->bitmap->get_buffer(orig);

	/* Copy the bitmap */
	memcpy(data, orig_data, stride * size);

	guit->bitmap->modified(b);

	/* We've not modified the original image, but we called
	 * bitmap_get_buffer(), so we need to pair that with a
	 * bitmap_modified() call to appease certain front ends. */
	guit->bitmap->modified(orig);

	return b;
}


/**
 * Create bitmap from rotation of another bitmap
 *
 * \param orig bitmap to create rotation of
 * \param size required bitmap size
 */
static struct bitmap *
treeview_generate_rotate_bitmap(struct bitmap *orig, int size)
{
	struct bitmap *b = NULL;
	int x, y;
	unsigned char *rpos;
	unsigned char *pos;
	unsigned char *orig_data;
	unsigned char *orig_pos;
	size_t stride;

	if (orig == NULL)
		return NULL;

	assert(size == guit->bitmap->get_width(orig));
	assert(size == guit->bitmap->get_height(orig));

	/* Create the bitmap */
	b = guit->bitmap->create(size, size, BITMAP_OPAQUE);
	if (b == NULL)
		return NULL;

	stride = guit->bitmap->get_rowstride(b);
	assert(stride == guit->bitmap->get_rowstride(orig));

	rpos = guit->bitmap->get_buffer(b);
	orig_data = guit->bitmap->get_buffer(orig);

	/* Copy the rotated bitmap */
	for (y = 0; y < size; y++) {
		pos = rpos;

		for (x = 0; x < size; x++) {
			orig_pos = orig_data + x * stride + y * 4;
			*(pos++) = *(orig_pos++);
			*(pos++) = *(orig_pos++);
			*(pos++) = *(orig_pos);
			*(pos++) = 0xff;

		}

		rpos += stride;
	}

	guit->bitmap->modified(b);

	/* We've not modified the original image, but we called
	 * bitmap_get_buffer(), so we need to pair that with a
	 * bitmap_modified() call to appease certain front ends.
	 */
	guit->bitmap->modified(orig);

	return b;
}


/**
 * Measures width of characters used to represent treeview furniture.
 *
 * \return NSERROR_OK on success else error code
 */
static nserror treeview_init_furniture(void)
{
	int size = tree_g.line_height / 2;

	plot_style_odd.furn[TREE_FURN_EXPAND].size = size;
	plot_style_odd.furn[TREE_FURN_EXPAND].bmp =
		treeview_generate_triangle_bitmap(
			plot_style_odd.bg.fill_colour,
			plot_style_odd.itext.foreground, size);
	plot_style_odd.furn[TREE_FURN_EXPAND].sel =
		treeview_generate_triangle_bitmap(
			plot_style_odd.sbg.fill_colour,
			plot_style_odd.sitext.foreground, size);

	plot_style_even.furn[TREE_FURN_EXPAND].size = size;
	plot_style_even.furn[TREE_FURN_EXPAND].bmp =
		treeview_generate_triangle_bitmap(
			plot_style_even.bg.fill_colour,
			plot_style_even.itext.foreground, size);
	plot_style_even.furn[TREE_FURN_EXPAND].sel =
		treeview_generate_copy_bitmap(
			plot_style_odd.furn[TREE_FURN_EXPAND].sel, size);

	plot_style_odd.furn[TREE_FURN_CONTRACT].size = size;
	plot_style_odd.furn[TREE_FURN_CONTRACT].bmp =
		treeview_generate_rotate_bitmap(
			plot_style_odd.furn[TREE_FURN_EXPAND].bmp, size);
	plot_style_odd.furn[TREE_FURN_CONTRACT].sel =
		treeview_generate_rotate_bitmap(
			plot_style_odd.furn[TREE_FURN_EXPAND].sel, size);

	plot_style_even.furn[TREE_FURN_CONTRACT].size = size;
	plot_style_even.furn[TREE_FURN_CONTRACT].bmp =
		treeview_generate_rotate_bitmap(
			plot_style_even.furn[TREE_FURN_EXPAND].bmp, size);
	plot_style_even.furn[TREE_FURN_CONTRACT].sel =
		treeview_generate_rotate_bitmap(
			plot_style_even.furn[TREE_FURN_EXPAND].sel, size);

	if (plot_style_odd.furn[TREE_FURN_EXPAND].bmp == NULL ||
	    plot_style_odd.furn[TREE_FURN_EXPAND].sel == NULL ||
	    plot_style_even.furn[TREE_FURN_EXPAND].bmp == NULL ||
	    plot_style_even.furn[TREE_FURN_EXPAND].sel == NULL ||
	    plot_style_odd.furn[TREE_FURN_CONTRACT].bmp == NULL ||
	    plot_style_odd.furn[TREE_FURN_CONTRACT].sel == NULL ||
	    plot_style_even.furn[TREE_FURN_CONTRACT].bmp == NULL ||
	    plot_style_even.furn[TREE_FURN_CONTRACT].sel == NULL)
		return NSERROR_NOMEM;

	tree_g.furniture_width = size + tree_g.line_height / 4;

	return NSERROR_OK;
}


/* Exported interface, documented in treeview.h */
nserror treeview_init(void)
{
	long long font_px_size;
	long long font_pt_size;
	nserror res;

	if (tree_g.initialised > 0) {
		tree_g.initialised++;
		return NSERROR_OK;
	}

	NSLOG(netsurf, INFO, "Initialising treeview module");

	font_pt_size = nsoption_int(treeview_font_size);
	if (font_pt_size <= 0) {
		font_pt_size = 11 * 10;
	}

	font_px_size = (font_pt_size * FIXTOINT(nscss_screen_dpi) /
			10 + 36) / 72;
	tree_g.line_height = (font_px_size * 8 + 3) / 6;

	res = treeview_init_plot_styles(font_pt_size * PLOT_STYLE_SCALE / 10);
	if (res != NSERROR_OK) {
		return res;
	}

	treeview_init_resources();

	res = treeview_init_furniture();
	if (res != NSERROR_OK) {
		return res;
	}

	tree_g.step_width = tree_g.furniture_width;
	tree_g.window_padding = 6;
	tree_g.icon_size = 17;
	tree_g.icon_step = 23;
	tree_g.move_offset = 18;

	tree_g.initialised++;

	NSLOG(netsurf, INFO, "Initialised treeview module");

	return NSERROR_OK;
}


/* Exported interface, documented in treeview.h */
nserror treeview_fini(void)
{
	int i;

	if (tree_g.initialised > 1) {
		tree_g.initialised--;
		return NSERROR_OK;

	} else if (tree_g.initialised == 0) {
		NSLOG(netsurf, INFO,
		      "Warning: tried to finalise uninitialised treeview module");
		return NSERROR_OK;
	}

	NSLOG(netsurf, INFO, "Finalising treeview module");

	for (i = 0; i < TREE_RES_LAST; i++) {
		hlcache_handle_release(treeview_res[i].c);
	}

	guit->bitmap->destroy(plot_style_odd.furn[TREE_FURN_EXPAND].bmp);
	guit->bitmap->destroy(plot_style_odd.furn[TREE_FURN_EXPAND].sel);
	guit->bitmap->destroy(plot_style_even.furn[TREE_FURN_EXPAND].bmp);
	guit->bitmap->destroy(plot_style_even.furn[TREE_FURN_EXPAND].sel);
	guit->bitmap->destroy(plot_style_odd.furn[TREE_FURN_CONTRACT].bmp);
	guit->bitmap->destroy(plot_style_odd.furn[TREE_FURN_CONTRACT].sel);
	guit->bitmap->destroy(plot_style_even.furn[TREE_FURN_CONTRACT].bmp);
	guit->bitmap->destroy(plot_style_even.furn[TREE_FURN_CONTRACT].sel);

	tree_g.initialised--;

	NSLOG(netsurf, INFO, "Finalised treeview module");

	return NSERROR_OK;
}
