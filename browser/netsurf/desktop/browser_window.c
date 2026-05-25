/*
 * Copyright 2008 Michael Drake <tlsa@netsurf-browser.org>
 * Copyright 2010 Daniel Silverstone <dsilvers@digital-scurf.org>
 * Copyright 2010-2020 Vincent Sanders <vince@netsurf-browser.org>
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
 * Browser window creation and manipulation implementation.
 */

#include "utils/config.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <nsutils/time.h>

#include "utils/errors.h"
#include "utils/log.h"
#include "utils/corestrings.h"
#include "utils/messages.h"
#include "utils/nsoption.h"
#include "netsurf/types.h"
#include "netsurf/browser_window.h"
#include "netsurf/window.h"
#include "netsurf/misc.h"
#include "netsurf/content.h"
#include "netsurf/search.h"
#include "netsurf/plotters.h"
#include "content/content.h"
#include "content/hlcache.h"
#include "content/urldb.h"
#include "content/content_debug.h"

#include "html/html.h"
#include "html/form_internal.h"
#include "javascript/js.h"

#include "desktop/browser_private.h"

/* fixes161c — Apple post-READY crash probes. MS_LOG comes from the
 * macos9 frontend's debug header; included here so scheduled_reformat
 * and browser_window_schedule_reformat can mark their entry points. */
#include "macsurf_debug.h"
#include "desktop/scrollbar.h"
#include "desktop/gui_internal.h"
#include "desktop/download.h"
#include "desktop/frames.h"
#include "desktop/global_history.h"
#include "desktop/textinput.h"
#include "desktop/hotlist.h"
#include "desktop/knockout.h"
#include "desktop/browser_history.h"
#include "desktop/theme.h"

#ifdef WITH_THEME_INSTALL
#include "desktop/theme.h"
#endif

/**
 * smallest scale that can be applied to a browser window
 */
#define SCALE_MINIMUM 0.2

/**
 * largests scale that can be applied to a browser window
 */
#define SCALE_MAXIMUM 10.0

/**
 * maximum frame depth
 */
#define FRAME_DEPTH 8

/* Forward declare internal navigation function */
static nserror browser_window__navigate_internal(
	struct browser_window *bw, struct browser_fetch_parameters *params);


/**
 * Close and destroy all child browser window.
 *
 * \param bw browser window
 */
static void browser_window_destroy_children(struct browser_window *bw)
{
	int i;

	if (bw->children) {
		for (i = 0; i < (bw->rows * bw->cols); i++) {
			browser_window_destroy_internal(&bw->children[i]);
		}
		free(bw->children);
		bw->children = NULL;
		bw->rows = 0;
		bw->cols = 0;
	}
}


/**
 * Free the stored fetch parameters
 *
 * \param bw The browser window
 */
static void
browser_window__free_fetch_parameters(struct browser_fetch_parameters *params)
{
	if (params->url != NULL) {
		nsurl_unref(params->url);
		params->url = NULL;
	}
	if (params->referrer != NULL) {
		nsurl_unref(params->referrer);
		params->referrer = NULL;
	}
	if (params->post_urlenc != NULL) {
		free(params->post_urlenc);
		params->post_urlenc = NULL;
	}
	if (params->post_multipart != NULL) {
		fetch_multipart_data_destroy(params->post_multipart);
		params->post_multipart = NULL;
	}
	if (params->parent_charset != NULL) {
		free(params->parent_charset);
		params->parent_charset = NULL;
	}
}


/**
 * Get position of scrollbar widget within browser window.
 *
 * \param bw The browser window
 * \param horizontal Whether to get position of horizontal scrollbar
 * \param x Updated to x-coord of top left of scrollbar widget
 * \param y Updated to y-coord of top left of scrollbar widget
 */
static inline void
browser_window_get_scrollbar_pos(struct browser_window *bw,
				 bool horizontal,
				 int *x, int *y)
{
	if (horizontal) {
		*x = 0;
		*y = bw->height - SCROLLBAR_WIDTH;
	} else {
		*x = bw->width - SCROLLBAR_WIDTH;
		*y = 0;
	}
}


/**
 * Get browser window horizontal scrollbar widget length
 *
 * \param bw The browser window
 * \return the scrollbar's length
 */
static inline int get_horz_scrollbar_len(struct browser_window *bw)
{
	if (bw->scroll_y == NULL) {
		return bw->width;
	}
	return bw->width - SCROLLBAR_WIDTH;
}


/**
 * Get browser window vertical scrollbar widget length
 *
 * \param bw The browser window
 * \return the scrollbar's length
 */
static inline int get_vert_scrollbar_len(struct browser_window *bw)
{
	return bw->height;
}


/**
 * Set or remove a selection.
 *
 * \param bw browser window with selection
 * \param selection true if bw has a selection, false if removing selection
 * \param read_only true iff selection is read only (e.g. can't cut it)
 */
static void
browser_window_set_selection(struct browser_window *bw,
			     bool selection,
			     bool read_only)
{
	struct browser_window *top;

	assert(bw != NULL);

	top = browser_window_get_root(bw);

	assert(top != NULL);

	if (bw != top->selection.bw &&
	    top->selection.bw != NULL &&
	    top->selection.bw->current_content != NULL) {
		/* clear old selection */
		content_clear_selection(top->selection.bw->current_content);
	}

	if (selection) {
		top->selection.bw = bw;
	} else {
		top->selection.bw = NULL;
	}

	top->selection.read_only = read_only;
}


/**
 * Set the scroll position of a browser window.
 *
 * scrolls the viewport to ensure the specified rectangle of the
 *   content is shown.
 *
 * \param bw window to scroll
 * \param rect The rectangle to ensure is shown.
 * \return NSERROR_OK on success or apropriate error code.
 */
static nserror
browser_window_set_scroll(struct browser_window *bw, const struct rect *rect)
{
	if (bw->window != NULL) {
		return guit->window->set_scroll(bw->window, rect);
	}

	if (bw->scroll_x != NULL) {
		scrollbar_set(bw->scroll_x, rect->x0, false);
	}
	if (bw->scroll_y != NULL) {
		scrollbar_set(bw->scroll_y, rect->y0, false);
	}

	return NSERROR_OK;
}


/**
 * Internal helper for getting the positional features
 *
 * \param[in] bw browser window to examine.
 * \param[in] x x-coordinate of point of interest
 * \param[in] y y-coordinate of point of interest
 * \param[out] data Feature structure to update.
 * \return NSERROR_OK or appropriate error code on faliure.
 */
static nserror
browser_window__get_contextual_content(struct browser_window *bw,
				       int x, int y,
				       struct browser_window_features *data)
{
	nserror ret = NSERROR_OK;

	/* Handle (i)frame scroll offset (core-managed browser windows only) */
	x += scrollbar_get_offset(bw->scroll_x);
	y += scrollbar_get_offset(bw->scroll_y);

	if (bw->children) {
		/* Browser window has children, so pass request on to
		 * appropriate child.
		 */
		struct browser_window *bwc;
		int cur_child;
		int children = bw->rows * bw->cols;

		/* Loop through all children of bw */
		for (cur_child = 0; cur_child < children; cur_child++) {
			/* Set current child */
			bwc = &bw->children[cur_child];

			/* Skip this frame if (x, y) coord lies outside */
			if ((x < bwc->x) ||
			    (bwc->x + bwc->width < x) ||
			    (y < bwc->y) ||
			    (bwc->y + bwc->height < y)) {
				continue;
			}

			/* Pass request into this child */
			return browser_window__get_contextual_content(bwc,
					      (x - bwc->x), (y - bwc->y), data);
		}

		/* Coordinate not contained by any frame */

	} else if (bw->current_content != NULL) {
		/* Pass request to content */
		ret = content_get_contextual_content(bw->current_content,
						     x, y, data);
		data->main = bw->current_content;
	}

	return ret;
}


/**
 * implements the download operation of a window navigate
 */
static nserror
browser_window_download(struct browser_window *bw,
			nsurl *url,
			nsurl *nsref,
			uint32_t fetch_flags,
			bool fetch_is_post,
			llcache_post_data *post)
{
	llcache_handle *l;
	struct browser_window *root;
	nserror error;

	root = browser_window_get_root(bw);
	assert(root != NULL);

	fetch_flags |= LLCACHE_RETRIEVE_FORCE_FETCH;
	fetch_flags |= LLCACHE_RETRIEVE_STREAM_DATA;

	error = llcache_handle_retrieve(url, fetch_flags, nsref,
					fetch_is_post ? post : NULL,
					NULL, NULL, &l);
	if (error == NSERROR_NO_FETCH_HANDLER) {
		/* no internal handler for this type, call out to frontend */
		error = guit->misc->launch_url(url);
	} else if (error != NSERROR_OK) {
		nslog_log(__FILE__, "", __LINE__, "Failed to fetch download: %d", error);
	} else {
		error = download_context_create(l, root->window);
		if (error != NSERROR_OK) {
			nslog_log(__FILE__, "", __LINE__, "Failed creating download context: %d", error);
			llcache_handle_abort(l);
			llcache_handle_release(l);
		}
	}

	return error;
}


/**
 * recursively check browser windows for activity
 *
 * \param bw browser window to start checking from.
 */
static bool browser_window_check_throbber(struct browser_window *bw)
{
	int children, index;

	if (bw->throbbing)
		return true;

	if (bw->children) {
		children = bw->rows * bw->cols;
		for (index = 0; index < children; index++) {
			if (browser_window_check_throbber(&bw->children[index]))
				return true;
		}
	}

	if (bw->iframes) {
		for (index = 0; index < bw->iframe_count; index++) {
			if (browser_window_check_throbber(&bw->iframes[index]))
				return true;
		}
	}

	return false;
}


/**
 * Start the busy indicator.
 *
 * \param bw browser window
 */
static nserror browser_window_start_throbber(struct browser_window *bw)
{
	bw->throbbing = true;

	while (bw->parent)
		bw = bw->parent;

	return guit->window->event(bw->window, GW_EVENT_START_THROBBER);
}


/**
 * Stop the busy indicator.
 *
 * \param bw browser window
 */
static nserror browser_window_stop_throbber(struct browser_window *bw)
{
	nserror res = NSERROR_OK;

	bw->throbbing = false;

	while (bw->parent) {
		bw = bw->parent;
	}

	if (!browser_window_check_throbber(bw)) {
		res = guit->window->event(bw->window, GW_EVENT_STOP_THROBBER);
	}
	return res;
}


/**
 * Callback for fetchcache() for browser window favicon fetches.
 *
 * \param c content handle of favicon
 * \param event The event to process
 * \param pw a context containing the browser window
 * \return NSERROR_OK on success else appropriate error code.
 */
static nserror
browser_window_favicon_callback(hlcache_handle *c,
				const hlcache_event *event,
				void *pw)
{
	struct browser_window *bw = pw;

	switch (event->type) {
	case CONTENT_MSG_DONE:
		if (bw->favicon.current != NULL) {
			content_close(bw->favicon.current);
			hlcache_handle_release(bw->favicon.current);
		}

		bw->favicon.current = c;
		bw->favicon.loading = NULL;

		/* content_get_bitmap on the hlcache_handle should give
		 *   the favicon bitmap at this point
		 */
		guit->window->set_icon(bw->window, c);
		break;

	case CONTENT_MSG_ERROR:

		/* clean up after ourselves */
		if (c == bw->favicon.loading) {
			bw->favicon.loading = NULL;
		} else if (c == bw->favicon.current) {
			bw->favicon.current = NULL;
		}

		hlcache_handle_release(c);

		if (bw->favicon.failed == false) {
			nsurl *nsref = NULL;
			nsurl *nsurl;
			nserror error;

			bw->favicon.failed = true;

			error = nsurl_create("resource:favicon.ico", &nsurl);
			if (error != NSERROR_OK) {
				NSLOG(netsurf, INFO,
				      "Unable to create default location url");
			} else {
				hlcache_handle_retrieve(nsurl,
							HLCACHE_RETRIEVE_SNIFF_TYPE,
							nsref, NULL,
							browser_window_favicon_callback,
							bw, NULL, CONTENT_IMAGE,
							&bw->favicon.loading);

				nsurl_unref(nsurl);
			}

		}
		break;

	default:
		break;

	}
	return NSERROR_OK;
}


/**
 * update the favicon associated with the browser window
 *
 * \param c the page content handle.
 * \param bw A top level browser window.
 * \param link A link context or NULL to attempt fallback scanning.
 */
static nserror
browser_window_update_favicon(hlcache_handle *c,
			      struct browser_window *bw,
			      struct content_rfc5988_link *link)
{
	nsurl *nsref = NULL;
	nsurl *nsurl;
	nserror res;

	assert(c != NULL);
	assert(bw !=NULL);

	if (bw->window == NULL) {
		/* Not top-level browser window; not interested */
		return NSERROR_OK;
	}

	/* already fetching the favicon - use that */
	if (bw->favicon.loading != NULL) {
		return NSERROR_OK;
	}

	bw->favicon.failed = false;

	if (link == NULL) {
		/* Look for "icon" */
		link = content_find_rfc5988_link(c, corestring_lwc_icon);
	}

	if (link == NULL) {
		/* Look for "shortcut icon" */
		link = content_find_rfc5988_link(c, corestring_lwc_shortcut_icon);
	}

	if (link == NULL) {
		lwc_string *scheme;
		bool speculative_default = false;
		unsigned char match;

		nsurl = hlcache_handle_get_url(c);

		scheme = nsurl_get_component(nsurl, NSURL_SCHEME);

		/* If the document was fetched over http(s), then speculate
		 * that there's a favicon living at /favicon.ico */
		if ((lwc_string_caseless_isequal(scheme,
						 corestring_lwc_http,
						 &match) == lwc_error_ok &&
		     match) ||
		    (lwc_string_caseless_isequal(scheme,
						 corestring_lwc_https,
						 &match) == lwc_error_ok &&
		     match)) {
			speculative_default = true;
		}

		lwc_string_unref(scheme);

		if (speculative_default) {
			/* no favicon via link, try for the default location */
			res = nsurl_join(nsurl, "/favicon.ico", &nsurl);
		} else {
			bw->favicon.failed = true;
			res = nsurl_create("resource:favicon.ico", &nsurl);
		}
		if (res != NSERROR_OK) {
			NSLOG(netsurf, INFO,
			      "Unable to create default location url");
			return res;
		}
	} else {
		nsurl = nsurl_ref(link->href);
	}

	if (link == NULL) {
		nslog_log(__FILE__, "", __LINE__,
			      "fetching general favicon from '%s'",
			      nsurl_access(nsurl));
	} else {
		nslog_log(__FILE__, "", __LINE__,
			      "fetching favicon rel:%s '%s'",
			      lwc_string_data(link->rel),
			      nsurl_access(nsurl));
	}

	res = hlcache_handle_retrieve(nsurl,
				      HLCACHE_RETRIEVE_SNIFF_TYPE,
				      nsref,
				      NULL,
				      browser_window_favicon_callback,
				      bw,
				      NULL,
				      CONTENT_IMAGE,
				      &bw->favicon.loading);

	nsurl_unref(nsurl);

	return res;
}


/**
 * Handle meta http-equiv refresh time elapsing by loading a new page.
 *
 * \param p browser window to refresh with new page
 */
static void browser_window_refresh(void *p)
{
	struct browser_window *bw = p;
	nsurl *url;
	nsurl *refresh;
	hlcache_handle *parent = NULL;
	enum browser_window_nav_flags flags = BW_NAVIGATE_UNVERIFIABLE;

	assert(bw->current_content != NULL &&
	       (content_get_status(bw->current_content) ==
		CONTENT_STATUS_READY ||
		content_get_status(bw->current_content) ==
		CONTENT_STATUS_DONE));

	/* Ignore if the refresh URL has gone
	 * (may happen if a fetch error occurred) */
	refresh = content_get_refresh_url(bw->current_content);
	if (refresh == NULL)
		return;

	/* mark this content as invalid so it gets flushed from the cache */
	content_invalidate_reuse_data(bw->current_content);

	url = hlcache_handle_get_url(bw->current_content);
	if ((url == NULL) || (nsurl_compare(url, refresh, NSURL_COMPLETE))) {
		flags |= BW_NAVIGATE_HISTORY;
	}

	/* Treat an (almost) immediate refresh in a top-level browser window as
	 * if it were an HTTP redirect, and thus make the resulting fetch
	 * verifiable.
	 *
	 * See fetchcache.c for why redirected fetches should be verifiable at
	 * all.
	 */
	if (bw->refresh_interval <= 100 && bw->parent == NULL) {
		flags &= ~BW_NAVIGATE_UNVERIFIABLE;
	} else {
		parent = bw->current_content;
	}

	browser_window_navigate(bw,
				refresh,
				url,
				flags,
				NULL,
				NULL,
				parent);

}


/**
 * Transfer the loading_content to a new download window.
 */
