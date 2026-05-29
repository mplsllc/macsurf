# MacSurf 1.2 — Sealed

**Released:** 2026-05-29
**Codename:** Sealed
**Engine HEAD:** fixes315
**Verified on:** Power Macintosh G3 iMac, Mac OS 9.2.2

<p align="center">
  <img src="https://raw.githubusercontent.com/mplsllc/macsurf/master/img/mactls-logo.png" alt="macTLS" width="420">
</p>

---

## The headline

**macTLS v0.x ran on a documented insecure-stub entropy source.** It was the largest known security limitation in 1.0 — the rotate-XOR placeholder lifted from BearSSL's reference scaffolding, sitting in the spot where a real CSPRNG belongs. 1.2 closes that hole. macTLS v1.0 (macEntropy v1.0) replaces it with a SHA-256 accumulator feeding BearSSL's HMAC-DRBG, fed by OT packet-arrival jitter, mouse and key-press timing from the event loop, high-resolution clock samples, and a cold-start seed file persisted across boots so the first handshake after a clean boot isn't thin.

This is what "production HTTPS" was supposed to mean. 1.0 to 1.1 was a chrome polish release; 1.0 to **1.2** is a security release.

Three pieces in scope:

1. **macTLS v1.0 (macEntropy v1.0).** Hardware-validated on G3 across four separate launches with the Stage E statistical self-test: distinct seed fingerprints per run (`94A7251B`, `52AB2050`, `665DF442`, `165814CB`) — the actual per-run-entropy proof, not just non-degenerate within a single launch. The pre-1.2 binary was using a rotating XOR seeded only from `TickCount()`; *every* TLS handshake on that build shared a predictable seed line. Replaced.
2. **POST forms work.** Through 1.0 the fetcher API quietly discarded `post_urlenc` (`(void)pu;`) on both the HTTP and HTTPS paths. Every form POST silently became a no-op: search forms on results pages, login forms, comment submissions, anything. 1.2 wires the body all the way through — DDG's "search again from the results page" (the canonical #144 repro) now actually returns fresh results.
3. **A working download manager.** Through 1.0 clicking a download link did one of two things: nothing visible, or NetSurf rendered the binary as HTML (the "weird characters in a giant page" failure). 1.2 implements all four `gui_download_table` callbacks with NavServices Save dialog, FSWrite streaming, MIME-mapped Mac type/creator codes, partial-file cleanup on error, and `Content-Disposition: attachment` detection in the fetcher so Drupal-style sites (mactrove, macintoshgarden) route through download instead of render. Hardware-verified end-to-end with a `.sit` from gardenmirror.

---

## Headline screenshot

