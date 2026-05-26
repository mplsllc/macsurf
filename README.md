<p align="center">
  <img src="img/bannerlogo.png" alt="MacSurf" width="460">
</p>

<p>
  <strong>The modern web, on a 25-year-old Mac.</strong>
</p>

<p>
  MacSurf is a web browser for Classic Mac OS 9 PowerPC.<br>
  Real CSS3. Real ES5 JavaScript. Real PNGs with alpha. Running on a beige G3.
</p>

<div align="center">
  <a href="https://www.buymeacoffee.com/Ptricky">
    <img src="https://cdn.buymeacoffee.com/buttons/v2/default-blue.png" alt="Buy Me A Coffee" height="48" width="217">
  </a>
</div>
<p align="center">
  <a href="https://github.com/mplsllc/macsurf/releases"><img alt="Latest release" src="https://img.shields.io/github/v/release/mplsllc/macsurf?include_prereleases&label=release&color=ff8800"></a>
  <a href="./LICENSE"><img alt="License" src="https://img.shields.io/github/license/mplsllc/macsurf?color=blue"></a>
  <a href="https://github.com/mplsllc/macsurf/issues"><img alt="Open issues" src="https://img.shields.io/github/issues/mplsllc/macsurf"></a>
  <a href="https://github.com/mplsllc/macsurf/stargazers"><img alt="Stars" src="https://img.shields.io/github/stars/mplsllc/macsurf?style=flat"></a>
  <a href="https://en.wikipedia.org/wiki/Mac_OS_9"><img alt="Mac OS 9" src="https://img.shields.io/badge/Mac%20OS-9.1%E2%80%939.2.2-silver?logo=apple&logoColor=white"></a>
  <a href="https://en.wikipedia.org/wiki/PowerPC_G3"><img alt="PowerPC" src="https://img.shields.io/badge/CPU-PowerPC%20G3%2FG4-blue"></a>
  <a href="https://en.wikipedia.org/wiki/CodeWarrior"><img alt="Built with CodeWarrior" src="https://img.shields.io/badge/built%20with-CodeWarrior%208-orange"></a>
  <a href="https://en.wikipedia.org/wiki/Carbon_(API)"><img alt="Carbon API" src="https://img.shields.io/badge/API-Carbon-lightgrey"></a>
  <a href="docs/css-status.md"><img alt="CSS coverage" src="https://img.shields.io/badge/CSS-150%2B%20properties-9cf"></a>
  <img alt="JavaScript engine" src="https://img.shields.io/badge/JavaScript-Duktape%20ES5-lightgrey">
  <a href="https://github.com/mplsllc/macsurf/discussions"><img alt="Discussions" src="https://img.shields.io/github/discussions/mplsllc/macsurf?color=purple"></a>
</p>

---

<p align="center">
  <img src="img/mactls.png" alt="Mac OS 9, meet TLS — Native, no proxy needed." width="500">
</p>

---

