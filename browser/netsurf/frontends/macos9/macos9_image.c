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

/* Forward declaration for LRU links. */
struct macos9_qt_image_content;
typedef struct macos9_qt_image_content macos9_qt_image_content;

struct macos9_qt_image_content {
	struct content base;     /* MUST be first */
	Handle compressed;       /* raw bytes, kept alive for deferred decode */
	void *bitmap;            /* struct macos9_bitmap (RGBA pixels) */
	/* fixes161b — tracked decoded bytes for this content. Set when the
	 * bitmap was successfully decoded; cleared on destroy. Used to
	 * decrement macsurf__decoded_img_bytes_current symmetrically. */
	long decoded_bytes;
	/* fixes162 — display-size deferred decode + LRU cache.
	 *
	 * Previously: convert() decoded the QT importer at NATURAL pixel
	 * size and stored the bitmap once. A 1262x580 hero JPEG consumed
	 * 2.9 MB regardless of how small it was actually displayed. Modern
	 * pages serve images at multiples of their display size; we paid
	 * full natural cost for thumbnails. The 4 MB cache cap then made
	 * us silently skip half the images on apple.com and huffpost.com.
	 *
	 * Now: convert() opens the QT importer and queries natural bounds
	 * only — no pixel decode. importer + compressed bytes are kept
	 * alive on the qti so the FIRST redraw (when display dims are
	 * known via data->width/height after layout has placed the image)
	 * can decode at display size via the existing
	 * macos9_qt_decode_to_bitmap path. Subsequent redraws at the same
	 * display size reuse the cached bitmap. A display-size change
	 * (window resize, reflow) triggers a re-decode.
	 *
	 * lru_prev/lru_next link this entry into a global MRU-at-head /
	 * LRU-at-tail list. On every successful redraw the entry is moved
	 * to MRU. When admitting a new decode would exceed the cache cap
	 * we walk from the tail, calling guit->bitmap->destroy on the
	 * oldest unreferenced entries until the new one fits. Evicted
	 * entries keep their importer + compressed bytes, so a subsequent
	 * redraw re-decodes them transparently. */
	GraphicsImportComponent importer; /* NULL = PNG (eager OR deferred) */
	int bitmap_w;                     /* display size of cached bitmap */
	int bitmap_h;
	bool wants_alpha;                 /* QT decode mode hint */
	macos9_qt_image_content *lru_prev;
	macos9_qt_image_content *lru_next;
	bool in_lru;
	/* fixes259 — deferred PNG. Mirrors the QT path: at convert time we
	 * only run lodepng_inspect to extract dimensions; the actual decode
	 * (full inflate + unfilter + box-filter downscale to display size)
	 * happens on the first redraw when the layout's display dimensions
	 * are known. Memory + paint-time box-filter wins same as QT. */
	bool is_png_deferred;
};

/* fixes162 — global decoded image memory cache.
 *
 * 32 MB ceiling on simultaneously-decoded RGBA pixels. Backed by an
 * LRU list (macos9_qti_lru_*) that evicts the least-recently-redrawn
 * entries when admitting a new decode would exceed the cap. Evicted
 * entries drop their bitmap but keep importer + compressed bytes, so
 * a later redraw re-decodes at whatever display size that paint
 * needs. This replaces the fixes161b skip-on-full budget gate, which
 * was a survival pattern (any new image past 4 MB total just got
 * dropped on the floor). The new design is a real cache: every image
 * gets a chance, recently-shown ones stay decoded, old ones evict.
 *
 * 32 MB requires the Carbon partition to be raised to 96 MB
 * preferred / 32 MB minimum. The previous 16 MB partition cannot
 * back a 32 MB cache; user must bump MacSurf.mcp partition before
 * running this build. Documented in CLAUDE.md "Build State" section.
 *
 * Why 32 MB: maxed retro hardware (G4 with 512 MB-1.5 GB RAM) has
 * the headroom. Smaller G3 / 64 MB systems can drop this to 16 MB
 * by changing this one define; the LRU still evicts cleanly. */
#define MACOS9_DECODED_IMG_MAX_BYTES (32L * 1024L * 1024L)

/* fixes162 — per-image hard ceiling. With display-size decode the
 * worst case is a single image filling the entire viewport.
 * fixes260 — bumped 8 MB -> 16 MB after observing mactrove's hero
 * images request 1241x1754 display (~8.7 MB) and getting skipped
 * under the 8 MB cap. 16 MB now covers ~2000x2000 displays. Pages
 * trying to decode a true 4000x3000 hero at full resolution still
 * trip this and degrade to broken-image; pages at typical content
 * widths sail through. Cap stays well under the global 32 MB
 * MACOS9_DECODED_IMG_MAX_BYTES so one outsize image can't starve
 * the LRU. */
#define MACOS9_IMG_MAX_DECODED_BYTES (16L * 1024L * 1024L)

