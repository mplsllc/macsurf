<p align="center">
  <img src="img/bannerlogo.png" alt="MacSurf" width="460">
</p>

<p>
  <strong>The modern web, on a 25-year-old Mac.</strong>
</p>

<p>
  MacSurf is a web browser for Classic Mac OS 9 PowerPC. CSS3, ES5 JavaScript, and PNGs with alpha — running on a G3 iMac.
</p>
<em>MacSurf is a full-time project. If it brought your old Mac back to the web, every dollar helps keep development going. Also follow the progress on Patreon with daily dev logs.</em><br/><br/>

<div align="center">
  <a href="https://ko-fi.com/macsurf" target="_blank">
    <img src="https://github.com/mplsllc/macsurf/blob/master/support_me_on_kofi_badge_blue.png" alt="Follow me on Ko-Fi!" height="60" max-width="150px">
  </a>
<a href="https://www.patreon.com/cw/MacSurf" target="_blank"><img src="https://github.com/mplsllc/macsurf/blob/master/patreon-banner-519x200.png" width="150" /></a>
</p>
<p align="center">
  <img src="img/mactls.png" alt="Mac OS 9, meet TLS — Native, no proxy needed." width="500">
</p></div>

<p align="center"><h3>Intro Video</h3></p>

[![Watch the video](https://img.youtube.com/vi/PLpbHSXca60/hqdefault.jpg)](https://www.youtube.com/watch?v=PLpbHSXca60)


> [!NOTE]
> Early alpha — it runs, renders, and speaks TLS 1.3 natively on a 233 MHz G3, but most of the modern web won't work in it yet. Expect hand-built pages, retro-style sites, and the strange thrill of ES5 JavaScript on PowerPC. Don't expect smooth browsing on arbitrary modern sites, video, or anything React-heavy.
>
> If you've got a Power Mac G3 or G4 sitting around, load it up and see what breaks — bug reports and real-hardware screenshots are exactly what this project needs. See [docs/status.md](docs/status.md) for the current punch list.

---

## Why this exists

The web outgrew Classic Mac OS twenty years ago. Modern HTTPS finished it off around 2016. Pull a G3 or G4 out of the closet today and it can barely reach a single live website.

MacSurf is an attempt to fix that on the machine itself — no screenshot proxy, no remote terminal trick. A native browser, built with the tools that were already on the platform: CodeWarrior, Carbon, QuickDraw, Open Transport. Real CSS3 layouts and real JavaScript, running inside the 64 MB memory floor of a 1999 iMac. Since late May 2026 it speaks TLS 1.2 directly to the modern web through [macTLS](https://github.com/mplsllc/macTLS), a BearSSL-based stack that ships inside the browser binary with 121 trust anchors from the Mozilla CA bundle. No proxy needed anymore.

As far as we can tell, this is the first serious [NetSurf](https://www.netsurf-browser.org/) port to Classic Mac OS, and the first browser ever shipped on Mac OS 9 with native CSS Grid, CSS custom properties, and ES5 JavaScript.

---

## The progression

Each shot below is a real milestone, captured on a Power Macintosh G3 running Mac OS 9. The fix-number annotations match this repo's commit history.

<table>
<tr>
<td width="50%" align="center" valign="top">
  <img src="screenshots/01-javascript-on-os9.jpg" alt="JavaScript Hello World on Mac OS 9"><br>
  <strong>v0.2: JavaScript on Mac OS 9</strong><br>
  <em>First real-world JS-bearing page. Duktape 2.7.0 ES5 evaluating live: <code>Math.sqrt</code>, JSON, ES5 array methods.</em>
</td>
<td width="50%" align="center" valign="top">
  <img src="screenshots/02-css-transforms.jpg" alt="CSS transform rotate, scale, translate"><br>
  <strong>fixes73: CSS transforms</strong><br>
  <em>Native <code>transform: rotate() / scale() / translate()</code>. Integer Q15 sin/cos table, no FPU dependency, arbitrary angles on QuickDraw.</em>
</td>
</tr>
<tr>
<td width="50%" align="center" valign="top">
  <img src="screenshots/03-css-radial-gradients.jpg" alt="CSS radial gradients"><br>
  <strong>fixes74d: radial gradients</strong><br>
  <em>2-stop radial gradients via concentric <code>PaintOval</code> stack. 16 levels smeared on decode. Shape + position keywords parsed.</em>
</td>
<td width="50%" align="center" valign="top">
  <img src="screenshots/04-css-animations.jpg" alt="CSS animations: wiggle, swing, slow spin"><br>
  <strong>fixes77: CSS animations</strong><br>
  <em>Linear ping-pong animation player on top of fixes73 rotation. Wiggle, swing, and full 0&deg;&rarr;359&deg; spin.</em>
</td>
</tr>
<tr>
<td width="50%" align="center" valign="top">
  <img src="screenshots/05-png-transparency.jpg" alt="PNG image with transparency on Mac OS 9"><br>
  <strong>fixes79b: PNG transparency</strong><br>
  <em>QuickTime Graphics Importer feeding the NetSurf image content handler. PNG + GIF + BMP, all with real transparency.</em>
</td>
<td width="50%" align="center" valign="top">
  <img src="screenshots/06-css-word-overflow.jpg" alt="CSS word-break and overflow-wrap"><br>
  <strong>fixes136: word-break / overflow-wrap</strong><br>
  <em><code>word-break: break-all</code>, <code>keep-all</code>, <code>white-space: nowrap</code>, legacy <code>word-wrap: break-word</code>. URL-style aggressive wrapping.</em>
</td>
</tr>
<tr>
<td width="50%" align="center" valign="top">
  <img src="screenshots/07-css-stacking-contexts.jpg" alt="CSS z-index stacking contexts"><br>
  <strong>fixes147: stacking contexts</strong><br>
  <em>CSS 2.1 painting order. Opacity, transforms, and explicit <code>z-index</code> all create new stacking contexts, properly painted on real hardware.</em>
</td>
<td width="50%" align="center" valign="top">
  <img src="screenshots/08-css-grid-placement.jpg" alt="CSS Grid column placement"><br>
  <strong>fixes151: CSS Grid column placement</strong><br>
  <em><code>grid-column: span N</code>, <code>1 / -1</code> full-row hero, positional <code>start / end</code>, span + auto-wrap. Real Grid layout on OS 9.</em>
</td>
</tr>
<tr>
<td width="100%" colspan="2" align="center" valign="top">
  <img src="screenshots/macsurf-1.0-home.jpg" alt="MacSurf 1.0 rendering home.macsurf.org"><br>
  <strong>v1.0: Showcase</strong><br>
  <em>The new tool-belt toolbar, razor-sharp URL field, and matted icons rendering <a href="https://home.macsurf.org/">home.macsurf.org</a> on a G3 iMac running OS 9.2.2. Native HTTPS via macTLS direct to the origin, server-rendered portal, true-colour images end to end.</em>
</td>
</tr>
</table>

---

## The pieces

<table>
<tr><th align="left">Component</th><th align="left">Language</th><th align="left">Purpose</th></tr>
<tr>
<td><a href="browser/"><code>browser/</code></a></td>
<td>C (C89, CW8)</td>
<td>NetSurf fork with a <code>macos9</code> frontend. Carbon for the UI, QuickDraw for drawing, Open Transport for networking, Duktape for JS.</td>
</tr>
<tr>
<td><a href="proxy/"><code>proxy/</code></a></td>
<td>Go (stdlib only)</td>
<td>The old TLS-stripping HTTP proxy. Largely retired now that macTLS works natively, but still useful as a fallback or on machines without CarbonLib. Mac sends plain HTTP, proxy fetches via HTTPS, returns plain HTTP.</td>
</tr>
<tr>
<td><code>macTLS</code><br><sub>sibling repo</sub></td>
<td>C (CW8)</td>
<td>Native TLS 1.2 library for OS 9 — modern HTTPS straight from the Mac, no proxy required. BearSSL underneath, 121 trust anchors baked in.</td>
</tr>
</table>

---

## What works today

<table>
<tr>
<td valign="top" width="50%">

**Rendering pipeline**
- Full NetSurf fetch → parse → cascade → layout → plot
- Native libcss with `var()` resolution
- QuickDraw plotters with an offscreen GWorld back-buffer

**CSS** — around 150 properties consumed in layout
- Custom properties and `var()`
- Flex: `justify-content`, `align-content`, `order`
- Grid V1 plus `grid-template-columns/rows`, `gap`
- `border-radius`, `box-shadow`, opacity
- Linear and radial gradients
- `text-shadow`, `text-overflow: ellipsis`
- `transform` (rotate, translate, scale)
- z-index stacking contexts (CSS 2.1 painting order)
- CSS counters, viewport units, `aspect-ratio`
- Font-family aliases for sans, serif, monospace

[Full CSS status &rarr;](docs/css-status.md)

</td>
<td valign="top" width="50%">

**JavaScript** — Duktape 2.7.0, full ES5
- Closures, prototypes, regex, JSON
- Promises (polyfill), recursion, Mandelbrot
- About 6 seconds for `ackermann(3,7)` on a 233 MHz G3

**Images** — all five formats
- PNG with real per-pixel alpha (lodepng + `CopyMask`)
- GIF with palette transparency
- JPEG, BMP, TIFF

**Networking**
- Open Transport TCP, plain non-`InContext` calls
- HTTP/1.1 + chunked + keep-alive + 3xx follow
- Connection pooling, 15-second no-progress timeout
- HTTPS via macTLS (default) or the Go proxy (fallback)

**Browser chrome**
- Address bar, back / forward / reload / home
- Status bar, page-info, multi-window
- Smooth scroll bar, keyboard scrolling

</td>
</tr>
</table>

---

## Download

Latest is **[MacSurf v1.4 — Open House](https://github.com/mplsllc/macsurf/releases/tag/v1.4)** (2026-06-01): **JavaScript marathon closed.** 23 JS-bridge issues went from open to closed in one release — `setTimeout` / `setInterval` / `requestAnimationFrame`, `window.location`, `window.history`, `URL`, `URLSearchParams`, `classList`, `style`, `Event` constructors, `MutationObserver`, `DOMParser`, `FormData`, `localStorage`, `fetch`, `load` / `DOMContentLoaded` events, plus `<details>` / `<summary>` toggle. The new probe suite scored `JS 19/19 pass, 0 fail` on a G3 iMac. Diagnostic pages live: `about:cache`, `about:memory`, `about:config`, `about:perf` all render real counters. View Source renders inline, Find-in-page opens a real Carbon dialog. Predecessor [v1.3.1 "Forward, refined"](https://github.com/mplsllc/macsurf/releases/tag/v1.3.1) shipped multi-curve TLS 1.3; [v1.3 "Forward"](https://github.com/mplsllc/macsurf/releases/tag/v1.3) was the first native TLS 1.3 on Classic Mac OS; [v1.2 "Sealed"](https://github.com/mplsllc/macsurf/releases/tag/v1.2) closed the entropy hole; [v1.0 "Showcase"](https://github.com/mplsllc/macsurf/releases/tag/v1.0) was the chrome-redesign release; [v0.6.2 "Speed-Run"](https://github.com/mplsllc/macsurf/releases/tag/v0.6.2) was the cold-load speedup (mactrove.com 30+s → ~2-3s); first numbered alpha at [v0.1a1](https://github.com/mplsllc/macsurf/releases/tag/v0.1a1).

- **[MacSurf.sit](https://github.com/mplsllc/macsurf/releases/download/v1.4/MacSurf.sit)** — the v1.4 binary, ready to run. Expand on Mac OS 9.1+ with CarbonLib 1.5+ and launch.
- Building from source: clone the repo, then on the Mac side open `browser/netsurf/frontends/macos9/MacSurf.mcp` in CodeWarrior 8 and choose Build. v1.4 builders on a v1.3.1 workspace need to add **one** new source file to the project: `browser/netsurf/desktop/search.c` (provides `browser_window_search` for the new Find dialog; `content/textsearch.c` is already in). macTLS is unchanged from v1.3.1. v1.3 builders on a v1.2 workspace need to add four macTLS files for TLS 1.3: `bearssl/src/ec/ec_c25519_m15.c`, `os9/ostls_tls13_keysched.c`, `os9/ostls_tls13_record.c`, `os9/ostls_tls13_handshake.c`. v1.2 builders on a 1.0 workspace need `desktop/download.c`. The earliest release ships a [BuildPack.sit](https://github.com/mplsllc/macsurf/releases/download/v0.1a1/MacSurf-BuildPack.sit) snapshot with the CW8 project pre-wired, but current builds work straight from a fresh clone.

Earlier alpha notes if you want context: [docs/release-notes/MacSurf-0.1a1.md](docs/release-notes/MacSurf-0.1a1.md).

---

## Getting started

<table>
<tr>
<td valign="top" width="50%">

### Building the browser
MacSurf is built on Mac OS 9 with CodeWarrior 8 Pro (8.3 update). The source is cross-compile-clean against Retro68 PowerPC GCC, which is what we use for fast Linux-side syntax checks.

- [Mac-side build guide](docs/codewarrior-setup.md)
- [Linux cross-dev workflow](docs/cross-dev-from-linux.md)

</td>
