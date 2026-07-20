/*
 * Copyright 2013 Vincent Sanders <vince@netsurf-browser.org>
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
 * Processing for html content object operations.
 */

#include <assert.h>
#include <ctype.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <nsutils/time.h>

#include "utils/corestrings.h"
#include "utils/config.h"
#include "utils/log.h"
#include "utils/nsoption.h"
#include "netsurf/content.h"
#include "netsurf/misc.h"
#include "content/hlcache.h"
#include "css/utils.h"
#include "desktop/scrollbar.h"
#include "desktop/gui_internal.h"

#include "html/html.h"
#include "html/private.h"
#include "html/interaction.h"
#include "html/box.h"
#include "html/box_inspect.h"
#include "html/object.h"
#include "macos9_deathrow.h"

extern int macsurf_ptr_is_heap(const void *);

/* break reference loop */
static void html_object_refresh(void *p);

/* fixes533: out-of-band live-content registry (anti-UAF).  html_fetch_object
 * is reached by a re-entrant box walk, and html_object_refresh is scheduled
 * with a content_html_object* (NOT the content), so the by-owner scheduler
 * cancel in content_destroy/html_destroy cannot match it.  Validate the
 * owning content by registry membership (never dereferences it) before
 * acting.  No-op (always live) on non-Mac syntax-check builds. */
#ifdef __MACOS9__
extern int macos9_content_is_live(struct content *c);
#else
#define macos9_content_is_live(c) (1)
#endif

/**
 * Retrieve objects used by HTML document
 *
 * \param h  Content to retrieve objects from
 * \param n  Pointer to location to receive number of objects
 * \return Pointer to list of objects
 */
struct content_html_object *html_get_objects(hlcache_handle *h, unsigned int *n)
{
	html_content *c = (html_content *) hlcache_handle_get_content(h);

	assert(c != NULL);
	assert(n != NULL);

	*n = c->num_objects;

	return c->object_list;
}

/**
 * Handle object fetching or loading failure.
 *
 * \param  box         box containing object which failed to load
 * \param  content     document of type CONTENT_HTML
 * \param  background  the object was the background image for the box
 */

static void
html_object_failed(struct box *box, html_content *content, bool background)
{
	/* fixes347 — formerly a no-op `return;`. That left every box
	 * with a failed-fetch <img> sitting at IS_REPLACED + box->object
	 * == NULL forever, so layout fell into the "replaced with no
	 * object" 1em × 1em fallback and any container relying on the
	 * image's natural size (e.g. mactrove's Welcome panel banner
	 * slider with `width:100%; height:100%; object-fit:contain`)
	 * collapsed to roughly zero height. The user then saw the
	 * surrounding chrome (gradient/window bg) where the banners
	 * should have rendered.
	 *
	 * For foreground (`background == false`) failures we clear
	 * IS_REPLACED so layout treats the box as a normal inline that
	 * can flow its alt text, and we walk parents invalidating
	 * max_width so the next layout pass picks up the new flag
	 * state. Background failures (`background == true`) just leave
	 * box->background NULL — the box still paints its colour /
	 * gradient / parent surface, no collapse risk, no recovery
	 * needed beyond NOT corrupting anything. */
	struct box *b;
	(void)content;

	if (background) {
		/* nothing more to do — element keeps its other backgrounds */
		return;
	}

	box->flags &= ~IS_REPLACED;

	/* mirror html_object_done's invalidation walk so the next
	 * reformat picks up the post-failure flag state cleanly. */
	for (b = box; b != NULL; b = b->parent)
		b->max_width = UNKNOWN_MAX_WIDTH;
}

/**
 * Update a box whose content has completed rendering.
 */

static void
html_object_done(struct box *box,
		 hlcache_handle *object,
		 bool background)
{
	struct box *b;

	if (background) {
		box->background = object;
		return;
	}

	box->object = object;

	/* Normalise the box type, now it has been replaced. */
	switch (box->type) {
	case BOX_TABLE:
		box->type = BOX_BLOCK;
		break;
	default:
		/* TODO: Any other box types need mapping? */
		break;
	}

	if (!(box->flags & REPLACE_DIM)) {
		/* invalidate parent min, max widths */
		for (b = box; b; b = b->parent)
			b->max_width = UNKNOWN_MAX_WIDTH;

		/* delete any clones of this box */
		while (box->next && (box->next->flags & CLONE)) {
			/* box_free_box(box->next); */
			box->next = box->next->next;
		}
	}
}


