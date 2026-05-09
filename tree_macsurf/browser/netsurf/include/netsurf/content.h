/*
 * Copyright 2016 Vincent Sanders <vince@netsurf-browser.org>
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
 * Public content interface.
 *
 * The content functions manipulate content objects.
 */

#ifndef _NETSURF_CONTENT_H_
#define _NETSURF_CONTENT_H_

#include <libwapcaplet/libwapcaplet.h>

#include "netsurf/types.h"
#include "netsurf/content_type.h"

struct bitmap;
struct hlcache_handle;
struct rect;
struct redraw_context;

/** parameters to content redraw */
struct content_redraw_data {
	int x; /**< coordinate for top-left of redraw */
	int y; /**< coordinate for top-left of redraw */

	/** dimensions to render content at (for scaling contents with
	 *  intrinsic dimensions)
	 */
	int width; /**< horizontal dimension */
	int height; /**< vertical dimension */

	/** The background colour */
	colour background_colour;

	/** Scale for redraw
	 *  (for scaling contents without intrinsic dimensions)
	 */
	float scale; /**< Scale factor for redraw */

	bool repeat_x; /**< whether content is tiled in x direction */
	bool repeat_y; /**< whether content is tiled in y direction */
};

/**
 * Retrieve the bitmap contained in an image content
 *
 * \param h handle to the content.
 * \return Pointer to bitmap, or NULL if none.
 */
struct bitmap *content_get_bitmap(struct hlcache_handle *h);


/**
 * Retrieve the encoding of a content
 *
 * \param h handle to the content.
 * \param op encoding operation.
 * \return Pointer to content info or NULL if none.
 */
const char *content_get_encoding(struct hlcache_handle *h, enum content_encoding_type op);


/**
 * Retrieve mime-type of content
 *
 * \param h handle to the content to retrieve mime type from
 * \return Pointer to referenced mime type, or NULL if not found.
 */
lwc_string *content_get_mime_type(struct hlcache_handle *h);


/**
 * Retrieve source of content
 *
 * \param h Content handle to retrieve source of
 * \param size Pointer to location to receive byte size of source
 * \return Pointer to source data
 */
const uint8_t *content_get_source_data(struct hlcache_handle *h, size_t *size);


/**
 * Retrieve title associated with content
 *
 * \param h handle to the content to retrieve title from
 * \return Pointer to title, or NULL if not found.
 */
const char *content_get_title(struct hlcache_handle *h);


/**
 * Retrieve computed type of content
 *
 * \param h handle to the content to retrieve type of.
 * \return Computed content type
 */
content_type content_get_type(struct hlcache_handle *h);


/**
 * Retrieve width of content
 *
 * \param h handle to the content to get width of.
 * \return Content width
 */
int content_get_width(struct hlcache_handle *h);


/**
 * Retrieve height of content
 *
 * \param h handle to the content to get height of.
 * \return Content height
 */
int content_get_height(struct hlcache_handle *h);


/**
 * Invalidate content reuse data.
 *
 * causes subsequent requests for content URL to query server to
 * determine if content can be reused. This is required behaviour for
 * forced reloads etc.
 *
 * \param h Content handle to invalidate.
 */
void content_invalidate_reuse_data(struct hlcache_handle *h);


/**
 * Display content on screen with optional tiling.
 *
 * \param h The content to redraw.
 * \param data The contents redraw data.
 * \param clip The clipping rectangle to use when redrawing the content.
 * \param ctx current redraw context.
 * \return true if successful otherwise false.
 *
 * Calls the redraw function for the content.
 */
bool content_redraw(struct hlcache_handle *h, struct content_redraw_data *data, const struct rect *clip, const struct redraw_context *ctx);


/**
 * Redraw a content with scale set for horizontal fit.
 *
 * Redraws the content at a specified width and height with the
 * content drawing scaled to fit within the area.
 *
 * \param h The content to redraw
 * \param width The target width
 * \param height The target height
 * \param ctx current redraw context
 * \return true if successful, false otherwise
 *
 * The thumbnail is guaranteed to be filled to its width/height extents, so
 * there is no need to render a solid background first.
 *
 * Units for width and height are pixels.
 */
bool content_scaled_redraw(struct hlcache_handle *h, int width, int height, const struct redraw_context *ctx);


/**
 * Retrieve the URL associated with a high level cache handle
 *
 * \param handle The handle to inspect
 * \return URL object without reference or NULL if not available.
 */
struct nsurl *hlcache_handle_get_url(const struct hlcache_handle *handle);

#endif
