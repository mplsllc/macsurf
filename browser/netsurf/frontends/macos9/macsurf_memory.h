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

/*
 * fixes711 (#207) blank-screen reconnaissance. Both emit 'RECON'
 * lines that survive the crash-only log gate.
 *
 * macsurf_recon_mem(tag): one flushed line -- VM on/off (Gestalt),
 *   FreeMem, MaxBlock (contiguity), Temp + Purge pools. Call it very
 *   early at launch so an early blank still leaves the baseline, and
 *   again per navigation to snapshot the heap before each page.
 *
 * macsurf_recon_note(where,a,b,n): throttled NULL-pointer report from
 *   the libcss selection hot path; lets the caller bail safely instead
 *   of dereferencing NULL. Implemented here so libcss needs only an
 *   extern declaration, no frontend headers.
 */
void macsurf_recon_mem(const char *tag);
void macsurf_recon_note(const char *where, const void *a,
                        const void *b, long n);

/*
 * fixes712a (#207) blank-page harness.
 *
 * Poison bytes. Only malloc is poisoned -- calloc's contract is zeroed
 * memory, and the malloc/calloc distinction is exactly what we're testing.
 * Read back through a 32-bit signed field these become:
 *     0xA5A5A5A5 -> -1515870811   (never-initialised memory)
 *     0xDDDDDDDD ->  -572662307   (freed memory; reserved for fixes712b)
 * Both trip the redraw clamp's < -200000 tests, so the VALUE ITSELF says
 * which bug we caught.
 */
#define MACSURF_POISON_ALLOC_BYTE 0xA5
#define MACSURF_POISON_FREE_BYTE  0xDD
#define MACSURF_POISON_ALLOC_WORD (-1515870811L)
#define MACSURF_POISON_FREE_WORD   (-572662307L)

/*
 * Called by the redraw defensive clamp when it finds a garbage box field.
 * Until now that clamp silently overwrote the value -- and a zeroed root
 * height collapses the clip and paints a blank page. Throttled; decodes the
 * poison patterns above. Ungated: it costs nothing unless garbage appears.
 */
void macsurf_recon_clamp(const char *field, long value);

/*
 * fixes719 (#207) — runtime application-partition pointer bounds. Call
 * macsurf_heap_bounds_init() ONCE at startup (in main(), after
 * macsurf_debug_log_init() and before any string interning / URL parse /
 * content fetch) to capture this process's real partition window from the
 * Process Manager. Then the two predicates replace every hardcoded
 * 0x01000000/0x20000000/0x28000000 pointer-range guard:
 *   macsurf_ptr_is_heap(p)  -> p is inside the app partition (malloc'd objs)
 *   macsurf_ptr_is_valid(p) -> p is merely non-NULL/non-tiny (may be a
 *                              static const vtable / PEF literal off-heap)
 * Library / core files that cannot include this header reach them with a
 * bare `extern int macsurf_ptr_is_heap(const void *);` at the point of use,
 * exactly as they already do for macsurf_debug_log_writef.
 */
void macsurf_heap_bounds_init(void);
int  macsurf_ptr_is_heap(const void *p);
int  macsurf_ptr_is_valid(const void *p);


#endif /* MACSURF_MEMORY_H */
