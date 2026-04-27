/*
 * MacSurf stub -- css/css.h
 * Minimal C89-compatible stub for CodeWarrior 8 compilation.
 * Licensed under GPL v2.
 */

#ifndef NETSURF_CSS_CSS_H_
#define NETSURF_CSS_CSS_H_

#include <stdint.h>
#include "utils/ns_errors.h"
#include <libcss/libcss.h>

struct hlcache_handle;

struct nscss_import {
	struct hlcache_handle *c;
};

nserror nscss_init(void);
css_stylesheet *nscss_get_stylesheet(struct hlcache_handle *h);
struct nscss_import *nscss_get_imports(struct hlcache_handle *h,
		uint32_t *n);

#endif
