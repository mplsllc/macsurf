# Performance baseline — pre-fixes409 (reflow-storm clock fix)

Source: `forclaude/MacSurf Debug.log` (sessions 2026-06-04 22:39–23:00, the
pre-revert / pre-fixes409 build). This is the "before" we measure perf
improvements against. Re-run the same sites after each perf change and diff.

## Headline metric: reformats per single page load

A correct load relayouts a few times. The dead monotonic clock disabled
NetSurf's reflow throttle, so the page did a full relayout on nearly every
subresource arrival. **This is the #1 load-time bottleneck.**

| host | reformats/load (max) | page weight | subresources | fetch FAILs | outcome |
|---|---:|---:|---:|---:|---|
| mactrove.com | 432 | 10.5 MB | 59 | 38 | loads, very slow (~52s) |
| www.reddit.com | 57 | 2.97 MB | 6 | 0 | loads |
| 68kmla.org/bb | 52 | 2.5 MB | 29 | 0 | loads |
| infinitemac.org | 41 | 944 KB | 24 | 0 | loads |
| www.youtube.com | 28 | 10.5 MB | 15 | 3 | JS SPA, no useful render |
| emaculation.com | 25 | 5 KB | 1 | 1 | loads |
| x.com | 21 | 2.97 MB | 6 | 0 | JS SPA |
| macintoshgarden.org | 20 | 339 KB | 13 | 1 | loads (http) |
| www.wikipedia.org | 7 | 162 KB | 6 | 0 | loads |

## Sites that fail outright (loadability, not speed)

| host | fetch FAILs | cause |
|---|---:|---|
| macos9live.com | 18 | OT error 2008 (connect-layer) |
| mac84.net | 6 | X509_NOT_TRUSTED — valid Sectigo cert wrongly rejected |
| lowendmac.com | 2 | data-stall timeout |
| vintageapple.org | 1 | data-stall |
| vintagemacmuseum.com | 1 | data-stall |

## TLS/fetch failure fingerprints (across the whole log)

- 25× `handshake/transport failed` (state=3)
- 16× data-stall: `ot_err=2147483647 (sentinel) cipher_dec=4867` — partial
  handshake then the 4s no-progress timeout fires
- 6× `X509_NOT_TRUSTED` (all mac84.net / Sectigo)
- 44× `aborted` (navigation-cancelled, mostly benign)

## Targets after fixes409 (clock/reflow fix)

- mactrove reformats/load: **432 → expect single/low-double digits**
- mactrove wall-clock: ~52s → expect a large drop
- Other sites' reformats/load should fall proportionally.

Method: rebuild with fixes409, load the same sites, capture a fresh log,
re-run this extraction, and record the after-numbers next to these.
