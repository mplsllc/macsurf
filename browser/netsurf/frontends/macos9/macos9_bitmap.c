/*
 * MacSurf — Mac OS 9 frontend for NetSurf
 * macos9_bitmap.c — All gui_bitmap_table callbacks
 *
 * This file is part of MacSurf, built on the NetSurf engine.
 * Licensed under GPL v2.
 */

#include <stdlib.h>
#include <string.h>

#include "utils/ns_errors.h"
#include "utils/log.h"
#include "netsurf/bitmap.h"
#include "netsurf/content.h"

#include "macos9.h"

struct macos9_bitmap {
	int width;
	int height;
	bool opaque;
	unsigned char *data;
	/* Optional 1-bit alpha mask, packed MSB-first per byte, rounded up
	 * to whole bytes per row. NULL when the bitmap is opaque or when
	 * the decoder didn't supply one. Plotter checks this pointer to
	 * decide between srcCopy and CopyMask. */
	unsigned char *mask;
	int mask_rowbytes;
	/* fixes648 (Track C1, #43/#168): cached PREPARED source for repaints.
	 * macos9_plot_bitmap builds a 32-bit GWorld (RGBA->XRGB byte-swap, plus a
	 * box-filter downscale when the image is shrunk >=1.5x) on EVERY paint —
	 * the measured ~2.5s-per-paint image cost and the scroll-jank cause. We
	 * keep that finished, ready-to-blit GWorld here so repaints at the same
	 * render size skip straight to the CopyBits/CopyMask. prep_gw stays
	 * LockPixels'd for the entry's whole life (NewGWorld pixmaps are purgeable
	 * when unlocked). prep_mask is the box-filtered dest-sized 1-bit mask for
	 * the downscaled case (NULL => the blit uses the live `mask`). The cache
	 * lives ON the bitmap and is disposed in macos9_bitmap_destroy/_modified/
	 * _set_opaque/_set_mask, so it can NEVER outlive or mismatch its bitmap —
	 * keeping the #168 stale-pointer class OFF this new surface (the GWorld is
	 * NOT hung on the fragile qti LRU node). A small global byte-budgeted LRU
	 * bounds total prepared memory; oversize images fall back to the transient
	 * build-and-dispose path. */
	unsigned char *prep_mask;
	int prep_mask_rowbytes;
	int prep_w;
	int prep_h;
	int prep_src_w;
	int prep_src_h;
	bool prep_opaque;
	bool prep_had_mask;
	long prep_bytes;
	struct macos9_bitmap *prep_lru_prev;
	struct macos9_bitmap *prep_lru_next;
	bool prep_in_lru;
#ifdef __MACOS9__
	GWorldPtr prep_gw;
#endif
};

#ifdef __MACOS9__
/* Global byte-budgeted LRU of prepared GWorlds (intrusive links inside the
 * bitmap struct, mirroring macos9_image.c's macos9_qti_lru_*). Eviction drops
 * ONLY the prepared GWorld+mask of the least-recently-blitted bitmap; the
 * bitmap and its RGBA buffer are untouched (its next paint just re-prepares). */
static struct macos9_bitmap *g_prep_lru_head = NULL;  /* MRU */
static struct macos9_bitmap *g_prep_lru_tail = NULL;  /* LRU */
static long g_prep_lru_bytes = 0;
#define MACOS9_PREP_GW_MAX_BYTES (8L * 1024L * 1024L)

static void macos9_prep_lru_unlink(struct macos9_bitmap *bm)
{
	if (!bm->prep_in_lru) return;
	/* fixes650b (review): only rewrite a global if it actually points at bm.
	 * After a corruption-sever (head=tail=NULL) a stranded entry can still
	 * have prep_in_lru==true with prep_lru_prev==NULL; a naive
	 * "else head = next" would then revive the head with a dangling node. */
	if (bm->prep_lru_prev != NULL)
		bm->prep_lru_prev->prep_lru_next = bm->prep_lru_next;
	else if (g_prep_lru_head == bm)
		g_prep_lru_head = bm->prep_lru_next;
	if (bm->prep_lru_next != NULL)
		bm->prep_lru_next->prep_lru_prev = bm->prep_lru_prev;
	else if (g_prep_lru_tail == bm)
		g_prep_lru_tail = bm->prep_lru_prev;
	bm->prep_lru_prev = NULL;
	bm->prep_lru_next = NULL;
	bm->prep_in_lru = false;
}

