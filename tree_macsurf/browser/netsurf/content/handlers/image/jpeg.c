/*
 * Copyright 2004 James Bursa <bursa@users.sourceforge.net>
 * Copyright 2004 John M Bell <jmb202@ecs.soton.ac.uk>
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
 * implementation of content handling for image/jpeg
 *
 * This implementation uses the IJG JPEG library.
 */

#include <stdbool.h>
#include <stdlib.h>
#include <setjmp.h>

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

#define JPEG_INTERNAL_OPTIONS
#include "jpeglib.h"
#include "jpeg.h"

/** absolute minimum size of a jpeg below which it is not even worth
 * trying to read header data
 */
#define MIN_JPEG_SIZE 20

#ifndef LIBJPEG_TURBO_VERSION
#warning Using libjpeg (libjpeg-turbo is recommended)
#endif

static char nsjpeg_error_buffer[JMSG_LENGTH_MAX];

static unsigned char nsjpeg_eoi[] = { 0xff, JPEG_EOI };

/**
 * Content create entry point.
 */
static nserror nsjpeg_create(const content_handler *handler,
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
 * JPEG data source manager: initialize source.
 */
static void nsjpeg_init_source(j_decompress_ptr cinfo)
{
}


/**
 * JPEG data source manager: fill the input buffer.
 *
 * This can only occur if the JPEG data was truncated or corrupted. Insert a
 * fake EOI marker to allow the decompressor to output as much as possible.
 */
static boolean nsjpeg_fill_input_buffer(j_decompress_ptr cinfo)
{
	cinfo->src->next_input_byte = nsjpeg_eoi;
	cinfo->src->bytes_in_buffer = 2;
 	return TRUE;
}


/**
 * JPEG data source manager: skip num_bytes worth of data.
 */

static void nsjpeg_skip_input_data(j_decompress_ptr cinfo, long num_bytes)
{
	if ((long) cinfo->src->bytes_in_buffer < num_bytes) {
		cinfo->src->next_input_byte = 0;
		cinfo->src->bytes_in_buffer = 0;
	} else {
		cinfo->src->next_input_byte += num_bytes;
		cinfo->src->bytes_in_buffer -= num_bytes;
	}
}


/**
 * JPEG data source manager: terminate source.
 */
static void nsjpeg_term_source(j_decompress_ptr cinfo)
{
}


/**
 * Error output handler for JPEG library.
 *
 * This logs to NetSurf log instead of stderr.
 * Warnings only - fatal errors are trapped by nsjpeg_error_exit
 *                 and do not call the output handler.
 */
static void nsjpeg_error_log(j_common_ptr cinfo)
{
	cinfo->err->format_message(cinfo, nsjpeg_error_buffer);
	NSLOG(netsurf, INFO, "%s", nsjpeg_error_buffer);
}


/**
 * Fatal error handler for JPEG library.
 *
 * This prevents jpeglib calling exit() on a fatal error.
 */
static void nsjpeg_error_exit(j_common_ptr cinfo)
{
	jmp_buf *setjmp_buffer = (jmp_buf *) cinfo->client_data;

	cinfo->err->format_message(cinfo, nsjpeg_error_buffer);
	NSLOG(netsurf, INFO, "%s", nsjpeg_error_buffer);

	longjmp(*setjmp_buffer, 1);
}

/**
 * Convert scan lines from CMYK to core client bitmap layout.
 */
static inline void nsjpeg__decode_cmyk(
		struct jpeg_decompress_struct *cinfo,
		uint8_t * volatile pixels,
		size_t rowstride)
{
	int width = cinfo->output_width * 4;

	do {
		JSAMPROW scanlines[1] = {
			[0] = (JSAMPROW)
				(pixels + rowstride * cinfo->output_scanline),
		};
		jpeg_read_scanlines(cinfo, scanlines, 1);

		for (int i = width - 4; 0 <= i; i -= 4) {
			/* Trivial inverse CMYK -> RGBA */
			const int c = scanlines[0][i + 0];
			const int m = scanlines[0][i + 1];
			const int y = scanlines[0][i + 2];
			const int k = scanlines[0][i + 3];

			const int ck = c * k;
			const int mk = m * k;
			const int yk = y * k;

#define DIV255(x) ((x) + 1 + ((x) >> 8)) >> 8
			scanlines[0][i + bitmap_layout.r] = DIV255(ck);
			scanlines[0][i + bitmap_layout.g] = DIV255(mk);
			scanlines[0][i + bitmap_layout.b] = DIV255(yk);
			scanlines[0][i + bitmap_layout.a] = 0xff;
#undef DIV255
		}
	} while (cinfo->output_scanline != cinfo->output_height);
}

/**
 * Convert scan lines from CMYK to core client bitmap layout.
 */
static inline void nsjpeg__decode_rgb(
		struct jpeg_decompress_struct *cinfo,
		uint8_t * volatile pixels,
		size_t rowstride)
{
#if RGB_RED != 0 || RGB_GREEN != 1 || RGB_BLUE != 2 || RGB_PIXELSIZE != 4
	int width = cinfo->output_width;
#endif

	do {
		JSAMPROW scanlines[1] = {
			[0] = (JSAMPROW)
				(pixels + rowstride * cinfo->output_scanline),
		};
		jpeg_read_scanlines(cinfo, scanlines, 1);

#if RGB_RED != 0 || RGB_GREEN != 1 || RGB_BLUE != 2 || RGB_PIXELSIZE != 4
		/* Missmatch between configured libjpeg pixel format and
		 * NetSurf pixel format.  Convert to RGBA */
		for (int i = width - 1; 0 <= i; i--) {
			int r = scanlines[0][i * RGB_PIXELSIZE + RGB_RED];
			int g = scanlines[0][i * RGB_PIXELSIZE + RGB_GREEN];
			int b = scanlines[0][i * RGB_PIXELSIZE + RGB_BLUE];
			scanlines[0][i * 4 + bitmap_layout.r] = r;
			scanlines[0][i * 4 + bitmap_layout.g] = g;
			scanlines[0][i * 4 + bitmap_layout.b] = b;
			scanlines[0][i * 4 + bitmap_layout.a] = 0xff;
		}
#endif
	} while (cinfo->output_scanline != cinfo->output_height);
}

/**
 * Convert scan lines from CMYK to core client bitmap layout.
 */
static inline void nsjpeg__decode_client_fmt(
		struct jpeg_decompress_struct *cinfo,
		uint8_t * volatile pixels,
		size_t rowstride)
{
	do {
		JSAMPROW scanlines[1] = {
			[0] = (JSAMPROW)
				(pixels + rowstride * cinfo->output_scanline),
		};
		jpeg_read_scanlines(cinfo, scanlines, 1);
	} while (cinfo->output_scanline != cinfo->output_height);
}

/**
 * create a bitmap from jpeg content.
 */
static struct bitmap *
jpeg_cache_convert(struct content *c)
{
	const uint8_t *source_data; /* Jpeg source data */
	size_t source_size; /* length of Jpeg source data */
	struct jpeg_decompress_struct cinfo;
	struct jpeg_error_mgr jerr;
	jmp_buf setjmp_buffer;
	struct bitmap * volatile bitmap = NULL;
	uint8_t * volatile pixels = NULL;
	size_t rowstride;
	struct jpeg_source_mgr source_mgr = {
		0,
		0,
		nsjpeg_init_source,
		nsjpeg_fill_input_buffer,
		nsjpeg_skip_input_data,
		jpeg_resync_to_restart,
		nsjpeg_term_source };

	/* obtain jpeg source data and perfom minimal sanity checks */
	source_data = content__get_source_data(c, &source_size);

	if ((source_data == NULL) ||
	    (source_size < MIN_JPEG_SIZE)) {
		return NULL;
	}

	/* setup a JPEG library error handler */
	cinfo.err = jpeg_std_error(&jerr);
	jerr.error_exit = nsjpeg_error_exit;
	jerr.output_message = nsjpeg_error_log;

	/* handler for fatal errors during decompression */
	if (setjmp(setjmp_buffer)) {
		jpeg_destroy_decompress(&cinfo);
		return bitmap;
	}

	cinfo.client_data = &setjmp_buffer;
	jpeg_create_decompress(&cinfo);

	/* setup data source */
	source_mgr.next_input_byte = source_data;
	source_mgr.bytes_in_buffer = source_size;
	cinfo.src = &source_mgr;

	/* read JPEG header information */
	jpeg_read_header(&cinfo, TRUE);

	/* set output processing parameters */
	if (cinfo.jpeg_color_space == JCS_CMYK ||
	    cinfo.jpeg_color_space == JCS_YCCK) {
		cinfo.out_color_space = JCS_CMYK;
	} else {
#ifdef JCS_ALPHA_EXTENSIONS
		switch (bitmap_fmt.layout) {
		case BITMAP_LAYOUT_R8G8B8A8:
			cinfo.out_color_space = JCS_EXT_RGBA;
			break;
		case BITMAP_LAYOUT_B8G8R8A8:
			cinfo.out_color_space = JCS_EXT_BGRA;
			break;
		case BITMAP_LAYOUT_A8R8G8B8:
			cinfo.out_color_space = JCS_EXT_ARGB;
			break;
		case BITMAP_LAYOUT_A8B8G8R8:
			cinfo.out_color_space = JCS_EXT_ABGR;
			break;
		default:
			NSLOG(netsurf, ERROR, "Unexpected bitmap format: %u",
					bitmap_fmt.layout);
			jpeg_destroy_decompress(&cinfo);
			return NULL;
		}
#else
		cinfo.out_color_space = JCS_RGB;
#endif
	}
	cinfo.dct_method = JDCT_ISLOW;

	/* commence the decompression, output parameters now valid */
	jpeg_start_decompress(&cinfo);

	/* create opaque bitmap (jpegs cannot be transparent) */
	bitmap = guit->bitmap->create(
			cinfo.output_width,
			cinfo.output_height, BITMAP_OPAQUE);
	if (bitmap == NULL) {
		/* empty bitmap could not be created */
		jpeg_destroy_decompress(&cinfo);
		return NULL;
	}

	pixels = guit->bitmap->get_buffer(bitmap);
	if (pixels == NULL) {
		/* bitmap with no buffer available */
		guit->bitmap->destroy(bitmap);
		jpeg_destroy_decompress(&cinfo);
		return NULL;
	}

	/* Convert scanlines from jpeg into bitmap */
	rowstride = guit->bitmap->get_rowstride(bitmap);

	switch (cinfo.out_color_space) {
	case JCS_CMYK:
		nsjpeg__decode_cmyk(&cinfo, pixels, rowstride);
		break;

	case JCS_RGB:
		nsjpeg__decode_rgb(&cinfo, pixels, rowstride);
		break;

	default:
		nsjpeg__decode_client_fmt(&cinfo, pixels, rowstride);
		break;
	}

	guit->bitmap->modified(bitmap);

	jpeg_finish_decompress(&cinfo);
	jpeg_destroy_decompress(&cinfo);

	return bitmap;
}

/**
 * Convert a CONTENT_JPEG for display.
 */
static bool nsjpeg_convert(struct content *c)
{
	struct jpeg_decompress_struct cinfo;
	struct jpeg_error_mgr jerr;
	jmp_buf setjmp_buffer;
	struct jpeg_source_mgr source_mgr = { 0, 0,
		nsjpeg_init_source, nsjpeg_fill_input_buffer,
		nsjpeg_skip_input_data, jpeg_resync_to_restart,
		nsjpeg_term_source };
	union content_msg_data msg_data;
	const uint8_t *data;
	size_t size;
	char *title;

	/* check image header is valid and get width/height */
	data = content__get_source_data(c, &size);

	cinfo.err = jpeg_std_error(&jerr);
	jerr.error_exit = nsjpeg_error_exit;
	jerr.output_message = nsjpeg_error_log;

	if (setjmp(setjmp_buffer)) {
		jpeg_destroy_decompress(&cinfo);

		msg_data.errordata.errorcode = NSERROR_UNKNOWN;
		msg_data.errordata.errormsg = nsjpeg_error_buffer;
		content_broadcast(c, CONTENT_MSG_ERROR, &msg_data);
		return false;
	}

	cinfo.client_data = &setjmp_buffer;
	jpeg_create_decompress(&cinfo);
	source_mgr.next_input_byte = (unsigned char *) data;
	source_mgr.bytes_in_buffer = size;
	cinfo.src = &source_mgr;
	jpeg_read_header(&cinfo, TRUE);
	cinfo.out_color_space = JCS_RGB;
	cinfo.dct_method = JDCT_ISLOW;

	jpeg_calc_output_dimensions(&cinfo);

	c->width = cinfo.output_width;
	c->height = cinfo.output_height;
	c->size = c->width * c->height * 4;

	jpeg_destroy_decompress(&cinfo);

	image_cache_add(c, NULL, jpeg_cache_convert);

	/* set title text */
	title = messages_get_buff("JPEGTitle",
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
static nserror nsjpeg_clone(const struct content *old, struct content **newc)
{
	struct content *jpeg_c;
	nserror error;

	jpeg_c = calloc(1, sizeof(struct content));
	if (jpeg_c == NULL)
		return NSERROR_NOMEM;

	error = content__clone(old, jpeg_c);
	if (error != NSERROR_OK) {
		content_destroy(jpeg_c);
		return error;
	}

	/* re-convert if the content is ready */
	if ((old->status == CONTENT_STATUS_READY) ||
	    (old->status == CONTENT_STATUS_DONE)) {
		if (nsjpeg_convert(jpeg_c) == false) {
			content_destroy(jpeg_c);
			return NSERROR_CLONE_FAILED;
		}
	}

	*newc = jpeg_c;

	return NSERROR_OK;
}

static const content_handler nsjpeg_content_handler = {
	.create = nsjpeg_create,
	.data_complete = nsjpeg_convert,
	.destroy = image_cache_destroy,
	.redraw = image_cache_redraw,
	.clone = nsjpeg_clone,
	.get_internal = image_cache_get_internal,
	.type = image_cache_content_type,
	.is_opaque = image_cache_is_opaque,
	.no_share = false,
};

static const char *nsjpeg_types[] = {
	"image/jpeg",
	"image/jpg",
	"image/pjpeg"
};

CONTENT_FACTORY_REGISTER_TYPES(nsjpeg, nsjpeg_types, nsjpeg_content_handler);
