/*
 * Copyright 2005-2007 James Bursa <bursa@users.sourceforge.net>
 * Copyright 2003 Philip Pemberton <philpem@users.sourceforge.net>
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
 * Protected interface to Content handling.
 *
 * The content functions manipulate struct contents, which correspond to URLs.
 */

#ifndef NETSURF_CONTENT_CONTENT_PROTECTED_H_
#define NETSURF_CONTENT_CONTENT_PROTECTED_H_

#include <stdio.h>
#include <libwapcaplet/libwapcaplet.h>

#include "utils/errors.h"
#include "netsurf/content_type.h"
#include "netsurf/mouse.h" /* mouse state enums */

struct nsurl;
struct content_redraw_data;
union content_msg_data;
struct http_parameter;
struct llcache_handle;
struct object_params;
struct content;
struct redraw_context;
struct rect;
struct browser_window;
struct browser_window_features;
struct textsearch_context;
struct box;
struct selection;
struct selection_string;

typedef struct content_handler content_handler;

/**
 * Content operation function table
 *
 * function table implementing a content type.
 */
struct content_handler {
	void (*fini)(void);

	nserror (*create)(const struct content_handler *handler,
                          lwc_string *imime_type,
                          const struct http_parameter *params,
                          struct llcache_handle *llcache,
                          const char *fallback_charset, bool quirks,
                          struct content **c);

	bool (*process_data)(struct content *c,
			const char *data, unsigned int size);
	bool (*data_complete)(struct content *c);
	void (*reformat)(struct content *c, int width, int height);
	void (*destroy)(struct content *c);
	void (*stop)(struct content *c);
	nserror (*mouse_track)(struct content *c, struct browser_window *bw,
			browser_mouse_state mouse, int x, int y);
	nserror (*mouse_action)(struct content *c, struct browser_window *bw,
			browser_mouse_state mouse, int x, int y);
	bool (*keypress)(struct content *c, uint32_t key);
	bool (*redraw)(struct content *c, struct content_redraw_data *data,
			const struct rect *clip,
			const struct redraw_context *ctx);
	nserror (*open)(struct content *c, struct browser_window *bw,
			struct content *page, struct object_params *params);
	nserror (*close)(struct content *c);
	void (*clear_selection)(struct content *c);
	char * (*get_selection)(struct content *c);
	nserror (*get_contextual_content)(struct content *c, int x, int y,
			struct browser_window_features *data);
	bool (*scroll_at_point)(struct content *c, int x, int y,
			int scrx, int scry);
	bool (*drop_file_at_point)(struct content *c, int x, int y,
			char *file);
	nserror (*debug_dump)(struct content *c, FILE *f, enum content_debug op);
	nserror (*debug)(struct content *c, enum content_debug op);
	nserror (*clone)(const struct content *old, struct content **newc);
	bool (*matches_quirks)(const struct content *c, bool quirks);
	const char *(*get_encoding)(const struct content *c, enum content_encoding_type op);
	content_type (*type)(void);
	void (*add_user)(struct content *c);
	void (*remove_user)(struct content *c);
	bool (*exec)(struct content *c, const char *src, size_t srclen);
	bool (*saw_insecure_objects)(struct content *c);

	/**
	 * content specific free text search find
	 */
	nserror (*textsearch_find)(struct content *c, struct textsearch_context *context, const char *pattern, int p_len, bool case_sens);

	/**
	 * get bounds of free text search match
	 */
	nserror (*textsearch_bounds)(struct content *c, unsigned start_idx, unsigned end_idx, struct box *start_ptr, struct box *end_ptr, struct rect *bounds_out);

	/**
	 * redraw an area of selected text
	 *
	 * The defined text selection will cause an area of the
	 *   content to be marked as invalid and hence redrawn.
	 *
	 * \param c The content being redrawn
	 * \param start_idx The start index of the text region to be redrawn
	 * \param end_idx The end index of teh text region to be redrawn
	 * \return NSERROR_OK on success else error code
	 */
	nserror (*textselection_redraw)(struct content *c, unsigned start_idx, unsigned end_idx);

	/**
	 * copy selected text into selection string possibly with formatting
	 */
	nserror (*textselection_copy)(struct content *c, unsigned start_idx, unsigned end_idx, struct selection_string *selstr);

	/**
	 * get maximum index of text section.
	 *
	 * \param[in] c The content to measure
	 * \param[out] end_idx pointer to value to recive result
	 * \return NSERROR_OK and \a end_idx updated else error code
	 */
	nserror (*textselection_get_end)(struct content *c, unsigned *end_idx);

        /**
	 * handler dependant content sensitive internal data interface.
	 */
	void *(*get_internal)(const struct content *c, void *context);

	/**
	 * are the content contents opaque.
	 *
	 * Determine if this content would obscure (not mix with) any background
	 *
	 * \param c The content to check
	 */
	bool (*is_opaque)(struct content *c);

	/**
	 * There must be one content per user for this type.
	 */
	bool no_share;
};

/**
 * Linked list of users of a content.
 */
struct content_user
{
	void (*callback)(
			struct content *c,
			content_msg msg,
			const union content_msg_data *data,
			void *pw);
	void *pw;

