/*
 * Copyright 2003 John M Bell <jmb202@ecs.soton.ac.uk>
 * Copyright 2004 Richard Wilson <not_ginger_matt@users.sourceforge.net>
 * Copyright 2008 Sean Fox <dyntryx@gmail.com>
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
 * Content for image/gif implementation
 *
 * All GIFs are dynamically decompressed using the routines that gifread.c
 * provides. Whilst this allows support for progressive decoding, it is
 * not implemented here as NetSurf currently does not provide such support.
 *
 * [rjw] - Sun 4th April 2004
 */

#include <assert.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>

#include <nsutils/assert.h>

#include <nsgif.h>

#include "utils/log.h"
#include "utils/utils.h"
#include "utils/messages.h"
#include "utils/nsoption.h"
#include "netsurf/misc.h"
#include "netsurf/bitmap.h"
#include "netsurf/content.h"
#include "content/llcache.h"
#include "content/content.h"
#include "content/content_protected.h"
#include "content/content_factory.h"
#include "desktop/gui_internal.h"
#include "desktop/bitmap.h"

#include "image/image.h"
#include "image/gif.h"

typedef struct gif_content {
	struct content base;

	nsgif_t *gif; /**< GIF animation data */
	uint32_t current_frame;   /**< current frame to display [0...(max-1)] */
} gif_content;

static inline nserror gif__nsgif_error_to_ns(nsgif_error gif_res)
{
	nserror err;

	switch (gif_res) {
	case NSGIF_ERR_OOM:
		err = NSERROR_NOMEM;
		break;
	default:
		err = NSERROR_GIF_ERROR;
		break;
	}

	return err;
}

/**
 * Callback for libnsgif; forwards the call to bitmap_create()
 *
 * \param  width   width of image in pixels
 * \param  height  width of image in pixels
 * \return an opaque struct bitmap, or NULL on memory exhaustion
 */
static void *gif_bitmap_create(int width, int height)
{
	return guit->bitmap->create(width, height, BITMAP_NONE);
}

/**
 * Convert client bitmap format to a LibNSGIF format specifier.
 */
static nsgif_bitmap_fmt_t nsgif__get_bitmap_format(void)
{
	ns_static_assert((int)BITMAP_LAYOUT_R8G8B8A8 == (int)NSGIF_BITMAP_FMT_R8G8B8A8);
	ns_static_assert((int)BITMAP_LAYOUT_B8G8R8A8 == (int)NSGIF_BITMAP_FMT_B8G8R8A8);
	ns_static_assert((int)BITMAP_LAYOUT_A8R8G8B8 == (int)NSGIF_BITMAP_FMT_A8R8G8B8);
	ns_static_assert((int)BITMAP_LAYOUT_A8B8G8R8 == (int)NSGIF_BITMAP_FMT_A8B8G8R8);
	ns_static_assert((int)BITMAP_LAYOUT_RGBA8888 == (int)NSGIF_BITMAP_FMT_RGBA8888);
	ns_static_assert((int)BITMAP_LAYOUT_BGRA8888 == (int)NSGIF_BITMAP_FMT_BGRA8888);
	ns_static_assert((int)BITMAP_LAYOUT_ARGB8888 == (int)NSGIF_BITMAP_FMT_ARGB8888);
	ns_static_assert((int)BITMAP_LAYOUT_ABGR8888 == (int)NSGIF_BITMAP_FMT_ABGR8888);

	return (nsgif_bitmap_fmt_t)bitmap_fmt.layout;
}

static nserror gif_create_gif_data(gif_content *c)
{
	nsgif_error gif_res;
	const nsgif_bitmap_cb_vt gif_bitmap_callbacks = {
		.create = gif_bitmap_create,
		.destroy = guit->bitmap->destroy,
		.get_buffer = guit->bitmap->get_buffer,
		.set_opaque = guit->bitmap->set_opaque,
		.test_opaque = bitmap_test_opaque,
		.modified = guit->bitmap->modified,
	};

	gif_res = nsgif_create(&gif_bitmap_callbacks,
			nsgif__get_bitmap_format(), &c->gif);
	if (gif_res != NSGIF_OK) {
		nserror err = gif__nsgif_error_to_ns(gif_res);
		content_broadcast_error(&c->base, err, NULL);
		return err;
	}

	return NSERROR_OK;
}

