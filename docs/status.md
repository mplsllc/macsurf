# MacSurf Status

**Date:** 2026-05-24
**Engine HEAD:** fixes199h
**Current fix round:** fixes200 (CSS standards map docs/tools)
**Last release:** MacSurf v0.5.0 (2026-05-24)
**Last hardware-accepted (explicitly recorded in this file):** fixes158a (2026-05-20)

---

## What MacSurf is, today

MacSurf is a working web browser for Classic Mac OS 9.1–9.2.2 on PowerPC, built on a NetSurf fork with a Carbon / QuickDraw / Open Transport frontend. For modern HTTPS sites it uses a Go TLS-stripping proxy; native TLS is a separate sibling library (`macSSL`) and is not the default browsing path yet.

It runs on real beige G3-class hardware (with G4 upgrade also used in development). The build target is CodeWarrior 8 Pro (8.3 update), strict C89, with a ~16 MB application partition. The remote-fetch path is plain HTTP from the Mac → Go proxy → upstream HTTPS, streamed.

## What works in the current tree (engine: fixes199h, docs/tools: fixes200)

### Rendering pipeline
- Full NetSurf fetch → parse → cascade → layout → plot
- libcss with native `var()` resolution, custom properties
- libdom + libhubbub HTML5 parsing
- libnsbmp / libnsgif / libjpeg / lodepng / libtiff for images
- QuickDraw plotters with offscreen GWorld back buffer (fixes77g+)
- Defensive-clamp threshold at ±200000 px in `redraw.c` (fixes156)
- Layout hardening / watchdog caps to keep the engine alive on hostile modern pages (fixes170–173)
- Inline SVG V1 renderer for common page-chrome icons and logos (fixes195–197)

### CSS (layout + paint), ~150+ properties with a growing “parsed → consumed” surface
- Custom properties + `var()` resolution
- Flexbox: `justify-content`, `align-content`, `align-items`, `align-self`, `order`, `flex-direction`, `flex-wrap`, `flex-basis`, `flex-grow`, `flex-shrink`
- **CSS Grid (V1+V2):** track grammar (`fr`, `repeat()`, `minmax()`), `grid-template-rows`, gaps, explicit placement (`grid-column*`, `grid-row*`, `grid-area`), `grid-template-areas` name lowering, auto-flow occupancy avoidance, `align-items` / `align-self` consumption (justify-* still limited)
- **Multi-column layout (V1):** `column-count`, `column-width`, `column-gap`, `column-rule-*` paint (fixes179+)
- `border-radius`, `box-shadow`, opacity, linear & radial gradients
- `text-shadow` and `transform` bridged from standard CSS3 via `cssh_css` preprocessor (fixes175, fixes183)
- z-index stacking contexts (CSS 2.1 painting order, fixes147)
- CSS counters, viewport units (`vh`, `vw`), `aspect-ratio`
- Font-family aliases (sans → Helvetica, serif → Times, mono → Monaco), fixes157, no horizontal scrambling on mixed-family inline runs
- `background-size` (bitmaps V1), `position: sticky` (vertical V1), `inset` shorthand lowering (fixes191)
- `object-fit` + `object-position` (V1; `object-position` now has a real libcss property at fixes199h)
- See [css-status.md](css-status.md) for the full property-by-property audit

### JavaScript
- Duktape 2.7.0, full ES5
- Closures, prototypes, regex, JSON, promises (polyfill), recursion
- Date arithmetic (Mac epoch 1904 → Unix epoch 1970 bridge)
- Ackermann(3,7) in ~5-6 sec on a 233 MHz G3
- Mandelbrot fractal in pure JS
- Basic DOM bridge exists (document + element wrappers; expanding coverage), plus MacSurf-side timer and XHR plumbing

### Networking
- Open Transport TCP, `OTOpenEndpointInContext` synchronous calls yielding on `kOTSyncIdleEvent`
- HTTP/1.1 with chunked transfer, keep-alive, 3xx redirect follow
- Connection pooling (128 fetcher slots, 16 concurrent)
- 15s no-progress timeout
- Persistent on-disk HTTP body cache to reduce redo work after refresh/crash (fixes172)
- HTTPS via Go proxy; native macSSL exists as a sibling project and can be integrated as a later round

### Chrome
- Address bar, back / forward / reload / home
- Status bar, page-info, multi-window
- Smooth scrollbar, keyboard scrolling
- Hover state recascade + reformat
- UA stylesheet tweaks for modern pages (for example: collapse `<details>` by default, fixes186)

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

