# macSSL → MacSurf Integration Scope

**Branch:** `macSSL`
**Author:** Integration agent (Claude, 2026-05-24)
**Status:** Pre-implementation scope. No code wiring yet.

This document is the full scope for wiring `macSSL` into MacSurf so the
browser can perform native `https://` fetches without going through the
external `macsurf-proxy`. It supersedes [macssl-integration-notes.md](../../macSSL/docs/macssl-integration-notes.md)
and [integration-handoff.md](../../macSSL/docs/integration-handoff.md)
on MacSurf-side specifics; the macSSL repo docs remain authoritative for
the library itself.

---

## 1. What macSSL is, today

A hardware-verified TLS 1.2 client library targeting Carbon CFM / OS 9 /
PowerPC. Two public APIs:

- **v0.1 blocking** — `OSTLS_Fetch(...)`. One-shot synchronous GET. Used
  by MacSSLTest harness; **not** what MacSurf will call.
- **v0.2 / v0.3 async** — `OSTLS_New/Start/Pump/Read/Write/Close/Dispose`
  plus `OSTLS_HTTP_*` (chunked decoder, GET/POST formatters) and
  `OSTLS_CollectEntropy` (idle-loop noise feeder). **This is what
  MacSurf will use.**

Hardware-verified through Stage D on G3 / OS 9.1 (per
[memory](../../.claude memory)): library mode is the chosen path; the
"local proxy listener" experiment (Stage C) was abandoned because
Carbon CFM OT passive bind is structurally broken on this platform.

Cipher path: TLS 1.2 with ECDHE-ECDSA + ChaCha20-Poly1305 preferred
(`0xCCA9`), AES-GCM fallback. Trust anchors: ~10 embedded roots
(Amazon, DigiCert G2/G3, Google GTS R1-R4, Let's Encrypt ISRG X1/X2,
Starfield G2) valid through 2035-2038.

---

## 2. Inventory

### 2.1 macSSL files to add to MacSurf.mcp

**os9/ glue layer** — 7 files, ~7.6K LOC:

| File | Purpose |
|---|---|
| `ostls_async.c` | State machine: OTAsyncOpenEndpoint → OTBind → OTConnect → BearSSL handshake → app I/O |
| `ostls_entropy.c` | Production entropy collector (mouse/key/notifier jitter) + Stage A stub |
| `ostls_http.c` | Chunked decoder + GET/POST formatters |
| `ostls_b3_anchors.c` | Embedded trust anchor table |
| `ostls_time.c` | Mac clock → BearSSL day-count conversion |
| `ostls_log.c` | File-backed diagnostic channel (parallel to MacSurf's) |
| `ssl_engine_cw8.c` | BearSSL profile selection + CW8-specific config |

Skipped (probe-only / blocking baseline):

- `ostls_fetch.c` — blocking v0.1; the integration uses async only.
- `ostls_b1_tcp.c`, `ostls_b2_handshake.c`, `ostls_b3_handshake.c`,
  `ostls_d1_probe.c`, `ostls_mul64_probe.c`, `ostls_smoketest.c` —
  staged validation harnesses, not part of the library proper.

### 2.2 BearSSL — 294 .c files, ~1.6 MB source

Located under `macSSL/bearssl/src/{aead,codec,ec,hash,int,kdf,mac,rand,rsa,ssl,symcipher,x509}/`.
Per the CLAUDE.md library-port discipline, **all 294 files must be
added to MacSurf.mcp**. This is by far the largest single library port
in the project's history (current project total = 443 .c files; this
takes it to ~744).

Mitigation: BearSSL is already C89-clean and has a documented
`br_ssl_client_init_full` profile that omits TLS 1.3, server-side
handshake, and protocols we don't need. We may be able to drop ~30-50
files if the link references stay clean. Confirmation requires a real
link experiment, deferred to implementation.

### 2.3 New MacSurf-side file

