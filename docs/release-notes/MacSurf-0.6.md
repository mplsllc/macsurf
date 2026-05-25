# MacSurf 0.6 — Mac OS 9, meet TLS

**Released:** 2026-05-25
**Codename:** First-Light
**Engine HEAD:** fixes229
**Verified on:** Power Macintosh G3 iMac, Mac OS 9.1

---

## The headline

**Native HTTPS works.** MacSurf 0.6 speaks TLS 1.2 directly to the modern web from a 233 MHz beige G3, with the full Mozilla CA bundle (121 trust anchors) baked into the binary. The Go TLS-stripping proxy that earlier versions required is retired from the default path.

The whole encrypted stack — BearSSL + Open Transport + the cooperative async pump — runs inside the same 16 MB application partition that hosts the layout engine, JavaScript runtime, and image decoders.

Cipher in the wild on `mactrove.com`:
`TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256` (0xCCA9). Modern, forward-secret, no AES hardware required.

This is the first numbered MacSurf release where you can point the URL bar at `https://` and get the real page back. No `http://localhost:8765` proxy on a VPS somewhere. No hand-cracked TLS workarounds. Just the browser, talking to the internet.

---

## What's new since v0.5.0

This release captures the **fixes208–229 chain**, two weeks of work split across three distinct fronts:

### 1. Native HTTPS — fixes208–215 (the macTLS integration)

- **fixes208** — `macos9_https_fetcher.c` lands (~480 lines). Implements NetSurf's `fetcher_operation_table` against the `OSTLS_New / Start / Pump / Read / Write` async API. 9-state machine (`HS_IDLE → QUEUED → STARTING → TLSING → SEND_REQ → HEADERS → BODY → DONE/FAIL`). Cooperative pump in 8-step slices yields to the rest of the cooperative-multitasking system between bites.
- **fixes209–212** — first-light against `https://mactrove.com/advanced.html` on real OS 9 hardware. Path from URL bar to rendered page works end-to-end: TLS handshake against 10 hardcoded anchors → request sent → body streamed → libcss cascade → QuickDraw paint.
- **fixes213** — browser-shape request headers + `User-Agent: MacSurf/0.2 (Macintosh; PPC Mac OS 9)`.
- **fixes214** — home URL switches to `https://mactrove.com/`.
- **fixes215** — query strings preserved through `nsurl_get_component(NSURL_QUERY)`; HTTPS slot pool 4 → 16 for Drupal pages with many sub-resources.

### 2. Diagnostics + retry — fixes216, 226–229 (the resilience layer)

- **fixes216** — `hctx_fail` now logs the failure reason via `MS_LOG`. Silent peer-closes, handshake failures, header-buffer overflows, and timeouts finally surface in the log instead of just routing to `about:fetcherror`.
- **fixes226** — full TLS-failure diagnostic dump. Every `https: FAIL` line is followed by host/port/path, `os_err`, `ot_err`, `br_err` (BearSSL error code), OSTLS state, cipher suite, pump counts, OT send/recv counters. HTTPS slot pool bumped 16 → 32 so mactrove's bursts of sub-resource fetches stop hitting `NO FREE SLOTS`.
- **fixes227** — format-string fix (`macsurf_debug_log_writef` supports only `%d`/`%ld`/`%p`/`%s`/`%%`; `%lu` and `%X` were printing literally).
- **fixes228** — auto-retry on benign peer-close. When `kOSTLSEventClosed` or `kOSTLSEventFailed` fires before we have headers, tear down and retry from `HS_QUEUED` with a fresh `OSTLS_New`. Cap at 2 retries.
- **fixes229** — retry from `HS_HEADERS` too. Cloudflare and Google CDN frequently close the first connection right after handshake; the second usually accepts cleanly. After fixes229, mactrove's cold-cache pages and most CDN-fronted endpoints recover via `RETRY 1`.

### 3. Cache extraction + reliability — fixes217, 218, 222

- **fixes217** — macTLS trust anchor bundle expanded **from 10 to 121** — the full Mozilla CCADB root bundle (curl.se / cacert.pem snapshot), 82 RSA + 39 EC across `secp256r1` / `secp384r1` / `secp521r1`. Same trust set Firefox ships with.
- **fixes218** — on-disk body cache (originally fixes172) extracted to `macos9_disk_cache.[ch]` and shared between HTTP and HTTPS fetchers. Cache STORE is live; cache HIT serving is temporarily disabled (fixes222) until the synthetic-header dispatch matches what `html_create` expects.

### 4. Title-bar + paint correctness — fixes219, 220, 225

