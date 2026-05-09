/*
 * MacSurf stub -- image/image.h
 * Minimal C89-compatible stub for CodeWarrior 8 compilation.
 * Licensed under GPL v2.
 */

#ifndef NETSURF_IMAGE_IMAGE_H_
#define NETSURF_IMAGE_IMAGE_H_

#include "utils/ns_errors.h"

struct bitmap;
struct content_redraw_data;
struct rect;
struct redraw_context;

nserror image_init(void);

unsigned char image_bitmap_plot(struct bitmap *bitmap,
		struct content_redraw_data *data,
		const struct rect *clip,
		const struct redraw_context *ctx);

#endif