![MacSurf 1.0 — base showcase](https://raw.githubusercontent.com/mplsllc/macsurf/master/screenshots/macsurf-1.0-home.jpg)

Same surface as 1.0 (rendering is unchanged), but now the TLS underneath is genuinely random and the form/download cliffs are gone.

---

## What landed (by area)

### macTLS — macEntropy v1.0

The pool is a running BearSSL SHA-256 context that every source sample is folded into via `br_sha256_update`. Seed extraction clones the pool (BearSSL's `br_sha256_out` is const), mixes a domain-separation tag, and finalises 32 bytes. The extracted seed is then folded back into the live pool so successive extractions are independent — the output is never the raw running hash. The 32-byte seed is injected into BearSSL's engine, which runs its own HMAC-DRBG downstream. macTLS supplies the seed material, BearSSL runs the generator. This is the standard NIST construction; the previous build implemented neither side of it.

**Sources active in 1.2:**

- **OT packet-arrival jitter at every `OTRcv`.** macTLS's `OSTLS_StirTimer` folds a fresh `Microseconds()` + `TickCount()` plus a caller hint into the pool at every byte-delivering `OTRcv`. The unpredictable arrival timing of every network packet during a fetch becomes entropy — a high-rate source genuinely jittery, needs no host cooperation, active the moment the build picks up macTLS v1.0.
- **High-resolution clock sampling.** Every pool access folds `Microseconds()` + `TickCount()` in alongside whatever else is being mixed.
- **Stack-noise probes.** Stack-allocated buffers from previous calls retain unpredictable bits; the pool folds those in as a coarse but real entropy source on a cooperative-scheduling system where stack contents reflect past activity.
- **Mouse and key-press jitter from the event loop (fixes315).** `macos9_poll` now calls `OSTLS_StirEntropy(&ev, sizeof ev)` on every `WaitNextEvent` pass. Mouse coordinates, event times in ticks, key codes, and message words all reach macTLS through the host stir seam. This is the richest source on a single-user desktop where the user is interacting with the browser.
- **Cold-start seed file (fixes315).** `OSTLS_LoadSeed()` runs at startup after OT init, reading a 32-byte seed from the Preferences folder and folding it into the pool. `OSTLS_SaveSeed()` runs at clean shutdown to persist a fresh, domain-separated seed back. The seed file is *never* trusted alone — it's only ever mixed in alongside live samples, and the persisted seed uses a different extraction tag than the seed handed to TLS, so reading the file does not reveal handshake randomness. The mechanism just makes sure the very first handshake after a clean boot isn't thin.

**Stage E statistical self-test.** macTLS includes `OSTLS_EntropySelfTest` which extracts a batch of seeds and checks for non-degenerate output: successive seeds differ, byte values spread across the range, bit balance near 50%, and emits a 32-bit fingerprint of the batch. The fingerprint MUST differ across separate launches — that's the real proof of per-run entropy, since a SHA-256 hash chain looks random regardless of input entropy. Verified 2026-05-29 on the G3 across four separate launches with distinct fingerprints.

**What this fixes from a security posture standpoint.** Before 1.2, the entropy hole meant TLS handshakes were nominally secure (full BearSSL TLS 1.2 cipher suites, 121-anchor CCADB bundle, real cert chain verification) but seeded from a predictable source. Any attacker who could observe two handshakes from the same launch could potentially recover the seed line and reproduce future handshake randomness. 1.2 makes the seed material genuinely unpredictable — every handshake draws from a pool that is being fed continuously and was warmed by a persisted seed from the previous session.

### Forms — POST body actually goes out (fixes312, #144)

Both the HTTP and HTTPS fetcher's setup functions started with `(void)o;(void)d;(void)pu;(void)pm;(void)h;` — the URL-encoded POST body and the multipart body were both being thrown away at the entrypoint. Every form POST allocated a slot, sent no body, and either failed at the server or returned a generic response.

Wiring in 1.2:

- **Both fetchers** capture `pu` at setup time into a heap-owned `post_body` copy.
- **HTTPS `build_request`** emits POST headers when `post_body != NULL` (Content-Type: `application/x-www-form-urlencoded`, Content-Length); `HS_SEND_REQ` streams `post_body` after headers via a second `OSTLS_Write` phase. `hctx_reset_for_retry` rewinds the send counter so the peer-close retry path resends the same payload.
- **HTTP `mfs_open`** emits POST request line / headers in both proxy and direct paths and sends the body via a second `OTSnd` before flipping to non-blocking. POST forces `Connection: close` because origin servers commonly reset after a POST.
- **Cache safety.** Lookup and store paths gated on `post_body == NULL` in both fetchers. POSTs are non-idempotent — the URL alone doesn't identify the response, so caching would serve stale or wrong data on subsequent GETs for the same URL.

Verified on G3: httpbin.org/post returns 200 with JSON echoing the body. DDG `/html/` POST goes 302 → `html.duckduckgo.com` GET → full results page with stylesheets and images. Search-from-the-DDG-results-page (the original #144 repro) now navigates to fresh results.

### Download manager — first implementation (fixes313 + 313a + 313b, #149)

Through 1.0 all four `gui_download_table` callbacks were complete TODOs: `create` allocated a struct, `data` dropped every chunk on the floor, `error`/`done` did nothing. Clicking a download link looked like a no-op or, when the server happened to set `Content-Type: text/html` for a download (Drupal default), rendered the binary as HTML.

**fixes313 — backing implementation:**
- `create` → `NavPutFile` Carbon save dialog with the suggested filename from NetSurf as default. On Save: `FSpCreate` with MIME-mapped type/creator (`PDF`/`CARO`, `ZIP`/`SITx`, `SITD`/`SIT!`, `JPEG`/`8BIM`, `PNGf`/`8BIM`, `GIFf`/`8BIM`, `TEXT`/`ttxt`, default `BINA`/`????`) → `FSpOpenDF` for write. `AEGetNthPtr` extracts the chosen `FSSpec` from the `AEDescList` in `reply.selection`.
- `data` → `FSWrite` each chunk; tracks `bytes_written`; throttled status-bar progress every ~16 KB (`"Downloading X.sit: N of M bytes"`).
- `done` → `FSClose`; status bar `"Saved X.sit (N bytes)"`.
- `error` → `FSClose` + `FSpDelete` (partial-file cleanup); status bar + log line with the error message.
- One active download at a time (V1). A second attempt while the slot is busy returns NULL → NetSurf surfaces as a fetch failure rather than silently overwriting.

**fixes313a — NavServices swap.** Initial implementation used `StandardPutFile` which is classic-only and didn't link under CarbonLib (undefined symbol). Switched to `NavPutFile` + `NavGetDefaultDialogOptions`, which CarbonLib provides.

**fixes313b — Content-Disposition: attachment handling.** mactrove and other Drupal-style sites serve downloads with `Content-Type: text/html` + `Content-Disposition: attachment; filename="..."`. NetSurf trusted text/html, dispatched to the HTML handler, and rendered raw binary as HTML. NetSurf's hlcache already has a download-trigger path at `content/hlcache.c:386` gated on `type == CONTENT_NONE && HLCACHE_RETRIEVE_MAY_DOWNLOAD` — the MAY_DOWNLOAD flag is set on every navigate call, so the only missing piece was making the effective type resolve to CONTENT_NONE. Fix: in `parse_headers`, detect `Content-Disposition: attachment` and override the forwarded Content-Type to `application/octet-stream`. NetSurf has no registered handler for that → `type = CONTENT_NONE` → MAY_DOWNLOAD branch fires `CONTENT_MSG_DOWNLOAD` → `download_context_create` → our `macos9_download_create` → NavPutFile.

Implementation note: `llcache_handle_get_header` returns the FIRST matching header. The fetcher couldn't just append a second Content-Type — it had to BUFFER all header lines, decide if download is forced, then replay them with the substitution. Single-pass parse + forward was restructured to parse-collect, decide, then forward-with-substitution.

Verified on G3: `.sit` download from macintoshgarden via direct HTTP — Save dialog appears, file streams to disk, "Saved PlasticInternetApps.sit (179944 bytes)" lands in the status bar.

### CSS layout — bg-attachment fixed (fixes309, #41)

`background-attachment: fixed` was parsed and cascaded but the fixes137 paint anchor returned the wrong reference frame. The helper returned `(scroll_x, scroll_y)` — page coordinates of the viewport's top-left — which the redraw caller then added to the bg-position. Net effect: the image drifted *down* on screen at the scroll rate instead of staying anchored. Fix in `macos9_get_bg_fixed_origin`: return `(content_rect.left, content_rect.top)` — window-coord origin of the content area, the frame NetSurf's redraw operates in. Image now pins to `(content_top + bg_position)` regardless of scroll position. Diagnosed via the fixes308 round which dropped one-line MS_LOG entries in the fixes137 block and showed the `origin=(0, 48) → (0, 96) → (0, 144)` drift across scroll positions.

### CSS layout — white-space pre-line (fixes307, #56)

`white-space: pre-line` is supposed to preserve newlines but collapse runs of internal whitespace to a single space. `box_construct.c`'s PRE/PRE_WRAP/PRE_LINE branch did the tab-to-space conversion but never collapsed the resulting whitespace. Added a single in-place pass after the conversion: walk the buffer, emit one space at most per run, preserve everything else as-is. Test surface on mactrove.com/t.html shows pre, pre-wrap, pre-line, and normal all rendering distinctly.

### HTTPS abort crash (fixes315, #150)

Surfaced on duckduckgo.com which loads ~40+ CSS files in parallel under `/_next/static/css/`. The CSS budget gate trips and NetSurf cancels remaining in-flight fetches. Each cancelled fetch landed in `hctx_fail` and was a roulette spin against fixes249b's "HTTPS → HTTP retro auto-upgrade FALLBACK" path: when the slot's pool_key was marked for fallback, the fail path emitted a synthetic `FETCH_REDIRECT` via `fetch_send_callback`. But at the moment `ops.abort` had already been called (NetSurf-side cancellation), `object->fetch.fetch` had been NULLed by llcache — and `llcache_fetch_redirect`'s internal `fetch_abort(object->fetch.fetch)` crashed on the NULL deref.

Fix: one-line gate. Skip the auto-upgrade FALLBACK when `c->aborted == 0`. The fallback was designed for OUR detected failures (peer close, timeout, handshake reject) — not NetSurf-initiated cancellation, where the fetch handle is already invalid from llcache's perspective. Diagnosed from a CodeWarrior stack screenshot showing `fetch_abort(f=NULL)` at the top frame with `llcache_fetch_redirect` immediately below.

---

## Issues closed since 1.0

- **#41 — background-attachment: fixed** (fixes309)
- **#56 — white-space pre-line internal-whitespace collapse** (fixes307)
- **#143 — `<img>` HTML width/height verified not reproducing** (fixes194 + the inline-style preprocessor work in the fixes202–267 window already closed it; the t.html probe confirmed)
- **#144 — POST form body** (fixes312)
- **#149 — Content-Disposition: attachment routes to download** (fixes313b)
- **#150 — HTTPS abort + auto-upgrade FALLBACK crash** (fixes315)
- **#36 — SVG fill-opacity / stroke-opacity** (fixes305 + 305a, closed during the lead-in)

Plus the macTLS chain (macEntropy Stages A → C → B → E → v1.0 → D), tracked in the [macTLS repo](https://github.com/mplsllc/macTLS) and tagged `macentropy-v1.0`.

---

## Building from source

Same as 1.0. Clone the repo, open `browser/netsurf/frontends/macos9/MacSurf.mcp` in CodeWarrior 8 Pro (8.3 update) on the Mac side, choose Build. No new access-path or library-list changes since 1.0; the macTLS objects compile in alongside everything else.

One project-file note for builders pulling 1.2 onto a 1.0 workspace: `desktop/download.c` needs to be in MacSurf.mcp (it was added as part of fixes313 — provides `download_context_create` and the four `download_context_get_*` accessors the download manager depends on).

---

## What's next

The post-1.2 queue is mostly fit-and-finish:

- **Gradient pipeline trio (#145, #147, #148)** — mactrove chrome polish, all three issues bundle into one rewrite round.
- **Inset box-shadow re-enable** — the Platinum bevel on `.window` cards. Currently disabled at fixes225 because `var()`-resolved shadow colors arrived at `box_shadow_color == 0` and the paint fell through to a hard-coded `#666666`, washing the content area dark. The cascade trace is the gate.
- **HTTP fetcher Content-Disposition mirror** — natural follow-up to fixes313b. HTTPS is the common case for downloads today but HTTP-side has the same gap.
- **Multi-slot download manager (V2)** — current cap is one active download at a time. Modeless progress window is also deferred to V2.

---

*MacSurf is a working web browser for Mac OS 9. The repo is at [github.com/mplsllc/macsurf](https://github.com/mplsllc/macsurf), macTLS is at [github.com/mplsllc/macTLS](https://github.com/mplsllc/macTLS), and the showcase home page lives at [home.macsurf.org](https://home.macsurf.org/). Bug reports and screenshots from real hardware are exactly what this project wants.*
