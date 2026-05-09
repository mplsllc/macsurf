/*
 * Copyright 2023 Vincent Sanders <vince@netsurf-browser.org>
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
 * implementation of content handling for image/jpegxl
 *
 * This implementation uses the JXL library.
 */

#include <stdbool.h>
#include <stdlib.h>
#include <setjmp.h>
#include <string.h>

#include <jxl/decode.h>

#include "utils/utils.h"
#include "utils/log.h"
#include "utils/messages.h"
#include "netsurf/bitmap.h"
#include "content/llcache.h"
#include "content/content.h"
#include "content/content_protected.h"
#include "content/content_factory.h"
#include "desktop/gui_internal.h"
#include "desktop/bitmap.h"

#include "image/image_cache.h"

#include "image/jpegxl.h"


/**
 * output image format
 */
static const JxlPixelFormat jxl_output_format = {
	.num_channels = 4,
	.data_type    = JXL_TYPE_UINT8,
	.endianness   = JXL_LITTLE_ENDIAN,
	.align        = 0,
};

/**
 * Content create entry point.
 */
static nserror
nsjpegxl_create(const content_handler *handler,
		lwc_string *imime_type, const struct http_parameter *params,
		llcache_handle *llcache, const char *fallback_charset,
		bool quirks, struct content **c)
{
	struct content *jpeg;
	nserror error;

	jpeg = calloc(1, sizeof(struct content));
	if (jpeg == NULL)
		return NSERROR_NOMEM;

	error = content__init(jpeg, handler, imime_type, params,
			      llcache, fallback_charset, quirks);
	if (error != NSERROR_OK) {
		free(jpeg);
		return error;
	}

	*c = jpeg;

	return NSERROR_OK;
}

/**
 * create a bitmap from jpeg xl content.
 */
static struct bitmap *
jpegxl_cache_convert(struct content *c)
{
	struct bitmap * bitmap = NULL;
	JxlDecoder *jxldec;
	JxlDecoderStatus decstatus;
	JxlBasicInfo binfo;
	const uint8_t *src_data;
	size_t src_size;
	uint8_t * output;
	bitmap_fmt_t jxl_fmt = {
		/** TODO: At the moment we have to set the layout to the only
		 *        pixel layout that libjxl supports. It looks like they
		 *        plan to add support for decoding to other layouts
		 *        in the future, as shown by the TODO in the docs:
		 *
		 * https://libjxl.readthedocs.io/en/latest/api_common.html#_CPPv414JxlPixelFormat
		 */
		.layout = BITMAP_LAYOUT_R8G8B8A8,
		.pma    = bitmap_fmt.pma,
	};

	jxldec = JxlDecoderCreate(NULL);
	if (jxldec == NULL) {
		NSLOG(netsurf, ERROR, "Unable to allocate decoder");
		return NULL;
	}

	decstatus = JxlDecoderSetUnpremultiplyAlpha(jxldec, !bitmap_fmt.pma);
	if (decstatus != JXL_DEC_SUCCESS) {
		NSLOG(netsurf, ERROR, "unable to set premultiplied alpha status: %d",
				decstatus);
		JxlDecoderDestroy(jxldec);
		return NULL;
	}

	decstatus= JxlDecoderSubscribeEvents(jxldec, JXL_DEC_FULL_IMAGE);
	if (decstatus != JXL_DEC_SUCCESS) {
		NSLOG(netsurf, ERROR, "Unable to subscribe");
		return NULL;
	}
	src_data = content__get_source_data(c, &src_size);

	decstatus = JxlDecoderSetInput(jxldec, src_data, src_size);
	if (decstatus != JXL_DEC_SUCCESS) {
		NSLOG(netsurf, ERROR, "unable to set input");
		return NULL;
	}

	decstatus = JxlDecoderProcessInput(jxldec);
	if (decstatus != JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
		NSLOG(netsurf, ERROR,
		      "expected status JXL_DEC_NEED_IMAGE_OUT_BUFFER(%d) got %d",
		      JXL_DEC_NEED_IMAGE_OUT_BUFFER,
		      decstatus);
		JxlDecoderDestroy(jxldec);
		return NULL;
	}

	decstatus = JxlDecoderGetBasicInfo(jxldec, &binfo);
	if (decstatus != JXL_DEC_SUCCESS) {
		NSLOG(netsurf, ERROR, "unable to get basic info status:%d",decstatus);
		JxlDecoderDestroy(jxldec);
		return NULL;
	}

	/* create bitmap with appropriate opacity */
	if (binfo.alpha_bits > 0) {
		bitmap = guit->bitmap->create(c->width, c->height, BITMAP_OPAQUE);
	} else {
		bitmap = guit->bitmap->create(c->width, c->height, BITMAP_NONE);
	}
	if (bitmap == NULL) {
		/* empty bitmap could not be created */
		JxlDecoderDestroy(jxldec);
		return NULL;
	}

	/* ensure buffer was allocated */
	output = guit->bitmap->get_buffer(bitmap);
	if (output == NULL) {
		/* bitmap with no buffer available */
		guit->bitmap->destroy(bitmap);
		JxlDecoderDestroy(jxldec);
		return NULL;
	}
	decstatus = JxlDecoderSetImageOutBuffer(jxldec, &jxl_output_format, output, c->size);
	if (decstatus != JXL_DEC_SUCCESS) {
		NSLOG(netsurf, ERROR, "unable to set output buffer callback status:%d",decstatus);
		guit->bitmap->destroy(bitmap);
		JxlDecoderDestroy(jxldec);
		return NULL;
	}

	decstatus = JxlDecoderProcessInput(jxldec);
	if (decstatus != JXL_DEC_FULL_IMAGE) {
		NSLOG(netsurf, ERROR, "did not get decode event");
		guit->bitmap->destroy(bitmap);
		JxlDecoderDestroy(jxldec);
		return NULL;
	}
	
	JxlDecoderDestroy(jxldec);

	bitmap_format_to_client(bitmap, &jxl_fmt);
	guit->bitmap->modified(bitmap);

	return bitmap;
}