/* fixes162 — LRU list of all decoded macos9_qt_image_content
 * entries. lru_head is MRU (most recently redrawn), lru_tail is LRU
 * (oldest, eviction candidate). lru_total_bytes mirrors the sum of
 * decoded_bytes across the list and is the value compared against
 * MACOS9_DECODED_IMG_MAX_BYTES for admission decisions. */
static macos9_qt_image_content *macos9_qti_lru_head = NULL;
static macos9_qt_image_content *macos9_qti_lru_tail = NULL;
static long macos9_qti_lru_total_bytes = 0;

static void
macos9_qti_lru_unlink(macos9_qt_image_content *q)
{
	if (!q->in_lru) return;
	if (q->lru_prev != NULL) q->lru_prev->lru_next = q->lru_next;
	else macos9_qti_lru_head = q->lru_next;
	if (q->lru_next != NULL) q->lru_next->lru_prev = q->lru_prev;
	else macos9_qti_lru_tail = q->lru_prev;
	q->lru_prev = NULL;
	q->lru_next = NULL;
	q->in_lru = false;
}

static void
macos9_qti_lru_insert_mru(macos9_qt_image_content *q)
{
	if (q->in_lru) macos9_qti_lru_unlink(q);
	q->lru_prev = NULL;
	q->lru_next = macos9_qti_lru_head;
	if (macos9_qti_lru_head != NULL) macos9_qti_lru_head->lru_prev = q;
	else macos9_qti_lru_tail = q;
	macos9_qti_lru_head = q;
	q->in_lru = true;
}

static void
macos9_qti_lru_touch(macos9_qt_image_content *q)
{
	if (!q->in_lru || q->lru_prev == NULL) return; /* already MRU */
	macos9_qti_lru_unlink(q);
	macos9_qti_lru_insert_mru(q);
}

/* Drop bitmaps from the LRU tail until admitting need_bytes would
 * leave the GLOBAL running total at or below the cap. Each evicted
 * entry keeps its importer + compressed bytes; only the decoded
 * bitmap is freed. A subsequent redraw re-decodes. PNG entries are
 * not in the LRU (they're decoded once in convert at natural size)
 * but their bytes are counted in macsurf__decoded_img_bytes_current,
 * so the check is against the global counter — eviction only walks
 * the QT-deferred LRU but the cap is enforced over both populations.
 * Returns true if there is now room; false if even after evicting
 * every LRU entry the cap is still exceeded (PNGs alone are full,
 * or the request itself is bigger than the cap). */
static bool
macos9_qti_lru_make_room(long need_bytes)
{
	macos9_qt_image_content *victim;
	extern long macsurf__decoded_img_bytes_current;
	while (macsurf__decoded_img_bytes_current + need_bytes >
			MACOS9_DECODED_IMG_MAX_BYTES &&
			macos9_qti_lru_tail != NULL) {
		victim = macos9_qti_lru_tail;
		macsurf_debug_log_writef(
			"img lru evict: qti=%p bytes=%ld global=%ld need=%ld",
			(void *)victim,
			(long)victim->decoded_bytes,
			macsurf__decoded_img_bytes_current,
			need_bytes);
		macos9_qti_lru_unlink(victim);
		macos9_qti_lru_total_bytes -= victim->decoded_bytes;
		if (macos9_qti_lru_total_bytes < 0) {
			macos9_qti_lru_total_bytes = 0;
		}
		if (macsurf__decoded_img_bytes_current >=
				victim->decoded_bytes) {
			macsurf__decoded_img_bytes_current -=
				victim->decoded_bytes;
		} else {
			macsurf__decoded_img_bytes_current = 0;
		}
		if (victim->bitmap != NULL &&
				guit != NULL && guit->bitmap != NULL &&
				guit->bitmap->destroy != NULL) {
			guit->bitmap->destroy(victim->bitmap);
		}
		victim->bitmap = NULL;
		victim->bitmap_w = 0;
		victim->bitmap_h = 0;
		victim->decoded_bytes = 0;
	}
	return macsurf__decoded_img_bytes_current + need_bytes <=
			MACOS9_DECODED_IMG_MAX_BYTES;
}

static bool
macos9_img_is_oversize(int w, int h)
{
	long bytes;
	if (w <= 0 || h <= 0) return false;
	bytes = (long)w * (long)h * 4L;
	return bytes > MACOS9_IMG_MAX_DECODED_BYTES;
}

/* Read IHDR width/height from a PNG buffer without invoking lodepng's
 * full inflate. PNG layout is fixed: 8-byte signature, then a 4-byte
 * chunk length, "IHDR" tag, width (big-endian u32) at offset 16, height
 * at offset 20. Returns false if the bytes don't look like a PNG or
 * the dimensions are absurd. */