> [!WARNING]
> **MacSurf is a very early alpha.** It runs, it renders real CSS3, it executes JavaScript on a beige G3, **and as of 2026-05-25 it speaks TLS 1.2 natively** against real modern HTTPS sites (`mactrove.com` is the canonical hardware test target). **Most websites in 2026 will still not work** in it — heavy SPAs, modern CSS features MacSurf doesn't ship yet, missing form interactions, slow JS on real hardware. A lot is still rough.
>
> **It is ready to be tested.** If you've got a Power Mac G3 / G4 sitting around, please load it up and try it. Bug reports and screenshots from real hardware are exactly what this project needs right now. **Coders welcome too**; there's an enormous amount of CSS / DOM / JS surface left to fill in, and the code is approachable C89 (the same C you'd have written in 1999). See [docs/status.md](docs/status.md) for the current punch list and [docs/README.md](docs/README.md) for the doc index.
>
> What you should *not* expect yet: smooth browsing of arbitrary modern sites, video, audio, WebGL, service workers, anything React-heavy. What you *can* expect: hand-built pages, retro-style sites, a respectable subset of the CSS Grid spec, native HTTPS with the full Mozilla CA bundle, and the surreal experience of running ES5 JavaScript on a 233 MHz PowerPC.
>
> First numbered version: **0.1a1** (2026-05-20). Latest release: **[v0.7 — Cleanup](https://github.com/mplsllc/macsurf/releases/latest)** (2026-05-26). Twelve issues closed in one sprint — cross-element `var()`, CSS Grid V2 alignment, image-purge on nav, gradients via standard CSS syntax. Full notes: [docs/release-notes/MacSurf-0.7.md](docs/release-notes/MacSurf-0.7.md).

---

## Why MacSurf exists

The web outgrew Classic Mac OS twenty years ago. Modern HTTPS killed it for good around 2016. Today, an out-of-the-box G3 or G4 running OS 9 can barely reach a single live website.

MacSurf brings the real web back. Not a screenshot proxy. Not a remote terminal session. A native browser, built with the tools that were on the platform—CodeWarrior, the Carbon API, QuickDraw, Open Transport—running real CSS3 layouts and real JavaScript inside the 64-megabyte memory floor of a 1999 iMac. **As of 2026-05-25 it talks TLS 1.2 directly to the modern web**, through a BearSSL-based stack ([macTLS](https://github.com/mplsllc/macTLS)) that ships inside the browser binary with the full 121-anchor Mozilla CA bundle. No proxy required.

It is, as far as we can find, the first serious [NetSurf](https://www.netsurf-browser.org/) port to Classic Mac OS, and the first browser shipped on Mac OS 9 with native CSS Grid, CSS custom properties, and ES5 JavaScript.

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
</table>

---

## What's in the box

<table>
<tr><th align="left">Component</th><th align="left">Language</th><th align="left">Purpose</th></tr>
<tr>
<td><a href="browser/"><code>browser/</code></a></td>
<td>C (C89, CW8)</td>
<td>NetSurf fork with a <code>macos9</code> frontend. Carbon UI, QuickDraw plotters, Open Transport networking, Duktape JS engine.</td>
</tr>
<tr>
<td><a href="proxy/"><code>proxy/</code></a></td>
<td>Go (stdlib only)</td>
<td>TLS-stripping HTTP proxy. Mac sends plain HTTP, proxy fetches via HTTPS, returns plain HTTP. Deploy on a VPS or run locally.</td>
</tr>
<tr>
<td><code>macTLS</code><br><sub>sibling repo</sub></td>
<td>C (CW8)</td>
<td>Native TLS 1.2 library for OS 9, modern HTTPS straight from the Mac, no proxy required. Built on BearSSL with ten embedded root CAs.</td>
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
- QuickDraw plotters with offscreen GWorld back-buffer

**CSS**: ~150 properties consumed in layout
- Custom properties &amp; `var()`
- Flex (`justify-content`, `align-content`, `order`)
- Grid V1 + `grid-template-columns/rows`, `gap`
- `border-radius`, `box-shadow`, opacity
- Linear &amp; radial gradients
- `text-shadow`, `text-overflow: ellipsis`
- `transform` (rotate / translate / scale)
- z-index stacking contexts (CSS 2.1 painting order)
- CSS counters, viewport units, `aspect-ratio`
- Font-family aliases (sans / serif / monospace)

[Full CSS status &rarr;](docs/css-status.md)

</td>
<td valign="top" width="50%">

**JavaScript**: Duktape 2.7.0 ES5
- Closures, prototypes, regex, JSON
- Promises (polyfill), recursion, Mandelbrot
- ~6&nbsp;sec ackermann(3,7) on a 233&nbsp;MHz G3

**Images**: all five formats
- PNG with real per-pixel alpha (lodepng + `CopyMask`)
- GIF with palette transparency
- JPEG, BMP, TIFF

**Networking**
- Open Transport TCP, plain non-`InContext` calls
- HTTP/1.1 + chunked + keep-alive + 3xx follow
- Connection pooling, 15s no-progress timeout
- HTTPS via Go proxy or native macTLS

**Chrome**
- Address bar, back / forward / reload / home
- Status bar, page-info, multi-window
- Smooth scroll bar, keyboard scrolling

</td>
</tr>
</table>

---

## Download

Latest release: **[MacSurf v0.7 — Cleanup](https://github.com/mplsllc/macsurf/releases/tag/v0.7)** (2026-05-26, correctness + cleanup sprint — 12 issues closed in one day: cross-element `var()`, CSS Grid V2 alignment, image-purge on nav, gradients via standard CSS syntax). Previous: [v0.6.2 — Speed-Run](https://github.com/mplsllc/macsurf/releases/tag/v0.6.2) (cold mactrove.com 30+ s → ~2-3 s), [v0.6.1](https://github.com/mplsllc/macsurf/releases/tag/v0.6.1), [v0.6](https://github.com/mplsllc/macsurf/releases/tag/v0.6), [v0.5.0](https://github.com/mplsllc/macsurf/releases/tag/v0.5.0), [v0.1a1](https://github.com/mplsllc/macsurf/releases/tag/v0.1a1).

- **[MacSurf.sit](https://github.com/mplsllc/macsurf/releases/download/v0.7/MacSurf.sit)** (latest binary, v0.7), ready-to-run. Expand on Mac OS 9.1+ with CarbonLib 1.5+ and launch.
- Building from source: clone this repo, then on the Mac side open `browser/netsurf/frontends/macos9/MacSurf.mcp` in CodeWarrior 8 and choose Build. The earliest release ships a [BuildPack.sit](https://github.com/mplsllc/macsurf/releases/download/v0.1a1/MacSurf-BuildPack.sit) snapshot with the CW8 project pre-wired; current builds work straight from the git clone.

All releases (with full notes per version): [github.com/mplsllc/macsurf/releases](https://github.com/mplsllc/macsurf/releases). Earlier alpha release notes: [docs/release-notes/MacSurf-0.1a1.md](docs/release-notes/MacSurf-0.1a1.md).

---

## Getting started

<table>
<tr>
<td valign="top" width="50%">

### Building the browser
MacSurf is built on Mac OS 9 with CodeWarrior 8 Pro (8.3 update). Source is cross-compile-clean against Retro68 PowerPC GCC for fast Linux-side syntax checks.

- [Mac-side build guide](docs/codewarrior-setup.md)
- [Linux cross-dev workflow](docs/cross-dev-from-linux.md)

</td>
<td valign="top" width="50%">

### Running the proxy
A single Go binary. No config files. No dependencies beyond stdlib.

```bash
cd proxy
go build -o macsurf-proxy
./macsurf-proxy