static void macos9_prep_lru_insert_mru(struct macos9_bitmap *bm)
{
	bm->prep_lru_prev = NULL;
	bm->prep_lru_next = g_prep_lru_head;
	if (g_prep_lru_head != NULL) g_prep_lru_head->prep_lru_prev = bm;
	g_prep_lru_head = bm;
	if (g_prep_lru_tail == NULL) g_prep_lru_tail = bm;
	bm->prep_in_lru = true;
}

static void macos9_prep_lru_touch(struct macos9_bitmap *bm)
{
	if (!bm->prep_in_lru) return;
	macos9_prep_lru_unlink(bm);
	macos9_prep_lru_insert_mru(bm);
}

/* Single dispose primitive: unlock + dispose the prepared GWorld, free the
 * box-filtered mask, unlink from the LRU, subtract the byte charge, and zero
 * the prep_* state. Alignment-guarded like the fixes430/431 data/mask frees. */
static void macos9_prep_dispose_locked(struct macos9_bitmap *bm)
{
	if (bm == NULL) return;
	if (bm->prep_gw != NULL) {
		if (((unsigned long)bm->prep_gw & 3) == 0) {
			PixMapHandle ppm = GetGWorldPixMap(bm->prep_gw);
			if (ppm != NULL) UnlockPixels(ppm);
			DisposeGWorld(bm->prep_gw);
		}
		bm->prep_gw = NULL;
	}
	if (bm->prep_mask != NULL) {
		if (((unsigned long)bm->prep_mask & 3) == 0) free(bm->prep_mask);
		bm->prep_mask = NULL;
	}
	if (bm->prep_in_lru) macos9_prep_lru_unlink(bm);
	g_prep_lru_bytes -= bm->prep_bytes;
	if (g_prep_lru_bytes < 0) g_prep_lru_bytes = 0;
	bm->prep_bytes = 0;
	bm->prep_w = 0;
	bm->prep_h = 0;
	bm->prep_src_w = 0;
	bm->prep_src_h = 0;
	bm->prep_mask_rowbytes = 0;
	bm->prep_had_mask = false;
}

/* Evict LRU-tail prepared entries until need_bytes fits the budget. */
static bool macos9_prep_make_room(long need_bytes)
{
	while (g_prep_lru_bytes + need_bytes > MACOS9_PREP_GW_MAX_BYTES &&
			g_prep_lru_tail != NULL) {
		struct macos9_bitmap *victim = g_prep_lru_tail;
		if (((unsigned long)victim & 3) != 0) {
			/* Corrupted tail link — sever the walk, don't deref. */
			g_prep_lru_tail = NULL;
			g_prep_lru_head = NULL;
			break;
		}
		macos9_prep_dispose_locked(victim);
	}
	return (g_prep_lru_bytes + need_bytes <= MACOS9_PREP_GW_MAX_BYTES);
}

/* Public: drop any prepared entry (invalidation-hook wrapper). */
void macos9_bitmap_drop_prepared(void *bitmap)
{
	macos9_prep_dispose_locked((struct macos9_bitmap *)bitmap);
}

/* Public (plotters.c): return the prepared GWorld iff it matches the render
 * size AND opaque/mask-presence; else drop the stale one and return NULL. On a
 * hit, fills out_src_w/h + out_mask/out_mask_rowbytes and marks the entry MRU.
 * out_mask == NULL means "use the live bm->mask" (non-downscaled case). */
GWorldPtr macos9_bitmap_get_prepared(void *bitmap, int render_w, int render_h,
		bool is_opaque, int *out_src_w, int *out_src_h,
		unsigned char **out_mask, int *out_mask_rowbytes)
{
	struct macos9_bitmap *bm = bitmap;
	bool had_mask;
	if (bm == NULL || bm->prep_gw == NULL) return NULL;
	had_mask = (bm->mask != NULL) && !is_opaque;
	if (bm->prep_w != render_w || bm->prep_h != render_h ||
			bm->prep_opaque != is_opaque ||
			bm->prep_had_mask != had_mask) {
		macos9_prep_dispose_locked(bm);
		return NULL;
	}
	if (out_src_w != NULL) *out_src_w = bm->prep_src_w;
	if (out_src_h != NULL) *out_src_h = bm->prep_src_h;
	if (out_mask != NULL) *out_mask = bm->prep_mask;
	if (out_mask_rowbytes != NULL) *out_mask_rowbytes = bm->prep_mask_rowbytes;
	macos9_prep_lru_touch(bm);
	return bm->prep_gw;
}