static void
browser_window_convert_to_download(struct browser_window *bw,
				   llcache_handle *stream)
{
	struct browser_window *root = browser_window_get_root(bw);
	nserror error;

	assert(root != NULL);

	error = download_context_create(stream, root->window);
	if (error != NSERROR_OK) {
		llcache_handle_abort(stream);
		llcache_handle_release(stream);
	}

	/* remove content from browser window */
	hlcache_handle_release(bw->loading_content);
	bw->loading_content = NULL;

	browser_window_stop_throbber(bw);
}


/**
 * scroll to a fragment if present
 *
 * \param bw browser window
 * \return true if the scroll was sucessful
 */
static bool frag_scroll(struct browser_window *bw)
{
	struct rect rect;

	if (bw->frag_id == NULL) {
		return false;
	}

	if (!html_get_id_offset(bw->current_content,
				bw->frag_id,
				&rect.x0,
				&rect.y0)) {
		return false;
	}

	rect.x1 = rect.x0;
	rect.y1 = rect.y0;
	if (browser_window_set_scroll(bw, &rect) == NSERROR_OK) {
		if (bw->current_content != NULL &&
		    bw->history != NULL &&
		    bw->history->current != NULL) {
			browser_window_history_update(bw, bw->current_content);
		}
		return true;
	}
	return false;
}


/**
 * Redraw browser window, set extent to content, and update title.
 *
 * \param  bw		  browser_window
 * \param  scroll_to_top  move view to top of page
 */
static void browser_window_update(struct browser_window *bw, bool scroll_to_top)
{
	static const struct rect zrect = {
		.x0 = 0,
		.y0 = 0,
		.x1 = 0,
		.y1 = 0
	};

	if (bw->current_content == NULL) {
		return;
	}

	switch (bw->browser_window_type) {

	case BROWSER_WINDOW_NORMAL:
		/* Root browser window, constituting a front end window/tab */
		guit->window->set_title(bw->window,
					content_get_title(bw->current_content));

		browser_window_update_extent(bw);

		/* if frag_id exists, then try to scroll to it */
		/** @todo don't do this if the user has scrolled */
		if (!frag_scroll(bw)) {
			if (scroll_to_top) {
				browser_window_set_scroll(bw, &zrect);
			}
		}

		guit->window->invalidate(bw->window, NULL);

		break;

	case BROWSER_WINDOW_IFRAME:
		/* Internal iframe browser window */
		assert(bw->parent != NULL);
		assert(bw->parent->current_content != NULL);

		browser_window_update_extent(bw);

		if (scroll_to_top) {
			browser_window_set_scroll(bw, &zrect);
		}

		/* if frag_id exists, then try to scroll to it */
		/** @todo don't do this if the user has scrolled */
		frag_scroll(bw);

		browser_window_invalidate_iframe(bw);

		break;

	case BROWSER_WINDOW_FRAME:
		{
			struct rect rect;
			browser_window_update_extent(bw);

			if (scroll_to_top) {
				browser_window_set_scroll(bw, &zrect);
			}

			/* if frag_id exists, then try to scroll to it */
			/** @todo don't do this if the user has scrolled */
			frag_scroll(bw);

			rect.x0 = scrollbar_get_offset(bw->scroll_x);
			rect.y0 = scrollbar_get_offset(bw->scroll_y);
			rect.x1 = rect.x0 + bw->width;
			rect.y1 = rect.y0 + bw->height;

			browser_window_invalidate_rect(bw, &rect);
		}
		break;

	default:
	case BROWSER_WINDOW_FRAMESET:
		/* Nothing to do */
		break;
	}
}


/**
 * handle message for content ready on browser window
 */
static nserror browser_window_content_ready(struct browser_window *bw)
{
	int width, height;
	nserror res = NSERROR_OK;

	/* close and release the current window content */
	if (bw->current_content != NULL) {
		content_close(bw->current_content);
		hlcache_handle_release(bw->current_content);
	}

#ifdef __MACOS9__
	/* fixes268 (#10) — on top-level non-frame navigation, evict the
	 * decoded-image LRU and purge the low-level cache so heavy page →
	 * heavy page transitions start with fresh budget headroom. The old
	 * current_content has just been released above, so PNG-deferred
	 * bitmaps owned by that content have already been destroyed via
	 * the content_handler destroy chain. The LRU evict catches anything
	 * the destroy chain didn't (long-lived QT entries) and zeroes the
	 * global decoded-bytes counter. llcache_clean(true) drops cached
	 * fetched bytes from the previous page. */
	if (bw->parent == NULL) {
		extern void macos9_purge_decoded_images(void);
		extern void llcache_clean(bool purge);
		macos9_purge_decoded_images();
		llcache_clean(true);
	}
#endif

	bw->current_content = bw->loading_content;
	bw->loading_content = NULL;

	if (!bw->internal_nav) {
		/* Transfer the fetch parameters */
		browser_window__free_fetch_parameters(&bw->current_parameters);
		bw->current_parameters = bw->loading_parameters;
		memset(&bw->loading_parameters, 0, sizeof(bw->loading_parameters));
		/* Transfer the certificate chain */
		cert_chain_free(bw->current_cert_chain);
		bw->current_cert_chain = bw->loading_cert_chain;
		bw->loading_cert_chain = NULL;
	}

	/* Format the new content to the correct dimensions */
	browser_window_get_dimensions(bw, &width, &height);
	width /= bw->scale;
	height /= bw->scale;
	content_reformat(bw->current_content, false, width, height);

	/* history */
	if (bw->history_add && bw->history && !bw->internal_nav) {
		nsurl *url = hlcache_handle_get_url(bw->current_content);

		if (urldb_add_url(url)) {
			urldb_set_url_title(url, content_get_title(bw->current_content));
			urldb_update_url_visit_data(url);
			urldb_set_url_content_type(url,
						   content_get_type(bw->current_content));

			/* This is safe as we've just added the URL */
			global_history_add(urldb_get_url(url));
		}
		/**
		 * \todo Urldb / Thumbnails / Local history brokenness
		 *
		 * We add to local history after calling urldb_add_url rather
		 *  than in the block above.  If urldb_add_url fails (as it
		 *  will for urls like "about:about", "about:config" etc),
		 *  there would be no local history node, and later calls to
		 *  history_update will either explode or overwrite the node
		 *  for the previous URL.
		 *
		 * We call it after, rather than before urldb_add_url because
		 *  history_add calls bitmap render, which tries to register
		 *  the thumbnail with urldb.  That thumbnail registration
		 *  fails if the url doesn't exist in urldb already, and only
		 *  urldb-registered thumbnails get freed.  So if we called
		 *  history_add before urldb_add_url we would leak thumbnails
		 *  for all newly visited URLs.  With the history_add call
		 *  after, we only leak the thumbnails when urldb does not add
		 *  the URL.
		 *
		 * Also, since browser_window_history_add can create a
		 *  thumbnail (content_redraw), we need to do it after
		 *  content_reformat.
		 */
		browser_window_history_add(bw, bw->current_content, bw->frag_id);
	}

	browser_window_remove_caret(bw, false);

	if (bw->window != NULL) {
		guit->window->event(bw->window, GW_EVENT_NEW_CONTENT);

		browser_window_refresh_url_bar(bw);
	}

	/* new content; set scroll_to_top */
	browser_window_update(bw, true);
	content_open(bw->current_content, bw, 0, 0);
	browser_window_set_status(bw, content_get_status_message(bw->current_content));

	/* frames */
	res = browser_window_create_frameset(bw);

	/* iframes */
	res = browser_window_create_iframes(bw);

	/* Indicate page status may have changed */
	if (res == NSERROR_OK) {
		struct browser_window *root = browser_window_get_root(bw);
		res = guit->window->event(root->window, GW_EVENT_PAGE_INFO_CHANGE);
	}

	return res;
}


/**
 * handle message for content done on browser window
 */
static nserror
browser_window_content_done(struct browser_window *bw)
{
	float sx, sy;
	struct rect rect;
	int scrollx;
	int scrolly;

	if (bw->window == NULL) {
		/* Updated browser window's scrollbars. */
		/**
		 * \todo update browser window scrollbars before CONTENT_MSG_DONE
		 */
		browser_window_reformat(bw, true, bw->width, bw->height);
		browser_window_handle_scrollbars(bw);
	}

	browser_window_update(bw, false);
	browser_window_set_status(bw, content_get_status_message(bw->current_content));
	browser_window_stop_throbber(bw);
	browser_window_update_favicon(bw->current_content, bw, NULL);

	if (browser_window_history_get_scroll(bw, &sx, &sy) == NSERROR_OK) {
		scrollx = (int)((float)content_get_width(bw->current_content) * sx);
		scrolly = (int)((float)content_get_height(bw->current_content) * sy);
		rect.x0 = rect.x1 = scrollx;
		rect.y0 = rect.y1 = scrolly;
		if (browser_window_set_scroll(bw, &rect) != NSERROR_OK) {
			nslog_log(__FILE__, "", __LINE__,
				      "Unable to set browser scroll offsets to %d by %d",
				      scrollx,
				      scrolly);
		}
	}

	if (!bw->internal_nav) {
		browser_window_history_update(bw, bw->current_content);
		hotlist_update_url(hlcache_handle_get_url(bw->current_content));
	}

	if (bw->refresh_interval != -1) {
		guit->misc->schedule(bw->refresh_interval * 10,
				     browser_window_refresh, bw);
	}

	return NSERROR_OK;
}


/**
 * Handle query responses from SSL requests
 */
static nserror
browser_window__handle_ssl_query_response(bool proceed, void *pw)
{
	struct browser_window *bw = (struct browser_window *)pw;

	/* If we're in the process of loading, stop the load */
	if (bw->loading_content != NULL) {
		/* We had a loading content (maybe auth page?) */
		browser_window_stop(bw);
		browser_window_remove_caret(bw, false);
		browser_window_destroy_children(bw);
		browser_window_destroy_iframes(bw);
	}

	if (!proceed) {
		/* We're processing a "back to safety", do a rough-and-ready
		 * nav to the old 'current' parameters, with any post data
		 * stripped away
		 */
		return browser_window__reload_current_parameters(bw);
	}

	/* We're processing a "proceed" attempt from the form */
	/* First, we permit the SSL */
	urldb_set_cert_permissions(bw->loading_parameters.url, true);

	/* And then we navigate to the original loading parameters */
	bw->internal_nav = false;

	return browser_window__navigate_internal(bw, &bw->loading_parameters);
}


/**
 * Unpack a "username:password" to components.
 *
 * \param[in]  userpass      The input string to split.
 * \param[in]  username_out  Returns username on success.  Owned by caller.
 * \param[out] password_out  Returns password on success.  Owned by caller.
 * \return NSERROR_OK, or appropriate error code.
 */
static nserror
browser_window__unpack_userpass(const char *userpass,
				char **username_out,
				char **password_out)
{
	const char *tmp;
	char *username;
	char *password;
	size_t len;

	if (userpass == NULL) {
		username = malloc(1);
		password = malloc(1);
		if (username == NULL || password == NULL) {
			free(username);
			free(password);
			return NSERROR_NOMEM;
		}
		username[0] = '\0';
		password[0] = '\0';

		*username_out = username;
		*password_out = password;
		return NSERROR_OK;
	}

	tmp = strchr(userpass, ':');
	if (tmp == NULL) {
		return NSERROR_BAD_PARAMETER;
	} else {
		size_t len2;
		len = tmp - userpass;
		len2 = strlen(++tmp);

		username = malloc(len + 1);
		password = malloc(len2 + 1);
		if (username == NULL || password == NULL) {
			free(username);
			free(password);
			return NSERROR_NOMEM;
		}
		memcpy(username, userpass, len);
		username[len] = '\0';
		memcpy(password, tmp, len2 + 1);
	}

	*username_out = username;
	*password_out = password;
	return NSERROR_OK;
}


/**
 * Build a "username:password" from components.
 *
 * \param[in]  username      The username component.
 * \param[in]  password      The password component.
 * \param[out] userpass_out  Returns combined string on success.
 *                           Owned by caller.
 * \return NSERROR_OK, or appropriate error code.
 */
static nserror
browser_window__build_userpass(const char *username,
			       const char *password,
			       char **userpass_out)
{
	char *userpass;
	size_t len;

	len = strlen(username) + 1 + strlen(password) + 1;

	userpass = malloc(len);
	if (userpass == NULL) {
		return NSERROR_NOMEM;
	}

	snprintf(userpass, len, "%s:%s", username, password);

	*userpass_out = userpass;
	return NSERROR_OK;
}


/**
 * Handle a response from the UI when prompted for credentials
 */
static nserror
browser_window__handle_userpass_response(nsurl *url,
					 const char *realm,
					 const char *username,
					 const char *password,
					 void *pw)
{
	struct browser_window *bw = (struct browser_window *)pw;
	char *userpass;
	nserror err;

	err = browser_window__build_userpass(username, password, &userpass);
	if (err != NSERROR_OK) {
		return err;
	}

	urldb_set_auth_details(url, realm, userpass);

	free(userpass);

	/**
	 * \todo QUERY - Eventually this should fill out the form *NOT* nav
	 *               to the original location
	 */
	/* Finally navigate to the original loading parameters */
	if (bw->loading_content != NULL) {
		/* We had a loading content (maybe auth page?) */
		browser_window_stop(bw);
		browser_window_remove_caret(bw, false);
		browser_window_destroy_children(bw);
		browser_window_destroy_iframes(bw);
	}
	bw->internal_nav = false;
	return browser_window__navigate_internal(bw, &bw->loading_parameters);
}


/**
 * Handle login request (BAD_AUTH) during fetch
 *
 */
static nserror
browser_window__handle_login(struct browser_window *bw,
			     const char *realm,
			     nsurl *url) {
	char *username = NULL, *password = NULL;
	nserror err = NSERROR_OK;
	struct browser_fetch_parameters params;

	memset(&params, 0, sizeof(params));

	/* Step one, retrieve what we have */
	err = browser_window__unpack_userpass(
				      urldb_get_auth_details(url, realm),
				      &username, &password);
	if (err != NSERROR_OK) {
		goto out;
	}

	/* Step two, construct our fetch parameters */
	params.url = nsurl_ref(corestring_nsurl_about_query_auth);
	params.referrer = nsurl_ref(url);
	params.flags = BW_NAVIGATE_HISTORY | BW_NAVIGATE_NO_TERMINAL_HISTORY_UPDATE | BW_NAVIGATE_INTERNAL;

	err = fetch_multipart_data_new_kv(&params.post_multipart,
					  "siteurl",
					  nsurl_access(url));
	if (err != NSERROR_OK) {
		goto out;
	}

	err = fetch_multipart_data_new_kv(&params.post_multipart,
					  "realm",
					  realm);
	if (err != NSERROR_OK) {
		goto out;
	}

	err = fetch_multipart_data_new_kv(&params.post_multipart,
					  "username",
					  username);
	if (err != NSERROR_OK) {
		goto out;
	}

	err = fetch_multipart_data_new_kv(&params.post_multipart,
					  "password",
					  password);
	if (err != NSERROR_OK) {
		goto out;
	}

	/* Now we issue the fetch */
	bw->internal_nav = true;
	err = browser_window__navigate_internal(bw, &params);

	if (err != NSERROR_OK) {
		goto out;
	}

	err = guit->misc->login(url, realm, username, password,
				browser_window__handle_userpass_response, bw);

	if (err == NSERROR_NOT_IMPLEMENTED) {
		err = NSERROR_OK;
	}
 out:
	if (username != NULL) {
		free(username);
	}
	if (password != NULL) {
		free(password);
	}
	browser_window__free_fetch_parameters(&params);
	return err;
}


/**
 * Handle a certificate verification request (BAD_CERTS) during a fetch
 */
static nserror
browser_window__handle_bad_certs(struct browser_window *bw,
				 nsurl *url)
{
	struct browser_fetch_parameters params;
	nserror err;
	/* Initially we don't know WHY the SSL cert was bad */
	const char *reason = messages_get_sslcode(SSL_CERT_ERR_UNKNOWN);
	size_t depth;
	nsurl *chainurl = NULL;

	memset(&params, 0, sizeof(params));

	params.url = nsurl_ref(corestring_nsurl_about_query_ssl);
	params.referrer = nsurl_ref(url);
	params.flags = BW_NAVIGATE_HISTORY | BW_NAVIGATE_NO_TERMINAL_HISTORY_UPDATE | BW_NAVIGATE_INTERNAL;

	err = fetch_multipart_data_new_kv(&params.post_multipart,
					  "siteurl",
					  nsurl_access(url));
	if (err != NSERROR_OK) {
		goto out;
	}

	if (bw->loading_cert_chain != NULL) {
		for (depth = 0; depth < bw->loading_cert_chain->depth; ++depth) {
			size_t idx = bw->loading_cert_chain->depth - (depth + 1);
			ssl_cert_err err = bw->loading_cert_chain->certs[idx].err;
			if (err != SSL_CERT_ERR_OK) {
				reason = messages_get_sslcode(err);
				break;
			}
		}

		err = cert_chain_to_query(bw->loading_cert_chain, &chainurl);
		if (err != NSERROR_OK) {
			goto out;
		}

		err = fetch_multipart_data_new_kv(&params.post_multipart,
						  "chainurl",
						  nsurl_access(chainurl));
		if (err != NSERROR_OK) {
			goto out;
		}
	}

	err = fetch_multipart_data_new_kv(&params.post_multipart,
					  "reason",
					  reason);
	if (err != NSERROR_OK) {
		goto out;
	}

	/* Now we issue the fetch */
	bw->internal_nav = true;
	err = browser_window__navigate_internal(bw, &params);
	if (err != NSERROR_OK) {
		goto out;
	}

 out:
	browser_window__free_fetch_parameters(&params);
	if (chainurl != NULL)
		nsurl_unref(chainurl);
	return err;
}


