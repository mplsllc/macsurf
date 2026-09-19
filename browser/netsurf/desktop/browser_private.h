/*
 * Copyright 2003 Phil Mellor <monkeyson@users.sourceforge.net>
 * Copyright 2006 James Bursa <bursa@users.sourceforge.net>
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
 * Browser window private structure.
 */

#ifndef NETSURF_DESKTOP_BROWSER_PRIVATE_H_
#define NETSURF_DESKTOP_BROWSER_PRIVATE_H_

#include "content/fetch.h"
#include "desktop/frame_types.h"

struct box;
struct hlcache_handle;
struct gui_window;
struct selection;
struct nsurl;

/**
 * history entry page information
 */
struct history_page {
	struct nsurl *url;    /**< Page URL, never NULL. */
	lwc_string *frag_id; /** Fragment identifier, or NULL. */
	char *title;  /**< Page title, never NULL. */
	struct bitmap *bitmap;  /**< Thumbnail bitmap, or NULL. */
	float scroll_x; /**< Scroll X offset when visited */
	float scroll_y; /**< Scroll Y offset when visited */
};

/**
 * A node in the history tree.
 */
struct history_entry {
	struct history_page page;
	struct history_entry *back;  /**< Parent. */
	struct history_entry *next;  /**< Next sibling. */
	struct history_entry *forward;  /**< First child. */
	struct history_entry *forward_pref;  /**< Child in direction of
						  current entry. */
	struct history_entry *forward_last;  /**< Last child. */
	unsigned int children;  /**< Number of children. */
	int x;  /**< Position of node. */
	int y;  /**< Position of node. */
};

/**
 * History tree for a window.
 */
struct history {
	/** First page in tree (page that window opened with). */
	struct history_entry *start;
	/** Current position in tree. */
	struct history_entry *current;
	/** Width of layout. */
	int width;
	/** Height of layout. */
	int height;
};

/**
 * The parameters for a fetch.
 */
struct browser_fetch_parameters {
	struct nsurl *url;                           /**< The URL to fetch */
	struct nsurl *referrer;			     /**< Optional refererer */
	enum browser_window_nav_flags flags;	     /**< Navigation flags */
	char *post_urlenc;			     /**< URL encoded post data */
	struct fetch_multipart_data *post_multipart; /**< Multipart post data */
	char *parent_charset;			     /**< Optional parent character set */
	bool parent_quirks;			     /**< Optional parent quirks */
};


/**
 * Browser window data.
 */
struct browser_window {
	/**
	 * Content handle of page currently displayed which must have
	 *  READY or DONE status or NULL for no content.
	 */
	struct hlcache_handle *current_content;

	/**
	 * The fetch parameters for the current content
	 */
	struct browser_fetch_parameters current_parameters;

	/**
	 * The certificate chain for the current content
	 */
	struct cert_chain *current_cert_chain;

	/**
	 * Content handle of page in process of being loaded or NULL
	 * if no page is being loaded.
	 */
	struct hlcache_handle *loading_content;

	/**
	 * The fetch parameters for the loading content
	 */
	struct browser_fetch_parameters loading_parameters;

	/**
	 * The certificate chain for the loading content
	 */
	struct cert_chain *loading_cert_chain;

	/**
	 * Favicon
	 */
	struct {
		/**
		 * content handle of current page favicon
		 */
		struct hlcache_handle *current;

		/**
		 * content handle for favicon which we started loading
		 * early
		 */
		struct hlcache_handle *loading;

		/**
		 * flag to indicate favicon fetch already failed which
		 * prevents infinite error looping.
		 */
		bool failed;
	} favicon;

	/** local history handle. */
	struct history *history;

	/**
	 * Platform specific window data only valid at top level.
	 */
	struct gui_window *window;

	/** Busy indicator is active. */
	bool throbbing;
	/** Add loading_content to the window history when it loads. */
	bool history_add;
	/** Internal navigation, do not update URL etc */
	bool internal_nav;

	/** Fragment identifier for current_content. */
	lwc_string *frag_id;

	/**
	 * Current drag status.
	 *
	 * These values are only vald whle type is not DRAGGING_NONE
	 */
	struct {
		/** the type of drag in progress */
		browser_drag_type type;

		/** Current drag's browser window, when not in root bw. */
		struct browser_window *window;

		/** Mouse position at start of current scroll drag. */
		int start_x;
		int start_y;

		/** Scroll offsets at start of current scroll draw. */
		int start_scroll_x;
		int start_scroll_y;

		/** Frame resize directions for current frame resize drag. */
		unsigned int resize_left : 1;
		unsigned int resize_right : 1;
		unsigned int resize_up : 1;
		unsigned int resize_down : 1;
	} drag;

	/** Current fetch is download */
	bool download;

	/** Refresh interval (-1 if undefined) */
	int refresh_interval;

	/** Window dimensions */
	int x;
	int y;
	int width;
	int height;

	struct scrollbar *scroll_x;  /**< Horizontal scroll. */
	struct scrollbar *scroll_y;  /**< Vertical scroll. */

	/** scale of window contents */
	float scale;

