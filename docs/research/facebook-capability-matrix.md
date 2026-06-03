# Facebook capability matrix — what MacSurf has, needs, and won't chase

Honest mapping of the "what a browser needs to run Facebook" matrix against MacSurf's
reality (Classic Mac OS 9 / PPC / CW8 / strict C89 / no threads / ~64 MB RAM / G3-G4).

**The load-bearing insight:** the full matrix describes **desktop facebook.com** — the
416 KB React SPA with video, calls, and games. That target is structurally out of reach
on a 64 MB G3, and **we are not aiming at it.** We target the surface FB serves our UA
(the lightweight `m.facebook.com` / `mbasic` feed: text, images, basic interaction). For
*that* target most of the matrix is **not needed**, and the real critical path is small.

Legend: ✅ have · 🟡 partial/in-progress · 🔨 buildable (with effort) · ⛔ out of reach on this HW · ➖ not needed for our target surface

---

## 1. Networking & Protocols
| Item | Status | Notes |
|---|---|---|
| TLS 1.3 | ✅ | **macTLS** — hand-written TLS 1.3 over BearSSL. Our standout; FB's strict HTTPS works. |
| HTTP/1.1 | ✅ | chunked, keep-alive, redirects, pooling over Open Transport. |
| HTTP/2 | 🔨 | multiplexing over the existing macTLS conn. FB *prefers* it but works over h1.1. **High value (load speed for hundreds of asset requests) — Tier 2.** |
| HTTP/3 (QUIC) | ⛔/➖ | QUIC = UDP + a new transport stack. Big lift, low marginal value over h2. Defer indefinitely. |
| IPv4 | ✅ | Open Transport. FB is dual-stack, IPv4 reaches it. |
| IPv6 | ➖ | `NO_IPV6` set. Not needed — FB has IPv4. |
| WebSockets | 🔨 | HTTP upgrade + frame codec over the TCP/macTLS conn. **Needed for chat/notifications/presence — Tier 2.** No threads needed (poll in the coop loop). |

## 2. Rendering & Layout
| Item | Status | Notes |
|---|---|---|
| HTML5 parsing | ✅ | libhubbub, full HTML5 tokeniser/tree-builder. |
| CSS3 Flexbox/Grid/vars | ✅🟡 | libcss; Flexbox + Grid V1 + `var()` shipped. Documented gaps (some parsed-but-dropped props) but the core is there. |
| Canvas 2D | 🟡 | partial. Feeds don't need it; some widgets do. Tier 3. |
| WebGL / HW-accel canvas | 🟡/⛔ | separate WebGL project exists (software rasteriser, long arc). 3D posts/games/filters are ⛔ on this HW and ➖ for the feed. |

## 3. JavaScript Engine & Runtime — **the critical path**
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
| Cache API / Service Workers | ⛔ | SW needs a persistent background process + Cache API. No background process on OS 9. FB degrades gracefully without it (loses PWA/offline only). Park. |

## 5. Media & Device — **mostly out of reach, mostly not needed**
| Item | Status | Notes |
|---|---|---|
| MSE (adaptive video) | ⛔ | PowerFox needs a 1 GHz G4 + GPU for 360p; a 64 MB G3 can't decode video. ➖ for the feed (text/images). |
| EME (DRM) | ⛔ | Widevine never existed on PPC. |
| WebRTC (Messenger calls) | ⛔ | no media/RTC stack on this HW. |
| Geolocation | 🔨 | trivial stub (return unavailable/denied). Low value, Tier 3. |
| Push API | ⛔ | needs background. Park. |

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

**Out of reach, and we stop chasing them:** video (MSE/EME), calls (WebRTC), Service Workers,
Push, HTTP/3, WebGL-accelerated canvas. None are needed for a readable, usable feed.

### Sequenced roadmap (step by step)
1. **JS→DOM→render** (re-convert after DOM mutation). *In progress.* Unlocks the SPA shell + JS-built feed painting. **Nothing proceeds without it.**
2. **Finish JS API fills as FB hits them** — Intersection Observer (lazy-load/infinite-scroll), IndexedDB shim (GraphQL cache), remaining DOM APIs. Driven by the named-error log, round by round.
3. **Confront the ES6 ceiling** — measure how many FB scripts `SyntaxError`; decide UA-for-ES5 vs a transpile pass vs accept the cap.
4. **HTTP/2** — multiplexing for load speed (hundreds of asset requests). Big but high-value.
5. **WebSockets** — chat / notifications / presence (real-time, no reload).
6. **Polish** — CSP/SameSite correctness, Geolocation stub, Canvas2D as needed.

Parked permanently (HW limits): MSE/EME video, WebRTC, Service Workers, Push, HTTP/3, WebGL games.

> The win already banked: **login works and persists** (c_user/xs). The remaining FB work is
> the *render* problem, which is the same problem as "render any modern SPA" — so every step
> here pays off across a large slice of the web, not just Facebook.