/**
 * Callback for hlcache_handle_retrieve() for objects with no box.
 */
static nserror
html_object_nobox_callback(hlcache_handle *object,
			   const hlcache_event *event,
			   void *pw)
{
	struct content_html_object *chobject = pw;

	/* fixes535: this hlcache callback carries a content_html_object whose
	 * owning content is chobject->parent.  The reliable cancellation is the
	 * object handle release in html_object_free_objects / abort (which
	 * deregisters this callback), but a bulk hlcache_clean double-destroy can
	 * dispatch it against a freed+reused owner first.  Validate the owner by
	 * registry membership (never dereferences it) before touching chobject. */
	if (macos9_content_is_live(chobject->parent) == 0) {
		return NSERROR_OK;
	}

	switch (event->type) {
	case CONTENT_MSG_ERROR:
		/* fixes515: NULL before release. */
		safe_hlcache_handle_release(&chobject->content);
		break;

	default:
		break;
	}

	return NSERROR_OK;
}


/**
 * fixes797 (#235): deferred, coalesced reflow used when a late object-link
 * reflow would otherwise be dropped by the reformat_time throttle.
 *
 * A macintoshgarden-style <img> with no dimensions links box->object via
 * CONTENT_MSG_READY and never sends CONTENT_MSG_DONE. The size-resolving
 * reflow is throttled to at most one per reformat_time window, which on a
 * G3 can be >1s; a trailing link within that window used to be dropped
 * with no retry, leaving the box at its pre-load line-height (squashed)
 * until the next navigation. Instead of dropping, the throttled branch now
 * schedules this callback for just after the window (cancel+reschedule so
 * only the last one survives -- a single coalesced reflow, no #208 storm).
 *
 * Fires from the scheduler, so the content may have been destroyed in
 * between; validate liveness (fixes535 registry) before touching it.
 */
static void html_object_deferred_reflow(void *pw)
{
	html_content *c = (html_content *) pw;

	if (macos9_content_is_live(&c->base) == 0) {
		return;
	}

	if (c->base.status == CONTENT_STATUS_READY ||
			c->base.status == CONTENT_STATUS_DONE) {
		content__reformat(&c->base, false,
				c->base.available_width,
				c->base.available_height);
	}
}


/**
 * Callback for hlcache_handle_retrieve() for objects with a box.
 */
