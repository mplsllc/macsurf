# MacSurf 1.68.3 — The Blank Page

*A native web browser for Classic Mac OS (PowerPC, Mac OS 9.1–9.2.2). This release fixes the blank page on launch — the one that only a cache-clear seemed to cure.*

**Verified on:** Power Macintosh G3 iMac, Mac OS 9.2.2.

---

## The blank page is fixed

If MacSurf opened to a blank window, and clearing the cache fixed it for a while before it came back — this release fixes that, and no cache-clearing is needed. **1.68.3 repairs an affected machine automatically on first launch.**

**The reason.** MacSurf keeps a list of "dead hosts" so that an unreachable server can't stall a page with dozens of doomed requests. That list was being saved to disk, inside the Cache folder, and reloaded every time you launched — with no expiry.

So a single bad moment on your network — one timed-out connection, one dropped handshake — was enough to write a server to that file **permanently**. Every launch after that refused to even try contacting it. If the server that got condemned happened to be your home page, MacSurf opened to a blank window, forever.

Deleting the cache erased the file, which is why it appeared to help. But the very next network hiccup wrote it again, which is why the fix never stuck.

In 1.68.3 the dead-host list lives only for the current session. A server can no longer be condemned by one bad moment, the list never survives a relaunch, and typing a URL or clicking a bookmark always means "try this now." On first run, 1.68.3 also deletes any poisoned list an older version left behind.

---

## About the Virtual Memory and cache workarounds

Both are now explained, and neither was ever a real fix:

* **Clearing the cache** deleted the bad list. That genuinely worked — until the next network hiccup recreated it.
* **Virtual Memory made no difference at all.** We measured it directly on real hardware: with VM on and with VM off, the application heap reported identical free and largest-block figures, unfragmented in both cases. Nothing MacSurf can see changes when you toggle it. If it seemed to help, that was the restart.

The `$0000` memory bug fixed in 1.68.1 and 1.68.2 was real and remains fixed. It simply wasn't the cause of the blank page.

---

## Also in 1.68.3

* **History manager (#47).** A real, persistent, clearable history — day-grouped and scrollable (History → Show All History, ⌘H).
* **Bookmark manager (#221).** Folders, rename, delete, drag-and-drop reordering, and a folder picker for moves (Bookmarks → Manage Bookmarks, ⌘B). The Bookmarks menu now nests folders as submenus.
* **Clear Cache** menu item (History menu).
* **Image-heavy pages load faster (#208).** Avatar-rich forum threads were re-laying-out the entire page for *every* image that finished loading. Reflows on a typical forum page dropped from about 15 to 2–4.
* **JavaScript is more responsive (#209, #210).** Less event-loop overhead during script execution, and `querySelector` now stops at the first match instead of walking the whole document.
* **Crash fix** for a bad entry in the image cache on picture-heavy pages.
* **Better bug reports.** `MacSurf Debug.log` now carries a per-session header with the version, build stamp and launch time, and flushes its first lines immediately so a failure during startup still leaves evidence behind.
* **Version reporting.** About box, `User-Agent`, `navigator.userAgent`, and the debug-log banner all report **MacSurf/1.68.3**.

---

## Known issues

* **macintoshgarden.org loads over http, not https (#206).** This is a real TLS handshake failure in MacSurf's own stack against that server's configuration, not a server-side redirect. Diagnostics landed in this release; the fix is still open.
* Genuine memory exhaustion still ends in a clean alert and a graceful quit, with the exact figures written to `MacSurf Debug.log`. That is by design.

**If a blank page still happens on 1.68.3**, please reopen [#207](https://github.com/mplsllc/macsurf/issues/207) and attach `MacSurf Debug.log` — that would mean a second cause we haven't seen yet, and the log will show it.

---

## Community Credits

Thanks to **@glossolallia**, **@CaptainPanic29**, and **@Azryael** for the crash logs and hardware testing on G3 and G4 systems, and for sticking with a blank-page report that took a long time to pin down.
