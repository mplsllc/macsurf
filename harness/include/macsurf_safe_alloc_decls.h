/* harness: force-included so every TU sees the REAL void*-returning
 * prototypes for macsurf_safe_alloc/calloc/realloc before first use.
 *
 * On the real Mac build, macsurf_prefix.h is injected before every
 * compilation unit (CW8 project prefix-file mechanism) and declares these.
 * The harness compiles files standalone without that injection, so a file
 * like macsurf_qjs.c that calls macsurf_safe_calloc() without including
 * macsurf_prefix.h/macsurf_alloc_override.h gets an IMPLICIT (int-returning)
 * declaration under -w -- on a 64-bit target that silently truncates every
 * returned pointer to 32 bits (0x515000002100 -> 0x2100), a real bug class
 * but one that cannot occur on the real Mac build. This header closes that
 * harness-only gap; it does not redirect malloc/calloc/realloc themselves. */
#ifndef MACSURF_SAFE_ALLOC_DECLS_H
#define MACSURF_SAFE_ALLOC_DECLS_H
#include <stddef.h>
extern void *macsurf_safe_alloc(size_t size);
extern void *macsurf_safe_calloc(size_t count, size_t size);
extern void *macsurf_safe_realloc(void *ptr, size_t size);
#endif