static nserror
html_object_callback(hlcache_handle *object,
		     const hlcache_event *event,
		     void *pw)
{
	struct content_html_object *o = pw;
	html_content *c;
	int x, y;
	struct box *box;

	/* fixes535: this hlcache callback carries a content_html_object whose
	 * owning content is o->parent.  The reliable cancellation is the object
	 * handle release in html_object_free_objects / html_object_abort_objects
	 * (which deregisters this callback), but a bulk hlcache_clean double-destroy
	 * can dispatch it against a freed+reused owner first; the reused struct's
	 * bytes read as plausible garbage so c->base.* below walks freed memory.
	 * macos9_content_is_live never dereferences o->parent, so validate it
	 * before reading anything through it. */
	if (macos9_content_is_live(o->parent) == 0) {
		return NSERROR_OK;
	}

	c = (html_content *) o->parent;

	box = o->box;

	switch (event->type) {
	case CONTENT_MSG_LOADING:
		if (c->base.status != CONTENT_STATUS_LOADING && c->bw != NULL)
			content_open(object,
					c->bw, &c->base,
					box->object_params);
		break;

	case CONTENT_MSG_READY:
		if (content_can_reformat(object)) {
			/* TODO: avoid knowledge of box internals here */
			content_reformat(object, false,
					box->max_width != UNKNOWN_MAX_WIDTH ?
							box->width : 0,
					box->max_width != UNKNOWN_MAX_WIDTH ?
							box->height : 0);

			/* Adjust parent content for new object size */
			html_object_done(box, object, o->background);
			if ((c->base.status == CONTENT_STATUS_READY ||
					c->base.status == CONTENT_STATUS_DONE) &&
					c->base.active == 0) {
				/* fixes689 (#208): the per-image reflow storm. This
				 * CONTENT_MSG_READY branch used to fire a full
				 * content__reformat for EVERY object whose size resolves,
				 * gated only on parent status — and synchronously, outside
				 * the 50ms scheduler dedup that coalesces every other
				 * reformat trigger. On an avatar-heavy forum page that was
				 * ~15 full layout_document walks per load. Any reformat
				 * fired while active>0 is immediately superseded by the next
				 * object and finally by the guaranteed active==0 reformat at
				 * the end of this callback, so gate on active==0. Progressive
				 * display during load is already handled by the throttled
				 * incremental_reflow branch below (>=250ms). Collapses the
				 * storm to ~2-3 reflows. */
				macsurf_debug_log_writef(
					"reformat: trig=obj-size active=%d",
					(int)c->base.active);
				content__reformat(&c->base, false,
						c->base.available_width,
						c->base.available_height);
			}
		}
		break;

	case CONTENT_MSG_DONE:
		c->base.active--;
		NSLOG(netsurf, INFO, "%d fetches active", c->base.active);

		html_object_done(box, object, o->background);

		if (c->base.status != CONTENT_STATUS_LOADING &&
				box->flags & REPLACE_DIM) {
			union content_msg_data data;

			if (c->had_initial_layout == false) {
				break;
			}

			if (!box_visible(box))
				break;

			box_coords(box, &x, &y);

			data.redraw.x = x + box->padding[LEFT];
			data.redraw.y = y + box->padding[TOP];
			data.redraw.width = box->width;
			data.redraw.height = box->height;

			content_broadcast(&c->base, CONTENT_MSG_REDRAW, &data);
		}
		break;

	case CONTENT_MSG_ERROR:
		/* fixes428: null box->object before release. hlcache_handle_release
		 * frees the handle immediately. Without this null, box->object holds
		 * freed memory and get_mouse_action_node crashes dereferencing it. */
		if (!o->background && box != NULL && box->object == object) {
			box->object = NULL;
		}

		/* fixes515: NULL before release. */
		safe_hlcache_handle_release(&o->content);

		c->base.active--;
		NSLOG(netsurf, INFO, "%d fetches active", c->base.active);

		html_object_failed(box, c, o->background);

		break;

	case CONTENT_MSG_REDRAW:
		if (c->base.status != CONTENT_STATUS_LOADING) {
			union content_msg_data data = event->data;

			if (c->had_initial_layout == false) {
				break;
			}

			if (!box_visible(box))
				break;

			box_coords(box, &x, &y);

			if (object == box->background) {
				/* Redraw request is for background */
				css_fixed hpos = 0, vpos = 0;
				css_unit hunit = CSS_UNIT_PX;
				css_unit vunit = CSS_UNIT_PX;
				int width = box->padding[LEFT] + box->width +
						box->padding[RIGHT];
				int height = box->padding[TOP] + box->height +
						box->padding[BOTTOM];
				int t, h, l, w;

				/* Need to know background-position */
				css_computed_background_position(box->style,
						&hpos, &hunit, &vpos, &vunit);

				w = content_get_width(box->background);
				if (hunit == CSS_UNIT_PCT) {
					l = (width - w) * hpos / INTTOFIX(100);
				} else {
					l = FIXTOINT(css_unit_len2device_px(
							box->style,
							&c->unit_len_ctx,
							hpos, hunit));
				}

				h = content_get_height(box->background);
				if (vunit == CSS_UNIT_PCT) {
					t = (height - h) * vpos / INTTOFIX(100);
				} else {
					t = FIXTOINT(css_unit_len2device_px(
							box->style,
							&c->unit_len_ctx,
							vpos, vunit));
				}

				/* Redraw area depends on background-repeat */
				switch (css_computed_background_repeat(
						box->style)) {
				case CSS_BACKGROUND_REPEAT_REPEAT:
					data.redraw.x = 0;
					data.redraw.y = 0;
					data.redraw.width = box->width;
					data.redraw.height = box->height;
					break;

				case CSS_BACKGROUND_REPEAT_REPEAT_X:
					data.redraw.x = 0;
					data.redraw.y += t;
					data.redraw.width = box->width;
					break;

				case CSS_BACKGROUND_REPEAT_REPEAT_Y:
					data.redraw.x += l;
					data.redraw.y = 0;
					data.redraw.height = box->height;
					break;

				case CSS_BACKGROUND_REPEAT_NO_REPEAT:
					data.redraw.x += l;
					data.redraw.y += t;
					break;

				default:
					break;
				}

				/* Add offset to box */
				data.redraw.x += x;
				data.redraw.y += y;

			} else {
				/* Non-background case */
				int w = content_get_width(object);
				int h = content_get_height(object);

				if (w != 0 && box->width != w) {
					/* Not showing image at intrinsic
					 * width; need to scale the redraw
					 * request area. */
					data.redraw.x = data.redraw.x *
							box->width / w;
					data.redraw.width =
							data.redraw.width *
							box->width / w;
				}

				if (h != 0 && box->height != w) {
					/* Not showing image at intrinsic
					 * height; need to scale the redraw
					 * request area. */
					data.redraw.y = data.redraw.y *
							box->height / h;
					data.redraw.height =
							data.redraw.height *
							box->height / h;
				}

				data.redraw.x += x + box->padding[LEFT];
				data.redraw.y += y + box->padding[TOP];
			}

			content_broadcast(&c->base, CONTENT_MSG_REDRAW, &data);
		}
		break;

	case CONTENT_MSG_REFRESH:
		if (content_get_type(object) == CONTENT_HTML) {
			/* only for HTML objects */
			guit->misc->schedule(event->data.delay * 1000,
					html_object_refresh, o);
		}

		break;

	case CONTENT_MSG_LINK:
		/* Don't care about favicons that aren't on top level content */
		break;

	case CONTENT_MSG_GETTHREAD:
		/* Objects don't have JS threads */
		*(event->data.jsthread) = NULL;
		break;

	case CONTENT_MSG_GETDIMS:
		*(event->data.getdims.viewport_width) =
				content__get_width(&c->base);
		*(event->data.getdims.viewport_height) =
				content__get_height(&c->base);
		break;

	case CONTENT_MSG_SCROLL:
		if (box->scroll_x != NULL)
			scrollbar_set(box->scroll_x, event->data.scroll.x0,
					false);
		if (box->scroll_y != NULL)
			scrollbar_set(box->scroll_y, event->data.scroll.y0,
					false);
		break;

	case CONTENT_MSG_DRAGSAVE:
	{
		union content_msg_data msg_data;
		if (event->data.dragsave.content == NULL)
			msg_data.dragsave.content = object;
		else
			msg_data.dragsave.content =
					event->data.dragsave.content;

		content_broadcast(&c->base, CONTENT_MSG_DRAGSAVE, &msg_data);
	}
		break;

	case CONTENT_MSG_SAVELINK:
	case CONTENT_MSG_POINTER:
	case CONTENT_MSG_SELECTMENU:
	case CONTENT_MSG_GADGETCLICK:
		/* These messages are for browser window layer.
		 * we're not interested, so pass them on. */
		content_broadcast(&c->base, event->type, &event->data);
		break;

	case CONTENT_MSG_CARET:
	{
		union html_focus_owner focus_owner;
		focus_owner.content = box;

		switch (event->data.caret.type) {
		case CONTENT_CARET_REMOVE:
		case CONTENT_CARET_HIDE:
			html_set_focus(c, HTML_FOCUS_CONTENT, focus_owner,
					true, 0, 0, 0, NULL);
			break;
		case CONTENT_CARET_SET_POS:
			html_set_focus(c, HTML_FOCUS_CONTENT, focus_owner,
					false, event->data.caret.pos.x,
					event->data.caret.pos.y,
					event->data.caret.pos.height,
					event->data.caret.pos.clip);
			break;
		}
	}
		break;

	case CONTENT_MSG_DRAG:
	{
		html_drag_type drag_type = HTML_DRAG_NONE;
		union html_drag_owner drag_owner;
		drag_owner.content = box;

		switch (event->data.drag.type) {
		case CONTENT_DRAG_NONE:
			drag_type = HTML_DRAG_NONE;
			drag_owner.no_owner = true;
			break;
		case CONTENT_DRAG_SCROLL:
			drag_type = HTML_DRAG_CONTENT_SCROLL;
			break;
		case CONTENT_DRAG_SELECTION:
			drag_type = HTML_DRAG_CONTENT_SELECTION;
			break;
		}
		html_set_drag_type(c, drag_type, drag_owner,
				event->data.drag.rect);
	}
		break;

	case CONTENT_MSG_SELECTION:
	{
		html_selection_type sel_type;
		union html_selection_owner sel_owner;

		if (event->data.selection.selection) {
			sel_type = HTML_SELECTION_CONTENT;
			sel_owner.content = box;
		} else {
			sel_type = HTML_SELECTION_NONE;
			sel_owner.none = true;
		}
		html_set_selection(c, sel_type, sel_owner,
				event->data.selection.read_only);
	}
		break;

	default:
		break;
	}

	if (c->base.active == 0 &&
	    (event->type == CONTENT_MSG_LOADING ||
	     event->type == CONTENT_MSG_DONE ||
	     event->type == CONTENT_MSG_ERROR) &&
	    (c->base.status == CONTENT_STATUS_READY ||
	     c->base.status == CONTENT_STATUS_DONE)) {
		/* All objects have arrived. #235: also fire this guaranteed
		 * reflow when the parent is already DONE -- a viewport-lazy
		 * image (#223) scrolled into view completes AFTER the page
		 * flipped to DONE, so the READY-only guard used to skip it and
		 * the throttled incremental branch could drop it with no retry,
		 * leaving the image box collapsed (vertically squished) until
		 * the next navigation. Its intrinsic dimensions are known at
		 * convert, so one reflow here sizes the box. active==0 keeps
		 * this coalesced (fires once per completed batch -- no storm). */
		content__reformat(&c->base, false, c->base.available_width,
				c->base.available_height);
		if (c->base.status == CONTENT_STATUS_READY)
			content_set_done(&c->base);
	} else if (nsoption_bool(incremental_reflow) &&
		   (event->type == CONTENT_MSG_DONE ||
		    event->type == CONTENT_MSG_READY) &&
		   box != NULL &&
		   !(box->flags & REPLACE_DIM) &&
		   (c->base.status == CONTENT_STATUS_READY ||
		    c->base.status == CONTENT_STATUS_DONE)) {
		/* #235: a macintoshgarden-style <img> with no width/height
		 * attrs and no CSS dimensions links box->object via
		 * CONTENT_MSG_READY (content_can_reformat is true for it) and
		 * NEVER sends CONTENT_MSG_DONE, so the DONE-only guaranteed and
		 * incremental reflows above both skip it. Without a reflow after
		 * the object links, layout keeps the box at its pre-load
		 * line-height (~17/19px) and the image renders vertically
		 * squashed. Allowing READY here re-lays-out once the object is
		 * linked -- layout then sees b->object != NULL, takes the
		 * replaced branch, and sizes the box from the object's intrinsic
		 * dimensions. The 250ms reformat_time throttle below keeps this
		 * coalesced (no #208 reflow storm). */
		/* 1) the configuration option to reflow pages while
		 *      objects are fetched is set
		 * 2) an object is newly fetched & converted,
		 * 3) the box's dimensions need to change due to being replaced
		 * 4) the object's parent HTML is ready for reformat,
		 */
		uint64_t ms_now;
		nsu_getmonotonic_ms(&ms_now);
		if (ms_now > c->base.reformat_time) {
			/* The time since the previous reformat is
			 *  more than the configured minimum time
			 *  between reformats so reformat the page to
			 *  display newly fetched objects
			 */
			content__reformat(&c->base,
					  false,
					  c->base.available_width,
					  c->base.available_height);
		} else {
			/* fixes797 (#235): still inside the throttle window.
			 * The DONE-only guaranteed reflow will never fire for
			 * these READY-only images, so dropping here would leave
			 * a late-linked <img> squashed until the next nav. Defer
			 * one coalesced reflow to just past the window instead
			 * (cancel any pending, reschedule) so the trailing link
			 * always gets sized -- and only once, no #208 storm.
			 *
			 * fixes916 — this was `else if (READY)`, which left a
			 * hole: the enclosing condition admits DONE *and* READY,
			 * so a DONE landing inside the window matched NEITHER
			 * branch and its reflow was dropped silently, leaving the
			 * image at unsized dimensions.
			 *
			 * That hole is what squashes macintoshgarden.org on a
			 * CACHED revisit. Hardware timeline (2026-07-19):
			 *   cold  visit: reconvert seq=1 runs BEFORE NAV DONE, so
			 *                the page's own completion reflow follows
			 *                and sizes everything -> images correct.
			 *   cached visit: page completes FIRST, then reconvert
			 *                seq=2 rebuilds the tree, frees every
			 *                image object (H2) and re-fetches. From
			 *                cache those images complete instantly as
			 *                DONE, not READY, while the reconvert's
			 *                own full layout_document has just set a
			 *                long throttle window -> dropped -> squashed.
			 * The cache only decides the TIMING; the hole is the bug.
			 *
			 * Deferring for DONE as well costs nothing extra: the
			 * schedule is cancel-then-reschedule, so a burst of image
			 * completions still collapses to ONE reflow. */
			int delay = (int)(c->base.reformat_time - ms_now) + 50;
			guit->misc->schedule(-1, html_object_deferred_reflow, c);
			guit->misc->schedule(delay,
					html_object_deferred_reflow, c);
		}
	}

	return NSERROR_OK;
}


