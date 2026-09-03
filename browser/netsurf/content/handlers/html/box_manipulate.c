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
/* fixes889 - corestring_dom___ns_key_box_node_data (the box<->node backlink)
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


/* fixes889 - how many boxes retracted their own node backlink on the way out.
 * Reported per reconvert / teardown. A NON-ZERO count that the old
 * children/next-only walk in html_reconvert_clear_node_boxes could not have
 * reached (floats, list markers) is the dangling box_for_node() pointer behind
 * both hardware crashes. Read by html.c. */
unsigned long macsurf_box_backlink_cleared = 0;

/* The recently-freed-box ring was diagnostic scaffolding for the resolved
 * reconvert double-free hunt. Keeping it active costs global writes for every
 * box destructor and permanently reserves the static ring. Retain the exported
 * hooks because talloc's fatal diagnostic references them, but make normal
 * teardown allocation- and write-free. */
void macsurf_box_freed_note(void *box, const char *ctx)
{
	(void)box;
	(void)ctx;
}

const char *macsurf_box_freed_lookup(void *box)
{
	(void)box;
	return "(boxfree-diag-disabled)";
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

	/* fixes1076 - free an ORPHAN form control here, not only in
	 * box_free_box.
	 *
	 * box_free_box frees box->gadget, but a RECONVERT does not go through
	 * it: html_reconvert_free_old() bulk-frees the whole old tree with
	 * talloc_free(old_bctx), which runs this destructor per chunk and
	 * nothing else. So every gadget on the old tree was simply dropped.
	 *
	 * It only leaks for controls with no owning <form>, and that is not a
	 * coincidence -- it is the same asymmetry from both ends.
	 * html_forms_get_control_for_node reuses an existing control only if it
	 * can find it by walking c->forms; parse_* calls form_add_control ONLY
	 * when the element has a <form>, and invent_fake_gadget links its
	 * control nowhere. So a form-owned control is found and REUSED across a
	 * reconvert (no garbage), while an orphan is unreachable, rebuilt every
	 * time, and never freed by anything on any path.
	 *
	 * That shape is exactly the modern-page case: a JS-driven comment box or
	 * editor whose controls are not inside a <form>. fixes1073's forced
	 * synchronous layout multiplies reconverts, so this stops being
	 * theoretical.
	 *
	 * Safe to call unconditionally on an orphan: form_free_control unlinks
	 * from control->form first (form.c), and an orphan has none. Guarded on
	 * CLONE for the same reason box_free_box is -- a cloned box shares the
	 * pointer with its original and must not free it. Form-OWNED controls
	 * are deliberately untouched here: they are reused across reconverts and
	 * freed with their form at content destroy. */
	if (b->gadget != NULL && b->gadget->form == NULL &&
	    !(b->flags & CLONE)) {
		form_free_control(b->gadget);
		b->gadget = NULL;
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
		/* fixes889 - THE CHOKE POINT. Clear this node's back-pointer to
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
		}
		dom_node_unref(b->node);
	}

	if (b->scroll_x != NULL) {
		data = scrollbar_get_data(b->scroll_x);
		scrollbar_destroy(b->scroll_x);
		free(data);
		b->scroll_x = NULL;   /* fixes499f - idempotent: a second
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
	struct box *box;

	/* talloc_zero has already cleared every ordinary zero/NULL field.
	 * Only initialise non-zero sentinels/defaults and caller-owned refs here. */
	box = talloc_zero(context, struct box);
	if (!box) {
		return 0;
	}

	talloc_set_destructor(box, box_talloc_destructor);

	box->type = BOX_INLINE;
	if (style_owned)
		box->flags = STYLE_OWNED;
	box->styles = styles;
	box->style = style;
	box->width = UNKNOWN_WIDTH;
	box->max_width = UNKNOWN_MAX_WIDTH;
	box->href = (href == NULL) ? NULL : nsurl_ref(href);
	box->target = target;
	box->title = title;
	box->columns = 1;
	box->rows = 1;
	box->list_value = 1;
	box->id = id;

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
		/* fixes1076 - NULL it. box_free_box ends in talloc_free(box),
		 * which runs box_talloc_destructor, which now also frees an
		 * orphan gadget; without clearing the pointer here that path
		 * would free the same control twice. */
		if (box->gadget) {
			form_free_control(box->gadget);
			box->gadget = NULL;
		}
		if (box->scroll_x != NULL)
			scrollbar_destroy(box->scroll_x);
		if (box->scroll_y != NULL)
			scrollbar_destroy(box->scroll_y);
		if (box->styles != NULL)
			css_select_results_destroy(box->styles);
		/* fixes1268c (#167) - release this box's reference to its
		 * custom-property environment. Guarded by !CLONE alongside
		 * styles: a CLONE box shares both with its original and must
		 * not drop a reference it never took. */
		if (box->custom_env != NULL) {
			css_custom_env_unref(box->custom_env);
			box->custom_env = NULL;
		}
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
	/* fixes992 - WHO puts a scrollbar in the middle of the page?
	 *
	 * The reported "split viewport with an extra scroll bar" is NOT the
	 * fixes625 canvas blowup: the fixes990 gate (canvas over twice the
	 * viewport) did not fire on the page that shows it, with the
	 * diagnostic confirmed present in the binary. So the scrollbar belongs
	 * to a BOX -- an overflow container -- and this is where those are
	 * made. Anomaly-scoped by construction: only a box that actually gets a
	 * horizontal scrollbar logs, and the numbers say whether the overflow
	 * is real (full_width really exceeds visible_width, so the content is
	 * genuinely too wide) or whether it was forced by overflow:scroll with
	 * nothing actually overflowing.
	 *
	 * Capped per session so a page full of legitimate overflow containers
	 * cannot flood the log; the handful on the reported page arrive first.
	 * Note MacSurf has no min-content/max-content solver, so a container
	 * whose intrinsic width is over-estimated is a live candidate for a
	 * scrollbar that no other browser draws. */
	if (bottom) {
		static int hbar_logged = 0;
		if (hbar_logged < 12) {
			hbar_logged++;
			macsurf_debug_log_writef(
				"LIFE HBAR type=%d x=%d w=%d vis_w=%d full_w=%d"
				" dx0=%d dx1=%d new=%d",
				(int)box->type, (int)box->x, (int)box->width,
				visible_width, full_width,
				(int)box->descendant_x0,
				(int)box->descendant_x1,
				(box->scroll_x == NULL) ? 1 : 0);
		}
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


