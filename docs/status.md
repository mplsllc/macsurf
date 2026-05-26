# MacSurf Status

**Date:** 2026-05-26
**Engine HEAD:** fixes271b
**Current fix round:** fixes264–271b — correctness + cleanup sprint ("Cleanup"): cross-element `var()` via doc-global inline-extras, CSS Grid V2 alignment (`justify-content`/`align-content`), `background-image: linear-gradient()` rewrite, image-purge on top-level nav, above-fold lazy decode, total CSS budget, heavy/blocker SITE diagnostic, multicol balancing via bisection, header audit cleanup
**Last release:** **MacSurf v0.7 "Cleanup"** (2026-05-26). Twelve open issues closed in one sprint. Full notes: [release-notes/MacSurf-0.7.md](release-notes/MacSurf-0.7.md)
**Last hardware-accepted:** fixes271b (2026-05-26, header audit cleanup — Linux/Mac `box.h` aligned, `_ns` suffix workaround retired, standard `box_multicol_data`/`box_multicol_segment` names restored)
**Open issues on `mplsllc/macsurf`:** **0**

---

## What MacSurf is, today

MacSurf is a working web browser for Classic Mac OS 9.1–9.2.2 on PowerPC, built on a NetSurf fork with a Carbon / QuickDraw / Open Transport frontend. **Native HTTPS works end-to-end via macTLS (BearSSL + Open Transport); the Go TLS-stripping proxy is retired from the default path** as of 2026-05-25.

It runs on real beige G3-class hardware (with G4 upgrade also used in development). The build target is CodeWarrior 8 Pro (8.3 update), strict C89, with a ~16 MB application partition. The remote-fetch path is now direct TLS 1.2 from the Mac to the origin, using the full Mozilla CA bundle (121 trust anchors) bundled into the binary.

## What works in the current tree (engine: fixes225)

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
- Connection pooling (128 fetcher slots, 16 concurrent HTTP + 16 concurrent HTTPS)
- 15s no-progress timeout
- Persistent on-disk body cache shared between HTTP and HTTPS (fixes172, refactored to `macos9_disk_cache.[ch]` at fixes218)
- **Native HTTPS via macTLS (BearSSL + Open Transport).** TLS 1.2 with the full Mozilla CA bundle (121 trust anchors; ISRG, DigiCert, GTS, Sectigo, GlobalSign, Entrust, AAA, etc.). Cipher in the wild: `TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256`. **Verified end-to-end on real OS 9 hardware against mactrove.com 2026-05-25 (first-light, fixes208-212).** The Go TLS-stripping proxy is retired from the default path; macTLS is now the canonical HTTPS fetcher.
- HTTPS cache-hit serving is temporarily disabled (fixes222) pending a synthetic-header rework — cache STORE is live, READ comes back in the load-time QOL round.

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

**fixes216–225, macTLS hardening + cache extraction + dark-grey root cause.** Closed the long-standing "mactrove looks dark" regression and landed the production native-HTTPS path.

**Engine baseline:** fixes225. Highlights:

- **fixes225** — disable the inset box-shadow paint that was washing mactrove dark grey. Root cause was a fallback to `#666666` when `box_shadow_color == 0` (var()-resolved colors not round-tripping through the RGB555 pack/unpack). Gated behind `MACSURF_INSET_BOX_SHADOW` (default 0). Visual cost: 1px Platinum inner-bevel on `.window` cards. V2: trace and fix the var() → `box_shadow_color` round-trip, re-enable.
- **fixes222** — disabled HTTPS HS_CACHEHIT serving because synthetic header dispatch didn't match `html_create` expectations. Cache STORE still runs. Pending rework.
- **fixes219** — title-bar UTF-8 mojibake fix (em-dash, smart quotes now route through `macos9_utf8_to_macroman`). Plot-rect log gate bumped 8 → 300.
- **fixes218** — extract on-disk cache to `macos9_disk_cache.[ch]`, shared between HTTP and HTTPS fetchers.
- **fixes217** — macTLS trust anchor bundle expanded from 10 → 121 (full Mozilla CCADB bundle from curl.se/cacert.pem).
- **fixes216** — `MS_LOG` of `hctx_fail` reason so silent HTTPS failures (peer-closed, handshake-failed, timeout) surface in the log.

