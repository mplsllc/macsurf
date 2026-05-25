# MacSurf 0.1a1, Release Notes

**Release date:** 2026-05-20
**Source ref:** commit `dd118ad6` (fixes158a hardware-acceptance build)
**Distribution:** `builds/MacSurf.sit` (compiled binary), `builds/MacSurf-BuildPack.sit` (source + CodeWarrior project)
**Platform:** Mac OS 9.1, 9.2.2, PowerPC G3 / G4, CarbonLib 1.5+
**Memory floor:** 16 MB application partition (Carbon), 64 MB system RAM
**Built with:** CodeWarrior 8 Pro (8.3 update)

This is the first numbered alpha, the build everything else has converged toward across the fixes100+ sweep. It is the stable foundation for the fixes159 Grid V2 alignment work and beyond.

---

## What works

### Rendering pipeline

- Full NetSurf pipeline: fetch → parse (libhubbub) → DOM (libdom) → CSS cascade (libcss) → layout → QuickDraw plot.
- libcss native `var()` resolution for CSS custom properties.
- QuickDraw plotters back-buffered through an offscreen `GWorld` (introduced fixes77g) so updates don't flicker.
- Defensive box-clamp at `redraw.c` widened to ±200000 (fixes156), pages up to ~200k px tall render cleanly.

### CSS, ~150 properties consumed by layout

- **Box model:** margin, padding, border (color / style / width), border-radius (FrameRoundRect / PaintRoundRect).
- **Background:** color, image (URL), linear-gradient, radial-gradient, gradient stops.
- **Typography:** font-family with native dispatch (sans-serif → Helvetica, serif → Times, monospace → Monaco, landed fixes157), font-size, font-weight (incl. real bold faces on the Mac), line-height, text-align, text-decoration, text-shadow (via the `-macsurf-text-shadow` vendor property, fixes50).
- **Color:** rgb / rgba / hsl / hsla / 6-digit hex / 8-digit hex / named.
- **Display / positioning:** block, inline, inline-block, none, flex, grid, position absolute / relative / fixed.
- **Flexbox** (read-axis V1): flex-direction, justify-content, align-items, align-content, order, flex-wrap.
- **Grid V1:** grid-template-columns, grid-template-rows, grid-gap / column-gap / row-gap, span N, `1 / -1` full-row hero.
- **Grid placement V1** (fixes158): `grid-column: A / B`, `grid-row: A / B`, longhand `grid-column-start / -end`, `grid-row-start / -end`. Positive integer lines only, with the cssh_css preprocessor packing all four placement fields into one int32. fixes158a's three-pass walker uses an occupancy bitmap so auto-flow correctly skips past explicitly-placed cells instead of collapsing onto them.
- **Transforms:** rotate, scale, translate (fixes73 / 73e).
- **Effects:** box-shadow, opacity (QuickDraw stipple), text-shadow.
- **Custom properties:** `var()` lookups, including secondary-storage compose for properties like rotation+scale companion data (project gotcha note in `project_libcss_secondary_storage_compose.md`).
- **Pseudo-classes / pseudo-elements:** `:hover`, `:first-child`, `:last-child`, `:nth-child`, `::before`, `::after`.
- **Media queries:** `min-width`, `max-width`.
- **Viewport units:** `vh`, `vw`.
- **Aspect-ratio:** `aspect-ratio: 16 / 9` and bare-number form (fixes152).
- **Counters:** counter-reset, counter-increment, `counter()` in content.
- **Text overflow:** `text-overflow: ellipsis`, `word-break`, `overflow-wrap`.

### JavaScript

- **Engine:** Duktape 2.7.0, ES5 evaluator, embedded as `libduktape` and built into the binary.
- **Working:** closures, prototypes, regex (test / match / replace), JSON (parse / stringify), promises (polyfill), recursion (Ackermann 3,7 in ~6 seconds on a 233 MHz G3), array higher-order methods, ES5 strict mode, math benchmarks, ASCII Mandelbrot fractal rendered live.
- **Limitations:** no JIT (tree-walking interpreter), no `eval()` sandbox, no Web APIs (Promise polyfill is JS-only).

### Images

- **PNG** with real per-pixel alpha via lodepng + `CopyMask`.
- **GIF** with palette transparency (fixes79b).
- **JPEG** via the system JPEG decoder.
- **BMP** native.
- **TIFF** via QuickTime.

### Networking

- **Transport:** Open Transport TCP, plain non-`InContext` calls. Connection pooling, keep-alive, 15-second no-progress timeout.
- **HTTP/1.1:** chunked transfer, keep-alive, 3xx redirect follow.
- **HTTPS:** via the bundled Go TLS-stripping proxy (the Mac speaks plain HTTP; the proxy fetches HTTPS upstream and returns plain). Optional native TLS 1.2 path via the **macTLS** sibling project (BearSSL-based, ChaCha20-Poly1305 default cipher), shipped separately.

### Chrome

- Address bar, back / forward / reload / home buttons.
- Status bar, page-info text, multi-window.
- Smooth scrolling, keyboard navigation, click-to-navigate links, hover dispatch with recascade + reformat.

---

## Known limitations in 0.1a1