/* Public (plotters.c): store a freshly-prepared GWorld (still LockPixels'd) +
 * optional box-filtered mask. Takes ownership + returns true on success.
 * Returns false (caller keeps + disposes transiently) if the entry exceeds the
 * budget or room can't be made. */
bool macos9_bitmap_set_prepared(void *bitmap, GWorldPtr gw, int render_w,
		int render_h, int src_w, int src_h, unsigned char *mask,
		int mask_rowbytes, bool is_opaque, long bytes)
{
	struct macos9_bitmap *bm = bitmap;
	if (bm == NULL) return false;
	macos9_prep_dispose_locked(bm);
	if (bytes <= 0 || bytes > MACOS9_PREP_GW_MAX_BYTES) return false;
	if (!macos9_prep_make_room(bytes)) return false;
	bm->prep_gw = gw;
	bm->prep_w = render_w;
	bm->prep_h = render_h;
	bm->prep_src_w = src_w;
	bm->prep_src_h = src_h;
	bm->prep_mask = mask;
	bm->prep_mask_rowbytes = mask_rowbytes;
	bm->prep_opaque = is_opaque;
	bm->prep_had_mask = (mask != NULL) ||
			((bm->mask != NULL) && !is_opaque);
	bm->prep_bytes = bytes;
	g_prep_lru_bytes += bytes;
	macos9_prep_lru_insert_mru(bm);
	return true;
}
#endif /* __MACOS9__ */

unsigned char *
macos9_bitmap_get_mask(void *bitmap)
{
	struct macos9_bitmap *bm = bitmap;
	if (bm != NULL) return bm->mask;
	return NULL;
}

int
macos9_bitmap_get_mask_rowbytes(void *bitmap)
{
	struct macos9_bitmap *bm = bitmap;
	if (bm != NULL) return bm->mask_rowbytes;
	return 0;
}

/* Decoders that have real per-pixel alpha call this to attach a 1-bit
 * mask to the bitmap. The mask buffer is owned by the bitmap thereafter
 * and freed at destroy time. */
void
macos9_bitmap_set_mask(void *bitmap, unsigned char *mask, int mask_rowbytes)
{
	struct macos9_bitmap *bm = bitmap;
	if (bm == NULL) return;
#ifdef __MACOS9__
	/* fixes648: attaching/replacing the 1-bit mask changes mask-presence and
	 * the box-filtered prep_mask — drop any prepared entry. */
	macos9_prep_dispose_locked(bm);
#endif
	if (bm->mask != NULL) free(bm->mask);
	bm->mask = mask;
	bm->mask_rowbytes = mask_rowbytes;
}

static void *
macos9_bitmap_create(int width, int height, enum gui_bitmap_flags flags)
{
	struct macos9_bitmap *bm;

	bm = calloc(1, sizeof(*bm));
	if (bm == NULL) {
		return NULL;
	}

	bm->width = width;
	bm->height = height;
	bm->opaque = (flags & BITMAP_OPAQUE) != 0;

	bm->data = calloc((size_t)width * height, 4);
	if (bm->data == NULL) {
		free(bm);
		return NULL;
	}

	return bm;
}