	/** Window characteristics */
	enum {
		BROWSER_WINDOW_NORMAL,
		BROWSER_WINDOW_IFRAME,
		BROWSER_WINDOW_FRAME,
		BROWSER_WINDOW_FRAMESET,
	} browser_window_type;

	/** frameset characteristics */
	int rows;
	int cols;

	/** frame dimensions */
	struct frame_dimension frame_width;
	struct frame_dimension frame_height;
	int margin_width;
	int margin_height;

	/** frame name for targetting */
	char *name;

	/** frame characteristics */
	bool no_resize;
	browser_scrolling scrolling;
	bool border;
	colour border_colour;

	/** iframe parent box */
	struct box *box;

	/** [cols * rows] children */
	struct browser_window *children;
	struct browser_window *parent;

	/** [iframe_count] iframes */
	int iframe_count;
	struct browser_window *iframes;

	/** browser window child of root browser window which has input focus */
	struct browser_window *focus;

	/** Last time a link was followed in this window */
	uint64_t last_action;

	/** Current selection */
	struct {
		struct browser_window *bw;
		bool read_only;
	} selection;
	bool can_edit;

	/** current javascript context */
	struct jsheap *jsheap;

	/** cache of the currently displayed status text. */
	struct {
		char *text; /**< Current status bar text. */
		int text_len; /**< Length of the status::text buffer. */
		int match; /**< Number of times an idempotent status-set operation was performed. */
		int miss; /**< Number of times status was really updated. */
	} status;
};


/**
 * Initialise common parts of a browser window
 *
 * \param flags     Flags to control operation
 * \param bw        The window to initialise
 * \param existing  The existing window if cloning, else NULL
 */
nserror browser_window_initialise_common(enum browser_window_create_flags flags,
		struct browser_window *bw,
		const struct browser_window *existing);


/**
 * Release all memory associated with a browser window.
 *
 * \param bw browser window
 */
nserror browser_window_destroy_internal(struct browser_window *bw);

/**
 * Get the dimensions of the area a browser window occupies
 *
 * \param  bw      The browser window to get dimensions of
 * \param  width   Updated to the browser window viewport width
 * \param  height  Updated to the browser window viewport height
 * \return NSERROR_OK and width and height updated otherwise error code
 */
nserror browser_window_get_dimensions(struct browser_window *bw,
		int *width, int *height);


/**
 * Update the extent of the inside of a browser window to that of the current
 * content
 *
 * \param bw browser_window to update the extent of
 */
void browser_window_update_extent(struct browser_window *bw);


/**
 * Cause an area of a browser window to be marked invalid and hence redrawn.
 *
 * \param bw The browser window to update.
 * \param rect The area to redraw
 */
nserror browser_window_invalidate_rect(struct browser_window *bw, struct rect *rect);


/**
 * Change the status bar of a browser window.
 *
 * \param  bw	 browser window
 * \param  text  new status text (copied)
 */
void browser_window_set_status(struct browser_window *bw, const char *text);


/**
 * Get the root level browser window
 *
 * \param  bw     browser window to set the type of the current drag for
 * \return  root browser window
 */
struct browser_window * browser_window_get_root(
		struct browser_window *bw);


/**
 * Create a new history tree for a browser window window.
 *
 * \param bw browser window to create history for.
 *
 * \return NSERROR_OK or appropriate error otherwise
 */
nserror browser_window_history_create(struct browser_window *bw);

/**
 * Clone a bw's history tree for new bw
 *
 * \param  existing	browser window with history to clone.
 * \param  clone	browser window to make cloned history for.
 *
 * \return  NSERROR_OK or appropriate error otherwise
 */
nserror browser_window_history_clone(const struct browser_window *existing,
		struct browser_window *clone);


/**
 * Insert a url into the history tree.
 *
 * \param  bw       browser window with history object
 * \param  content  content to add to history
 * \param  frag_id  fragment identifier, or NULL.
 * \return NSERROR_OK or error code on faliure.
 *
 * The page is added after the current entry and becomes current.
 */
nserror browser_window_history_add(struct browser_window *bw,
		struct hlcache_handle *content, lwc_string *frag_id);

/**
 * Update the thumbnail and scroll offsets for the current entry.
 *
 * \param bw The browser window to update the history within.
 * \param content content for current entry
 * \return NSERROR_OK or error code on faliure.
 */
nserror browser_window_history_update(struct browser_window *bw,
		struct hlcache_handle *content);

/**
 * Retrieve the stored scroll offsets for the current history entry
 *
 * \param bw The browser window to retrieve scroll offsets for.
 * \param sx Pointer to a float for the X scroll offset
 * \param sy Pointer to a float for the Y scroll offset
 * \return NSERROR_OK or error code on failure.
 */
nserror browser_window_history_get_scroll(struct browser_window *bw,
					  float *sx, float *sy);

/**
 * Free a history structure.
 *
 * \param bw The browser window to destroy the history within.
 */
void browser_window_history_destroy(struct browser_window *bw);

/**
 * Type for handling query responses short-term
 */
typedef nserror (*browser_window_query_callback)(bool proceed, void *pw);

/**
 * Navigate a browser window to the current parameters
 *
 * \param bw The browser window to cause to navigate
 */
nserror browser_window__reload_current_parameters(struct browser_window *bw);

#endif
