/*
 * Copyright 2005-2007 James Bursa <bursa@users.sourceforge.net>
 * Copyright 2003 Phil Mellor <monkeyson@users.sourceforge.net>
 * Copyright 2005 John M Bell <jmb202@ecs.soton.ac.uk>
 * Copyright 2008 Michael Drake <tlsa@netsurf-browser.org>
 * Copyright 2020 Vincent Sanders <vince@netsurf-browser.org>
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
 * implementation of box tree manipulation.
 */


#include "utils/ns_errors.h"
#include "utils/talloc.h"
#include "utils/nsurl.h"
/* fixes889 — corestring_dom___ns_key_box_node_data (the box<->node backlink)
 * and the WORK diagnostic channel. */
#include "utils/corestrings.h"
#include "macsurf_debug_log.h"
#include "netsurf/types.h"
#include "netsurf/mouse.h"
#include "desktop/scrollbar.h"

#include "html/private.h"
#include "html/form_internal.h"
#include "html/interaction.h"
#include "html/box.h"
#include "html/box_manipulate.h"


/* fixes889 — how many boxes retracted their own node backlink on the way out.
 * Reported per reconvert / teardown. A NON-ZERO count that the old
 * children/next-only walk in html_reconvert_clear_node_boxes could not have
 * reached (floats, list markers) is the dangling box_for_node() pointer behind
 * both hardware crashes. Read by html.c. */
unsigned long macsurf_box_backlink_cleared = 0;

/* fixes908 -- recently-freed-box ring for the reconvert double-free hunt.
 * box_talloc_destructor is the single choke point every box passes through on
 * its FIRST free (via box_free_box, or via talloc_free(tree) recursion -- talloc
 * runs the destructor per chunk). Recording (box, during-label) here lets the
 * talloc double-free abort in talloc.c report WHERE the box was first freed --
 * the missing half of the picture (the second-free path is the live during=
 * label). 4096 slots so a whole heavy-page tree-free does not evict the target
 * before the double-free lands. Diagnostic only. */
#define MACSURF_BOXFREE_RING 4096
static void *g_boxfree_ptr[MACSURF_BOXFREE_RING];
static const char *g_boxfree_ctx[MACSURF_BOXFREE_RING];
static int g_boxfree_ix = 0;

void macsurf_box_freed_note(void *box, const char *ctx)
{
	g_boxfree_ptr[g_boxfree_ix] = box;
	g_boxfree_ctx[g_boxfree_ix] = (ctx != NULL) ? ctx : "(null)";
	g_boxfree_ix++;
	if (g_boxfree_ix >= MACSURF_BOXFREE_RING) {
		g_boxfree_ix = 0;
	}
}

const char *macsurf_box_freed_lookup(void *box)
{
	int i;
	for (i = 0; i < MACSURF_BOXFREE_RING; i++) {
		if (g_boxfree_ptr[i] == box) {
			return g_boxfree_ctx[i];
		}
	}
	return "(not-in-ring)";
}

/**
 * Destructor for box nodes which own styles
 *
 * \param b The box being destroyed.
 * \return 0 to allow talloc to continue destroying the tree.
 */