/**
 * Handle a timeout during a fetch
 */
static nserror
browser_window__handle_timeout(struct browser_window *bw, nsurl *url)
{
	struct browser_fetch_parameters params;
	nserror err;

	memset(&params, 0, sizeof(params));

	params.url = nsurl_ref(corestring_nsurl_about_query_timeout);
	params.referrer = nsurl_ref(url);
	params.flags = BW_NAVIGATE_HISTORY | BW_NAVIGATE_NO_TERMINAL_HISTORY_UPDATE | BW_NAVIGATE_INTERNAL;

	err = fetch_multipart_data_new_kv(&params.post_multipart,
					  "siteurl",
					  nsurl_access(url));
	if (err != NSERROR_OK) {
		goto out;
	}

	/* Now we issue the fetch */
	bw->internal_nav = true;
	err = browser_window__navigate_internal(bw, &params);
	if (err != NSERROR_OK) {
		goto out;
	}

 out:
	browser_window__free_fetch_parameters(&params);
	return err;
}


/**
 * Handle non specific errors during a fetch
 */
static nserror
browser_window__handle_fetcherror(struct browser_window *bw,
				  const char *reason,
				  nsurl *url)
{
	struct browser_fetch_parameters params;
	nserror err;

	memset(&params, 0, sizeof(params));

	params.url = nsurl_ref(corestring_nsurl_about_query_fetcherror);
	params.referrer = nsurl_ref(url);
	params.flags = BW_NAVIGATE_HISTORY | BW_NAVIGATE_NO_TERMINAL_HISTORY_UPDATE | BW_NAVIGATE_INTERNAL;

	err = fetch_multipart_data_new_kv(&params.post_multipart,
					  "siteurl",
					  nsurl_access(url));
	if (err != NSERROR_OK) {
		goto out;
	}

	err = fetch_multipart_data_new_kv(&params.post_multipart,
					  "reason",
					  reason);
	if (err != NSERROR_OK) {
		goto out;
	}

	/* Now we issue the fetch */
	bw->internal_nav = true;
	err = browser_window__navigate_internal(bw, &params);
	if (err != NSERROR_OK) {
		goto out;
	}

 out:
	browser_window__free_fetch_parameters(&params);
	return err;
}


/**
 * Handle errors during content fetch
 */
static nserror
browser_window__handle_error(struct browser_window *bw,
			     hlcache_handle *c,
			     const hlcache_event *event)
{
	const char *message = event->data.errordata.errormsg;
	nserror code = event->data.errordata.errorcode;
	nserror res;
	nsurl *url = hlcache_handle_get_url(c);

	/* Unexpected OK? */
	assert(code != NSERROR_OK);

	if (message == NULL) {
		message = messages_get_errorcode(code);
	} else {
		message = messages_get(message);
	}

	if (c == bw->loading_content) {
		bw->loading_content = NULL;
	} else if (c == bw->current_content) {
		bw->current_content = NULL;
		browser_window_remove_caret(bw, false);
	}

	hlcache_handle_release(c);

	switch (code) {
	case NSERROR_BAD_AUTH:
		res = browser_window__handle_login(bw, message, url);
		break;

	case NSERROR_BAD_CERTS:
		res = browser_window__handle_bad_certs(bw, url);
		break;

	case NSERROR_TIMEOUT:
		res = browser_window__handle_timeout(bw, url);
		break;

	default:
		res = browser_window__handle_fetcherror(bw, message, url);
		break;
	}

	return res;
}


/**
 * Update URL bar for a given browser window to given URL
 *
 * \param bw	Browser window to update URL bar for.
 * \param url	URL for content displayed by bw including any fragment.
 */
static inline nserror
browser_window_refresh_url_bar_internal(struct browser_window *bw, nsurl *url)
{
	assert(bw);
	assert(url);

	if ((bw->parent != NULL) || (bw->window == NULL)) {
		/* Not root window or no gui window so do not set a URL */
		return NSERROR_OK;
	}

	return guit->window->set_url(bw->window, url);
}


/**
 * Browser window content event callback handler.
 */
static nserror
browser_window_callback(hlcache_handle *c, const hlcache_event *event, void *pw)
{
	struct browser_window *bw = pw;
	nserror res = NSERROR_OK;

	switch (event->type) {
	case CONTENT_MSG_SSL_CERTS:
		/* SSL certificate information has arrived, store it */
		cert_chain_free(bw->loading_cert_chain);
		cert_chain_dup(event->data.chain, &bw->loading_cert_chain);
		break;

	case CONTENT_MSG_LOG:
		browser_window_console_log(bw,
					   event->data.log.src,
					   event->data.log.msg,
					   event->data.log.msglen,
					   event->data.log.flags);
		break;

	case CONTENT_MSG_DOWNLOAD:
		assert(bw->loading_content == c);

		browser_window_convert_to_download(bw, event->data.download);

		if (bw->current_content != NULL) {
			browser_window_refresh_url_bar(bw);
		}
		break;

	case CONTENT_MSG_LOADING:
		assert(bw->loading_content == c);

#ifdef WITH_THEME_INSTALL
		if (content_get_type(c) == CONTENT_THEME) {
			theme_install_start(c);
			bw->loading_content = NULL;
			browser_window_stop_throbber(bw);
		} else
#endif
		{
			bw->refresh_interval = -1;
			browser_window_set_status(bw,
						  content_get_status_message(c));
		}
		break;

	case CONTENT_MSG_READY:
		assert(bw->loading_content == c);

		res = browser_window_content_ready(bw);
		break;

	case CONTENT_MSG_DONE:
		assert(bw->current_content == c);

		res = browser_window_content_done(bw);
		break;

	case CONTENT_MSG_ERROR:
		res = browser_window__handle_error(bw, c, event);
		break;

	case CONTENT_MSG_REDIRECT:
		if (urldb_add_url(event->data.redirect.from)) {
			urldb_update_url_visit_data(event->data.redirect.from);
		}
		browser_window_refresh_url_bar_internal(bw, event->data.redirect.to);
		break;

	case CONTENT_MSG_STATUS:
		if (event->data.explicit_status_text == NULL) {
			/* Object content's status text updated */
			const char *status = NULL;
			if (bw->loading_content != NULL) {
				/* Give preference to any loading content */
				status = content_get_status_message(
							bw->loading_content);
			}

			if (status == NULL) {
				status = content_get_status_message(c);
			}

			if (status != NULL) {
				browser_window_set_status(bw, status);
			}
		} else {
			/* Object content wants to set explicit message */
			browser_window_set_status(bw,
					event->data.explicit_status_text);
		}
		break;

	case CONTENT_MSG_REFORMAT:
		if (c == bw->current_content) {
			/* recompute frameset */
			browser_window_recalculate_frameset(bw);

			/* recompute iframe positions, sizes and scrollbars */
			browser_window_recalculate_iframes(bw);
		}

		/* Hide any caret, but don't remove it */
		browser_window_remove_caret(bw, true);

		if (!(event->data.background)) {
			/* Reformatted content should be redrawn */
			browser_window_update(bw, false);
		}
		break;

	case CONTENT_MSG_REDRAW:
		{
			struct rect rect = {
					    .x0 = event->data.redraw.x,
					    .y0 = event->data.redraw.y,
					    .x1 = event->data.redraw.x + event->data.redraw.width,
					    .y1 = event->data.redraw.y + event->data.redraw.height
			};

			browser_window_invalidate_rect(bw, &rect);
		}
		break;

	case CONTENT_MSG_REFRESH:
		bw->refresh_interval = event->data.delay * 100;
		break;

	case CONTENT_MSG_LINK: /* content has an rfc5988 link element */
		{
			unsigned char match;

			/* Handle "icon" and "shortcut icon" */
			if ((lwc_string_caseless_isequal(
							 event->data.rfc5988_link->rel,
							 corestring_lwc_icon,
							 &match) == lwc_error_ok && match) ||
			    (lwc_string_caseless_isequal(
							 event->data.rfc5988_link->rel,
							 corestring_lwc_shortcut_icon,
							 &match) == lwc_error_ok && match)) {
				/* it's a favicon perhaps start a fetch for it */
				browser_window_update_favicon(c, bw,
						      event->data.rfc5988_link);
			}
		}
		break;

	case CONTENT_MSG_GETTHREAD:
		{
			/* only the content object created by the browser
			 * window requires a new javascript thread object
			 */
			jsthread *thread;
			assert(bw->loading_content == c);

			if (js_newthread(bw->jsheap,
					 bw,
					 hlcache_handle_get_content(c),
					 &thread) == NSERROR_OK) {
				/* The content which is requesting the thread
				 * is required to keep hold of it and
				 * to destroy it when it is finished with it.
				 */
				*(event->data.jsthread) = thread;
			}
		}
		break;

	case CONTENT_MSG_GETDIMS:
		{
			int width;
			int height;

			browser_window_get_dimensions(bw, &width, &height);

			*(event->data.getdims.viewport_width) = width / bw->scale;
			*(event->data.getdims.viewport_height) = height / bw->scale;
			break;
		}

	case CONTENT_MSG_SCROLL:
		{
			struct rect rect = {
					    .x0 = event->data.scroll.x0,
					    .y0 = event->data.scroll.y0,
			};

			/* Content wants to be scrolled */
			if (bw->current_content != c) {
				break;
			}

			if (event->data.scroll.area) {
				rect.x1 = event->data.scroll.x1;
				rect.y1 = event->data.scroll.y1;
			} else {
				rect.x1 = event->data.scroll.x0;
				rect.y1 = event->data.scroll.y0;
			}
			browser_window_set_scroll(bw, &rect);

			break;
		}

	case CONTENT_MSG_DRAGSAVE:
		{
			/* Content wants drag save of a content */
			struct browser_window *root = browser_window_get_root(bw);
			hlcache_handle *save = event->data.dragsave.content;

			if (save == NULL) {
				save = c;
			}

			switch(event->data.dragsave.type) {
			case CONTENT_SAVE_ORIG:
				guit->window->drag_save_object(root->window,
							       save,
							       GUI_SAVE_OBJECT_ORIG);
				break;

			case CONTENT_SAVE_NATIVE:
				guit->window->drag_save_object(root->window,
							       save,
							       GUI_SAVE_OBJECT_NATIVE);
				break;

			case CONTENT_SAVE_COMPLETE:
				guit->window->drag_save_object(root->window,
							       save,
							       GUI_SAVE_COMPLETE);
				break;

			case CONTENT_SAVE_SOURCE:
				guit->window->drag_save_object(root->window,
							       save,
							       GUI_SAVE_SOURCE);
				break;
			}
		}
		break;

	case CONTENT_MSG_SAVELINK:
		{
			/* Content wants a link to be saved */
			struct browser_window *root = browser_window_get_root(bw);
			guit->window->save_link(root->window,
						event->data.savelink.url,
						event->data.savelink.title);
		}
		break;

	case CONTENT_MSG_POINTER:
		/* Content wants to have specific mouse pointer */
		browser_window_set_pointer(bw, event->data.pointer);
		break;

	case CONTENT_MSG_DRAG:
		{
			browser_drag_type bdt = DRAGGING_NONE;

			switch (event->data.drag.type) {
			case CONTENT_DRAG_NONE:
				bdt = DRAGGING_NONE;
				break;
			case CONTENT_DRAG_SCROLL:
				bdt = DRAGGING_CONTENT_SCROLLBAR;
				break;
			case CONTENT_DRAG_SELECTION:
				bdt = DRAGGING_SELECTION;
				break;
			}
			browser_window_set_drag_type(bw, bdt, event->data.drag.rect);
		}
		break;

	case CONTENT_MSG_CARET:
		switch (event->data.caret.type) {
		case CONTENT_CARET_REMOVE:
			browser_window_remove_caret(bw, false);
			break;
		case CONTENT_CARET_HIDE:
			browser_window_remove_caret(bw, true);
			break;
		case CONTENT_CARET_SET_POS:
			browser_window_place_caret(bw,
						   event->data.caret.pos.x,
						   event->data.caret.pos.y,
						   event->data.caret.pos.height,
						   event->data.caret.pos.clip);
			break;
		}
		break;

	case CONTENT_MSG_SELECTION:
		browser_window_set_selection(bw,
					     event->data.selection.selection,
					     event->data.selection.read_only);
		break;

	case CONTENT_MSG_SELECTMENU:
		if (event->data.select_menu.gadget->type == GADGET_SELECT) {
			struct browser_window *root =
				browser_window_get_root(bw);
			guit->window->create_form_select_menu(root->window,
							      event->data.select_menu.gadget);
		}

		break;

	case CONTENT_MSG_GADGETCLICK:
		if (event->data.gadget_click.gadget->type == GADGET_FILE) {
			struct browser_window *root =
				browser_window_get_root(bw);
			guit->window->file_gadget_open(root->window, c,
						       event->data.gadget_click.gadget);
		}

		break;


	case CONTENT_MSG_TEXTSEARCH:
		switch (event->data.textsearch.type) {
		case CONTENT_TEXTSEARCH_FIND:
			guit->search->hourglass(event->data.textsearch.state,
						event->data.textsearch.ctx);
			break;

		case CONTENT_TEXTSEARCH_MATCH:
			guit->search->status(event->data.textsearch.state,
					     event->data.textsearch.ctx);
			break;

		case CONTENT_TEXTSEARCH_BACK:
			guit->search->back_state(event->data.textsearch.state,
						 event->data.textsearch.ctx);
			break;

		case CONTENT_TEXTSEARCH_FORWARD:
			guit->search->forward_state(event->data.textsearch.state,
						    event->data.textsearch.ctx);
			break;

		case CONTENT_TEXTSEARCH_RECENT:
			guit->search->add_recent(event->data.textsearch.string,
						 event->data.textsearch.ctx);

			break;
		}
		break;

	default:
		break;
	}

	return res;
}


/**
 * internal scheduled reformat callback.
 *
 * scheduled reformat callback to allow reformats from unthreaded context.
 *
 * \param vbw The browser window to be reformatted
 */
static void scheduled_reformat(void *vbw)
{
	struct browser_window *bw = vbw;
	int width;
	int height;
	nserror res;

	MS_LOG("scheduled_reformat: entry");
	res = guit->window->get_dimensions(bw->window, &width, &height);
	if (res == NSERROR_OK) {
		macsurf_debug_log_writef(
			"scheduled_reformat: dims w=%d h=%d -> browser_window_reformat",
			width, height);
		browser_window_reformat(bw, false, width, height);
		MS_LOG("scheduled_reformat: return from browser_window_reformat");
	} else {
		macsurf_debug_log_writef(
			"scheduled_reformat: get_dimensions FAIL res=%d", (int)res);
	}
}

/* exported interface documented in desktop/browser_private.h */
nserror browser_window_destroy_internal(struct browser_window *bw)
{
	assert(bw);

	browser_window_destroy_children(bw);
	browser_window_destroy_iframes(bw);

	/* Destroy scrollbars */
	if (bw->scroll_x != NULL) {
		scrollbar_destroy(bw->scroll_x);
	}

	if (bw->scroll_y != NULL) {
		scrollbar_destroy(bw->scroll_y);
	}

	/* clear any pending callbacks */
	guit->misc->schedule(-1, browser_window_refresh, bw);
	nslog_log(__FILE__, "", __LINE__,
		      "Clearing reformat schedule for browser window %p",
		      bw);
	guit->misc->schedule(-1, scheduled_reformat, bw);

	/* If this brower window is not the root window, and has focus, unset
	 * the root browser window's focus pointer. */
	if (!bw->window) {
		struct browser_window *top = browser_window_get_root(bw);

		if (top->focus == bw)
			top->focus = top;

		if (top->selection.bw == bw) {
			browser_window_set_selection(top, false, false);
		}
	}

	/* Destruction order is important: we must ensure that the frontend
	 * destroys any window(s) associated with this browser window before
	 * we attempt any destructive cleanup.
	 */

	if (bw->window) {
		/* Only the root window has a GUI window */
		guit->window->destroy(bw->window);
	}

	if (bw->loading_content != NULL) {
		hlcache_handle_abort(bw->loading_content);
		hlcache_handle_release(bw->loading_content);
		bw->loading_content = NULL;
	}

	if (bw->current_content != NULL) {
		content_close(bw->current_content);
		hlcache_handle_release(bw->current_content);
		bw->current_content = NULL;
	}

	if (bw->favicon.loading != NULL) {
		hlcache_handle_abort(bw->favicon.loading);
		hlcache_handle_release(bw->favicon.loading);
		bw->favicon.loading = NULL;
	}

	if (bw->favicon.current != NULL) {
		content_close(bw->favicon.current);
		hlcache_handle_release(bw->favicon.current);
		bw->favicon.current = NULL;
	}

	if (bw->jsheap != NULL) {
		js_destroyheap(bw->jsheap);
		bw->jsheap = NULL;
	}

	/* These simply free memory, so are safe here */

	lwc_string_unref(bw->frag_id);

	browser_window_history_destroy(bw);

	cert_chain_free(bw->current_cert_chain);
	cert_chain_free(bw->loading_cert_chain);
	bw->current_cert_chain = NULL;
	bw->loading_cert_chain = NULL;

	free(bw->name);
	free(bw->status.text);
	bw->status.text = NULL;
	browser_window__free_fetch_parameters(&bw->current_parameters);
	browser_window__free_fetch_parameters(&bw->loading_parameters);
	nslog_log(__FILE__, "", __LINE__,
		      "Status text cache match:miss %d:%d",
		      bw->status.match,
		      bw->status.miss);

	return NSERROR_OK;
}