/**
 * Start a fetch for an object required by a page, replacing an existing object.
 *
 * \param  object          Object to replace
 * \param  url             URL of object to fetch (copied)
 * \return  true on success, false on memory exhaustion
 */
static bool html_replace_object(struct content_html_object *object, nsurl *url)
{
	html_content *c;
	hlcache_child_context child;
	html_content *page;
	nserror error;

	assert(object != NULL);
	assert(object->box != NULL);

	c = (html_content *) object->parent;

	child.charset = c->encoding;
	child.quirks = c->base.quirks;

	if (object->content != NULL) {
		/* remove existing object */
		if (content_get_status(object->content) != CONTENT_STATUS_DONE) {
			c->base.active--;
			NSLOG(netsurf, INFO, "%d fetches active",
			      c->base.active);
		}

		safe_hlcache_handle_release(&object->content); /* fixes515 */

		object->box->object = NULL;
	}

	/* initialise fetch */
	error = hlcache_handle_retrieve(url, HLCACHE_RETRIEVE_SNIFF_TYPE,
			content_get_url(&c->base), NULL,
			html_object_callback, object, &child,
			object->permitted_types,
			&object->content);

	if (error != NSERROR_OK)
		return false;

	for (page = c; page != NULL; page = page->page) {
		page->base.active++;
		NSLOG(netsurf, INFO, "%d fetches active", c->base.active);

		page->base.status = CONTENT_STATUS_READY;
	}

	return true;
}