static int box_talloc_destructor(struct box *b)
{
	struct html_scrollbar_data *data;

	/* fixes908 -- note this box's first free + the free path in flight. */
	{
		extern const char *macsurf_talloc_free_ctx;
		macsurf_box_freed_note(b, macsurf_talloc_free_ctx);
	}

	if ((b->flags & STYLE_OWNED) && b->style != NULL) {
		css_computed_style_destroy(b->style);
		b->style = NULL;
	}

	if (b->styles != NULL) {
		css_select_results_destroy(b->styles);
		b->styles = NULL;
	}

	if (b->href != NULL)
		nsurl_unref(b->href);

	lwc_string_unref(b->id);

	if (b->node != NULL) {
		/* fixes889 — THE CHOKE POINT. Clear this node's back-pointer to
		 * THIS box before dropping our ref.
		 *
		 * box_for_node() reads a raw `struct box *` stored as user data on
		 * the DOM node (corestring_dom___ns_key_box_node_data, set in
		 * box_construct). Nothing else clears it except
		 * html_reconvert_clear_node_boxes(), which walks children/next ONLY --
		 * so any box reachable only via `float_children`/`next_float` or
		 * `list_marker` (a marker is linked ONLY through box->list_marker;
		 * box_construct sets marker->parent but never puts it in children)
		 * keeps its node pointing at the box after the tree is freed. Then:
		 *   - a click -> get_mouse_action_node -> link_box_for_ancestor ->
		 *     box_for_node() hands back FREED memory and it is dereferenced
		 *     (HW: illegal instruction, PC in zeroed System heap);
		 *   - teardown walks the same freed box again (HW: allocator blowup
		 *     inside _dom_element_finalise).
		 *
		 * Doing it HERE instead of extending that walk is deliberate: every
		 * box passes through this destructor exactly once, however it was
		 * linked, so no future link field can reintroduce the hole.
		 *
		 * The `cur == b` identity test is load-bearing, not defensive noise.
		 * On a reconvert the OLD tree is deliberately kept alive THROUGH
		 * dom_to_box (the fixes421 double-buffer) and freed afterwards, by
		 * which time the node's pointer legitimately belongs to the NEW box.
		 * An unconditional clear here would null the LIVE tree's back-pointer
		 * and break box_for_node for the page that just rendered. Only ever
		 * retract our own backlink. */
		struct box *cur = NULL;
		if (dom_node_get_user_data(b->node,
				corestring_dom___ns_key_box_node_data,
				&cur) == DOM_NO_ERR && cur == b) {
			void *old_ud = NULL;
			(void) dom_node_set_user_data(b->node,
					corestring_dom___ns_key_box_node_data,
					NULL, NULL, &old_ud);
			macsurf_box_backlink_cleared++;
		}
		dom_node_unref(b->node);
	}

	if (b->scroll_x != NULL) {
		data = scrollbar_get_data(b->scroll_x);
		scrollbar_destroy(b->scroll_x);
		free(data);
		b->scroll_x = NULL;   /* fixes499f — idempotent: a second
		                       * teardown pass must skip the freed bar
		                       * rather than re-destroy a stale pointer. */
	}

	if (b->scroll_y != NULL) {
		data = scrollbar_get_data(b->scroll_y);
		scrollbar_destroy(b->scroll_y);
		free(data);
		b->scroll_y = NULL;
	}

	return 0;
}


/* Exported function documented in html/box.h */
struct box *
box_create(css_select_results *styles,
	   css_computed_style *style,
	   bool style_owned,
	   nsurl *href,
	   const char *target,
	   const char *title,
	   lwc_string *id,
	   void *context)
{
	unsigned int i;
	struct box *box;

	box = talloc_zero(context, struct box);
	if (!box) {
		return 0;
	}

	talloc_set_destructor(box, box_talloc_destructor);

	box->type = BOX_INLINE;
	box->flags = 0;
	box->flags = style_owned ? (box->flags | STYLE_OWNED) : box->flags;
	box->styles = styles;
	box->style = style;
	box->x = box->y = 0;
	box->width = UNKNOWN_WIDTH;
	box->height = 0;
	box->descendant_x0 = box->descendant_y0 = 0;
	box->descendant_x1 = box->descendant_y1 = 0;
	for (i = 0; i != 4; i++)
		box->margin[i] = box->padding[i] = box->border[i].width = 0;
	box->scroll_x = box->scroll_y = NULL;
	box->min_width = 0;
	box->max_width = UNKNOWN_MAX_WIDTH;
	box->byte_offset = 0;
	box->text = NULL;
	box->length = 0;
	box->space = 0;
	box->href = (href == NULL) ? NULL : nsurl_ref(href);
	box->target = target;
	box->title = title;
	box->columns = 1;
	box->rows = 1;
	box->start_column = 0;
	box->next = NULL;
	box->prev = NULL;
	box->children = NULL;
	box->last = NULL;
	box->parent = NULL;
	box->inline_end = NULL;
	box->float_children = NULL;
	box->float_container = NULL;
	box->next_float = NULL;
	box->cached_place_below_level = 0;
	box->list_value = 1;
	box->list_marker = NULL;
	box->col = NULL;
	box->gadget = NULL;
	box->usemap = NULL;
	box->id = id;
	box->background = NULL;
	box->object = NULL;
	box->object_params = NULL;
	box->iframe = NULL;
	box->node = NULL;

	return box;
}


/* Exported function documented in html/box.h */
void box_add_child(struct box *parent, struct box *child)
{
	assert(parent);
	assert(child);

	if (parent->children != 0) {	/* has children already */
		parent->last->next = child;
		child->prev = parent->last;
	} else {			/* this is the first child */
		parent->children = child;
		child->prev = 0;
	}

	parent->last = child;
	child->parent = parent;
}


/* Exported function documented in html/box.h */
void box_insert_sibling(struct box *box, struct box *new_box)
{
	new_box->parent = box->parent;
	new_box->prev = box;
	new_box->next = box->next;
	box->next = new_box;
	if (new_box->next)
		new_box->next->prev = new_box;
	else if (new_box->parent)
		new_box->parent->last = new_box;
}