/**
 * Set browser window scale.
 *
 * \param bw Browser window.
 * \param absolute scale value.
 * \return NSERROR_OK on success else error code
 */
static nserror
browser_window_set_scale_internal(struct browser_window *bw, float scale)
{
	int i;
	nserror res = NSERROR_OK;

	/* do not apply tiny changes in scale */
	if (fabs(bw->scale - scale) < 0.0001)
		return res;

	bw->scale = scale;

	if (bw->current_content != NULL) {
		if (content_can_reformat(bw->current_content) == false) {
			browser_window_update(bw, false);
		} else {
			res = browser_window_schedule_reformat(bw);
		}
	}

	/* scale frames */
	for (i = 0; i < (bw->cols * bw->rows); i++) {
		res = browser_window_set_scale_internal(&bw->children[i], scale);
	}

	/* scale iframes */
	for (i = 0; i < bw->iframe_count; i++) {
		res = browser_window_set_scale_internal(&bw->iframes[i], scale);
	}

	return res;
}


/**
 * Find browser window.
 *
 * \param bw Browser window.
 * \param target Name of target.
 * \param depth Depth to scan.
 * \param page The browser window page.
 * \param rdepth The rdepth.
 * \param bw_target the output browser window.
 */
static void
browser_window_find_target_internal(struct browser_window *bw,
				    const char *target,
				    int depth,
				    struct browser_window *page,
				    int *rdepth,
				    struct browser_window **bw_target)
{
	int i;

	if ((bw->name) && (!strcasecmp(bw->name, target))) {
		if ((bw == page) || (depth > *rdepth)) {
			*rdepth = depth;
			*bw_target = bw;
		}
	}

	if ((!bw->children) && (!bw->iframes))
		return;

	depth++;

	if (bw->children != NULL) {
		for (i = 0; i < (bw->cols * bw->rows); i++) {
			if ((bw->children[i].name) &&
			    (!strcasecmp(bw->children[i].name,
					 target))) {
				if ((page == &bw->children[i]) ||
				    (depth > *rdepth)) {
					*rdepth = depth;
					*bw_target = &bw->children[i];
				}
			}
			if (bw->children[i].children)
				browser_window_find_target_internal(
							    &bw->children[i],
							    target, depth, page,
							    rdepth, bw_target);
		}
	}

	if (bw->iframes != NULL) {
		for (i = 0; i < bw->iframe_count; i++) {
			browser_window_find_target_internal(&bw->iframes[i],
							    target,
							    depth,
							    page,
							    rdepth,
							    bw_target);
		}
	}
}


/**
 * Handles the end of a drag operation in a browser window.
 *
 * \param  bw	  browser window
 * \param  mouse  state of mouse buttons and modifier keys
 * \param  x	  coordinate of mouse
 * \param  y	  coordinate of mouse
 *
 * \todo Remove this function, once these things are associated with content,
 *       rather than bw.
 */
static void
browser_window_mouse_drag_end(struct browser_window *bw,
			      browser_mouse_state mouse,
			      int x, int y)
{
	int scr_x, scr_y;

	switch (bw->drag.type) {
	case DRAGGING_SELECTION:
	case DRAGGING_OTHER:
	case DRAGGING_CONTENT_SCROLLBAR:
		/* Drag handled by content handler */
		break;

	case DRAGGING_SCR_X:

		browser_window_get_scrollbar_pos(bw, true, &scr_x, &scr_y);

		scr_x = x - scr_x - scrollbar_get_offset(bw->scroll_x);
		scr_y = y - scr_y - scrollbar_get_offset(bw->scroll_y);

		scrollbar_mouse_drag_end(bw->scroll_x, mouse, scr_x, scr_y);

		bw->drag.type = DRAGGING_NONE;
		break;

	case DRAGGING_SCR_Y:

		browser_window_get_scrollbar_pos(bw, false, &scr_x, &scr_y);

		scr_x = x - scr_x - scrollbar_get_offset(bw->scroll_x);
		scr_y = y - scr_y - scrollbar_get_offset(bw->scroll_y);

		scrollbar_mouse_drag_end(bw->scroll_y, mouse, scr_x, scr_y);

		bw->drag.type = DRAGGING_NONE;
		break;

	default:
		browser_window_set_drag_type(bw, DRAGGING_NONE, NULL);
		break;
	}
}

/**
 * Process mouse click event
 *
 * \param bw The browsing context receiving the event
 * \param mouse The mouse event state
 * \param x The scaled x co-ordinate of the event
 * \param y The scaled y co-ordinate of the event
 */
static void
browser_window_mouse_click_internal(struct browser_window *bw,
				    browser_mouse_state mouse,
				    int x, int y)
{
	hlcache_handle *c = bw->current_content;
	const char *status = NULL;
	browser_pointer_shape pointer = BROWSER_POINTER_DEFAULT;

	if (bw->children) {
		/* Browser window has children (frames) */
		struct browser_window *child;
		int cur_child;
		int children = bw->rows * bw->cols;

		for (cur_child = 0; cur_child < children; cur_child++) {

			child = &bw->children[cur_child];

			if ((x < child->x) ||
			    (y < child->y) ||
			    (child->x + child->width < x) ||
			    (child->y + child->height < y)) {
				/* Click not in this child */
				continue;
			}

			/* It's this child that contains the click; pass it
			 * on to child. */
			browser_window_mouse_click_internal(
				child,
				mouse,
				x - child->x + scrollbar_get_offset(child->scroll_x),
				y - child->y + scrollbar_get_offset(child->scroll_y));

			/* Mouse action was for this child, we're done */
			return;
		}

		return;
	}

	if (!c)
		return;

	if (bw->scroll_x != NULL) {
		int scr_x, scr_y;
		browser_window_get_scrollbar_pos(bw, true, &scr_x, &scr_y);
		scr_x = x - scr_x - scrollbar_get_offset(bw->scroll_x);
		scr_y = y - scr_y - scrollbar_get_offset(bw->scroll_y);

		if (scr_x > 0 && scr_x < get_horz_scrollbar_len(bw) &&
		    scr_y > 0 && scr_y < SCROLLBAR_WIDTH) {
			status = scrollbar_mouse_status_to_message(
					   scrollbar_mouse_action(
						  bw->scroll_x, mouse,
						  scr_x, scr_y));
			pointer = BROWSER_POINTER_DEFAULT;

			if (status != NULL)
				browser_window_set_status(bw, status);

			browser_window_set_pointer(bw, pointer);
			return;
		}
	}

	if (bw->scroll_y != NULL) {
		int scr_x, scr_y;
		browser_window_get_scrollbar_pos(bw, false, &scr_x, &scr_y);
		scr_x = x - scr_x - scrollbar_get_offset(bw->scroll_x);
		scr_y = y - scr_y - scrollbar_get_offset(bw->scroll_y);

		if (scr_y > 0 && scr_y < get_vert_scrollbar_len(bw) &&
		    scr_x > 0 && scr_x < SCROLLBAR_WIDTH) {
			status = scrollbar_mouse_status_to_message(
						scrollbar_mouse_action(
							bw->scroll_y,
							mouse,
							scr_x,
							scr_y));
			pointer = BROWSER_POINTER_DEFAULT;

			if (status != NULL) {
				browser_window_set_status(bw, status);
			}

			browser_window_set_pointer(bw, pointer);
			return;
		}
	}

	switch (content_get_type(c)) {
	case CONTENT_HTML:
	case CONTENT_TEXTPLAIN:
		{
			/* Give bw focus */
			struct browser_window *root_bw = browser_window_get_root(bw);
			if (bw != root_bw->focus) {
				browser_window_remove_caret(bw, false);
				browser_window_set_selection(bw, false, true);
				root_bw->focus = bw;
			}

			/* Pass mouse action to content */
			content_mouse_action(c, bw, mouse, x, y);
		}
		break;
	default:
		if (mouse & BROWSER_MOUSE_MOD_2) {
			if (mouse & BROWSER_MOUSE_DRAG_2) {
				guit->window->drag_save_object(bw->window, c,
							       GUI_SAVE_OBJECT_NATIVE);
			} else if (mouse & BROWSER_MOUSE_DRAG_1) {
				guit->window->drag_save_object(bw->window, c,
							       GUI_SAVE_OBJECT_ORIG);
			}
		} else if (mouse & (BROWSER_MOUSE_DRAG_1 |
				    BROWSER_MOUSE_DRAG_2)) {
			browser_window_page_drag_start(bw, x, y);
			browser_window_set_pointer(bw, BROWSER_POINTER_MOVE);
		}
		break;
	}
}


/**
 * Process mouse movement event
 *
 * \param bw The browsing context receiving the event
 * \param mouse The mouse event state
 * \param x The scaled x co-ordinate of the event
 * \param y The scaled y co-ordinate of the event
 */
static void
browser_window_mouse_track_internal(struct browser_window *bw,
				    browser_mouse_state mouse,
				    int x, int y)
{
	hlcache_handle *c = bw->current_content;
	const char *status = NULL;
	browser_pointer_shape pointer = BROWSER_POINTER_DEFAULT;

	if (bw->window != NULL && bw->drag.window && bw != bw->drag.window) {
		/* This is the root browser window and there's an active drag
		 * in a sub window.
		 * Pass the mouse action straight on to that bw. */
		struct browser_window *drag_bw = bw->drag.window;
		int off_x = 0;
		int off_y = 0;

		browser_window_get_position(drag_bw, true, &off_x, &off_y);

		if (drag_bw->browser_window_type == BROWSER_WINDOW_FRAME) {
			browser_window_mouse_track_internal(drag_bw,
							    mouse,
							    x - off_x,
							    y - off_y);

		} else if (drag_bw->browser_window_type == BROWSER_WINDOW_IFRAME) {
			browser_window_mouse_track_internal(drag_bw, mouse,
							    x - off_x / bw->scale,
							    y - off_y / bw->scale);
		}
		return;
	}

	if (bw->children) {
		/* Browser window has children (frames) */
		struct browser_window *child;
		int cur_child;
		int children = bw->rows * bw->cols;

		for (cur_child = 0; cur_child < children; cur_child++) {

			child = &bw->children[cur_child];

			if ((x < child->x) ||
			    (y < child->y) ||
			    (child->x + child->width < x) ||
			    (child->y + child->height < y)) {
				/* Click not in this child */
				continue;
			}

			/* It's this child that contains the mouse; pass
			 * mouse action on to child */
			browser_window_mouse_track_internal(
				child,
				mouse,
				x - child->x + scrollbar_get_offset(child->scroll_x),
				y - child->y + scrollbar_get_offset(child->scroll_y));

			/* Mouse action was for this child, we're done */
			return;
		}

		/* Odd if we reached here, but nothing else can use the click
		 * when there are children. */
		return;
	}

	if (c == NULL && bw->drag.type != DRAGGING_FRAME) {
		return;
	}

	if (bw->drag.type != DRAGGING_NONE && !mouse) {
		browser_window_mouse_drag_end(bw, mouse, x, y);
	}

	/* Browser window's horizontal scrollbar */
	if (bw->scroll_x != NULL && bw->drag.type != DRAGGING_SCR_Y) {
		int scr_x, scr_y;
		browser_window_get_scrollbar_pos(bw, true, &scr_x, &scr_y);
		scr_x = x - scr_x - scrollbar_get_offset(bw->scroll_x);
		scr_y = y - scr_y - scrollbar_get_offset(bw->scroll_y);

		if ((bw->drag.type == DRAGGING_SCR_X) ||
		    (scr_x > 0 &&
		     scr_x < get_horz_scrollbar_len(bw) &&
		     scr_y > 0 &&
		     scr_y < SCROLLBAR_WIDTH &&
		     bw->drag.type == DRAGGING_NONE)) {
			/* Start a scrollbar drag, or continue existing drag */
			status = scrollbar_mouse_status_to_message(
					scrollbar_mouse_action(bw->scroll_x,
							       mouse,
							       scr_x,
							       scr_y));
			pointer = BROWSER_POINTER_DEFAULT;

			if (status != NULL) {
				browser_window_set_status(bw, status);
			}

			browser_window_set_pointer(bw, pointer);
			return;
		}
	}

	/* Browser window's vertical scrollbar */
	if (bw->scroll_y != NULL) {
		int scr_x, scr_y;
		browser_window_get_scrollbar_pos(bw, false, &scr_x, &scr_y);
		scr_x = x - scr_x - scrollbar_get_offset(bw->scroll_x);
		scr_y = y - scr_y - scrollbar_get_offset(bw->scroll_y);

		if ((bw->drag.type == DRAGGING_SCR_Y) ||
		    (scr_y > 0 &&
		     scr_y < get_vert_scrollbar_len(bw) &&
		     scr_x > 0 &&
		     scr_x < SCROLLBAR_WIDTH &&
		     bw->drag.type == DRAGGING_NONE)) {
			/* Start a scrollbar drag, or continue existing drag */
			status = scrollbar_mouse_status_to_message(
					scrollbar_mouse_action(bw->scroll_y,
							       mouse,
							       scr_x,
							       scr_y));
			pointer = BROWSER_POINTER_DEFAULT;

			if (status != NULL) {
				browser_window_set_status(bw, status);
			}

			browser_window_set_pointer(bw, pointer);
			return;
		}
	}

	if (bw->drag.type == DRAGGING_FRAME) {
		browser_window_resize_frame(bw, bw->x + x, bw->y + y);
	} else if (bw->drag.type == DRAGGING_PAGE_SCROLL) {
		/* mouse movement since drag started */
		struct rect rect;

		rect.x0 = bw->drag.start_x - x;
		rect.y0 = bw->drag.start_y - y;

		/* new scroll offsets */
		rect.x0 += bw->drag.start_scroll_x;
		rect.y0 += bw->drag.start_scroll_y;

		bw->drag.start_scroll_x = rect.x1 = rect.x0;
		bw->drag.start_scroll_y = rect.y1 = rect.y0;

		browser_window_set_scroll(bw, &rect);
	} else {
		assert(c != NULL);
		content_mouse_track(c, bw, mouse, x, y);
	}
}


/**
 * perform a scroll operation at a given coordinate
 *
 * \param bw The browsing context receiving the event
 * \param x The scaled x co-ordinate of the event
 * \param y The scaled y co-ordinate of the event
 */
static bool
browser_window_scroll_at_point_internal(struct browser_window *bw,
					int x, int y,
					int scrx, int scry)
{
	assert(bw != NULL);

	/* Handle (i)frame scroll offset (core-managed browser windows only) */
	x += scrollbar_get_offset(bw->scroll_x);
	y += scrollbar_get_offset(bw->scroll_y);

	if (bw->children) {
		/* Browser window has children, so pass request on to
		 * appropriate child */
		struct browser_window *bwc;
		int cur_child;
		int children = bw->rows * bw->cols;

		/* Loop through all children of bw */
		for (cur_child = 0; cur_child < children; cur_child++) {
			/* Set current child */
			bwc = &bw->children[cur_child];

			/* Skip this frame if (x, y) coord lies outside */
			if (x < bwc->x || bwc->x + bwc->width < x ||
			    y < bwc->y || bwc->y + bwc->height < y)
				continue;

			/* Pass request into this child */
			return browser_window_scroll_at_point_internal(
								bwc,
								(x - bwc->x),
								(y - bwc->y),
								scrx, scry);
		}
	}

	/* Try to scroll any current content */
	if (bw->current_content != NULL &&
	    content_scroll_at_point(bw->current_content, x, y, scrx, scry) == true) {
		/* Scroll handled by current content */
		return true;
	}

	/* Try to scroll this window. */
	return (int)scrollbar_scroll(bw->scroll_y, scry) |
		(int)scrollbar_scroll(bw->scroll_x, scrx);
}


