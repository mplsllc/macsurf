# MacSurf Status

**Date:** 2026-08-19
**Release baseline:** fixes894 (`master`, MacSurf 2.0.5). **Active development:**
`workflow` through fixes1216; this work is not part of a released build.

**Post-release workflow status:** fixes1197–1201 and fixes1203–1209 are shipped;
fixes1208 native grid `justify-self` is iMac hardware-verified. fixes1210/1211
replace the About-window Unix audio launcher with in-memory QuickTime playback;
Gary and Kaija audio are iMac hardware-verified, with no audio files written to disk.
fixes1215/1216 add longhand `background-origin`; `border-box`, `padding-box`,
and `content-box` are iMac hardware-verified for raster backgrounds.
fixes1218–1223 add `background-clip: text`; gradient and repeated-bitmap sources
are iMac hardware-verified inside glyph masks.
**Current release:** **MacSurf 2.0.5 "HACKADAY"** (2026-07-17) — a polish release over 2.0. Headline: **hackaday.com renders at full desktop width**. The load-bearing fix underneath was browser-wide — every author `font-size` was drawing ~25% too small and CSS `em`/`rem`/`@media` width queries were computing against the wrong number, so pages came up cramped into a narrow column; MacSurf now measures type in real device pixels (#244/#287, fixes859). On top of that: a large modern-CSS pass (justified text #271, soft hyphens #272/#275, `tab-size`/typography cluster #251, box-alignment shorthands #253, logical properties #247, grid auto-track sizing #62, `caret-color`/`accent-color` #252, `background-clip` #255, `image-rendering` #256, inline-`style` rewriters #277); a much more capable on-device JavaScript engine (real `fetch()`/`XMLHttpRequest`, draining Promise chains, `document.cookie`, DOM traversal + `querySelector`, load lifecycle — #283–#302); tracker/ad-network blocking; text/plain rendering inline (#232); rgba backdrop compositing (#227); and a typing-latency dirty-rect fix (#212/#239). Full notes: [release-notes/MacSurf-2.0.5.md](release-notes/MacSurf-2.0.5.md). *(The 2.0, 1.68.1, and v1.4 narratives below are retained as history.)*

**Historical (v1.4 round):** v1.4 "Open House" — **the JavaScript marathon closed.** Twenty-three GitHub issues went from open to closed across fixes319-352 — `setTimeout` / `setInterval` / `requestAnimationFrame`, `window.location` (full surface), `window.history` (`pushState` / `replaceState` / `state`), `URL` + `URLSearchParams`, `element.classList`, `element.style`, `Event` / `CustomEvent` / `MouseEvent` / `KeyboardEvent` constructors, `MutationObserver`, `DOMParser`, `FormData`, `localStorage`, `fetch`, `window.addEventListener` for `load` + `DOMContentLoaded`, `<details>` / `<summary>` click-to-toggle, `hidden` attribute. The purpose-built probe page at `mactrove.com/t.html` scored **`JS 19/19 pass, 0 fail`** on a G3 iMac. Two structural bugs caught along the way: **fixes349** repaired the IIFE per-element installer broken by fixes342's `_noresult` change (`TypeError: [object Object] not callable` on every element wrapper, the install aborted mid-stream and elements lost classList / style / matches / closest / etc); **fixes350** extended `js_fire_event` to dispatch `_winListeners` so `load` / `DOMContentLoaded` actually reach `addEventListener` listeners (was only firing the inline `on<type>` handler).

Diagnostic + power-user features also landed: **about:cache**, **about:memory**, **about:config**, **about:perf** all render real diagnostic pages (about:perf carries a live counters table including `reformat_ms` captured in `html_reformat`). **View Source** now uses `content_get_source_data` + a `data:text/html` URL — renders inline as HTML in a `<pre>` block (fixes352a fixed the underlying `data:` URL fetcher, which had been a stub returning empty body since launch, so every `data:` URL on every page now works). **Find-in-page** opens a real Carbon dialog (kDocumentWindowClass + TextEdit input + Find / Cancel buttons) routing to `browser_window_search`. **#99 root cause** turned out to be the URL-bar `strstr("://")` heuristic mangling opaque schemes (about:, data:, javascript:, mailto:, file:, resource:) to `https://about:cache` etc; replaced with a proper RFC 3986 scheme scanner that unblocks all five opaque schemes from URL-bar typing.

CSS / gradient fidelity also moved in this window: **fixes348** downgraded alpha-overlay gradients to NONE so Platinum pinstripes (`rgba(...) 1px, transparent 1px`) stop rendering as harsh black-to-white bands; **fixes344b** added real alpha-aware gradient stops on an outer-struct side channel so RGBA + `transparent` no longer truncate to opaque; **fixes345** captured radial-gradient size+position prefixes; **fixes346** added pinstripe / repeating-pattern recovery so first==last across N≥3 stops swaps to the first distinct intermediate colour.
**Predecessor:** v1.3.1 "Forward, refined" (2026-05-29) — multi-curve ECDHE in TLS 1.3. v1.3 "Forward" (2026-05-29) — first native TLS 1.3 on Classic Mac OS, ever. v1.2 "Sealed" (same day) — macEntropy v1.0 closed the entropy hole, POST forms wired, V1 download manager landed.
**Latest release:** **MacSurf 2.0.5 "HACKADAY"** (2026-07-17). Full notes: [release-notes/MacSurf-2.0.5.md](release-notes/MacSurf-2.0.5.md). Predecessor: 2.0 ([notes](release-notes/MacSurf-2.0.md)), 1.68.1 "macQJS" ([notes](release-notes/MacSurf-1.68.1.md)), v1.4 "Open House" ([notes](release-notes/MacSurf-1.4.md)).
**Last hardware-accepted:** the entire MacSurf 2.0 batch (blank-screen #207, cross-signed HTTPS #206, lazy images #223, text-select cluster, autocomplete #231, typing latency #212, History/Bookmark managers) on a G3 iMac OS 9.2.2 (2026-07-11). The 2.0.5 CSS/JS batch is shipped and awaiting hardware sign-off on the specific reported cases.
**Companion site:** **[home.macsurf.org](https://home.macsurf.org/)** — server-rendered PHP portal with search, weather, and four news feeds. No JS dependency, class-based CSS only.
**Open issues on `mplsllc/macsurf`:** ~90, the modern HTML5 / JS / CSS long tail the project intentionally tracks separately (the 2.0.5 batch just closed ~30). Nothing in the long tail blocks real-site rendering.

---

## Where the project sits today

MacSurf is a working web browser for Classic Mac OS 9.1–9.2.2 on PowerPC, built on a NetSurf fork with a Carbon / QuickDraw / Open Transport frontend. As of 2026-05-25 it speaks TLS end-to-end via macTLS (BearSSL on top of Open Transport), so the Go TLS-stripping proxy is no longer on the default path. As of 2026-05-29 (v1.2) the entropy backing those TLS handshakes is **macEntropy v1.0** — SHA-256 accumulator + BearSSL HMAC-DRBG, fed by OT packet jitter, event-loop input, high-res clock, and a persisted seed file. The pre-v1.2 insecure-stub entropy source is closed. **Also as of 2026-05-29 (v1.3), the wire protocol is TLS 1.3** — hand-written per RFC 8446 on BearSSL primitives, X25519 key exchange, ChaCha20-Poly1305 and AES-128-GCM, with transparent fallback to TLS 1.2. First native TLS 1.3 on Classic Mac OS, ever.

The build runs on a G3 iMac for current work, with a beige G3 Minitower (Sonnet G4 upgrade) for the initial development arc. The target compiler is CodeWarrior 8 Pro with the 8.3 update, strict C89. The application partition is set to **224 MB preferred / 128 MB minimum, 16 MB stack** — validated stable across heavy JavaScript sites (~208 MB usable heap; on a smaller partition a single large allocation fails as "out of contiguous memory" long before total free is exhausted). That maps to **128 MB RAM minimum, 256 MB recommended, 384 MB for the heaviest JS sites**. Network fetches go direct to the origin over **TLS 1.3 (1.2 fallback)**, using the full Mozilla CA bundle (121 trust anchors) baked into the binary.

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
- **Type measured in real device pixels** (2.0.5, #244/#287) — author `font-size`, `em`/`rem`, and `@media` width queries all resolve at 96 dpi like a mainstream browser, so pages lay out at their designed desktop width instead of a cramped column
- Flexbox: `justify-content`, `align-content`, `align-items`, `align-self`, `order`, `flex-direction`, `flex-wrap`, `flex-basis`, `flex-grow`, `flex-shrink`
- **CSS Grid (V1 + V2)**: track grammar (`fr`, `repeat()`, `minmax()`), `grid-template-rows`, gaps, explicit placement (`grid-column*`, `grid-row*`, `grid-area`), `grid-template-areas` name lowering, auto-flow occupancy avoidance, **content auto-sized tracks** (2.0.5, #62), `align-items`/`align-self`, `justify-items`, and native `justify-self` (fixes1204/1208, iMac hardware-verified). Placement/span-aware auto sizing and minmax composition remain in Grid Round 2.
- **Box-alignment shorthands** `place-items` / `place-content` / `place-self` (2.0.5, #253) and **CSS Logical Properties** `margin-inline` / `padding-block` / `inset-*` (2.0.5, #247)
- **Typography** (2.0.5): justified text (#271), soft hyphens `&shy;` (#272/#275), `tab-size`, `text-align-last`, `text-justify`, `word-break` (#251), `caret-color`/`accent-color` (#252), `text-decoration` color/style/thickness (#249) — several first-for-classic-Mac, not in upstream NetSurf
- **Multi-column layout (V1)**: `column-count`, `column-width`, `column-gap`, `column-rule-*` paint (fixes179 onwards)
- `border-radius`, `box-shadow`, opacity, linear and radial gradients; **rgba backgrounds composite against the real backdrop** (2.0.5, #227); `background-clip` box values plus hardware-verified gradient/bitmap `text` clipping (fixes1218–1223), longhand `background-origin` (fixes1215/1216, iMac hardware-verified), and `background: none` reset (2.0.5, #255/#268)
- `text-shadow` and `transform` bridged from standard CSS3 via the `cssh_css` preprocessor; the modern-CSS rewriters now also run on inline `style=""` (2.0.5, #277)
- z-index stacking contexts following CSS 2.1 painting order (fixes147)
- CSS counters, viewport units (`vh`, `vw`), `aspect-ratio`
- Font-family aliases (sans → Helvetica, serif → Times, mono → Monaco) — no horizontal scrambling on mixed-family inline runs
- `background-size` for bitmaps (V1), vertical `position: sticky` (V1), `inset` shorthand lowering
- `object-fit` plus `object-position` (V1)
- See [css-support.md](css-support.md) for the property-by-property audit.

### JavaScript — macQJS (QuickJS, ES2023)
- QuickJS port running modern ES2023 natively on PowerPC — `let`/`const`, arrows, classes, template literals, generators, native `Promise`, modern regex (the in-house ES6→ES5 transpiler was retired at fixes522)
- Real `fetch()` and `XMLHttpRequest` backed by the network (over the same fetch path as the rest of the browser, so cookies/UA apply), relative-URL resolution against the document base, and a drained job queue so Promise chains resolve past their first step (2.0.5, #283–#302)
- Real DOM: node traversal (`firstChild`/`childNodes`/`cloneNode`), `querySelector`/`querySelectorAll` compound selectors, `createElementNS`, `document.cookie` over the real jar, and the `readyState`/`DOMContentLoaded`/`load` lifecycle
- Date arithmetic bridging the Mac epoch (1904) to the Unix epoch (1970)
- Runs real site bundles on-device (jQuery, Preact, webpack runtimes); heavy DOM-mutation SPAs are the open frontier

### Networking
- Open Transport TCP, `OTOpenEndpointInContext` synchronous calls yielding on `kOTSyncIdleEvent`
- HTTP/1.1 with chunked transfer, keep-alive, and 3xx redirect follow
- Connection pooling (128 fetcher slots, 16 concurrent HTTP + 16 concurrent HTTPS)
- 15-second no-progress timeout
- Persistent on-disk body + image/font cache shared between HTTP and HTTPS (`macos9_disk_cache.[ch]`), 64 MB LRU budget
- Cookie jar (RFC 6265) wired into both fetchers, per-host User-Agent override, disk-persisted cookies so logins survive relaunch
- **Native HTTPS via macTLS** (BearSSL on Open Transport). **TLS 1.3 (RFC 8446) with TLS 1.2 fallback** — X25519 + multi-curve ECDHE, ChaCha20-Poly1305 / AES-128-GCM — and the full Mozilla CA bundle (121 trust anchors). Out-of-order cross-signed chains are reordered so sites like macintoshgarden.org validate (#206); cert dates are checked against GMT via the Mac's timezone offset (#282). The Go TLS-stripping proxy is retired; macTLS is the canonical HTTPS fetcher.
- **Tracker/ad-network blocking** (2.0.5) — known analytics/ad hosts are refused before they load
- Viewport-gated lazy image loading for all images (#223) — heavy forums load in seconds, not minutes

### Browser chrome
- Address bar with type-ahead autocomplete + suggestions dropdown (#231), back / forward / reload / home
- Real text input: blinking caret, click-drag selection, Cut/Copy/Paste, Tab between form fields
- Day-grouped History manager (Cmd-H) and folder-based Bookmark manager (Cmd-B) windows; downloads manager
- Status bar, page-info, multi-window (File → New Window / Cmd-N); tabs are unimplemented
- Dirty-rect repaint so typing/caret/hover/small-scroll are snappy (2.0.5, #212/#239)
- Smooth scrollbar, keyboard scrolling; no Carbon mouse-wheel (not available in CarbonLib on OS 9)
- Hover-state recascade plus reformat; UA stylesheet tweaks for modern pages (e.g. collapsing `<details>` by default)

---

## Build target

- **Compiler:** Metrowerks CodeWarrior 8 Pro with the 8.3 update
- **Output:** PEF / CFM, PowerPC only
- **Project file:** `MacSurf.mcp` (binary, not in this repo — see [`builds/MacSurf-BuildPack.sit`](../builds/MacSurf-BuildPack.sit))
- **Target settings:** 224 MB preferred / 128 MB minimum application partition, 16 MB stack (~208 MB usable heap, validated stable on heavy JS sites; ~76 MB leaner than the old 300/102 config, reclaimed from an oversized stack), 64 MB disk cache budget, 128/16 fetcher pool
- **Prerequisites:** Mac OS 9.1+, CarbonLib 1.5+, StuffIt Expander, and a real Power Mac (G3 or G4) — or SheepShaver with caveats
- **Cross-dev pre-flight:** Retro68 PowerPC GCC + `scripts/verify_macsurf.sh` for `-std=c89 -pedantic` syntax checks before any fix ships

See [codewarrior-setup.md](codewarrior-setup.md) for the Mac-side build walkthrough and [cross-dev-from-linux.md](cross-dev-from-linux.md) for the Linux-side workflow.

---

## Current fix round

**fixes876–894 — the 2.0.5 "HACKADAY" batch.** Post-2.0 this line landed the whole CSS-coverage push (fixes771–834), the Facebook Round 2 / JS-engine work (fixes835–875), and the revamp Phase 0/1 batch (fixes876–893), then shipped as 2.0.5. The release head is **fixes894** (a revert of fixes884/885 — the cert-failure-no-cleartext gate broke bare-domain→http→https redirects like hackernews.com, so the http fallback the retro-http audience relies on was restored for the release, to be fixed forward). Highlights:

- **Device-pixel type (fixes859, #244/#287)** — the load-bearing fix: `css_unit_len2device_px` at 96 dpi so author `font-size`, `em`/`rem`, and `@media` queries resolve like a mainstream browser. This is what flipped Hackaday (and many sites) from a cramped column to full desktop layout.
- **Modern-CSS pass (fixes804–834)** — typography cluster, box-alignment shorthands, logical properties, grid auto-track sizing, `justify-items`, `background-clip`, `image-rendering`, inline-`style` rewriters, rgba backdrop compositing.
- **JS engine (fixes846–875, #283–#302)** — real `fetch()`/`XMLHttpRequest`, a drained Promise job queue, `createElementNS`, compound `querySelector`, `on*` handlers, `document.cookie`, DOM traversal, and the document load lifecycle.
- **TLS date/chain correctness (fixes739/834, #206/#282)** and tracker/ad-network blocking.

*Reconvert (re-running box construction after JS DOM mutation) is turned OFF in this release while a deep hover/scrollbar crash chain is chased forward.*

---

## Recently shipped (releases)

| Release | Date | Headline | Status |
|-----|------|----------|--------|
| **2.0.5 "HACKADAY"** | 2026-07-17 | hackaday.com renders; device-px type; big CSS + JS-engine batch | Shipped |
| **2.0** | 2026-07-11 | blank-screen fix (#207); cross-signed HTTPS; lazy images; autocomplete; History/Bookmarks | Shipped, hw-verified |
| **1.68.x "macQJS"** | 2026-07-07/08 | QuickJS migration; sticky hit-test; login persistence; text input; disk cache | Shipped, hw-verified |
| **1.4 "Open House"** | 2026-06-01 | the JavaScript marathon (23 issues) | Shipped, hw-verified |
| **1.3 "Forward"** | 2026-05-29 | first native TLS 1.3 on Classic Mac OS | Shipped |

See the [release notes](release-notes/) for per-version history.

---

## What's queued next

Highest-value remaining work, from [research/css-gap-inventory-2026-07-13.md](research/css-gap-inventory-2026-07-13.md) and the open JS/DOM frontier:

- **`appearance` + form-control styling (#80/#90)** — synthetic CSS-painted controls replacing Carbon Control Manager where `appearance:none` is set. Its own round.
- **`min-content`/`max-content`/`fit-content` intrinsic sizing** — the structural prerequisite that unblocks `table-layout:auto` and correct flex/grid shrink-to-fit.
- **Grid Round 2 (#279)** — placement/span-aware auto sizing, minmax composition, and §12.8 stretch. `justify-self` is complete (fixes1204/1208) and is no longer part of this queue.
- **Reconvert crash chain** — land the hover/scrollbar/timer fixes so JS DOM mutation can repaint safely, then re-enable reconvert.
- **Heavy DOM-mutation SPAs** — the open JS frontier (event bubbling/dispatch bridge #264, `getComputedStyle`/`getBoundingClientRect` #265, real in-page interactivity).

---

## Known limitations

- **Cache-hit first-paint.** A navigation first-paints the placeholder before the deferred cache-hit delivery completes, so a cached page can flash `about:` before the real content lands. Cache STORE and READ both run; the bug is at the paint-trigger timing, not the fetcher. Open.
- **Reconvert is off in this release.** JS that mutates the DOM after load won't repaint until the hover/scrollbar/timer crash chain is landed and reconvert is re-enabled.
- **Heavy DOM-mutation SPAs** — GitHub, video, React-heavy apps — don't render. Deep in-page interactivity (event bubbling/dispatch bridge #264, `getComputedStyle`/`getBoundingClientRect` #265) is still being built.
- **No preemptive threading.** Cooperative `WaitNextEvent` event loop only; all networking yields via `kOTSyncIdleEvent`.
- **No Carbon mouse-wheel** — the event class was never back-ported to CarbonLib on OS 9; scroll via bar, arrows, Page Up/Down, Home/End.
- **No subgrid.**
- **8 grid tracks maximum** per row or column; max 256 children per grid container (excess fall back to the auto-flow path). Grid Round 2 placement/span-aware auto sizing, minmax composition, and §12.8 stretch remain open.
- **JavaScript Date arithmetic** is anchored to a fixed 2026 baseline because Mac OS 9's `GetDateTime` returns 1904-epoch seconds with no DST handling.

---

## Documentation index

- [architecture.md](architecture.md) — system architecture, module map, networking model
- [release-notes/](release-notes/) — per-version release notes back to v0.1
- [css-support.md](css-support.md) — property-by-property CSS audit
- [research/css-gap-inventory-2026-07-13.md](research/css-gap-inventory-2026-07-13.md) — the deep CSS gap inventory (current ground truth)
- [codewarrior-setup.md](codewarrior-setup.md) — Mac-side build walkthrough
- [cross-dev-from-linux.md](cross-dev-from-linux.md) — Linux cross-dev workflow + Retro68 syntax pre-flight
- [resources.md](resources.md) — the `'carb'` / icon / BNDL resource pipeline
