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
- **App-relative files.** Cache, debug log, downloads, and bookmarks now live next to the app instead of the boot-volume Desktop.
- **Logins are retained.** A header-parser bug that dropped the final `Set-Cookie` on a login 302 is fixed, so site logins now persist.

## Known issues

- **Window maximize under QEMU is cosmetically wrong (emulator artifact — not chasing).** On the real G3 / Mac OS 9 the maximize/restore works perfectly. Under **QEMU** the window is mispositioned / oversized on zoom because emulated screen-bounds / window-region geometry differs from real hardware. This is an emulator discrepancy, not a MacSurf bug; it is intentionally not being fixed. (See issue #188.)
