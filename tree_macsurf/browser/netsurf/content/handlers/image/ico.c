/*
 * Copyright 2006 Richard Wilson <info@tinct.net>
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
 * implementation for image/ico content handler
 */

#include <stdbool.h>
#include <stdlib.h>
#include <libnsbmp.h>

#include "utils/utils.h"
#include "utils/log.h"
#include "utils/messages.h"
#include "netsurf/bitmap.h"
#include "netsurf/content.h"
#include "content/llcache.h"
#include "content/content_protected.h"
#include "content/content_factory.h"
#include "desktop/gui_internal.h"
#include "desktop/bitmap.h"

#include "image/image.h"
#include "image/ico.h"

typedef struct nsico_content {
	struct content base;

	struct ico_collection *ico; /** ICO collection data */
} nsico_content;

/**
 * Callback for libnsbmp; forwards the call to bitmap_create()
 *
 * \param  width   width of image in pixels
 * \param  height  width of image in pixels
 * \param  bmp_state A flag word indicating the initial state
 * \return an opaque struct bitmap, or NULL on memory exhaustion
 */
static void *nsico_bitmap_create(int width, int height, unsigned int bmp_state)
{
	unsigned int bitmap_state = BITMAP_NONE;

	/* set bitmap state based on bmp state */
	bitmap_state |= (bmp_state & BMP_OPAQUE) ? BITMAP_OPAQUE : 0;
	bitmap_state |= (bmp_state & BMP_CLEAR_MEMORY) ?
			BITMAP_CLEAR : 0;

	/* return the created bitmap */
	return guit->bitmap->create(width, height, bitmap_state);
}

static nserror nsico_create_ico_data(nsico_content *c)
{
	bmp_bitmap_callback_vt bmp_bitmap_callbacks = {
		.bitmap_create = nsico_bitmap_create,
		.bitmap_destroy = guit->bitmap->destroy,
		.bitmap_get_buffer = guit->bitmap->get_buffer,
	};

	c->ico = calloc(1, sizeof(ico_collection));
	if (c->ico == NULL) {
		content_broadcast_error(&c->base, NSERROR_NOMEM, NULL);
		return NSERROR_NOMEM;
	}
	ico_collection_create(c->ico, &bmp_bitmap_callbacks);
	return NSERROR_OK;
}


static nserror nsico_create(const content_handler *handler,
		lwc_string *imime_type, const struct http_parameter *params,
		llcache_handle *llcache, const char *fallback_charset,
		bool quirks, struct content **c)
{
	nsico_content *result;
	nserror error;

	result = calloc(1, sizeof(nsico_content));
	if (result == NULL)
		return NSERROR_NOMEM;

	error = content__init(&result->base, handler, imime_type, params,
			llcache, fallback_charset, quirks);
	if (error != NSERROR_OK) {
		free(result);
		return error;
	}

	error = nsico_create_ico_data(result);
	if (error != NSERROR_OK) {
		free(result);
		return error;
	}

	*c = (struct content *) result;

	return NSERROR_OK;
}



static bool nsico_convert(struct content *c)
{
	nsico_content *ico = (nsico_content *) c;
	struct bmp_image *bmp;
	bmp_result res;
	const uint8_t *data;
	size_t size;
	char *title;

	/* set the ico data */
	data = content__get_source_data(c, &size);

	/* analyse the ico */
	res = ico_analyse(ico->ico, size, (unsigned char *) data);

	switch (res) {
	case BMP_OK:
		break;
	case BMP_INSUFFICIENT_MEMORY:
		content_broadcast_error(c, NSERROR_NOMEM, NULL);
		return false;
	case BMP_INSUFFICIENT_DATA:
	case BMP_DATA_ERROR:
		content_broadcast_error(c, NSERROR_ICO_ERROR, NULL);
		return false;
	}

	/* Store our content width, height and calculate size */
	c->width = ico->ico->width;
	c->height = ico->ico->height;
	c->size += (ico->ico->width * ico->ico->height * 4) + 16 + 44;

	/* set title text */
	title = messages_get_buff("ICOTitle",
			nsurl_access_leaf(llcache_handle_get_url(c->llcache)),
			c->width, c->height);
	if (title != NULL) {
		content__set_title(c, title);
		free(title);
	}

	/* select largest icon to ensure one can be selected */
	bmp = ico_find(ico->ico, 255, 255);
	if (bmp == NULL) {
		/* return error */
		NSLOG(netsurf, INFO, "Failed to select icon");
		return false;
	}

	content_set_ready(c);
	content_set_done(c);

	/* Done: update status bar */
	content_set_status(c, "");
	return true;
}

