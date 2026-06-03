# MacSurf → Facebook (Native) — Roadmap (HEAVY path)

> **⚡ 2026-06-03 — read this first.** This document is the **heavy** path: the full
> `www.facebook.com` SPA, which needs a QuickJS engine swap + HTTP/2 (18–30 months).
> We are **not** building this yet. The **active** effort is the **lightweight** path —
> Facebook's no-JavaScript `mbasic.facebook.com` surface — scoped and first step
> shipped in **[facebook-mbasic-scope.md](facebook-mbasic-scope.md)** (issue
> [#167](https://github.com/mplsllc/macsurf/issues/167), fixes367). That gets a usable,
> logged-in Facebook onto a G3 *now*, on the current engine, no new JS engine, no proxy.
> This heavy roadmap remains the long-term north star for the full SPA.
>
> Also note this doc predates v1.4: its **Phase 1 (Tier 1 JS globals)** and most of
> **Phase 2 (DOM bindings)** already landed in the JS marathon (fixes319–352, 19/19 JS
> probe on a G3). The real gate it identifies is **Phase 5 (QuickJS engine swap)**.

**Status:** Heavy path scoped, deferred (lightweight mbasic path is active instead — see above). Captured so the full-SPA plan is documented and ready when the project chooses to take it on.

**Why this document exists:** Facebook is the most architecturally ambitious site the project might ever target. Scoping it forces clarity on what MacSurf is missing structurally, not just what's polish. The decisions captured here apply equally to other modern SPAs (Instagram, Twitter/X, LinkedIn, Slack web client, Discord web client) and the work below would unblock most of those at the same time.

---

## Why native, not proxy

The MacSurf architecture has always reserved a proxy-side "render and flatten" path for sites too modern for the in-browser engine. That path doesn't work for Facebook specifically, for two independent reasons:

1. **Auth has to live on the client.** Login cookies, OAuth flows, MFA prompts, and session refresh all need to be tied to the actual user agent talking to facebook.com. A render-and-flatten proxy is a third party between the user and the auth surface; the auth state would have to be re-issued every navigation, which doesn't match how FB's auth works.

2. **FB anti-bot blocks shared proxies fast.** Facebook's edge sees "N logins from one IP" as adversarial. A multi-user MacSurf proxy would be rate-limited or blacklisted within hours. A single-user self-hosted proxy works around that but recreates the "Facebook only loads on my home server" problem we're trying to escape.

Native is the only path. The whole scope below is what "native" actually requires.

---

## Architectural decisions that have to land before anything else

Three decisions gate the entire roadmap. They need to be made deliberately, not discovered mid-implementation.

### Decision 1: JavaScript engine

Duktape 2.7.0 (currently linked) is ES5-only. Facebook's JS bundle is post-Babel ES2020+ with native `class` / `async`-`await` / optional chaining / nullish coalescing / destructuring assumed throughout. There is no realistic transpile-at-fetch workaround: the bundle is megabytes of minified JS that assumes runtime semantics transpilation doesn't fully replicate (Proxy, Reflect, Symbol, async generators, dynamic `import()`).

**Real options:**

| Engine | ES level | Size | Verdict |
|---|---|---|---|
| **QuickJS** (Fabrice Bellard) | ES2020 + most ES2023 | ~210 KB | **Best fit.** C99 surface is small and portable. Needs a CW8 PPC port pass. |
| mujs | ES5 | tiny | Doesn't help; same generation as Duktape. |
| V8 / JSC fragment port | ES current | huge | Effectively impossible. Both assume modern toolchains and pull `chromium-base` / `WTF`. |
| Stay on Duktape + transpiler | ES5 | tiny | Won't survive contact with FB's actual bundle. |

**Decision: QuickJS.** Port effort is the single biggest item on this whole roadmap. Estimated **3–6 months of focused work** to get it building, running, and ES2020-clean on G3 hardware. Memory footprint is the biggest unknown; QuickJS has a configurable GC budget but FB's working set might exceed the 16 MB Carbon partition.

### Decision 2: HTTP/2 vs HTTP/1.1 + accept-the-cost

Facebook serves hundreds of small sub-resources per pageview and assumes HTTP/2 multiplexing. Over HTTP/1.1, head-of-line blocking makes initial load minutes-long even on a 100 Mbps line. With macTLS already shipping TLS 1.2/1.3, HTTP/2 sits at the application layer on top of the existing connection.

H2 requires:
- HPACK header compression (state-machine, reasonable scope)
- Stream framing (binary protocol, well-specified)
- Stream multiplexing (connection-level state)
- Flow control (per-stream and per-connection windows)
- Server push handling (FB uses sparingly; can probably stub initially)

Estimated **6–8 weeks**, well-specified RFC 7540 territory.

**HTTP/3 / QUIC is optional for the FB target.** FB falls back to H2 cleanly. QUIC over UDP would require its own congestion control + ack mechanism + 0-RTT setup on top of OT's UDP path, estimated 8–12 weeks of additional work. Defer to post-FB-load.

**Decision: implement HTTP/2 on macTLS. Defer H3.**

### Decision 3: Media strategy

Software-decoding AV1 / VP9 / H.264 at usable framerate on a 233 MHz G3 is not physically possible. AV1 is barely real-time on hardware 1000× faster.

**Decision: substitute static still-frame previews for `<video>` elements.** Detect video in DOM, fetch the poster frame, paint the still, add a click-through that opens the source URL in QuickTime Player (which will mostly fail on modern bitrates but at least the page renders). FB users lose autoplay video. The site loads.

This eliminates Phase 8 from being "build a codec stack" to being "intercept video elements and substitute." A few weeks instead of impossible.

---

## Phase plan in dependency order

Sequenced so each phase unblocks the next. Some phases can run in parallel (noted where).

### Phase 0 — Foundation (done or near-done)

- TLS 1.2 / 1.3 ✅ (macTLS / macSSL, v1.2 + post-1.2)
- 121-anchor Mozilla CCADB CA store ✅
- macEntropy v1.0 (production CSPRNG) ✅
- Tier 0 JS bridge wiring NetSurf `js_exec` to Duktape ✅
- Native fetcher architecture (HTTP/1.1 with chunked, keep-alive, redirect, pool) ✅

### Phase 1 — Tier 1 JS globals (4–6 weeks)

Straightforward globals real-site JS expects. Ships one issue per round, mactrove-regression-tested between each per DIRECTIVE #5.

- `console.log` / `.warn` / `.error` / `.info` / `.debug` (#115)
- `navigator.userAgent` / `.appVersion` / `.platform` / `.language` (#120)
- `atob` / `btoa` / `encodeURI` / `decodeURI` / `encodeURIComponent` (#121)
- `alert` / `confirm` / `prompt` modal dialogs (#116)
- `window.location` read + write (#117)
- `window.history.pushState` / `popState` / `back` / `forward` (#118)
- `document.title` getter / setter (#119)
- `setTimeout` / `setInterval` / `clearTimeout` / `clearInterval` (#103, event-loop integration)

**Can run in parallel with Phase 5 once Phase 5 starts.**

### Phase 2 — DOM bindings live (6–8 weeks)

The existing `macsurf_js_dom.c` has function bodies that don't link because the `dom_*` symbols aren't in MacSurf.mcp. The bindings exist on paper; the libdom integration is what's missing.

Tasks:
- Add the relevant libdom symbols (`dom_document_create_element`, `dom_element_set_attribute`, `dom_node_append_child`, `dom_node_unref`, `dom_node_ref`, `dom_string_unref`, `dom_element_get_attribute`, `dom_element_get_tag_name`, `dom_document_get_element_by_id`) to the project file list.
- Wire `document.getElementById` (#29 partial)
- Wire `document.querySelector` / `querySelectorAll` (#29 — basic tag/id/class selectors first, complex combinators later)
- Wire `element.classList.add` / `.remove` / `.toggle` / `.contains` (#30)
- Wire `element.addEventListener` for `click` / `input` / `submit` / `load` / `change` (#31)
- Wire `element.style.<prop>` setters (#32)
- Wire `element.innerHTML` get + set
- Wire `createElement` / `appendChild` / `insertBefore` / `removeChild`
- Wire `setAttribute` / `getAttribute` / `removeAttribute`
- Wire `dispatchEvent`

### Phase 3 — Network APIs (3–4 months)

- **`fetch()` API** (#104) — Promise-based wrapper around the existing `macos9_http_fetcher` / `macos9_https_fetcher` abstraction. **3–4 weeks.**
- **XHR polish** — the existing `macsurf_js_xhr.c` is mostly placeholder. Finish it. **2 weeks.**
- **CORS** — preflight `OPTIONS` request, response header validation (`Access-Control-Allow-Origin`, `-Allow-Methods`, `-Allow-Credentials`), credential-mode handling. NetSurf core has some plumbing already; wire and test. **3–4 weeks.**
- **WebSockets** (#127, currently wontfix; un-wontfix for this work) — RFC 6455 over HTTP/1.1 `Upgrade`. FB uses WebSockets for chat presence, notifications, real-time feed updates. **2–3 weeks** once the H1.1 upgrade flow is in place.
- **HTTP/2 over macTLS** — the big one in this phase. HPACK encoder/decoder, frame parser, stream state machine, flow control, multiplexing dispatch into the existing fetcher slot model. **6–8 weeks.**

### Phase 4 — Storage (6–10 weeks)

- **`localStorage` / `sessionStorage`** (#46) — Preferences-folder-backed (`localStorage` persistent across launches, `sessionStorage` cleared on window close). Per-origin keyed. **2 weeks.**
- **IndexedDB** — transactional async key-value with index queries. Implementation strategy: port a minimal SQLite to CW8 PPC as the backing store, wrap with the IndexedDB JS API. SQLite is famously portable but CW8 C89 strict mode will require an audit pass. **6–8 weeks.**

### Phase 5 — QuickJS engine swap (3–6 months) — THE GATE

Nothing FB-shaped works until this lands. Phase 1 + Phase 2 ship on Duktape and get re-verified on QuickJS after the swap.

Tasks:
- **Port QuickJS to CW8 PPC.** Audit the source for C99 features CW8 chokes on (compound literals, designated initializers, VLAs, `__attribute__`, `// comments`, for-scope decls). Shim `malloc`/`free`/`realloc` integration. Handle PPC big-endian. Wire file I/O if QuickJS expects it. **6–10 weeks** of porting + audit.
- **Replace the NetSurf js_thread API surface.** Re-export `js_initialise`, `js_newheap`, `js_newthread`, `js_exec`, etc. backed by QuickJS contexts instead of Duktape. **2 weeks.**
- **Re-implement registered C functions.** `console.log`, `navigator.*`, `atob`, `btoa`, `fetch`, `setTimeout`, etc. on the QuickJS C API instead of Duktape's. **3–4 weeks.**
- **Re-validate Phase 1 + Phase 2 against QuickJS.** Probe by probe. **3–4 weeks.**
- **Performance tune.** Flip on QuickJS optimization options (`-O2`, inline caching, hidden classes). Measure against Duktape baseline. Optimize hot paths. **2–4 weeks.**
- **Memory footprint validation.** Confirm QuickJS heap + working set fits in the 16 MB Carbon partition or bump the partition (probably required). **1 week.**

Phase 5 is the single highest-risk item on the roadmap. If QuickJS doesn't port cleanly to CW8, the entire FB plan dies and we have to pick a different engine or commit to a port project measured in years.

### Phase 6 — Observers + Web Crypto (3–4 months)

- **Web Crypto (`window.crypto.subtle`)** — wrap BearSSL primitives in the SubtleCrypto API. We have AES-GCM, ChaCha20-Poly1305, SHA-256, SHA-384, SHA-512, ECDH (P-256/384/521), ECDSA, RSA-PSS, HMAC already linked via macTLS. Just need the JS wrapper. **4–6 weeks.**
- **MutationObserver** — libdom mutation event hooks + JS observer queue + microtask integration with the event loop. **3–4 weeks.**
- **IntersectionObserver** — scroll-position + element-bounding-box callbacks + threshold ratios + root margin. **3–4 weeks.**
- **ResizeObserver** — layout-engine resize hooks + JS observer queue. **3–4 weeks.**

### Phase 7 — Service Workers + Cache API (4–6 weeks)

Service workers are designed for separate-thread execution. Cooperative multitasking forces a different shape: a single execution context that runs on navigation events + periodic timer, scheduled into the existing `WaitNextEvent` loop. Not spec-perfect but functionally close.

- Register service worker JS via `navigator.serviceWorker.register()`
- Single SW context per origin, executed in foreground time slices
- Intercept fetcher calls; check the SW's `fetch` event handler first; fall through to network if SW returns nothing
- Cache API backed by an extension of the existing `macos9_disk_cache` infrastructure
- `postMessage` between SW and main context

FB uses service workers for background asset prefetch, offline support, and push-notification reception. We don't need the push side; the prefetch and offline paths are enough.

### Phase 8 — Media substitution (2–4 weeks)

- Detect `<video>` elements in DOM
- Fetch poster frame URL from `<video poster="...">`
- Paint the still as a normal `<img>`
- Add overlay play-button + click handler that opens source URL in QuickTime Player
- `<audio>` elements: render as link to source

Not a real media stack. Renders FB's video posts as silent stills. Acceptable trade-off given the hardware.

### Phase 9 — CSS gaps (4–6 weeks)

- **CSS `:has()` selector** — libcss extension. The selector engine needs a backwards-walking match path. Nontrivial. **3–4 weeks.**
- **Shadow DOM** — libdom needs shadow tree support, style scoping, slot projection. Significant. **6–8 weeks.** (Possibly defer; FB uses Shadow DOM lightly.)
- **Custom Elements** — JS-side registry + libdom integration for element upgrade. **2–3 weeks.**

### Phase 10 — Facebook-specific debugging (the bottomless pit, 6–12 months)

Everything theoretically works at this point. Now we actually load facebook.com and watch what happens. Realistic expectations:

- First few weeks: nothing renders. Diagnose why. Probably JS heap exhaustion or a missing API surface that FB depends on but nobody had thought of.
- Next few months: bits of FB render. Login form might work. News feed might be empty because some fetch path is failing. Diagnose round by round.
- Then: site mostly works but is slow. Profile and optimize.
- Then: FB-specific anti-bot fingerprinting starts blocking us. Diagnose and work around.
- Then: edge cases. Stories. Live video posts. Marketplace. Messenger. Settings pages. Each has its own quirks.

This phase is the actual product. The previous nine were just preconditions.

---

## Total scope honest assessment

**Wall-clock:** 18–30 months of focused 8+ hour/day work, assuming nothing catastrophic.

| Phase | Estimate | Risk |
|---|---|---|
| 0 — Foundation | done | none |
| 1 — Tier 1 JS globals | 4–6 wk | low |
| 2 — DOM bindings | 6–8 wk | low |
| 3 — Network APIs | 3–4 mo | medium (H2) |
| 4 — Storage | 6–10 wk | medium (IndexedDB) |
| 5 — **QuickJS port** | **3–6 mo** | **high** |
| 6 — Crypto + Observers | 3–4 mo | medium |
| 7 — Service Workers | 4–6 wk | medium |
| 8 — Media substitution | 2–4 wk | low |
| 9 — CSS gaps | 4–6 wk | medium |
| 10 — FB debugging | 6–12 mo | unknown |
| **Total** | **18–30 mo** | |

The 16 MB Carbon partition is going to fight us throughout, especially at Phase 5 (QuickJS heap) and Phase 10 (FB's actual memory pressure). Likely need to push to 24–32 MB mid-project. RAM pressure may surface as the actual ship-blocker once everything else is in place.

## When work begins

Phase 1 runs in foreground (small rounds, mactrove-verified, one global per round). Phase 5 (QuickJS port) runs in parallel as background scoping work: pull the source, audit the C99 surface against CW8 C89, get a real number for the port cost.

Once Phase 1 ships, switch foreground to Phase 5. Phases 2–4 follow on Duktape, then get re-validated against QuickJS after Phase 5 lands.

## When work pauses

This roadmap was captured during the v1.2 + Tier 0 JS bridge round. Active work pauses here. Picked back up once MacSurf core has been sharpened to the maintainer's satisfaction. No commitment date.

## Decisions to revisit when work resumes

- **Engine choice.** QuickJS may have evolved or been superseded. Re-evaluate.
- **HTTP/3 priority.** If H3 has become more dominant than H2 by then, the H2-only call may not survive.
- **Hardware floor.** If the target hardware has shifted (e.g. project decides to drop G3 and require G4 only), the QuickJS performance picture changes meaningfully.
- **Anti-bot landscape.** FB's bot detection in 2027+ may be stricter than today's. Login may require capabilities (TLS fingerprinting, WebGL fingerprinting, behavioral analysis) that we can't satisfy regardless of how complete the engine is.

---

*This document is a scoping artifact, not a commitment. It exists so the work is documented if and when the project chooses to take it on.*