	struct content_user *next;
};

/**
 * Content which corresponds to a single URL.
 */
struct content {
	/**
	 * Low-level cache object
	 */
	struct llcache_handle *llcache;

	/**
	 * Original MIME type of data
	 */
	lwc_string *mime_type;

	/**
	 * Handler for content
	 */
	const struct content_handler *handler;

	/**
	 * Current status.
	 */
	content_status status;

	/**
	 * Width dimension, if applicable.
	 */
	int width;
	/**
	 * Height dimension, if applicable.
	 */
	int height;
	/**
	 * Viewport width.
	 */
	int available_width;
	/**
	 * Viewport height.
	 */
	int available_height;

	/**
	 * Content is in quirks mode
	 */
	bool quirks;
	/**
	 * Fallback charset, or NULL
	 */
	char *fallback_charset;

	/**
	 * URL for refresh request
	 */
	struct nsurl *refresh;

	/**
	 * list of metadata links
	 */
	struct content_rfc5988_link *links;

	/**
	 * Creation timestamp when LOADING or READY.  Total time in ms
	 * when DONE.
	 */
	uint64_t time;

	/**
	 * Earliest time to attempt a period reflow while fetching a
	 * page's objects.
	 */
	uint64_t reformat_time;

	/**
	 * Estimated size of all data associated with this content
	 */
	unsigned int size;
	/**
	 * Title for browser window.
	 */
	char *title;
	/**
	 * Number of child fetches or conversions currently in progress.
	 */
	unsigned int active;
	/**
	 * List of users.
	 */
	struct content_user *user_list;
	/**
	 * Full text for status bar.
	 */
	char status_message[120];
	/**
	 * Status of content.
	 */
	char sub_status[80];
	/**
	 * Content is being processed: data structures may be
	 * inconsistent and content must not be redrawn or modified.
	 */
	bool locked;

	/**
	 * Total data size, 0 if unknown.
	 */
	unsigned long total_size;
	/**
	 * HTTP status code, 0 if not HTTP.
	 */
	long http_code;

	/**
	 * Free text search state
	 */
	struct {
		char *string;
		struct textsearch_context *context;
	} textsearch;
};

extern const char * const content_type_name[];
extern const char * const content_status_name[];


/**
 * Initialise a new base content structure.
 *
 * \param c                 Content to initialise
 * \param handler           Content handler
 * \param imime_type        MIME type of content
 * \param params            HTTP parameters
 * \param llcache           Source data handle
 * \param fallback_charset  Fallback charset
 * \param quirks            Quirkiness of content
 * \return NSERROR_OK on success, appropriate error otherwise
 */
nserror content__init(struct content *c, const struct content_handler *handler,
		lwc_string *imime_type, const struct http_parameter *params,
		struct llcache_handle *llcache, const char *fallback_charset,
		bool quirks);

/**
 * Clone a content's data members
 *
 * \param c   Content to clone
 * \param nc  Content to populate
 * \return NSERROR_OK on success, appropriate error otherwise
 */
nserror content__clone(const struct content *c, struct content *nc);

/**
 * Put a content in status CONTENT_STATUS_READY and unlock the content.
 */
void content_set_ready(struct content *c);

/**
 * Put a content in status CONTENT_STATUS_DONE.
 */
void content_set_done(struct content *c);

/**
 * Put a content in status CONTENT_STATUS_ERROR and unlock the content.
 *
 * \note We expect the caller to broadcast an error report if needed.
 */
void content_set_error(struct content *c);

/**
 * Updates content with new status.
 *
 * The textual status contained in the content is updated with given string.
 *
 * \param c The content to set status in.
 * \param status_message new textual status
 */
void content_set_status(struct content *c, const char *status_message);

/**
 * Send a message to all users.
 */
void content_broadcast(struct content *c, content_msg msg, const union content_msg_data *data);

/**
 * Send an error message to all users.
 *
 * \param c The content whose users should be informed of an error
 * \param errorcode The nserror code to send
 * \param msg The error message to send alongside
 */
void content_broadcast_error(struct content *c, nserror errorcode, const char *msg);

/**
 * associate a metadata link with a content.
 *
 * \param c content to add link to
 * \param link The rfc5988 link to add
 */
bool content__add_rfc5988_link(struct content *c, const struct content_rfc5988_link *link);

/**
 * free a rfc5988 link
 *
 * \param link The link to free
 * \return The next link in the chain
 */
struct content_rfc5988_link *content__free_rfc5988_link(struct content_rfc5988_link *link);

/**
 * cause a content to be reformatted.
 *
 * \param c content to be reformatted
 * \param background perform reformat in background
 * \param width The available width to reformat content in
 * \param height The available height to reformat content in
 */
void content__reformat(struct content *c, bool background, int width, int height);

/**
 * Request a redraw of an area of a content
 *
 * \param c	  Content
 * \param x	  x co-ord of left edge
 * \param y	  y co-ord of top edge
 * \param width	  Width of rectangle
 * \param height  Height of rectangle
 */
void content__request_redraw(struct content *c, int x, int y, int width, int height);

