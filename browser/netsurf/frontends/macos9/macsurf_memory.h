/*
 * MacSurf -- macsurf_memory.h
 *
 * Bulletproof allocation wrappers for the NetSurf / QuickJS
 * integration layer. On Classic Mac OS, a NULL dereference from a
 * failed malloc writes through address 0x00000000, corrupting the
 * Low Memory Globals and halting the machine instantly.
 *
 * These functions NEVER return NULL (except safe_realloc with
 * size==0, which is a legal free). On failure they log
 * fragmentation diagnostics, post a native StandardAlert, and
 * call ExitToShell().
 *
 * C89 compatible. Uses Carbon Toolbox (Memory.h, Dialogs.h).
 */

#ifndef MACSURF_MEMORY_H
#define MACSURF_MEMORY_H

#include <stddef.h>

/*
 * Allocate `size` bytes. Returns a valid pointer on success.
 * On failure this function does NOT return -- it halts the
 * application with a user-visible alert.
 */
void *macsurf_safe_alloc(size_t size);

/*
 * Zero-filled allocation. Same never-returns-NULL guarantee.
 */
void *macsurf_safe_calloc(size_t count, size_t size);

/*
 * Grow or shrink a block. Never returns NULL when size > 0.
 * realloc(ptr, 0) is treated as free(ptr) and returns NULL
 * per C89 convention.
 */
void *macsurf_safe_realloc(void *ptr, size_t size);

#endif /* MACSURF_MEMORY_H */
