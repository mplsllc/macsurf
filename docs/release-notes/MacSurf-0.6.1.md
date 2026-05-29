# MacSurf 0.6.1 — small-response fix

**Released:** 2026-05-25
**Codename:** First-Light-Patch
**Engine HEAD:** fixes230
**Verified on:** Power Macintosh G3 iMac, Mac OS 9.2.2

---

## The headline

**Pages with short responses load again.** v0.6 shipped with a one-line ordering bug in the HTTPS fetcher: when the origin closed the TLS connection right after sending a small response, the fetcher retried *before* reading the buffered decrypted bytes BearSSL was already holding. Three retries, three discards, then `FETCH_ERROR` → `about:fetcherror`.

Symptom: opening the browser landed on the about page, even though the home URL was perfectly fetchable. Large pages (advanced.php at 2903 bytes) worked because they needed multiple pump cycles naturally — by the time `Closed` fired we had already drained the body. Small pages (404s, 304s, the bare mactrove homepage, favicons, redirects) hit the bug every time.

---

## What's fixed

### fixes230 — close-retry deferred until after Read

[browser/netsurf/frontends/macos9/macos9_https_fetcher.c](../../browser/netsurf/frontends/macos9/macos9_https_fetcher.c)

**Root cause.** `hctx_poll` checked `ev == kOSTLSEventClosed` and triggered a retry *before* the OSTLS_Read pass. nginx for small responses sends body and `close_notify` in one batch; BearSSL decrypts both inside a single `OSTLS_Pump`. The pump returns `kOSTLSEventClosed` with the response sitting in the read buffer. The old code retried without touching that buffer.

Diag signature from the bug report log: `ot_recv bytes=4159` (handshake plus a small post-handshake delivery), `br_state=24` = `BR_SSL_RECVAPP | BR_SSL_SENDAPP` (BearSSL has decrypted data ready), `status=0` (we never parsed any of it).

**The fix.** Move the Closed-retry block from before-Read to after-Read. Read now always gets a chance to drain pending bytes. Only retry if the response is still pre-body (state != HS_BODY) after the read completes.

**Side benefit.** `large` responses keep working exactly as before. `small` responses now parse cleanly and render. The HTTP/1.0-style "peer closed with no Content-Length" finish (already in the read block) is unchanged.

**Plus a small instrumentation fix.** The setup log line printed `path=%.40s` literally because `macsurf_debug_log_writef` doesn't implement printf precision specifiers; it's now `%s` so the actual URL path lands in the log.

---

## Verified on hardware

- **Power Macintosh G3 iMac, Mac OS 9.2.2, CarbonLib 1.5+** — mactrove.com home page now renders on cold boot, no about-page detour.

---

## Known limitations carried over from v0.6

All of v0.6's known limitations remain:

- Google / Facebook / Tier-1 fingerprinted endpoints still reject our TLS ClientHello (BearSSL's JA3 fingerprint, not browser-shape)
- HTTPS cache HIT serving still disabled (re-enable scheduled for v0.6.x)
- Direct-navigation to image URLs still blank
- All v0.5 known limitations carry forward unchanged

See [MacSurf-0.6.md](MacSurf-0.6.md) for the full v0.6 notes.

---

## Download

- **[MacSurf.sit](https://github.com/mplsllc/macsurf/releases/download/v0.6.1/MacSurf.sit)** — ready-to-run StuffIt archive. Expand on Mac OS 9.1+ with CarbonLib 1.5+ and launch.
- Source: this repository at tag `v0.6.1`.