/**
 * allows a dragged file to be dropped into a browser window at a position
 *
 * \param bw The browsing context receiving the event
 * \param x The scaled x co-ordinate of the event
 * \param y The scaled y co-ordinate of the event
 * \param file filename to be put in the widget
 */
static bool
browser_window_drop_file_at_point_internal(struct browser_window *bw,
					   int x, int y,
					   char *file)
{
	assert(bw != NULL);

	/* Handle (i)frame scroll offset (core-managed browser windows only) */
	x += scrollbar_get_offset(bw->scroll_x);
	y += scrollbar_get_offset(bw->scroll_y);

	if (bw->children) {
		/* Browser window has children, so pass request on to
		 * appropriate child */
		struct browser_window *bwc;
		int cur_child;
		int children = bw->rows * bw->cols;

		/* Loop through all children of bw */
		for (cur_child = 0; cur_child < children; cur_child++) {
			/* Set current child */
			bwc = &bw->children[cur_child];

			/* Skip this frame if (x, y) coord lies outside */
			if (x < bwc->x || bwc->x + bwc->width < x ||
			    y < bwc->y || bwc->y + bwc->height < y)
				continue;

			/* Pass request into this child */
			return browser_window_drop_file_at_point_internal(
								bwc,
								(x - bwc->x),
								(y - bwc->y),
								file);
		}
	}

	/* Pass file drop on to any content */
	if (bw->current_content != NULL) {
		return content_drop_file_at_point(bw->current_content,
						  x, y, file);
	}

	return false;
}


/**
 * Check if this is an internal navigation URL.
 *
 * This safely checks if the given url is an internal navigation even
 *  for urls with no scheme or path.
 *
 * \param url The URL to check
 * \return true if an internal navigation url else false
 */
static bool
is_internal_navigate_url(nsurl *url)
{
	bool is_internal = false;
	lwc_string *scheme, *path;

	scheme = nsurl_get_component(url, NSURL_SCHEME);
	if (scheme != NULL) {
		path = nsurl_get_component(url, NSURL_PATH);
		if (path != NULL) {
			if (scheme == corestring_lwc_about) {
				if (path == corestring_lwc_query_auth) {
					is_internal = true;
				} else if (path == corestring_lwc_query_ssl) {
					is_internal = true;
				} else if (path == corestring_lwc_query_timeout) {
					is_internal = true;
				} else if (path == corestring_lwc_query_fetcherror) {
					is_internal = true;
				}
			}
			lwc_string_unref(path);
		}
		lwc_string_unref(scheme);
	}
	return is_internal;
}


/* exported interface, documented in netsurf/browser_window.h */
nserror
browser_window_get_name(struct browser_window *bw, const char **out_name)
{
	assert(bw != NULL);

	*out_name = bw->name;

	return NSERROR_OK;
}


/* exported interface, documented in netsurf/browser_window.h */
nserror
browser_window_set_name(struct browser_window *bw, const char *name)
{
	char *nname = NULL;

	assert(bw != NULL);

	if (name != NULL) {
		nname = strdup(name);
		if (nname == NULL) {
			return NSERROR_NOMEM;
		}
	}

	if (bw->name != NULL) {
		free(bw->name);
	}

	bw->name = nname;

	return NSERROR_OK;
}


/* exported interface, documented in netsurf/browser_window.h */
bool
browser_window_redraw(struct browser_window *bw,
		      int x, int y,
		      const struct rect *clip,
		      const struct redraw_context *ctx)
{
	struct redraw_context new_ctx = *ctx;
	int width = 0;
	int height = 0;
	bool plot_ok = true;
	content_type content_type;
	struct content_redraw_data data;
	struct rect content_clip;
	nserror res;

	if (bw == NULL) {
		NSLOG(netsurf, INFO, "NULL browser window");
		return false;
	}

	x /= bw->scale;
	y /= bw->scale;

	if ((bw->current_content == NULL) &&
	    (bw->children == NULL)) {
		/* Browser window has no content, render blank fill */
		ctx->plot->clip(ctx, clip);
		return (ctx->plot->rectangle(ctx, plot_style_fill_white, clip) == NSERROR_OK);
	}

	/* Browser window has content OR children (frames) */
	if ((bw->window != NULL) &&
	    (ctx->plot->option_knockout)) {
		/* Root browser window: start knockout */
		knockout_plot_start(ctx, &new_ctx);
	}

	new_ctx.plot->clip(ctx, clip);

	/* Handle redraw of any browser window children */
	if (bw->children) {
		struct browser_window *child;
		int cur_child;
		int children = bw->rows * bw->cols;

		if (bw->window != NULL) {
			/* Root browser window; start with blank fill */
			plot_ok &= (new_ctx.plot->rectangle(ctx,
							    plot_style_fill_white,
							    clip) == NSERROR_OK);
		}

		/* Loop through all children of bw */
		for (cur_child = 0; cur_child < children; cur_child++) {
			/* Set current child */
			child = &bw->children[cur_child];

			/* Get frame edge area in global coordinates */
			content_clip.x0 = (x + child->x) * child->scale;
			content_clip.y0 = (y + child->y) * child->scale;
			content_clip.x1 = content_clip.x0 +
				child->width * child->scale;
			content_clip.y1 = content_clip.y0 +
				child->height * child->scale;

			/* Intersect it with clip rectangle */
			if (content_clip.x0 < clip->x0)
				content_clip.x0 = clip->x0;
			if (content_clip.y0 < clip->y0)
				content_clip.y0 = clip->y0;
			if (clip->x1 < content_clip.x1)
				content_clip.x1 = clip->x1;
			if (clip->y1 < content_clip.y1)
				content_clip.y1 = clip->y1;

			/* Skip this frame if it lies outside clip rectangle */
			if (content_clip.x0 >= content_clip.x1 ||
			    content_clip.y0 >= content_clip.y1)
				continue;

			/* Redraw frame */
			plot_ok &= browser_window_redraw(child,
							 x + child->x,
							 y + child->y,
							 &content_clip,
							 &new_ctx);
		}

		/* Nothing else to redraw for browser windows with children;
		 * cleanup and return
		 */
		if (bw->window != NULL && ctx->plot->option_knockout) {
			/* Root browser window: knockout end */
			knockout_plot_end(ctx);
		}

		return plot_ok;
	}

	/* Handle browser windows with content to redraw */

	content_type = content_get_type(bw->current_content);
	if (content_type != CONTENT_HTML && content_type != CONTENT_TEXTPLAIN) {
		/* Set render area according to scale */
		width = content_get_width(bw->current_content) * bw->scale;
		height = content_get_height(bw->current_content) * bw->scale;

		/* Non-HTML may not fill viewport to extents, so plot white
		 * background fill */
		plot_ok &= (new_ctx.plot->rectangle(&new_ctx,
						    plot_style_fill_white,
						    clip) == NSERROR_OK);
	}

	/* Set up content redraw data */
	data.x = x - scrollbar_get_offset(bw->scroll_x);
	data.y = y - scrollbar_get_offset(bw->scroll_y);
	data.width = width;
	data.height = height;

	data.background_colour = 0xFFFFFF;
	data.scale = bw->scale;
	data.repeat_x = false;
	data.repeat_y = false;

	content_clip = *clip;

	if (!bw->window) {
		int x0 = x * bw->scale;
		int y0 = y * bw->scale;
		int x1 = (x + bw->width - ((bw->scroll_y != NULL) ?
					   SCROLLBAR_WIDTH : 0)) * bw->scale;
		int y1 = (y + bw->height - ((bw->scroll_x != NULL) ?
					    SCROLLBAR_WIDTH : 0)) * bw->scale;

		if (content_clip.x0 < x0) content_clip.x0 = x0;
		if (content_clip.y0 < y0) content_clip.y0 = y0;
		if (x1 < content_clip.x1) content_clip.x1 = x1;
		if (y1 < content_clip.y1) content_clip.y1 = y1;
	}

	/* Render the content */
	plot_ok &= content_redraw(bw->current_content, &data,
				  &content_clip, &new_ctx);

	/* Back to full clip rect */
	new_ctx.plot->clip(&new_ctx, clip);

	if (!bw->window) {
		/* Render scrollbars */
		int off_x, off_y;
		if (bw->scroll_x != NULL) {
			browser_window_get_scrollbar_pos(bw, true,
							 &off_x, &off_y);
			res = scrollbar_redraw(bw->scroll_x,
					       x + off_x, y + off_y, clip,
					       bw->scale, &new_ctx);
			if (res != NSERROR_OK) {
				plot_ok = false;
			}
		}
		if (bw->scroll_y != NULL) {
			browser_window_get_scrollbar_pos(bw, false,
							 &off_x, &off_y);
			res = scrollbar_redraw(bw->scroll_y,
					       x + off_x, y + off_y, clip,
					       bw->scale, &new_ctx);
			if (res != NSERROR_OK) {
				plot_ok = false;
			}
		}
	}

	if (bw->window != NULL && ctx->plot->option_knockout) {
		/* Root browser window: end knockout */
		knockout_plot_end(ctx);
	}

	return plot_ok;
}


/* exported interface, documented in netsurf/browser_window.h */
bool browser_window_redraw_ready(struct browser_window *bw)
{
	if (bw == NULL) {
		NSLOG(netsurf, INFO, "NULL browser window");
		return false;
	} else if (bw->current_content != NULL) {
		/* Can't render locked contents */
		return !content_is_locked(bw->current_content);
	}

	return true;
}


/* exported interface, documented in browser_private.h */
void browser_window_update_extent(struct browser_window *bw)
{
	if (bw->window != NULL) {
		/* Front end window */
		guit->window->event(bw->window, GW_EVENT_UPDATE_EXTENT);
	} else {
		/* Core-managed browser window */
		browser_window_handle_scrollbars(bw);
	}
}


/* exported interface, documented in netsurf/browser_window.h */
void
browser_window_get_position(struct browser_window *bw,
			    bool root,
			    int *pos_x,
			    int *pos_y)
{
	*pos_x = 0;
	*pos_y = 0;

	assert(bw != NULL);

	while (bw) {
		switch (bw->browser_window_type) {

		case BROWSER_WINDOW_FRAMESET:
			*pos_x += bw->x * bw->scale;
			*pos_y += bw->y * bw->scale;
			break;

		case BROWSER_WINDOW_NORMAL:
			/* There is no offset to the root browser window */
			break;

		case BROWSER_WINDOW_FRAME:
			/* Iframe and Frame handling is identical;
			 * fall though */
		case BROWSER_WINDOW_IFRAME:
			*pos_x += (bw->x - scrollbar_get_offset(bw->scroll_x)) *
				bw->scale;
			*pos_y += (bw->y - scrollbar_get_offset(bw->scroll_y)) *
				bw->scale;
			break;
		}

		bw = bw->parent;

		if (!root) {
			/* return if we just wanted the position in the parent
			 * browser window. */
			return;
		}
	}
}


/* exported interface, documented in netsurf/browser_window.h */
void browser_window_set_position(struct browser_window *bw, int x, int y)
{
	assert(bw != NULL);

	if (bw->window == NULL) {
		/* Core managed browser window */
		bw->x = x;
		bw->y = y;
	} else {
		NSLOG(netsurf, INFO,
		      "Asked to set position of front end window.");
		assert(0);
	}
}


/* exported interface, documented in netsurf/browser_window.h */
void
browser_window_set_drag_type(struct browser_window *bw,
			     browser_drag_type type,
			     const struct rect *rect)
{
	struct browser_window *top_bw = browser_window_get_root(bw);
	gui_drag_type gtype;

	bw->drag.type = type;

	if (type == DRAGGING_NONE) {
		top_bw->drag.window = NULL;
	} else {
		top_bw->drag.window = bw;

		switch (type) {
		case DRAGGING_SELECTION:
			/** \todo tell front end */
			return;
		case DRAGGING_SCR_X:
		case DRAGGING_SCR_Y:
		case DRAGGING_CONTENT_SCROLLBAR:
			gtype = GDRAGGING_SCROLLBAR;
			break;
		default:
			gtype = GDRAGGING_OTHER;
			break;
		}

		guit->window->drag_start(top_bw->window, gtype, rect);
	}
}


/* exported interface, documented in netsurf/browser_window.h */
browser_drag_type browser_window_get_drag_type(struct browser_window *bw)
{
	return bw->drag.type;
}


/* exported interface, documented in netsurf/browser_window.h */
struct browser_window * browser_window_get_root(struct browser_window *bw)
{
	while (bw && bw->parent) {
		bw = bw->parent;
	}
	return bw;
}


/* exported interface, documented in netsurf/browser_window.h */
browser_editor_flags browser_window_get_editor_flags(struct browser_window *bw)
{
	browser_editor_flags ed_flags = BW_EDITOR_NONE;
	assert(bw->window);
	assert(bw->parent == NULL);

	if (bw->selection.bw != NULL) {
		ed_flags |= BW_EDITOR_CAN_COPY;

		if (!bw->selection.read_only)
			ed_flags |= BW_EDITOR_CAN_CUT;
	}

	if (bw->can_edit)
		ed_flags |= BW_EDITOR_CAN_PASTE;

	return ed_flags;
}


/* exported interface, documented in netsurf/browser_window.h */
bool browser_window_can_select(struct browser_window *bw)
{
	if (bw == NULL || bw->current_content == NULL)
		return false;

	/* TODO: We shouldn't have to know about specific content types
	 *       here.  There should be a content_is_selectable() call. */
	if (content_get_type(bw->current_content) != CONTENT_HTML &&
	    content_get_type(bw->current_content) !=
	    CONTENT_TEXTPLAIN)
		return false;

	return true;
}


/* exported interface, documented in netsurf/browser_window.h */
char * browser_window_get_selection(struct browser_window *bw)
{
	assert(bw->window);
	assert(bw->parent == NULL);

	if (bw->selection.bw == NULL ||
	    bw->selection.bw->current_content == NULL)
		return NULL;

	return content_get_selection(bw->selection.bw->current_content);
}


/* exported interface, documented in netsurf/browser_window.h */
bool browser_window_can_search(struct browser_window *bw)
{
	if (bw == NULL || bw->current_content == NULL)
		return false;

	/** \todo We shouldn't have to know about specific content
	 * types here. There should be a content_is_searchable() call.
	 */
	if ((content_get_type(bw->current_content) != CONTENT_HTML) &&
	    (content_get_type(bw->current_content) != CONTENT_TEXTPLAIN)) {
		return false;
	}

	return true;
}


/* exported interface, documented in netsurf/browser_window.h */
bool browser_window_is_frameset(struct browser_window *bw)
{
	return (bw->children != NULL);
}


/* exported interface, documented in netsurf/browser_window.h */
nserror
browser_window_get_scrollbar_type(struct browser_window *bw,
				  browser_scrolling *h,
				  browser_scrolling *v)
{
	*h = bw->scrolling;
	*v = bw->scrolling;

	return NSERROR_OK;
}


/* exported interface, documented in netsurf/browser_window.h */
nserror
browser_window_get_features(struct browser_window *bw,
			    int x, int y,
			    struct browser_window_features *data)
{
	/* clear the features structure to empty values */
	data->link = NULL;
	data->link_title = NULL;
	data->link_title_length = 0;
	data->object = NULL;
	data->main = NULL;
	data->form_features = CTX_FORM_NONE;

	return browser_window__get_contextual_content(bw,
						      x / bw->scale,
						      y / bw->scale,
						      data);
}


/* exported interface, documented in netsurf/browser_window.h */
bool
browser_window_scroll_at_point(struct browser_window *bw,
			       int x, int y,
			       int scrx, int scry)
{
	return browser_window_scroll_at_point_internal(bw,
						       x / bw->scale,
						       y / bw->scale,
						       scrx,
						       scry);
}


/* exported interface, documented in netsurf/browser_window.h */
bool
browser_window_drop_file_at_point(struct browser_window *bw,
				  int x, int y,
				  char *file)
{
	return browser_window_drop_file_at_point_internal(bw,
							  x / bw->scale,
							  y / bw->scale,
							  file);
}


/* exported interface, documented in netsurf/browser_window.h */
void
browser_window_set_gadget_filename(struct browser_window *bw,
				   struct form_control *gadget,
				   const char *fn)
{
	html_set_file_gadget_filename(bw->current_content, gadget, fn);
}


/* exported interface, documented in netsurf/browser_window.h */
nserror
browser_window_debug_dump(struct browser_window *bw,
			  FILE *f,
			  enum content_debug op)
{
	if (bw->current_content != NULL) {
		return content_debug_dump(bw->current_content, f, op);
	}
	return NSERROR_OK;
}


/* exported interface, documented in netsurf/browser_window.h */
nserror browser_window_debug(struct browser_window *bw, enum content_debug op)
{
	if (bw->current_content != NULL) {
		return content_debug(bw->current_content, op);
	}
	return NSERROR_OK;
}