static bool
macos9_png_read_dims(const unsigned char *data, size_t size,
		int *out_w, int *out_h)
{
	unsigned long w, h;
	if (size < 24) return false;
	if (data[0] != 0x89 || data[1] != 0x50 ||
			data[2] != 0x4E || data[3] != 0x47) return false;
	w = ((unsigned long)data[16] << 24) |
		((unsigned long)data[17] << 16) |
		((unsigned long)data[18] << 8) |
		(unsigned long)data[19];
	h = ((unsigned long)data[20] << 24) |
		((unsigned long)data[21] << 16) |
		((unsigned long)data[22] << 8) |
		(unsigned long)data[23];
	if (w == 0 || h == 0 || w > 32768UL || h > 32768UL) return false;
	*out_w = (int)w;
	*out_h = (int)h;
	return true;
}

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

/* fixes259 — decode PNG at native via lodepng, then box-filter
 * downscale to (target_w, target_h) into a single NetSurf bitmap +
 * matching 1-bit mask. If target dims match native (or upscale would
 * be needed), produces a 1:1 copy (no downscale work). Caller is
 * responsible for releasing the bitmap when done.
 *
 * This is the deferred-PNG equivalent of the QT path's display-size
 * decode (fixes162). The native RGBA buffer is allocated, populated,
 * box-filtered, and freed all inside this function so the caller never
 * holds the native-resolution buffer. */