**`browser/netsurf/frontends/macos9/macos9_https_fetcher.c`** — implements
the same `fetcher_operation_table` shape as the existing
`macos9_http_fetcher.c`, but with each per-fetch context wrapping an
`OSTLSConnection`. Estimated ~600-800 LOC; structurally mirrors the
existing HTTP fetcher's state machine (MFS_QUEUED → MFS_INIT →
MFS_CONNECTING → MFS_HEADERS → MFS_BODY → MFS_NOTIFIED → MFS_DONE), with
the OT primitives replaced by `OSTLS_Pump` / `OSTLS_Read` / `OSTLS_Write`.

### 2.4 Access paths (CodeWarrior)

Add to user paths:
- `{Project}::patrick:macsurf-source Folder:macSSL:os9:`
- `{Project}::patrick:macsurf-source Folder:macSSL:bearssl:inc:`
- `{Project}::patrick:macsurf-source Folder:macSSL:bearssl:src:`
- `{Project}::patrick:macsurf-source Folder:macSSL:bearssl:src:aead:`
- `{Project}::patrick:macsurf-source Folder:macSSL:bearssl:src:codec:`
- `{Project}::patrick:macsurf-source Folder:macSSL:bearssl:src:ec:`
- `{Project}::patrick:macsurf-source Folder:macSSL:bearssl:src:hash:`
- `{Project}::patrick:macsurf-source Folder:macSSL:bearssl:src:int:`
- `{Project}::patrick:macsurf-source Folder:macSSL:bearssl:src:kdf:`
- `{Project}::patrick:macsurf-source Folder:macSSL:bearssl:src:mac:`
- `{Project}::patrick:macsurf-source Folder:macSSL:bearssl:src:rand:`
- `{Project}::patrick:macsurf-source Folder:macSSL:bearssl:src:rsa:`
- `{Project}::patrick:macsurf-source Folder:macSSL:bearssl:src:ssl:`
- `{Project}::patrick:macsurf-source Folder:macSSL:bearssl:src:symcipher:`
- `{Project}::patrick:macsurf-source Folder:macSSL:bearssl:src:x509:`

(All non-recursive per project convention.)

### 2.5 Memory budget per HTTPS fetch

Per the macSSL doc:

| Component | Bytes |
|---|---:|
| `br_ssl_client_context` | ~10 KB |
| `br_x509_minimal_context` | ~5 KB |
| `BR_SSL_BUFSIZE_BIDI` (TLS record I/O buf) | 33,178 |
| OT endpoint state | ~2 KB |
| **Per-fetch total** | **~50 KB** |

MacSurf's Carbon partition is 16 MB preferred / 8 MB minimum. NetSurf's
default 8 concurrent fetches per host * ~50 KB = 400 KB — well under
budget. **A hard cap of `MAX_HTTPS_FETCHES = 4` (per
integration-handoff.md) keeps the worst-case ceiling at ~200 KB.**

---

## 3. The fetcher state machine

Mirrors `macos9_http_fetcher.c`'s shape so NetSurf core's
`fetch_dispatch_jobs` / `fetch_send_callback` continuations work
identically.

```
MFS_IDLE
   │
   ├─ http_setup ──────────→  MFS_QUEUED
   │                              │
   │                              ├─ http_start ──→  MFS_INIT
   │                                                    │
   │                                                    ├─ OSTLS_New + OSTLS_Start
   │                                                    └─→ MFS_CONNECTING
   │
   ├─ poll tick:
   │     OSTLS_CollectEntropy()
   │     OSTLS_Pump(conn, 6, &ev)
   │     │
   │     ├─ ev == kOSTLSEventHandshakeDone:
   │     │     OSTLS_HTTP_FormatGet(...) + OSTLS_Write(...)
   │     │     → MFS_HEADERS
   │     │
   │     ├─ state == Open && in MFS_HEADERS:
   │     │     OSTLS_Read(...) until \r\n\r\n
   │     │     parse Status-Line + headers
   │     │     fetch_send_callback(FETCH_HEADER)
   │     │     → MFS_BODY
   │     │
   │     ├─ state == Open && in MFS_BODY:
   │     │     OSTLS_Read(...) → if Transfer-Encoding: chunked,
   │     │         feed through OSTLS_HTTP_ChunkDecoderProcess
   │     │     fetch_send_callback(FETCH_DATA, decoded_buf, n)
   │     │
   │     ├─ ev == kOSTLSEventClosed && body complete:
   │     │     fetch_send_callback(FETCH_FINISHED)
   │     │     fetch_remove_from_queues + fetch_free
   │     │     OSTLS_Close + OSTLS_Dispose
   │     │     → MFS_NOTIFIED
   │     │
   │     └─ ev == kOSTLSEventFailed:
   │           map BearSSL/OT err → NSERROR_* via translate table
   │           fetch_send_callback(FETCH_ERROR, msg)
   │           fetch_remove_from_queues + fetch_free
   │           OSTLS_Dispose
   │           → MFS_FAIL
```

