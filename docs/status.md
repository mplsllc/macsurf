# MacSurf Status

**Date:** 2026-05-20
**Last shipped:** fixes159a, crash fix for fixes159's Grid V2 alignment round
**Last hardware-accepted:** fixes158a, auto-flow occupancy bitmap (Grid V2 explicit placement, 6 of 6 probes passing on G3)

---

## What MacSurf is, today

MacSurf is a working web browser for Mac OS 9.1+ on PowerPC, built on a NetSurf fork with a Carbon / QuickDraw / Open Transport frontend, paired with a Go TLS-stripping proxy and the sibling `macSSL` library for native HTTPS.

It is hardware-verified on a Power Macintosh G3 iMac. The build target is CodeWarrior 8 Pro (8.3 update), strict C89, 16 MB application partition. The remote-fetch path is plain HTTP from the Mac → Go proxy → upstream HTTPS, fully streamed.

## What works on hardware today

### Rendering pipeline
- Full NetSurf fetch → parse → cascade → layout → plot
- libcss with native `var()` resolution, custom properties
- libdom + libhubbub HTML5 parsing
- libnsbmp / libnsgif / libjpeg / lodepng / libtiff for images
- QuickDraw plotters with offscreen GWorld back buffer (fixes77g+)
- Defensive-clamp threshold at ±200000 px in `redraw.c` (fixes156, handles tall pages without zeroing legitimate boxes)

### CSS, roughly 150 properties consumed by layout
- Custom properties + `var()` resolution
- Flexbox: `justify-content`, `align-content`, `align-items`, `align-self`, `order`, `flex-direction`, `flex-wrap`, `flex-basis`, `flex-grow`, `flex-shrink`
- **CSS Grid (V1+V2):** `display: grid`, `grid-template-columns`, `grid-template-rows`, `gap` / `column-gap` / `row-gap`, explicit placement (`grid-column`, `grid-row`, `grid-column-start/-end`, `grid-row-start/-end`), span notation, full-row sentinel (`1 / -1`), auto-flow with occupancy avoidance, alignment (`justify-items`, `align-items`, `justify-self`, `align-self`, `start | end | center | stretch`)
- `border-radius`, `box-shadow`, opacity, linear & radial gradients
- `text-shadow` (vendor `-macsurf-text-shadow` from fixes50), `text-overflow: ellipsis`
- `transform` (rotate / translate / scale via vendor `-macsurf-transform`)
- z-index stacking contexts (CSS 2.1 painting order, fixes147)
- CSS counters, viewport units (`vh`, `vw`), `aspect-ratio`
- Font-family aliases (sans → Helvetica, serif → Times, mono → Monaco), fixes157, no horizontal scrambling on mixed-family inline runs
- See [css-status.md](css-status.md) for the full property-by-property audit

### JavaScript
- Duktape 2.7.0, full ES5
- Closures, prototypes, regex, JSON, promises (polyfill), recursion
- Date arithmetic (Mac epoch 1904 → Unix epoch 1970 bridge)
- Ackermann(3,7) in ~5-6 sec on a 233 MHz G3
- Mandelbrot fractal in pure JS

### Networking
- Open Transport TCP, `OTOpenEndpointInContext` synchronous calls yielding on `kOTSyncIdleEvent`
- HTTP/1.1 with chunked transfer, keep-alive, 3xx redirect follow
- Connection pooling (128 fetcher slots, 16 concurrent)
- 15s no-progress timeout
- HTTPS via Go proxy or native macSSL (sibling project)

### Chrome
- Address bar, back / forward / reload / home
- Status bar, page-info, multi-window
- Smooth scrollbar, keyboard scrolling
- Hover state recascade + reformat

---

## Build target

- **Compiler:** Metrowerks CodeWarrior 8 Pro with the 8.3 update
- **Output:** PEF / CFM, PowerPC-only
- **Project file:** `MacSurf.mcp` (binary, not in this repo, see [`builds/MacSurf-BuildPack.sit`](../builds/MacSurf-BuildPack.sit))
- **Target settings:** 16 MB application partition, 2 MB image cache, 128/16 fetcher pool
- **Prerequisites:** Mac OS 9.1+, CarbonLib 1.5+, StuffIt Expander, a real Power Mac (G3/G4) or SheepShaver with caveats
- **Cross-dev pre-flight:** Retro68 PowerPC GCC + `scripts/verify_macsurf.sh` for `-std=c89 -pedantic` syntax checks before each fix ships

See [codewarrior-setup.md](codewarrior-setup.md) for the full Mac-side build walkthrough and [cross-dev-from-linux.md](cross-dev-from-linux.md) for the Linux-side workflow.

---

## Current fix round

**fixes159 / fixes159a, Grid V2 alignment.** Adds `justify-items`, `align-items`, `justify-self`, `align-self` for grid containers and items (values: `start | end | center | stretch`).

- `align-items` / `align-self` ride libcss's existing flexbox plumbing (no changes there)
- `justify-items` / `justify-self` go through a new vendor libcss property `-macsurf-justify` (single packed int32, low nibble = justify-items, high nibble = justify-self)
- `cssh_css.c` preprocessor adds a fourth pass that rewrites `justify-items: X; justify-self: Y` declarations into the packed property
- `layout_grid.c` pass 3 reads container defaults + per-item overrides and applies horizontal / vertical offsets when the child is narrower / shorter than its cell

