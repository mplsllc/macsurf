/* macos9_image.c -- NetSurf image content handler backed by QuickTime
 * Graphics Importers. fixes78.
 *
 * Architecture (fixes78j refactor):
 *
 *   process_data: accumulate raw bytes into a Mac Handle.
 *   data_complete: ask QT for an importer, query natural bounds, decode
 *                  ONCE into a 32-bit GWorld, byte-swap into a NetSurf
 *                  struct bitmap (RGBA), free importer + Handle + temp
 *                  GWorld.
 *   redraw:       one-liner into ctx->plot->bitmap (= macos9_plot_bitmap).
 *   destroy:      drop the bitmap via guit->bitmap->destroy.
 *
 * Why not call GraphicsImportDraw at redraw time? content_redraw for
 * images is dispatched outside the html_redraw box walker's plot_clip
 * scope, so a direct QT draw lands at an undefined clip / port state.
 * Routing through ctx->plot->bitmap puts the actual blit inside the
 * box walker's clip context (the path Atari / RISC OS / Amiga
 * frontends use), which is fixes77f's offscreen GWorld during paint.
 */

#include "macos9.h"

#include <Movies.h>

#include <stdlib.h>
#include <string.h>

#include "utils/errors.h"
#include "netsurf/types.h"
#include "netsurf/plotters.h"
#include "netsurf/content.h"
#include "netsurf/bitmap.h"
#include "content/content_protected.h"
#include "content/content_factory.h"
#include "desktop/gui_internal.h"
#include "desktop/gui_table.h"

#include "macsurf_debug.h"

#include "lodepng.h"

#ifndef graphicsModeStraightAlpha
#define graphicsModeStraightAlpha 256
#endif

/* k32ARGBPixelFormat = 'ARGB'. Defined in <ImageCompression.h> but
 * some CW8 SDKs are missing it -- provide the literal as a fallback. */
#ifndef k32ARGBPixelFormat
#define k32ARGBPixelFormat 0x41524742
#endif

typedef ComponentInstance GraphicsImportComponent;

typedef struct macos9_qt_image_content {
	struct content base;     /* MUST be first */
	Handle compressed;       /* raw bytes, grown by process_data */
	void *bitmap;            /* struct macos9_bitmap (RGBA pixels) */
} macos9_qt_image_content;

static nserror
macos9_qt_image_create(const struct content_handler *handler,
		lwc_string *imime_type, const struct http_parameter *params,
		struct llcache_handle *llcache, const char *fallback_charset,
		bool quirks, struct content **c)
{
	macos9_qt_image_content *qti;
	nserror err;

	qti = (macos9_qt_image_content *)calloc(1,
			sizeof(macos9_qt_image_content));
	if (qti == NULL) {
		return NSERROR_NOMEM;
	}

	err = content__init(&qti->base, handler, imime_type, params,
			llcache, fallback_charset, quirks);
	if (err != NSERROR_OK) {
		free(qti);
		return err;
	}

	qti->compressed = NULL;
	qti->bitmap = NULL;

	*c = (struct content *)qti;
	return NSERROR_OK;
}

static bool
macos9_qt_image_process(struct content *c, const char *data,
		unsigned int size)
{
	macos9_qt_image_content *qti = (macos9_qt_image_content *)c;
	Size old_size;
	OSErr err;

	if (size == 0) {
		return true;
	}

	if (qti->compressed == NULL) {
		qti->compressed = NewHandle((Size)size);
		if (qti->compressed == NULL) {
			content_broadcast_error(c, NSERROR_NOMEM, NULL);
			return false;
		}
		old_size = 0;
	} else {
		old_size = GetHandleSize(qti->compressed);
		SetHandleSize(qti->compressed, old_size + (Size)size);
		err = MemError();
		if (err != noErr) {
			content_broadcast_error(c, NSERROR_NOMEM, NULL);
			return false;
		}
	}

	HLock(qti->compressed);
	BlockMoveData(data, (*qti->compressed) + old_size, (Size)size);
	HUnlock(qti->compressed);
	return true;
}

