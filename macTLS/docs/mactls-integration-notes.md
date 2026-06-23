# macTLS → MacSurf Integration Notes (Stage B5)

Status: text-only design doc, no code wiring yet.

This document describes how the verified MacTLSTest stack (BearSSL +
Open Transport + embedded trust anchors + OS 9 clock conversion)
becomes a usable HTTPS transport in MacSurf. The MacTLSTest binary
remains useful as a standalone diagnostic; the integration is
additive.

## What MacTLSTest proved

Verified on real Mac OS 9.1 / G3 hardware at 2026-05-19 (see git
history `fixes7..fixes12`):

- **A.5**: CW8 PPC `uint32_t * uint32_t → uint64_t` codegen is sound.
  BearSSL's `MUL31` kernel is the bigint substrate for every cert
  validation; if this is broken nothing else matters.
- **A**: BearSSL static init + client context setup link cleanly into
  a Carbon CFM binary; the engine reaches `BR_SSL_SENDREC`.
- **B1**: Open Transport delivers TCP connectivity from a Carbon
  binary on this platform. `OTOpenEndpointInContext` + sync + blocking
  works; `OTConnect` against `example.com:443` succeeds.
- **B2**: BearSSL's record-layer state machine drives correctly when
  its `SENDREC`/`RECVREC` records are pumped over `OTSnd`/`OTRcv`.
  TLS 1.2 handshake completes with cipher suite **0xCCA9
  (ECDHE-ECDSA + ChaCha20-Poly1305)** against `example.com:443` using
  an insecure permissive validator.
- **B3**: With five embedded trust anchors (Amazon, DigiCert G2, GTS
  R1, GTS R4, ISRG X1) and the OS 9 system clock fed through
  `br_x509_minimal_set_time`, real chain validation completes against
  `google.com:443`. Failure modes are mapped to distinct result codes
  (UnknownCA / Expired / HostnameMismatch / BadSignature /
  TimeUnknown / Other).
- **B4**: A tiny `GET / HTTP/1.0` over the validated channel produces
  a decrypted HTTP response captured byte-for-byte. End-to-end
  application I/O proven.

## What the integration into MacSurf looks like

### 1. New transport object

`browser/netsurf/frontends/macos9/macos9_https_fetcher.c` (sibling of
the existing HTTP fetcher) implements the same
`fetcher_operation_table` shape that NetSurf's `fetch` subsystem
expects, scheme = `"https"`. Reuses the OT setup pattern from
`macos9_http_fetcher.c` but wraps the endpoint in a BearSSL session.

Per-fetch context:

```
struct macos9_https_ctx {
    EndpointRef        ep;            /* OT endpoint, same as HTTP */
    br_ssl_client_context  client;    /* ~10 KB */
    br_x509_minimal_context x509;     /* ~5 KB */
    unsigned char      iobuf[BR_SSL_BUFSIZE_BIDI];  /* 33 178 bytes */
    /* response state, redirect state, chunked decoder, etc. */
    ...
};
```

### 2. Three callable units to lift from macTLS

Treat these as a stable API surface; they're already isolated by
file:

- **`ostls_b3_anchors`** — `OSTLS_B3_GetAnchors(&anchors, &count)`.
  Returns a static-lifetime `br_x509_trust_anchor` array. Lift the
  whole file into MacSurf's frontend tree. Rotation policy lives in
  the header.
- **`ostls_time`** — `OSTLS_GetBearSSLTime(&days, &seconds)`. Mac
  clock → BearSSL's day-count-since-0-AD. Note the proleptic
  Gregorian epoch (Unix Epoch = day 719528); the day this was
  wrong cost a round-trip in B3 (see `fixes8` commit message).
- **`ostls_entropy`** — `OSTLS_InjectEntropy(&eng)` /
  `OSTLS_CollectEntropy()`. **macEntropy v1.0** (hardware-validated on
  G3, 2026-05-29): a SHA-256 accumulator, not the old stub. Call
  `OSTLS_CollectEntropy()` from the host idle loop and (once the host
  stir seam lands) `OSTLS_StirEntropy()` with key/mouse jitter.