static bool nsico__decode(struct bmp_image *ico)
{
	if (ico->decoded == false) {
		NSLOG(netsurf, DEBUG, "Decoding ICO %p", ico);
		if (bmp_decode(ico) != BMP_OK) {
			return false;
		}

		bitmap_format_to_client(ico->bitmap, &(bitmap_fmt_t) {
			.layout = BITMAP_LAYOUT_R8G8B8A8,
		});
		guit->bitmap->modified(ico->bitmap);

	}

	return true;
}

static bool nsico_redraw(struct content *c, struct content_redraw_data *data,
		const struct rect *clip, const struct redraw_context *ctx)
{
	nsico_content *ico = (nsico_content *)c;
	struct bmp_image *bmp;

	/* select most appropriate sized icon for size */
	bmp = ico_find(ico->ico, data->width, data->height);
	if (bmp == NULL) {
		/* return error */
		NSLOG(netsurf, INFO, "Failed to select icon");
		return false;
	}

	/* ensure its decided */
	if (!nsico__decode(bmp)) {
		return false;
	}

	return image_bitmap_plot(bmp->bitmap, data, clip, ctx);
}


static void nsico_destroy(struct content *c)
{
	nsico_content *ico = (nsico_content *) c;

	ico_finalise(ico->ico);
	free(ico->ico);
}

static nserror nsico_clone(const struct content *old, struct content **newc)
{
	nsico_content *ico;
	nserror error;

	ico = calloc(1, sizeof(nsico_content));
	if (ico == NULL)
		return NSERROR_NOMEM;

	error = content__clone(old, &ico->base);
	if (error != NSERROR_OK) {
		content_destroy(&ico->base);
		return error;
	}

	/* Simply replay creation and conversion */
	error = nsico_create_ico_data(ico);
	if (error != NSERROR_OK) {
		content_destroy(&ico->base);
		return error;
	}

	if (old->status == CONTENT_STATUS_READY ||
			old->status == CONTENT_STATUS_DONE) {
		if (nsico_convert(&ico->base) == false) {
			content_destroy(&ico->base);
			return NSERROR_CLONE_FAILED;
		}
	}

	*newc = (struct content *) ico;

	return NSERROR_OK;
}

static void *nsico_get_internal(const struct content *c, void *context)
{
	nsico_content *ico = (nsico_content *) c;
	/* TODO: Pick best size for purpose.
	 *       Currently assumes it's for a URL bar. */
	struct bmp_image *bmp;

	bmp = ico_find(ico->ico, 16, 16);
	if (bmp == NULL) {
		/* return error */
		NSLOG(netsurf, INFO, "Failed to select icon");
		return NULL;
	}

	if (!nsico__decode(bmp)) {
		return NULL;
	}

	return bmp->bitmap;
}

static content_type nsico_content_type(void)
{
	return CONTENT_IMAGE;
}

static bool nsico_is_opaque(struct content *c)
{
	nsico_content *ico = (nsico_content *) c;
	struct bmp_image *bmp;

	/**
	 * \todo Pick best size for purpose. Currently assumes
	 *         it's for a URL bar.
	 */
	bmp = ico_find(ico->ico, 16, 16);
	if (bmp == NULL) {
		/* return error */
		NSLOG(netsurf, INFO, "Failed to select icon");
		return false;
	}

	if (!nsico__decode(bmp)) {
		return false;
	}

	return guit->bitmap->get_opaque(bmp->bitmap);
}

static const content_handler nsico_content_handler = {
	.create = nsico_create,
	.data_complete = nsico_convert,
	.destroy = nsico_destroy,
	.redraw = nsico_redraw,
	.clone = nsico_clone,
	.get_internal = nsico_get_internal,
	.type = nsico_content_type,
	.is_opaque = nsico_is_opaque,
	.no_share = false,
};

static const char *nsico_types[] = {
	"application/ico",
	"application/x-ico",
	"image/ico",
	"image/vnd.microsoft.icon",
	"image/x-icon"
};

CONTENT_FACTORY_REGISTER_TYPES(nsico, nsico_types, nsico_content_handler);
