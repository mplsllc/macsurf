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


/**
 * Container for scripts used by an HTML document
 */
struct html_script {
	/** Type of script */
	enum html_script_type { HTML_SCRIPT_INLINE,
				HTML_SCRIPT_SYNC,
				HTML_SCRIPT_DEFER,
				HTML_SCRIPT_ASYNC } type;
	union {
		struct hlcache_handle *handle;
		struct dom_string *string;
	} data;	/**< Script data */
	struct dom_string *mimetype;
	struct dom_string *encoding;
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