/**
 * schedule callback for object refresh
 */
static void html_object_refresh(void *p)
{
	struct content_html_object *object = p;
	nsurl *refresh_url;

	/* fixes533: this callback is keyed on the content_html_object, not its
	 * owning content, so content_destroy's by-owner scheduler cancel does not
	 * reach it.  Validate the OWNER (object->parent) by registry membership
	 * before touching object->content; if the document was torn down the
	 * owner is no longer registered and we bail without driving a refresh
	 * through freed state. */
	if (object == NULL)
		return;
	if (macos9_content_is_live(object->parent) == 0) {
		macsurf_debug_log_writef(
			"html_object_refresh: owner NOT LIVE (registry) object=%p parent=%p",
			(void *)object, (void *)object->parent);
		return;
	}

	assert(content_get_type(object->content) == CONTENT_HTML);

	refresh_url = content_get_refresh_url(object->content);

	/* Ignore if refresh URL has gone
	 * (may happen if fetch errored) */
	if (refresh_url == NULL)
		return;

	content_invalidate_reuse_data(object->content);

	if (!html_replace_object(object, refresh_url)) {
		/** \todo handle memory exhaustion */
	}
}


/* exported interface documented in html/object.h */
nserror html_object_open_objects(html_content *html, struct browser_window *bw)
{
	struct content_html_object *object, *next;

	for (object = html->object_list; object != NULL; object = next) {
		next = object->next;

		if (object->content == NULL || object->box == NULL)
			continue;

		if (content_get_type(object->content) == CONTENT_NONE)
			continue;

		content_open(object->content,
			     bw,
			     &html->base,
			     object->box->object_params);
	}
	return NSERROR_OK;
}


