# Facebook capability matrix — what MacSurf has, needs, and will build

Honest mapping of the "what a browser needs to run Facebook" matrix against MacSurf's
reality (Classic Mac OS 9 / PPC / CW8 / strict C89 / no threads / ~64 MB RAM / G3-G4).

**The load-bearing insight:** the full matrix describes **desktop facebook.com** — the
416 KB React SPA with video, calls, and games. That target is the heaviest climb
on a 64 MB G3, — so we **start** below it and climb. We begin with the surface FB serves our UA (the
lightweight `m.facebook.com` / `mbasic` feed: text, images, basic interaction), which needs only
a small slice of the matrix; then we build outward through the heavy tier until the full SPA runs
on capable hardware. Start light, climb to everything.

**Stance (per the maintainer): scope and attempt EVERYTHING. Aim for perfection.** A capability
being too heavy for a 64 MB G3 is not a reason to skip it — it's a reason to *gate it on
hardware*. A G4 with more RAM is a real target; faster machines benefit; and a G3 user simply
avoids the pages that need the heavy bits. So nothing below is "abandoned" — the ⛔ tier means
"heavy / needs more capable hardware," and it gets scoped and attempted like everything else.

Legend: ✅ have · 🟡 partial/in-progress · 🔨 buildable (with effort) · ⛔ heavy — needs G4+/faster HW (still scoped + attempted; G3 users skip those sites) · ➖ lower priority for the feed (still scoped)

---

## 1. Networking & Protocols
| Item | Status | Notes |
|---|---|---|
| TLS 1.3 | ✅ | **macTLS** — hand-written TLS 1.3 over BearSSL. Our standout; FB's strict HTTPS works. |
| HTTP/1.1 | ✅ | chunked, keep-alive, redirects, pooling over Open Transport. |
| HTTP/2 | 🔨 | multiplexing over the existing macTLS conn. FB *prefers* it but works over h1.1. **High value (load speed for hundreds of asset requests) — Tier 2.** |
| HTTP/3 (QUIC) | ⛔ | QUIC = UDP + a new transport stack over Open Transport. Deepest networking lift, but TLS 1.3 (which QUIC needs) is already ours via macTLS. Scoped, roadmap step 12. |
| IPv4 | ✅ | Open Transport. FB is dual-stack, IPv4 reaches it. |
| IPv6 | ➖ | `NO_IPV6` set. Not needed — FB has IPv4. |
| WebSockets | 🔨 | HTTP upgrade + frame codec over the TCP/macTLS conn. **Needed for chat/notifications/presence — Tier 2.** No threads needed (poll in the coop loop). |

## 2. Rendering & Layout
| Item | Status | Notes |
|---|---|---|
| HTML5 parsing | ✅ | libhubbub, full HTML5 tokeniser/tree-builder. |
| CSS3 Flexbox/Grid/vars | ✅🟡 | libcss; Flexbox + Grid V1 + `var()` shipped. Documented gaps (some parsed-but-dropped props) but the core is there. |
| Canvas 2D | 🟡 | partial. Feeds don't need it; some widgets do. Tier 3. |
| WebGL / HW-accel canvas | 🟡 | the `webgl/` project (software rasteriser + GLSL ES walker) is already underway — Khronos hello-triangle on a G3 is the milestone, richer scenes climb the HW tier. Roadmap step 9. |

## 3. JavaScript Engine & Runtime — **the critical path (start here)**
| Item | Status | Notes |
|---|---|---|
| ES5 + broad runtime | ✅ | Duktape 2.7 + the fixes319-380 marathon: timers, URL, classList, Event ctors, fetch, localStorage, Set/Map/Array.from, Promise(+all), the FB `__d`/`requireLazy` module loader, performance API. |
| **JS→DOM→render** | 🟡 **IN PROGRESS** | **THE current blocker.** JS mutates the real libdom, but mutations don't trigger a box-tree rebuild, so JS-built pages don't paint. Re-convert-after-mutation design is being researched now. **Tier 1 — everything depends on this.** |
| ES6+ syntax (arrow fns, classes, template literals, destructuring, async/await, modules) | 🟡/⛔ | Duktape parses ES5 + *some* ES2015 (let/const, Proxy, Symbol) but **NOT** arrow functions / classes / template literals → the `SyntaxError: parse error` cases. This is the **hard ceiling**: code FB ships in modern syntax can't even parse. Levers: (a) UA that makes FB serve more ES5; (b) an on-the-fly ES6→ES5 transpile pass (large); (c) keep filling what *does* parse. **Tier 3, decision needed.** |
| Intersection Observer | 🔨 | **Needed** for lazy-load + infinite scroll. Shimmable (report visible / wire to scroll). Tier 2. |
| Web Workers | 🟡/⛔ | no OS-9 threads → no *real* workers. Could run worker code synchronously on the main thread (many sites tolerate it). Tier 3, best-effort. |

## 4. Storage & Caching
| Item | Status | Notes |
|---|---|---|
| localStorage / sessionStorage | ✅ | shipped. |
| IndexedDB | 🔨 | FB caches GraphQL responses here. Shimmable (in-memory, or backed by our disk cache). Tier 2 — keeps FB's JS from erroring + speeds it up. |
| Cache API / Service Workers | ⛔ | No OS-9 background process — so synthesize an SW scope serviced from the event loop, with our disk cache as the Cache API backend. Hardest “no background” problem; scoped, roadmap step 7. |