static nserror
macos9_png_decode_target(const unsigned char *data, size_t size,
		int target_w, int target_h,
		void **out_bitmap)
{
	unsigned char *rgba;
	unsigned src_w, src_h;
	unsigned lerr;
	LodePNGState state;
	void *bm;
	unsigned char *dst_buf;
	unsigned char *mask;
	long row_bytes;
	int mask_rowbytes;
	int dst_w, dst_h;
	int dy, dx;

	*out_bitmap = NULL;

	rgba = NULL;
	src_w = 0;
	src_h = 0;
	lodepng_state_init(&state);
	state.info_raw.colortype  = LCT_RGBA;
	state.info_raw.bitdepth   = 8;
	state.decoder.ignore_crc  = 1;
	state.decoder.zlibsettings.ignore_adler32 = 1;
	lerr = lodepng_decode(&rgba, &src_w, &src_h, &state, data, size);
	lodepng_state_cleanup(&state);
	if (lerr != 0 || rgba == NULL) {
		macsurf_debug_log_writef("png decode_target: lodepng err=%ld",
				(long)lerr);
		if (rgba != NULL) free(rgba);
		return NSERROR_INVALID;
	}
	if (src_w == 0 || src_h == 0) {
		free(rgba);
		return NSERROR_INVALID;
	}

	/* Clamp target to <= native (upscale stays at native for QuickDraw
	 * to handle via CopyBits; pre-upscaling here would only waste CPU
	 * and degrade quality). */
	dst_w = target_w;
	dst_h = target_h;
	if (dst_w <= 0 || dst_w > (int)src_w) dst_w = (int)src_w;
	if (dst_h <= 0 || dst_h > (int)src_h) dst_h = (int)src_h;

	bm = guit->bitmap->create(dst_w, dst_h, BITMAP_CLEAR);
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

	mask_rowbytes = ((dst_w + 15) / 16) * 2;
	mask = calloc((size_t)mask_rowbytes * (size_t)dst_h, 1);
	if (mask == NULL) {
		free(rgba);
		guit->bitmap->destroy(bm);
		return NSERROR_NOMEM;
	}

	if (dst_w == (int)src_w && dst_h == (int)src_h) {
		/* 1:1 — straight per-pixel copy + mask build. */
		unsigned row, col;
		for (row = 0; row < src_h; row++) {
			unsigned char *src_row = rgba + (long)row * src_w * 4;
			unsigned char *dst_row = dst_buf + (long)row * row_bytes;
			unsigned char *mask_row = mask + (long)row * mask_rowbytes;
			for (col = 0; col < src_w; col++) {
				dst_row[col * 4 + 0] = src_row[col * 4 + 0];
				dst_row[col * 4 + 1] = src_row[col * 4 + 1];
				dst_row[col * 4 + 2] = src_row[col * 4 + 2];
				dst_row[col * 4 + 3] = src_row[col * 4 + 3];
				if (src_row[col * 4 + 3] >= 8) {
					mask_row[col >> 3] |= (unsigned char)
						(0x80 >> (col & 7));
				}
			}
		}
	} else {
		/* Downscale via box-filter: each dst pixel averages a
		 * (sx × sy) source block. Integer Q8 ratios. */
		long sx_q8 = ((long)src_w << 8) / dst_w;
		long sy_q8 = ((long)src_h << 8) / dst_h;
		for (dy = 0; dy < dst_h; dy++) {
			long sy0 = (long)dy * sy_q8;
			long sy1 = sy0 + sy_q8;
			int sy_start = (int)(sy0 >> 8);
			int sy_end   = (int)((sy1 + 255) >> 8);
			unsigned char *dst_row = dst_buf + (long)dy * row_bytes;
			unsigned char *mask_row = mask + (long)dy * mask_rowbytes;
			if (sy_end > (int)src_h) sy_end = (int)src_h;
			if (sy_start >= sy_end) sy_start = sy_end - 1;
			for (dx = 0; dx < dst_w; dx++) {
				long sx0 = (long)dx * sx_q8;
				long sx1 = sx0 + sx_q8;
				int sx_start = (int)(sx0 >> 8);
				int sx_end   = (int)((sx1 + 255) >> 8);
				long r_acc = 0, g_acc = 0, b_acc = 0;
				long a_acc = 0, n_acc = 0;
				int sy, sx;
				if (sx_end > (int)src_w) sx_end = (int)src_w;
				if (sx_start >= sx_end) sx_start = sx_end - 1;
				for (sy = sy_start; sy < sy_end; sy++) {
					unsigned char *p = rgba +
						(long)sy * src_w * 4 +
						(long)sx_start * 4;
					for (sx = sx_start; sx < sx_end; sx++) {
						r_acc += p[0];
						g_acc += p[1];
						b_acc += p[2];
						a_acc += p[3];
						n_acc++;
						p += 4;
					}
				}
				if (n_acc > 0) {
					dst_row[dx * 4 + 0] = (unsigned char)(r_acc / n_acc);
					dst_row[dx * 4 + 1] = (unsigned char)(g_acc / n_acc);
					dst_row[dx * 4 + 2] = (unsigned char)(b_acc / n_acc);
					dst_row[dx * 4 + 3] = (unsigned char)(a_acc / n_acc);
					if ((a_acc / n_acc) >= 8) {
						mask_row[dx >> 3] |= (unsigned char)
							(0x80 >> (dx & 7));
					}
				}
			}
		}
	}

	free(rgba);

	guit->bitmap->set_opaque(bm, false);
	macos9_bitmap_set_mask(bm, mask, mask_rowbytes);

	*out_bitmap = bm;
	return NSERROR_OK;
}

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
	LodePNGState state;

	*out_bitmap = NULL;

	rgba = NULL;
	w = 0;
	h = 0;
	/* fixes258 — skip CRC32 + Adler32 validation. On a 233 MHz G3,
	 * those per-byte checksums add ~5-15% to PNG decode time for no
	 * benefit on trusted-source images. lodepng_decode32 doesn't
	 * expose state controls, so use the full lodepng_decode API. */
	lodepng_state_init(&state);
	state.info_raw.colortype  = LCT_RGBA;
	state.info_raw.bitdepth   = 8;
	state.decoder.ignore_crc  = 1;
	state.decoder.zlibsettings.ignore_adler32 = 1;
	lerr = lodepng_decode(&rgba, &w, &h, &state, data, size);
	lodepng_state_cleanup(&state);
	if (lerr != 0 || rgba == NULL) {
		/* fixes160b — was "%u"; minimal formatter prints %u
		 * literally without consuming the va_arg. Use %ld. */
		macsurf_debug_log_writef("png decode: lodepng err=%ld",
				(long)lerr);
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

	/* fixes188 — Store unpremultiplied RGBA with the actual alpha
	 * byte preserved in the 4th slot. fixes187's premultiply was
	 * mathematically correct for CopyMask-as-flat-blit but dimmed
	 * anti-aliased edges to near-black, which read as "fade" on
	 * light backgrounds. The real fix happens at plot time, where
	 * scaled draws now sample the destination port and do a true
	 * alpha-over composite (see macos9_plot_bitmap). The 1-bit
	 * mask is still built for the unscaled CopyMask fast path. */
	for (row = 0; row < h; row++) {
		src_row = rgba + (long)row * (long)w * 4;
		dst_row = dst_buf + (long)row * row_bytes;
		mask_row = mask + (long)row * mask_rowbytes;
		for (col = 0; col < w; col++) {
			unsigned char r = src_row[col * 4 + 0];
			unsigned char g = src_row[col * 4 + 1];
			unsigned char b = src_row[col * 4 + 2];
			unsigned char a = src_row[col * 4 + 3];
			dst_row[col * 4 + 0] = r;
			dst_row[col * 4 + 1] = g;
			dst_row[col * 4 + 2] = b;
			dst_row[col * 4 + 3] = a;
			if (a >= 8) {
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
	nserror err = NSERROR_OK;
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
		int gate_w = 0;
		int gate_h = 0;
		/* fixes160b — size gate. Inspect IHDR first; if the
		 * decoded pixel buffer would blow the partition, skip
		 * the decode entirely and let the box layout reserve
		 * the right amount of space for a broken-image rect. */
		if (macos9_png_read_dims(src_bytes, (size_t)src_size,
				&gate_w, &gate_h) &&
				macos9_img_is_oversize(gate_w, gate_h)) {
			long bytes = (long)gate_w * (long)gate_h * 4L;
			macsurf_debug_log_writef(
				"img skip: oversize png %dx%d "
				"decoded=%ld cap=%ld",
				gate_w, gate_h, bytes,
				(long)MACOS9_IMG_MAX_DECODED_BYTES);
			HUnlock(qti->compressed);
			DisposeHandle(qti->compressed);
			qti->compressed = NULL;
			c->width = gate_w;
			c->height = gate_h;
			{
				extern long macsurf__site_img_fail;
				macsurf__site_img_fail++;
			}
			content_broadcast_error(c, NSERROR_NOMEM, NULL);
			return false;
		}
		/* fixes161b — global decoded-budget gate (PNG path). The
		 * per-image fixes160b gate said this image fits individually;
		 * verify the SUM of all live decoded bitmaps + this one
		 * stays within budget. Apple's promo strip plus the home
		 * gallery thumbnails can each pass the 1 MB single-image
		 * cap but blow past 4 MB together. */
		{
			extern long macsurf__decoded_img_bytes_current;
			extern long macsurf__site_decoded_img_skip_budget;
			long png_bytes = (long)gate_w * (long)gate_h * 4L;
			if (macsurf__decoded_img_bytes_current + png_bytes >
					MACOS9_DECODED_IMG_MAX_BYTES) {
				macsurf_debug_log_writef(
					"img skip: budget png %dx%d "
					"bytes=%ld cur=%ld cap=%ld",
					gate_w, gate_h, png_bytes,
					macsurf__decoded_img_bytes_current,
					(long)MACOS9_DECODED_IMG_MAX_BYTES);
				HUnlock(qti->compressed);
				DisposeHandle(qti->compressed);
				qti->compressed = NULL;
				c->width = gate_w;
				c->height = gate_h;
				macsurf__site_decoded_img_skip_budget++;
				{
					extern long macsurf__site_img_fail;
					macsurf__site_img_fail++;
				}
				content_broadcast_error(c, NSERROR_NOMEM, NULL);
				return false;
			}
		}
		/* fixes259 — defer PNG decode to redraw time, mirror the QT
		 * path (fixes162). At convert we already have the natural
		 * dimensions from macos9_png_read_dims above; we don't decode
		 * pixels here. qti->compressed stays alive until first redraw
		 * decodes at display size with on-the-fly box-filter downscale.
		 * Same total CPU as before (PNG can't truly partial-decode),
		 * but page-load latency drops because the parse → layout phase
		 * doesn't block on N PNG decodes, and memory drops because we
		 * never materialise the native-resolution buffer in long-term
		 * storage. */
		HUnlock(qti->compressed);
		bw = gate_w;
		bh = gate_h;
		qti->is_png_deferred = true;
		qti->wants_alpha = true;   /* PNG path always builds 1-bit mask */
		qti->bitmap_w = 0;
		qti->bitmap_h = 0;
		c->width = bw;
		c->height = bh;
		macsurf_debug_log_writef(
			"img convert: %dx%d png, deferred to redraw",
			bw, bh);
		{
			extern long macsurf__site_img_ok;
			macsurf__site_img_ok++;
		}
		content_set_ready(c);
		content_set_done(c);
		content_set_status(c, "");
		return true;
	}
	/* (non-PNG falls through to the QT importer path below) */

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

	/* fixes162 — deferred decode. QT path stops here: importer +
	 * compressed bytes stay alive on the qti; the actual pixel
	 * decode happens on the FIRST redraw, sized to that paint's
	 * data->width/height. This means a 1262x580 hero JPEG displayed
	 * at 320x180 decodes to 230 KB (display size) instead of 2.9 MB
	 * (natural size), and pages like apple.com / huffpost.com that
	 * previously hit the cache cap during decode now fit. The
	 * NetSurf core only needs c->width/height set so layout can
	 * place the image; pixel data is deferred until paint.
	 *
	 * Note: bw/bh on c->width/height are natural pixel dims, used
	 * by layout for aspect-ratio and intrinsic sizing. The actual
	 * bitmap stored in qti->bitmap (allocated later in redraw) is
	 * at DISPLAY size; bitmap_w / bitmap_h record those. Don't
	 * confuse the two — natural is for layout, display is for the
	 * GPU-bound RGBA buffer. */
	qti->importer = importer;
	qti->wants_alpha = wants_alpha;
	qti->bitmap_w = 0;
	qti->bitmap_h = 0;
	c->width = bw;
	c->height = bh;
	(void)err; /* unused on the deferred path */

	macsurf_debug_log_writef(
		"img convert: %dx%d %s, deferred to redraw",
		bw, bh, wants_alpha ? "alpha" : "opaque");

	{
		extern long macsurf__site_img_ok;
		macsurf__site_img_ok++;
	}
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
	int dst_w;
	int dst_h;
	int nat_w;
	int nat_h;

	(void)clip;

	if (ctx == NULL || ctx->plot == NULL || ctx->plot->bitmap == NULL) {
		return true;
	}

	dst_w = data->width;
	dst_h = data->height;
	nat_w = (int)c->width;
	nat_h = (int)c->height;

	/* fixes265 — scale-fit-to-cap for top-level image navigation.
	 * When a user types e.g. https://example.com/photo.jpg directly in
	 * the URL bar, browser_window_redraw passes the content's natural
	 * width/height (not a CSS-constrained box). For a 4000x3000 JPEG
	 * that's 48 MB of RGBA at display size — exceeds the 16 MB per-image
	 * cap and the redraw silently returns true at the cap-check below,
	 * rendering blank. Pre-clamp dst dims proportionally to fit within
	 * the decoded-bytes ceiling so direct-nav images render at the
	 * largest size that fits instead of disappearing. Aspect ratio
	 * preserved. */
	if (dst_w > 0 && dst_h > 0) {
		long want = (long)dst_w * (long)dst_h * 4L;
		if (want > MACOS9_IMG_MAX_DECODED_BYTES) {
			long cap_pixels;
			long scale_num;
			long scale_den;
			int new_w;
			int new_h;
			cap_pixels = MACOS9_IMG_MAX_DECODED_BYTES / 4L;
			/* want_pixels = dst_w * dst_h. Scale factor s satisfies
			 * (dst_w*s)*(dst_h*s) = cap_pixels, so s^2 = cap/want.
			 * Avoid float: use integer sqrt approximation via
			 * iterative refinement of scale_num/scale_den. */
			scale_num = 1000L;
			scale_den = 1000L;
			while ((long)dst_w * scale_num / scale_den *
			       (long)dst_h * scale_num / scale_den >
			       cap_pixels && scale_num > 0) {
				scale_num -= 10L;
			}
			new_w = (int)((long)dst_w * scale_num / scale_den);
			new_h = (int)((long)dst_h * scale_num / scale_den);
			if (new_w < 1) new_w = 1;
			if (new_h < 1) new_h = 1;
			macsurf_debug_log_writef(
				"img fit-to-cap: %dx%d -> %dx%d (cap=%ld)",
				dst_w, dst_h, new_w, new_h,
				(long)MACOS9_IMG_MAX_DECODED_BYTES);
			dst_w = new_w;
			dst_h = new_h;
		}
	}

	/* fixes111 — aspect-ratio guard for the replaced-element layout
	 * bug. NetSurf's layout has at least one path that fails to
	 * proportionally rescale height when CSS height was originally
	 * auto. The pattern we catch: dst_h == nat_h (height never moved
	 * from intrinsic) while dst_w != nat_w (width was scaled by the
	 * layout container — typically flex's target_main_size or a
	 * max-width:100% on a wide canvas). Result on screen: mactrove's
	 * 1058x245 logo at 1500x245 looks like a thin colored stripe,
	 * 540x338 Dark Castle screenshot at 1700x338 looks horizontally
	 * smeared. Detecting this exact "stuck-height" pattern and
	 * rescaling proportionally fixes the visible glitch without
	 * disturbing genuinely-distorted images. Audit also logs every
	 * image-plot dimension so the layout path's actual outputs are
	 * visible in MacSurf Debug.log for follow-up. */
	if (nat_w > 0 && nat_h > 0 && dst_w > 0 && dst_h > 0) {
		if (dst_h == nat_h && dst_w != nat_w) {
			int new_h = (int)(((long)dst_w * (long)nat_h) /
					(long)nat_w);
			macsurf_debug_log_writef(
				"img aspect-fix: nat=%dx%d req=%dx%d -> %dx%d",
				nat_w, nat_h, dst_w, dst_h, dst_w, new_h);
			dst_h = new_h;
		} else if (nat_w > 0 && nat_h > 0 &&
				dst_w == nat_w && dst_h != nat_h &&
				dst_h > 0) {
			/* fixes126: symmetric mirror of fixes111 above.
			 * NetSurf core's layout produces requests where
			 * the OPPOSITE dim is stuck at intrinsic — width
			 * is unchanged from natural while height was
			 * scaled by CSS (e.g. mactrove logo: nat 1058x245,
			 * CSS height resolves to 92, layout passes
			 * 1058x92 to the plotter). Without correction the
			 * QuickDraw CopyBits scales source 1058x245 into
			 * dst 1058x92 = vertical squash / smear. Detect
			 * the pattern and rescale width to preserve
			 * aspect ratio. */
			int new_w = (int)(((long)dst_h * (long)nat_w) /
					(long)nat_h);
			if (new_w > 0) {
				macsurf_debug_log_writef(
					"img aspect fix126: nat=%dx%d req=%dx%d -> %dx%d",
					nat_w, nat_h, dst_w, dst_h,
					new_w, dst_h);
				dst_w = new_w;
			}
		} else if (dst_w != nat_w || dst_h != nat_h) {
			macsurf_debug_log_writef(
				"img plot: nat=%dx%d req=%dx%d (scaled)",
				nat_w, nat_h, dst_w, dst_h);
		}
	}

	/* fixes162 — display-size deferred decode for QT path. If this is
	 * a QT-deferred entry (importer kept alive in convert) and we have
	 * no bitmap at the right display size, decode now at (dst_w, dst_h)
	 * directly. The existing macos9_qt_decode_to_bitmap parameterizes
	 * by target dims and QT's GraphicsImportDraw scales the source
	 * automatically to fit, so a 1262x580 hero JPEG decoded with
	 * dst_w=320 dst_h=147 produces a 320x147 RGBA buffer (188 KB) not
	 * the natural 2.9 MB.
	 *
	 * LRU is consulted to make room before allocation. If the new
	 * decode wouldn't fit even with everything evicted (single
	 * oversize image), we skip and the plot below no-ops on bitmap
	 * NULL. */
	/* fixes259 — PNG-deferred decode + display-size box-filter. Mirror
	 * the QT-deferred branch below. The compressed PNG bytes are kept
	 * alive in qti->compressed since fixes259's convert path. */
	if (qti->is_png_deferred) {
		bool needs_decode;
		needs_decode = (qti->bitmap == NULL) ||
				(qti->bitmap_w != dst_w) ||
				(qti->bitmap_h != dst_h);
		if (needs_decode && dst_w > 0 && dst_h > 0 &&
		    qti->compressed != NULL) {
			nserror derr;
			long need_bytes;
			extern long macsurf__decoded_img_bytes_current;
			extern long macsurf__site_decoded_img_bytes_peak;

			if (qti->bitmap != NULL) {
				if (guit != NULL && guit->bitmap != NULL &&
				    guit->bitmap->destroy != NULL) {
					guit->bitmap->destroy(qti->bitmap);
				}
				qti->bitmap = NULL;
				if (macsurf__decoded_img_bytes_current >=
				    qti->decoded_bytes) {
					macsurf__decoded_img_bytes_current -=
						qti->decoded_bytes;
				} else {
					macsurf__decoded_img_bytes_current = 0;
				}
				macos9_qti_lru_total_bytes -=
					qti->decoded_bytes;
				if (macos9_qti_lru_total_bytes < 0) {
					macos9_qti_lru_total_bytes = 0;
				}
				macos9_qti_lru_unlink(qti);
				qti->decoded_bytes = 0;
			}

			need_bytes = (long)dst_w * (long)dst_h * 4L;
			if (need_bytes > MACOS9_IMG_MAX_DECODED_BYTES) {
				macsurf_debug_log_writef(
					"img skip: per-image ceiling png %dx%d",
					dst_w, dst_h);
				return true;
			}
			if (!macos9_qti_lru_make_room(need_bytes)) {
				macsurf_debug_log_writef(
					"img skip: cache cap exceeded png %dx%d",
					dst_w, dst_h);
				return true;
			}

			HLock(qti->compressed);
			derr = macos9_png_decode_target(
					(const unsigned char *)*qti->compressed,
					(size_t)GetHandleSize(qti->compressed),
					dst_w, dst_h, &qti->bitmap);
			HUnlock(qti->compressed);
			if (derr != NSERROR_OK || qti->bitmap == NULL) {
				MS_LOG("img redraw: png decode FAIL");
				qti->bitmap = NULL;
				return true;
			}

			qti->bitmap_w = dst_w;
			qti->bitmap_h = dst_h;
			qti->decoded_bytes = need_bytes;
			macsurf__decoded_img_bytes_current += need_bytes;
			macos9_qti_lru_total_bytes += need_bytes;
			if (macsurf__decoded_img_bytes_current >
			    macsurf__site_decoded_img_bytes_peak) {
				macsurf__site_decoded_img_bytes_peak =
					macsurf__decoded_img_bytes_current;
			}
			macos9_qti_lru_insert_mru(qti);
			macsurf_debug_log_writef(
				"img decoded@display png %dx%d -> %ld bytes",
				dst_w, dst_h, need_bytes);
		} else if (qti->bitmap != NULL) {
			macos9_qti_lru_touch(qti);
		}
	} else if (qti->importer != NULL) {
		bool needs_decode;
		needs_decode = (qti->bitmap == NULL) ||
				(qti->bitmap_w != dst_w) ||
				(qti->bitmap_h != dst_h);
		if (needs_decode && dst_w > 0 && dst_h > 0) {
			nserror derr;
			long need_bytes;
			extern long macsurf__decoded_img_bytes_current;
			extern long macsurf__site_decoded_img_bytes_peak;

			/* Drop the existing bitmap if size mismatched. */
			if (qti->bitmap != NULL) {
				if (guit != NULL && guit->bitmap != NULL &&
						guit->bitmap->destroy != NULL) {
					guit->bitmap->destroy(qti->bitmap);
				}
				qti->bitmap = NULL;
				if (macsurf__decoded_img_bytes_current >=
						qti->decoded_bytes) {
					macsurf__decoded_img_bytes_current -=
						qti->decoded_bytes;
				} else {
					macsurf__decoded_img_bytes_current = 0;
				}
				macos9_qti_lru_total_bytes -=
					qti->decoded_bytes;
				if (macos9_qti_lru_total_bytes < 0) {
					macos9_qti_lru_total_bytes = 0;
				}
				macos9_qti_lru_unlink(qti);
				qti->decoded_bytes = 0;
			}

			need_bytes = (long)dst_w * (long)dst_h * 4L;

			/* Per-image ceiling: still skip if even one
			 * display-sized buffer is absurdly big. */
			if (need_bytes > MACOS9_IMG_MAX_DECODED_BYTES) {
				macsurf_debug_log_writef(
					"img skip: per-image ceiling %dx%d "
					"bytes=%ld cap=%ld",
					dst_w, dst_h, need_bytes,
					(long)MACOS9_IMG_MAX_DECODED_BYTES);
				return true;
			}

			/* Evict LRU tail until this fits. */
			if (!macos9_qti_lru_make_room(need_bytes)) {
				macsurf_debug_log_writef(
					"img skip: cache cap exceeded %dx%d",
					dst_w, dst_h);
				return true;
			}

			derr = macos9_qt_decode_to_bitmap(qti->importer,
					dst_w, dst_h, qti->wants_alpha,
					&qti->bitmap);
			if (derr != NSERROR_OK || qti->bitmap == NULL) {
				MS_LOG("img redraw: qt decode FAIL");
				qti->bitmap = NULL;
				return true;
			}

			qti->bitmap_w = dst_w;
			qti->bitmap_h = dst_h;
			qti->decoded_bytes = need_bytes;
			macsurf__decoded_img_bytes_current += need_bytes;
			macos9_qti_lru_total_bytes += need_bytes;
			if (macsurf__decoded_img_bytes_current >
					macsurf__site_decoded_img_bytes_peak) {
				macsurf__site_decoded_img_bytes_peak =
					macsurf__decoded_img_bytes_current;
			}
			macos9_qti_lru_insert_mru(qti);
			macsurf_debug_log_writef(
				"img decoded@display %dx%d -> %ld bytes "
				"(lru_total=%ld)",
				dst_w, dst_h, need_bytes,
				macos9_qti_lru_total_bytes);
		} else if (qti->bitmap != NULL) {
			macos9_qti_lru_touch(qti);
		}
	}

	if (qti->bitmap == NULL) {
		return true;
	}

	flags = 0;
	if (data->repeat_x) flags |= BITMAPF_REPEAT_X;
	if (data->repeat_y) flags |= BITMAPF_REPEAT_Y;

	return ctx->plot->bitmap(ctx, (struct bitmap *)qti->bitmap,
			data->x, data->y,
			dst_w, dst_h,
			data->background_colour,
			flags) == NSERROR_OK;
}

static void
macos9_qt_image_destroy(struct content *c)
{
	macos9_qt_image_content *qti = (macos9_qt_image_content *)c;

	/* fixes162 — symmetric LRU cleanup: unlink before freeing the
	 * bitmap so the LRU walker can't see a stale node, then close
	 * the kept-alive importer if the QT-deferred path was used. */
	if (qti->in_lru) {
		macos9_qti_lru_total_bytes -= qti->decoded_bytes;
		if (macos9_qti_lru_total_bytes < 0) {
			macos9_qti_lru_total_bytes = 0;
		}
		macos9_qti_lru_unlink(qti);
	}

	if (qti->bitmap != NULL) {
		if (guit != NULL && guit->bitmap != NULL &&
				guit->bitmap->destroy != NULL) {
			guit->bitmap->destroy(qti->bitmap);
		}
		qti->bitmap = NULL;
		/* fixes161b — symmetric decrement of the global decoded-bytes
		 * counter. Skipped images never incremented decoded_bytes (it
		 * stays 0 from calloc), so this is safe to always run. */
		if (qti->decoded_bytes > 0) {
			extern long macsurf__decoded_img_bytes_current;
			macsurf__decoded_img_bytes_current -= qti->decoded_bytes;
			if (macsurf__decoded_img_bytes_current < 0) {
				macsurf__decoded_img_bytes_current = 0;
			}
			qti->decoded_bytes = 0;
		}
	}

	if (qti->importer != NULL) {
		CloseComponent(qti->importer);
		qti->importer = NULL;
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
