/*
 * Copyright 2004 James Bursa <bursa@users.sourceforge.net>
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
 * Interface to text/html content handler.
 *
 * These functions should in general be called via the content interface.
 */

#ifndef NETSURF_HTML_HTML_H
#define NETSURF_HTML_HTML_H

#include <stdbool.h>

#include "netsurf/types.h"
#include "netsurf/content_type.h"
#include "netsurf/browser_window.h"
#include "netsurf/mouse.h"
#include "desktop/frame_types.h"

struct fetch_multipart_data;
struct box;
struct rect;
struct browser_window;
struct content;
struct hlcache_handle;
struct http_parameter;
struct imagemap;
struct object_params;
struct plotters;
struct textarea;
struct scrollbar;
struct scrollbar_msg_data;
struct search_context;
struct selection;
struct nsurl;
struct plot_font_style;

/**
 * Container for stylesheets used by an HTML document
 */
struct html_stylesheet {
	struct dom_node *node; /**< dom node associated with sheet */
	struct hlcache_handle *sheet;
	bool modified;
	bool unused;
};


/** Type of script - hoisted before struct for C89 compatibility. */
enum html_script_type {
	HTML_SCRIPT_INLINE,
	HTML_SCRIPT_SYNC,
	HTML_SCRIPT_DEFER,
	HTML_SCRIPT_ASYNC
};

/**
 * Container for scripts used by an HTML document
 */
struct html_script {
	enum html_script_type type;
	union {
		struct hlcache_handle *handle;
		struct dom_string *string;
	} data;	/**< Script data */
	struct dom_string *mimetype;
	struct dom_string *encoding;
	/* fixes869 (#295) - the <script> ELEMENT this entry came from, so a
	 * `load` / `error` event can be fired AT it once the fetch+exec finishes.
	 * Nothing here recorded the node before, so there was no way to reach the
	 * element from html_script_exec, and script.onload could never fire.
	 * That is fatal for the universal dynamic-loader idiom:
	 *     const s = document.createElement('script');
	 *     s.onload = () => resolve();       // <- resolves the caller's Promise
	 *     s.src = url; document.body.appendChild(s);
	 * With no load event the promise never settles and the caller's chain
	 * stalls forever (hackaday's verbum loader does exactly this, and
	 * wp-polyfill now executes but verbum is never requested).
	 * Owned ref (dom_node_ref at store, unref in html_script_free), because a
	 * page may remove the element from the DOM before the fetch completes.
	 * NULL for parser-inserted scripts, which need no such event. */
	struct dom_node *node;
	bool already_started;
	bool parser_inserted;
	bool force_async;
	bool ready_exec;
	bool async;
	bool defer;
};


/**
 * An object (img, object, etc. tag) in a CONTENT_HTML document.
 */
struct content_html_object {
	struct content *parent;		/**< Parent document */
	struct content_html_object *next; /**< Next in chain */

	struct hlcache_handle *content;  /**< Content, or 0. */
	struct box *box;  /**< Node in box tree containing it. */
	/** Bitmap of acceptable content types */
	content_type permitted_types;
	bool background;  /**< This object is a background image. */
	/** fixes975 - the URL this object was fetched for, owned (nsurl_ref'd
	 * at fetch, unref'd in html_object_free_objects). This is the key the
	 * creation-time adoption in html_fetch_object matches on, so a URL the
	 * document is already fetching is never fetched a second time.
	 *
	 * Held here rather than read back from the hlcache handle because a
	 * handle whose retrieval has not yet resolved has no content and
	 * reports its URL only by walking hlcache's private retrieval ring --
	 * i.e. exactly the in-flight case adoption exists to catch. Deliberately
	 * LAST in the struct: only html_fetch_object allocates one of these, so
	 * appending keeps every existing field at its current offset. */
	struct nsurl *url;
};


/**
 * Frame tree (frameset or frame tag)
 */
struct content_html_frames {
	int cols;	/** number of columns in frameset */
	int rows;	/** number of rows in frameset */

	struct frame_dimension width;	/** frame width */
	struct frame_dimension height;	/** frame width */
	int margin_width;	/** frame margin width */
	int margin_height;	/** frame margin height */

	char *name;	/** frame name (for targetting) */
	struct nsurl *url;	/** frame url */

	bool no_resize;	/** frame is not resizable */
	browser_scrolling scrolling;	/** scrolling characteristics */
	bool border;	/** frame has a border */
	colour border_colour;	/** frame border colour */

	struct content_html_frames *children; /** [cols * rows] children */
};

/**
 * Inline frame list (iframe tag)
 */
struct content_html_iframe {
	struct box *box;

	int margin_width;	/** frame margin width */
	int margin_height;	/** frame margin height */

	char *name;	/** frame name (for targetting) */
	struct nsurl *url;	/** frame url */

	browser_scrolling scrolling;	/** scrolling characteristics */
	bool border;	/** frame has a border */
	colour border_colour;	/** frame border colour */

	struct content_html_iframe *next;
};

/* entries in stylesheet_content */
#define STYLESHEET_BASE		0	/* base style sheet */
#define STYLESHEET_QUIRKS	1	/* quirks mode stylesheet */
#define STYLESHEET_ADBLOCK	2	/* adblocking stylesheet */
#define STYLESHEET_USER		3	/* user stylesheet */
#define STYLESHEET_START	4	/* start of document stylesheets */

/**
 * initialise content handler
 *
 * \return NSERROR_OK on success otherwise appropriate error code
 */
nserror html_init(void);

/**
 * redraw a specific box
 *
 * used by core browser
 */
void html_redraw_a_box(struct hlcache_handle *h, struct box *box);

/**
 * MacSurf (#252): resolve the CSS caret-color of the currently focused
 * editable box (text input / textarea) in an HTML content. Writes the
 * NetSurf colour and returns true when caret-color is an explicit colour
 * or currentColor; returns false for auto / no focus (caller keeps the
 * default caret colour). C89 / CW8-safe.
 *
 * used by core browser (frontend caret paint)
 */
bool html_get_caret_colour(struct hlcache_handle *h, colour *colour_out);

/**
 * obtain html frame content from handle
 *
 * used by core browser
 */
struct content_html_frames *html_get_frameset(struct hlcache_handle *h);

/**
 * obtain html iframe content from handle
 *
 * used by core browser
 */
struct content_html_iframe *html_get_iframe(struct hlcache_handle *h);

/**
 * obtain html base target from handle
 *
 * used by core browser
 */
const char *html_get_base_target(struct hlcache_handle *h);

/**
 * set filename on a file gadget
 *
 * used by core browser
 */
void html_set_file_gadget_filename(struct hlcache_handle *hl,
	struct form_control *gadget, const char *fn);

/**
 * Retrieve stylesheets used by HTML document
 *
 * \param h Content to retrieve stylesheets from
 * \param n Pointer to location to receive number of sheets
 * \return Pointer to array of stylesheets
 */
struct html_stylesheet *html_get_stylesheets(struct hlcache_handle *h,
		unsigned int *n);

/**
 * Retrieve objects used by HTML document
 *
 * \param h Content to retrieve objects from
 * \param n Pointer to location to receive number of objects
 * \return Pointer to array of objects
 */
struct content_html_object *html_get_objects(struct hlcache_handle *h,
		unsigned int *n);

/**
 * get the offset within the docuemnt of a fragment id
 */
bool html_get_id_offset(struct hlcache_handle *h, lwc_string *frag_id,
		int *x, int *y);

#endif