/* Sniff the compressed bytes for an alpha-capable format.
 *   PNG  -> 89 50 4E 47 0D 0A 1A 0A
 *   TIFF -> 49 49 2A 00 (LE) or 4D 4D 00 2A (BE)
 * GIF is excluded: QT's GIF importer doesn't respect
 * graphicsModeStraightAlpha and produces useless output through that
 * path. JPEG / 24-bit BMP have no alpha at file level. */
static bool
macos9_qt_format_has_alpha(const unsigned char *p, long n)
{
	if (n < 4) return false;
	if (p[0] == 0x89 && p[1] == 0x50 && p[2] == 0x4E && p[3] == 0x47) {
		return true;
	}
	if ((p[0] == 'I' && p[1] == 'I' && p[2] == 0x2A && p[3] == 0x00) ||
			(p[0] == 'M' && p[1] == 'M' &&
			 p[2] == 0x00 && p[3] == 0x2A)) {
		return true;
	}
	return false;
}

/* Sentinel for color-key transparency. PNG/TIFF transparent pixels
 * (alpha < 128) get rewritten to this RGB at decode time; the bitmap
 * plotter blits with `transparent` transfer mode + magenta bgColor so
 * matching pixels skip and the destination (card background) shows. */
#define MACOS9_IMG_TRANSPARENT_R 0xFF
#define MACOS9_IMG_TRANSPARENT_G 0x00
#define MACOS9_IMG_TRANSPARENT_B 0xFF

/* Decode the importer into a freshly-allocated NetSurf bitmap.
 * Returns NSERROR_OK on success. On failure the bitmap is freed and
 * *out_bitmap is left NULL.
 *
 * For alpha-capable formats (PNG/TIFF) the temp GWorld is allocated
 * via QTNewGWorld with k32ARGBPixelFormat -- this tells the QT
 * graphics importer that the destination pixmap has a real alpha
 * channel, so PNG/TIFF importers map the file's per-pixel alpha into
 * the high byte rather than treating it as filler.
 *
 * Opaque formats (JPEG / 24-bit BMP / GIF) still allocate via the
 * vanilla NewGWorld(32) path -- no alpha to preserve. */
