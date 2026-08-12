/*
 * MacSurf - macsurf_alloc_override.h
 *
 * Forces inclusion of MSL standard library headers and overrides
 * malloc/calloc/realloc with the safe allocator wrappers.
 *
 * By defining these macros after pulling in <stdlib.h>, we ensure the
 * standard declarations are parsed first, and subsequent standard includes
 * are skipped by MSL header guards.
 */

#ifndef MACSURF_ALLOC_OVERRIDE_H
#define MACSURF_ALLOC_OVERRIDE_H

#include <stddef.h>
#include <stdlib.h>

/* Bulletproof allocator functions */
extern void *macsurf_safe_alloc(size_t size);
extern void *macsurf_safe_calloc(size_t count, size_t size);
extern void *macsurf_safe_realloc(void *ptr, size_t size);

#undef malloc
#undef calloc
#undef realloc

#define malloc  macsurf_safe_alloc
#define calloc  macsurf_safe_calloc
#define realloc macsurf_safe_realloc

#endif /* MACSURF_ALLOC_OVERRIDE_H */