/* Exported function documented in html/box.h */
void box_unlink_and_free(struct box *box)
{
	struct box *parent = box->parent;
	struct box *next = box->next;
	struct box *prev = box->prev;

	if (parent) {
		if (parent->children == box)
			parent->children = next;
		if (parent->last == box)
			parent->last = next ? next : prev;
	}

	if (prev)
		prev->next = next;
	if (next)
		next->prev = prev;

	box_free(box);
}


/* Exported function documented in html/box.h */
void box_free(struct box *box)
{
	struct box *child, *next;
	/* fixes908 -- label the second-free path (box_free covers box_normalise
	 * table fixups + box_unlink_and_free). Save/restore for the recursion. */
	extern const char *macsurf_talloc_free_ctx;
	const char *prev_ctx = macsurf_talloc_free_ctx;
	/* fixes909 -- only claim the generic "box_free" label when no more
	 * specific site label (e.g. box_normalise's) is already in flight, so a
	 * caller's site name survives through this recursion to the abort. */
	macsurf_talloc_free_ctx =
		(prev_ctx == NULL || prev_ctx[0] == '(') ? "box_free" : prev_ctx;

	/* free children first */
	for (child = box->children; child; child = next) {
		next = child->next;
		box_free(child);
	}

	/* last this box */
	box_free_box(box);

	macsurf_talloc_free_ctx = prev_ctx;
}


/* Exported function documented in html/box.h */
void box_free_box(struct box *box)
{
	if (!(box->flags & CLONE)) {
		if (box->gadget)
			form_free_control(box->gadget);
		if (box->scroll_x != NULL)
			scrollbar_destroy(box->scroll_x);
		if (box->scroll_y != NULL)
			scrollbar_destroy(box->scroll_y);
		if (box->styles != NULL)
			css_select_results_destroy(box->styles);
	}

	talloc_free(box);
}


/* exported interface documented in html/box.h */
nserror
box_handle_scrollbars(struct content *c,
		      struct box *box,
		      bool bottom,
		      bool right)
{
	struct html_scrollbar_data *data;
	int visible_width, visible_height;
	int full_width, full_height;
	nserror res;

	if (!bottom && box->scroll_x != NULL) {
		data = scrollbar_get_data(box->scroll_x);
		scrollbar_destroy(box->scroll_x);
		free(data);
		box->scroll_x = NULL;
	}

	if (!right && box->scroll_y != NULL) {
		data = scrollbar_get_data(box->scroll_y);
		scrollbar_destroy(box->scroll_y);
		free(data);
		box->scroll_y = NULL;
	}

	if (!bottom && !right) {
		return NSERROR_OK;
	}

	visible_width = box->width + box->padding[RIGHT] + box->padding[LEFT];
	visible_height = box->height + box->padding[TOP] + box->padding[BOTTOM];

	full_width = ((box->descendant_x1 - box->border[RIGHT].width) >
			visible_width) ?
			box->descendant_x1 + box->padding[RIGHT] :
			visible_width;
	full_height = ((box->descendant_y1 - box->border[BOTTOM].width) >
			visible_height) ?
			box->descendant_y1 + box->padding[BOTTOM] :
			visible_height;

	if (right) {
		if (box->scroll_y == NULL) {
			data = malloc(sizeof(struct html_scrollbar_data));
			if (data == NULL) {
				return NSERROR_NOMEM;
			}
			data->c = c;
			data->box = box;
			res = scrollbar_create(false,
					       visible_height,
					       full_height,
					       visible_height,
					       data,
					       html_overflow_scroll_callback,
					       &(box->scroll_y));
			if (res != NSERROR_OK) {
				return res;
			}
		} else  {
			scrollbar_set_extents(box->scroll_y,
					      visible_height,
					      visible_height,
					      full_height);
		}
	}
	if (bottom) {
		if (box->scroll_x == NULL) {
			data = malloc(sizeof(struct html_scrollbar_data));
			if (data == NULL) {
				return NSERROR_OK;
			}
			data->c = c;
			data->box = box;
			res = scrollbar_create(true,
					       visible_width - (right ? SCROLLBAR_WIDTH : 0),
					       full_width,
					       visible_width,
					       data,
					       html_overflow_scroll_callback,
					       &box->scroll_x);
			if (res != NSERROR_OK) {
				return res;
			}
		} else {
			scrollbar_set_extents(box->scroll_x,
					visible_width -
					(right ? SCROLLBAR_WIDTH : 0),
					visible_width, full_width);
		}
	}

	if (right && bottom) {
		scrollbar_make_pair(box->scroll_x, box->scroll_y);
	}

	return NSERROR_OK;
}


