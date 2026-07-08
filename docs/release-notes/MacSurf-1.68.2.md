# MacSurf 1.68.2 — Stability Patch

*A native web browser for Classic Mac OS (PowerPC, Mac OS 9.1–9.2.2). A small, focused follow-up to 1.68.1 that closes two ways the browser could leave you staring at a blank or frozen window on real hardware.*

**Verified on:** Power Macintosh G3 iMac and G4, Mac OS 9.2.2.

---

## Fixes in 1.68.2

* **Blank screen / silent crash on low-contiguous-memory machines (#207, follow-up).**
  1.68.1 introduced the bulletproof allocator but only routed **QuickJS and the NetSurf core** through it. The HTML/CSS layout libraries (libdom, libcss, libwapcaplet) were still calling the standard library directly, so under heap fragmentation one of them could take a failed allocation, store a `NULL` node into the DOM tree, and later write through it to address `$0000` — which, with no memory protection (especially **Virtual Memory off**), silently corrupts the system and leaves a blank, dead window instead of a clean error. 1.68.2 wires **all three layout libraries** through `macsurf_safe_alloc` as well, so no engine on the page path can ever receive a `NULL` allocation. The `$0000` write is eliminated at its source rather than caught after the fact. Reporters who could reliably reproduce the blank screen (Virtual Memory off / tight RAM) no longer can.

* **Frozen or blank window while loading image-heavy pages (e.g. avatar-rich forum threads).**
  When the browser reused a pooled keep-alive HTTPS connection for the next request on a busy page, a full TCP send window could block the cooperative thread indefinitely, hanging the whole UI at a half-drawn or blank window. The TLS connection layer is now fully non-blocking: it detects the flow-control condition, yields to the rest of the system, and resumes the send the moment the network buffer drains — so heavy forum pages keep loading instead of locking up.

* **Version reporting.** About box, request `User-Agent`, and the JavaScript `navigator.userAgent` all report **MacSurf/1.68.2**.

---

## Notes

* **Genuine memory exhaustion** (a heap so fragmented that even compaction can't satisfy a request) now ends in a clean alert and a graceful quit — with the exact `MaxBlock`/`FreeMem` figures written to `MacSurf Debug.log` — instead of a crash. This is by design.
* No changes to the JavaScript engine, rendering, or feature set since 1.68.1. This is strictly a stability patch.

---

## Community Credits

Thanks again to **@glossolallia**, **@CaptainPanic29**, and **@Azryael** for the crash logs and hardware testing on G3 and G4 systems that pinned this down.

---

## Downloading & Support

Grab the latest build over plain HTTP from **[macsurf.org](http://macsurf.org/)**, directly from your vintage Mac.

If MacSurf put your old Mac back online, please consider supporting the project:
* **Patreon:** https://www.patreon.com/cw/MacSurf
* **Ko-Fi:** https://ko-fi.com/macsurf
