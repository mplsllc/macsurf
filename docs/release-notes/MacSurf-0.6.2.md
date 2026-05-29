# MacSurf 0.6.2 — Speed run

**Released:** 2026-05-25
**Codename:** Speed-Run
**Engine HEAD:** fixes263
**Verified on:** Power Macintosh G3 iMac, Mac OS 9.2.2

---

## The headline

**Cold-load mactrove.com went from 30+ seconds to ~2-3 seconds.** v0.6.2 is a focused performance + correctness sprint on top of v0.6.1. ~33 fix rounds (fixes231 through fixes263) covering HTTPS keep-alive, persistent dead-host blocklist, on-disk cache HIT serving, partial-body salvage, friendly fetch-error page, HTTPS-default URL submit with HTTP fallback, deferred PNG decode at display resolution, TLS session resumption, TCP_NODELAY, and ~80% reduction in log overhead.

The browser feels different. Type, click, and the page is there.

---

## Headline numbers — cold mactrove.com load

|                                    | v0.6.1   | v0.6.2     |
| ---------------------------------- | -------- | ---------- |
| Click → finish_conversion          | ~30 s    | **~2 s**   |
| Fonts.googleapis timeout (cold)    | 15 s × 2 | 4 s × 1, then 17 ms FAST-FAIL forever |
| Warm reload (cache HIT)            | n/a      | **<200 ms** |
| Per-PNG decode at native           | 56 forced | deferred to display-size on first paint |
| FlushVol-per-cache-write           | ~500 ms wasted | gone |
| Log-write overhead per cold load   | ~1 s of FSWrite | **~10 ms** (4 KB buffered) |
| Cold handshakes (mactrove session) | 34       | **4**    |

Net effect: **35–50 seconds of wall-clock savings per cold mactrove load**, plus warm reloads that feel instantaneous.

---

## What landed (by area)

### HTTPS transport

- **fixes231 / 232a — Keep-alive pool.** 16-entry `OSTLSConnection` cache keyed by `host:port`. `HS_QUEUED` first tries the pool; on hit, skip handshake entirely and jump straight to `HS_SEND_REQ`. ~700 ms ECDHE keygen + cert chain validation saved per reuse. Paired with a `started` flag (fixes232a) so NetSurf's `max_fetchers_per_host` throttle gates **dispatch**, not just setup — pool actually catches reuses on parallel sub-resource loads.
- **fixes234 — Bigger read + pump.** `READ_CHUNK` 1 KB → 8 KB, `PUMP_STEPS` 8 → 32. Body delivery ceiling lifts from ~60 KB/s to ~480 KB/s; BearSSL decrypts a full TLS record per pump instead of being yielded mid-decrypt.
- **fixes235 — 4 s no-progress timeout.** Was 15 s; the only thing that was waiting that long was fingerprint-rejected hosts.
- **fixes236 / 244 — Dead-host blocklist + success guard.** Hosts that time out get a session-scope blocklist (`https: dead-host ADD` ... `FAST-FAIL`). The success-guard refuses to blocklist any host that *ever* fetched successfully — transient timeouts on healthy origins (mactrove on a slow moment) never poison.
- **fixes246 — Pool entry TTL.** Entries older than 20 s get disposed at take-time, dodging server idle-close + stale-write failures.
- **fixes252 — TCP_NODELAY.** Disables Nagle's algorithm post-OTConnect. Saves ~200 ms per cold connection from Nagle/delayed-ACK interaction.
- **fixes254 — TLS session resumption.** 16-entry session-parameters cache keyed by `host:port`. After every successful handshake, `br_ssl_engine_get_session_parameters` saves the session ID + master secret + cipher; before the next handshake to the same host, `br_ssl_engine_set_session_parameters` injects it so the ClientHello carries the session ID. Server-supported abbreviated handshake saves ~700-1200 ms per pool-expired reconnect.
- **fixes255 — Salvage threshold + don't-pool-stalled.** Partial-body salvage now requires ≥ 512 bytes (tiny 200-byte JA3-reject responses aren't useful content); salvaged connections are flagged `keep_alive_ok = 0` so the dead-host blocklist still catches them and the stalled connection doesn't get pooled.
- **fixes256 — Persistent dead-host blocklist.** `deadhosts.txt` in the cache folder; saved after every `dead_host_add`, preloaded at startup. Cold-launch first attempt to fonts.googleapis.com now fast-fails in ~17 ms instead of paying the 4 s timeout.
- **fixes262 / 263 — Auto-upgrade FALLBACK actually works.** Two bugs from the v0.6.1 → v0.6.2 work fixed: stack-buffer dangling pointer for the redirect URL (262), and llcache's `unsupported redirect %d` rejection because the FETCH_REDIRECT carried `http_code = 0` instead of 301 (263). Now `classic.mactrove.com` and `macintoshgarden.org` (HTTP-only retro sites) auto-fall-back to HTTP and load.
- **fixes241 — Slot pool 64.** Setup is called for every queued fetch up-front; image-heavy pages were hitting the 32-slot ceiling and silently failing sub-resources.

### Cache + storage

- **fixes237 — HTTPS cache HIT path re-enabled.** Disabled in v0.6 because the synthetic `FETCH_HEADER` dispatch included an `"HTTP/1.1 200"` status line that confused `html_create`. Now matches the HTTP fetcher's working pattern (Content-Type header only + `fetch_set_http_code`). Warm reloads of mactrove serve from disk in ~17 ms.
- **fixes248 — FlushVol off in cache_store.** Was firing per file × ~20 stores per cold load = ~500 ms of disk-sync wait. HFS's normal write-back is sufficient; explicit flush happens at app quit.

### Layout + render