/* exported interface, documented in netsurf/browser_window.h */
nserror
browser_window_create(enum browser_window_create_flags flags,
		      nsurl *url,
		      nsurl *referrer,
		      struct browser_window *existing,
		      struct browser_window **bw)
{
	gui_window_create_flags gw_flags = GW_CREATE_NONE;
	struct browser_window *ret;
	nserror err;

	/* Check parameters */
	if (flags & BW_CREATE_CLONE) {
		if (existing == NULL) {
			assert(0 && "Failed: No existing window provided.");
			return NSERROR_BAD_PARAMETER;
		}
	}

	if (!(flags & BW_CREATE_HISTORY)) {
		if (!(flags & BW_CREATE_CLONE) || existing == NULL) {
			assert(0 && "Failed: Must have existing for history.");
			return NSERROR_BAD_PARAMETER;
		}
	}

	ret = calloc(1, sizeof(struct browser_window));
	if (ret == NULL) {
		return NSERROR_NOMEM;
	}

	/* Initialise common parts */
	err = browser_window_initialise_common(flags, ret, existing);
	if (err != NSERROR_OK) {
		browser_window_destroy(ret);
		return err;
	}

	/* window characteristics */
	ret->browser_window_type = BROWSER_WINDOW_NORMAL;
	ret->scrolling = BW_SCROLLING_YES;
	ret->border = true;
	ret->no_resize = true;
	ret->focus = ret;

	/* initialise last action with creation time */
	nsu_getmonotonic_ms(&ret->last_action);

	/* The existing gui_window is on the top-level existing
	 * browser_window. */
	existing = browser_window_get_root(existing);

	/* Set up gui_window creation flags */
	if (flags & BW_CREATE_TAB)
		gw_flags |= GW_CREATE_TAB;
	if (flags & BW_CREATE_CLONE)
		gw_flags |= GW_CREATE_CLONE;
	if (flags & BW_CREATE_FOREGROUND)
		gw_flags |= GW_CREATE_FOREGROUND;
	if (flags & BW_CREATE_FOCUS_LOCATION)
		gw_flags |= GW_CREATE_FOCUS_LOCATION;

	ret->window = guit->window->create(ret,
					   (existing != NULL) ? existing->window : NULL,
					   gw_flags);

	if (ret->window == NULL) {
		browser_window_destroy(ret);
		return NSERROR_BAD_PARAMETER;
	}

	if (url != NULL) {
		enum browser_window_nav_flags nav_flags;
		nav_flags = BW_NAVIGATE_NO_TERMINAL_HISTORY_UPDATE;
		if (flags & BW_CREATE_UNVERIFIABLE) {
			nav_flags |= BW_NAVIGATE_UNVERIFIABLE;
		}
		if (flags & BW_CREATE_HISTORY) {
			nav_flags |= BW_NAVIGATE_HISTORY;
		}
		browser_window_navigate(ret,
					url,
					referrer,
					nav_flags,
					NULL,
					NULL,
					NULL);
	}

	if (bw != NULL) {
		*bw = ret;
	}

	return NSERROR_OK;
}


/* exported internal interface, documented in desktop/browser_private.h */
nserror
browser_window_initialise_common(enum browser_window_create_flags flags,
				 struct browser_window *bw,
				 const struct browser_window *existing)
{
	nserror err;
	assert(bw);

	/* new javascript context for each window/(i)frame */
	err = js_newheap(nsoption_int(script_timeout), &bw->jsheap);
	if (err != NSERROR_OK)
		return err;

	if (flags & BW_CREATE_CLONE) {
		assert(existing != NULL);

		/* clone history */
		err = browser_window_history_clone(existing, bw);

		/* copy the scale */
		bw->scale = existing->scale;
	} else {
		/* create history */
		err = browser_window_history_create(bw);

		/* default scale */
		bw->scale = (float) nsoption_int(scale) / 100.0;
	}

	if (err != NSERROR_OK)
		return err;

	/* window characteristics */
	bw->refresh_interval = -1;

	bw->drag.type = DRAGGING_NONE;

	bw->scroll_x = NULL;
	bw->scroll_y = NULL;

	bw->focus = NULL;

	/* initialise status text cache */
	bw->status.text = NULL;
	bw->status.text_len = 0;
	bw->status.match = 0;
	bw->status.miss = 0;

	return NSERROR_OK;
}


/* exported interface, documented in netsurf/browser_window.h */
void browser_window_destroy(struct browser_window *bw)
{
	/* can't destoy child windows on their own */
	assert(!bw->parent);

	/* destroy */
	browser_window_destroy_internal(bw);
	free(bw);
}


/* exported interface, documented in netsurf/browser_window.h */
nserror browser_window_refresh_url_bar(struct browser_window *bw)
{
	nserror ret;
	nsurl *display_url, *url;

	assert(bw);

	if (bw->parent != NULL) {
		/* Not root window; don't set a URL in GUI URL bar */
		return NSERROR_OK;
	}

	if (bw->current_content == NULL) {
		/* no content so return about:blank */
		ret = browser_window_refresh_url_bar_internal(bw,
						corestring_nsurl_about_blank);
	} else if (bw->throbbing && bw->loading_parameters.url != NULL) {
		/* Throbbing and we have loading parameters, use those */
		url = bw->loading_parameters.url;
		ret = browser_window_refresh_url_bar_internal(bw, url);
	} else if (bw->frag_id == NULL) {
		if (bw->internal_nav) {
			url = bw->loading_parameters.url;
		} else {
			url = hlcache_handle_get_url(bw->current_content);
		}
		ret = browser_window_refresh_url_bar_internal(bw, url);
	} else {
		/* Combine URL and Fragment */
		if (bw->internal_nav) {
			url = bw->loading_parameters.url;
		} else {
			url = hlcache_handle_get_url(bw->current_content);
		}
		ret = nsurl_refragment(
				       url,
				       bw->frag_id, &display_url);
		if (ret == NSERROR_OK) {
			ret = browser_window_refresh_url_bar_internal(bw,
								display_url);
			nsurl_unref(display_url);
		}
	}

	return ret;
}


/* exported interface documented in netsurf/browser_window.h */
nserror
browser_window_navigate(struct browser_window *bw,
			nsurl *url,
			nsurl *referrer,
			enum browser_window_nav_flags flags,
			char *post_urlenc,
			struct fetch_multipart_data *post_multipart,
			hlcache_handle *parent)
{
	int depth = 0;
	struct browser_window *cur;
	uint32_t fetch_flags = 0;
	bool fetch_is_post = (post_urlenc != NULL || post_multipart != NULL);
	llcache_post_data post;
	hlcache_child_context child;
	nserror error;
	bool is_internal = false;
	struct browser_fetch_parameters params, *pass_params = NULL;

	assert(bw);
	assert(url);

	nslog_log(__FILE__, "", __LINE__, "bw %p, url %s", bw, nsurl_access(url));

	/*
	 * determine if navigation is internal url, if so, we do not
	 * do certain things during the load.
	 */
	is_internal = is_internal_navigate_url(url);

	if (is_internal &&
	    !(flags & BW_NAVIGATE_INTERNAL)) {
		/* Internal navigation detected, but flag not set, only allow
		 * this is there's a fetch multipart
		 */
		if (post_multipart == NULL) {
			return NSERROR_NEED_DATA;
		}
		/* It *is* internal, set it as such */
		flags |= BW_NAVIGATE_INTERNAL | BW_NAVIGATE_HISTORY;
		/* If we were previously internal, don't update again */
		if (bw->internal_nav) {
			flags |= BW_NAVIGATE_NO_TERMINAL_HISTORY_UPDATE;
		}
	}

	/* If we're navigating and we have a history entry and a content
	 * then update the history entry before we navigate to save our
	 * current state.  However since history navigation pre-moves
	 * the history state, we ensure that we only do this if we've not
	 * been suppressed.  In the suppressed case, the history code
	 * updates the history itself before navigating.
	 */
	if (bw->current_content != NULL &&
	    bw->history != NULL &&
	    bw->history->current != NULL &&
	    !is_internal &&
	    !(flags & BW_NAVIGATE_NO_TERMINAL_HISTORY_UPDATE)) {
		browser_window_history_update(bw, bw->current_content);
	}

	/* don't allow massively nested framesets */
	for (cur = bw; cur->parent; cur = cur->parent) {
		depth++;
	}
	if (depth > FRAME_DEPTH) {
		NSLOG(netsurf, INFO, "frame depth too high.");
		return NSERROR_FRAME_DEPTH;
	}

	/* Set up retrieval parameters */
	if (!(flags & BW_NAVIGATE_UNVERIFIABLE)) {
		fetch_flags |= LLCACHE_RETRIEVE_VERIFIABLE;
	}

	if (post_multipart != NULL) {
		post.type = LLCACHE_POST_MULTIPART;
		post.data.multipart = post_multipart;
	} else if (post_urlenc != NULL) {
		post.type = LLCACHE_POST_URL_ENCODED;
		post.data.urlenc = post_urlenc;
	}

	child.charset = content_get_encoding(parent, CONTENT_ENCODING_NORMAL);
	if ((parent != NULL) && (content_get_type(parent) == CONTENT_HTML)) {
		child.quirks = content_get_quirks(parent);
	} else {
		child.quirks = false;
	}

	url = nsurl_ref(url);

	if (referrer != NULL) {
		referrer = nsurl_ref(referrer);
	}

	/* Get download out of the way */
	if ((flags & BW_NAVIGATE_DOWNLOAD) != 0) {
		error = browser_window_download(bw,
						url,
						referrer,
						fetch_flags,
						fetch_is_post,
						&post);
		nsurl_unref(url);
		if (referrer != NULL) {
			nsurl_unref(referrer);
		}
		return error;
	}

	lwc_string_unref(bw->frag_id);
	bw->frag_id = NULL;

	if (nsurl_has_component(url, NSURL_FRAGMENT)) {
		bool same_url = false;

		bw->frag_id = nsurl_get_component(url, NSURL_FRAGMENT);

		/* Compare new URL with existing one (ignoring fragments) */
		if ((bw->current_content != NULL) &&
		    (hlcache_handle_get_url(bw->current_content) != NULL)) {
			same_url = nsurl_compare(
				url,
				hlcache_handle_get_url(bw->current_content),
				NSURL_COMPLETE);
		}

		/* if we're simply moving to another ID on the same page,
		 * don't bother to fetch, just update the window.
		 */
		if ((same_url) &&
		    (fetch_is_post == false) &&
		    (nsurl_has_component(url, NSURL_QUERY) == false)) {
			nsurl_unref(url);

			if (referrer != NULL) {
				nsurl_unref(referrer);
			}

			if ((flags & BW_NAVIGATE_HISTORY) != 0) {
				browser_window_history_add(bw,
							   bw->current_content,
							   bw->frag_id);
			}

			browser_window_update(bw, false);

			if (bw->current_content != NULL) {
				browser_window_refresh_url_bar(bw);
			}
			return NSERROR_OK;
		}
	}

	browser_window_stop(bw);
	browser_window_remove_caret(bw, false);
	browser_window_destroy_children(bw);
	browser_window_destroy_iframes(bw);

	/* Set up the fetch parameters */
	memset(&params, 0, sizeof(params));

	params.url = nsurl_ref(url);

	if (referrer != NULL) {
		params.referrer = nsurl_ref(referrer);
	}

	params.flags = flags;

	if (post_urlenc != NULL) {
		params.post_urlenc = strdup(post_urlenc);
	}

	if (post_multipart != NULL) {
		params.post_multipart = fetch_multipart_data_clone(post_multipart);
	}

	if (parent != NULL) {
		params.parent_charset = strdup(child.charset);
		params.parent_quirks = child.quirks;
	}

	bw->internal_nav = is_internal;

	if (is_internal) {
		pass_params = &params;
	} else {
		/* At this point, we're navigating, so store the fetch parameters */
		browser_window__free_fetch_parameters(&bw->loading_parameters);
		memcpy(&bw->loading_parameters, &params, sizeof(params));
		memset(&params, 0, sizeof(params));
		pass_params = &bw->loading_parameters;
	}

	error = browser_window__navigate_internal(bw, pass_params);

	nsurl_unref(url);

	if (referrer != NULL) {
		nsurl_unref(referrer);
	}

	if (is_internal) {
		browser_window__free_fetch_parameters(&params);
	}

	return error;
}


/**
 * Internal navigation handler for normal fetches
 */
static nserror
navigate_internal_real(struct browser_window *bw,
		       struct browser_fetch_parameters *params)
{
	uint32_t fetch_flags = 0;
	bool fetch_is_post;
	llcache_post_data post;
	hlcache_child_context child;
	nserror res;
	hlcache_handle *c;

	nslog_log(__FILE__, "", __LINE__, "Loading '%s'", nsurl_access(params->url));

	fetch_is_post = (params->post_urlenc != NULL || params->post_multipart != NULL);

	/* Clear SSL info for load */
	cert_chain_free(bw->loading_cert_chain);
	bw->loading_cert_chain = NULL;

	/* Set up retrieval parameters */
	if (!(params->flags & BW_NAVIGATE_UNVERIFIABLE)) {
		fetch_flags |= LLCACHE_RETRIEVE_VERIFIABLE;
	}

	if (params->post_multipart != NULL) {
		post.type = LLCACHE_POST_MULTIPART;
		post.data.multipart = params->post_multipart;
	} else if (params->post_urlenc != NULL) {
		post.type = LLCACHE_POST_URL_ENCODED;
		post.data.urlenc = params->post_urlenc;
	}

	if (params->parent_charset != NULL) {
		child.charset = params->parent_charset;
		child.quirks = params->parent_quirks;
	}

	browser_window_set_status(bw, messages_get("Loading"));
	bw->history_add = (params->flags & BW_NAVIGATE_HISTORY);

	/* Verifiable fetches may trigger a download */
	if (!(params->flags & BW_NAVIGATE_UNVERIFIABLE)) {
		fetch_flags |= HLCACHE_RETRIEVE_MAY_DOWNLOAD;
	}

	res = hlcache_handle_retrieve(params->url,
				      fetch_flags | HLCACHE_RETRIEVE_SNIFF_TYPE,
				      params->referrer,
				      fetch_is_post ? &post : NULL,
				      browser_window_callback,
				      bw,
				      params->parent_charset != NULL ? &child : NULL,
				      CONTENT_ANY,
				      &c);

	switch (res) {
	case NSERROR_OK:
		bw->loading_content = c;
		browser_window_start_throbber(bw);
		if (bw->window != NULL) {
			guit->window->set_icon(bw->window, NULL);
		}
		if (bw->internal_nav == false) {
			res = browser_window_refresh_url_bar_internal(bw,
								      params->url);
		}
		break;

	case NSERROR_NO_FETCH_HANDLER: /* no handler for this type */
		/** \todo does this always try and download even
		 * unverifiable content?
		 */
		res = guit->misc->launch_url(params->url);
		break;

	default: /* report error to user */
		browser_window_set_status(bw, messages_get_errorcode(res));
		break;

	}

	/* Record time */
	nsu_getmonotonic_ms(&bw->last_action);

	return res;
}


/**
 * Internal navigation handler for the authentication query handler
 *
 * If the parameters indicate we're processing a *response* from the handler
 * then we deal with that, otherwise we pass it on to the about: handler
 */
static nserror
navigate_internal_query_auth(struct browser_window *bw,
			     struct browser_fetch_parameters *params)
{
	char *userpass = NULL;
	const char *username, *password, *realm, *siteurl;
	nsurl *sitensurl;
	nserror res;
	bool is_login = false, is_cancel = false;

	assert(params->post_multipart != NULL);

	is_login = fetch_multipart_data_find(params->post_multipart, "login") != NULL;
	is_cancel = fetch_multipart_data_find(params->post_multipart, "cancel") != NULL;

	if (!(is_login || is_cancel)) {
		/* This is a request, so pass it on */
		return navigate_internal_real(bw, params);
	}

	if (is_cancel) {
		/* We're processing a cancel, do a rough-and-ready nav to
		 * about:blank
		 */
		browser_window__free_fetch_parameters(&bw->loading_parameters);
		bw->loading_parameters.url = nsurl_ref(corestring_nsurl_about_blank);
		bw->loading_parameters.flags = BW_NAVIGATE_NO_TERMINAL_HISTORY_UPDATE | BW_NAVIGATE_INTERNAL;
		bw->internal_nav = true;
		return browser_window__navigate_internal(bw, &bw->loading_parameters);
	}

	/* We're processing a "login" attempt from the form */

	/* Retrieve the data */
	username = fetch_multipart_data_find(params->post_multipart, "username");
	password = fetch_multipart_data_find(params->post_multipart, "password");
	realm = fetch_multipart_data_find(params->post_multipart, "realm");
	siteurl = fetch_multipart_data_find(params->post_multipart, "siteurl");

