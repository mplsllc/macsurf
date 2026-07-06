# MacSurf 1.86 — (in progress)

**Status:** unreleased draft — accumulates notes for the 1.86 release.
**Version note:** the 1.86 number honors 68kmla.org, the focus of the majority of this cycle's work.
**Engine HEAD:** fixes646+ (MacSurf side)
**Verified on:** Power Macintosh G3 iMac, Mac OS 9.2.2

---

## What's new (chrome / usability round)

- **Bookmarks are now a real menu.** Saved bookmarks appear as clickable items under the Bookmarks menu (page title, URL fallback); selecting one navigates the front window there. Bookmarks persist to a `MacSurf Bookmarks` file next to the app, so they survive relaunch. (Replaces the old "Show Bookmarks" alert dump.)
- **Downloads work over HTTPS and have a manager.** Downloads auto-save to a `MacSurf Downloads` folder next to the app (server-suggested filename, sanitised for HFS + de-duplicated), and a modeless **Downloads** window lists each transfer with live byte progress and Done/Stopped status. Active downloads have a per-row **Cancel** button.
  - Root cause of the previous "HTTPS downloads do nothing": the modal save dialog (`NavPutFile`) returned `kNavInvalidSystemConfigErr` (-5699) when invoked from inside the fetch callback. Removing the modal dialog (auto-save instead) fixed it and also allows several concurrent downloads.
- **Window maximize (zoom box) works.** Clicking the zoom box (top-right of the title bar) fills the screen below the menu bar; clicking again restores the previous size and position exactly.
- **One `MacSurfData` folder.** Everything MacSurf writes now nests under a single `MacSurfData` folder next to the app — `Cache/` and `Downloads/` subfolders, plus the bookmarks and debug-log files at its root — instead of a scatter of `MacSurf *` folders on the boot Desktop. Bookmarks live *outside* `Cache/`, so clearing the cache can't delete them. (One-time note: bookmarks/downloads from the earlier scattered layout aren't migrated; re-add bookmarks and delete any leftover old `MacSurf Cache` / `MacSurf Downloads` folders.)
- **Logins are retained.** A header-parser bug that dropped the final `Set-Cookie` on a login 302 is fixed, so site logins now persist. The cookie jar also nests inside `MacSurfData` now.

## What's new (site compatibility)

- **68kmla / XenForo reply + post editor works again.** The rich post/reply editor had collapsed to a bare one-line box because the QuickJS migration (fixes522) ran XenForo's real `preamble`/`core`/`editor` bundles natively — and they crash on this engine (a `parentNode`-null → jQuery Sizzle → `jQuery.support` → `XF.Element.newHandler` cascade), so `has-js` was never set and the editor never revealed. Restored the ES5 stub substitution (the fixes476–481 mechanism) that swaps in small shims for those three bundles, setting `has-js`, defining `XF.Element`, and revealing + sizing the editor. This was the mechanism that first let real forum replies post from Mac OS 9.

## What's new (performance)

- **Images no longer re-process on every repaint.** `plot_bitmap` used to rebuild a 32-bit GWorld (full RGBA→XRGB byte-swap, plus a box-filter downscale for shrunk images) on *every* paint — a measured ~2.5 s per paint on a big image, and the cause of scroll jank. The prepared, ready-to-blit GWorld is now cached on the bitmap (bounded 8 MB budget; oversize images still stream through the transient path), so repaints and scrolls skip straight to the blit. (Also fixes the decoded-image LRU use-after-free, #168.)
- **Redraw skips a per-box DOM lookup.** The element tag-type resolve that ran for every box on every frame is deferred to the one branch that needs it (canvas).

## Known issues

- **Window maximize under QEMU is cosmetically wrong (emulator artifact — not chasing).** On the real G3 / Mac OS 9 the maximize/restore works perfectly. Under **QEMU** the window is mispositioned / oversized on zoom because emulated screen-bounds / window-region geometry differs from real hardware. This is an emulator discrepancy, not a MacSurf bug; it is intentionally not being fixed. (See issue #188.)