fixes159 shipped with the new field inserted mid-struct in `css_computed_style_i`, which crashed on G3 because CW8 didn't rebuild libcss's internal .o files against the shifted field offsets (the [CW8 misses-header-recompile](https://github.com/mplsllc/macsurf/blob/master/.private/llm/CLAUDE.md) gotcha). fixes159a relocated the field to the end of the struct, no other field's offset shifts, so libcss .o files compiled against the old layout still see consistent offsets for everything else. Awaiting hardware verification.

### V1 limitations (documented, not bugs)
- Items with `width: auto` still stretch horizontally even when `justify-*` requests a non-stretch alignment, without an intrinsic-content pass we can't size an item smaller than its cell at this layer. Use explicit widths.
- Same applies to `height: auto` + `align-*`.
- `place-items` / `place-self` shorthands deferred.
- Baseline alignment, safe/unsafe modifiers, writing-mode interactions deferred.
- Column AND row alignment for the same element must live in the same CSS rule (cascade collapses the merged storage at the property level, inherited from fixes158).

---

## Recently shipped

| Fix | Description | Status |
|-----|-------------|--------|
| **fixes159a** | Move `macsurf_justify` to end of `css_computed_style_i` struct to avoid the offset-shift CW8 stale-recompile crash from fixes159 | Awaiting G3 |
| **fixes159** | Grid V2 alignment (justify/align items/self) | Crashed pre-159a |
| **fixes158a** | Auto-flow occupancy bitmap so explicit grid items aren't overdrawn by auto-flow siblings | ✓ G3 accepted |
| **fixes158** | Explicit CSS Grid placement V1 (`grid-column`, `grid-row`, longhand start/end) | ✓ via 158a |
| **fixes157** | Font-family aliases (sans→Helvetica, serif→Times, mono→Monaco). Closes fixes52/fixes145 5-year sidestep | ✓ G3 accepted |
| **fixes157a** | Silence FONTDIAG after acceptance (`MACSURF_FONT_ALIAS_DIAG = 0`) | ✓ G3 accepted |
| **fixes156** | Raise defensive-clamp y/height thresholds from ±10000 to ±200000 in `redraw.c html_redraw_box`. Closes the post-fixes152 "empty render" saga, page-height growth past 10000 px from accumulated probe cards was nuking real content | ✓ G3 accepted |
| **fixes152** | CSS `aspect-ratio` V1 | ✓ G3 accepted |
| **fixes151** | Grid `grid-column` explicit column placement (V1, span + A/B + 1/-1) | ✓ G3 accepted |
| **fixes150** | `grid-template-rows` (px row tracks; FR/percent degrade to tallest-child) | ✓ G3 accepted |
| **fixes149** | Verified CSS_STATUS claims that `min-height` and `vw`/`vh` were broken, found them already wired (false alarm in status doc) | ✓ G3 accepted |
| **fixes148** | CSS Grid V2 standard track grammar (`grid-template-columns: 1fr 1fr`, `repeat()`, `minmax()`, idents) | ✓ G3 accepted |
| **fixes147** | CSS 2.1 stacking-context paint order (sibling-level z-index correct) | ✓ G3 accepted |

See [HISTORY.md](HISTORY.md) for the full version timeline going back to v0.1.

---

## What's queued next

Per the planning notes from the most recent feature round:

- **fixes160**, `grid-template-areas`. Named cell regions for explicit placement by name.
- **fixes161**, `column-count` / `column-rule`. Multi-column layout outside Grid.
- **fixes162**, `outline` / focus-ring accessibility polish.

The font-family work is intentionally cooling after fixes157 hardware acceptance, broader stacks and per-font metric integration are deferred.

---

## Known limitations

- **No HTTPS in the browser core.** All TLS goes through the Go proxy (which fetches upstream HTTPS and serves plain HTTP to the Mac) or via macSSL (sibling library, not yet integrated into the MacSurf binary).
- **No preemptive threading.** Cooperative `WaitNextEvent` event loop only, all networking yields via `kOTSyncIdleEvent`.
- **No subgrid.** Grid V1+V2 only.
- **16 MB application partition ceiling.** libcss allocates from the OS heap and runs out below ~12 MB free on heavy pages.
- **8 grid tracks max** per row or column.
- **Max 256 children per grid container.** Excess fall back to fixes151 auto-flow.
- **No baseline alignment**, no `place-*` shorthands, no writing-mode logical alignment (fixes159 V1 scope).
- **JavaScript Date arithmetic** anchored to a fixed 2026 baseline because Mac OS 9's `GetDateTime` returns 1904-epoch seconds with no DST handling.

---

## Documentation index

- [architecture.md](architecture.md), System architecture, module map, networking model
- [HISTORY.md](HISTORY.md), Milestone timeline from v0.1 forward
- [css-status.md](css-status.md), Property-by-property CSS audit
- [codewarrior-setup.md](codewarrior-setup.md), Mac-side build
- [cross-dev-from-linux.md](cross-dev-from-linux.md), Linux cross-dev workflow + Retro68 syntax pre-flight
- [deploying-proxy.md](deploying-proxy.md), Go proxy deploy guide
- [story.html](story.html), Narrative writeup with screenshots