static nserror
macos9_qt_decode_to_bitmap(GraphicsImportComponent gi,
		int bw, int bh, bool wants_alpha, void **out_bitmap)
{
	void *bm;
	unsigned char *dst_buf;
	GWorldPtr temp_gw;
	Rect tmp_bounds;
	PixMapHandle tmp_pm;
	OSErr err;
	long row_bytes;
	long src_rowbytes;
	int row, col;
	unsigned char *src_base;
	unsigned char *src_row;
	unsigned char *dst_row;
	CGrafPtr save_port;
	GDHandle save_gdh;

	*out_bitmap = NULL;

	if (guit == NULL || guit->bitmap == NULL ||
			guit->bitmap->create == NULL) {
		MS_LOG("img decode: guit bitmap NULL");
		return NSERROR_INIT_FAILED;
	}

	bm = guit->bitmap->create(bw, bh, BITMAP_CLEAR);
	if (bm == NULL) {
		MS_LOG("img decode: bitmap create FAIL");
		return NSERROR_NOMEM;
	}

	dst_buf = (unsigned char *)guit->bitmap->get_buffer(bm);
	row_bytes = (long)guit->bitmap->get_rowstride(bm);
	if (dst_buf == NULL || row_bytes <= 0) {
		MS_LOG("img decode: bitmap buf/stride bad");
		guit->bitmap->destroy(bm);
		return NSERROR_NOMEM;
	}

	tmp_bounds.left = 0;
	tmp_bounds.top = 0;
	tmp_bounds.right = (short)bw;
	tmp_bounds.bottom = (short)bh;

	temp_gw = NULL;
	if (wants_alpha) {
		OSErr qt_err1 = 0, qt_err2 = 0;
		/* Path B probe: QTNewGWorld with k32ARGBPixelFormat asks
		 * for a pixmap with a real alpha component. If this
		 * environment's QT supports it, the importer will write
		 * meaningful alpha. */
		qt_err1 = QTNewGWorld(&temp_gw, k32ARGBPixelFormat,
				&tmp_bounds, NULL, NULL, (GWorldFlags)4);
		if (qt_err1 != noErr || temp_gw == NULL) {
			qt_err2 = QTNewGWorld(&temp_gw, k32ARGBPixelFormat,
					&tmp_bounds, NULL, NULL, 0);
		}
		if (qt_err1 != noErr && qt_err2 != noErr) {
			/* Graceful degradation: QT rejected the ARGB
			 * pixel format request. Fall back to a vanilla
			 * 32-bit GWorld and render this image as opaque
			 * (the file's stored RGB for source-transparent
			 * pixels will show, but at least the image is
			 * visible). Real alpha will arrive when Path A
			 * (NetSurf core image handlers + libpng/etc.)
			 * lands. */
			macsurf_debug_log_writef(
				"img decode: QTNewGWorld ARGB FAIL "
				"err1=%d err2=%d, falling back to opaque",
				(int)qt_err1, (int)qt_err2);
			wants_alpha = false;
			err = NewGWorld(&temp_gw, 32, &tmp_bounds,
					NULL, NULL, (GWorldFlags)4);
			if (err != noErr || temp_gw == NULL) {
				err = NewGWorld(&temp_gw, 32, &tmp_bounds,
						NULL, NULL, 0);
			}
			if (err != noErr || temp_gw == NULL) {
				MS_LOG("img decode: fallback NewGWorld FAIL");
				guit->bitmap->destroy(bm);
				return NSERROR_NOMEM;
			}
		}
	} else {
		err = NewGWorld(&temp_gw, 32, &tmp_bounds, NULL, NULL,
				(GWorldFlags)4);
		if (err != noErr || temp_gw == NULL) {
			err = NewGWorld(&temp_gw, 32, &tmp_bounds, NULL,
					NULL, 0);
		}
		if (err != noErr || temp_gw == NULL) {
			MS_LOG("img decode: NewGWorld FAIL");
			guit->bitmap->destroy(bm);
			return NSERROR_NOMEM;
		}
	}

	tmp_pm = GetGWorldPixMap(temp_gw);
	if (tmp_pm == NULL || !LockPixels(tmp_pm)) {
		MS_LOG("img decode: LockPixels FAIL");
		DisposeGWorld(temp_gw);
		guit->bitmap->destroy(bm);
		return NSERROR_NOMEM;
	}

	src_rowbytes = (*tmp_pm)->rowBytes & 0x3FFF;
	src_base = (unsigned char *)GetPixBaseAddr(tmp_pm);

	GetGWorld(&save_port, &save_gdh);
	SetGWorld(temp_gw, NULL);
	{
		RGBColor c;
		if (wants_alpha) {
			/* Erase to fully transparent (alpha = 0 across the
			 * whole pixmap). If QT honors the ARGB format,
			 * opaque source pixels will overwrite alpha with
			 * 0xFF and source-transparent pixels will leave
			 * alpha as 0 -- giving the byte-swap real per-
			 * pixel alpha to read. */
			c.red = 0;
			c.green = 0;
			c.blue = 0;
		} else {
			c.red = 0xFFFF;
			c.green = 0xFFFF;
			c.blue = 0xFFFF;
		}
		RGBBackColor(&c);
		EraseRect(&tmp_bounds);
	}

	GraphicsImportSetGWorld(gi, temp_gw, NULL);
	GraphicsImportSetBoundsRect(gi, &tmp_bounds);
	GraphicsImportDraw(gi);

	SetGWorld(save_port, save_gdh);

	if (wants_alpha) {
		/* Probe: log the alpha byte at 8 evenly-spaced columns
		 * across the middle row. If QT honored k32ARGBPixelFormat
		 * we expect VARYING values (00 for transparent pixels,
		 * FF for opaque). A row of uniform 00 or FF means QT
		 * ignored the pixel format and the high byte is filler. */
		int probe_row = bh / 2;
		unsigned char *p = src_base + (long)probe_row * src_rowbytes;
		macsurf_debug_log_writef(
			"alpha probe row=%d: %d %d %d %d %d %d %d %d",
			probe_row,
			(int)p[(bw * 0 / 8) * 4 + 0],
			(int)p[(bw * 1 / 8) * 4 + 0],
			(int)p[(bw * 2 / 8) * 4 + 0],
			(int)p[(bw * 3 / 8) * 4 + 0],
			(int)p[(bw * 4 / 8) * 4 + 0],
			(int)p[(bw * 5 / 8) * 4 + 0],
			(int)p[(bw * 6 / 8) * 4 + 0],
			(int)p[(bw * 7 / 8) * 4 + 0]);
	}

	/* Byte-swap. GWorld is ARGB (byte 0=A, 1=R, 2=G, 3=B), bitmap
	 * is RGBA. For alpha format, alpha < 128 becomes the magenta
	 * sentinel for color-key transparency at blit time. Opaque
	 * formats force alpha = 0xFF. */
	for (row = 0; row < bh; row++) {
		src_row = src_base + row * src_rowbytes;
		dst_row = dst_buf + row * row_bytes;
		for (col = 0; col < bw; col++) {
			if (wants_alpha) {
				unsigned char a = src_row[col * 4 + 0];
				if (a < 128) {
					dst_row[col * 4 + 0] =
						MACOS9_IMG_TRANSPARENT_R;
					dst_row[col * 4 + 1] =
						MACOS9_IMG_TRANSPARENT_G;
					dst_row[col * 4 + 2] =
						MACOS9_IMG_TRANSPARENT_B;
					dst_row[col * 4 + 3] = 0xFF;
					continue;
				}
			}
			dst_row[col * 4 + 0] = src_row[col * 4 + 1];
			dst_row[col * 4 + 1] = src_row[col * 4 + 2];
			dst_row[col * 4 + 2] = src_row[col * 4 + 3];
			dst_row[col * 4 + 3] = 0xFF;
		}
	}

	UnlockPixels(tmp_pm);
	DisposeGWorld(temp_gw);

	if (guit->bitmap->set_opaque != NULL) {
		guit->bitmap->set_opaque(bm, !wants_alpha);
	}
	if (guit->bitmap->modified != NULL) {
		guit->bitmap->modified(bm);
	}

	*out_bitmap = bm;
	return NSERROR_OK;
}

