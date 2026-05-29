# macLink — what we reuse from macTLS / MacSurf / BearSSL

**Purpose:** concrete file-by-file inventory of what we lift directly, what we adapt, and what we write fresh. Drives Phase 0 deliverables and the initial `macLink-Proxy.mcp` project file list.

---

## Headline finding: Phase 0's biggest risks are gone

The macLink scope flagged two big technical unknowns: (1) does BearSSL ship usable server-side TLS, and (2) does it have the primitives to mint our own X.509 certs. Both answered cleanly **yes**:

### BearSSL server-side TLS — already ships

`bearssl/src/ssl/` contains the complete server-side handshake state machine and pre-built profiles:

- `ssl_server.c` — engine
- `ssl_hs_server.c` — handshake state machine
- `ssl_server_full_ec.c` — ECDSA-cert server profile (we'd use this)
- `ssl_server_full_rsa.c` — RSA-cert server profile (alternative)
- `ssl_server_mine2c.c`, `ssl_server_mine2g.c`, etc. — minimized variants

**Implication:** macTLS Phase 1 (server-side API) is not "port a TLS server to OS 9" — it's "wire 3 more BearSSL files to the existing OT pump." Mirrors the work already done for client-side. Maybe ~600 LOC of glue, not 2-3 KLOC of crypto.

### BearSSL X.509 cert minting — primitives present

`bearssl/src/x509/` and `bearssl/src/ec/`:

- `asn1enc.c` — generic ASN.1/DER encoder
- `encode_ec_pk8der.c` / `encode_ec_rawder.c` — EC private/public key DER encoding
- `encode_rsa_pk8der.c` / `encode_rsa_rawder.c` — RSA equivalents
- `ecdsa_default_sign_asn1.c` / `ecdsa_i15_sign_asn1.c` — ECDSA signature production
- `x509_decoder.c` — parses certs we've minted (useful for sanity-check)

**Implication:** writing a leaf-cert minter is ~300 LOC of TBSCertificate DER assembly + signing call + outer cert wrapping. No new crypto primitives needed. Reference implementations available (Caddy's `certmagic`, mitmproxy's `certauthority`).

### Reference architecture for the proxy itself

`proxy/proxy.go` is 150 lines and handles both `CONNECT` and plain HTTP proxy modes. **That's the architectural reference for the C port of macLink-Proxy.** Same protocol logic, different implementation language.

---

## Direct reuse — link these files as-is

### From `macTLS/bearssl/` (already linked in MacSurf via macTLS)

These are already in MacSurf's build — macLink picks them up at link time when it links against macTLS, no separate inclusion needed:

| Category | Files | What we get |
|---|---|---|
| **AEAD** | `aead/ghash_*.c`, `aead/chacha20_*.c`, `aead/poly1305_*.c`, `aead/gcm.c` | AES-GCM, ChaCha20-Poly1305 |
| **Hash** | `hash/sha2small.c`, `hash/sha2big.c`, `hash/mgf1.c` | SHA-256, SHA-384, SHA-512 |
| **EC** | `ec/ec_c25519_m15.c`, `ec/ec_p256_m15.c`, `ec/ec_prime_i15.c` | X25519, P-256, P-384 (multi-curve as of v1.3.1) |
| **ECDSA** | `ec/ecdsa_i15_sign_asn1.c`, `ec/ecdsa_i15_vrfy_asn1.c` | Sign + verify |
| **X.509 verify** | `x509/x509_minimal.c`, `x509/x509_decoder.c` | Verifies upstream server certs against our 121-anchor bundle |
| **TLS engine (client)** | `ssl/ssl_engine.c`, `ssl/ssl_hs_client.c`, `ssl/ssl_client.c`, `ssl/ssl_client_full.c` | Already in macTLS — talks TLS to real upstream servers |
| **Random** | `rand/hmac_drbg.c` | HMAC-DRBG seeded by macEntropy |

### From `macTLS/os9/`

These get linked directly into macLink — same source files, different host project:

| File | What we get |
|---|---|
| `ostls_async.c` + `.h` | The Open Transport async pump pattern, connection state machine, kOTSyncIdleEvent yields, the entire driving loop for client connections — and the basis we extend for server-side |
| `ostls_entropy.c` + `.h` | macEntropy: HMAC-DRBG + accumulator + persisted seed. Used as randomness source for cert serial numbers, ephemeral keys, anywhere we need bytes |
| `ostls_b3_anchors.c` + `.h` | The 121-anchor Mozilla CCADB bundle as compiled-in trust roots for upstream cert verification |
| `ostls_log.c` + `.h` | Diagnostic log channel (file-backed). macLink uses this for its own logging, sharing format with macTLS |
| `ostls_time.c` + `.h` | Mac time → Unix time conversions. Used in cert validity dates |
| `ostls_tls13_handshake.c` + `.h` | TLS 1.3 client handshake — already shipping in macTLS |
| `ostls_tls13_keysched.c` + `.h` | TLS 1.3 key schedule |
| `ostls_tls13_record.c` + `.h` | TLS 1.3 record encryption |
| `ostls_b1_tcp.c` + `.h` | TCP/OT helpers — extend for server-side `OTListen` / `OTAccept` |
| `ostls_b2_handshake.c` + `.h` | TLS 1.2 client handshake bridge |
| `ostls_b3_handshake.c` + `.h` | Outer handshake driver |

**Build-time decision:** macLink links these as a static library (the cleanest pattern, mirrors how macTLS-the-library would be consumed by any downstream product), or includes them directly in the macLink-Proxy.mcp project file list. CW8 supports both; recommend the second (simpler to manage during early development) and migrate to a library once macLink stabilizes.

### From `proxy/` (the existing Go macsurf-proxy)

**Architectural reference only**, not linkable since it's Go. Reading the file `proxy/proxy.go` (150 lines) before starting the C port is the fastest way to absorb the proxy protocol logic:

- `ServeHTTP` — top-level dispatch on Method (CONNECT vs plain)
- `handleConnect` — TCP tunnel pattern with timeout and bidirectional copy
- `handleHTTP` — request rewriting (`http://` → `https://`) and origin contact

We don't lift the code. We lift the architecture.

### From `browser/netsurf/frontends/macos9/`

| File | What we adapt |
|---|---|
| `macos9_http_fetcher.c` | HTTP/1.1 parsing patterns (header parsing, status code handling, chunked transfer decode). Methods like `process_chunked_bytes` would translate verbatim. |
| `macos9_https_fetcher.c` | macTLS consumer pattern — exactly how macLink connects to upstream macTLS sessions. Lift `c->conn` lifecycle, pump loop, error handling. |
| `macos9_fetcher_init.c` | NetSurf-specific bootstrap. Not directly applicable — macLink isn't NetSurf-hosted. Read for OT init sequence. |

**Don't lift directly.** These are NetSurf-bound (they implement `fetcher_operation_table`). The patterns transfer; the code does not.

---

## Adapt — substantial mechanical rewrites

### `ostls_async.c` → `mlink_async.c`

macTLS's `ostls_async.c` is ~2000 LOC of OT pump for **outbound** connections (client side). macLink needs the same pattern for **inbound** connections (server side). The structural answer: extend `OSTLSConnection` to also represent a server-side session, parallelize the pump callbacks for server-side handshake.

Estimated new code: ~600 LOC, mostly mirror-image of the existing client code. Same architecture, different BearSSL initialization (`br_ssl_server_init_full_ec` instead of `br_ssl_client_init_full`).

**Recommended approach:** add to `ostls_async.c` rather than fork it. Keep one async pump file that handles both sides. New public API:

```c
/* In ostls_async.h, parallel to existing client API: */

typedef struct OSTLSServerConfig {
    const uint8_t *cert_chain;       /* DER-encoded leaf + intermediates */
    size_t cert_chain_len;
    const uint8_t *private_key_der;  /* PKCS#8 ECDSA P-256 */
    size_t private_key_len;
    /* … other server-specific options … */
} OSTLSServerConfig;

OSErr OSTLS_AcceptNew(OSTLSConnection **out_conn,
                      const OSTLSServerConfig *config,
                      EndpointRef accepted_ep);

OSErr OSTLS_AcceptStart(OSTLSConnection *conn);

/* Pump / Read / Write / Close are shared between client and server */
```

`OSTLS_AcceptNew` takes an already-accepted OT endpoint (caller did `OTListen` + `OTAccept`). `OSTLS_AcceptStart` drives the server-side handshake forward via the existing pump.

This new surface is **the single net-new contribution to macTLS itself**. Everything else macLink needs is pure macLink-side new code that links against the existing macTLS.

---

## Net-new — entirely new code in macLink/

These don't exist in any form across the projects today. Files we'd create in `macLink/os9/`:

| File | Purpose | LOC est. |
|---|---|---:|
| `mlink_main.c` | Faceless Background App main(): WaitNextEvent loop, accept-loop dispatch, connection table | ~400 |
| `mlink_listener.c` + `.h` | Multi-port OT listener (HTTPS-CONNECT on 8765, SMTP on 8587, IMAP on 8143, POP3 on 8110, FTP on 8121). Each calls `OTListen` and dispatches accepted endpoints to the right protocol handler. | ~400 |
| `mlink_connect_proxy.c` + `.h` | HTTP CONNECT method parser + tunnel setup. Receives CONNECT from client, opens upstream via macTLS, becomes a TLS-terminating bridge. | ~350 |
| `mlink_http_proxy.c` + `.h` | Plain `GET http://...` proxy with optional `https://` upstream upgrade (the original macsurf-proxy pattern). For apps that don't speak CONNECT. | ~250 |
| `mlink_starttls.c` + `.h` | Generic STARTTLS state machine — parametrized by protocol command set. | ~200 |
| `mlink_starttls_smtp.c` | SMTP-specific glue (EHLO sniffing, STARTTLS injection point) | ~150 |
| `mlink_starttls_imap.c` | IMAP-specific glue (CAPABILITY sniffing, STARTTLS injection) | ~150 |
| `mlink_starttls_pop3.c` | POP3 STLS | ~120 |
| `mlink_ftps.c` | FTP AUTH TLS — dual-channel handling | ~200 |
| `mlink_ca.c` + `.h` | Root CA generation, persistence, leaf cert minting | ~600 |
| `mlink_cert_cache.c` + `.h` | Per-host leaf cert cache (LRU, 30-day TTL, persisted) | ~150 |
| `mlink_ic_proxy.c` + `.h` | Internet Config proxy-pref read/write | ~150 |
| `mlink_prefs.c` + `.h` | macLink's own preferences file (port, on/off, advanced options). Reuses pattern from the auto-update prefs proposal. | ~200 |
| `mlink_log.c` + `.h` | Thin wrapper around `ostls_log` with macLink-prefixed lines | ~50 |
| **Subtotal net-new** | | **~3370** |

**Plus** the Control Panel CDEV (`macLink CDEV`) and Installer app, both Carbon resource projects, estimated:

| Project | LOC | Notes |
|---|---:|---|
| macLink CDEV | ~800 | Control Panel UI (resources + C code) |
| macLink Installer | ~400 | Setup wizard + first-run cert install walkthrough |

**Grand total:** ~3370 + 1200 = ~4570 LOC net-new in macLink + ~600 LOC extension to macTLS. Consistent with the scope-doc estimate.

---

## Phase 0 deliverables (the immediate next steps)

Concrete, finishable in ~1 week if focused:

1. **`macLink/REUSE_MAP.md`** — this document. Done.
2. **`macLink/os9/mlink_main.c` skeleton** — empty Carbon FBA main() that opens a window-less event loop, logs "macLink alive" on tick, and exits cleanly on Cmd-Q (via TRAP installer or signal). Just enough to confirm the FBA pattern works on real hardware. ~80 LOC.
3. **`macLink/os9/mlink_listener.c` skeleton** — opens ONE OT TCP listener on `127.0.0.1:8765`. Accepts a connection. Reads bytes until EOF. Closes. Doesn't proxy anything yet. Validates we can listen on loopback under Classic Mac OS. ~150 LOC.
4. **`macLink/macLink.mcp`** — CW8 project file skeleton (manually authored from MacSurf's `MacSurf.mcp` as template). Lists the OS9-side files above, the macTLS files reused, and the BearSSL primitives.
5. **`macLink/docs/PHASE_0_REPORT.md`** — a short report after Phase 0 confirms or denies the two assumptions: (a) FBA accepts loopback TCP under CarbonLib, (b) Process Manager keeps it alive across multiple connection cycles. Go/no-go for Phase 1.

If Phase 0 comes back clean, Phase 1 (macTLS server-side API in `ostls_async.c`) starts in parallel with Phase 2 (CA / cert minting in `mlink_ca.c`). Both are independently startable and converge at Phase 3 (the first real TLS-bridged connection).

---

## Build environment

**CodeWarrior 8 / strict C89**, same conventions as MacSurf and macTLS:

- No `inline`, no `//` comments, no C99 designated initializers, no for-scope declarations
- All variables at top of block
- Mac CR line endings on shipped sources
- Universal Interfaces + MSL for Carbon types
- Linux-side syntax check via Retro68: `/home/patrick/Retro68/toolchain/bin/powerpc-apple-macos-gcc -std=c89 -pedantic-errors -Wall -Wno-long-long`

**Project file pattern (macLink-Proxy.mcp):**

- One flat folder under the project root for `.c` files (mirrors MacSurf's flat-folder build)
- Access paths same as macTLS (CIncludes, MacHeaders, MSL_C, MSL_Extras, MSL_MacOS)
- New access path: `macLink/os9/`
- Carbon partition: start with 8 MB preferred / 4 MB minimum (we don't have libcss; lighter footprint than MacSurf)
- Set `'carb'` resource at ID 0 (REQUIRED for CarbonLib — see MacSurf's `MacSurf.r` for the pattern)

**MacSurf and macTLS sources stay where they are** in the macsurf repo. macLink's build directly references them via access paths — no duplication.

---

## Next concrete action

Drop the Phase 0 skeleton files into `macLink/os9/` and write the .mcp project file. The user (or the maintainer running on real hardware) opens the .mcp in CW8 and tries to build. First success criterion: clean compile + link, FBA launches at boot, logs "macLink alive" to MacSurf Debug.log (we share the log channel via `ostls_log.c`).

If that works, Phase 1 (macTLS server-side) starts in parallel with Phase 2 (CA / cert minting).
