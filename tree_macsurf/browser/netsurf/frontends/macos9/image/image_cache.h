/*
 * MacSurf stub -- image/image_cache.h
 * Minimal C89-compatible stub for CodeWarrior 8 compilation.
 * Licensed under GPL v2.
 */

#ifndef NETSURF_IMAGE_IMAGE_CACHE_H_
#define NETSURF_IMAGE_IMAGE_CACHE_H_

#include <stddef.h>
#include "utils/errors.h"

struct content;
struct bitmap;

typedef struct bitmap *(*image_cache_convert_fn)(struct content *content);

struct image_cache_parameters {
	unsigned int bg_clean_time;
	size_t limit;
	size_t hysteresis;
	size_t speculative_small;
};

nserror image_cache_init(
		const struct image_cache_parameters *params);
nserror image_cache_fini(void);

#endif
