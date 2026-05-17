/*
 * MacSurf — Mac OS 9 frontend for NetSurf
 * macos9_bitmap.c — All gui_bitmap_table callbacks
 *
 * This file is part of MacSurf, built on the NetSurf engine.
 * Licensed under GPL v2.
 */

#include <stdlib.h>
#include <string.h>

#include "utils/errors.h"
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
};

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

	if (bm != NULL) {
		if (bm->mask != NULL) free(bm->mask);
		free(bm->data);
		free(bm);
	}
}

static void
macos9_bitmap_set_opaque(void *bitmap, bool opaque)
{
	struct macos9_bitmap *bm = bitmap;

	if (bm != NULL) {
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
	/* TODO: invalidate any cached GWorld representation */
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
