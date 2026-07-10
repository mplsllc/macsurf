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

/* fixes712a/713 blank-page harness (RETIRED, kept re-armable).
 * Define MACSURF_POISON to fill every malloc() with 0xA5, reproducing the
 * VM-off "garbage in fresh memory" condition on any machine. Used to prove
 * there is NO uninitialised-heap read on the render path: a full page load
 * completed clean with poison armed. Left off -- it memsets every malloc. */

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
#include <Gestalt.h>
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

/* ================================================================== */
/* fixes711 (#207) BLANK-SCREEN RECONNAISSANCE                          */
/*                                                                      */
/* Two observers, both routed through the 'RECON' crash-only log gate:  */
/*   macsurf_recon_mem(tag)  -- one line per call: VM on/off (Gestalt) +*/
/*      FreeMem / MaxBlock (contiguity) / Temp + Purge pools. Labels a  */
/*      whole run VM-on vs VM-off and shows fragmentation. Flushed so an */
/*      early blank still leaves the baseline on disk.                  */
/*   macsurf_recon_note(...) -- called from the libcss selection hot    */
/*      path when a per-node pointer that must never be NULL is NULL     */
/*      (the $0000-scribble seen in the G3 StdLog). Logs, throttled, so  */
/*      the caller can bail safely instead of writing through NULL.     */
/* Remove this whole block + the call sites for a release build.        */
/* ================================================================== */

void macsurf_recon_mem(const char *tag)
{
#ifdef __MACOS9__
    long vmresp = 0;
    long vm_on;
    long freem, maxblk;
    long tmpfree, tmpmax;
    long purge_total = 0, purge_contig = 0;
    Size grow = 0;

#ifndef gestaltVMAttr
#define gestaltVMAttr 'vm  '
#endif
#ifndef gestaltVMPresent
#define gestaltVMPresent 0
#endif

    if (Gestalt(gestaltVMAttr, &vmresp) != noErr)
        vm_on = -1;                         /* couldn't query */
    else
        vm_on = (vmresp & (1L << gestaltVMPresent)) ? 1 : 0;

    freem  = (long)FreeMem();
    maxblk = (long)MaxBlock();
    tmpfree = (long)TempFreeMem();
    tmpmax  = (long)TempMaxMem(&grow);
    PurgeSpace(&purge_total, &purge_contig);

    /* One line, well under the 255-byte writef cap. vm=1 on, 0 off,
     * -1 unknown. maxblk << free proves fragmentation. */
    macsurf_debug_log_writef(
        "RECON MEM %s vm=%ld free=%ld maxblk=%ld tmpfree=%ld tmpmax=%ld purge=%ld/%ld",
        (tag != NULL) ? tag : "?",
        vm_on, freem, maxblk, tmpfree, tmpmax, purge_total, purge_contig);
    macsurf_debug_log_flush();
#else
    (void)tag;
#endif
}

void macsurf_recon_note(const char *where, const void *a,
                        const void *b, long n)
{
    static long count = 0;
    if (count >= 40) return;                /* cap the flood */
    count++;
    macsurf_debug_log_writef("RECON SELNULL %s a=%p b=%p n=%ld",
        (where != NULL) ? where : "?", a, b, n);
    macsurf_debug_log_flush();
    if (count == 40)
        macsurf_debug_log_writef("RECON SELNULL (capped at 40)");
}

/* fixes712a: the redraw defensive clamp found a garbage box field. Decode
 * the value against the poison patterns so the log says WHERE the garbage
 * came from, not merely that it existed. */
void macsurf_recon_clamp(const char *field, long value)
{
    static long count = 0;
    const char *kind;

    if (count >= 60) return;                /* cap the flood */
    count++;

    if (value == MACSURF_POISON_ALLOC_WORD)     kind = "UNINIT";
    else if (value == MACSURF_POISON_FREE_WORD) kind = "FREED";
    else                                        kind = "OTHER";

    macsurf_debug_log_writef("RECON CLAMP %s=%ld (%s)",
        (field != NULL) ? field : "?", value, kind);
    macsurf_debug_log_flush();

    if (count == 60)
        macsurf_debug_log_writef("RECON CLAMP (capped at 60)");
}

/* ------------------------------------------------------------------ */
/* macsurf_safe_alloc                                                  */
/* ------------------------------------------------------------------ */
void *macsurf_safe_alloc(size_t size)
{
    void *p;
    if (size == 0) size = 1;
    p = malloc(size);
    if (p == NULL) macsurf_oom_panic(size); /* never returns */
#ifdef MACSURF_POISON
    /* fixes712a: malloc returns INDETERMINATE memory. Under Virtual Memory
     * a fresh page is zero-filled, so an uninitialised read is silently
     * benign; with VM off it reads whatever the previous process left.
     * Poisoning reproduces the VM-off worst case on ANY machine, VM on or
     * off -- removing our dependence on a reporter's unlucky heap.
     * calloc is deliberately NOT poisoned: zeroed memory is its contract. */
    memset(p, MACSURF_POISON_ALLOC_BYTE, size);
#endif
    return p;
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