	if (username == NULL || password == NULL ||
	    realm == NULL || siteurl == NULL) {
		/* Bad inputs, simply fail */
		return NSERROR_INVALID;
	}

	/* Parse the URL */
	res = nsurl_create(siteurl, &sitensurl);
	if (res != NSERROR_OK) {
		return res;
	}

	/* Construct the username/password */
	res = browser_window__build_userpass(username, password, &userpass);
	if (res != NSERROR_OK) {
		nsurl_unref(sitensurl);
		return res;
	}

	/* And let urldb know */
	urldb_set_auth_details(sitensurl, realm, userpass);

	/* Clean up */
	free(userpass);
	nsurl_unref(sitensurl);

	/* Finally navigate to the original loading parameters */
	bw->internal_nav = false;
	return navigate_internal_real(bw, &bw->loading_parameters);
}


/**
 * Internal navigation handler for the SSL/privacy query page.
 *
 * If the parameters indicate we're processing a *response* from the handler
 * then we deal with that, otherwise we pass it on to the about: handler
 */
static nserror
navigate_internal_query_ssl(struct browser_window *bw,
			    struct browser_fetch_parameters *params)
{
	bool is_proceed = false, is_back = false;
	const char *siteurl = NULL;
	nsurl *siteurl_ns;

	assert(params->post_multipart != NULL);

	is_proceed = fetch_multipart_data_find(params->post_multipart, "proceed") != NULL;
	is_back = fetch_multipart_data_find(params->post_multipart, "back") != NULL;
	siteurl = fetch_multipart_data_find(params->post_multipart, "siteurl");

	if (!(is_proceed || is_back) || siteurl == NULL) {
		/* This is a request, so pass it on */
		return navigate_internal_real(bw, params);
	}

	if (nsurl_create(siteurl, &siteurl_ns) != NSERROR_OK) {
		NSLOG(netsurf, ERROR, "Unable to reset ssl loading parameters");
	} else {
		/* In order that we may proceed, replace the loading parameters */
		nsurl_unref(bw->loading_parameters.url);
		bw->loading_parameters.url = siteurl_ns;
	}

	return browser_window__handle_ssl_query_response(is_proceed, bw);
}


/**
 * Internal navigation handler for the timeout query page.
 *
 * If the parameters indicate we're processing a *response* from the handler
 * then we deal with that, otherwise we pass it on to the about: handler
 */
static nserror
navigate_internal_query_timeout(struct browser_window *bw,
				struct browser_fetch_parameters *params)
{
	bool is_retry = false, is_back = false;

	nslog_log(__FILE__, "", __LINE__, "bw:%p params:%p", bw, params);

	assert(params->post_multipart != NULL);

	is_retry = fetch_multipart_data_find(params->post_multipart, "retry") != NULL;
	is_back = fetch_multipart_data_find(params->post_multipart, "back") != NULL;

	if (is_back) {
		/* do a rough-and-ready nav to the old 'current'
		 * parameters, with any post data stripped away
		 */
		return browser_window__reload_current_parameters(bw);
	}

	if (is_retry) {
		/* Finally navigate to the original loading parameters */
		bw->internal_nav = false;
		return navigate_internal_real(bw, &bw->loading_parameters);
	}

	return navigate_internal_real(bw, params);
}


/**
 * Internal navigation handler for the fetch error query page.
 *
 * If the parameters indicate we're processing a *response* from the handler
 * then we deal with that, otherwise we pass it on to the about: handler
 */
static nserror
navigate_internal_query_fetcherror(struct browser_window *bw,
				   struct browser_fetch_parameters *params)
{
	bool is_retry = false, is_back = false;

	nslog_log(__FILE__, "", __LINE__, "bw:%p params:%p", bw, params);

	assert(params->post_multipart != NULL);

	is_retry = fetch_multipart_data_find(params->post_multipart, "retry") != NULL;
	is_back = fetch_multipart_data_find(params->post_multipart, "back") != NULL;

	if (is_back) {
		/* do a rough-and-ready nav to the old 'current'
		 * parameters, with any post data stripped away
		 */
		return browser_window__reload_current_parameters(bw);
	}

	if (is_retry) {
		/* Finally navigate to the original loading parameters */
		bw->internal_nav = false;
		return navigate_internal_real(bw, &bw->loading_parameters);
	}

	return navigate_internal_real(bw, params);
}


/**
 * dispatch to internal query handlers or normal navigation
 *
 * Here we determine if we're navigating to an internal query URI and
 * if so, what we need to do about it.
 *
 * \note these check must match those in is_internal_navigate_url()
 *
 * If we're not, then we just move on to the real navigate.
 */
nserror
browser_window__navigate_internal(struct browser_window *bw,
				  struct browser_fetch_parameters *params)
{
	lwc_string *scheme, *path;

	/* All our special URIs are in the about: scheme */
	scheme = nsurl_get_component(params->url, NSURL_SCHEME);
	if (scheme != corestring_lwc_about) {
		lwc_string_unref(scheme);
		goto normal_fetch;
	}
	lwc_string_unref(scheme);

	/* Is it the auth query handler? */
	path = nsurl_get_component(params->url, NSURL_PATH);
	if (path == corestring_lwc_query_auth) {
		lwc_string_unref(path);
		return navigate_internal_query_auth(bw, params);
	}
	if (path == corestring_lwc_query_ssl) {
		lwc_string_unref(path);
		return navigate_internal_query_ssl(bw, params);
	}
	if (path == corestring_lwc_query_timeout) {
		lwc_string_unref(path);
		return navigate_internal_query_timeout(bw, params);
	}
	if (path == corestring_lwc_query_fetcherror) {
		lwc_string_unref(path);
		return navigate_internal_query_fetcherror(bw, params);
	}
	lwc_string_unref(path);

	/* Fall through to a normal about: fetch */

 normal_fetch:
	return navigate_internal_real(bw, params);
}


/* Exported interface, documented in netsurf/browser_window.h */
bool browser_window_up_available(struct browser_window *bw)
{
	bool result = false;

	if (bw != NULL && bw->current_content != NULL) {
		nsurl *parent;
		nserror	err;
		err = nsurl_parent(hlcache_handle_get_url(bw->current_content),
				   &parent);
		if (err == NSERROR_OK) {
			result = nsurl_compare(hlcache_handle_get_url(
						      bw->current_content),
					       parent,
					       NSURL_COMPLETE) == false;
			nsurl_unref(parent);
		}
	}

	return result;
}


/* Exported interface, documented in netsurf/browser_window.h */
nserror browser_window_navigate_up(struct browser_window *bw, bool new_window)
{
	nsurl *current, *parent;
	nserror err;

	if (bw == NULL)
		return NSERROR_BAD_PARAMETER;

	current = browser_window_access_url(bw);

	err = nsurl_parent(current, &parent);
	if (err != NSERROR_OK) {
		return err;
	}

	if (nsurl_compare(current, parent, NSURL_COMPLETE) == true) {
		/* Can't go up to parent from here */
		nsurl_unref(parent);
		return NSERROR_OK;
	}

	if (new_window) {
		err = browser_window_create(BW_CREATE_CLONE,
					    parent, NULL, bw, NULL);
	} else {
		err = browser_window_navigate(bw, parent, NULL,
					      BW_NAVIGATE_HISTORY,
					      NULL, NULL, NULL);
	}

	nsurl_unref(parent);
	return err;
}


/* Exported interface, documented in include/netsurf/browser_window.h */
nsurl* browser_window_access_url(const struct browser_window *bw)
{
	assert(bw != NULL);

	if (bw->current_content != NULL) {
		return hlcache_handle_get_url(bw->current_content);

	} else if (bw->loading_content != NULL) {
		/* TODO: should we return this? */
		return hlcache_handle_get_url(bw->loading_content);
	}

	return corestring_nsurl_about_blank;
}


/* Exported interface, documented in include/netsurf/browser_window.h */
nserror
browser_window_get_url(struct browser_window *bw, bool fragment,nsurl** url_out)
{
	nserror err;
	nsurl *url;

	assert(bw != NULL);

	if (!fragment || bw->frag_id == NULL || bw->loading_content != NULL) {
		/* If there's a loading content, then the bw->frag_id will have
		 * been trampled, possibly with a new frag_id, but we will
		 * still be returning the current URL, so in this edge case
		 * we just drop any fragment. */
		url = nsurl_ref(browser_window_access_url(bw));

	} else {
		err = nsurl_refragment(browser_window_access_url(bw),
				       bw->frag_id, &url);
		if (err != NSERROR_OK) {
			return err;
		}
	}

	*url_out = url;
	return NSERROR_OK;
}


/* Exported interface, documented in netsurf/browser_window.h */
const char* browser_window_get_title(struct browser_window *bw)
{
	assert(bw != NULL);

	if (bw->current_content != NULL) {
		return content_get_title(bw->current_content);
	}

	/* no content so return about:blank */
	return nsurl_access(corestring_nsurl_about_blank);
}


/* Exported interface, documented in netsurf/browser_window.h */
struct history * browser_window_get_history(struct browser_window *bw)
{
	assert(bw != NULL);

	return bw->history;
}


/* Exported interface, documented in netsurf/browser_window.h */
bool browser_window_has_content(struct browser_window *bw)
{
	assert(bw != NULL);

	if (bw->current_content == NULL) {
		return false;
	}

	return true;
}


/* Exported interface, documented in netsurf/browser_window.h */
struct hlcache_handle *browser_window_get_content(struct browser_window *bw)
{
	return bw->current_content;
}


/* Exported interface, documented in netsurf/browser_window.h */
nserror browser_window_get_extents(struct browser_window *bw, bool scaled,
				   int *width, int *height)
{
	assert(bw != NULL);

	if (bw->current_content == NULL) {
		*width = 0;
		*height = 0;
		return NSERROR_BAD_CONTENT;
	}

	*width = content_get_width(bw->current_content);
	*height = content_get_height(bw->current_content);

	if (scaled) {
		*width *= bw->scale;
		*height *= bw->scale;
	}

	return NSERROR_OK;
}


/* exported internal interface, documented in desktop/browser_private.h */
nserror
browser_window_get_dimensions(struct browser_window *bw,
			      int *width,
			      int *height)
{
	nserror res;
	assert(bw);

	if (bw->window == NULL) {
		/* Core managed browser window */
		*width = bw->width;
		*height = bw->height;
		res = NSERROR_OK;
	} else {
		/* Front end window */
		res = guit->window->get_dimensions(bw->window, width, height);
	}
	return res;
}


/* Exported interface, documented in netsurf/browser_window.h */
void
browser_window_set_dimensions(struct browser_window *bw, int width, int height)
{
	assert(bw);

	if (bw->window == NULL) {
		/* Core managed browser window */
		bw->width = width;
		bw->height = height;
	} else {
		NSLOG(netsurf, INFO,
		      "Asked to set dimensions of front end window.");
		assert(0);
	}
}


/* Exported interface, documented in browser/browser_private.h */
nserror
browser_window_invalidate_rect(struct browser_window *bw, struct rect *rect)
{
	int pos_x;
	int pos_y;
	struct browser_window *top = bw;

	assert(bw);

	if (bw->window == NULL) {
		/* Core managed browser window */
		browser_window_get_position(bw, true, &pos_x, &pos_y);

		top = browser_window_get_root(bw);

		rect->x0 += pos_x / bw->scale;
		rect->y0 += pos_y / bw->scale;
		rect->x1 += pos_x / bw->scale;
		rect->y1 += pos_y / bw->scale;
	}

	rect->x0 *= top->scale;
	rect->y0 *= top->scale;
	rect->x1 *= top->scale;
	rect->y1 *= top->scale;

	return guit->window->invalidate(top->window, rect);
}


/* Exported interface, documented in netsurf/browser_window.h */
void browser_window_stop(struct browser_window *bw)
{
	int children, index;

	if (bw->loading_content != NULL) {
		hlcache_handle_abort(bw->loading_content);
		hlcache_handle_release(bw->loading_content);
		bw->loading_content = NULL;
	}

	if (bw->current_content != NULL &&
	    content_get_status(bw->current_content) != CONTENT_STATUS_DONE) {
		nserror error;
		assert(content_get_status(bw->current_content) ==
		       CONTENT_STATUS_READY);
		error = hlcache_handle_abort(bw->current_content);
		assert(error == NSERROR_OK);
	}

	guit->misc->schedule(-1, browser_window_refresh, bw);

	if (bw->children) {
		children = bw->rows * bw->cols;
		for (index = 0; index < children; index++)
			browser_window_stop(&bw->children[index]);
	}
	if (bw->iframes) {
		children = bw->iframe_count;
		for (index = 0; index < children; index++)
			browser_window_stop(&bw->iframes[index]);
	}

	if (bw->current_content != NULL) {
		browser_window_refresh_url_bar(bw);
	}

	browser_window_stop_throbber(bw);
}


/* Exported interface, documented in netsurf/browser_window.h */
nserror browser_window_reload(struct browser_window *bw, bool all)
{
	hlcache_handle *c;
	unsigned int i;
	struct nsurl *reload_url;

	if ((bw->current_content) == NULL ||
	    (bw->loading_content) != NULL) {
		return NSERROR_INVALID;
	}

	if (all && content_get_type(bw->current_content) == CONTENT_HTML) {
		struct html_stylesheet *sheets;
		struct content_html_object *object;
		unsigned int count;

		c = bw->current_content;

		/* invalidate objects */
		object = html_get_objects(c, &count);

		for (; object != NULL; object = object->next) {
			if (object->content != NULL)
				content_invalidate_reuse_data(object->content);
		}

		/* invalidate stylesheets */
		sheets = html_get_stylesheets(c, &count);

		for (i = STYLESHEET_START; i != count; i++) {
			if (sheets[i].sheet != NULL) {
				content_invalidate_reuse_data(sheets[i].sheet);
			}
		}
	}

	content_invalidate_reuse_data(bw->current_content);

	reload_url = hlcache_handle_get_url(bw->current_content);

	return browser_window_navigate(bw,
				       reload_url,
				       NULL,
				       BW_NAVIGATE_NONE,
				       NULL,
				       NULL,
				       NULL);
}


/* Exported interface, documented in netsurf/browser_window.h */
void browser_window_set_status(struct browser_window *bw, const char *text)
{
	int text_len;
	/* find topmost window */
	while (bw->parent)
		bw = bw->parent;

	if ((bw->status.text != NULL) &&
	    (strcmp(text, bw->status.text) == 0)) {
		/* status text is unchanged */
		bw->status.match++;
		return;
	}

	/* status text is changed */

	text_len = strlen(text);

	if ((bw->status.text == NULL) || (bw->status.text_len < text_len)) {
		/* no current string allocation or it is not long enough */
		free(bw->status.text);
		bw->status.text = strdup(text);
		bw->status.text_len = text_len;
	} else {
		/* current allocation has enough space */
		memcpy(bw->status.text, text, text_len + 1);
	}

	bw->status.miss++;
	guit->window->set_status(bw->window, bw->status.text);
}


/* Exported interface, documented in netsurf/browser_window.h */
void browser_window_set_pointer(struct browser_window *bw,
				browser_pointer_shape shape)
{
	struct browser_window *root = browser_window_get_root(bw);
	gui_pointer_shape gui_shape;
	bool loading;
	uint64_t ms_now;

	assert(root);
	assert(root->window);

	loading = ((bw->loading_content != NULL) ||
		   ((bw->current_content != NULL) &&
		    (content_get_status(bw->current_content) == CONTENT_STATUS_READY)));

	nsu_getmonotonic_ms(&ms_now);

	if (loading && ((ms_now - bw->last_action) < 1000)) {
		/* If loading and less than 1 second since last link followed,
		 * force progress indicator pointer */
		gui_shape = GUI_POINTER_PROGRESS;

	} else if (shape == BROWSER_POINTER_AUTO) {
		/* Up to browser window to decide */
		if (loading) {
			gui_shape = GUI_POINTER_PROGRESS;
		} else {
			gui_shape = GUI_POINTER_DEFAULT;
		}

	} else {
		/* Use what we were told */
		gui_shape = (gui_pointer_shape)shape;
	}

	guit->window->set_pointer(root->window, gui_shape);
}


/* exported function documented in netsurf/browser_window.h */
nserror browser_window_schedule_reformat(struct browser_window *bw)
{
	if (bw->window == NULL) {
		return NSERROR_BAD_PARAMETER;
	}

	MS_LOG("browser_window_schedule_reformat: entry");
	/* fixes240 — 50ms coalescing delay (was 0). The scheduler dedups
	 * pending entries with the same callback+param, but at delay=0 each
	 * entry fires within one poll cycle, so resource-arrival bursts
	 * during page load produce one reformat per resource. A short
	 * delay lets bursts coalesce into a single reformat at the burst
	 * tail. Measured: ~44 reformats * 250 ms = ~11 s of redundant box-
	 * tree walks on a cold mactrove load. Pre-fixes240 perceived speed
	 * was bottlenecked here. 50 ms is short enough that interactive
	 * actions (hover, click) still feel immediate.
	 *
	 * Cross-platform note: this change is generic NetSurf core but
	 * benefits any frontend whose scheduler dedups. RISC OS, Atari,
	 * Amiga all use the same dedup-on-insert pattern. */
	return guit->misc->schedule(50, scheduled_reformat, bw);
}


