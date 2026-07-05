# ⚠️ READ THIS FIRST: WORKING TREE ≠ HEAD (provenance) + PER-COMPONENT G3 STATUS (2026-06-30)

**The committed tree (HEAD) and the working tree are two different projects. Reason about the WORKING TREE — that is what the user builds and ships. HEAD is far behind it.**

- **HEAD = `f305816c` — the QuickJS migration commit itself** (titled "QuickJS engine migration + fixes482–554"). It already contains the engine swap. The *pre-migration Duktape baseline* is `664273a4` (fixes481); that is the tree to diff against to see what the migration changed.
- **Working tree ≈ fixes560+.** It is HEAD (`f305816c`) plus uncommitted fixes555+ (parser-resume "chase" layer, cache-hit PAUSED fix, fixes471 one-column restore, on-screen fetch readout + profiling, cascade-dedup attempt). Per DIRECTIVE #4 the diffs are held uncommitted until hardware-confirmed — but uncommitted does **NOT** mean unrun: see the per-component G3 status below.
- **Two diff spans matter and must not be conflated:** `664273a4 → f305816c` is the *regression-introducing* span (engine swap + fetcher replacement + content registry + parser-resume rewrite + ns_content rename — where Failure A and Failure B were born). `f305816c → working tree` is the *attempted-fix* span (reactions to those crashes, not their source).
- Anything below describing "current state" describes the working tree unless it says HEAD. Do not assume `git log` reflects what the user is running — it does not.