- **Grid V2 alignment** (`justify-items`, `align-items`, `justify-self`, `align-self`), **not in this release.** The first attempt (fixes159) hit a libcss struct-layout gotcha and is reworking. 0.1a1 grids default to stretch alignment.
- **Subgrid, `grid-template-areas`, named grid lines, dense auto-flow**, out of V1 scope.
- **`place-items`, `place-self`** shorthands, out of V1 scope.
- **Negative grid line numbers**, only `-1` is supported as the "fill row" sentinel; `-2`, etc. are not.
- **Cooperative multitasking only**, no preemptive threads anywhere. `WaitNextEvent` drives the UI; Open Transport synchronous calls yield via the Thread Manager on `kOTSyncIdleEvent`.
- **Strict C89**, no `inline`, no `//`, no designated initializers, no variadic macros, no for-scope declarations. CW8 doesn't compile anything more modern.
- **16 MB Carbon application partition**, libcss allocates from the OS heap and runs out below ~12 MB on real pages. Heavy SPAs may crash; lean docs are fine.
- **No HTTPS in the browser itself** without macTLS, TLS is bridged through the proxy.
- **CSS engine is grid-V1-aware but not yet grid-V2-aware**, stretch is the universal default; non-stretch alignment lands in 0.2.

---

## Verified hardware platforms

- **Power Mac G3 iMac** (beige / blueberry), Mac OS 9.1, 233 MHz, 96 MB RAM, primary dev rig, all probes pass.
- **CarbonLib 1.5+**, required for the Carbon API surface MacSurf uses. CarbonLib 1.6 is fine.

Not yet tested:
- Beige Power Mac G3 Minitower (kernel attributes verified but never run-tested, wheel-mouse crash from the earlier sweep remains deferred).
- iBook / Pismo PowerBooks (should work; not exercised).
- SheepShaver, likely works but no canonical test rig.

---

## Milestones reached during the 0.1a1 sprint

| Fix round | What landed |
|-----------|-------------|
| fixes24-33 | "CSS applies on G3 hardware", first real-world CSS3 rendering on real Mac OS 9 (2026-05-13) |
| fixes55-70 | Recovery sprint after a CSS-engine regression; restored full pipeline |
| fixes73-73e | CSS Transform (rotate / translate / scale) |
| fixes74-74d | Radial gradient |
| fixes75-75e | CSS Grid V1 (PROBE G1-G7 all green incl. variable-height rows + nested grids) |
| fixes77g | Offscreen GWorld back buffer |
| fixes78-79b | Image pipeline (PNG / GIF / JPEG / BMP / TIFF) with PNG alpha |
| fixes105+ | Fetch system recovery (NetSurf pipeline restored after rewrite) |
| fixes147 | z-index stacking contexts (CSS 2.1 painting order) |
| fixes148 | CSS Grid V2 standard track grammar (`fr` units, `repeat()`, `minmax()`) |
| fixes149-152 | Viewport units, `aspect-ratio`, grid-template-rows |
| fixes153 | GetFontInfo vertical-metric probe |
| fixes154-157 | Font-family alias retry, closing a five-year force-Helvetica workaround |
| fixes156 | Defensive-clamp threshold raised (the root cause of the empty-render saga; closed a four-round red-herring chase) |
| fixes157 | Sans / serif / monospace dispatch finally clean on hardware |
| fixes158 | Explicit grid placement V1 (`grid-column`, `grid-row`, longhands) |
| **fixes158a** | **Auto-flow occupancy bitmap, final shipping commit for 0.1a1.** |

---

## Build & install

### Pre-built binary
1. Download [**MacSurf.sit**](https://github.com/mplsllc/macsurf/releases/download/v0.1a1/MacSurf.sit) (581 KB).
2. Expand with StuffIt Expander on Mac OS 9.
3. Drop the resulting `MacSurf` application anywhere on disk.
4. Double-click to launch.

### Build from source
1. Download [**MacSurf-BuildPack.sit**](https://github.com/mplsllc/macsurf/releases/download/v0.1a1/MacSurf-BuildPack.sit) (4 MB).
2. Expand on Mac OS 9.
3. Open `MacSurf.mcp` in CodeWarrior 8 Pro.
4. Make sure the 8.3 update is applied.
5. **Build → Build**.
6. The resulting application drops into the same folder.

See the [CodeWarrior setup guide](https://github.com/mplsllc/macsurf/blob/master/docs/codewarrior-setup.md) for the full setup walkthrough including required CarbonLib SDK paths.

---

## Prior art and credit

- [NetSurf](https://www.netsurf-browser.org/), MacSurf is a fork. GPLv2 + OpenSSL linking exception inherited.
- [Classilla](https://sourceforge.net/projects/classilla/), Open Transport architecture borrowed from `macsockotpt.c`.
- [cy384/ssheven](https://github.com/cy384/ssheven), cooperative-multitasking + OT reference on real hardware.
- [Duktape](https://duktape.org/), embedded as the JS engine.
- [lodepng](https://lodev.org/lodepng/), PNG decoder with alpha.

---

## What's next (0.2 / 0.1a2 candidates)

- **Grid V2 alignment**, `justify-items`, `align-items`, `justify-self`, `align-self`. Currently in flight (fixes159 → fixes159c diagnostic round).
- **`grid-template-areas`**, named cell layouts.
- **`column-count` / `column-rule`**, CSS columns.
- **`outline`**, focus rings, accessibility polish.
- **`place-items` / `place-self`** shorthands once the four base properties are stable.

---

*MacSurf 0.1a1 is the first version anyone could reasonably hand someone and say "load it on a beige G3 and read the web." The CSS engine renders real modern stylesheets at the level fixes24-33 promised; the grid machinery is robust enough to lay out card-heavy sites; JavaScript actually runs. It's an alpha, there are gaps, but it's a real, useful alpha.*
