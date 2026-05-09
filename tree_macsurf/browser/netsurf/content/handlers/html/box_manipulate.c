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


#include "utils/errors.h"
#include "utils/talloc.h"
#include "utils/nsurl.h"
#include "netsurf/types.h"
#include "netsurf/mouse.h"
#include "desktop/scrollbar.h"

#include "html/private.h"
#include "html/form_internal.h"
#include "html/interaction.h"
#include "html/box.h"
#include "html/box_manipulate.h"


/**
 * Destructor for box nodes which own styles
 *
 * \param b The box being destroyed.
 * \return 0 to allow talloc to continue destroying the tree.
 */
static int box_talloc_destructor(struct box *b)
{
	struct html_scrollbar_data *data;

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
		dom_node_unref(b->node);
	}

	if (b->scroll_x != NULL) {
		data = scrollbar_get_data(b->scroll_x);
		scrollbar_destroy(b->scroll_x);
		free(data);
	}

	if (b->scroll_y != NULL) {
		data = scrollbar_get_data(b->scroll_y);
		scrollbar_destroy(b->scroll_y);
		free(data);
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

	/* free children first */
	for (child = box->children; child; child = next) {
		next = child->next;
		box_free(child);
	}

	/* last this box */
	box_free_box(box);
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