- **fixes240 — Reformat coalescing.** `browser_window_schedule_reformat` now uses a 50 ms delay (was 0). Resource-arrival bursts during page load (each sub-resource triggers a reformat) dedup into single reformats at burst tail. ~5 s saved per cold load (was ~44 redundant reformats × 250 ms).
- **fixes257 — Image fade fix.** Box-filter pre-downscale (fixes203) was disabled in v0.6 during a dark-grey investigation that turned out to have a different root cause; re-enabled and threshold lowered from 3× to 1.5×. mactrove's 1058×245 logo at 400×92 (2.6× downscale) now pre-downscales sharply instead of going through CopyMask's 1-bit-mask-loses-pixels fade path.
- **fixes239 — Hover debounce.** `macos9_poll_mouse_hover` throttled to 10 Hz; node-boundary recascade calls drop from ~60 per second of mouse movement to ~10.

### Image decode

- **fixes259 — Deferred PNG decode at display resolution.** Mirrors the QT-image path. Convert phase does `lodepng_inspect` only (IHDR read); the actual decode + box-filter to display size happens on first redraw when layout's dst dims are known. Memory: native PNG buffers never held long-term. Perceived speed: page renders without waiting for N PNG decodes during convert.
- **fixes258 — lodepng CRC + Adler32 skip.** ~5-15% PNG decode speedup for free (per-byte checksums add up on G3).
- **fixes260 — Per-image cap 8 MB → 16 MB.** Mactrove's hero images at near-native display were getting skipped under 8 MB; 16 MB now covers ~2000×2000 displays with headroom.

### UX

- **fixes242 — Friendly fetch-error page.** Replaces the bare white "MacSurf" page that NetSurf's `about:query/fetcherror` was rendering. Shows the actual failed URL in a styled `<tt>` box, three plausible reasons, and a retry link.
- **fixes243 — Partial-body salvage.** Servers that send some HTTP response then stall (Google's JA3-blocked path is the classic case) get treated as HTTP/1.0-style "response ends on close" if we have ≥ 512 bytes — NetSurf renders the truncated HTML gracefully instead of routing to about:fetcherror.
- **fixes249 / 249c — HTTPS-default URL submit + HTTP fallback.** Typed bare hostnames (`google.com`, `mactrove.com`) now upgrade to `https://`. If the HTTPS fetch fails for any reason (handshake, timeout, peer-close), the fetcher emits `FETCH_REDIRECT` to `http://`, so retro HTTP-only sites still work.

### Diagnostics + log

- **fixes233 — Log timestamps.** Every line is now prefixed with `[N]` (ticks since first log) so the in-session wall-clock is visible.
- **fixes250 / 251 / 253 — Log spam silenced.** `LAYOUTPHASE` / `MCOL` / `FLEXPHASE` (~7300 lines/cold-load), `plot_rect[N]` / `plot_text[N]` / `svg_*` / `slot[N]` / `GRADIENT` (~3000 more), `update:` chrome lines + `gw_event: e=N` + `LAYOUT_CRUMB` + `SITE url=` (~600 more). ~10000 lines saved per cold load; per-category re-enable with `MACSURF_VERBOSE_LAYOUT_LOG` / `_PAINT_LOG` / `_REDRAW_LOG`.
- **fixes261 — Buffered log writes.** 4 KB chunk: ~3 FSWrite calls per `MS_LOG` → ~10 FSWrites per cold load total. Flushes on overflow, on explicit `macsurf_debug_log_flush`, or on session close.
- **fixes247 — Font probes off by default.** ~84 ms / launch saved (diagnostic-only).
- **fixes245 — Silenced `unaccept #N` counter.** Title-bar noise from NetSurf core's content-handler reject path; zero diagnostic value to users.

### HTTP fetcher

- **fixes239 — HTTP no-progress aligned to 240 ticks (4 s).** HTTPS-default routes nearly everything through the HTTPS fetcher now; HTTP fetcher's parity in case of legacy `http://` traffic.

---

## Known limitations carried forward from v0.6

- Google / Facebook / Tier-1 fingerprinted endpoints still reject our TLS ClientHello (BearSSL's JA3 fingerprint, not browser-shape) — fingerprint-blocked hosts fast-fail via the persistent dead-host list now.
- Direct-navigation to image URLs still renders blank (image content_handler dispatch at top level).
- CSS `background-image: var(--header-tile)` where `--header-tile` is set inline on a single element doesn't resolve — our libcss custom-property aggregation is document-scoped global, not per-element. Mactrove's random header background falls back to no-image.
- Stacked `background-image: linear-gradient(...), linear-gradient(...)` (mactrove's grid tile) not natively rendered — NetSurf only renders `background-image: url(...)`; our `-macsurf-gradient:` rewrite only patches the `background-color` slot.
- All v0.5 known limitations carry forward unchanged.

---

## Hardware verification

- **Power Macintosh G3 iMac, Mac OS 9.2.2, CarbonLib 1.5+** — primary development hardware, full multi-page smoke test (mactrove, classic.mactrove, macintoshgarden.org) all rendered correctly.
- **SheepShaver + OS 9.0.4** — full Carbon init + UI smoke; networking is the limitation (SheepShaver's OT TCP doesn't reach the live internet without manual ethernet config, so HTTPS fetches hit `NO_PROGRESS_TICKS` and route to about:fetcherror). Good for build-smoke gating; not a substitute for hardware-side fetcher testing.

---

## Download

- **[MacSurf.sit](https://github.com/mplsllc/macsurf/releases/download/v0.6.2/MacSurf.sit)** — ready-to-run StuffIt archive. Expand on Mac OS 9.1+ with CarbonLib 1.5+ and launch.
- Source: this repository at tag `v0.6.2`.