/* exported interface documented in html/object.h */
nserror html_object_abort_objects(html_content *htmlc)
{
	struct content_html_object *object;

	for (object = htmlc->object_list;
	     object != NULL;
	     object = object->next) {
		if (object->content == NULL)
			continue;

		switch (content_get_status(object->content)) {
		case CONTENT_STATUS_DONE:
			/* already loaded: do nothing */
			break;

		case CONTENT_STATUS_READY:
			hlcache_handle_abort(object->content);
			/* Active count will be updated when
			 * html_object_callback receives
			 * CONTENT_MSG_DONE from this object
			 */
			break;

		default:
			hlcache_handle_abort(object->content);
			safe_hlcache_handle_release(&object->content); /* fixes515 */
			if (object->box != NULL) {
				htmlc->base.active--;
				NSLOG(netsurf, INFO, "%d fetches active",
				      htmlc->base.active);
			}
			break;

		}
	}

	return NSERROR_OK;
}


/* exported interface documented in html/object.h */
nserror html_object_close_objects(html_content *html)
{
	struct content_html_object *object, *next;

	for (object = html->object_list; object != NULL; object = next) {
		next = object->next;

		if (object->content == NULL || object->box == NULL)
			continue;

		if (content_get_type(object->content) == CONTENT_NONE)
			continue;

		if (content_get_type(object->content) == CONTENT_HTML) {
			guit->misc->schedule(-1, html_object_refresh, object);
		}

		content_close(object->content);
	}
	return NSERROR_OK;
}