/**
 * Retrieve mime-type of content
 *
 * \param c Content to retrieve mime-type of
 * \return Pointer to referenced mime-type, or NULL if not found.
 */
lwc_string *content__get_mime_type(struct content *c);

/**
 * Set title associated with content
 *
 * \param c Content to set title on.
 * \param title The new title to set.
 * \return true on sucess else false.
 */
bool content__set_title(struct content *c, const char *title);

/**
 * Retrieve title associated with content
 *
 * \param c Content to retrieve title from
 * \return Pointer to title, or NULL if not found.
 */
const char *content__get_title(struct content *c);

/**
 * Retrieve status message associated with content
 *
 * \param c Content to retrieve status message from
 * \return Pointer to status message, or NULL if not found.
 */
const char *content__get_status_message(struct content *c);

/**
 * Retrieve width of content
 *
 * \param c Content to retrieve width of
 * \return Content width
 */
int content__get_width(struct content *c);

/**
 * Retrieve height of content
 *
 * \param c Content to retrieve height of
 * \return Content height
 */
int content__get_height(struct content *c);

/**
 * Retrieve available width of content
 *
 * \param c content to get available width of.
 * \return Available width of content.
 */
int content__get_available_width(struct content *c);

/**
 * Retrieve source of content.
 *
 * \param c    Content to retrieve source of.
 * \param size Pointer to location to receive byte size of source.
 * \return Pointer to source data.
 */
const uint8_t *content__get_source_data(struct content *c, size_t *size);

/**
 * Invalidate content reuse data.
 *
 * causes subsequent requests for content URL to query server to
 * determine if content can be reused. This is required behaviour for
 * forced reloads etc.
 *
 * \param c Content to invalidate.
 */
void content__invalidate_reuse_data(struct content *c);

/**
 * Retrieve the refresh URL for a content
 *
 * \param c Content to retrieve refresh URL from
 * \return Pointer to URL or NULL if none
 */
struct nsurl *content__get_refresh_url(struct content *c);

/**
 * Retrieve the bitmap contained in an image content
 *
 * \param c Content to retrieve opacity from
 * \return Pointer to bitmap or NULL if none.
 */
struct bitmap *content__get_bitmap(struct content *c);

/**
 * Determine if a content is opaque
 *
 * \param c Content to retrieve opacity from
 * \return false if the content is not opaque or information is not
 *         known else true.
 */
bool content__get_opaque(struct content *c);

/**
 * Retrieve the encoding of a content
 *
 * \param c the content to examine the encoding of.
 * \param op encoding operation.
 * \return Pointer to content info or NULL if none.
 */
const char *content__get_encoding(struct content *c, enum content_encoding_type op);

/**
 * Return whether a content is currently locked
 *
 * \param c Content to test
 * \return true iff locked, else false
 */
bool content__is_locked(struct content *c);

/**
 * Destroy and free a content.
 *
 * Calls the destroy function for the content, and frees the structure.
 */
void content_destroy(struct content *c);

/**
 * Register a user for callbacks.
 *
 * \param c the content to register
 * \param callback the user callback function
 * \param pw callback private data
 * \return true on success, false otherwise on memory exhaustion
 *
 * The callback will be called when content_broadcast() is
 * called with the content.
 */
bool content_add_user(struct content *h,
		      void (*callback)(
				       struct content *c,
				       content_msg msg,
				       const union content_msg_data *data,
				       void *pw),
		      void *pw);

/**
 * Remove a callback user.
 *
 * The callback function and pw must be identical to those passed to
 * content_add_user().
 *
 * \param c Content to remove user from
 * \param callback passed when added
 * \param ctx Context passed when added
 */
void content_remove_user(struct content *c,
			 void (*callback)(
					  struct content *c,
					  content_msg msg,
					  const union content_msg_data *data,
					  void *pw),
			 void *ctx);


/**
 * Count users for the content.
 *
 * \param c Content to consider
 */
uint32_t content_count_users(struct content *c);


/**
 * Determine if quirks mode matches
 *
 * \param c Content to consider
 * \param quirks  Quirks mode to match
 * \return True if quirks match, false otherwise
 */
bool content_matches_quirks(struct content *c, bool quirks);

/**
 * Determine if a content is shareable
 *
 * \param c  Content to consider
 * \return True if content is shareable, false otherwise
 */
bool content_is_shareable(struct content *c);

/**
 * Retrieve the low-level cache handle for a content
 *
 * \note only used by hlcache
 *
 * \param c Content to retrieve from
 * \return Low-level cache handle
 */
const struct llcache_handle *content_get_llcache_handle(struct content *c);

/**
 * Retrieve URL associated with content
 *
 * \param c  Content to retrieve URL from
 * \return Pointer to URL, or NULL if not found.
 */
struct nsurl *content_get_url(struct content *c);

/**
 * Clone a content object in its current state.
 *
 * \param c  Content to clone
 * \return Clone of \a c
 */
struct content *content_clone(struct content *c);

/**
 * Abort a content object
 *
 * \param c The content object to abort
 * \return NSERROR_OK on success, otherwise appropriate error
 */
nserror content_abort(struct content *c);

#endif