/* Decode a PNG buffer via lodepng straight into a NetSurf bitmap with
 * real per-pixel alpha. lodepng outputs RGBA so no byte-swap is needed.
 *
 * Per-pixel alpha is exposed to the plotter as a 1-bit mask built
 * alongside the RGBA bitmap: mask bit = 1 where alpha >= 128, = 0
 * where alpha < 128. The plotter uses CopyMask (a known-working classic
 * Mac transparency API) when a mask is attached -- this is more robust
 * than the magenta-keyed transparent-mode CopyBits path which never
 * worked reliably on 32-bit pixmaps in our testing.
 *
 * The actual RGB values for source-transparent pixels are preserved
 * (not overwritten with a sentinel) -- if a future round wants 8-bit
 * mask compositing via CopyDeepMask for anti-aliased edges, those RGB
 * values will be ready. */
extern void macos9_bitmap_set_mask(void *bitmap, unsigned char *mask,
		int mask_rowbytes);

static nserror
macos9_png_decode_to_bitmap(const unsigned char *data, size_t size,
		void **out_bitmap, int *out_w, int *out_h)
{
	unsigned char *rgba;
	unsigned w, h;
	unsigned lerr;
	void *bm;
	unsigned char *dst_buf;
	unsigned char *mask;
	long row_bytes;
	int mask_rowbytes;
	unsigned row, col;
	unsigned char *src_row;
	unsigned char *dst_row;
	unsigned char *mask_row;

	*out_bitmap = NULL;

	rgba = NULL;
	w = 0;
	h = 0;
	lerr = lodepng_decode32(&rgba, &w, &h, data, size);
	if (lerr != 0 || rgba == NULL) {
		macsurf_debug_log_writef("png decode: lodepng err=%u",
				(unsigned)lerr);
		if (rgba != NULL) free(rgba);
		return NSERROR_INVALID;
	}

	if (w == 0 || h == 0) {
		free(rgba);
		return NSERROR_INVALID;
	}

	bm = guit->bitmap->create((int)w, (int)h, BITMAP_CLEAR);
	if (bm == NULL) {
		free(rgba);
		return NSERROR_NOMEM;
	}

	dst_buf = (unsigned char *)guit->bitmap->get_buffer(bm);
	row_bytes = (long)guit->bitmap->get_rowstride(bm);
	if (dst_buf == NULL || row_bytes <= 0) {
		free(rgba);
		guit->bitmap->destroy(bm);
		return NSERROR_NOMEM;
	}

	/* 1-bit mask: ceil(w / 8) bytes per row, h rows. Round-up to
	 * even bytes for QuickDraw BitMap word-alignment requirements. */
	mask_rowbytes = (int)((w + 15) / 16) * 2;
	mask = calloc((size_t)mask_rowbytes * (size_t)h, 1);
	if (mask == NULL) {
		free(rgba);
		guit->bitmap->destroy(bm);
		return NSERROR_NOMEM;
	}

	for (row = 0; row < h; row++) {
		src_row = rgba + (long)row * (long)w * 4;
		dst_row = dst_buf + (long)row * row_bytes;
		mask_row = mask + (long)row * mask_rowbytes;
		for (col = 0; col < w; col++) {
			unsigned char a = src_row[col * 4 + 3];
			dst_row[col * 4 + 0] = src_row[col * 4 + 0];
			dst_row[col * 4 + 1] = src_row[col * 4 + 1];
			dst_row[col * 4 + 2] = src_row[col * 4 + 2];
			dst_row[col * 4 + 3] = 0xFF;
			if (a >= 128) {
				/* QuickDraw BitMap convention: MSB of each
				 * byte is leftmost pixel. mask bit set = 1
				 * means opaque (copy through). */
				mask_row[col >> 3] |=
					(unsigned char)(0x80 >> (col & 7));
			}
		}
	}

	free(rgba);

	macos9_bitmap_set_mask(bm, mask, mask_rowbytes);

	if (guit->bitmap->set_opaque != NULL) {
		guit->bitmap->set_opaque(bm, false);
	}
	if (guit->bitmap->modified != NULL) {
		guit->bitmap->modified(bm);
	}

	*out_bitmap = bm;
	*out_w = (int)w;
	*out_h = (int)h;
	return NSERROR_OK;
}

