/*
 * Copyright 2003 James Bursa <bursa@users.sourceforge.net>
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
 * Declaration of content enumerations.
 *
 * The content enumerations are defined here.
 */

#ifndef NETSURF_CONTENT_TYPE_H
#define NETSURF_CONTENT_TYPE_H

/** Debugging dump operations */
#ifndef MACSURF_CONTENT_DEBUG_ENUM
#define MACSURF_CONTENT_DEBUG_ENUM
enum content_debug {
	CONTENT_DEBUG_RENDER,
	CONTENT_DEBUG_DOM,
	CONTENT_DEBUG_REDRAW
};
#endif


/** Content encoding information types */
enum content_encoding_type {
	/** The content encoding */
	CONTENT_ENCODING_NORMAL,

	/** The content encoding source */
	CONTENT_ENCODING_SOURCE
};


/** The type of a content. */
typedef enum {
	/** no type for content */
	CONTENT_NONE		= 0x00,

	/** content is HTML */
	CONTENT_HTML		= 0x01,

	/** content is plain text */
	CONTENT_TEXTPLAIN	= 0x02,

	/** content is CSS */
	CONTENT_CSS		= 0x04,

	/** All images */
	CONTENT_IMAGE		= 0x08,

	/** Navigator API Plugins */
	CONTENT_PLUGIN		= 0x10,

	/** RISC OS themes content */
	CONTENT_THEME		= 0x20,

	/** Javascript */
	CONTENT_JS		= 0x40,

	/** All script types. */
	CONTENT_SCRIPT		= 0x40,

	/** Any content matches */
	CONTENT_ANY		= 0x7f
} content_type;


/** Status of a content */
typedef enum {
	/** Content is being fetched or converted and is not safe to display. */
	CONTENT_STATUS_LOADING,

	/** Some parts of content still being loaded, but can be displayed. */
	CONTENT_STATUS_READY,

	/** Content has completed all processing. */
	CONTENT_STATUS_DONE,

	/** Error occurred, content will be destroyed imminently. */
	CONTENT_STATUS_ERROR
} content_status;


/**
 * Used in callbacks to indicate what has occurred.
 */
typedef enum {
	/** Content wishes to log something */
	CONTENT_MSG_LOG,

	/** Content is from SSL and this is its chain */
	CONTENT_MSG_SSL_CERTS,

	/** fetching or converting */
	CONTENT_MSG_LOADING,

	/** may be displayed */
	CONTENT_MSG_READY,

	/** content has finished processing */
	CONTENT_MSG_DONE,

	/** error occurred */
	CONTENT_MSG_ERROR,

	/** fetch url redirect occured */
	CONTENT_MSG_REDIRECT,

	/** new status string */
	CONTENT_MSG_STATUS,

	/** content_reformat done */
	CONTENT_MSG_REFORMAT,

	/** needs redraw (eg. new animation frame) */
	CONTENT_MSG_REDRAW,

	/** wants refresh */
	CONTENT_MSG_REFRESH,

	/** download, not for display */
	CONTENT_MSG_DOWNLOAD,

	/** RFC5988 link */
	CONTENT_MSG_LINK,

	/** Javascript thread */
	CONTENT_MSG_GETTHREAD,

	/** Get viewport dimensions. */
	CONTENT_MSG_GETDIMS,

	/** Request to scroll content */
	CONTENT_MSG_SCROLL,

	/** Allow drag saving of content */
	CONTENT_MSG_DRAGSAVE,

	/** Allow URL to be saved */
	CONTENT_MSG_SAVELINK,

	/** Wants a specific mouse pointer set */
	CONTENT_MSG_POINTER,

	/** A selection made or cleared */
	CONTENT_MSG_SELECTION,

	/** Caret movement / hiding */
	CONTENT_MSG_CARET,

	/** A drag started or ended */
	CONTENT_MSG_DRAG,

	/** Create a select menu */
	CONTENT_MSG_SELECTMENU,

	/** A gadget has been clicked on (mainly for file) */
	CONTENT_MSG_GADGETCLICK,

	/** A free text search action has occurred */
	CONTENT_MSG_TEXTSEARCH
} content_msg;


#endif