**fixes200, CSS standards map (docs/tools).** Adds/refreshes the repo’s canonical “what’s implemented vs. what’s missing” view: `CSS_SUPPORT_MATRIX.md`, `CSS_IMPLEMENTATION_PLAN.md`, and supporting audit/docs.

**Engine baseline for this round:** fixes199h, cumulative updates. Focuses on “real site” layout correctness and a few long-requested CSS consumptions:

- Multi-column layout refinements in the layout engine (follow-on to fixes179/180/182*)
- `object-position` promoted from a cssh-css “drop” into a real libcss property + consumer path
- Build / integration fixes and misc layout/redraw adjustments

---

## Recently shipped

| Fix | Description | Status |
|-----|-------------|--------|
| **fixes200** | CSS standards map docs/tools (`CSS_SUPPORT_MATRIX.md`, implementation plan, audit/docs) | Landed |
| **fixes199h** | Multi-column refinements + `object-position` as a real libcss property + build fixes | Landed |
| **fixes195–197** | Inline SVG V1 renderer + sizing hints + diagnostics | Landed |
| **fixes191** | `inset` shorthand, `background-size` (bitmaps), `position: sticky` (V1), modern CSS “safe drop” bundle | Landed |
| **fixes189–190** | Alpha correctness in ARGB copy path + composite-path rollback | Landed |
| **fixes187–188** | PNG premultiply/mask fixes + scaled-PNG composite attempt | Landed |
| **fixes185–186** | Modern-CSS compatibility preprocessor bundle + collapse `<details>` by default | Landed |
| **fixes183–184** | Standard `transform` bridge + `table-layout: fixed` correctness | Landed |
| **fixes179–182*** | Multi-column layout V1 + follow-on routing / diagnostics / correctness fixes | Landed |
| **fixes171–174** | Layout watchdog + survival-layer hardening + CSS size cap raised | Landed |
| **fixes172** | Persistent on-disk HTTP body cache | Landed |

See [HISTORY.md](HISTORY.md) for the full version timeline going back to v0.1.

---

## What's queued next

The “modern site survival” work is now mostly about filling in specific missing consumptions rather than single big subsystems. The near-term queue (order may change):

- **Grid:** justify-* parser/consumption strategy that avoids the libcss mid-enum trap (place-* shorthands later)
- **Multi-column:** `column-span: all`, better balance/fill behaviour, edge-case block fragmentation
- **SVG V2:** gradients, transforms, `<text>`, and improved path coverage (`A/S/T`)
- **Forms / interaction:** hit-testing semantics (`pointer-events`), focus/outline polish, input widgets
- **Native TLS:** integrate `macSSL` as an optional direct-HTTPS path (proxy remains primary)

---

## Known limitations

- **No HTTPS by default in the browser core.** All TLS goes through the Go proxy; `macSSL` exists but is not yet the default integrated path.
- **No preemptive threading.** Cooperative `WaitNextEvent` event loop only, all networking yields via `kOTSyncIdleEvent`.
- **No subgrid.**
- **16 MB application partition ceiling.** libcss allocates from the OS heap and runs out below ~12 MB free on heavy pages.
- **8 grid tracks max** per row or column.
- **Max 256 children per grid container.** Excess fall back to fixes151 auto-flow.
- **Grid alignment gaps:** baseline alignment, `place-*` shorthands, writing-mode logical alignment; justify-* is still constrained.
- **JavaScript Date arithmetic** anchored to a fixed 2026 baseline because Mac OS 9's `GetDateTime` returns 1904-epoch seconds with no DST handling.

---

## Documentation index

- [architecture.md](architecture.md), System architecture, module map, networking model
- [HISTORY.md](HISTORY.md), Milestone timeline from v0.1 forward
- [css-status.md](css-status.md), Property-by-property CSS audit
- [codewarrior-setup.md](codewarrior-setup.md), Mac-side build
- [cross-dev-from-linux.md](cross-dev-from-linux.md), Linux cross-dev workflow + Retro68 syntax pre-flight
- [deploying-proxy.md](deploying-proxy.md), Go proxy deploy guide
- [security-notes.md](security-notes.md), Reachable attack surface + record of dismissed external scanner reports
- [story.html](story.html), Narrative writeup with screenshots
