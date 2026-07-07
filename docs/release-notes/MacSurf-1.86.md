# MacSurf 1.86 — "macQJS"

*A native web browser for Classic Mac OS (PowerPC, Mac OS 9.1–9.2.2), with real HTTPS and a modern JavaScript engine — no proxy, no companion machine.*

**Verified on:** Power Macintosh G3 iMac and G4, Mac OS 9.2.2.
The **1.86** number honors **68kmla.org** — the community and the site that drove the majority of this cycle's work.

---

## Help keep MacSurf full-time

MacSurf is a full-time project. Bringing the modern, HTTPS-only web back to real Mac OS 9 hardware — native TLS 1.3, a real JavaScript engine, the whole NetSurf rendering pipeline cross-compiled for PowerPC — is a lot of work, and this pace only continues if the project can pay for itself.

If MacSurf put your old Mac back online, please consider chipping in. Every bit genuinely helps, and supporters get the dev logs and a say in what gets built next:

- **Patreon:** https://www.patreon.com/cw/MacSurf
- **Ko-Fi:** https://ko-fi.com/macsurf

No pressure — but if I don't ask, I can't keep doing this, so I'm asking. Thank you to everyone who already has.

---

## For 68kmla.org — thank you

This release is named **1.86** in honor of **[68kmla.org](https://68kmla.org)**, the 68k/PowerPC Mac community that has been the heart of this cycle's work.

An enormous amount of 1.86 exists because 68kmla exists: it's the site I test against every single day, it's where the toughest real-world rendering, login, and JavaScript problems surfaced, and it's the reason so many of the fixes below are as thorough as they are. More than that, the 68kmla community has been genuinely kind and welcoming to a new, rough-around-the-edges project — patient with the bugs, generous with detailed feedback, and encouraging at every step. That reception is a big part of why MacSurf kept moving forward. So: **thank you, 68kmla.** Getting your forums to render, log in, and let you post a reply from a real Mac OS 9 machine has been one of the most rewarding targets to build toward.

---

## The headline: MacSurf now runs a real JavaScript engine — macQJS

![macQJS](https://raw.githubusercontent.com/mplsllc/macQJS/master/macqjs-banner.png)

The single biggest change since 1.5 is under the hood: **MacSurf's JavaScript engine has been replaced. Duktape (ES5) is gone; MacSurf now runs [macQJS](https://github.com/mplsllc/macQJS) — a QuickJS port for Classic Mac OS — which executes modern ES2023 JavaScript natively on a PowerPC running Mac OS 9.**

Why this matters:

- **Modern JS runs as-is.** The old engine was ES5-only, so MacSurf carried an in-house ES6→ES5 transpiler to pre-chew modern bundles before running them. That transpiler is **retired** — its async/await rewriting was silently corrupting minified bundles. macQJS runs `let`/`const`, arrow functions, classes, template literals, Promises, generators, modern regex, etc. directly.
- **Real sites' real scripts.** Pages no longer need their JavaScript dumbed down to run; the actual site bundles execute on-device.
- **Guarded.** A per-heap memory cap and an execution deadline keep a runaway or hostile script from hanging or OOMing the Mac.

macQJS is developed as its own project so other Classic Mac OS software can use it too. This was a multi-month migration (fixes482–554) plus a long hardware-verified crash-stability arc (fixes565–577) to make the new engine solid on real hardware.

---

## Showstoppers fixed this cycle

These are the "how was this ever shipped" basics that are now solid:

- **You can see and place a text cursor.** Input fields and textareas now draw a real blinking caret, so you can tell where your typing will land.
- **You can select text in fields.** Click-and-drag selection works; Cut/Copy/Paste route to the focused field (the clipboard is wired to the Mac Scrap Manager).
- **Logins persist.** Signing in to a site now keeps you signed in — the browser was serving a stale, cached, logged-out copy of the page right after login. Fixed.
- **Arrow keys behave.** They move the caret in fields and scroll the page — and no longer shove the whole viewport sideways.
- **Tab moves between form fields.** Fill out a login or search form with the keyboard.
- **The browser opens almost instantly.** A diagnostic self-test was running a heavy JavaScript benchmark battery at every launch — roughly a **17-second freeze** before the first page. It's gone; startup is now well under a second.
- **Cookie-consent / sticky bars are clickable.** The "Accept" button on sticky overlays (68kmla's cookie banner) now clicks where you'd expect, instead of falling through to the page behind it.
- **Second window no longer closes the first.** Window lifecycle is fixed.
- **Maximize (zoom box) works**, and **"About MacSurf"** opens.

---

## Everything new in 1.86, by area

### JavaScript engine (macQJS)

- Replaced Duktape (ES5) with **macQJS / QuickJS (ES2023)**, linked into the build and gated by `WITH_QUICKJS`.
- Retired the in-house ES6→ES5 transpiler and the old hand-built JS API surface (they papered over engine gaps and corrupted some bundles).
- Per-navigation fresh JS realm so a page's top-level `let`/`const`/`class` declarations don't collide with the previous page's on the next load.
- Memory limit + ~20 s execution deadline guard against runaway scripts.

### Site compatibility (68kmla / XenForo)

- **The reply/post editor works again.** The engine switch meant XenForo's real `preamble`/`core`/`editor` bundles ran natively and crashed (a `parentNode`-null → jQuery Sizzle → `jQuery.support` → `XF.Element` cascade), collapsing the editor to a bare one-line box. Small ES5 shims are substituted for those three bundles to set `has-js`, define `XF.Element`, and reveal + size the editor — the same mechanism that first let a real forum reply post from Mac OS 9.
- Doomed XenForo feature bundles (`lightbox`, media gallery, etc.) that only throw on this engine are now skipped rather than parsed-and-thrown, reclaiming page-load time.

### Text input & keyboard

- Blinking text caret in `<input>` / `<textarea>` (native `place_caret`), including password fields.
- Mouse click-drag text selection in fields.
- Cut / Copy / Paste to the focused field via the Edit menu and Cmd-keys (Scrap Manager clipboard).
- Tab / Shift-Tab cycle text fields in document order (works with or without a `<form>`).
- Arrow keys, Home/End, Page Up/Down, backspace and forward-delete route to the focused field; when nothing is focused they scroll the page. The old "arrows shove the viewport" bug is fixed.

### Chrome & usability

- **Bookmarks menu.** Saved bookmarks appear as clickable menu items and persist across relaunch.
- **Downloads manager.** HTTPS downloads now work and auto-save to a `Downloads` folder (server-suggested, HFS-sanitised, de-duplicated filename); a modeless Downloads window shows live byte progress with per-row Cancel. (The old "downloads do nothing" was a modal save dialog returning `-5699` from inside the fetch callback.)
- **Window maximize (zoom box)** fills the screen and restores exactly.
- **One `MacSurfData` folder** next to the app holds everything MacSurf writes — `Cache/` and `Downloads/` subfolders, plus bookmarks and the debug log at its root — instead of scattering `MacSurf *` folders on the boot Desktop. Bookmarks live outside `Cache/`, so clearing the cache can't delete them. (This also fixes the old bug where the cache and log were hardcoded to the boot volume's Desktop rather than next to the app.)
- **"About MacSurf"** Apple-menu item works.

### Networking & logins

- Login sessions persist (the post-login redirect no longer serves a stale logged-out page from cache).
- Cookie jar persists to disk inside `MacSurfData` so a login survives relaunch.
- Per-host User-Agent overrides and the RFC-6265 cookie jar remain wired for real logins.

### Rendering & CSS

- **Split-scrollbar / runaway page-width bug fixed** (the single biggest render fix this cycle): an unresolved box kept its "born" width of `INT_MAX`, blowing the document content width up to ~2.1 billion px and producing a narrow content column beside a giant empty canvas with a spurious horizontal scrollbar. XenForo forums (68kmla, tinkerdifferent) now lay out full-width.
- **Downloadable web-font icon glyphs render** — Material Design Icons / FontAwesome `@font-face` icon fonts now paint on Classic Mac OS (a first, as far as we know), via sfnt cmap parsing + QuickDraw region fill.
- **Text word-merging / awkward wrap fixed** — inter-word spaces no longer collapse ("softwareand"); font measurement and painting now advance identically.
- **SVG colour regression fixed** (inline SVGs were being blended away by the alpha-compositing path).
- Sticky-overlay hit-testing (see Showstoppers) and a corrected white page-base (light sites no longer show a grey base behind un-styled regions).
- CSS Grid track limit raised 8 → 16 columns (from 1.5) so modern 12-column grids stop collapsing.

### Performance

- **Instant startup** — removed a ~17 s diagnostic JS self-test battery that ran synchronously at every launch before the event loop.
- **Image & web-font disk cache** — images and downloadable fonts are now cached to disk (bounded 64 MB budget with LRU eviction), so revisiting an image-heavy page — or reloading after Mac OS evicts the RAM cache — no longer re-downloads everything.
- **Prepared-image GWorld cache** — `plot_bitmap` no longer rebuilds a 32-bit GWorld (RGBA→XRGB swap + downscale) on every paint (~2.5 s/paint on a big image); the ready-to-blit buffer is cached, killing scroll jank.
- **Faster box construction** — the HTML→box conversion yielded to the event loop every 10 nodes; it now processes 100, cutting scheduler round-trips ~10× on large pages.
- **Deferred per-box DOM lookups** in redraw, and a large log-volume reduction (per-element diagnostics suppressed by default).

### Stability

- A long arc of use-after-free and crash-stability fixes to make the new engine solid on real hardware (box-walk lifetime guards, content-registry liveness checks, the decoded-image LRU UAF).

---

## Known issues

- **GitHub and some heavy HTTPS sites don't render yet.** MacSurf's TLS connects to GitHub fine; the failure is a URL-parse quirk after a redirect, and beyond that GitHub is a very heavy modern single-page app. Getting it to render is a large, targeted effort deferred to a future cycle. **To download the latest MacSurf release from a Mac OS machine, use the plain non-SSL (http) version of macsurf.org** — no VM or GitHub required.
- **Tab doesn't reach links or buttons yet** — only text fields. Keyboard focus for links needs an engine focus model that isn't in place; it's tracked for the next cycle.
- **Logged-in XenForo account nav can render with a grey background** — a progressive-enhancement (`.has-js`) styling detail that applies too late; cosmetic, tracked.
- **Clicks can fall through `position: fixed` / `absolute` overlays** — the `sticky` case (cookie bars) is fixed; general stacking-aware hit-testing is tracked.
- **Window maximize under QEMU is cosmetically wrong** — an emulator geometry artifact only; maximize/restore works correctly on real G3/G4 hardware. Not a MacSurf bug; intentionally not chased.

---

## Downloading & updating

Grab the latest build from **macsurf.org** — and note there's a **plain non-SSL (http) version of the site** specifically so you can download new releases from a Mac OS machine even while heavy HTTPS sites like GitHub aren't yet supported.

## Support

If this is useful to you, please help keep it going:

- **Patreon:** https://www.patreon.com/cw/MacSurf
- **Ko-Fi:** https://ko-fi.com/macsurf