static bool
macos9_qt_image_convert(struct content *c)
{
	macos9_qt_image_content *qti = (macos9_qt_image_content *)c;
	Handle data_ref;
	OSErr osr;
	ComponentResult cr;
	GraphicsImportComponent importer;
	Rect bounds;
	int bw, bh;
	long src_size;
	bool wants_alpha;
	nserror err;
	const unsigned char *src_bytes;

	if (qti->compressed == NULL ||
			GetHandleSize(qti->compressed) == 0) {
		MS_LOG("img convert: no bytes");
		content_broadcast_error(c, NSERROR_INVALID, NULL);
		return false;
	}

	src_size = GetHandleSize(qti->compressed);
	HLock(qti->compressed);
	src_bytes = (const unsigned char *)*qti->compressed;

	/* PNG magic: 89 50 4E 47 0D 0A 1A 0A. If this is a PNG, dispatch
	 * to lodepng for real per-pixel alpha -- Path A for PNG only. */
	if (src_size >= 8 &&
			src_bytes[0] == 0x89 && src_bytes[1] == 0x50 &&
			src_bytes[2] == 0x4E && src_bytes[3] == 0x47) {
		bw = 0;
		bh = 0;
		err = macos9_png_decode_to_bitmap(src_bytes,
				(size_t)src_size, &qti->bitmap, &bw, &bh);
		HUnlock(qti->compressed);
		DisposeHandle(qti->compressed);
		qti->compressed = NULL;
		if (err != NSERROR_OK || qti->bitmap == NULL) {
			MS_LOG("img convert: lodepng FAIL");
			content_broadcast_error(c, err, NULL);
			return false;
		}
		c->width = bw;
		c->height = bh;
		macsurf_debug_log_writef("img decoded via lodepng %dx%d",
				bw, bh);
		content_set_ready(c);
		content_set_done(c);
		content_set_status(c, "");
		return true;
	}

	wants_alpha = macos9_qt_format_has_alpha(src_bytes, src_size);
	HUnlock(qti->compressed);
	MS_LOG(wants_alpha ? "img convert: alpha format" :
			"img convert: opaque format");

	data_ref = NULL;
	osr = PtrToHand(&qti->compressed, &data_ref, (long)sizeof(Handle));
	if (osr != noErr || data_ref == NULL) {
		MS_LOG("img convert: PtrToHand FAIL");
		content_broadcast_error(c, NSERROR_NOMEM, NULL);
		return false;
	}

	importer = NULL;
	cr = GetGraphicsImporterForDataRef(data_ref,
			HandleDataHandlerSubType, &importer);
	DisposeHandle(data_ref);
	if (cr != noErr || importer == NULL) {
		MS_LOG("img convert: no importer for data");
		content_broadcast_error(c, NSERROR_INVALID, NULL);
		return false;
	}

	bounds.top = 0;
	bounds.left = 0;
	bounds.bottom = 0;
	bounds.right = 0;
	cr = GraphicsImportGetNaturalBounds(importer, &bounds);
	if (cr != noErr) {
		MS_LOG("img convert: GetNaturalBounds FAIL");
		CloseComponent(importer);
		content_broadcast_error(c, NSERROR_INVALID, NULL);
		return false;
	}

	bw = (int)(bounds.right - bounds.left);
	bh = (int)(bounds.bottom - bounds.top);
	if (bw <= 0 || bh <= 0) {
		MS_LOG("img convert: bad bounds");
		CloseComponent(importer);
		content_broadcast_error(c, NSERROR_INVALID, NULL);
		return false;
	}
	macsurf_debug_log_writef("img convert: %dx%d %s, decoding",
			bw, bh, wants_alpha ? "alpha" : "opaque");

	err = macos9_qt_decode_to_bitmap(importer, bw, bh, wants_alpha,
			&qti->bitmap);
	CloseComponent(importer);
	if (qti->compressed != NULL) {
		DisposeHandle(qti->compressed);
		qti->compressed = NULL;
	}

	if (err != NSERROR_OK || qti->bitmap == NULL) {
		MS_LOG("img convert: decode FAIL");
		content_broadcast_error(c, err, NULL);
		return false;
	}

	c->width = bw;
	c->height = bh;

	MS_LOG("img decoded to bitmap");
	content_set_ready(c);
	content_set_done(c);
	content_set_status(c, "");
	return true;
}