/* exported interface documented in html/object.h */
nserror html_object_free_objects(html_content *html)
{
	while (html->object_list != NULL) {
		struct content_html_object *victim = html->object_list;

		if (victim->content != NULL) {
			NSLOG(netsurf, INFO, "object %p", victim->content);

			if (content_get_type(victim->content) == CONTENT_HTML) {
				guit->misc->schedule(-1, html_object_refresh, victim);
			}
			/* fixes429: null box->object before releasing the handle.
			 * hlcache_handle_release frees the handle immediately;
			 * the freed block's first bytes become a free-list
			 * chain pointer, making handle->entry appear non-NULL.
			 * get_mouse_action_node then passes the null-guard but
			 * crashes dereferencing the chain pointer as an entry.
			 * Same root cause as fixes428 (CONTENT_MSG_ERROR path). */
			if (!victim->background &&
			    victim->box != NULL &&
			    victim->box->object == victim->content) {
				victim->box->object = NULL;
			}
			/* fixes501x: NULL before release. */
			safe_hlcache_handle_release(&victim->content);
			}

		/* fixes917 (step 1) -- drop the node ref. Outside the
		 * `victim->content != NULL` arm above: an entry can carry a node
		 * with no content (retrieve failed), and it still owns the ref. */
		if (victim->node != NULL) {
			dom_node_unref(victim->node);
			victim->node = NULL;
		}

		html->object_list = victim->next;
		free(victim);
	}
	return NSERROR_OK;
}


/* exported interface documented in html/object.h */
bool
html_fetch_object(html_content *c,
		  nsurl *url,
		  struct box *box,
		  content_type permitted_types,
		  bool background)
{
	/* fixes918 -- unchanged 5-arg entry point, kept so the exported signature
	 * never moves. Callers with no originating element land here. */
	return html_fetch_object_node(c, url, box, permitted_types,
			background, NULL);
}

/* exported interface documented in html/object.h */
bool
html_fetch_object_node(html_content *c,
		  nsurl *url,
		  struct box *box,
		  content_type permitted_types,
		  bool background,
		  struct dom_node *n)
{
	struct content_html_object *object;
	hlcache_handle_callback object_callback;
	hlcache_child_context child;
	nserror error;