### 3. Production entropy — DONE (macEntropy v1.0)

The original Stage-A stub (32 bytes of TickCount + Microseconds + stack
address + a fixed tag — not cryptographically sound) has been replaced.
macEntropy v1.0 (see [MACENTROPY_SCOPE.md](../MACENTROPY_SCOPE.md)) is:

- A running SHA-256 pool; domain-separated extraction with fold-back,
  feeding BearSSL's engine HMAC-DRBG.
- Sources: high-resolution clock, mouse-delta jitter, stack noise, and
  OT packet-arrival timing folded in at every `OTRcv` during a fetch.
- A Preferences-folder seed file persisted across boots, so the first
  handshake after a cold boot is not thin.
- Validated by the Stage E self-test: non-degenerate output and distinct
  seed streams across separate launches.

The one remaining piece is the host stir seam (`OSTLS_StirEntropy`),
which lands with the MacSurf fold-in so a real event loop can feed
key-press latency and mouse deltas. The earlier warning — that MacSurf
must not advertise HTTPS as production-secure until real entropy lands —
is now satisfied: session keys derive from a validated CSPRNG seed, not
the predictable stub.

### 4. Trust anchor rotation

The five anchors in `ostls_b3_anchors.c` are valid through 2035-2038.
Rotation triggers are slow (years of CA/B Forum notice) but real.

When a root nears expiry or is decommissioned:

1. Fetch the replacement PEM from the operator (typically a
   well-known URL).
2. Run `brssl ta <new.pem>` to produce the byte arrays.
3. Replace the relevant `TAn_*` arrays in `ostls_b3_anchors.c`.
4. Update the `g_anchors[n]` runtime-init block to reference them.
5. Update the header comment with the new expiry.

The integration can also bundle a CA bundle approach: instead of 5
hand-picked roots, embed a fuller set (~50 roots) from a Mozilla
bundle. Tradeoff is partition footprint (~50 KB per anchor for the
RSA-4096 ones) vs reach. Keep the small set for v1.

### 5. Memory footprint per fetch

| Component                       | Bytes  |
|---------------------------------|-------:|
| br_ssl_client_context           | ~10 KB |
| br_x509_minimal_context         | ~5 KB  |
| BR_SSL_BUFSIZE_BIDI (I/O buf)   | 33 178 |
| OT endpoint state               | ~2 KB  |
| **Per-fetch total**             | **~50 KB** |

