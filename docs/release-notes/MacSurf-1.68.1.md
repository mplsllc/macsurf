# MacSurf 1.68.1 — Stability Patch

*A native web browser for Classic Mac OS (PowerPC, Mac OS 9.1–9.2.2). This is a critical stability release patching low-memory vulnerabilities and heap fragmentation on real hardware.*

**Verified on:** Power Macintosh G3 iMac and G4, Mac OS 9.2.2.

---

## Critical Fixes in 1.68.1

This patch release addresses a critical issue where the browser would crash and freeze the entire OS when running low on contiguous memory.

* **Low-Memory Crash Protection (Heap Fragmentation Mitigation — #207):** 
  On machines with limited RAM (e.g., 256MB G3/G4s), thousands of tiny allocations from NetSurf and QuickJS fragment the heap, causing allocations to fail even when total free memory is high. Instead of returning `NULL` and letting the browser write to `$0000` (which halts the Classic Mac OS instantly due to lacking memory protection), a new bulletproof allocator intercepts all allocations:
  * **Safe Allocator Wrappers:** `macsurf_safe_alloc`, `macsurf_safe_calloc`, and `macsurf_safe_realloc` are guaranteed to never return `NULL` to the engines.
  * **Clean Panic & Diagnostics:** On allocation failure, they capture the exact `MaxBlock()` and `FreeMem()` readings, write a fatal log entry to `MacSurf Debug.log` (flushed immediately), post a native Carbon `StandardAlert` explaining the heap fragmentation state, and terminate cleanly via `ExitToShell()` to avoid system corruption.
  * **QuickJS Allocator Hook:** QuickJS runtime initialization is updated to route allocations through these safe wrappers, preventing internal QuickJS OOM writes to `$0000`.
  * **Disk Cache Recovery:** The cached body lookup gracefully falls back to a network fetch under memory starvation rather than panicking.
  * **Standard Library Parsing Order Safeguard:** Allocator macro overrides in the prefix header are ordered to avoid compiler errors with Metrowerks Standard Library.

* **Version Visibility & Reporting:**
  * **About Box:** Updated the Apple menu's "About MacSurf..." dialog to display **MacSurf 1.68.1**.
  * **User-Agent Reporting:** Synchronized request headers to send `MacSurf/1.68.1`.
  * **JavaScript Environment:** Updated the engine's `navigator.userAgent` to report `MacSurf/1.68.1` to on-page scripts.

---

## Community Credits

Special thanks to **@glossolallia**, **@CaptainPanic29**, and **@Azryael** for their detailed bug reports, crash logs, and hardware verification work on G3 and G4 systems that made isolating this issue possible.

---

## Downloading & Support

Grab the latest build from the plain-HTTP **[macsurf.org](http://macsurf.org/)** directly from your vintage Mac.

If MacSurf put your old Mac back online, please consider supporting the project:
* **Patreon:** https://www.patreon.com/cw/MacSurf
* **Ko-Fi:** https://ko-fi.com/macsurf
