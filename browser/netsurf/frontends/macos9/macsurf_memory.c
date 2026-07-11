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

/* ==================================================================
 * fixes719 (#207) — runtime application-partition pointer bounds.
 *
 * THE BLANK-SCREEN BUG. Dozens of "is this pointer valid?" guards
 * across libwapcaplet, nsurl, libdom, hlcache/llcache, ns_content and
 * browser_window were hardcoded to assume a live heap pointer lives in
 * a FIXED low window (0x01000000..0x20000000, or ..0x28000000). That is
 * false: there is no kernel boundary at 0x28000000 on classic Mac OS —
 * it is just 640 MB of a flat 32-bit space. WHERE the Process Manager
 * maps MacSurf's partition depends on the machine's RAM. On the dev
 * iMac/minitower the partition lands low (0x10..) so the guards work;
 * on a higher-RAM Mac (user "autumn": partition at 0x2B..) EVERY valid
 * heap pointer is above the ceiling, so the guards fire on everything.
 * The behaviour-changing ones then break string interning
 * (libwapcaplet.c:128 breaks the intern hash-walk -> dedup fails -> the
 * nav URL's 'https' scheme gets a different lwc_string* than the
 * fetcher's registered 'https' -> lwc_string_isequal pointer-compare
 * fails -> get_fetcher_for_scheme == -1 -> NSERROR_NO_FETCH_HANDLER ->
 * no fetch is EVER started for any https URL -> current_content stays
 * NULL -> permanent blank page), plus reject valid cache/content
 * handles. Reproduced identically across two of autumn's launches.
 *
 * Fix: capture the REAL partition window once at startup from the
 * Process Manager (processLocation .. +processSize = the whole
 * partition: heap + gap + stack + globals), and test pointers against
 * that instead of the machine-specific constants. This still rejects
 * the genuine corruption the guards were added for (NULL/tiny scribble
 * below the partition; freed sched_entry alias ~0x07.. below the base;
 * code-space alias ~0x3E.. above the top) while ACCEPTING a valid high
 * heap pointer like 0x2B487BF0. Using the full partition (not
 * GetApplLimit(), which excludes the ~107 MB stack region) makes the
 * window immune to heap-growth / ApplLimit timing. Fails OPEN
 * (accept-all above the NULL page) if the Toolbox can't give a sane
 * window, so it can never re-introduce the blank page.
 * ================================================================== */
static unsigned long g_ptr_lo = 0x00001000UL;   /* fail-open: reject NULL page */
static unsigned long g_ptr_hi = 0xFFFFFFFFUL;   /* fail-open until init narrows */
static int           g_ptr_bounds_ok = 0;

void macsurf_heap_bounds_init(void)
{
#ifdef __MACOS9__
	ProcessSerialNumber psn;
	ProcessInfoRec      info;
	OSErr               err;
	unsigned long       lo = 0UL;
	unsigned long       hi = 0UL;
	/* fixes719a: GetApplLimit() is not exported by the CW8 link (undefined
	 * at link time), so it's dropped from the diagnostic. The real bounds
	 * come from GetProcessInformation below, not from ApplLimit. StackSpace()
	 * DOES link (used in macsurf_qjs.c) so it stays as a cushion readout. */
	extern long StackSpace(void);

	psn.highLongOfPSN = 0;
	psn.lowLongOfPSN  = kCurrentProcess;
	memset(&info, 0, sizeof(info));
	info.processInfoLength = sizeof(ProcessInfoRec);
	info.processName    = NULL;
	info.processAppSpec = NULL;
	err = GetProcessInformation(&psn, &info);
	if (err == noErr) {
		lo = (unsigned long)info.processLocation;
		hi = lo + (unsigned long)info.processSize;
	}

	/* One-shot diagnostic (RECON survives the crash-only log gate): the
	 * real partition window + heap ceiling + stack cushion, so a high-base
	 * machine's log PROVES its 0x2B.. pointers fall inside the window. */
	macsurf_debug_log_writef(
		"RECON HEAP BOUNDS lo=%p hi=%p size=%ld stack=%ld ok=%d",
		(void *)lo, (void *)hi, (long)info.processSize,
		(long)StackSpace(),
		(err == noErr) ? 1 : 0);

	/* Validate before trusting; fail OPEN on anything suspicious so a bad
	 * Toolbox return can never NARROW the window and false-reject valid
	 * pointers (which would re-create the blank page). Require a sane
	 * partition >= 8 MB. */
	if (err == noErr && hi > lo && (hi - lo) >= 0x00800000UL) {
		g_ptr_lo = lo;
		g_ptr_hi = hi;
		g_ptr_bounds_ok = 1;
	} else {
		macsurf_debug_log_writef(
			"RECON HEAP BOUNDS BAD err=%d -- fail-open accept-all",
			(int)err);
	}
	macsurf_debug_log_flush();
#endif
}

/* STRICT partition test — for malloc'd objects (lwc_string*, interned
 * dom_string, nsurl components, llcache_object, hlcache handle/entry/
 * content, content_user nodes, source buffers, parser, qjs dom_node).
 * These always come from MSL malloc / NewPtr backed by the app zone, so
 * they live inside the captured partition window on ANY machine. */
int macsurf_ptr_is_heap(const void *p)
{
	unsigned long a = (unsigned long)p;
	return (a >= g_ptr_lo) && (a < g_ptr_hi);
}

/* FLOOR-ONLY test — for pointers that may legitimately live OUTSIDE the
 * partition heap window (e.g. a static const vtable or PEF read-only
 * literal in the code fragment). Only rejects NULL/tiny scribble. */
int macsurf_ptr_is_valid(const void *p)
{
	return (unsigned long)p >= 0x00001000UL;
}