- **fixes219** — title-bar UTF-8 mojibake fix. `macos9_gw_set_title` now routes the title string through `macos9_utf8_to_macroman` so em-dashes, smart quotes, ellipses, and bullets render correctly instead of as garbage characters. "MacTrove — Classic Macintosh software archive | MacTrove" reads as intended.
- **fixes220** — defensive `RGBBackColor(white)` before stipple `FillRect`. The stipple path inherited whatever the QD port's `BackColor` happened to be; explicit white-set keeps stipple appearance consistent.
- **fixes225** — **dark-grey wash on mactrove cleared.** Root cause: `plotters.c`'s inset box-shadow paint hard-codes `#666666` as the fallback when `box_shadow_color == 0`. Mactrove's Platinum theme `.window` cards use `box-shadow: inset -1px -1px 0 var(--platinum-bevel-dark), …` — the var()-resolved shadow colors don't round-trip through the RGB555 pack/unpack correctly, so the paint falls through to default grey and stamps an inset rect into every `.window` card. fixes225 gates the entire inset paint behind `MACSURF_INSET_BOX_SHADOW` (default 0 = disabled). Visual cost: the 1px Platinum inner-bevel on `.window` cards is gone; the cards still render with full outer chrome. V2 will trace the var()-resolution bug and re-enable the paint.

---

## Known limitations of 0.6

### TLS

- **Google / Facebook / Tier-1 fingerprinted endpoints** don't load. BearSSL's TLS 1.2 ClientHello has a JA3 fingerprint distinct from real browsers, and these origins close the TCP connection cleanly right after handshake. Workaround: none today. Long-term V2: add ALPN extension support to macTLS, or swap BearSSL for a library that ships TLS 1.3.

- **First hit to a brand-new URL on a Cloudflare-fronted origin** can fail with `peer closed before complete` even after both retries. CF's cold-path response framing (`Transfer-Encoding: chunked` + immediate `Connection: close`) races against our chunked decoder. Reloading the page once it's warmed CF usually works. Proper fix scheduled for v0.6.x.

- **Direct-navigation to image URLs** (e.g. typing `https://example.com/foo.png` into the URL bar instead of viewing an embedded `<img>`) renders blank. Image content_handler dispatch at top level isn't wired the same way as embedded sub-resource decode. v0.6.x.

### CSS

- **Inset box-shadow disabled** (fixes225) until the var()-resolved-color round-trip is repaired. Pages without inset declarations are unaffected.
- **HTTPS cache HIT serving disabled** (fixes222) until the synthetic header dispatch is reworked. Cache STORE is collecting bodies; reload still goes over the wire.

### Other

- All v0.5 known limitations carry forward unchanged (no preemptive threading, 16 MB partition ceiling, font-rendering rough on 9.1 vs 9.2.2, etc.).

---

## Hardware verification

Built and tested on:

- **Power Macintosh G3 iMac, Mac OS 9.1, CarbonLib 1.5+** — primary development hardware
- **Power Mac G4, Mac OS 9.2.2** — secondary, cleaner font rendering
- **SheepShaver / OS 9.0.4** — smoke-only

Canonical hardware-verification page: `https://mactrove.com/`. Screenshot at [screenshots/mactrove-fixes225.jpg](screenshots/mactrove-fixes225.jpg).

---

## Project-side milestones in this release

- **macTLS standalone repo archived** at fixes47 (2026-05-25). All future macTLS development happens inside `macsurf/macTLS/`. The standalone [mplsllc/macTLS](https://github.com/mplsllc/macTLS) repository remains live as a reference snapshot of how to bridge BearSSL + Open Transport on Classic Mac OS.
- **Documentation refresh** — `docs/status.md` now reflects native-HTTPS-by-default and the load-time QOL queue. README banner explicitly calls out native TLS 1.2 working as of 2026-05-25.

---

## What's next (v0.6.x / v0.7)

The load-time QOL items deferred from this round:

- **Re-enable HTTPS cache HIT serving** — biggest reload-speed win (~6s on a cached mactrove reload). Synthetic `FETCH_HEADER` / `FETCH_DATA` / `FETCH_FINISHED` dispatch needs to mirror the live header stream exactly.
- **TLS keep-alive / connection pooling** — currently every sub-resource fetch does a fresh handshake. 40 redundant handshakes × ~200ms each on a G3 = ~8s saved on cold load.
- **Reformat coalescing** — ~18 full reformats per mactrove load; batching to a tick window cuts layout work ~5×.
- **The cold-CF chunked-encoding bug** — proper fix for the small-response peer-close race.
- **NetSurf-core abort-during-nav** — investigation into why navigation while sub-resources are streaming triggers `ops.abort` on the new top-level fetch.

---

## Credits

This release was the culmination of two weeks of focused TLS work plus the entire prior 200-fix arc that brought NetSurf's rendering pipeline to a state where modern pages render correctly on a beige G3. macTLS itself is a single-author project, built on top of [BearSSL](https://www.bearssl.org/) by Thomas Pornin.

---

## Download

- **[MacSurf.sit](https://github.com/mplsllc/macsurf/releases/download/v0.6/MacSurf.sit)** — ready-to-run StuffIt archive. Expand on Mac OS 9.1+ with CarbonLib 1.5+ and launch.
- Source: this repository at tag `v0.6`. Open `browser/netsurf/frontends/macos9/MacSurf.mcp` in CodeWarrior 8 Pro and choose Build.

If your G3/G4 has been sitting in a closet, this is a good week to plug it in.