static nserror gif_create(const content_handler *handler,
		lwc_string *imime_type, const struct http_parameter *params,
		llcache_handle *llcache, const char *fallback_charset,
		bool quirks, struct content **c)
{
	gif_content *result;
	nserror error;

	result = calloc(1, sizeof(gif_content));
	if (result == NULL)
		return NSERROR_NOMEM;

	error = content__init(&result->base, handler, imime_type, params,
			llcache, fallback_charset, quirks);
	if (error != NSERROR_OK) {
		free(result);
		return error;
	}

	error = gif_create_gif_data(result);
	if (error != NSERROR_OK) {
		free(result);
		return error;
	}

	*c = (struct content *) result;

	return NSERROR_OK;
}

/**
 * Scheduler callback. Performs any necessary animation.
 *
 * \param p  The content to animate
*/
static void gif_animate_cb(void *p);

/**
 * Performs any necessary animation.
 *
 * \param p  The content to animate
*/
static nserror gif__animate(gif_content *gif, bool redraw)
{
	nsgif_error gif_res;
	nsgif_rect_t rect;
	uint32_t delay;
	uint32_t f;

	gif_res = nsgif_frame_prepare(gif->gif, &rect, &delay, &f);
	if (gif_res != NSGIF_OK) {
		return gif__nsgif_error_to_ns(gif_res);
	}

	gif->current_frame = f;

	/* Continue animating if we should */
	if (nsoption_bool(animate_images) && delay != NSGIF_INFINITE) {
		guit->misc->schedule(delay * 10, gif_animate_cb, gif);
	}

	if (redraw) {
		union content_msg_data data;

		/* area within gif to redraw */
		data.redraw.x = rect.x0;
		data.redraw.y = rect.y0;
		data.redraw.width  = rect.x1 - rect.x0;
		data.redraw.height = rect.y1 - rect.y0;

		content_broadcast(&gif->base, CONTENT_MSG_REDRAW, &data);
	}

	return NSERROR_OK;
}

static void gif_animate_cb(void *p)
{
	gif_content *gif = p;

	gif__animate(gif, true);
}

static bool gif_convert(struct content *c)
{
	gif_content *gif = (gif_content *) c;
	const nsgif_info_t *gif_info;
	const uint8_t *data;
	nsgif_error gif_err;
	nserror err;
	size_t size;
	char *title;

	/* Get the animation */
	data = content__get_source_data(c, &size);

	/* Initialise the GIF */
	gif_err = nsgif_data_scan(gif->gif, size, data);
	if (gif_err != NSGIF_OK) {
		NSLOG(netsurf, INFO, "nsgif scan: %s", nsgif_strerror(gif_err));
		/* Not fatal unless we have no frames. */
	}

	nsgif_data_complete(gif->gif);

	gif_info = nsgif_get_info(gif->gif);
	assert(gif_info != NULL);

	/* Abort on bad GIFs */
	if (gif_info->frame_count == 0) {
		err = gif__nsgif_error_to_ns(gif_err);
		content_broadcast_error(c, err, "GIF with no frames.");
		return false;
	} else if (gif_info->width == 0 || gif_info->height == 0) {
		err = gif__nsgif_error_to_ns(gif_err);
		content_broadcast_error(c, err, "Zero size image.");
		return false;
	}

	/* Store our content width, height and calculate size */
	c->width = gif_info->width;
	c->height = gif_info->height;
	c->size += (gif_info->width * gif_info->height * 4) + 16 + 44;

	/* set title text */
	title = messages_get_buff("GIFTitle",
			nsurl_access_leaf(llcache_handle_get_url(c->llcache)),
			c->width, c->height);
	if (title != NULL) {
		content__set_title(c, title);
		free(title);
	}

	err = gif__animate(gif, false);
	if (err != NSERROR_OK) {
		content_broadcast_error(c, NSERROR_GIF_ERROR, NULL);
		return false;
	}

	/* Exit as a success */
	content_set_ready(c);
	content_set_done(c);

	/* Done: update status bar */
	content_set_status(c, "");
	return true;
}