/* exported function documented in netsurf/browser_window.h */
void
browser_window_reformat(struct browser_window *bw,
			bool background,
			int width,
			int height)
{
	hlcache_handle *c = bw->current_content;

	if (c == NULL)
		return;

	if (bw->browser_window_type != BROWSER_WINDOW_IFRAME) {
		/* Iframe dimensions are already scaled in parent's layout */
		width  /= bw->scale;
		height /= bw->scale;
	}

	if (bw->window == NULL) {
		/* Core managed browser window; subtract scrollbar width */
		width  -= bw->scroll_y ? SCROLLBAR_WIDTH : 0;
		height -= bw->scroll_x ? SCROLLBAR_WIDTH : 0;

		width  = width  > 0 ? width  : 0;
		height = height > 0 ? height : 0;
	}

	content_reformat(c, background, width, height);
}


/* exported interface documented in netsurf/browser_window.h */
nserror
browser_window_set_scale(struct browser_window *bw, float scale, bool absolute)
{
	nserror res;

	/* get top browser window */
	while (bw->parent) {
		bw = bw->parent;
	}

	if (!absolute) {
		/* snap small values around 1.0 */
		if ((scale + bw->scale) > (1.01 - scale) &&
		    (scale + bw->scale) < (0.99 + scale)) {
			scale = 1.0;
		} else {
			scale += bw->scale;
		}
	}

	/* clamp range between 0.1 and 10 (10% and 1000%) */
	if (scale < SCALE_MINIMUM) {
		scale = SCALE_MINIMUM;
	} else if (scale > SCALE_MAXIMUM) {
		scale = SCALE_MAXIMUM;
	}

	res = browser_window_set_scale_internal(bw, scale);
	if (res == NSERROR_OK) {
		browser_window_recalculate_frameset(bw);
	}

	return res;
}


/* exported interface documented in netsurf/browser_window.h */
float browser_window_get_scale(struct browser_window *bw)
{
	if (bw == NULL) {
		return 1.0;
	}

	return bw->scale;
}


/* exported interface documented in netsurf/browser_window.h */
struct browser_window *
browser_window_find_target(struct browser_window *bw,
			   const char *target,
			   browser_mouse_state mouse)
{
	struct browser_window *bw_target;
	struct browser_window *top;
	hlcache_handle *c;
	int rdepth;
	nserror error;
	int flags = BW_CREATE_HISTORY | BW_CREATE_CLONE;

	if (nsoption_bool(foreground_new))
		flags |= BW_CREATE_FOREGROUND;

	/* use the base target if we don't have one */
	c = bw->current_content;
	if (target == NULL &&
	    c != NULL &&
	    content_get_type(c) == CONTENT_HTML) {
		target = html_get_base_target(c);
	}
	if (target == NULL) {
		target = "_self";
	}

	/* allow the simple case of target="_blank" to be ignored if requested
	 */
	if ((!(mouse & BROWSER_MOUSE_CLICK_2)) &&
	    (!((mouse & BROWSER_MOUSE_CLICK_2) &&
	       (mouse & BROWSER_MOUSE_MOD_2))) &&
	    (!nsoption_bool(target_blank))) {
		/* not a mouse button 2 click
		 * not a mouse button 1 click with ctrl pressed
		 * configured to ignore target="_blank" */
		if (!strcasecmp(target, "_blank"))
			return bw;
	}

	/* handle reserved keywords */
	if (((nsoption_bool(button_2_tab)) &&
	     (mouse & BROWSER_MOUSE_CLICK_2))||
	    ((!nsoption_bool(button_2_tab)) &&
	     ((mouse & BROWSER_MOUSE_CLICK_1) &&
	      (mouse & BROWSER_MOUSE_MOD_2))) ||
	    ((nsoption_bool(button_2_tab)) &&
	     (!strcasecmp(target, "_blank")))) {
		/* open in new tab if:
		 * - button_2 opens in new tab and button_2 was pressed
		 * OR
		 * - button_2 doesn't open in new tabs and button_1 was
		 *   pressed with ctrl held
		 * OR
		 * - button_2 opens in new tab and the link target is "_blank"
		 */
		flags |= BW_CREATE_TAB;
		error = browser_window_create(flags, NULL, NULL, bw, &bw_target);
		if (error != NSERROR_OK) {
			return bw;
		}
		return bw_target;
	} else if (((!nsoption_bool(button_2_tab)) &&
		    (mouse & BROWSER_MOUSE_CLICK_2)) ||
		   ((nsoption_bool(button_2_tab)) &&
		    ((mouse & BROWSER_MOUSE_CLICK_1) &&
		     (mouse & BROWSER_MOUSE_MOD_2))) ||
		   ((!nsoption_bool(button_2_tab)) &&
		    (!strcasecmp(target, "_blank")))) {
		/* open in new window if:
		 * - button_2 doesn't open in new tabs and button_2 was pressed
		 * OR
		 * - button_2 opens in new tab and button_1 was pressed with
		 *   ctrl held
		 * OR
		 * - button_2 doesn't open in new tabs and the link target is
		 *   "_blank"
		 */
		error = browser_window_create(flags, NULL, NULL, bw, &bw_target);
		if (error != NSERROR_OK) {
			return bw;
		}
		return bw_target;
	} else if (!strcasecmp(target, "_self")) {
		return bw;
	} else if (!strcasecmp(target, "_parent")) {
		if (bw->parent)
			return bw->parent;
		return bw;
	} else if (!strcasecmp(target, "_top")) {
		while (bw->parent)
			bw = bw->parent;
		return bw;
	}

	/* find frame according to B.8, ie using the following priorities:
	 *
	 *  1) current frame
	 *  2) closest to front
	 */
	rdepth = -1;
	bw_target = NULL;
	for (top = bw; top->parent; top = top->parent);
	browser_window_find_target_internal(top, target, 0, bw, &rdepth,
					    &bw_target);
	if (bw_target)
		return bw_target;

	/* we require a new window using the target name */
	if (!nsoption_bool(target_blank))
		return bw;

	error = browser_window_create(flags, NULL, NULL, bw, &bw_target);
	if (error != NSERROR_OK) {
		return bw;
	}

	/* frame names should begin with an alphabetic character (a-z,A-Z),
	 * however in practice you get things such as '_new' and '2left'. The
	 * only real effect this has is when giving out names as it can be
	 * assumed that an author intended '_new' to create a new nameless
	 * window (ie '_blank') whereas in the case of '2left' the intention
	 * was for a new named window. As such we merely special case windows
	 * that begin with an underscore. */
	if (target[0] != '_') {
		bw_target->name = strdup(target);
	}
	return bw_target;
}


/* exported interface documented in netsurf/browser_window.h */
void
browser_window_mouse_track(struct browser_window *bw,
			   browser_mouse_state mouse,
			   int x, int y)
{
	browser_window_mouse_track_internal(bw,
					    mouse,
					    x / bw->scale,
					    y / bw->scale);
}

/* exported interface documented in netsurf/browser_window.h */
void
browser_window_mouse_click(struct browser_window *bw,
			   browser_mouse_state mouse,
			   int x, int y)
{
	browser_window_mouse_click_internal(bw,
					    mouse,
					    x / bw->scale,
					    y / bw->scale);
}


/* exported interface documented in netsurf/browser_window.h */
void browser_window_page_drag_start(struct browser_window *bw, int x, int y)
{
	assert(bw != NULL);

	browser_window_set_drag_type(bw, DRAGGING_PAGE_SCROLL, NULL);

	bw->drag.start_x = x;
	bw->drag.start_y = y;

	if (bw->window != NULL) {
		/* Front end window */
		guit->window->get_scroll(bw->window,
					 &bw->drag.start_scroll_x,
					 &bw->drag.start_scroll_y);

		guit->window->event(bw->window, GW_EVENT_SCROLL_START);
	} else {
		/* Core managed browser window */
		bw->drag.start_scroll_x = scrollbar_get_offset(bw->scroll_x);
		bw->drag.start_scroll_y = scrollbar_get_offset(bw->scroll_y);
	}
}


/* exported interface documented in netsurf/browser_window.h */
bool browser_window_back_available(struct browser_window *bw)
{
	if (bw != NULL && bw->internal_nav) {
		/* Internal nav, back is possible */
		return true;
	}
	return (bw && bw->history && browser_window_history_back_available(bw));
}


/* exported interface documented in netsurf/browser_window.h */
bool browser_window_forward_available(struct browser_window *bw)
{
	return (bw && bw->history && browser_window_history_forward_available(bw));
}

/* exported interface documented in netsurf/browser_window.h */
bool browser_window_reload_available(struct browser_window *bw)
{
	return (bw && bw->current_content && !bw->loading_content);
}


/* exported interface documented in netsurf/browser_window.h */
bool browser_window_stop_available(struct browser_window *bw)
{
	return (bw && (bw->loading_content ||
		       (bw->current_content &&
			(content_get_status(bw->current_content) !=
			 CONTENT_STATUS_DONE))));
}

/* exported interface documented in netsurf/browser_window.h */
bool
browser_window_exec(struct browser_window *bw, const char *src, size_t srclen)
{
	assert(bw != NULL);

	if (!bw->current_content) {
		NSLOG(netsurf, DEEPDEBUG, "Unable to exec, no content");
		return false;
	}

	if (content_get_status(bw->current_content) != CONTENT_STATUS_DONE) {
		NSLOG(netsurf, DEEPDEBUG, "Unable to exec, content not done");
		return false;
	}

	/* Okay it should be safe, forward the request through to the content
	 * itself.  Only HTML contents currently support executing code
	 */
	return content_exec(bw->current_content, src, srclen);
}


/* exported interface documented in browser_window.h */
nserror
browser_window_console_log(struct browser_window *bw,
			   browser_window_console_source src,
			   const char *msg,
			   size_t msglen,
			   browser_window_console_flags flags)
{
	browser_window_console_flags log_level = flags & BW_CS_FLAG_LEVEL_MASK;
	struct browser_window *root = browser_window_get_root(bw);

	assert(msg != NULL);
	/* We don't assert msglen > 0, if someone wants to log a real empty
	 * string then we won't stop them.  It does sometimes happen from
	 * JavaScript for example.
	 */

	/* bw is the target of the log, but root is where we log it */

	nslog_log(__FILE__, "", __LINE__, "Logging message in %p targetted at %p", root, bw);
	nslog_log(__FILE__, "", __LINE__,
		      "Log came from %s",
		      ((src == BW_CS_INPUT) ? "user input" :
	       (src == BW_CS_SCRIPT_ERROR) ? "script error" :
	       (src == BW_CS_SCRIPT_CONSOLE) ? "script console" :
	       "unknown input location"));

	switch (log_level) {
	case BW_CS_FLAG_LEVEL_DEBUG:
		nslog_log(__FILE__, "", __LINE__, "%.*s", (int)msglen, msg);
		break;
	case BW_CS_FLAG_LEVEL_LOG:
		nslog_log(__FILE__, "", __LINE__, "%.*s", (int)msglen, msg);
		break;
	case BW_CS_FLAG_LEVEL_INFO:
		nslog_log(__FILE__, "", __LINE__, "%.*s", (int)msglen, msg);
		break;
	case BW_CS_FLAG_LEVEL_WARN:
		nslog_log(__FILE__, "", __LINE__, "%.*s", (int)msglen, msg);
		break;
	case BW_CS_FLAG_LEVEL_ERROR:
		nslog_log(__FILE__, "", __LINE__, "%.*s", (int)msglen, msg);
		break;
	default:
		/* Unreachable */
		break;
	}

	guit->window->console_log(root->window, src, msg, msglen, flags);

	return NSERROR_OK;
}


/* Exported interface, documented in browser_private.h */
nserror
browser_window__reload_current_parameters(struct browser_window *bw)
{
	assert(bw != NULL);

	if (bw->current_parameters.post_urlenc != NULL) {
		free(bw->current_parameters.post_urlenc);
		bw->current_parameters.post_urlenc = NULL;
	}

	if (bw->current_parameters.post_multipart != NULL) {
		fetch_multipart_data_destroy(bw->current_parameters.post_multipart);
		bw->current_parameters.post_multipart = NULL;
	}

	if (bw->current_parameters.url == NULL) {
		/* We have never navigated so go to about:blank */
		bw->current_parameters.url = nsurl_ref(corestring_nsurl_about_blank);
	}

	bw->current_parameters.flags &= ~BW_NAVIGATE_HISTORY;
	bw->internal_nav = false;

	browser_window__free_fetch_parameters(&bw->loading_parameters);
	memcpy(&bw->loading_parameters, &bw->current_parameters, sizeof(bw->loading_parameters));
	memset(&bw->current_parameters, 0, sizeof(bw->current_parameters));
	return browser_window__navigate_internal(bw, &bw->loading_parameters);
}

/* Exported interface, documented in browser_window.h */
browser_window_page_info_state browser_window_get_page_info_state(
		const struct browser_window *bw)
{
	lwc_string *scheme;
	unsigned char match;

	assert(bw != NULL);

	/* Do we have any content?  If not -- UNKNOWN */
	if (bw->current_content == NULL) {
		return PAGE_STATE_UNKNOWN;
	}

	scheme = nsurl_get_component(
		hlcache_handle_get_url(bw->current_content), NSURL_SCHEME);

	/* Is this an internal scheme? */
	if ((lwc_string_isequal(scheme, corestring_lwc_about,
				&match) == lwc_error_ok &&
	     (match == true)) ||
	    (lwc_string_isequal(scheme, corestring_lwc_data,
				&match) == lwc_error_ok &&
	     (match == true)) ||
	    (lwc_string_isequal(scheme, corestring_lwc_resource,
				&match) == lwc_error_ok &&
	     (match == true))) {
		lwc_string_unref(scheme);
		return PAGE_STATE_INTERNAL;
	}

	/* Is this file:/// ? */
	if (lwc_string_isequal(scheme, corestring_lwc_file,
			       &match) == lwc_error_ok &&
	    match == true) {
		lwc_string_unref(scheme);
		return PAGE_STATE_LOCAL;
	}

	/* If not https, from here on down that'd be insecure */
	if ((lwc_string_isequal(scheme, corestring_lwc_https,
			&match) == lwc_error_ok &&
			(match == false))) {
		/* Some remote content, not https, therefore insecure */
		lwc_string_unref(scheme);
		return PAGE_STATE_INSECURE;
	}

	lwc_string_unref(scheme);

	/* Did we have to override this SSL setting? */
	if (urldb_get_cert_permissions(hlcache_handle_get_url(bw->current_content))) {
		return PAGE_STATE_SECURE_OVERRIDE;
	}

	/* If we've seen insecure content internally then we need to say so */
	if (content_saw_insecure_objects(bw->current_content)) {
		return PAGE_STATE_SECURE_ISSUES;
	}

	/* All is well, return secure state */
	return PAGE_STATE_SECURE;
}

/* Exported interface, documented in browser_window.h */
nserror
browser_window_get_ssl_chain(struct browser_window *bw,
			     struct cert_chain **chain)
{
	assert(bw != NULL);

	if (bw->current_cert_chain == NULL) {
		return NSERROR_NOT_FOUND;
	}

	*chain = bw->current_cert_chain;

	return NSERROR_OK;
}

/* Exported interface, documented in browser_window.h */
int browser_window_get_cookie_count(
		const struct browser_window *bw)
{
	int count = 0;
	char *cookies = urldb_get_cookie(browser_window_access_url(bw), true);
	if (cookies == NULL) {
		return 0;
	}

	{
		char *c;
		for (c = cookies; *c != '\0'; c++) {
			if (*c == ';')
				count++;
		}
	}

	free(cookies);

	return count;
}

/* Exported interface, documented in browser_window.h */
nserror browser_window_show_cookies(
		const struct browser_window *bw)
{
	nserror err;
	nsurl *url = browser_window_access_url(bw);
	lwc_string *host = nsurl_get_component(url, NSURL_HOST);
	const char *string = (host != NULL) ? lwc_string_data(host) : NULL;

	err = guit->misc->present_cookies(string);

	lwc_string_unref(host);
	return err;
}

/* Exported interface, documented in browser_window.h */
nserror browser_window_show_certificates(struct browser_window *bw)
{
	nserror res;
	nsurl *url;

	if (bw->current_cert_chain == NULL) {
		return NSERROR_NOT_FOUND;
	}

	res = cert_chain_to_query(bw->current_cert_chain, &url);
	if (res == NSERROR_OK) {
		res = browser_window_create(BW_CREATE_HISTORY |
					    BW_CREATE_FOREGROUND |
					    BW_CREATE_TAB,
					    url,
					    NULL,
					    bw,
					    NULL);

		nsurl_unref(url);
	}

	return res;
}