**Critical: self-free via `fetch_remove_from_queues + fetch_free` after
every terminal callback** (FINISHED / ERROR / REDIRECT). This is the
fixes102/103 discipline from the existing HTTP fetcher (CLAUDE.md
"v0.5 fetch system recovery" entry); the new HTTPS fetcher must follow
the same pattern or NetSurf will accumulate dangling handles.

### 3.1 Yielding

`OSTLS_Pump` is designed to never block. Per-poll-tick cost: at most
`max_steps` atomic actions, recommended 4-8. The MacSurf event loop's
existing 1-tick fast path while any fetcher is active (`macos9_fetching
|| stub_active || http_active`) extends naturally to HTTPS.

`YieldToAnyThread()` inside the notifier is not needed because the
state machine is cooperative by design — no synchronous OT call inside
Pump.

### 3.2 Timeouts

`OSTLSConfig.connect_timeout_ticks` / `handshake_timeout_ticks` default
to 30s at 60 Hz = 1800 ticks. The existing HTTP fetcher's no-progress
timeout (fixes107, 15s at 60 Hz) should apply on top for the body
phase: if `OSTLS_Read` returns 0 bytes for > 900 ticks, force MFS_FAIL.

### 3.3 Error translation

```
BearSSL / OSTLS error          →  NetSurf nserror + UI message
─────────────────────────────────────────────────────────────────────────
BR_ERR_X509_NOT_TRUSTED        →  NSERROR_BAD_AUTH  "Site certificate not trusted"
BR_ERR_X509_EXPIRED            →  NSERROR_BAD_AUTH  "Site certificate expired or system clock is wrong"
BR_ERR_X509_BAD_SERVER_NAME    →  NSERROR_BAD_AUTH  "Certificate hostname mismatch"
BR_ERR_X509_BAD_SIGNATURE      →  NSERROR_BAD_AUTH  "Site certificate signature invalid"
BR_ERR_SSL_* (record/engine)   →  NSERROR_BAD_URL   "TLS protocol error"
kOSTLSAsync_ClockBefore2000    →  NSERROR_BAD_AUTH  "Please set the Mac's date and time before browsing HTTPS"  ← important
kOSTLSAsync_HandshakeTimeout   →  NSERROR_TIMEOUT   "TLS handshake took too long"
kOSTLSAsync_PeerClosed         →  NSERROR_BAD_URL   "Server closed connection before responding"
kOSTLSAsync_OT*                →  existing OT-error mapping
```

The **clock-wrong dialog** is the single most important UX surface.
Dead PRAM batteries on G3/G4 are extremely common and a user has zero
chance of guessing "cert expired" really means "clock is wrong".

---

## 4. Scheme registration changes