	/* fixes533: registry-membership liveness check FIRST, before any field of
	 * `c` is read.  The box walk that reaches here runs inside a
	 * convert_xml_to_box callback the scheduler already DEQUEUED before
	 * invoking, so a bulk hlcache_clean -> content_destroy -> html_destroy that
	 * fires while this long walk is yielding (hlcache_handle_retrieve drives
	 * OT) frees and reuses this html_content WITHOUT being able to stop the
	 * in-flight walk.  The reused struct's bytes read as plausible garbage
	 * (aborted=0, encoding=1), so the c->aborted / CONTENT_IS_DEAD gates below
	 * pass and we crash dereferencing the dead content (the html_fetch_object
	 * charset-deref / r4=1 signature).  macos9_content_is_live walks the
	 * registry and never dereferences `c`, so it is the only signal that
	 * survives a freed+reused struct.  Not live => teardown underway: treat as
	 * fetch-handled and unwind without touching it. */
	if (macos9_content_is_live((struct content *)c) == 0) {
		macsurf_debug_log_writef(
			"html_fetch_object: DEAD content c=%p, drop fetch", (void *)c);
		return true;
	}

	/* If we've already been aborted, don't bother attempting the fetch */
	if (c->aborted)
		return true;

	/* fixes519: the box walk can reach here holding a freed+reused
	 * html_content whose byte fields read as plausible-but-wild values
	 * (aborted=0 above, encoding=1 here).  hlcache_handle_retrieve strdup()s
	 * child.charset, so a wild encoding pointer makes strdup scan unmapped
	 * memory and crash (observed: lbzu through r4=1 in hlcache_handle_retrieve).
	 * Treat a dead content, or an encoding pointer outside the malloc heap,
	 * as "no fetch" / "unknown charset" rather than dereferencing garbage. */
	if (CONTENT_IS_DEAD(&c->base))
		return true;
	child.charset = c->encoding;
	if (child.charset != NULL) {
		if (!macsurf_ptr_is_heap((const void *)(child.charset))) {
			macsurf_debug_log_writef(
				"html_fetch_object: WILD encoding=%p c=%p, drop charset",
				(void *)child.charset, (void *)c);
			child.charset = NULL;
		}
	}
	child.quirks = c->base.quirks;

	object = calloc(1, sizeof(struct content_html_object));
	if (object == NULL) {
		return false;
	}

	if (box == NULL) {
		object_callback = html_object_nobox_callback;
	} else {
		object_callback = html_object_callback;
	}

	object->parent = (struct content *) c;
	object->next = NULL;
	object->content = NULL;
	object->box = box;
	object->permitted_types = permitted_types;
	object->background = background;
	/* fixes917 (step 1) -- record the originating node, refcounted.
	 * box->node is NOT usable here: convert_special_elements (which runs
	 * box_image and friends) is called at box_construct.c:1345, well before
	 * box->node is assigned at :1438, so it is still NULL at fetch time.
	 * Hence the explicit parameter. Released in html_object_free_objects. */
	object->node = (n != NULL) ? dom_node_ref(n) : NULL;

	error = hlcache_handle_retrieve(url,
					HLCACHE_RETRIEVE_SNIFF_TYPE,
					content_get_url(&c->base),
					NULL,
					object_callback,
					object,
					&child,
					object->permitted_types,
					&object->content);
	if (error != NSERROR_OK) {
		free(object);
		return error != NSERROR_NOMEM;
	}

	/* add to content object list */
	object->next = c->object_list;
	c->object_list = object;

	c->num_objects++;
	if (box != NULL) {
		c->base.active++;
		NSLOG(netsurf, INFO, "%d fetches active", c->base.active);
	}

	return true;
}


/*
 * Stage 1 death-row pinned-check: does a pending html_object_refresh
 * continuation still reference `target`?  Checks BOTH the raw parent
 * content (object->parent) and the object's own content handle, since the
 * refresh re-drives into the parent and touches the image content.
 */
bool
html_object_sched_pins(void (*cb)(void *), void *p, struct content *target)
{
	struct content_html_object *o;

	if (cb != html_object_refresh || p == NULL) {
		return false;
	}
	o = (struct content_html_object *) p;
	if (o->parent == target) {
		return true;
	}
	if (o->content != NULL &&
	    hlcache_handle_get_content(o->content) == target) {
		return true;
	}
	return false;
}