## 5. Media & Device — **the heavy tier (hardware-gated, all scoped)**
| Item | Status | Notes |
|---|---|---|
| MSE (adaptive video) | ⛔ | Adaptive-bitrate plumbing + a software decoder. PowerFox proves software H.264 runs on PPC (G4/G5 class); slow-to-none on a 64 MB G3, real on a fast G4/G5. Scope the decoder, start at the lowest rung. Roadmap step 10. |
| EME (DRM) | ⛔ | No Widevine on PPC, so target ClearKey/clear-content EME (the spec path that needs no proprietary CDM) + scope what DRM is reachable. Roadmap step 13. |
| WebRTC (Messenger calls) | ⛔ | signaling over WebSocket/HTTP + a media path; audio first (lighter than video). The calls frontier — scoped, roadmap step 11. |
| Geolocation | 🔨 | trivial stub (return unavailable/denied). Low value, Tier 3. |
| Push API | ⛔ | Synthetic push serviced from the event loop (poll/long-poll a notifications endpoint, dispatch Push events). Roadmap step 13. |

## 6. Security
| Item | Status | Notes |
|---|---|---|
| CORS | ✅ (n/a) | CORS is a *browser-enforced restriction* to protect users. We don't enforce it → cross-subdomain/CDN assets (scontent.fbcdn.net) load freely. **Not a blocker.** |
| CSP | ➖ | we don't enforce CSP. Ignoring it is permissive, not blocking — it doesn't stop rendering. Could add later for correctness. |
| SameSite cookies | ✅🟡 | urldb jar stores/sends cookies (c_user/xs persist, login works). SameSite *enforcement* is minor; sending behaviour is correct. |

---

## What this means — the real plan

**Already solid:** the transport (TLS 1.3, HTTP/1.1), the parser (HTML5), the style engine
(CSS3), cookies/session. The "security" row is a non-issue (those features *restrict*; not
enforcing them is permissive). **None of these is the blocker.**

**The blocker is one thing:** **JS→DOM→render** (Tier 1, in progress). FB's feed is built
by JS; until JS mutations paint, nothing else matters. This is being designed now.

**The ceiling is one thing:** **ES6 syntax** that Duktape can't parse. This caps how much of
FB's *modern* JS runs. We'll know its real height once JS→DOM→render lands and we see how far
FB's (KaiOS-surface) JS — which targets old phones and may already be ES5-ish — actually gets.

**The heavy tier — built, not parked.** Video (MSE/EME), calls (WebRTC), Service Workers,
Push, HTTP/3, WebGL: these are **hardware-gated, not abandoned.** Precedent: native TLS on
Classic Mac OS was "impossible" — macTLS shipped it, then shipped **TLS 1.3**. Every one of
these gets a real scope and a real attempt. If a build is slow on a 64 MB G3, it's slow — and
it'll be usable on a G4 / faster machine, and a G3 user simply avoids the pages that need it.
**Nothing is parked.**

### Sequenced roadmap — everything, in attempt order (impact × feasibility first)
Earlier = higher impact and/or lower effort. Later ≠ optional; later = heavier, so it follows
the dependencies and the hardware climbs. All of it ships eventually.

1. **JS→DOM→render** (re-convert after DOM mutation). *In progress.* Unlocks every JS-built page. Foundation — everything visual depends on it.
2. **JS API fills, log-driven** — IntersectionObserver (lazy-load/infinite-scroll), MutationObserver, IndexedDB (GraphQL cache), remaining DOM APIs. Round by round off the named-error log.
3. **ES6 reach** — measure the `SyntaxError` surface, then *raise the ceiling*: an on-device or pre-fetch ES6→ES5 transpile pass so modern syntax (arrow fns, classes, template literals) runs. (Or UA-for-ES5 as the interim.) The "impossible" item that makes the rest tractable.
4. **HTTP/2** — multiplexing over the macTLS conn; hundreds of asset requests in flight. Big load-speed win.
5. **WebSockets** — chat / notifications / presence / live reactions, real-time, no reload. HTTP-upgrade + frame codec over OT+macTLS, polled in the coop loop.
6. **Web Workers** — no OS-9 threads, so run worker scripts on the main loop (cooperative, synchronous-ish). Many sites tolerate it.
7. **Service Workers + Cache API** — the hardest "no background process" problem; approach: a synthetic SW scope serviced from the event loop + our disk cache as the Cache API backend. Scope it, attempt it.
8. **Canvas 2D** — software raster (we already plot in QuickDraw); a real CanvasRenderingContext2D over offscreen GWorlds.
9. **WebGL** — the existing `webgl/` project (software rasteriser + GLSL ES walker). Khronos hello-triangle on a G3 is the milestone; richer scenes climb the HW tier.
10. **MSE + software video** — adaptive-bitrate plumbing + a software H.264/VP-something decoder. Slow on a G3, real on a fast G4/G5 (PowerFox proves software H.264 is possible on PPC). Scope the decoder, start with stills/lowest-rung.
11. **WebRTC** — signaling (WebSocket/HTTP) + the media path; audio first (lighter than video). The Messenger-calls frontier.
12. **HTTP/3 / QUIC** — UDP transport + QUIC over Open Transport; the deepest networking lift, but TLS 1.3 (which QUIC needs) is already ours via macTLS.
13. **EME, Push, Geolocation, CSP/SameSite correctness** — DRM, background push (synthetic), device APIs, security-header fidelity.

> Precedent that settles the argument: **macTLS.** Native TLS 1.3 on a 233 MHz G3 was supposed
> to be impossible. It isn't. None of the above is either — it's a matter of order and effort.
> The win already banked: **login works and persists** (c_user/xs). And because "render a modern
> SPA" is the same problem for every site, every step here unlocks a large slice of the web — not
> just Facebook. **We build all of it.**
