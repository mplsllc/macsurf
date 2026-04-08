/*
 * MacSurf — Mac OS 9 frontend for NetSurf
 * macos9_bitmap.c — All gui_bitmap_table callbacks
 *
 * This file is part of MacSurf, built on the NetSurf engine.
 * Licensed under GPL v2.
 */

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "utils/errors.h"
#include "utils/log.h"
#include "netsurf/bitmap.h"
#include "netsurf/content.h"

#include "macos9/macos9.h"

struct macos9_bitmap {
	int width;
	int height;
	bool opaque;
	unsigned char *data;
};

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

static bool
macos9_bitmap_get_opaque(void *bitmap)
{
	struct macos9_bitmap *bm = bitmap;

	if (bm != NULL) {
		return bm->opaque;
	}
	return false;
}

static unsigned char *
macos9_bitmap_get_buffer(void *bitmap)
{
	struct macos9_bitmap *bm = bitmap;

	if (bm != NULL) {
		return bm->data;
	}
	return NULL;
}

static size_t
macos9_bitmap_get_rowstride(void *bitmap)
{
	struct macos9_bitmap *bm = bitmap;

	if (bm != NULL) {
		return (size_t)bm->width * 4;
	}
	return 0;
}

static int
macos9_bitmap_get_width(void *bitmap)
{
	struct macos9_bitmap *bm = bitmap;

	if (bm != NULL) {
		return bm->width;
	}
	return 0;
}

static int
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

static struct gui_bitmap_table bitmap_table = {
	.create = macos9_bitmap_create,
	.destroy = macos9_bitmap_destroy,
	.set_opaque = macos9_bitmap_set_opaque,
	.get_opaque = macos9_bitmap_get_opaque,
	.get_buffer = macos9_bitmap_get_buffer,
	.get_rowstride = macos9_bitmap_get_rowstride,
	.get_width = macos9_bitmap_get_width,
	.get_height = macos9_bitmap_get_height,
	.modified = macos9_bitmap_modified,
	.render = macos9_bitmap_render,
};

struct gui_bitmap_table *macos9_bitmap_table = &bitmap_table;