**What changed in the working tree that this file used to get wrong (now corrected below):**
- **JS engine is QuickJS, not Duktape.** See [JavaScript Engine](#javascript-engine).
- **HTTPS fetcher renamed** `macos9_https_fetcher.c` → `macos9_tls_fetcher.c` (untracked).
- **Untracked working-tree files** not yet in HEAD: `macos9_tls_fetcher.c`, `macos9_content_registry.c` / `.h`, `javascript/macsurf_qjs.c` / `.h`, `browser/libquickjs/`. The old `content.c` has been **renamed to `ns_content.c`** in the worktree (old `content.c` shows as deleted).

## 🟢🟡🔴 G3 Hardware Verification Status (2026-06-30)

**Process note: every fix this session is tested on real G3 hardware. Status below is PER-COMPONENT — do NOT assume tree-wide non-verification.** The old blanket "hardware-UNVERIFIED" banner was accurate only while the migration was a fresh untouched pile; it is now false for the load path. Treat each component by its bucket, not by the fact that the diff is uncommitted.

### ✅ VERIFIED ON G3 (ran correctly on real hardware)
- **fixes556 — cache-hit navigation load.** Confirmed: nav goes straight to `NAV: DONE url=https://…`, no `NAV: ERROR code=0`, no `about:query`/`about:fetcherror`, no URL-bar re-enter needed. The bug where every cache-hit nav errored to the placeholder is fixed.
- **fixes560 — on-screen diagnostics.** The `active fetches: N` title-bar readout, `html_reformat #1/#2/#3` sequencing, and profile stamps (`[+NNNNus] LABEL`) all confirmed working on G3.
- **fixes559 — fixes471 one-column restore.** `lh__box_is_absolute` again excludes `CSS_POSITION_STICKY` from flex space allocation. Confirmed by the user this session: the 68kmla front page renders multi-column ("the home page IS loading properly"). *(This corrects the earlier triage that listed it as still-open — it is fixed and confirmed.)*
- **Per-URL terminal-fail flag (fixes554).** Confirmed firing on G3: `TERMINAL FAIL` once per URL, then `terminal-URL FAST-FAIL`, no storm.
- **Box-walk pin guard (hlcache-evict path).** Held through at least one clean full-page load to event-loop-exit with a bulk clean landing *post*-walk. (Covers the hlcache evict path only — see the crashing bucket for the gap.)

### 🟡 SHIPPED, PENDING G3 CONFIRMATION (code on hardware, specific catch not yet observed)
- **Fix A / parser-lifetime token** (`deferred_parser_unpause` ABA guard). The `deferred_unpause: ENTRY` instrumentation fires on G3 and the happy path (token match, `savedtok==curtok`) works — but a token **MISMATCH has never been observed**, so the ABA bail itself is UNPROVEN. Needs a run where `savedtok != curtok` to confirm `PARSER TOKEN INVALID` actually catches. Note: the token is currently keyed on `parent->base`; investigation indicated the freed lifetime is `parent->parser`, so a re-key may still be needed.
- **fixes561/562 — CSS cascade-dedup.** Shipped (URL-keyed dedup in `html_css_process_link`) but the G3 log shows it **not yet effective**: the 239 KB sheet still cascades twice (~4.8s pre-paint waste), zero `css dedup` hits. fixes562 adds an unconditional `css link scan: count=… withsheet=… matched=…` diagnostic to pin down why the match fails on the next run. Treat as in-diagnosis, not landed.

### 🔴 ACTIVELY CRASHING ON G3 (open, reproduced on hardware)
**Box-walk UAF via the LLCACHE user-destroy path.** Chain: `convert_xml_to_box` → `box_construct_element` → `convert_special_elements` → `_dom_html_element_get_attribute` → `box_image` → `box_image_resolve_url` → `html_fetch_object` → `hlcache_handle_retrieve` (faults at `lbzu r0,0x0001(r4)`, r4 wild). The free comes from `llcache_object_user_destroy` → `llcache_object_notify_users` → `macos9_handle_update` (scheduler) firing **mid-walk** — NOT a bulk `hlcache_clean` eviction this time. Reproduces RIGHT AT STARTUP on the first home-page load (cache-hit home delivered in one synchronous burst; the first `convert_xml_to_box` descends while the initial content's llcache user bookkeeping is still settling) AND on heavy pages.
- **Coverage gap:** the pin guard `macos9_box_walk_owns_content` is consulted ONLY at [ns_content.c content_destroy](browser/netsurf/content/ns_content.c) and [hlcache.c hlcache_clean](browser/netsurf/content/hlcache.c) — `llcache.c` never consults it. The llcache-level user-destroy path is uncovered.
- **Fix in progress:** extend the pin/registry-generation-token check DOWN to the `llcache_object_user_destroy` path (defer the destroy when the owning content is pinned by an active box walk, gen-validated for ABA), and ensure the pin is armed before the first descent on the startup deferred-nav path. Same registry mechanism as the existing pin — one liveness model, one level lower. Log the catch (`llcache: USER-DESTROY DEFERRED (walk live)`). **Not fixed — diagnosis confirmed, fix not yet shipped.**
- The fixes554 per-URL terminal-fail set (see Networking) was the storm-collapse that *manufactured* less of the cache pressure behind the older eviction-path variant; it does not address this llcache-path variant.

### ⚪ OPEN / NON-CRASH (triaged this session, not yet fixed)
- **fixes515 deferred-broadcast FIFO** — same scheduled-work-outlives-object pattern, guarded only by a weak `handler==NULL` sentinel rather than the registry generation token. Latent; audit/harden pending (should adopt the same registry-token mechanism as the box-walk pin).
- **Perf — CSS cascade runs twice** (~4.8s pre-paint waste). Cascade-dedup gate shipped but not yet effective (see SHIPPED/PENDING bucket).
- **Perf — oversized images.** A 3024×2507 source JPEG; decode is already deferred + display-sized (fixes162), so the cost on this load was the ~50s network transfer of a 3.6 MB file (post-first-paint), not a 22 MB RGB buffer (this Mac had ~280 MB free). Lever is lazy/deferred off-screen image fetching, a fetcher change — held. Possible duplicate large-image fetch (same byte count twice) flagged for investigation.
- **hiddenscroll "removeChild of null" JS error** on every page — non-fatal, waits on the phase-one DOM wrapper port.

# DIRECTIVE #1, NEVER BLAME STALE FILES, EVER

This is a repeating, lazy failure mode. You are NOT ALLOWED to consider, suggest, hint, or imply that:
- the Mac copy of any file is stale, out-of-date, or wasn't extracted from the zip
- CW8 has cached a precompiled prefix or stale .o files
- the user's rebuild was incomplete, missed a file, or didn't pick up changes
- the workflow's transfer step (zip → scp → unpack → CW8 rebuild) failed silently

The user's build IS what they say it is. If the symptoms suggest staleness, the answer is **always** to research the issue THROUGH THE CODE on Linux and find a real root cause, a missing #include, a wrong access path, a header collision, a guard mismatch, an autogenerated file's content, etc., and ship a code-side fix.

If you genuinely believe a previously shipped file may need to be reshipped because the symptoms continued, just include it in the next zip without commentary about why. Do NOT narrate the assumption. Do NOT lecture. Just ship the file.

When shipping a fix, the deliverable is:
1. The updated files, zipped with full `browser/netsurf/...` tree preserved
2. A list of files (if any) the user should add to or remove from MacSurf.mcp
3. A list of paths (if any) the user should add to or remove from Access Paths.xml

Nothing else. No "and please verify the Mac unpack worked." No "if this still fails, check whether CW8 picked it up." No staleness theories.

---

# MacSurf

A lightweight web browser for Mac OS 9 PowerPC, built on the NetSurf engine, with native TLS (macTLS) for direct HTTPS — no proxy.

> **Maintenance:** this file falls out of date fast. Update protocol at the bottom under [CLAUDE.md Maintenance](#claudemd-maintenance).

## Project Structure

```
macsurf/
├── browser/          # NetSurf fork with macos9 frontend (native macTLS HTTPS built in)
├── macTLS/           # Native TLS 1.3 stack for Classic Mac OS (BearSSL-based; nested project)
├── proxy/            # Legacy Go TLS proxy — RETIRED, no longer on the path (kept for history)
└── docs/             # Build and deployment docs
```

## Components

### 1. MacSurf Browser
A port of NetSurf to Classic Mac OS 9 using the Carbon API and CodeWarrior 8. Cross-compiled from Linux targeting PowerPC. Tabs disabled by default. Fetches HTTP directly over Open Transport and HTTPS directly via the built-in macTLS stack — no proxy involved.

### 2. macTLS (native TLS)
A native TLS stack for Classic Mac OS, built on BearSSL primitives and linked into the browser. Hand-written TLS 1.3 (RFC 8446; X25519 + multi-curve ECDHE, ChaCha20-Poly1305 / AES-128-GCM) with TLS 1.2 fallback, the full Mozilla CA bundle (121 anchors) baked in, and the macEntropy RNG behind it. This is how the Mac reaches HTTPS sites itself. Lives in `macTLS/` (a nested project folded into the browser build). First native TLS 1.3 on Classic Mac OS (v1.3, 2026-05-29).

### (Retired) MacSurf Proxy
The original design used a single Go binary in `proxy/` that stripped TLS for the Mac. **It has been retired** — native macTLS replaced it around 2026-05-25 and `use_proxy` is pinned off. The directory and a few dead defines remain pending cleanup. Do not reintroduce a proxy dependency or document one as current.

## Key Technical Constraints

- Development environment: Mac OS 9.2.2 on a G3 iMac. All verified-working results come from this machine.
- Community compatibility target: Mac OS 9.2.2 on a Power Mac G4. Most-common active OS 9 setup today; **font rendering verified clean on 9.2.2 G4 at fixes67 (2026-05-15)**, outline + AA path produces smooth, well-spaced text.
- **Known visual delta between 9.1 and 9.2.2:** font rendering is noticeably rougher on 9.1 than on 9.2.2 even with identical binary and identical `SetOutlinePreferred(true)` / `SetAntiAliasedTextEnabled(true, 8)` calls (the fixes51 settings). QuickDraw's AA path was significantly improved in 9.2, 9.1 implements `SetAntiAliasedTextEnabled` weakly above 8pt. This is an OS-side limitation, not a MacSurf bug; the same code emits noticeably crisper text on 9.2.2. When working on the dev 9.1 G3 and font output looks rough, don't chase it as a MacSurf regression, confirm on a 9.2.2 machine first.
- Broader target range: Power Mac G3/G4, Mac OS 9.1-9.2.2, minimum 64MB RAM
- Compiler: CodeWarrior 8 (on-machine) or cross-compile GCC PPC from Linux
- No threading, OS 9 is cooperative multitasking, use WaitNextEvent loop
- HTTPS handled natively in the browser by macTLS (TLS 1.3, TLS 1.2 fallback) over Open Transport — no proxy
- JavaScript runs **on-device** via **QuickJS** (ES2023, working tree; HEAD still builds Duktape — see banner), linked into the build and gated by `WITH_QUICKJS`. QuickJS replaced Duktape so modern JS runs natively, and the in-house ES6→ES5 transpiler was retired (fixes522). Heavy SPA frameworks and very large DOM-mutation apps are the open frontier (tracked in issues). There is **no proxy and no JS offload** — what runs, runs on the Mac, and gaps get filled in-house (see DIRECTIVE #2). Full detail in [JavaScript Engine](#javascript-engine).
- Carbon API for UI, works on OS 9 and early OS X

## Coding Conventions

- C for everything that ships to the Mac: the browser frontend (matches the NetSurf codebase) and macTLS (C89 for CW8; macTLS keeps its own conventions)
- Keep Mac Toolbox calls isolated in their own files (window.c, bitmap.c, font.c etc.)
- (The retired `proxy/` was Go, stdlib-only — historical, not part of the shipped product.)

## Carbon App Requirements

MacSurf is a Carbon CFM app running under CarbonLib on OS 9. For CarbonLib to fully engage, the binary MUST be identifiable as a Carbon fragment, otherwise `*InContext` calls crash at fixed addresses inside OTClientLib.

- **`'carb'` resource is mandatory.** Without it, CFM treats the binary as classic PEF, CarbonLib does not load as a dependency, and any `*InContext` OT call enters an uninitialized CarbonLib client context and crashes. This is the single most important requirement for a Carbon app on OS 9.
- **`MacSurf.rsrc`** contains the `'carb'` loader marker (ID 0) plus the application's icon family (`ICN#` / `icl4` / `icl8` / `ics#` / `ics4` / `ics8` at ID 128), `FREF` 128 (file type `'APPL'`), and `BNDL` 128 (creator `'MPLS'`, mapping icon local-ID 0 → `ICN#` 128 and FREF local-ID 0 → `FREF` 128). Pre-compiled binary resource fork generated on Linux by [tools/png_to_mac_icon_rez.py](tools/png_to_mac_icon_rez.py) from the source artwork at [puffpuff.png](puffpuff.png); the script also emits [browser/netsurf/frontends/macos9/MacSurfIcon.r](browser/netsurf/frontends/macos9/MacSurfIcon.r), which [browser/netsurf/frontends/macos9/MacSurf.r](browser/netsurf/frontends/macos9/MacSurf.r) `#include`s so the Rez source matches the binary fork byte-for-byte. CW8 links `.rsrc` files directly into the output resource fork with no Rez step; the `.r` source is the canonical record. Must be listed in the CW8 project alongside the `.c` files. **Creator code is uppercase `'MPLS'`** — Classic Mac type/creator codes are case-sensitive. See [docs/resources.md](docs/resources.md) for the full pipeline.
- **`RegisterAppearanceClient()`** must be called at startup after `InitCursor()`, gated by a Gestalt check for Appearance Manager presence. Matches Classilla's `CBrowserApp` constructor pattern.
- **Skip** `InitGraf`/`InitFonts`/`InitWindows`/`InitMenus`/`TEInit`/`InitDialogs` under Carbon, Classilla explicitly skips them and so should MacSurf. Keep `InitCursor()` and `FlushEvents(everyEvent, 0)`.
- **No preemptive threads.** OS 9 is cooperative. Use `WaitNextEvent` for the UI event loop. OT yields happen through the notifier callback (see below).

## Open Transport Rules

MacSurf currently uses **`*InContext` Open Transport calls** in the shipping browser code. The Carbon app initializes an OT client context in `main.c` and the fetchers open endpoints through that context.

- Use `InitOpenTransportInContext(kInitOTForApplicationMask, &macos9_ot_context)` at startup and `OTOpenEndpointInContext(..., macos9_ot_context)` in fetch paths. This matches the code in `main.c`, `macos9_http_fetcher.c`, `macos9_ns_fetcher.c`, and `macos9_fetch.c`.
- Use `OTUseSyncIdleEvents(ep, true)` plus a notifier that calls `YieldToAnyThread()` on `kOTSyncIdleEvent`. This is the cooperative-multitasking answer for synchronous OT calls, OT fires `kOTSyncIdleEvent` periodically while blocked, the notifier yields to the Thread Manager, and the app stays responsive without touching `WaitNextEvent` from inside the fetch.
- Use `OTInitDNSAddress(&dnsAddr, "host:port")` for address setup, one string, OT resolves hostname and port. Simpler than `OTInetStringToHost` + `OTInitInetAddress`.
- `OTBind(ep, NULL, NULL)` is legal and correct. No TBind ret buffer needed for outbound-only TCP clients.
- Include `<Threads.h>`, the classic Thread Manager is required for `YieldToAnyThread`.
- Reference implementations: [cy384/ssheven](https://github.com/cy384/ssheven) (production SSH client) and [cy384/miscellany retro68-demos/ot-tcp-demo.c](https://github.com/cy384/miscellany) (Apple `OTSimpleDownloadHTTP.c` adapted for Retro68) are still useful OT references, but MacSurf's current Carbon build does not mirror their plain-OT initialization exactly.

## Prior Art

- **MacSurf appears to be the first serious NetSurf port to Classic Mac OS.** The netsurf-dev list has a single 2017 "Port to OS9?" thread with no follow-through. There is no prior NetSurf OS 9 port to reference.
- **Best networking references:**
  - [Classilla](https://sourceforge.net/projects/classilla/), `macsockotpt.c` (NSPR's OT sockets layer) and `directory/c-sdk/ldap/libraries/macintosh/tcp-univhdrs/tcp.c` (standalone TCP over OT). Full Mozilla-era Carbon browser running on OS 9.
  - [cy384/ssheven](https://github.com/cy384/ssheven), modern production SSH client, cooperative thread + OT.
  - [cy384/miscellany `retro68-demos/ot-tcp-demo.c`](https://github.com/cy384/miscellany), shortest known-good OT HTTP client, ~220 lines.
- **Not references:** iCab (closed source), WaMCom (Classilla predecessor, same codebase), MoonlightOS (does not exist as far as we can find).

## Reference Frontends

NetSurf's RISC OS and AmigaOS frontends are the primary references for frontend architecture, both solved cooperative multitasking on non-POSIX systems. Study these before writing any frontend code.

- `frontends/riscos/`, closest analog to Mac OS 9
- `frontends/amiga/`, also cooperative multitasking

## Networking (native, no proxy)

The browser fetches directly: HTTP/1.1 over Open Transport (chunked transfer, keep-alive, 3xx redirect follow, connection pooling) and HTTPS over the built-in macTLS stack (TLS 1.3, TLS 1.2 fallback). Origin connections are made straight from the Mac — there is no proxy and no custom protocol. (The legacy Go proxy referenced in older notes is retired; see Components.) The HTTPS fetcher lives in `macos9_tls_fetcher.c` (renamed from `macos9_https_fetcher.c`, untracked).

### Per-URL terminal-fail set (fixes554)

`macos9_tls_fetcher.c` carries **two** fast-fail tiers, and they are distinct:
- **Per-HOST `dead_hosts` list** — fast-fails a whole host:port for the session (e.g. `fonts.googleapis.com` fingerprint-blocking). FIFO, so a host can age out and be retried.
- **Per-URL `terminal_urls` set ([macos9_tls_fetcher.c:535-575](browser/netsurf/frontends/macos9/macos9_tls_fetcher.c#L535))** — keyed on the **full URL string** (`strcmp` over `nsurl_access(c->url)`), marks an individual resource URL terminally failed on its first dead-host fast-fail. A terminal URL renders alt text and is **never retried**: no http scheme-fallback, no 301 follow, no re-queue. It is checked *before* the per-host list so it survives `dead_hosts` FIFO eviction. This collapses the `cdn.jsdelivr.net` emoji/avatar storm (dozens of distinct URLs each looping fast-fail → http → 301 → fast-fail) to **one `FETCH_ERROR` per URL** — which is what removed the cache-pressure that was triggering the convert_xml_to_box UAF (see the Active Crash note at top). Log lines: `terminal-URL FAST-FAIL %s` and `resource: TERMINAL FAIL url=%s`. HTTP-side wrapper: `macos9_https_url_is_terminal()`.

### Cookies + per-host User-Agent (fixes367, #167)

Both macos9 fetchers now wire NetSurf's cookie jar and select the User-Agent per host. This is the enabling work for **Facebook compatibility** (see below).

- **Cookie jar.** NetSurf's full RFC-6265 jar is `content/urldb.c` (in `MacSurf.mcp`), but upstream only ever wired it in `content/fetchers/curl.c`, which we don't build. So before fixes367 the macos9 HTTP/HTTPS fetchers sent **no `Cookie:` header and stored no `Set-Cookie:`** — login on any site was impossible. fixes367 adds, to both `macos9_http_fetcher.c` and `macos9_tls_fetcher.c` (the HTTPS fetcher, renamed from `macos9_https_fetcher.c` in the worktree): a `Cookie:` request header from `urldb_get_cookie(url, true)`, and `Set-Cookie:` capture via `fetch_set_cookie(parent, value)` **inside the header-parse loop** (so a login POST's 302 stores `c_user`/`xs` before the redirect tears the fetch down). Request buffers were enlarged (https `1024→8192`, http `2048→8192`) to hold a full session cookie header. Cookies are **in-memory only** for now (urldb, inited by `netsurf_init`); disk persistence (`urldb_save/load_cookies`) is the next, hardware-gated step.
- **Per-host UA.** `macos9_ua_for_host()` (a `static` duplicated in each fetcher — TODO: unify into `macos9_useragent.c`) suffix-matches `facebook.com` → vintage `Mozilla/4.0 (compatible; MSIE 5.0; Mac_PowerPC) MacSurf/1.4`; **every other host keeps the MacSurf default UA**. This is the Classilla `sitecontrol` / TenFourFox per-site-override pattern. The vintage UA is mandatory because Facebook **301-bounces a modern/MacSurf UA off the lightweight surface** to the 416 KB www SPA. The match guards spoof hosts (`evilfacebook.com` → default UA).

### Facebook (lightweight no-JS mbasic path) — ACTIVE

Strategy (2026-06-03, issue #167): target `mbasic.facebook.com`, Facebook's pure-HTML, **no-JavaScript**, feature-phone surface (~6 KB/page, tables + forms). It is *native* (the Mac is the real client, so auth/cookies live on the client — what FB requires; a proxy could never do this) and renders on the current engine — **no QuickJS, no proxy, no HTTP/2**. Ground truth: [docs/research/facebook-mbasic-scope.md](docs/research/facebook-mbasic-scope.md). The heavy full-SPA plan ([facebook-native-roadmap.md](docs/research/facebook-native-roadmap.md), QuickJS + H2, 18–30 mo) is the long-term north star, deferred.

Login flow (all pure HTML, handled by core `form.c` + fixes367 cookies + fixes312 POST): GET mbasic (vintage UA) → `200`, sets `datr` → user submits `email`/`pass` form (hidden `lsd`/`jazoest`/`m_ts` collected by core) → **POST** `/login/device-based/regular/login/` with `datr` attached → **302** sets `c_user`+`xs` (captured) → redirect GET sends them → logged in. `fb_dtsg` (post-login page) needed only for write actions. **Regression watch (DIRECTIVE #5):** mactrove must keep the default UA and render unchanged.

**fixes368** built out the software side to completeness: (a) **cookie disk persistence** (`macos9_cookies_load`/`_save` in `macos9_disk_cache.c`, wired in `main.c`) so a login survives relaunch — hardware-unverified MSL fopen path, no-op on failure; (b) **UA site-control module** — the duplicated per-host UA statics are unified into `macos9_user_agent_for_host()` (`macos9_fetch.c` + new header `macos9_useragent.h`), a one-row-per-site override table; (c) **meta-refresh verified** — FB checkpoint pages auto-redirect via core `CONTENT_MSG_REFRESH` + the macos9 scheduler, no new code. **Next:** hardware login bring-up on the G3 (the whole chain is unverified on real hardware — that's its smoke test).

## Do Not

- Do not assume JS capability limits (DIRECTIVE #2). Duktape is ES5 but capable, and a large browser runtime already runs on-device; missing pieces get filled in-house, not waved off. There is no proxy/offload to fall back on.
- Do not enable tabs by default
- Do not use preemptive threads anywhere in the browser
- Do not reintroduce a proxy. HTTPS is native via macTLS; the old Go proxy is retired (see Components). Do not add a proxy dependency or document one as current.
- Do not target OS X only, Carbon must run on OS 9

## Build Environment

### Compiler
- CodeWarrior 8 Pro (with 8.3 update) running on Mac OS 9
- CW8 compiles in C89 mode, no C99, no C++ features
- CW8 defines `__MWERKS__`, use this to detect the compiler
- The project defines `__MACOS9__ 1` via the prefix file `macsurf_prefix.h`
- CW8 does NOT support: `inline`, `//` comments, variadic macros, forward enum declarations, C99 designated initializers, `for (int i...)`

### Prefix File
`browser/netsurf/frontends/macos9/macsurf_prefix.h` is injected before every compilation unit. It currently defines:
- `__MACOS9__ 1`
- `NO_IPV6 1`
- `TARGET_API_MAC_CARBON 1`
- `#include <MacTypes.h>` (first line, must stay first to prevent bool/true/false conflict)

`WITHOUT_DUKTAPE` is **no longer defined**, Duktape is linked into the base build. See [JavaScript Engine](#javascript-engine) below.

### Shims Layer
POSIX functionality is provided by stubs in `browser/netsurf/frontends/macos9/shims/`. These must be C89 compatible. Mac Toolbox headers must always be included before any bool/true/false definitions.

### Stub Headers
External dependencies not available on OS 9 are stubbed in `browser/netsurf/frontends/macos9/`:
- `libwapcaplet/libwapcaplet.h`
- `dom/dom.h`
- `libcss/libcss.h`
- `nsutils/endian.h`, `nsutils/time.h`, `nsutils/base64.h`, `nsutils/unistd.h`
- `sys/time.h`, `sys/types.h`
- `shims/iconv.h`, `shims/zlib.h`, `shims/stat.h`
- `css/utils.h`
- `parserutils/charset/utf8.h`

### Access Paths (CodeWarrior)
All non-recursive. User paths:
- `{Project}::patrick:macsurf-source Folder:`
- `{Project}::patrick:macsurf-source Folder:browser:netsurf:`
- `{Project}::patrick:macsurf-source Folder:browser:netsurf:frontends:`
- `{Project}::patrick:macsurf-source Folder:browser:netsurf:frontends:macos9:`
- `{Project}::patrick:macsurf-source Folder:browser:netsurf:frontends:macos9:shims:`
- `{Project}::patrick:macsurf-source Folder:browser:netsurf:frontends:macos9:parserutils:`
- `{Project}::patrick:macsurf-source Folder:browser:netsurf:frontends:macos9:parserutils:charset:`
- `{Project}::patrick:macsurf-source Folder:browser:netsurf:include:`
- `{Project}::patrick:macsurf-source Folder:browser:netsurf:content:`
- `{Project}::patrick:macsurf-source Folder:browser:netsurf:desktop:`
- `{Project}::patrick:macsurf-source Folder:browser:netsurf:utils:`

System paths:
- `{Compiler}:MacOS Support:Universal:Interfaces:CIncludes:`
- `{Compiler}:MacOS Support:MacHeaders:`
- `{Compiler}:MSL:MSL_C:MSL_Common:Include:`
- `{Compiler}:MSL:MSL_Extras:MSL_Common:Include:`
- `{Compiler}:MSL:MSL_C:MSL_MacOS:Include:`

### Linux Cross-Check
Use `gcc -fsyntax-only -std=c89 -pedantic -Dinline= -Ibrowser/netsurf/frontends/macos9/shims -Ibrowser/netsurf/frontends -Ibrowser/netsurf/include -Ibrowser/netsurf -include stdbool.h` to syntax-check frontend files on Linux before copying to Mac.

### Project File List (470 .c files)
Added to MacSurf.mcp:
- 12 frontend `.c` files
- 5 shim `.c` files
- 10 NetSurf core `.c` files (utils + content + desktop)
- 15 libparserutils
- 30 libhubbub
- 95 libdom
- 303 libcss

`MacSurf.rsrc` (pre-compiled binary fork carrying `'carb'` + icon family + FREF + BNDL — see "Carbon App Requirements" above and [docs/resources.md](docs/resources.md)) must also be in the project; CW8 links `.rsrc` files directly into the output resource fork with no Rez step. The `*_stub.c` files exist on disk but are NOT in the project file list. See [docs/research/architecture-inventory.md](docs/research/architecture-inventory.md) for the full breakdown.


### Library Dependency Chain, COMPLETE

All five NetSurf core libraries are ported and C89-clean:

| Library | .c files | Status |
|---|---:|---|
| libwapcaplet | (via lwc_stub.c) | ✓ done at v0.1 |
| libparserutils | 15 | ✓ commit 8074a74 |
| libhubbub (HTML5 parser) | 30 | ✓ commit fd8d915 |
| libdom (DOM implementation) | 95 | ✓ commit 744232d |
| libcss (CSS parser + cascade) | 303 | ✓ commit 02628cf |
| **Total in MacSurf.mcp** | **443** | |

Combined LOC: ~125K. Stub footprint replaced: 3,688 lines (parserutils utf8.h + dom.h + libcss.h). All four port audits + execution reports live in [docs/research/](docs/research/):
- [parserutils-port.md](docs/research/parserutils-port.md)
- [libhubbub-port.md](docs/research/libhubbub-port.md)
- [libdom-port.md](docs/research/libdom-port.md)
- [libcss-port.md](docs/research/libcss-port.md)

**Next milestone, NetSurf core wiring (5 phases).** All five libraries are now ported and C89-clean (443 .c files in MacSurf.mcp), so the remaining work is glue between NetSurf core and the libraries. Full audit and sequencing in [docs/research/netsurf-core-wiring.md](docs/research/netsurf-core-wiring.md). Phases:

1. **HTTP fetcher rewrite**, `macos9_http_fetcher.c` implementing the real `fetcher_operation_table`, replacing the v0.1 standalone OT fetch path. Reuses the OT primitives from `macos9_fetch.c`. Delete `fetch_stub.c`.
2. **Content handler infrastructure**, add `content/content_factory.c` + ~9 utils/ helpers (corestrings, libdom, talloc, hashtable, idna, etc.) + ~4 desktop/ helpers (selection, scrollbar, textarea, system_colour). Cascading compile errors expected.
3. **CSS handler**, add 5 files from `content/handlers/css/`, convert designated initializers, delete `frontends/macos9/css/` stubs.
4. **HTML handler**, add 23 files from `content/handlers/html/`, convert ~9 designated initializers (including the `html_content_handler` vtable with 16+ function pointer fields), 1 for-scope decl in `layout_flex.c`, delete `frontends/macos9/html/` stubs, create `dom/bindings/hubbub/parser.h` wrapper header.
5. **End-to-end render**, implement `plot_text` / `plot_clip` / `plot_rectangle` in QuickDraw in `plotters.c`, wire `browser_window_create` to drive a real fetch through `hlcache_handle_retrieve`.

**Total scope:** ~44 new .c files in MacSurf.mcp (taking the project to ~487 files), ~800 lines of new frontend code, ~25 designated init conversions, 226 lines of stub deletion. **Image rendering deferred to v0.3**, `image_init()` is fully `#ifdef WITH_*` gated, so without `WITH_BMP`/`WITH_GIF`/etc. it's a no-op and saves 28 files / 5.4K LOC of work this milestone.

**Most likely bottleneck:** the talloc question. NetSurf's `utils/talloc.c` is a Samba-derived hierarchical allocator with POSIX-y patterns; if it doesn't compile under CW8 it needs its own port pass before HTML can land. Documented in §13 open question 3 of the wiring audit.

### Library port audit checklist

When auditing a new C99 library for CW8 / strict C89, grep for:
- `inline` keyword
- `//` line comments (start-of-line AND trailing, but EXCLUDE URLs in `/* */` block comments, especially `http://www.opensource.org/licenses/...`)
- C99 designated initializers (`^\s*\.\w+\s*=`), and **count instances per file**, not just file count. format_list_style.c had 47 in one file.
- For-scope declarations: integer types AND **pointer-type variants** (`for (TYPE *NAME = ...)`, `for (const TYPE *NAME = ...)`). The libcss audit missed pointer-type for-scope and undercounted by 10 sites.
- `restrict` keyword
- Compound literals
- `__VA_ARGS__` variadic macros
- `long long`
- Variable-length arrays
- Flexible array members
- Forward enum declarations
- `__attribute__` / `__builtin_*`
- `snprintf` / `vsnprintf`
- `%zu` / `%zd` printf formats
- `<iconv.h>`, `<errno.h>`, `<strings.h>`, `<sys/types.h>` and other POSIX
- **GNU union casts**, `(union_type)0` or `(typedef_name)expr` where the typedef resolves to a union. Standard C89 forbids casting to union types. The libcss audit missed 5 sites of `(css_fixed_or_calc)0`.
- **Union initializers using designated syntax**, `{.field = value}` for a typedef'd union looks identical to a struct designated init in grep output. C89 union initializers must use `{value}` (positional, first member only).
- Build-time codegen (`gperf`, perl scripts, `.inc` files included from `.c` files)
- Existing MacSurf stubs in `frontends/macos9/<libname>/` that will conflict with the real headers

### Adding new .c files
When a change introduces a new `.c` file, mention it plainly so the user can add it to the project. **Do NOT edit `MacSurf.mcp` and do NOT include it in fix zips**, the user maintains the project file list on the Mac side through the CW8 IDE, and a Linux-edited `.mcp` will clobber their local changes. Just list the new filename(s) in the handoff and let the user add them.

**Working-tree files not yet in HEAD (untracked / renamed — must be in `MacSurf.mcp`):**
- `frontends/macos9/macos9_tls_fetcher.c` — the HTTPS fetcher, **renamed** from `macos9_https_fetcher.c` (old name deleted in the worktree). Swap the entry in `MacSurf.mcp`.
- `frontends/macos9/macos9_content_registry.c` / `.h` — out-of-band live-content registry (anti-UAF, fixes533).
- `frontends/macos9/javascript/macsurf_qjs.c` / `.h` — the QuickJS engine glue (replaces the deleted `macsurf_js.c` / `macsurf_es6.c` family).
- `browser/libquickjs/` — the QuickJS engine sources (replaces the deleted `browser/libduktape/`).
- `content/content.c` is **renamed to `content/ns_content.c`** in the worktree (old `content.c` shows as deleted).

### Shipping discipline
- Deliverables for a fix round are: delta tar with full tree preserved, `MacSurf.mcp` add/remove list, and `Access Paths.xml` add/remove list.
- Standard transfer path is: build `fixesNN.tar`, then `scp -P 2222 -i ~/.ssh/macsurf_push -o StrictHostKeyChecking=no fixesNN.tar patrick@localhost:Documents/MacFiles/fixesNN.tar`.
- Do not stop at "tar created locally" when the user asked to send it.


## JavaScript Engine

**The engine in the working tree is QuickJS, not Duktape.** (HEAD still builds Duktape; the migration is uncommitted — see the banner at the top.)

- **QuickJS** is the JS engine, gated by `WITH_QUICKJS` (defined `1` by default in [macsurf_prefix.h:281](browser/netsurf/frontends/macos9/macsurf_prefix.h#L281)).
- The engine implementation lives in [browser/netsurf/frontends/macos9/javascript/macsurf_qjs.c](browser/netsurf/frontends/macos9/javascript/macsurf_qjs.c) (untracked). It owns `js_initialise` / `js_newheap` / `js_exec` / `js_fire_event` etc. and runs **ES2023 natively** through `JS_Eval` (`JS_NewRuntime` at heap creation).
- QuickJS sources are at [browser/libquickjs/](browser/libquickjs/) (untracked; `cutils.h`, `libregexp.c`, `libunicode.c`, `quickjs.c`, …). Not to be confused with the separate `quickjs-macos9/` standalone port.
- **The ES6→ES5 transpiler is removed / bypassed (fixes522).** `javascript/macsurf_es6.c` / `.h` are deleted in the worktree — QuickJS runs modern JS directly, so the transpiler (which was corrupting bundles) is gone. The earlier "JavaScript marathon" hand-built API surface (`macsurf_js.c`, `macsurf_js_dom.c`, the `.bnd` files, etc.) is likewise deleted.
- **`js_exec` accepts all JS natively** (no keyword fast-fail, no stub). A memory limit and ~20s eval timeout guard runaway scripts.

**Cleanup items (dead/inert, safe to remove when convenient):**
- `content/handlers/javascript/Makefile` still has an `ifeq ($(NETSURF_USE_DUKTAPE),YES)` branch — dead; MacSurf builds from the CW8 `.mcp`, not this Makefile.
- [js_stub.c](browser/netsurf/frontends/macos9/js_stub.c) provides no-op `js_*` fallbacks gated on `#ifndef WITH_QUICKJS` — inert while `WITH_QUICKJS` is defined.
- All `browser/libduktape/*` and `content/handlers/javascript/duktape/*` are deleted in the worktree.

## Browser Chrome

- Pixel-based scrolling operates on `content_get_height()`, not the v0.1 text-line model.
- Address bar routes through `browser_window_navigate` with `nsurl_create` URL normalization (prepends `http://` if no scheme).
- Back, Forward, Reload, Home buttons wired to real NetSurf navigation APIs.
- Status bar displays NetSurf status messages and hovered link URLs.
- Title bar auto-updates via `gui_window_set_title` from page content.
- Window resize triggers `browser_window_schedule_reformat` via a deferred flag pattern to prevent re-entrant layout.
- `MACSURF_HOME_URL` defined in `macsurf_config.h`.
- v0.1 fallback path (`strip_html` + direct `DrawText`) has been removed. Full NetSurf pipeline is the only rendering path.

## Mouse Wheel / Input Devices

- **No Carbon wheel handler.** `kEventMouseWheelMoved` is **not available in CarbonLib** on OS 9, Apple's own `CarbonEvents.h` marks the event class as `Mac OS X: 10.0+ in Carbon.framework; CarbonLib: not available`. fixes134 attempted to install a handler and the Mac crashed with illegal-instruction at `19DBDEB8` because CarbonLib's dispatcher destabilizes when asked about events whose class was never back-ported. Root-caused and disabled in fixes140. See [browser/netsurf/frontends/macos9/macos9_wheel.c](browser/netsurf/frontends/macos9/macos9_wheel.c), `macos9_wheel_install()` is retained as a visible no-op for ABI stability (Mac-side main.c may still reference it).
- **fixes141, interim defensive hardening.** Even with the Carbon wheel handler disabled, spinning the wheel under MacSurf still dropped into MacsBug with `Undefined A-Trap at 1BDC54E0` (no procedure name, execution in garbage memory). fixes141 narrowed the `WaitNextEvent` mask to an explicit whitelist of classic event kinds and added a matching whitelist guard at the top of `macos9_dispatch_event` so any unknown event class is silently dropped before touching any Toolbox or browser-core code. This is hardening, not diagnosis, the underlying crash is likely inside CarbonLib or USB Overdrive's trap patches and cannot be debugged further without capturing a real MacsBug stack, which requires an ADB keyboard the user does not currently have. **Proper wheel-crash diagnosis deferred until ADB hardware is available.**
- **USB Overdrive, current recommendation: "Do Nothing" on the Scroll Wheel action** until the wheel-crash root cause is understood. Users should configure USB Overdrive's Scroll Wheel setting to "Do Nothing" (or not install a wheel binding at all) when MacSurf is frontmost. Scrolling works via scroll bar, keyboard arrows, Page Up/Down, and Home/End. The previous recommendation (Up/Down arrow keys) is valid in principle, it flows through `macos9_handle_key_down`, but may still trigger the underlying crash if USB Overdrive's trap patches touch state before the synthesized key event reaches us. See [docs/usb-overdrive.md](docs/usb-overdrive.md).
- **Complete scroll-input set on OS 9 without the wheel:** scroll-bar drag, keyboard arrows, Page Up/Down, Home/End. All keyboard-sourced paths are tested and working. Carbon-native wheel events are architecturally out of reach on this platform regardless of the fixes141 defensive work.

## Rendering Pipeline (native CSS)

- HTTP fetcher registered for `http:` and `https:` schemes, fetching directly from origin over Open Transport; `https:` runs through the native macTLS stack. No proxy.
- Resource fetcher serves real CSS content for `resource:default.css`, `resource:internal.css`, `resource:quirks.css` (`macos9_fetcher_stubs.c`).
- `no_backing_store.c` returns `NSERROR_NOT_IMPLEMENTED` from store and fetch.
- Event-loop sleep shortens to 1 tick while any fetcher is active (`macos9_fetching || macos9_stub_fetcher_active() || macos9_http_fetcher_active()`) so NetSurf's fetcher ring progresses via `fetch_send_callback` continuations every pass. There is **no** explicit `fetch_poll()` call.
- **Full NetSurf pipeline executes: fetch → parse → CSS cascade with native var() resolution → layout → plot.**
- **Real HTML rendering with styled text, colours, fonts, layout all working natively.** MacTrove (Drupal 11 site) loads with body background, card chrome, link colours, and theme fonts resolving correctly from CSS custom properties. Verified signal: title bar shows `cp res OK`.
- **Architectural foundation:** [docs/research/state-survey-2026-04-18.md](docs/research/state-survey-2026-04-18.md) and [state-survey-2026-04-19.md](docs/research/state-survey-2026-04-19.md). The 2026-04-19 survey in particular (§A7) explicitly scoped three paths for var() support, native libcss, proxy preprocessor, browser preprocessor, and chose native. Without that scoping, the fast-looking proxy shortcut would have blocked the real fix. Treat both surveys as load-bearing architectural refs for any future CSS-layer work.
- Screenshot canonical location: `screenshots/v0.3-mactrove-fixes139.png` (user-saved from the 2026-04-20 session).
- Carbon partition bumped well past the original 16 MB floor (current project ~195 MB preferred / ~164 MB minimum) to accommodate libcss + DOM allocation footprint on real pages (CSS_NOMEM blocker long resolved; see Gotchas).

### Current blockers, CSS structural gaps (see [CSS_STATUS.md](CSS_STATUS.md) for full audit)

The CSS pipeline parses 167 properties via libcss but layout/redraw only consumes 87 of them. The visible "pages don't load properly" symptoms map to specific silent-fail properties:


**Other gaps documented in CSS_STATUS.md:**
- `background-attachment: fixed`, `quotes`, `empty-cells`, `table-layout`, `unicode-bidi`, `writing-mode`, `word-spacing`, `break-*`, `page-break-*`, `orphans`, `widows`, `fill-opacity`, `stroke-opacity`, all parsed, all silently dropped.
- `gap: A B` two-value form, single value works (97% of cases), two-value collapses to column-gap=B (deferred, fixes148).
- `font-family` matching narrow (Geneva/Monaco/Chicago/Charcoal only).
- `font-weight` 9 numeric values collapse to bold/normal.
- `column-span: all`, parsed but not consumed; multicol V1 shipped without spanning children.
- `transition`, `animation`, `clip-path`, `mask`, deferred (v0.4.5+).

**Pipeline bugs (not CSS feature gaps):**
- **Cache-hit first-paint bug (OPEN, undiagnosed at the paint-trigger level).** Every navigation first-paints the placeholder (`about:query` / `about:fetcherror`) before the real cached page lands. **This is NOT a synchronous-delivery problem** — and earlier theories that the fix is "deliver the cache hit synchronously" are wrong. Cache-hit delivery is *already* async-by-design: `macos9_https_start` only sets `c->started`; `hctx_poll` serves the `HS_CACHEHIT` state (FETCH_HEADER + FETCH_DATA + `hctx_finish`) on a **later** poll-loop pass ([macos9_tls_fetcher.c:1831](browser/netsurf/frontends/macos9/macos9_tls_fetcher.c#L1831), `ops.poll` registered at :2527). Root cause is in the **first-paint trigger timing**: the paint fires against the placeholder before the deferred cache delivery completes. Fix belongs at the paint-trigger, not the fetcher. Still open and undiagnosed at that level.
- **JPEG photo plot is slow when scrolling.** Pre-scale at decode time if it becomes the bottleneck.
- **Inline boxes occasionally duplicate**, known issue post-fixes33, cause unknown.

For features already shipped (flex alignment, border-radius, box-shadow, gradients, transforms, Grid V1, image handlers), see the milestone table under "Build State" and grep the codebase before re-implementing. Per-fix history is in [docs/changelog-fixes.md](docs/changelog-fixes.md).

## Native CSS implementation

MacSurf handles modern CSS natively in libcss and the layout engine rather than preprocessing it anywhere else. CSS custom properties (`var()`) ship at fixes133-139; full status (per-property coverage, parsed-but-dropped gaps, deferred features) lives in [docs/css-status.md](docs/css-status.md) — that's the ground truth, not this file.

Architectural notes worth keeping inline:

- `var()` is resolved at cascade time via token substitution; lexer keystone fix landed at fixes139 ([browser/libcss/src/lex/lex.c](browser/libcss/src/lex/lex.c)).
- CSS feature support is the browser's job, handled natively in libcss and the layout engine — never preprocessed or offloaded.
- Features that degrade gracefully to block layout / flat rendering: `transition`, `animation`, `clip-path`, `mask`, `filter`. Cosmetic in most cases.

## Build State


**Current state, in brief:**

| Milestone | Version | Status |
|---|---|---|
| Fetch system (chunked, keep-alive, multi-page) | v0.5 | Shipped (fixes98-105) |
| Images: PNG (real alpha) / GIF / BMP / TIFF / JPEG | v0.4.6 | Shipped (fixes78-79b) |
| CSS Grid V1, transforms, radial-gradient, offscreen composite | v0.4.2–v0.4.5 | Shipped (fixes70-77g) |
| Full CSS3 rendering (text, colour, gradients, shadows, opacity) | v0.4.1 | Shipped (fixes70) |
| CSS cascade applies | v0.4 | Shipped (fixes33) |
| Plain text + JS + OT networking | v0.2 | Stable baseline |

Per-version architecture narrative: see [docs/version-history.md](docs/version-history.md).
Full fix history: see [docs/changelog-fixes.md](docs/changelog-fixes.md).

**Build conventions:**

- CW8 project file: [browser/netsurf/frontends/macos9/MacSurf.mcp](browser/netsurf/frontends/macos9/MacSurf.mcp).
- Carbon partition: the current project ships **large** (~195 MB preferred / ~164 MB minimum; `MWProject_PPC_size = 199384` / `minsize = 168192` K). **16 MB preferred / 8 MB minimum is the floor** — smaller starves libcss → CSS_NOMEM mid-cascade. On a RAM-tight Mac, lower the preferred toward the floor.
- The CodeWarrior project mirrors the source directory tree via ~55 hierarchical access paths (not a single flat folder), ~850 `.c` files. The build pack in `builds/` carries the authoritative project file, target settings, and file list.
- Remove Object Code is rarely needed before a rebuild.
- MacsBug is installed on the G4 for pipeline debugging; `MS_LOG` checkpoints are active throughout.


- **Last hardware-verified release (PAST): v1.5 "Modernity" (2026-06-11), source tree at fixes415.** Verified on a G3 iMac running OS 9.2.2. That release brought: on-device ES6→ES5 transpilation (since **retired** in favour of QuickJS, fixes522); JS→DOM→render re-conversion so JS-mutated content paints; and the v1.5 stability pass (fixes404-415: monotonic clock fix, SHA-384 self-tests, UAF guards, CSS Grid 8→16 column limit fixing modern 12-col grid collapses on XenForo pages like `68kmla.org`).
- **Current working tree (NOT a release): an uncommitted QuickJS-migration branch at ~fixes560+.** This is well ahead of both the v1.5 release and HEAD (≈fixes481). It swaps the JS engine Duktape→QuickJS and stacks ~80 further fix rounds. It is **uncommitted but NOT tree-wide unverified** — verification is per-component as of 2026-06-30; see [G3 Hardware Verification Status](#-g3-hardware-verification-status-2026-06-30) at the top of this file (cache-hit nav, on-screen diagnostics, one-column restore, and terminal-fail are G3-confirmed; the parser-token catch is pending; the llcache user-destroy box-walk UAF is actively crashing). Do not blanket-label the whole tree "shipped" *or* "unverified" — go by the bucket. [docs/status.md](docs/status.md) and [docs/version-history.md](docs/version-history.md) lag this tree; [docs/changelog-fixes.md](docs/changelog-fixes.md) has the per-fix history.

**Full fix history (predecessor chain from fixes225 → fixes143a):** see [docs/changelog-fixes.md](docs/changelog-fixes.md).

### Next work queue (priorities from [CSS_STATUS.md](CSS_STATUS.md))

Real-world impact ranked, lowest-effort first within each priority. Numbering reflects the fixes132+ ship order.

[CSS_STATUS.md](CSS_STATUS.md) is the ground truth for what's shipped vs. open. The list below tracks the active queue only.

Currently-open queue:

- **Grid V2 follow-ups (post fixes150):**
  - **`align-items: stretch` (default behavior)**, cells should stretch to fill the allocated row height. Currently V1 leaves empty space within rows when row track > cell content height.
  - **FR row distribution against definite container height**, when the grid container has explicit `height: 500px`, distribute remaining height (after PX rows) across FR rows. Mirrors fixes148's column FR distribution.
  - **Two-value `gap: A B`**, currently row-gap shares column-gap storage (fixes148 deferred). Needed for full-fidelity gap parsing.
- **`outline` (separate from border)**, focus rings on accessible pages. Could be implemented as a draw-after-border pass in redraw.c.

- **Other parsed-but-silently-dropped gaps:** `unicode-bidi`, `writing-mode`, `break-*`, `page-break-*`, `orphans`, `widows`, `fill-opacity`, `stroke-opacity`, `transition`/`animation` (v0.4.5+ ambition), `clip-path`, `mask`, `filter`, multi-tier `font-weight` (platform limit, QuickDraw is bold/regular only).

- **Full-fidelity `row-gap` (deferred).** Two-value `gap: A B` loses A — both properties share `column-gap` storage. Splitting requires new `CSS_PROP_ROW_GAP` enum + field in `css_computed_style_i` + bit slot in `autogenerated_computed.h` + propset/propget + parse/select files + `layout_flex.c` wiring. Only worth doing if real pages exercise the two-value form.
- **Wheel crash diagnostic exhaustion, Linux-source audit is DONE.** Crash signature: 68k-looking data executed as code (called through a function pointer that wasn't a routine descriptor). Crash is downstream of `macos9_dispatch_event` entry. Hardenings through fixes141-147 (event-mask whitelist, port hardening, UPP fix, quitting flag) did not conclusively fix it. **Further progress requires MacsBug `wh` stack capture on ADB hardware** — Linux-source hypothesis space is exhausted. Detailed evidence in [docs/changelog-fixes.md](docs/changelog-fixes.md) under fixes146-147.
## Regression Audit Checklist

**New in fixes152 after the fixes149 missed-init regression.**

Any new subsystem shipped as part of a fix round MUST, before the round is closed, satisfy all three:

1. **Init call wired.** Grep the entrypoint (`main.c main()` for MacSurf) for the init function name. `grep -c macsurf_foo_init main.c` must return a non-zero count. If zero, the subsystem is linked but dead.
2. **Init function body is reachable.** If the init function's real body is gated on a feature macro (`#ifdef MACSURF_DEBUG`, `#ifdef WITH_X`, etc.), grep the prefix file and project config for the macro. If the macro is never defined, the init compiles to an empty release stub and fires silently, exactly the fixes305a regression. `grep -rn "define MACSURF_DEBUG" browser/netsurf/frontends/macos9/` must return at least one hit for the channel to be live.
3. **Smoke test confirming it runs.** Either a SheepShaver smoke launch (boot, relaunch, confirm the subsystem's externally-visible artefact, file, title-bar message, menu item, is present) or a hardware cycle. "Linux syntax check passes" is NOT a smoke test; syntax passes on code that is never executed.
4. **Dependency documented.** Add a one-line entry under this section's table for new infrastructure:

| Subsystem | Init function | Init call site | Externally-visible artefact at startup |
|---|---|---|---|
| File-backed diagnostic log | `macsurf_debug_log_init` | `main.c main()` after `FlushEvents` | `MacSurf Debug.log` on Desktop with `=== MacSurf startup ===` entry |
| Open Transport | `InitOpenTransportInContext` | `main.c main()` after log init | `ot_initialized = true` (internal) |
| NetSurf core | `netsurf_init` | `main.c main()` after OT init | Window shows with content pipeline live |
| Carbon Appearance | `RegisterAppearanceClient` | `main.c main()` after `InitCursor` | Controls render with platinum theme, not classic |
| Cookie jar persistence (fixes368) | `macos9_cookies_load` / `_save` | `main.c main()` after `netsurf_init` (load) and before `netsurf_exit` (save) | `MacSurf Debug.log` shows `cookies loaded` / `cookies saved`; a `MacSurf Cookies` file persists a logged-in session across relaunch. **Hardware-unverified** (MSL fopen-path behaviour); no-op on failure, never a regression. |

**Why this checklist exists.** fixes149 through fixes151 accumulated ~20 instrumentation lines across window.c / main.c / the handle functions. None of them wrote anything to the log file because `macsurf_debug_log_init` was never called. The bug was invisible because:
- `macsurf_debug_log_write` short-circuits silently when `g_log_open == 0`, no stderr, no toolbox call, no stdout.
- MacSurf Debug.log on the Desktop never appeared, but its absence wasn't flagged by any build step.
- fixes149/150/151 all "landed" per git and per user test cycles, but produced zero log output.
- A SheepShaver smoke test ("launch, quit, ls Desktop") would have caught it in 30 seconds of the first ship.

**fixes305a, same class of regression, different vector.** Sometime during the fixes260-304 sprint, `MACSURF_DEBUG` was lost from `macsurf_prefix.h`. The init call at [main.c:191](browser/netsurf/frontends/macos9/main.c#L191) stayed wired, but the entire body of `macsurf_debug_log_init`, `macsurf_debug_log_write`, and `MS_LOG()` is gated on `#ifdef MACSURF_DEBUG`. Without the define, init reduced to the empty release stub at [macsurf_debug_log.c:352](browser/netsurf/frontends/macos9/macsurf_debug_log.c#L352) and every MS_LOG call site compiled out. fixes302-304 instrumentation produced zero log output, mirroring the fixes149-151 pattern, but the grep-the-entrypoint check passed because the init call name was present. Fix: `#define MACSURF_DEBUG 1` (gated on `#ifndef MACSURF_RELEASE`) added to `macsurf_prefix.h`. Audit step 2 in the checklist above is new, it catches this specific vector.

**When reviewing / shipping a fix round:** if the round adds or touches a subsystem in the table above, verify the init path still fires AND that the init function's real body is not behind a never-defined feature macro. If the round adds a new subsystem, add an entry. Missed-init and gated-init are both regressions this table catches.

## File-Backed Diagnostic Channel

Shipped in fixes149. Writes one CR-terminated line per log call to
**`MacSurf Debug.log`** on the Desktop, flushing after every write
(`FlushVol` + `SetFPos` pair) so the file survives illegal-instruction
crashes, frozen Macs, and forced restarts. This is the **primary
post-crash back trace channel for MacSurf** on hardware we can't
attach MacsBug to.

- API: [browser/netsurf/frontends/macos9/macsurf_debug_log.h](browser/netsurf/frontends/macos9/macsurf_debug_log.h). `macsurf_debug_log_init()` at startup, `_close()` at shutdown, `_write(str)` for literal strings, `_writef(fmt, ...)` for minimal printf (`%d`, `%ld`, `%p`, `%s`, `%%`).
- `MS_LOG(msg)` now dual-channels: title bar (live feedback) **and** log file (durable record). File write comes first so a SetWTitle-adjacent crash still leaves the log entry on disk.
- `_writef` uses a hand-rolled formatter, NOT MSL's `vsnprintf` (unreliable on CW8 Carbon MSL). Supports only the format specifiers used by MacSurf instrumentation. Output is hard-capped at 255 bytes.
- Log file location: Desktop (via `FindFolder(kOnSystemDisk, kDesktopFolderType, ...)`). If FindFolder fails the log is silently inert, init does not crash, subsequent calls no-op.
- Reading the log: open `MacSurf Debug.log` in SimpleText. Each line is one log call. Crash forensics = "the last N lines before the log ends show the code path that was executing when the Mac died."
- **Release builds (`MACSURF_RELEASE` set) compile the channel to empty stubs**, symbols stay exported for link compatibility, but no file operations happen.
- **Gotcha:** the channel depends on HFS actually committing writes. `FlushVol` forces this, but if a volume is full / dismounted / read-only the write silently fails. If the log file exists but is truncated or stale after a crash, the HFS journal didn't catch up, retry the crash with a different volume or add an extra tick of delay after each write.
- Don't replace existing `MS_LOG` call sites with `macsurf_debug_log_writef` unless you need format arguments. `MS_LOG(literal)` is ergonomically equivalent and keeps the title bar updated for free.

## Docs

- [docs/architecture.md](docs/architecture.md), Full platform architecture: rendering modes, native networking, milestone plan
- [docs/research/architecture-inventory.md](docs/research/architecture-inventory.md), Snapshot of what currently exists in the repo (no decisions, just facts)
- [docs/research/window-architecture-2026-04-22.md](docs/research/window-architecture-2026-04-22.md), Window-framework architecture research (fixes161). Full state/event/redraw/scroll inventory of the Mac OS 9 frontend; architectural problem list; proposed unified window-state model; 6-round refactor plan (fixes162-fixes166). **fixes162+ follow this plan.**
- [docs/status.md](docs/status.md), Project status, milestones, test environment
- [docs/codewarrior-setup.md](docs/codewarrior-setup.md), How to install CodeWarrior 8 and build on a real Power Mac
- [docs/version-history.md](docs/version-history.md), Per-version architecture narrative (v0.2 → v1.4)

## SheepShaver as a Testing Tool

MacSurf is built on Linux but target-tested on real OS 9 hardware. SheepShaver (an OS 9 emulator) is a useful *partial* substitute, **not a full one**.

**Confirmed-running on SheepShaver:** OS **9.0.4** runs MacSurf well as of 2026-05-25 — full Carbon init, UI smoke, navigation, and rendering all work. Networking is the limitation: SheepShaver's OT TCP can't reach the live internet without manual ethernet config, so HTTPS fetches hit `NO_PROGRESS_TICKS` and route to about:fetcherror. Good for build-smoke gating; not a substitute for hardware-side fetcher testing.

- **SheepShaver setup lives at** `/home/patrick/Webs/MAC/sheepshaver/`, shared folder at `shared/`, prefs at `prefs`, Xvfb on `:99`. Shared folder uses `.finf/` (32-byte FInfo per file) + `.rsrc/` (raw resource fork) sidecars for Mac metadata.
- **Run the SheepShaver AppImage** from `/tmp/squashfs-root/AppRun` with `DISPLAY=:99 APPIMAGE=/tmp/squashfs-root HOME=/home/patrick`.
- **Decode a .hqx build into the shared folder** with `/tmp/binhex_decode.py <hqx> <shared-folder>`. Writes the file + `.finf/<name>` + `.rsrc/<name>` with correct APPL/MPLS type/creator and the full cfrg/carb/SIZE resource fork.
- **Hand-built resource forks don't work.** Shipping just the PEF data fork (the `/home/patrick/Webs/macsurf/MacSurf` checkin) leaves out `cfrg` (Code Fragment resource) and OS 9 refuses to launch it. The CW8 build-on-Mac workflow produces the full resource fork at link time; replicating it on Linux requires either the .hqx or reconstructing cfrg by hand (the hand-built variant was attempted and OS 9 still rejected the launch).

**What SheepShaver IS useful for:**
- Smoke test, does the build launch at all, does Carbon init succeed, do OT/CarbonLib dependencies resolve
- Rendering regression checks, does MacTrove render, does var() still resolve, does layout not regress
- Non-hardware-specific logic bugs, compile errors that make it through Linux syntax check but not the real linker, obvious toolbox misuse

**What SheepShaver is NOT useful for:**
- Hardware-specific crashes (wheel crash, scroll-bar click crash). SheepShaver's CarbonLib + Control Manager emulation is more forgiving than real hardware. A green light in SheepShaver does NOT mean the G3/G4 will also be green.
- USB Overdrive interactions, doesn't exist in the emulator
- Real network behavior, `/home/patrick/.sheepshaver_prefs` as shipped has no usable ethernet config, so the initial HTTPS fetch to the origin blocks until timeout (~2 min) without yielding. This is a test-env artifact, not a MacSurf bug.
- Timing-sensitive behavior, JIT / coop-scheduler pacing differs from real PPC

**Workflow:** use SheepShaver opportunistically, launch a new build, confirm it boots, click around. If anything hardware-specific is the suspect, move to real G3/G4. Don't treat SheepShaver "passed" as a substitute for hardware-side verification on wheel/scroll/USB-driven bugs.

## Known Gotchas

- **A box that layout never resolves keeps its BIRTH width `UNKNOWN_WIDTH == INT_MAX`, and it silently blows the document content width (`c_w`) up to 2147483647 — the "split scrollbar" bug (fixes625, the biggest single render fix in the engine; tag `fixes625-split-scrollbar-verified`).** Symptom: `reformat: ... c_w=2147483647 desc_x1=2147483647` while `lyt_wh` is a correct finite width and the Y axis is clean (`c_h`/`desc_y1` finite); visually the page content packs into a narrow left column with a giant empty canvas beside it plus a spurious horizontal scrollbar (both XenForo forums — tinkerdifferent.com and 68kmla.org — hit this). Root-cause chain: (1) every box is born `width = UNKNOWN_WIDTH (INT_MAX)` in `box_manipulate.c`, a "not laid out yet" sentinel; (2) the fork's **failure-tolerant** layout paths (fixes613f flex-item tolerance, fixes171 watchdog degrade, etc.) zero a failed box's **height but never its width**, so an unresolved box survives with `width == INT_MAX`; (3) the document-width walk accumulates each box's border-edge extent (`descendant_x1 = x + width`), and the defensive clamp in [layout.c `layout_get_box_bbox`](browser/netsurf/content/handlers/html/layout.c) "clamped" by **re-injecting `box->width` verbatim** (`if (*desc_x1 > 10000) *desc_x1 = box->width;` and the `<= *desc_x0` twin) — a no-op precisely when the *width itself* is the garbage; (4) INT_MAX rides to the root → `c->width = layout->x + descendant_x1 = INT_MAX` at [html.c:1799](browser/netsurf/content/handlers/html/html.c#L1799). Stock NetSurf never hits it (it lays out every box, no un-resolved sentinel). **The fix is at the single choke point:** in `layout_get_box_bbox`, sanitize `box->width`/`height` to 0 when it is `UNKNOWN_WIDTH`, negative, or `> LAYOUT_SAFE_MAX` **before** computing the extent (mirrors how a height-0 box already keeps the Y axis clean), and drive the clamps off the sanitized local, never `box->width`. Belt-and-braces: a ceiling on the bbox accumulation (`layout_update_descendant_bbox`), a `FLEX_SAFE_MAX` clamp + `UNKNOWN_MAX_WIDTH`-sentinel guard at the flex place-site in [layout_flex.c](browser/netsurf/content/handlers/html/layout_flex.c) (the fixes623 fallback was itself feeding `UNKNOWN_MAX_WIDTH`!), and a `descendant_x1 <= 1000000` backstop at the html.c sink. **Any new "tolerate a failed sub-layout" path MUST reset the degraded subtree's WIDTH (to 0 or a resolved value), not just its height** — the asymmetry (height-0 is harmless on Y, width-INT_MAX is fatal on X) is exactly why this class of bug is X-axis-only. Full root-cause dossier (source-verified, upstream-ready): [forclaude/split-scrollbar-bug-report.md](forclaude/split-scrollbar-bug-report.md). Sentinels: `UNKNOWN_WIDTH`/`UNKNOWN_MAX_WIDTH = INT_MAX` ([box.h:45](browser/netsurf/content/handlers/html/box.h#L45)); `AUTO = INT_MIN` ([layout_internal.h:27](browser/netsurf/content/handlers/html/layout_internal.h#L27)) — do NOT confuse the two (this bug wasted rounds being chased as an INT_MIN auto-margin, which was only a `box_dump` display artifact on finite boxes).

- **Defensive-clamp thresholds in [redraw.c html_redraw_box](browser/netsurf/content/handlers/html/redraw.c#L2356) must scale with realistic content height, not just catch obvious garbage.** The clamp at lines 2353-2374 zeroes any box field whose value looks like CSS-engine garbage (observed garbage: `box->x = 30728`, `descendant_y0 = -39845888`). Original thresholds were ±10000 across the board. That was fine for years, until probe cards accumulated and the test page (`advanced.html`) finally grew past 10000 px tall. At that point the clamp started firing on the *legitimate* root box (height = 10035), resetting `box->height` to 0 and then `descendant_y1` to that new (zero) height. First child's clip intersection collapsed to inverted, the line-2557 early-return fired without recursing, and the page rendered empty. Symptom signature: `visits=2 block=2 inlinec=0 inline=0 text=0 plot_text=0 plot_rect=1` from main.c's per-redraw counters AND recascade still walking the full tree (1041 boxes), proves the tree is intact but redraw bails after html+body. fixes156 raised the y/height/descendant_y thresholds to ±200000 (still catches the original -39845888 garbage with 4 orders of magnitude headroom; allows pages up to 200k px tall). **For any future field added to that clamp block: pick a threshold orders of magnitude larger than the largest realistic value, not just barely larger.** Pages can be very tall; viewport widths and x-coords stay narrow. The Mac32-pixel signed coordinate space (-32767..32767) is the hard ceiling; ±200000 already exceeds that, so the clamp's job is really "is this within int range" rather than "fits on screen." If a field exists where real-content values can plausibly reach 100k+, keep raising. **Diagnostic to identify when this gotcha is the active cause:** when redraw counters show `visits=2 block=2 text=0` while recascade walks the full tree, log the root box's `h` / `descendant_y1` directly, if they're zero but `c_h` in the reformat log isn't, the clamp is the culprit.

- **plotters.c must not assume the current QD port is the window** when looking up its `struct gui_window *`. The old pattern `GetPort(&p); gw = GetWRefCon((WindowRef)p)` worked for direct-paint-to-window but breaks the moment any code does `SetGWorld(offscreen, NULL)` mid-redraw, casting a GWorld pointer to WindowRef and calling GetWRefCon reads garbage memory. Symptom: `gw->content_rect` resolves to wildly out-of-range values like `(-32538, 11128, 31871, 0)`, SectRgn against that gives empty regions, and **every plot_clip / push_clip resolves effective=(0,0,0,0)**, PaintRect/DrawText calls fire but nothing lands on screen. Page renders blank with all the right log entries (visits / plot_text / plot_rect counters all look normal). main.c sets a global `macos9_paint_gw` around `browser_window_redraw`; plotters.c's `macos9_find_gw_for_plot()` reads that first, falls back to GetPort+GetWRefCon. The three sites (`macos9_push_clip`, `macos9_plot_clip`, transform-widen path) all go through the helper now. Fixed in fixes77g, was the root cause behind fixes77c's blank-page failure AND why two prior offscreen-GWorld attempts didn't work. Side effects observed when fixes77g landed: scroll bar tracking became smooth, URL bar text input started working on the initial window (previously was broken, see 2026-04-18 survey hypothesis), URL submit navigates pages. All three were downstream of the same empty-clip bug, just manifesting differently across direct vs offscreen paint paths. **Anytime you introduce a code path that changes the current QD port to anything other than the window, audit plotters.c for the GetPort assumption, and if you find new sites, add them to the helper rather than duplicating the WRefCon path.**
- **QuickDraw `CopyBits` / `CopyMask` colorize the transfer with the port's current foreground/background colour.** This is the classic-QD "colorizing copy" behaviour: even a 32-bit→32-bit `srcCopy` is tinted toward the port's `RGBForeColor` (source pixels are blended/mapped between fg and bg, so fg=black + bg=white is the only identity transform). MacSurf draws blue link text via `RGBForeColor(blue)` and `plot_bitmap` did its image blit without resetting fg, so **every image was tinted toward the leftover foreground colour** — dark images went blue, bright images washed out ("faded"). The bug is paint-order- and content-dependent, so it looked intermittent ("fine on first paint, blue after scroll", "faded on mactrove but not obviously blue"). It is NOT a byte-order / decode bug: the decoded RGBA buffer and the source GWorld were both verified correct; a dest-readback probe caught a black source pixel `[255,0,0,0]` landing in the composite as blue `[0,0,95,169]`. **Fix (fixes301j, [plotters.c](browser/netsurf/frontends/macos9/plotters.c) `macos9_plot_bitmap`): `RGBForeColor(black)` + `RGBBackColor(white)` immediately before every `CopyBits`/`CopyMask`.** **Any new QuickDraw blit anywhere in the frontend must reset fg=black/bg=white first unless it deliberately wants colorizing** — assume the port's fg is whatever colour the last text/rect draw left it. This is the same root cause as the long-standing "mactrove faded images" symptom. Diagnosing it took a chain of probes precisely because the decode looked perfect; when an image renders the wrong colour, check the blit's fg/bg before suspecting the decoder or a channel swap.
- **`kInitOTForApplicationMask` and `kOTInvalidConfigurationRef` are not defined in CW8's OT headers.** Define them manually where needed (`kInitOTForApplicationMask = 0x00000002`) to keep the current `InitOpenTransportInContext()` path building cleanly.
- **Including `<OpenTransport.h>` is safe** now that `kWindowStandardHandlerAttribute` has been removed from `CreateNewWindow`. An earlier crash that seemed like it was caused by including the header was actually the window-attribute bug manifesting later.
- **No `'carb'` resource → OTClientLib crash at a fixed address.** If the same instruction crashes every time somewhere inside OTClientLib, the cause is almost always that the binary is not a recognized Carbon fragment. Add `'carb'` before debugging anything else.
- **CW8 C89:** no `inline`, no `//` comments, no variadic macros, no forward enum declarations, no C99 designated initializers, no `for (int i...)`. All variables at the top of their enclosing block.
- **NSLOG requires `__VA_ARGS__`, the varargs-function approach does NOT work.** CW8 C89 rejects fixed-param macros called with extra args (e.g. `#define NSLOG(cat, level, fmt) do{}while(0)` rejects 4-arg calls with `')' expected`). A varargs C function (`static void noop_(const void*c, long l, const char*f, ...)`) looks correct but evaluates the category token as an expression, `NSLOG(fetch, DEBUG, ...)` becomes `noop_(fetch, DEBUG, ...)` and `fetch` must be in scope. In `dump_rings()` and other functions where `fetch` is the category but not a local variable, this fails with `'fetch' undeclared`. Same for `llcache`, `layout`, `flex`, `schedule` etc. **Correct fix**: `#define NSLOG(cat, level, ...) do{}while(0)`, CW8 supports `__VA_ARGS__` as a pre-C99 extension; category and level are consumed as unevaluated macro parameters. Retro68 PPC gcc (`-std=c89`) also accepts this cleanly. Fixed in fixes260.
- **Do NOT define NSLOG category tokens as macros.** `fetch`, `llcache`, `layout`, `flex`, `schedule` are all used as variable/parameter/struct-member names in the codebase. `#define llcache 0` turns `static struct llcache_s *llcache = NULL;` into `static struct llcache_s *0 = NULL;`, a syntax error. `#define schedule 0` turns `guit->misc->schedule(...)` into `guit->misc->0(...)`. The `__VA_ARGS__` macro approach makes these defines unnecessary.
- **CW8 C89 fails to complete a struct that has a named enum declared inside its body.** `struct foo { enum bar { A, B } type; ... };` leaves `struct foo` as an incomplete type and `bar` as an undefined identifier. Anonymous enums inside structs (`enum { A, B } type;`) ARE accepted. Fix: hoist named enums before the struct definition. Fixed in fixes259 for `html.h`'s `enum html_script_type`.
- **macos9/ shim headers that set the real guard WITHOUT defining the structs silently break all TUs that include them.** The CW8 access path puts `frontends/macos9/` before `content/handlers/html/` so shim headers are found first. If a shim like `html/html.h` sets `NETSURF_HTML_HTML_H` (the real guard) without defining `struct html_script`, every TU that includes `html/html.h` gets an empty substitution, the real header is never processed. Symptoms: `incomplete type 'struct html_script'`, `unknown identifier 'HTML_SCRIPT_INLINE'`. **Fix pattern**: make the shim a forwarder with its OWN guard (`MACSURF_SHIM_*`) and include the real header with its full path: `#include "content/handlers/html/html.h"`. The forwarder is found first, includes the real file, and the real guard prevents double inclusion. Applied in fixes260 to `html/html.h` and `html/form_internal.h`.
- **Carbon partition must be at least 16 MB preferred.** Set in CW8 under "PPC PEF" → Application Heap Size / Minimum Heap Size (`MWProject_PPC_size` / `MWProject_PPC_minsize` in the `.mcp` XML). A 4 MB partition (the CW8 default) runs out mid-CSS-cascade on a moderately-sized real page, `css_select_style` returns `CSS_NOMEM` somewhere around element 380. libcss allocates via raw `malloc`/`calloc` with no NetSurf wrapper, so OOM in libcss really does mean OS-heap exhaustion. Classilla's default is 32 MB; 16 MB is MacSurf's floor. (The current shipped project sets the preferred partition far higher — ~195 MB / ~164 MB min — but 16 MB remains the floor below which libcss starves.) See [docs/research/state-survey-2026-04-18.md](docs/research/state-survey-2026-04-18.md) §2.
- **CW8 PPC miscompiles `long long` / `int64_t` multiply-by-constant.** `(long long)a * small_const` writes `a >> log2(const)` into the high word instead of the correct `(a*const) >> 32`. Confirmed on real hardware via probe G (fixes113): `(long long)131072 * 1024LL` produced hi=128, lo=134217728, full product 549,890,031,616 instead of 134,217,728. This broke every FDIV/FMUL in libcss for weeks and masqueraded as a layout bug. **Mitigation:** route 64-bit fixed-point math through `double` under `#ifdef __MWERKS__`. PPC has a hardware FPU and IEEE 754's 52-bit mantissa covers every int32 fixed-point intermediate. See [browser/netsurf/include/libcss/fpmath.h](browser/netsurf/include/libcss/fpmath.h) (fixes114) for the reference pattern. Pure int32 multiplies and divides are fine, the miscompile is specifically the 64-bit shift-multiply path. **Any code doing `int64_t` or `long long` fixed-point math on CW8 PPC is suspect** and needs the same treatment or a confirmation that operands stay small enough that the miscompilation is harmless (e.g. `INTTOFIX(128)` happened to work because `128 >> 10 = 0`, which is the correct hi word by coincidence).
- **Mac CR line endings** are required for all `.c` / `.h` / `.r` files in the project. Convert with `sed 's/$/\r/' | tr -d '\n'` before packaging.
- **TextEdit (`TENew` / `TEDispose`) crashes with dsMemWZErr if WRefCon is not initialized before the first call.** The crash happens because `GetWRefCon` returns garbage on a fresh window and TextEdit dereferences it. Safe pattern: `SetPort(window)` then `SetWRefCon(window, 0)` (or to a valid struct pointer) before calling `TENew`. Once this is set, TextEdit is fully usable for the URL field and other text input widgets.
- **`kWindowStandardHandlerAttribute`** intercepts update events and leaves windows blank. Do not pass it to `CreateNewWindow`.
- **Synchronous `browser_window_schedule_reformat` during resize causes infinite layout loops.** Never call reformat directly from the grow box handler. Instead set a `needs_reformat` flag on `struct macsurf_window` and handle it in the next `nullEvent` pass. Add a `reformat_in_progress` re-entrancy guard that logs and returns if a reformat call arrives while one is already running.
- **TextEdit field activation requires explicit `TEActivate` on window activation and `TEIdle` on every `nullEvent` pass** for the caret to blink and the field to accept keystrokes. `TEKey` must be gated by a `url_field_active` flag so Return and Escape don't accidentally route as typed characters.
- **libcss lexer tokenizes `--foo` as two tokens without the keystone fix.** The original CSS 2.1 grammar allowed one leading dash for vendor prefixes. Custom properties use two. libcss's `CDCOrIdentOrFunctionOrNPD` state needs a branch where `--` followed by `startNMStart(c)` appends and continues into `IdentOrFunction` rather than rewinding to emit CHAR. Without this, the 19 `:root` definitions and 219 `var()` references in a typical modern theme drop at tokenization before any parser logic runs. Fixed in fixes139 ([browser/libcss/src/lex/lex.c](browser/libcss/src/lex/lex.c)).
- **Force-sticky title bar probes clobber each other, last writer wins.** If multiple rounds of code add `macsurf_debug_set_title_force` or `log_int_force` probes without stripping predecessors, the latest writer overwrites everything earlier in the same reformat cycle. Non-force `MS_LOG` cycling through different labels (e.g. `plot rect ↔ plot clip`) indicates no sticky is latched, which usually means the expected code path is dead. Strip upstream stickies before adding new diagnostics.
- **Fix zips only refresh the files they ship.** If a diagnostic probe was added to file X in an earlier round and subsequent zips don't ship X, the probe persists on the Mac across rounds even after removal from the Linux tree. Phantom output with no Linux-grep hit means the Mac copy of the file is out of sync with Linux. Ship the affected file explicitly to resync (fixes137 did this for `html.c` / `layout.c` / `box_construct.c`).
- **`row-gap` shares computed-style storage with `column-gap` (fixes148).** `css__parse_gap` and `css__parse_row_gap` both emit `CSS_PROP_COLUMN_GAP` bytecode. Consequences: (a) single-value `gap: N` and standalone `row-gap: N` both work as expected because they set one value the layout reads on both axes; (b) two-value `gap: A B` loses the first value, column-gap ends up equal to `B` and `A` is discarded; (c) `css_computed_row_gap` accessor does NOT exist, `layout_flex.c` reads `css_computed_column_gap` for both axes (`ctx->main_gap` and `ctx->cross_gap` both derive from it). Full-fidelity split requires adding CSS_PROP_ROW_GAP as a real property: ~17 files incl. the bit-packed `autogenerated_computed.h` struct, where a new field must be allocated in a free bit slot (word 15 has 27 free bits as of fixes116; word 14 bottom 5 bits are FULL). That work was scoped and deferred, it is not a 1-round change because offset miscounts in propset.h/propget.h silently corrupt unrelated properties. Defer until a dedicated bit-packing audit round.
- **Sub-int32 scalar fields in `css_computed_style_i` create non-deterministic padding (fixes151b).** Adding a `uint8_t`, `uint16_t`, or any non-4-byte scalar between two `int32_t` fields in `_i` makes the compiler insert padding bytes for next-field alignment. Cascade code writes the field byte, but nothing writes the padding, different cascade paths can leave padding with different garbage values (calloc zeroes it at allocation, but compose results constructed via copy-then-modify may carry source padding into the destination). Arena `memcmp(&a->i, &b->i, sizeof(_i))` then flags logically-equal styles as different → intern duplicates → use-after-free in `css_computed_style_destroy` at end-of-convert (crash signature: log ends right after `content broadcast READY`, no `reformat:` line ever fires). **Always use `int32_t` for scalar fields in `_i` so they self-align.** This is the same root cause as fixes117's inline track array crash, just with a smaller field. Defensive clamp to the intended range in the public accessor.

- **Never add multi-byte non-bit fields to `css_computed_style_i` for properties whose value isn't byte-deterministic across cascade paths.** libcss's arena interning at [src/select/arena.c:104](browser/libcss/src/select/arena.c#L104) does a raw `memcmp(&a->i, &b->i, sizeof(struct css_computed_style_i))` over the whole inner struct to find duplicate styles. Pointer fields, content arrays, font-family lists, etc. live in the OUTER `css_computed_style` struct and have dedicated `arena__compare_*` functions for logical comparison. Inline arrays in `_i` (e.g. `int32_t macsurf_grid_tracks[8]`) get bit-compared, and any cascade path that doesn't write byte-identical values for logically-equal styles will cause intern hash conflicts. The visible symptom is a crash in `css_computed_style_destroy` iterating `style->content` (the `::before`/`::after` content array), because the intern table fills with duplicates that get destroyed while still referenced. fixes117 hit this exact failure mode. **The fix pattern for variable-size or per-cascade-divergent data:** add a pointer field to the OUTER `css_computed_style`, allocate during cascade, add a comparison function in `arena.c` like `arena__compare_computed_content_item`, register it in `css__arena_style_is_equal`. Bit-packed flags in `bits[N]` are fine because every cascade path writes the bit slot deterministically.
- **`dispatch.c` and `s_dispatch.c` both define `prop_dispatch[]`, always ship BOTH.** Commit `a2f5656d` renamed `libcss/src/select/dispatch.c → s_dispatch.c` for CW8 flat-namespace deduplication. Commit `02da50f5` later re-added `dispatch.c` as a stale snapshot. Both files now exist on Linux. The Mac side compiles `s_dispatch.c` (basename match in CW8 flat folder per the rename intent); the Linux `.mcp` still references `dispatch.c` but never gets built. Result: shipping a dispatch update via just `dispatch.c` is invisible to the Mac, and only updating `properties.h` (which bumps `CSS_N_PROPERTIES`) without also updating `s_dispatch.c` produces an **out-of-bounds read on the dispatch table in `set_initial`** at the new property's index, crash signature is unmapped-memory exception with PC at a garbage address (e.g. `0x68F168F0`), stack trace shows `set_initial → 0xGARBAGE`. **Whenever a property is added to `CSS_PROP_*`, edit BOTH `dispatch.c` AND `s_dispatch.c` to add the matching entry, keep them byte-identical, and ship BOTH in the fix tar.** Discovered shipping fixes116; documented so future agents don't repeat the diagnosis. The duplicate-symbol concern (two files define `prop_dispatch`) doesn't fire because only one is in the build at a time.
- **UPP macro override on CarbonLib is unsafe, don't cast function pointers to UPPs.** CarbonLib's `TrackControl` / `InstallEventHandler` / etc. dispatch action procs through MixedMode: the UPP argument is expected to be a **RoutineDescriptor** (pre-Carbon style, still used by CarbonLib 1.x), not a raw PPC function pointer. Overriding `NewControlActionUPP(proc)` to `((ControlActionUPP)(proc))`, on the theory that "Carbon UPPs are just native function pointers", is correct only for Mach-O Carbon.framework on Mac OS X, **not** for CarbonLib on OS 9. Under CarbonLib, MixedMode reads "descriptor fields" from the function-entry bytes, resolves a routine address (typically 0 or garbage), and executes `bl 0`. Crash signature: PC in very low memory (e.g. `0x00000008`) with LR matching (e.g. `0x00000004`), because `bl` at address 0 sets LR=4 and CPU walks forward through low-memory globals until first illegal opcode. CurApName is often **`CodeWarrio...`** because CW runtime captures the low-memory trap. The override in `browser/netsurf/frontends/macos9/window.c` was removed in fixes147 after it caused scroll-bar clicks to crash on every attempt. **Correct approach:** avoid the action-UPP path entirely, call `TrackControl(ctrl, pt, NULL)` and respond on return using the `ControlPartCode` from `FindControl` plus `GetControlValue()` for live-tracking CDEFs. Or if a UPP is genuinely required, let CW8's Universal Interfaces expand the macro normally (CarbonLib does export `NewRoutineDescriptor` / `DisposeRoutineDescriptor`; they are deprecated in Mach-O Carbon but present and functional in CarbonLib). See [browser/netsurf/frontends/macos9/window.c](browser/netsurf/frontends/macos9/window.c) `macos9_window_handle_scrollbar_click`.
- **Appearance live-tracking scroll bar CDEF (`kControlScrollBarLiveProc = 386`) is unsafe on real G3/G4 hardware.** `TrackControl(ctrl, pt, NULL)` on a proc-386 control crashed into MacsBug with IDENT pointing at an Appearance Manager internal symbol (`hD*` prefix). The crash is not reproducible in SheepShaver, emulated CarbonLib tolerates the same call path that real hardware rejects. fixes147's UPP macro override removal did not close the crash; fixes159 swapped to the non-live Appearance CDEF (`kControlScrollBarProc = 384`), which is crash-free because its CDEF does not do per-frame app callbacks during thumb drag. The live-track path that proc 386 uses on real hardware appears to corrupt or dispatch through bad state that only manifests outside the emulator. **If a new Control Manager feature needs live-track behavior, do NOT reach for proc 386 without a reproducer and a MacsBug trace**, the crash is hardware-specific, not reproducible from Linux, and the only path forward is direct-hardware `wh`/`sc`/`ip` capture to identify the corrupted state inside the CDEF. See [browser/netsurf/frontends/macos9/window.c](browser/netsurf/frontends/macos9/window.c) near `macos9_window_handle_scrollbar_click` and the two `NewControl` calls for the current setup.
- **`int *` and `int32_t *` (i.e. `long *`) are INCOMPATIBLE pointer types in CW8 PPC.** On Mac PPC, `int32_t = long` (not `int`). CW8 is strict: `int *` and `long *` are treated as distinct types and passing one where the other is expected is a hard error ("illegal implicit conversion from 'long *' to 'int *'"). Common pattern: autogenerated libcss parse property files have `int32_t *ctx` in their signatures, but a shared utility function (`css__parse_calc`) declared `int *ctx`. Fix: make all signatures consistent, change the utility to `int32_t *ctx`. Applied in fixes262 to `utils.h`, `utils.c`, `p_utils.c`.
- **Headers that use `nserror` (or any typedef from `utils/errors.h`) must include `utils/errors.h` before first use.** `netsurf/layout.h` and `netsurf/window.h` both use `nserror` in function-pointer-type struct members without including `utils/errors.h`. When any `.c` file includes either header before `browser_window.h` (which includes `utils/errors.h`), CW8 sees `nserror` as an undeclared identifier, treats it as implicit `int`, then conflicts when `errors.h` later defines `typedef enum {...} nserror`. Fix: add `#include "utils/errors.h"` at the top of any public header that uses `nserror`. Applied in fixes262.
- **`"select/calc.h"` relative include from within `autogenerated_computed.h` fails on CW8.** `autogenerated_computed.h` is in `browser/libcss/src/select/`. It included `"select/calc.h"`. CW8's local-directory lookup for this resolves to `browser/libcss/src/select/select/calc.h`, doesn't exist. The correct fix: use `"calc.h"` (no path prefix) since `calc.h` is in the same directory. Do NOT rely on access-path traversal for relative includes within headers, use the minimum path prefix needed from the file's own directory. Applied in fixes262.
- **CW8 access paths with `AlwaysSearchUserPaths=true` intercept `<time.h>` via `sys/` directory.** If the `macos9:sys:` directory is in the user access paths and contains `time.h`, every `#include <time.h>` project-wide finds our `sys/time.h` first, including inside `sys/time.h` itself (circular). The old `sys/time.h` had `#include <time.h>` which was guarded out by its own circular loop, leaving `struct tm` and `localtime` never declared. Fix: remove the circular include and inline `struct tm` + `time_t` + `localtime`/`gmtime`/etc. declarations directly in the file. Applied in fixes264.
- **Stub headers must use the same include guard as the real header they shadow.** If a stub uses its own guard (e.g. `LIBWAPCAPLET_LIBWAPCAPLET_H`) but the real header uses a different guard (e.g. `libwapcaplet_h_`), BOTH files can be included in the same translation unit. The stub processes first, sets its own guard. Later in the same TU, the real file is found via a different access path, its guard is unset, it processes too, causing "illegal name overloading" for every type redefined. Fix: always make the stub's guard identical to the real header's guard. Applied in fixes264 to `libwapcaplet/libwapcaplet.h` (guard `libwapcaplet_h_`) and `css/utils.h` (guard `NETSURF_CSS_UTILS_H_`).
- **CW8 cannot resolve relative includes from within headers found via access paths (simple names excepted).** When a header found via an access path does `#include "subdir/foo.h"`, CW8 cannot find `foo.h` because the local-directory context is lost. Simple basenames like `#include "foo.h"` where `foo.h` is in the same directory generally work. Path prefixes (e.g. `#include "utils/list.h"`) fail. Fix: make headers self-contained or use access paths that put the target file within the same search directory. Applied in fixes264 to `shims/dirent.h` (inlined mac_dirent.h content instead of `#include "mac_dirent.h"`); prior in fixes261 by removing unused `#include "utils/list.h"` from `event.h`.
- **Carbon event classes have per-environment availability, check Apple's `CarbonEvents.h` before registering any handler.** Events added in Mac OS X 10.0+ that were never back-ported to CarbonLib (e.g. `kEventMouseWheelMoved`) will register without error but never dispatch, and CarbonLib's dispatcher destabilizes when something downstream tries to deliver an event whose class it doesn't know. The handler code will look correct in review (pascal calling convention, proper UPP, initialized EventTypeSpec, explicit return paths, all five "classic bugs" can be absent), run in hardware tests as "no crash from our code," and get blamed for illegal-instruction crashes at heap-looking addresses that are actually CarbonLib walking uninitialized dispatch state. Apple's `CarbonEvents.h` marks each event enum with either `Mac OS X: 10.x+ in Carbon.framework` AND `CarbonLib: 1.x+`, or `CarbonLib: not available`, respect the annotation. If `CarbonLib: not available`, the platform cannot deliver that event at all, and the correct fix is to not install a handler, not to debug the handler. See [browser/netsurf/frontends/macos9/macos9_wheel.c](browser/netsurf/frontends/macos9/macos9_wheel.c) and [docs/usb-overdrive.md](docs/usb-overdrive.md) for the wheel-event case study (fixes134 → fixes140).
- **Suppress Carbon.h's internal header chains by predefining the target header's own guard.** Carbon.h:130 includes InternetConfig.h, which at line 271 uses `AliasRecord` by value inside a struct, failing if Aliases.h hasn't been processed yet. Attempting to pre-include Files.h + Aliases.h before Carbon.h does NOT work because Files.h itself internally chains back to Carbon.h on CW8's SDK, re-entering the same problem. The clean solution: define the unwanted header's own include guard before including Carbon.h. For InternetConfig.h the guard is `__INTERNETCONFIG__` (Universal Interfaces convention: `#ifndef __HEADERNAME__` / `#define __HEADERNAME__`). `#define __INTERNETCONFIG__` before `#include <Carbon.h>` causes Carbon.h:130's own guard check to see the symbol already defined and skip the entire file. Safe for MacSurf because it never uses the Internet Config Manager API. Applied in [browser/netsurf/frontends/macos9/macos9.h](browser/netsurf/frontends/macos9/macos9.h) (fixes265). Generalizes: any Carbon.h sub-header that causes problems can be suppressed this way if the functionality is unused.
- **macsurf_prefix.h's global mac_dirent.h injection causes struct dirent redefinition if any .c file also includes `<dirent.h>`.** macsurf_prefix.h has `#include "mac_dirent.h"` inside `#ifdef __MWERKS__`, which processes for every CW8 TU and sets guard `MAC_DIRENT_H`, defining `struct dirent` and `MAC_DIR`. If the same TU later includes `<dirent.h>` → `shims/dirent.h` (guard `MACSURF_DIRENT_H`, different guard), CW8 processes `shims/dirent.h` and redefines `struct dirent`. Fix: make the overlapping definitions in `shims/dirent.h` claim the same `MAC_DIRENT_H` guard. Wrap the conflicting struct/typedef/function declarations in `#ifndef MAC_DIRENT_H` / `#define MAC_DIRENT_H` / `#endif`. Whichever file processes first claims the guard; the other skips its definitions. Applied in [browser/netsurf/frontends/macos9/shims/dirent.h](browser/netsurf/frontends/macos9/shims/dirent.h) (fixes265). General rule: when prefix.h injects a header globally, any shim for the same type family must use the injected header's guard, not a new one.
- **`#define NETSURF_LOG_H` in macsurf_prefix.h also suppresses the `nslog_ensure_t` typedef that log.c needs.** macsurf_prefix.h defines `NETSURF_LOG_H` to block log.h (which has GCC-style `##args` variadic NSLOG and GNU `__attribute__` annotations, both CW8-incompatible). But log.c includes log.h to get `typedef bool(nslog_ensure_t)(FILE *fptr)`, which appears in the signature of `nslog_init`. With the guard set, log.h is skipped and `nslog_ensure_t` is undeclared, causing `nslog_init` to fail with `')' expected` and every subsequent function in log.c to fail with `illegal function definition`. Fix: add the typedef directly to macsurf_prefix.h, after `#include <stdbool.h>` and `#include <stdio.h>` (both of which provide `bool` and `FILE`). Applied in [browser/netsurf/frontends/macos9/macsurf_prefix.h](browser/netsurf/frontends/macos9/macsurf_prefix.h) (fixes265). General rule: when suppressing a header via its guard, audit the suppressed content for typedefs needed by the files that include it.
- **Carbon.h chains deeper than InternetConfig.h, suppress each problematic sub-header via its own guard.** fixes265 suppressed InternetConfig.h; fixes266 found MacWindows.h (via LowMem.h→CoreServices.h) needs AliasHandle, KeychainCore.h uses AliasHandle in function prototypes, and ATSLayoutTypes.h (via ApplicationServices.h→Carbon.h:25) pulls in SFNTLayoutTypes.h which uses C11 anonymous members. Fix: pre-define `__ALIASES__` (with minimal AliasHandle forward declarations), `__KEYCHAINCORE__`, `__ATSLayoutTypes__` before `#include <Carbon.h>` in `macos9.h`. Pattern: whenever a new cascade of errors says "included from: SomeHeader.h:N → ... → macos9.h:45", find SomeHeader.h's include guard (Universal Interfaces uses `#ifndef __HEADERNAME__`) and add `#define __HEADERNAME__` before Carbon.h. The AliasHandle forward declarations (`struct AliasRecord; typedef struct AliasRecord *AliasPtr; typedef AliasPtr *AliasHandle;`) are required because MacWindows.h uses AliasHandle in function parameter types and the type must be complete enough to parse even with the full Aliases.h suppressed.
- **ns_time.c's `#ifndef _TIME_H` local struct tm workaround causes redefinition.** The original workaround defines `struct tm` inline if MSL's `<time.h>` hasn't been included yet. But the check uses `_TIME_H` (MSL's guard) which the workaround block never sets. So: (a) the block defines struct tm; (b) `utils/ns_time.h` later includes `<time.h>` → MSL defines struct tm again → redefinition error. Fix: replace the entire `#ifndef _TIME_H ... #endif` block with `#include <time.h>` at the top of ns_time.c. MSL's time.h then sets `_TIME_H` before the chained include in ns_time.h arrives. Applied in fixes266.
- **Forward enum declarations (`enum foo;`) are illegal in C89 / CW8.** Any C99 header that uses `enum foo;` to forward-declare an enum type will trigger "undefined identifier 'foo'" in CW8. Fix: replace the forward declaration with an `#include` of the header that provides the full enum definition, placed before the forward declaration would have been. Applied in fixes266 to `content/handlers/html/form_internal.h` where `enum browser_mouse_state;` was replaced by `#include "netsurf/mouse.h"` (which is already included a few lines later).
- **Custom fetchers MUST self-free via `fetch_remove_from_queues` + `fetch_free` after every terminal callback.** NetSurf core does NOT call `ops.free` on its own, every reference fetcher (curl.c lines 1404+1635, file.c:828-829, about.c:731-732, data.c:314-315, resource.c:469-470, css_fetcher.c:276-277, javascript/fetcher.c:188-189) self-invokes BOTH `fetch_remove_from_queues(handle)` and `fetch_free(handle)` immediately after `fetch_send_callback(FETCH_FINISHED|FETCH_ERROR|FETCH_REDIRECT)`. Calling only `fetch_free` is **worse than calling neither**, the struct gets freed but stays in `fetch_ring` as a dangling pointer, and the next `RING_GETSIZE` walks freed memory. After both calls, the handle is invalid; do not touch it (or `ctx->parent`) again. **Aborted-while-queued cleanup:** `ops.abort` typically just sets a deferred-cleanup flag (cooperative model, mirroring curl's `inside_curl` flag). The poll loop must check the abort flag BEFORE any state-based early-return, otherwise aborted-queued fetches stay in the ring forever, observed as "browser stops after about three pages" when sub-resource fetches get aborted mid-page-load. Symptom in MacsBug: `fetch_dispatch_jobs` shows `all_active=N` even after all visible fetches have logged FETCH_FINISHED. Applied in fixes102 (added fetch_free), fixes103 (added fetch_remove_from_queues), fixes104 (aborted-while-queued HTTP), fixes105 (same fix for stub fetchers, `macos9_fetcher_stubs.c`). Audit any new fetcher addition against the table in CLAUDE.md's v0.5 build state entry.
- **C89 does not allow casting to union types, `(union_type)expr` is a GNU extension.** libcss's `autogenerated_destroy.inc` resets properties to zero using `set_width(style, 0, (css_fixed_or_calc)0, CSS_UNIT_PX)` where `css_fixed_or_calc` is a `typedef union`. CW8 rejects this with "illegal explicit conversion from 'int' to 'union'". The C89 fix is a compound-statement local variable: `{ css_fixed_or_calc foc_zero_; foc_zero_.value = 0; set_width(style, 0, foc_zero_, CSS_UNIT_PX); }`. The block's opening brace creates a new scope where the declaration is legal at the top. When scanning autogenerated files for C89 compliance, `(typename)0` where `typename` resolves to a union typedef is a silently-passing grep pattern, `grep '(css_' catches it but only if you know to look for union typedefs specifically. Applied in [browser/libcss/src/select/autogenerated_destroy.inc](browser/libcss/src/select/autogenerated_destroy.inc) (fixes265).


## CLAUDE.md Maintenance

**This file must be kept current. It falls out of date fast when not actively maintained, and stale context causes agents to repeat solved problems.**

Update CLAUDE.md as part of every round that changes project state:

- When a blocker is resolved, remove it from "Known Issues" or "Current Blocker" immediately
- When a new class of bug is identified (like CW8 PPC `long long` miscompile), add it to "Known Gotchas" with the concrete reference pattern
- When a new subsystem lands (JS engine, chrome, image handlers), add a top-level section for it
- When a file count or project structure changes materially, update the "Project File List" section
- When the build state advances (v0.2 → v0.3 etc.), update the "Build State" section

The goal is that any new agent reading CLAUDE.md at the start of a session has an accurate picture of where the project actually stands, not where it was three rounds ago. If the file has drifted from reality, fix it before doing any new work.