MacSurf's current Carbon partition is 16 MB preferred / 8 MB minimum.
A single HTTPS fetch fits comfortably. Eight parallel HTTPS fetches
(NetSurf's default per-host limit) is ~400 KB — still well within
budget. No partition bump required.

### 6. Cooperative yielding

MacTLSTest's probes use synchronous + blocking OT throughout because
the binary is single-threaded and has no UI events to service mid-
handshake. MacSurf's fetcher is single-threaded too but the event
loop is alive — so the handshake loop MUST yield. Options:

- **Notifier + `YieldToAnyThread`** on `kOTSyncIdleEvent`. MacSurf
  already uses this pattern in `macos9_http_fetcher.c` — drop in the
  same notifier shape. The `<Threads.h>` include issue MacTLSTest
  worked around (see `fixes11` notes) is already handled in the
  MacSurf prefix.
- **Switch to non-blocking** + `OTLook` poll. More code, finer-
  grained yielding, but matches what `macos9_http_fetcher.c` already
  does post-`OTConnect`.

Recommendation: non-blocking + `OTLook` for the handshake too,
matching MacSurf's HTTP fetcher style.

### 7. Replace vs augment

`macos9_http_fetcher.c` handles `http:` URLs through MacSurf's local
proxy (`116.202.231.103:8765`). The proxy then performs the upstream
HTTPS via Go's `crypto/tls` and returns plaintext.

**Augment, don't replace.** The proxy is still needed for:

- Sites whose JavaScript needs server-side rendering (the render-and-
  flatten pipeline).
- Sites with cert chains that don't terminate in one of macTLS's five
  embedded roots (until a CA bundle ships).
- Fallback when entropy on the client is provably weak (no mouse
  movement yet during boot, etc.).

The new HTTPS fetcher handles `https:` URLs directly when:

- The cert chain terminates in an embedded root, AND
- Entropy was successfully gathered (post-Stage-A entropy work).

Otherwise NetSurf core can transparently fall back to "https://X"
→ "http://X via proxy" routing. The user's `nsoptions` value for
`http_proxy` already supports this path.

### 8. Error surfacing to the browser UI

BearSSL errors don't map naturally onto NetSurf's `nserror`. The
integration needs a small translation table at the boundary:

| BearSSL error                    | NetSurf surface |
|----------------------------------|-----------------|
| `BR_ERR_X509_NOT_TRUSTED`        | `NSERROR_BAD_AUTH` + dialog "site cert not trusted" |
| `BR_ERR_X509_EXPIRED`            | `NSERROR_BAD_AUTH` + dialog "site cert expired or clock wrong" |
| `BR_ERR_X509_BAD_SERVER_NAME`    | `NSERROR_BAD_AUTH` + dialog "cert / hostname mismatch" |
| `BR_ERR_X509_BAD_SIGNATURE`      | `NSERROR_BAD_AUTH` + dialog "site cert signature invalid" |
| `BR_ERR_*` (record layer / engine) | `NSERROR_BAD_URL` |
| OT failures                       | existing OT-error path |
| Clock before 2000                 | dialog "set the system clock first" |

The clock-before-2000 dialog is the most important user-visible
surface — PRAM-battery-dead Macs are common and the user has zero
chance of debugging "expired cert" if the underlying cause is a
1904-era system clock. macTLS's `kOSTLSB3_ClockBefore2000` /
`kOSTLSB4_ClockBefore2000` distinct codes carry this through; the
integration must keep them distinct, not collapse them into a
generic "TLS failed".

### 9. Known limitations of the v1 integration

- **No HTTP/2.** BearSSL is TLS-only. Negotiated protocol is HTTP/1.x.
- **No chunked decoder in macTLS.** MacSurf's HTTP fetcher already has
  `process_chunked_bytes` (see `macos9_http_fetcher.c`). Lift that
  into the HTTPS fetcher or share it.
- **No session resumption.** Every fetch does a fresh handshake. With
  ChaCha20 on a G3, handshakes are ~1-3 seconds. Session resumption
  would cut this 10x. Deferred to v2.
- **No ALPN, no SNI for IP literals**, no OCSP, no CT. These are
  acceptable v1 omissions.
- **No client certs.** Server auth only.

### 10. Testing harness

MacTLSTest is the canonical regression test. Any change to macTLS
should rerun all five stages (A.5, A, B1, B2, B3, B4) against the
same targets. The file-backed log (`MacTLSTest.log` on Desktop,
fixes11) gives a plain-text record per run; the harness can diff
log files between runs to catch regressions.

## What's NOT in this milestone

- The actual `macos9_https_fetcher.c` source. This document is the
  scope; the code follows in a later milestone tracked in MacSurf's
  CLAUDE.md.
- Production entropy. Tracked separately in the macTLS repo.
- A larger CA bundle. Tracked separately.

## Followups

| Item                                        | Where               |
|---------------------------------------------|---------------------|
| Production entropy (mouse/key/notifier)     | macTLS os9/ostls_entropy.c |
| macos9_https_fetcher.c implementation       | MacSurf frontend    |
| BearSSL error → NSERROR translation table   | MacSurf frontend    |
| Clock-wrong dialog                          | MacSurf UI          |
| Session resumption                          | macTLS (v2)         |
| Larger CA bundle                            | macTLS              |
| chunked decoder shared with HTTP fetcher    | MacSurf frontend    |
