/*
 * Copyright 2019 Vincent Sanders <vince@netsurf-browser.org>
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
 * implementation of content handling for image/webp
 *
 * This implementation uses the google webp library.
 * Image cache handling is performed by the generic NetSurf handler.
 */

#include <stdbool.h>
#include <stdlib.h>
#include <setjmp.h>

#include <webp/decode.h>

#include "utils/utils.h"
#include "utils/log.h"
#include "utils/messages.h"
#include "netsurf/bitmap.h"
#include "content/llcache.h"
#include "content/content_protected.h"
#include "content/content_factory.h"
#include "desktop/gui_internal.h"
#include "desktop/bitmap.h"

#include "image/image_cache.h"

#include "webp.h"

/**
 * Content create entry point.
 *
 * create a content object for the webp
 */
static nserror
webp_create(const content_handler *handler,
	      lwc_string *imime_type,
	      const struct http_parameter *params,
	      llcache_handle *llcache,
	      const char *fallback_charset,
	      bool quirks,
	      struct content **c)
{
	struct content *webp_c; /* webp content object */
	nserror res;

	webp_c = calloc(1, sizeof(struct content));
	if (webp_c == NULL) {
		return NSERROR_NOMEM;
	}

	res = content__init(webp_c,
			    handler,
			    imime_type,
			    params,
			    llcache,
			    fallback_charset,
			    quirks);
	if (res != NSERROR_OK) {
		free(webp_c);
		return res;
	}

	*c = webp_c;

	return NSERROR_OK;
}

/**
 * create a bitmap from webp content.
 */
static struct bitmap *
webp_cache_convert(struct content *c)
{
	const uint8_t *source_data; /* webp source data */
	size_t source_size; /* length of webp source data */
	VP8StatusCode webpres;
	WebPBitstreamFeatures webpfeatures;
	unsigned int bmap_flags;
	uint8_t *pixels = NULL;
	uint8_t *decoded;
	size_t rowstride;
	struct bitmap *bitmap = NULL;
	bitmap_fmt_t webp_fmt = {
		.layout = bitmap_fmt.layout,
	};

	source_data = content__get_source_data(c, &source_size);

	webpres = WebPGetFeatures(source_data, source_size, &webpfeatures);

	if (webpres != VP8_STATUS_OK) {
		return NULL;
	}

	if (webpfeatures.has_alpha == 0) {
		bmap_flags = BITMAP_OPAQUE;
		/* Image has no alpha. Premultiplied alpha makes no difference.
		 * Optimisation: Avoid unnecessary conversion by copying format.
		 */
		webp_fmt.pma = bitmap_fmt.pma;
	} else {
		bmap_flags = BITMAP_NONE;
	}

	/* create bitmap */
	bitmap = guit->bitmap->create(webpfeatures.width,
				      webpfeatures.height,
				      bmap_flags);
	if (bitmap == NULL) {
		/* empty bitmap could not be created */
		return NULL;
	}

	pixels = guit->bitmap->get_buffer(bitmap);
	if (pixels == NULL) {
		/* bitmap with no buffer available */
		guit->bitmap->destroy(bitmap);
		return NULL;
	}

	rowstride = guit->bitmap->get_rowstride(bitmap);

	switch (webp_fmt.layout) {
	default:
		/* WebP has no ABGR function, fall back to default. */
		webp_fmt.layout = BITMAP_LAYOUT_R8G8B8A8;
		fallthrough;
	case BITMAP_LAYOUT_R8G8B8A8:
		decoded = WebPDecodeRGBAInto(source_data, source_size, pixels,
				rowstride * webpfeatures.height, rowstride);
		break;

	case BITMAP_LAYOUT_B8G8R8A8:
		decoded = WebPDecodeBGRAInto(source_data, source_size, pixels,
				rowstride * webpfeatures.height, rowstride);
		break;

	case BITMAP_LAYOUT_A8R8G8B8:
		decoded = WebPDecodeARGBInto(source_data, source_size, pixels,
				rowstride * webpfeatures.height, rowstride);
		break;
	}
	if (decoded == NULL) {
		/* decode failed */
		guit->bitmap->destroy(bitmap);
		return NULL;
	}

	bitmap_format_to_client(bitmap, &webp_fmt);
	guit->bitmap->modified(bitmap);

	return bitmap;
}

/**
 * Convert the webp source data content.
 *
 * This ensures there is valid webp source data in the content object
 *  and then adds it to the image cache ready to be converted on
 *  demand.
 *
 * \param c The webp content object
 * \return true on successful processing of teh webp content else false
 */
static bool webp_convert(struct content *c)
{
	int res;
	const uint8_t* data;
	size_t data_size;
	int width;
	int height;

	data = content__get_source_data(c, &data_size);

	res = WebPGetInfo(data, data_size, &width, &height);
	if (res == 0) {
		NSLOG(netsurf, INFO, "WebPGetInfo failed:%p", c);
		return false;
	}

	c->width = width;
	c->height = height;
	c->size = c->width * c->height * 4;

	image_cache_add(c, NULL, webp_cache_convert);

	content_set_ready(c);
	content_set_done(c);

	return true;
}

/**
 * Clone content.
 */
static nserror webp_clone(const struct content *old, struct content **new_c)
{
	struct content *webp_c; /* cloned webp content */
	nserror res;

	webp_c = calloc(1, sizeof(struct content));
	if (webp_c == NULL) {
		return NSERROR_NOMEM;
	}

	res = content__clone(old, webp_c);
	if (res != NSERROR_OK) {
		content_destroy(webp_c);
		return res;
	}

	/* re-convert if the content is ready */
	if ((old->status == CONTENT_STATUS_READY) ||
	    (old->status == CONTENT_STATUS_DONE)) {
		if (webp_convert(webp_c) == false) {
			content_destroy(webp_c);
			return NSERROR_CLONE_FAILED;
		}
	}

	*new_c = webp_c;

	return NSERROR_OK;
}

static const content_handler webp_content_handler = {
	.create = webp_create,
	.data_complete = webp_convert,
	.destroy = image_cache_destroy,
	.redraw = image_cache_redraw,
	.clone = webp_clone,
	.get_internal = image_cache_get_internal,
	.type = image_cache_content_type,
	.is_opaque = image_cache_is_opaque,
	.no_share = false,
};

static const char *webp_types[] = {
	"image/webp"
};

CONTENT_FACTORY_REGISTER_TYPES(nswebp, webp_types, webp_content_handler);