**Current state** ([macos9_http_fetcher.c:1761](../../browser/netsurf/frontends/macos9/macos9_http_fetcher.c#L1761)):
the existing `macos9_http_fetcher_register` registers itself for **both
`http` and `https`** — the proxy handles both today.

**Phase 1 (integration):** Add native HTTPS fetcher in parallel.
`macos9_http_fetcher_register` keeps `http` only; new
`macos9_https_fetcher_register` claims `https`. NetSurf's `fetch.c`
matches scheme exactly, so the dispatch is unambiguous.

**Phase 2 (fallback):** When `macos9_https_fetcher` returns
`BR_ERR_X509_NOT_TRUSTED` AND the user opts in via a dialog ("This
site's certificate chain isn't in MacSurf's trusted root list. Try
the secure proxy instead?"), reissue the fetch as `http://...`
through the proxy. The proxy already does upstream HTTPS via Go's
`crypto/tls` with a full CA bundle. **Deferred** — V1 just returns
the error.

**Phase 3 (proxy retirement):** Once entropy + CA bundle work lands
(see §5), `http://` URLs can also route through the native HTTPS
fetcher with a TLS-upgrade if the proxy is down or the user
explicitly disables it. Out of V1 scope.

---

## 5. Known constraints / pre-shipping gates

Three issues from the macSSL handoff need addressing before HTTPS can
be advertised as user-facing secure:

### 5.1 Stage A entropy stub — **BLOCKER for security claim**

The current `ostls_entropy.c` Stage A entropy mixes TickCount,
Microseconds, stack address, and a fixed tag into 32 bytes. This
satisfies BearSSL's entropy gate but is **NOT cryptographically
sound** — predictable, low-entropy seeds let an observer who captures
the encrypted traffic decrypt it post-hoc.

The integration MUST NOT advertise HTTPS as production-secure until
production entropy lands (mouse-delta hashing, key-press latency,
notifier tick jitter, persisted seed). Mitigations during the V1
phase:

- **Disclaimer in title bar / about dialog** when an HTTPS fetch
  succeeds against the Stage A entropy build.
- **Title bar marker** ("MacSurf HTTPS (preview)") so users can tell
  the build apart from a future shipping one.
- **Documented limitation in README + ABOUT.txt**.

Production entropy work tracked separately in the macSSL repo
([os9/ostls_entropy.c](../../macSSL/os9/ostls_entropy.c) docstring).

### 5.2 Trust anchor coverage — **BLOCKER for breadth**

10 embedded roots. Sites whose chain doesn't terminate in one of those
roots will fail with `BR_ERR_X509_NOT_TRUSTED`. Estimated real-world
coverage: ~70-80% of the long-tail of HTTPS sites by traffic, but
maybe ~50% of MacSurf-relevant retro-friendly hosts (because retro
sites are over-represented on smaller CAs).

V1 path: ship the 10 baked-in roots, surface "cert not trusted" as a
non-scary dialog. V2 path: load a Mozilla `cacert.pem` from disk at
startup, parse with `brssl ta` equivalent at build time, embed ~50
roots. V3 path: per-user "trust this anchor" UI like Classilla did.

### 5.3 No session resumption — **performance ceiling**

Every HTTPS fetch does a fresh handshake. ChaCha20-Poly1305 on a G3
@ 350 MHz is ~1-3 seconds for the asymmetric phase. A typical
modern page with 20+ sub-resources would take 20+ seconds just on
handshakes.

V1 mitigation: **strict resource budget**. Cap per-page parallel HTTPS
fetches to 2-4, and connection-pool the underlying OT endpoint when
possible (the existing HTTP fetcher's `ep_pool` mechanism translates
directly — fixes98-105 round documents the pattern). The TLS state
must be torn down per-connection though; resumption is V2.

V2: BearSSL exposes `br_ssl_session_parameters` for session ID + ticket
resumption. Per-host cache of one session record cuts the handshake
to a single round-trip. Estimated 5-10x perceived speed-up on
HTTPS-heavy pages.

### 5.4 Clock-before-2000 dialog — **must be surfaced**

OS 9 PRAM batteries are dead more often than not. `kOSTLSAsync_ClockBefore2000`
fires when `OSTLS_GetBearSSLTime` returns a value before Y2K. The
dialog text:

> MacSurf can't verify HTTPS certificates because the Mac's date and
> time are set before the year 2000. Please open the Date & Time
> control panel and set the current date.

Must be a separate dialog (not a generic "TLS error") because the user
action is concrete and unrelated to the URL.

---

## 6. Risk audit

### 6.1 CW8 C89 compatibility

BearSSL is C89-clean per its README. The integration risk is mostly
header conflicts with MacSurf's existing macsurf_prefix.h injections:
- `__MWERKS__` checks
- `bool` / `true` / `false` redefinitions
- `<MacTypes.h>` first-include rule
- `__APPLE__` / `__POWERPC__` / `__GNUC__` predefined checks BearSSL
  uses for inline-asm paths (we want the C fallback)

BearSSL has `config.h` for compile-time selection. We probably need a
`macSSL/bearssl/inc/config.h` overlay that disables anything that uses
GCC inline asm and forces the portable bigint path. This is exactly
what `ssl_engine_cw8.c` is for; verify it's complete.

### 6.2 CW8 PPC int64 codegen — **already validated**

The macSSL doc Stage A.5 specifically verified `uint32_t * uint32_t →
uint64_t` codegen on CW8 PPC. This is BearSSL's `MUL31` substrate — if
this were broken the whole stack would fail. **It's not broken.** No
action.

### 6.3 Build time / link time

294 BearSSL files + 7 macSSL files = 301 new translation units, ~1.85
MB of source. On CW8 / G3 this will roughly double the build time of a
clean rebuild. Mitigation: build BearSSL into a separate static library
(`.lib`) in the CW8 project. Once stable, incremental builds touch
only the macSSL glue and the new fetcher.

### 6.4 Reentrancy / threading

OSTLS is single-threaded with cooperative yielding. NetSurf's fetch
subsystem is also single-threaded. No actual concurrency to worry
about, but **must not call `OSTLS_Pump` reentrantly** from inside an
NS event callback. The state machine in `macos9_https_fetcher.c` must
guard against this with a "pumping" flag the same way the existing
HTTP fetcher guards against re-entrant `mfs_open`.

### 6.5 OT endpoint pool sharing

The existing HTTP fetcher pools endpoints keyed by `host:port`. HTTPS
endpoints can NOT be pooled across hosts (different SNI / cert), but
they CAN be pooled per-host once session resumption lands. **V1: do
not pool HTTPS endpoints.** V2: pool with session-resumption.

### 6.6 The proxy is still load-bearing

Per [integration-notes §7](../../macSSL/docs/macssl-integration-notes.md):
the proxy stays. Sites needing JavaScript-render-and-flatten, sites
with unknown CAs, and the entropy-weak boot phase all still depend on
the proxy. The native HTTPS fetcher is **additive**, not a
replacement, in V1.

---

## 7. Phased plan

### Phase 0 — Scope + branch (this commit)

This document + the `macSSL` branch. No code changes.

### Phase 1 — Build skeleton (1 round)

- Add 7 macSSL/os9 + ~294 BearSSL files to MacSurf.mcp (user does this
  through CW8 IDE per the "do not edit .mcp" CLAUDE.md rule).
- Add access paths (user task; we list the paths in handoff).
- Verify the project still links with no functional change. **Critical
  acceptance:** the build still produces a working binary that renders
  mactrove.com via the existing HTTP fetcher. No regression in the
  proxy path.

### Phase 2 — Stub HTTPS fetcher (1-2 rounds)

- New `macos9_https_fetcher.c` registered for `https`, but **does
  nothing useful yet**: returns `FETCH_ERROR` with a fixed message
  ("HTTPS not yet implemented in this build").
- Modify `macos9_http_fetcher_register` to only claim `http`.
- Add `macos9_https_fetcher_register()` call to `macos9_fetcher_init.c`.
- **Critical acceptance:** typing `https://mactrove.com` produces an
  error page; typing `http://mactrove.com` still works through the
  proxy. The dispatch split works.

### Phase 3 — Connect + handshake (2-3 rounds)

- Wire `OSTLS_New` / `OSTLS_Start` / `OSTLS_Pump` into the new
  fetcher.
- Surface `kOSTLSEventConnected` / `kOSTLSEventHandshakeDone` as
  state-machine transitions.
- **Acceptance:** a debug build of MacSurf does an HTTPS handshake to
  `google.com` and logs the cipher suite. No HTTP request issued yet.

### Phase 4 — HTTP send + receive + chunked decode (1-2 rounds)

- `OSTLS_HTTP_FormatGet` + `OSTLS_Write` to send the request.
- `OSTLS_Read` loop to drain decrypted bytes.
- `OSTLS_HTTP_ChunkDecoderProcess` for chunked responses.
- Parse status line + headers + dispatch `FETCH_HEADER` / `FETCH_DATA`
  / `FETCH_FINISHED` callbacks correctly.
- **Acceptance:** `https://mactrove.com/advanced.html` renders the
  same content as `http://mactrove.com/advanced.html` does today
  through the proxy.

### Phase 5 — Error translation + clock dialog (1 round)

- BearSSL/OSTLS error code → `nserror` mapping.
- Clock-before-2000 dialog.
- "Cert not trusted" dialog with a one-time-bypass option (V1) or
  hard-fail (V0).
- **Acceptance:** `https://example-with-non-baked-root.com` shows a
  clear error; setting the Mac clock to 1999 and reloading any HTTPS
  URL surfaces the clock dialog.

### Phase 6 — Stability + smoke (1 round)

- Run the existing HTTP fetcher's leak audits (fixes102-105 patterns)
  against the new HTTPS path. Verify every terminal callback is
  matched by `fetch_remove_from_queues` + `fetch_free` + `OSTLS_Dispose`.
- Memory ceiling test: 8 concurrent HTTPS fetches against varied
  hosts; confirm partition stays under 4 MB used.
- **Acceptance:** real-page browsing for 30+ minutes without leaks
  or crashes.

**Total estimated rounds: 7-10.**

---

## 8. Deliverables checklist (final V1 ship)

- [ ] `macos9_https_fetcher.c` in MacSurf source tree
- [ ] Modified `macos9_http_fetcher_register` (http only)
- [ ] Modified `macos9_fetcher_init.c` registering https
- [ ] All 7 macSSL/os9 files in MacSurf.mcp
- [ ] All 294 BearSSL files in MacSurf.mcp (or library link)
- [ ] 15 new access paths in `Access Paths.xml` (user maintains)
- [ ] Clock-before-2000 dialog
- [ ] Cert-not-trusted dialog
- [ ] BearSSL/OSTLS → nserror translation table
- [ ] CLAUDE.md "Last shipped fix" entry
- [ ] README entry: "MacSurf v0.6 — Native HTTPS"
- [ ] HARDWARE-VERIFIED log file from a G3 fetch of `https://mactrove.com/advanced.html`
- [ ] Honest disclaimer in About / title bar: "Stage A entropy — preview HTTPS"

---

## 9. Out of scope for V1

- Production entropy gathering (lives in the macSSL repo)
- Mozilla CA bundle (V2)
- Session resumption (V2)
- HTTP/2 (BearSSL is TLS-only; OK as a permanent limit on this platform)
- Client certificates (server auth only, V∞)
- ALPN, OCSP stapling, Certificate Transparency (V∞)
- TLS 1.3 (BearSSL upstream supports it; CW8 codegen risk not yet
  evaluated; defer until V1 is solid)
- Retiring the proxy (proxy stays — see §4 phase 3)

---

## 10. Open questions for the user

1. **Trust anchor breadth for V1.** Stay with the existing 10 roots, or
   expand to ~50 roots from a Mozilla bundle in V1? The latter doubles
   the binary size (~50 KB per RSA-4096 anchor) but cuts the "site
   cert not trusted" rate ~3-5x. Recommendation: **ship V1 with 10**
   and add the bundle in V1.1 once the cert-not-trusted dialog is real
   so we can measure the failure rate.

2. **Stage A entropy disclosure.** The library is honest that the
   Stage A entropy is insecure. Where should the disclaimer appear:
   - title bar marker ("https — preview")? Recommendation.
   - one-shot dialog on first HTTPS load per session?
   - About box only?

3. **HTTPS fetcher cap.** `MAX_HTTPS_FETCHES = 4` per integration-
   handoff. Acceptable? Lower (2) would protect heap harder. Higher
   (6-8) matches NetSurf core's expectation and may reduce page-load
   latency at the cost of memory headroom.

4. **Proxy fallback on cert-not-trusted.** If we hit
   `BR_ERR_X509_NOT_TRUSTED`, do we:
   - hard-fail with the dialog (V0)
   - auto-retry through the proxy silently (transparent)
   - prompt the user to choose (V1 recommended)
   - let the user disable native HTTPS in preferences entirely

5. **MacSSL repo location.** macSSL is currently a sub-directory under
   `macsurf/macSSL/`. Stay that way (vendored), or move to a separate
   repo and add as a submodule? Vendoring is simpler for the CW8 build
   workflow; submodule would let macSSL release independently. Recommendation:
   **stay vendored** through V1, evaluate after.