/**
 * report failiure
 */
static bool jxl_report_fail(struct content *c, JxlDecoderStatus decstatus, const char *msg)
{
	union content_msg_data msg_data;
	NSLOG(netsurf, ERROR, "%s decoder status:%d", msg, decstatus);
	msg_data.errordata.errorcode = NSERROR_UNKNOWN;
	msg_data.errordata.errormsg = msg;
	content_broadcast(c, CONTENT_MSG_ERROR, &msg_data);
	return false;
}

/**
 * Convert a CONTENT_JPEGXL for display.
 */
static bool nsjpegxl_convert(struct content *c)
{
	JxlDecoder *jxldec;
	JxlSignature decsig;
	JxlDecoderStatus decstatus = JXL_DEC_ERROR;
	JxlBasicInfo binfo;
	union content_msg_data msg_data;
	const uint8_t *data;
	size_t size;
	char *title;
	size_t image_size;

	/* check image header is valid and get width/height */
	data = content__get_source_data(c, &size);

	decsig = JxlSignatureCheck(data,size);
	if ((decsig != JXL_SIG_CODESTREAM) && (decsig != JXL_SIG_CONTAINER)) {
		NSLOG(netsurf, ERROR, "signature failed");
		msg_data.errordata.errorcode = NSERROR_UNKNOWN;
		msg_data.errordata.errormsg = "Signature failed";
		content_broadcast(c, CONTENT_MSG_ERROR, &msg_data);
		return false;
	}

	jxldec = JxlDecoderCreate(NULL);
	if (jxldec == NULL) {
		return jxl_report_fail(c, decstatus, "Unable to allocate decoder");
	}
	decstatus= JxlDecoderSubscribeEvents(jxldec, JXL_DEC_BASIC_INFO);
	if (decstatus != JXL_DEC_SUCCESS) {
		return jxl_report_fail(c, decstatus, "Unable to subscribe");
	}
	decstatus = JxlDecoderSetInput(jxldec, data,size);
	if (decstatus != JXL_DEC_SUCCESS) {
		return jxl_report_fail(c, decstatus, "unable to set input");
	}
	decstatus = JxlDecoderProcessInput(jxldec);
	if (decstatus != JXL_DEC_BASIC_INFO) {
		return jxl_report_fail(c, decstatus, "did not get basic info event");
	}
	decstatus = JxlDecoderGetBasicInfo(jxldec, &binfo);
	if (decstatus != JXL_DEC_SUCCESS) {
		return jxl_report_fail(c, decstatus, "unable to get basic info");
	}
	decstatus = JxlDecoderImageOutBufferSize(jxldec, &jxl_output_format, &image_size);
	if (decstatus != JXL_DEC_SUCCESS) {
		return jxl_report_fail(c, decstatus, "unable get image size");
	}
	
	JxlDecoderDestroy(jxldec);

	NSLOG(netsurf, INFO, "got basic info size:%ld x:%d y:%d", image_size, binfo.xsize, binfo.ysize);

	c->width = binfo.xsize;
	c->height = binfo.ysize;
	c->size = image_size;

	image_cache_add(c, NULL, jpegxl_cache_convert);

	/* set title text */
	title = messages_get_buff("JPEGXLTitle",
			nsurl_access_leaf(llcache_handle_get_url(c->llcache)),
			c->width, c->height);
	if (title != NULL) {
		content__set_title(c, title);
		free(title);
	}

	content_set_ready(c);
	content_set_done(c);	
	content_set_status(c, ""); /* Done: update status bar */

	return true;
}


/**
 * Clone content.
 */
static nserror nsjpegxl_clone(const struct content *old, struct content **newc)
{
	struct content *jpegxl_c;
	nserror error;

	jpegxl_c = calloc(1, sizeof(struct content));
	if (jpegxl_c == NULL)
		return NSERROR_NOMEM;

	error = content__clone(old, jpegxl_c);
	if (error != NSERROR_OK) {
		content_destroy(jpegxl_c);
		return error;
	}

	/* re-convert if the content is ready */
	if ((old->status == CONTENT_STATUS_READY) ||
	    (old->status == CONTENT_STATUS_DONE)) {
		if (nsjpegxl_convert(jpegxl_c) == false) {
			content_destroy(jpegxl_c);
			return NSERROR_CLONE_FAILED;
		}
	}

	*newc = jpegxl_c;

	return NSERROR_OK;
}

static const content_handler nsjpegxl_content_handler = {
	.create = nsjpegxl_create,
	.data_complete = nsjpegxl_convert,
	.destroy = image_cache_destroy,
	.redraw = image_cache_redraw,
	.clone = nsjpegxl_clone,
	.get_internal = image_cache_get_internal,
	.type = image_cache_content_type,
	.is_opaque = image_cache_is_opaque,
	.no_share = false,
};

static const char *nsjpegxl_types[] = {
	"image/jxl",
};

CONTENT_FACTORY_REGISTER_TYPES(nsjpegxl, nsjpegxl_types, nsjpegxl_content_handler);
