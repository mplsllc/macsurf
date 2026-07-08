/*
 * MacSurf -- macsurf_memory.c
 *
 * Bulletproof allocation wrappers. See macsurf_memory.h for the
 * contract: macsurf_safe_alloc / _calloc / _realloc NEVER return
 * NULL (unless size is zero for realloc, which is a legal free).
 *
 * On allocation failure:
 *   1. Calls MaxBlock() + FreeMem() to capture heap state
 *      (proves fragmentation, not total exhaustion).
 *   2. Logs a FATAL line via macsurf_debug_log_writef + flush.
 *   3. Posts a native StandardAlert (kAlertStopAlert).
 *   4. Calls ExitToShell() for a clean cooperative-app exit.
 *
 * The prefix file (macsurf_prefix.h) redirects malloc/calloc/
 * realloc to these functions via object-like macros. This file
 * #undefs those macros so it calls MSL directly -- no recursion.
 *
 * C89 / CW8 / MSL compatible. Carbon Toolbox only.
 */

/* Restore real allocators before any header pulls in the prefix
 * macros. Must be the very first lines of the file. */
#undef malloc
#undef calloc
#undef realloc
#undef free

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "macsurf_memory.h"
#include "macsurf_debug_log.h"

#ifdef __MACOS9__
#include <Memory.h>
#include <Dialogs.h>
#include <Processes.h>
#endif

/* ------------------------------------------------------------------ */
/* Internal: build a Pascal string in `out` from a C string `src`.    */
/* Truncates at 255 bytes.                                            */
/* ------------------------------------------------------------------ */
static void c_to_pstr(unsigned char *out, const char *src)
{
    size_t len = strlen(src);
    if (len > 255) len = 255;
    out[0] = (unsigned char)len;
    memcpy(out + 1, src, len);
}

/* ------------------------------------------------------------------ */
/* Internal: OOM panic -- log, alert, terminate. Never returns.       */
/* ------------------------------------------------------------------ */
static void macsurf_oom_panic(size_t size)
{
#ifdef __MACOS9__
    long max_blk = (long)MaxBlock();
    long free_mem = (long)FreeMem();
#else
    long max_blk = 0;
    long free_mem = 0;
#endif

    /* 1. Log the failure with fragmentation proof. */
    macsurf_debug_log_writef(
        "FATAL OOM: alloc(%ld) failed  "
        "FreeMem=%ld  MaxBlock=%ld  (fragmented)",
        (long)size, free_mem, max_blk);
    macsurf_debug_log_flush();

#ifdef __MACOS9__
    /* 2. Build a human-readable alert message.
     * sprintf is stack-safe and links on CW8 MSL
     * (vsnprintf is the problematic one). */
    {
        unsigned char ptitle[256];
        unsigned char pbody[256];
        char body_c[256];
        short item;

        c_to_pstr(ptitle,
            "MacSurf: Out of Contiguous Memory");

        sprintf(body_c,
            "Need %ld bytes, largest gap %ld bytes "
            "(free %ld). Heap is too fragmented to continue.",
            (long)size, max_blk, free_mem);

        c_to_pstr(pbody, body_c);

        /* 3. Show the alert (blocks until dismissed). */
        StandardAlert(kAlertStopAlert, ptitle, pbody, NULL, &item);
    }

    /* 4. Clean exit -- cooperative-app safe. */
    ExitToShell();
#else
    /* Non-Mac build: crash loudly so tests catch it. */
    abort();
#endif
}

/* ------------------------------------------------------------------ */
/* macsurf_safe_alloc                                                  */
/* ------------------------------------------------------------------ */
void *macsurf_safe_alloc(size_t size)
{
    void *p;
    if (size == 0) size = 1;
    p = malloc(size);
    if (p != NULL) return p;
    macsurf_oom_panic(size);
    return NULL; /* unreachable */
}

/* ------------------------------------------------------------------ */
/* macsurf_safe_calloc                                                 */
/* ------------------------------------------------------------------ */
void *macsurf_safe_calloc(size_t count, size_t size)
{
    void *p;
    if (count == 0 || size == 0) { count = 1; size = 1; }
    p = calloc(count, size);
    if (p != NULL) return p;
    macsurf_oom_panic(count * size);
    return NULL; /* unreachable */
}

/* ------------------------------------------------------------------ */
/* macsurf_safe_realloc                                                */
/* ------------------------------------------------------------------ */
void *macsurf_safe_realloc(void *ptr, size_t size)
{
    void *p;

    /* realloc(ptr, 0) is a free -- NULL return is legal per C89 */
    if (size == 0) {
        free(ptr);
        return NULL;
    }

    /* realloc(NULL, size) is a malloc */
    if (ptr == NULL)
        return macsurf_safe_alloc(size);

    p = realloc(ptr, size);
    if (p != NULL) return p;

    /* Original pointer is still valid, but we are about to
     * ExitToShell so the leak is irrelevant -- stopping the
     * $0000 write is the only priority. */
    macsurf_oom_panic(size);
    return NULL; /* unreachable */
}