static bool
macos9_qt_image_redraw(struct content *c, struct content_redraw_data *data,
		const struct rect *clip, const struct redraw_context *ctx)
{
	macos9_qt_image_content *qti = (macos9_qt_image_content *)c;
	bitmap_flags_t flags;

	(void)clip;

	if (qti->bitmap == NULL || ctx == NULL ||
			ctx->plot == NULL || ctx->plot->bitmap == NULL) {
		return true;
	}

	flags = 0;
	if (data->repeat_x) flags |= BITMAPF_REPEAT_X;
	if (data->repeat_y) flags |= BITMAPF_REPEAT_Y;

	return ctx->plot->bitmap(ctx, (struct bitmap *)qti->bitmap,
			data->x, data->y,
			data->width, data->height,
			data->background_colour,
			flags) == NSERROR_OK;
}

static void
macos9_qt_image_destroy(struct content *c)
{
	macos9_qt_image_content *qti = (macos9_qt_image_content *)c;

	if (qti->bitmap != NULL) {
		if (guit != NULL && guit->bitmap != NULL &&
				guit->bitmap->destroy != NULL) {
			guit->bitmap->destroy(qti->bitmap);
		}
		qti->bitmap = NULL;
	}
	if (qti->compressed != NULL) {
		DisposeHandle(qti->compressed);
		qti->compressed = NULL;
	}
}