static void
macos9_bitmap_destroy(void *bitmap)
{
	struct macos9_bitmap *bm = bitmap;

	if (bm == NULL) return;

	/* fixes431: guard the bitmap struct pointer itself.
	 * If victim->bitmap is a corrupted LRU entry pointer (e.g. a
	 * free-list chain link like 0xD762FD0E with low bits != 00),
	 * free(bm) crashes inside MSL's heap scanner.  An aligned heap
	 * allocation is always 4-byte aligned on PPC. */
	if (((unsigned long)bm & 3) != 0) return;

#ifdef __MACOS9__
	/* fixes648 (Track C1): release the cached prepared GWorld + unlink it from
	 * the prep-LRU before freeing the bitmap, so a cached GWorld can never
	 * outlive its bitmap. This is the single funnel every LRU-evict / purge /
	 * display-size re-decode / content-destroy routes through
	 * (guit->bitmap->destroy) — the whole C1 invalidation safety net. Placed
	 * after the bm-align guard so we never dereference a corrupted bm. */
	macos9_prep_dispose_locked(bm);
#endif

	/* fixes430: guard against corrupted data/mask pointers.
	 * Heap pointers on OS 9 PPC are always 4-byte aligned.
	 * A free-list chain pointer (e.g. 0xD762FD0E, low bits != 00)
	 * means the block was already freed and the first word was
	 * overwritten by the allocator.  Skip the inner free to avoid
	 * crashing inside MSL's heap manager; the outer bm struct is
	 * still coherent and is freed normally. */
	if (bm->mask != NULL) {
		if (((unsigned long)bm->mask & 3) == 0)
			free(bm->mask);
		bm->mask = NULL;
	}
	if (bm->data != NULL) {
		if (((unsigned long)bm->data & 3) == 0)
			free(bm->data);
		bm->data = NULL;
	}
	free(bm);
}

static void
macos9_bitmap_set_opaque(void *bitmap, bool opaque)
{
	struct macos9_bitmap *bm = bitmap;

	if (bm != NULL) {
#ifdef __MACOS9__
		/* fixes648: opaque flip switches the CopyMask/CopyBits branch, so
		 * any prepared entry is stale. */
		if (bm->opaque != opaque) macos9_prep_dispose_locked(bm);
#endif
		bm->opaque = opaque;
	}
}

bool
macos9_bitmap_get_opaque(void *bitmap)
{
	struct macos9_bitmap *bm = bitmap;

	if (bm != NULL) {
		return bm->opaque;
	}
	return false;
}

unsigned char *
macos9_bitmap_get_buffer(void *bitmap)
{
	struct macos9_bitmap *bm = bitmap;

	if (bm != NULL) {
		return bm->data;
	}
	return NULL;
}

size_t
macos9_bitmap_get_rowstride(void *bitmap)
{
	struct macos9_bitmap *bm = bitmap;

	if (bm != NULL) {
		return (size_t)bm->width * 4;
	}
	return 0;
}

int
macos9_bitmap_get_width(void *bitmap)
{
	struct macos9_bitmap *bm = bitmap;

	if (bm != NULL) {
		return bm->width;
	}
	return 0;
}

int
macos9_bitmap_get_height(void *bitmap)
{
	struct macos9_bitmap *bm = bitmap;

	if (bm != NULL) {
		return bm->height;
	}
	return 0;
}

static void
macos9_bitmap_modified(void *bitmap)
{
#ifdef __MACOS9__
	/* fixes648 (Track C1): in-place pixel mutation (progressive/interlaced
	 * decode, animated-GIF next frame) invalidates the prepared GWorld — drop
	 * it so the next paint re-prepares from the new pixels. This is exactly
	 * what the long-standing TODO here asked for. */
	struct macos9_bitmap *bm = bitmap;
	if (bm != NULL) macos9_prep_dispose_locked(bm);
#else
	(void)bitmap;
#endif
}

static nserror
macos9_bitmap_render(struct bitmap *bitmap, struct hlcache_handle *content)
{
	/* TODO: SetGWorld() to offscreen, call content_scaled_redraw, restore */
	return NSERROR_OK;
}

/* Field order: create, destroy, set_opaque, get_opaque, get_buffer,
 * get_rowstride, get_width, get_height, modified, render
 * (see include/netsurf/bitmap.h) */
static struct gui_bitmap_table bitmap_table = {
	macos9_bitmap_create,
	macos9_bitmap_destroy,
	macos9_bitmap_set_opaque,
	macos9_bitmap_get_opaque,
	macos9_bitmap_get_buffer,
	macos9_bitmap_get_rowstride,
	macos9_bitmap_get_width,
	macos9_bitmap_get_height,
	macos9_bitmap_modified,
	macos9_bitmap_render
};

struct gui_bitmap_table *macos9_bitmap_table = &bitmap_table;