---

## Recently shipped

| Fix | Description | Status |
|-----|-------------|--------|
| **fixes216–225** | macTLS bundle expansion + cache refactor + dark-grey root cause + diag | Landed, hw-verified |
| **fixes208–212** | macTLS first-light on real OS 9 hardware against mactrove.com (proxy retired) | Landed, hw-verified |
| **fixes203** | SVG rect rotation + box-filter image downscale | Landed |
| **fixes201–202** | Big CSS round (box-shadow inset, pointer-events, text-shadow blur) + inline-style preprocessor | Landed |
| **fixes199h** | Multi-column refinements + `object-position` as a real libcss property + build fixes | Landed |
| **fixes195–197** | Inline SVG V1 renderer + sizing hints + diagnostics | Landed |
| **fixes191** | `inset` shorthand, `background-size` (bitmaps), `position: sticky` (V1), modern CSS "safe drop" bundle | Landed |
| **fixes189–190** | Alpha correctness in ARGB copy path + composite-path rollback | Landed |
| **fixes187–188** | PNG premultiply/mask fixes + scaled-PNG composite attempt | Landed |
| **fixes185–186** | Modern-CSS compatibility preprocessor bundle + collapse `<details>` by default | Landed |
| **fixes183–184** | Standard `transform` bridge + `table-layout: fixed` correctness | Landed |
| **fixes179–182*** | Multi-column layout V1 + follow-on routing / diagnostics / correctness fixes | Landed |
| **fixes172** | Persistent on-disk HTTP body cache | Landed |

See [HISTORY.md](HISTORY.md) for the full version timeline going back to v0.1.

---

## What's queued next

Now that native HTTPS is live and the dark-grey regression is closed, the next round is load-time QOL ahead of a 0.6 cut:

- **Re-enable HTTPS cache HIT serving.** Cache STORE is already collecting bodies; the synthetic FETCH_HEADER / DATA / FINISHED dispatch in HS_CACHEHIT needs to mirror the live header stream exactly so `html_create` accepts the bootstrapped content. Biggest single reload-speed win (~6s saved on a cached mactrove reload).
- **TLS keep-alive / connection pooling for HTTPS.** Right now every sub-resource fetch does a fresh handshake. 40 handshakes on a mactrove cold load × ~200ms each on a G3 = ~8s of redundant TLS work. Mirror the HTTP fetcher's pool pattern.
- **HTTPS slot pool 16 → 32.** ~21% of mactrove fetches currently hit `NO FREE SLOTS`. Bumping is one-line, ~32 KB per slot.
- **Reformat coalescing.** ~18 full reformats per mactrove load (every CSS/image arrival fires one). Batch to a single tick window to cut layout work ~5×.
- **NetSurf-core "abort during nav while old fetches in-flight" bug.** Navigating to a new URL while sub-resources from the previous page are still streaming triggers an `ops.abort` on the new top-level fetch, which falls back to `about:query/fetcherror`. Needs llcache / fetch.c instrumentation. **Tracked separately** (see "Known limitations" below).

The standards-coverage queue (Grid `justify-*`, multi-column `column-span: all`, SVG V2 gradients/transforms/text, form interaction, var() → `box_shadow_color` round-trip to re-enable inset bevels) gets picked up after the QOL round.

---

## Known limitations

- **Navigation during in-flight sub-resources can land on `about:query/fetcherror`.** When the user submits a new URL while the previous page's CSS/image fetches are still streaming, NetSurf core fires `ops.abort` on the new top-level fetch (likely `llcache` lifecycle interaction with high concurrency). The new fetch lands with `status=200` and partial body, then aborts. Workaround: wait for the status bar to read "Done" before navigating. Real fix needs NetSurf-core instrumentation.
- **HTTPS cache-hit serving is off** (fixes222). Cache STORE runs and the `MacSurf Cache` folder fills, but reloads currently re-fetch over fresh TLS. Comes back in the load-time QOL round.
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