static nserror
macos9_qt_image_clone(const struct content *old, struct content **newc)
{
	(void)old;
	(void)newc;
	return NSERROR_CLONE_FAILED;
}

static content_type
macos9_qt_image_type(void)
{
	return CONTENT_IMAGE;
}

static bool
macos9_qt_image_is_opaque(struct content *c)
{
	macos9_qt_image_content *qti = (macos9_qt_image_content *)c;
	if (qti->bitmap != NULL && guit != NULL && guit->bitmap != NULL &&
			guit->bitmap->get_opaque != NULL) {
		return guit->bitmap->get_opaque(qti->bitmap);
	}
	return false;
}

/* Vtable -- positional init (CW8 C89 has no designated initialisers).
 * Field order MUST match struct content_handler in
 * content/content_protected.h. */
static const struct content_handler macos9_qt_image_handler = {
	NULL,                          /* fini */
	macos9_qt_image_create,        /* create */
	macos9_qt_image_process,       /* process_data */
	macos9_qt_image_convert,       /* data_complete */
	NULL,                          /* reformat */
	macos9_qt_image_destroy,       /* destroy */
	NULL,                          /* stop */
	NULL,                          /* mouse_track */
	NULL,                          /* mouse_action */
	NULL,                          /* keypress */
	macos9_qt_image_redraw,        /* redraw */
	NULL,                          /* open */
	NULL,                          /* close */
	NULL,                          /* clear_selection */
	NULL,                          /* get_selection */
	NULL,                          /* get_contextual_content */
	NULL,                          /* scroll_at_point */
	NULL,                          /* drop_file_at_point */
	NULL,                          /* debug_dump */
	NULL,                          /* debug */
	macos9_qt_image_clone,         /* clone */
	NULL,                          /* matches_quirks */
	NULL,                          /* get_encoding */
	macos9_qt_image_type,          /* type */
	NULL,                          /* add_user */
	NULL,                          /* remove_user */
	NULL,                          /* exec */
	NULL,                          /* saw_insecure_objects */
	NULL,                          /* textsearch_find */
	NULL,                          /* textsearch_bounds */
	NULL,                          /* textselection_redraw */
	NULL,                          /* textselection_copy */
	NULL,                          /* textselection_get_end */
	NULL,                          /* get_internal */
	macos9_qt_image_is_opaque,     /* is_opaque */
	false                          /* no_share */
};

static const char *macos9_qt_image_mime[] = {
	"image/jpeg",
	"image/jpg",
	"image/pjpeg",
	"image/png",
	"image/x-png",
	"image/gif",
	"image/bmp",
	"image/x-bmp",
	"image/x-ms-bmp",
	"image/tiff",
	"image/x-tiff"
};

nserror image_init(void)
{
	nserror err;
	size_t i;
	size_t n;

	n = sizeof(macos9_qt_image_mime) / sizeof(macos9_qt_image_mime[0]);
	for (i = 0; i < n; ++i) {
		err = content_factory_register_handler(macos9_qt_image_mime[i],
				&macos9_qt_image_handler);
		if (err != NSERROR_OK) {
			return err;
		}
	}
	return NSERROR_OK;
}
