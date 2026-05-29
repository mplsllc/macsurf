# MacSurf Status

**Date:** 2026-05-29
**Engine HEAD:** fixes315
**Current fix round:** fixes305–315 — the v1.2 "Sealed" sprint. **macTLS v1.0 (macEntropy v1.0)** closes the documented insecure-stub entropy hole with a SHA-256 accumulator feeding BearSSL's HMAC-DRBG; OT packet-arrival jitter, event-loop mouse/key timing, high-resolution clock samples, and a Preferences-folder seed file across boots. **POST forms** now wire the body through both fetchers (was silently `(void)pu` at setup pre-fix). **First download manager** with NavPutFile save dialog, FSWrite streaming, MIME-mapped Mac type/creator codes, partial-file cleanup, and Content-Disposition: attachment routing for Drupal-style sites. **Background-attachment: fixed** anchors to viewport in window coords (#41). **white-space: pre-line** collapses internal whitespace runs (#56). **SVG fill-opacity / stroke-opacity** wired (#36). **HTTPS abort crash** on duckduckgo.com fixed (#150) — the fixes249b auto-upgrade FALLBACK was emitting synthetic FETCH_REDIRECT on NetSurf-initiated aborts, crashing llcache_fetch_redirect on a NULL fetch handle.
**Last release:** **MacSurf v1.2 "Sealed"** (2026-05-29). Six issues closed since v1.0 plus the macEntropy v1.0 fold-in. Full notes: [release-notes/MacSurf-1.2.md](release-notes/MacSurf-1.2.md).
**Last hardware-accepted:** fixes315 (2026-05-29).
**Companion site:** **[home.macsurf.org](https://home.macsurf.org/)** — server-rendered PHP portal with search, weather, and four news feeds. No JS dependency, class-based CSS only.
**Open issues on `mplsllc/macsurf`:** the long tail (~60), the modern HTML5 / JS / CSS features the project intentionally tracks separately. Nothing blocking real-site rendering at 1.2.

---

## Where the project sits today

MacSurf is a working web browser for Classic Mac OS 9.1–9.2.2 on PowerPC, built on a NetSurf fork with a Carbon / QuickDraw / Open Transport frontend. As of 2026-05-25 it speaks TLS 1.2 end-to-end via macTLS (BearSSL on top of Open Transport), so the Go TLS-stripping proxy is no longer on the default path. As of 2026-05-29 (v1.2) the entropy backing those TLS handshakes is **macEntropy v1.0** — SHA-256 accumulator + BearSSL HMAC-DRBG, fed by OT packet jitter, event-loop input, high-res clock, and a persisted seed file. The pre-v1.2 insecure-stub entropy source is closed.

The build runs on real beige G3-class hardware, with a G4 upgrade also used in development. The target compiler is CodeWarrior 8 Pro with the 8.3 update, strict C89, and a 16 MB application partition. Network fetches go direct via TLS 1.2 to the origin, using the full Mozilla CA bundle (121 trust anchors) baked into the binary.

## What works in the current tree

### Rendering pipeline
- Full NetSurf fetch → parse → cascade → layout → plot
- libcss with native `var()` resolution and custom properties
- libdom + libhubbub for HTML5 parsing
- libnsbmp, libnsgif, libjpeg, lodepng, libtiff for images
- QuickDraw plotters backed by an offscreen GWorld (fixes77g and later)
- Defensive-clamp threshold at ±200000 px in `redraw.c` (fixes156)
- Layout hardening and watchdog caps to keep the engine alive on hostile modern pages (fixes170–173)
- Inline SVG V1 renderer for common page-chrome icons and logos (fixes195–197)

### CSS — around 150 properties consumed in layout
- Custom properties + `var()` resolution
- Flexbox: `justify-content`, `align-content`, `align-items`, `align-self`, `order`, `flex-direction`, `flex-wrap`, `flex-basis`, `flex-grow`, `flex-shrink`
- **CSS Grid (V1 + V2)**: track grammar (`fr`, `repeat()`, `minmax()`), `grid-template-rows`, gaps, explicit placement (`grid-column*`, `grid-row*`, `grid-area`), `grid-template-areas` name lowering, auto-flow occupancy avoidance, `align-items` and `align-self`. `justify-*` is still limited.
- **Multi-column layout (V1)**: `column-count`, `column-width`, `column-gap`, `column-rule-*` paint (fixes179 onwards)
- `border-radius`, `box-shadow`, opacity, linear and radial gradients
- `text-shadow` and `transform` bridged from standard CSS3 via the `cssh_css` preprocessor (fixes175, fixes183)
- z-index stacking contexts following CSS 2.1 painting order (fixes147)
- CSS counters, viewport units (`vh`, `vw`), `aspect-ratio`
- Font-family aliases (sans → Helvetica, serif → Times, mono → Monaco), shipped at fixes157 — no horizontal scrambling on mixed-family inline runs
- `background-size` for bitmaps (V1), vertical `position: sticky` (V1), `inset` shorthand lowering (fixes191)
- `object-fit` plus `object-position` (V1; `object-position` got its own libcss property at fixes199h)
- See [css-status.md](css-status.md) for the property-by-property audit.

### JavaScript
- Duktape 2.7.0, full ES5
- Closures, prototypes, regex, JSON, promises (polyfill), recursion
- Date arithmetic that bridges the Mac epoch (1904) to the Unix epoch (1970)
- `ackermann(3,7)` in about 5–6 seconds on a 233 MHz G3
- Mandelbrot fractal in pure JS
- Basic DOM bridge (document + element wrappers; coverage growing), plus MacSurf-side timer and XHR plumbing

### Networking
- Open Transport TCP, `OTOpenEndpointInContext` synchronous calls yielding on `kOTSyncIdleEvent`
- HTTP/1.1 with chunked transfer, keep-alive, and 3xx redirect follow
- Connection pooling (128 fetcher slots, 16 concurrent HTTP + 16 concurrent HTTPS)
- 15-second no-progress timeout
- Persistent on-disk body cache shared between HTTP and HTTPS (fixes172, refactored into `macos9_disk_cache.[ch]` at fixes218)
- **Native HTTPS via macTLS** (BearSSL on Open Transport). TLS 1.2 with the full Mozilla CA bundle — 121 trust anchors including ISRG, DigiCert, GTS, Sectigo, GlobalSign, Entrust, AAA, and the rest. Cipher in the wild is typically `TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256`. Verified end-to-end against mactrove.com on real OS 9 hardware on 2026-05-25 (the "first-light" fixes208–212 round). The Go TLS-stripping proxy is retired from the default path; macTLS is the canonical HTTPS fetcher now.
- HTTPS cache-hit serving is temporarily disabled (fixes222) pending a synthetic-header rework. STORE is live; READ comes back in the load-time QOL round.

### Browser chrome
- Address bar, back / forward / reload / home
- Status bar, page-info, multi-window
- Smooth scrollbar, keyboard scrolling
- Hover-state recascade plus reformat
- A few UA stylesheet tweaks for modern pages — for example, collapsing `<details>` by default (fixes186)

---

## Build target

- **Compiler:** Metrowerks CodeWarrior 8 Pro with the 8.3 update
- **Output:** PEF / CFM, PowerPC only
- **Project file:** `MacSurf.mcp` (binary, not in this repo — see [`builds/MacSurf-BuildPack.sit`](../builds/MacSurf-BuildPack.sit))
- **Target settings:** 16 MB application partition, 2 MB image cache, 128/16 fetcher pool
- **Prerequisites:** Mac OS 9.1+, CarbonLib 1.5+, StuffIt Expander, and a real Power Mac (G3 or G4) — or SheepShaver with caveats
- **Cross-dev pre-flight:** Retro68 PowerPC GCC + `scripts/verify_macsurf.sh` for `-std=c89 -pedantic` syntax checks before any fix ships

See [codewarrior-setup.md](codewarrior-setup.md) for the Mac-side build walkthrough and [cross-dev-from-linux.md](cross-dev-from-linux.md) for the Linux-side workflow.

---

## Current fix round

**fixes216–225 — macTLS hardening, cache extraction, and the dark-grey root cause.** This round closed the long-standing "mactrove looks dark" regression and shipped the production native-HTTPS path.

Engine baseline is fixes225. Highlights:

- **fixes225** — disabled the inset box-shadow paint that was washing mactrove dark grey. Root cause was a fallback to `#666666` when `box_shadow_color == 0`, which happened because `var()`-resolved colors weren't round-tripping through the RGB555 pack/unpack. Gated behind `MACSURF_INSET_BOX_SHADOW` (default 0). Visual cost: the 1px Platinum inner bevel on `.window` cards. V2 is to trace and fix the var() → `box_shadow_color` round-trip and re-enable.
- **fixes222** — disabled HTTPS HS_CACHEHIT serving because the synthetic header dispatch didn't match what `html_create` expects. Cache STORE still runs. Rework pending.
- **fixes219** — title-bar UTF-8 mojibake fix. Em-dashes and smart quotes now route through `macos9_utf8_to_macroman`. Plot-rect log gate bumped from 8 to 300.
- **fixes218** — extracted the on-disk cache to `macos9_disk_cache.[ch]`, now shared between HTTP and HTTPS fetchers.
- **fixes217** — macTLS trust anchor bundle expanded from 10 to 121 (the full Mozilla CCADB bundle from curl.se/cacert.pem).
- **fixes216** — added `MS_LOG` of `hctx_fail` reason, so silent HTTPS failures (peer-closed, handshake-failed, timeout) finally surface in the log.

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
| **fixes189–190** | Alpha correctness in the ARGB copy path + composite-path rollback | Landed |
| **fixes187–188** | PNG premultiply / mask fixes + scaled-PNG composite attempt | Landed |
| **fixes185–186** | Modern-CSS compatibility preprocessor bundle + collapse `<details>` by default | Landed |
| **fixes183–184** | Standard `transform` bridge + `table-layout: fixed` correctness | Landed |
| **fixes179–182*** | Multi-column layout V1 + follow-on routing / diagnostics / correctness fixes | Landed |
| **fixes172** | Persistent on-disk HTTP body cache | Landed |

See [HISTORY.md](HISTORY.md) for the full timeline back to v0.1.

---

## What's queued next

With native HTTPS live and the dark-grey regression closed, the next round is load-time quality-of-life work ahead of a 0.6 cut:

- **Re-enable HTTPS cache HIT serving.** STORE already collects bodies; the synthetic FETCH_HEADER / DATA / FINISHED dispatch in HS_CACHEHIT needs to mirror the live header stream exactly so `html_create` accepts the bootstrapped content. This is the single biggest reload-speed win (about 6 seconds saved on a cached mactrove reload).
- **TLS keep-alive and connection pooling for HTTPS.** Today every sub-resource fetch does a fresh handshake. Forty handshakes on a cold mactrove load × ~200 ms each on a G3 is ~8 seconds of redundant TLS work. Should mirror the HTTP fetcher's pool pattern.
- **Bump the HTTPS slot pool from 16 to 32.** About 21% of mactrove fetches currently hit `NO FREE SLOTS`. One-line change, ~32 KB per slot.
- **Reformat coalescing.** Around 18 full reformats per mactrove load (every CSS or image arrival fires one). Batch into a single tick window to cut layout work roughly 5×.
- **The NetSurf-core "abort during nav while old fetches in-flight" bug.** Navigating to a new URL while sub-resources from the previous page are still streaming triggers an `ops.abort` on the new top-level fetch, which then falls back to `about:query/fetcherror`. Needs llcache / fetch.c instrumentation. **Tracked separately** (see "Known limitations" below).

The standards-coverage queue — Grid `justify-*`, multi-column `column-span: all`, SVG V2 gradients/transforms/text, form interaction, and the var() → `box_shadow_color` round-trip to re-enable inset bevels — picks up after the QOL round.

---

## Known limitations

- **Navigation during in-flight sub-resources can land on `about:query/fetcherror`.** Submit a new URL while the previous page's CSS / image fetches are still streaming and NetSurf core fires `ops.abort` on the new top-level fetch (probably an `llcache` lifecycle issue under high concurrency). The new fetch lands with `status=200` and a partial body, then aborts. Workaround: wait for the status bar to read "Done" before navigating. A real fix needs NetSurf-core instrumentation.
- **HTTPS cache-hit serving is off** (fixes222). STORE runs and the `MacSurf Cache` folder fills, but reloads currently re-fetch over fresh TLS. Coming back in the load-time QOL round.
- **No preemptive threading.** Cooperative `WaitNextEvent` event loop only; all networking yields via `kOTSyncIdleEvent`.
- **No subgrid.**
- **16 MB application partition ceiling.** libcss allocates from the OS heap and runs out below ~12 MB free on heavy pages.
- **8 grid tracks maximum** per row or column.
- **Max 256 children per grid container.** Excess fall back to the fixes151 auto-flow path.
- **Grid alignment gaps**: baseline alignment, the `place-*` shorthands, writing-mode logical alignment. `justify-*` is still constrained.
- **JavaScript Date arithmetic** is anchored to a fixed 2026 baseline because Mac OS 9's `GetDateTime` returns 1904-epoch seconds with no DST handling.

---

## Documentation index

- [architecture.md](architecture.md) — system architecture, module map, networking model
- [HISTORY.md](HISTORY.md) — milestone timeline from v0.1 forward
- [css-status.md](css-status.md) — property-by-property CSS audit
- [codewarrior-setup.md](codewarrior-setup.md) — Mac-side build walkthrough
- [cross-dev-from-linux.md](cross-dev-from-linux.md) — Linux cross-dev workflow + Retro68 syntax pre-flight
- [deploying-proxy.md](deploying-proxy.md) — Go proxy deploy guide
- [security-notes.md](security-notes.md) — reachable attack surface + record of dismissed external scanner reports
- [story.html](story.html) — narrative writeup with screenshots