/**
 * Updates the GIF bitmap to display the current frame
 *
 * \param gif The gif context to update.
 * \return NSGIF_OK on success else apropriate error code.
 */
static nsgif_error gif_get_frame(gif_content *gif,
		nsgif_bitmap_t **bitmap)
{
	uint32_t current_frame = gif->current_frame;
	if (!nsoption_bool(animate_images)) {
		current_frame = 0;
	}

	return nsgif_frame_decode(gif->gif, current_frame, bitmap);
}

static bool gif_redraw(struct content *c, struct content_redraw_data *data,
		const struct rect *clip, const struct redraw_context *ctx)
{
	gif_content *gif = (gif_content *) c;
	nsgif_bitmap_t *bitmap;

	if (gif_get_frame(gif, &bitmap) != NSGIF_OK) {
		return false;
	}

	return image_bitmap_plot(bitmap, data, clip, ctx);
}

static void gif_destroy(struct content *c)
{
	gif_content *gif = (gif_content *) c;

	/* Free all the associated memory buffers */
	guit->misc->schedule(-1, gif_animate_cb, c);
	nsgif_destroy(gif->gif);
}

static nserror gif_clone(const struct content *old, struct content **newc)
{
	gif_content *gif;
	nserror error;

	gif = calloc(1, sizeof(gif_content));
	if (gif == NULL)
		return NSERROR_NOMEM;

	error = content__clone(old, &gif->base);
	if (error != NSERROR_OK) {
		content_destroy(&gif->base);
		return error;
	}

	/* Simply replay creation and conversion of content */
	error = gif_create_gif_data(gif);
	if (error != NSERROR_OK) {
		content_destroy(&gif->base);
		return error;
	}

	if (old->status == CONTENT_STATUS_READY ||
			old->status == CONTENT_STATUS_DONE) {
		if (gif_convert(&gif->base) == false) {
			content_destroy(&gif->base);
			return NSERROR_CLONE_FAILED;
		}
	}

	*newc = (struct content *) gif;

	return NSERROR_OK;
}

static void gif_add_user(struct content *c)
{
	gif_content *gif = (gif_content *) c;

	/* Ensure this content has already been converted.
	 * If it hasn't, the animation will start at the conversion phase instead. */
	if (gif->gif == NULL) return;

	if (content_count_users(c) == 1) {
		/* First user, and content already converted, so start the animation. */
		if (nsgif_reset(gif->gif) == NSGIF_OK) {
			gif__animate(gif, true);
		}
	}
}

static void gif_remove_user(struct content *c)
{
	if (content_count_users(c) == 1) {
		/* Last user is about to be removed from this content, so stop the animation. */
		guit->misc->schedule(-1, gif_animate_cb, c);
	}
}

static nsgif_bitmap_t *gif_get_bitmap(
		const struct content *c, void *context)
{
	gif_content *gif = (gif_content *) c;
	nsgif_bitmap_t *bitmap;

	if (gif_get_frame(gif, &bitmap) != NSGIF_OK) {
		return NULL;
	}

	return bitmap;
}

static content_type gif_content_type(void)
{
	return CONTENT_IMAGE;
}

static bool gif_content_is_opaque(struct content *c)
{
	gif_content *gif = (gif_content *) c;
	nsgif_bitmap_t *bitmap;

	if (gif_get_frame(gif, &bitmap) != NSGIF_OK) {
		return false;
	}

	return guit->bitmap->get_opaque(bitmap);
}

static const content_handler gif_content_handler = {
	.create = gif_create,
	.data_complete = gif_convert,
	.destroy = gif_destroy,
	.redraw = gif_redraw,
	.clone = gif_clone,
	.add_user = gif_add_user,
	.remove_user = gif_remove_user,
	.get_internal = gif_get_bitmap,
	.type = gif_content_type,
	.is_opaque = gif_content_is_opaque,
	.no_share = false,
};

static const char *gif_types[] = {
	"image/gif"
};

CONTENT_FACTORY_REGISTER_TYPES(nsgif, gif_types, gif_content_handler);
