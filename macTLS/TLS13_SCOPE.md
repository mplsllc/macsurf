# TLS 1.3 for macTLS

**Status:** Stage 0 (design lock). No code written yet. This doc is the plan.

**Goal (closes the project):** macTLS negotiates a real TLS 1.3 handshake against a live 1.3 server on a Power Macintosh G3, reads back decrypted application data, and still falls back cleanly to its existing TLS 1.2 path when a server doesn't do 1.3. mactrove.com already serves `TLSv1.2 TLSv1.3`, so it's a ready test target for both halves.

## Why this, why now

macTLS is TLS 1.2 only because BearSSL is. That isn't a config gap we can flip. BearSSL has had 1.3 as "planned when it's an RFC" for years and it never shipped, with real design tensions around PSS, large cookies, and tickets versus its minimal-allocation model. curl is dropping BearSSL over exactly this. So waiting on upstream is a dead end, and a growing slice of the web is 1.3-preferred or 1.3-only.

The good news: we don't have to wait, and we don't have to invent the approach. **Certainly** (minorbug, MIT, at `/home/patrick/Webs/certainly`) already runs TLS 1.3 on Mac OS 9. It doesn't use BearSSL's TLS engine for 1.3. It hand-writes the 1.3 handshake and borrows BearSSL only for the primitives we already ship: X25519, AES-GCM, ChaCha20-Poly1305, HKDF/HMAC/SHA-256, and the X.509 validator. macTLS has all of those. So the work is a focused, proven-feasible port, not research into the unknown.

## The bet (locked at Stage 0)

Adapt Certainly's three `tls13_*` modules onto macTLS's existing stack: our OT pump, macEntropy for seeding, our 121 trust anchors, and BearSSL's crypto primitives. Keep the existing BearSSL T0 engine as the 1.2 fallback. Start every connection with a 1.3 ClientHello that also advertises 1.2 suites. If the server picks 1.3, the new handshake runs the connection. If it picks 1.2 (the `Fallback12` result), reset and let the current engine take the whole handshake, exactly as macTLS does today.

We study and adapt Certainly's code, we don't paste it. It's Retro68 / GCC 12 / C99; macTLS is CodeWarrior 8 / C89. So every module gets a C89 conversion pass on the way in. MIT license, attribution in the source.

## Scope

**In:**
- X25519 key exchange only (the one named group Certainly offers, and the realistic floor for a no-FPU-tricks PPC).
- AEAD: AES-128-GCM-SHA256 (0x1301) and ChaCha20-Poly1305-SHA256 (0x1303). SHA-256 transcript only.
- Server authentication via the existing BearSSL X.509 minimal validator and our anchors.
- The 1.2 fallback path (already works; we just gate it behind the 1.3 attempt).

**Out (at least for v1):**
- SHA-384 suites (0x1302). Rejected, same as Certainly, to keep one transcript-hash path.
- Client certificates.
- 0-RTT / early data.
- **1.3 session resumption (PSK / tickets).** Certainly's key schedule stubs the PSK path (always zeros), so it does 1.3 but not 1.3 resumption. Worth calling out: 1.3 tickets are what would finally give us resumption against the big CDNs that BearSSL's 1.2 session-ID path can't touch. That's the natural follow-on once 1.3 itself lands, and it ties the TLS 1.3 work back to the session-resumption thread.

## Compatibility floors

Same as the rest of macTLS, plus two things to watch:

- **CW8 C89** on the converted code. No `//`, no mid-block decls, no designated initializers, no `bool`/`stdbool` (use the project's existing pattern), no VLAs, enums and structs by the book.
- **16 MB Carbon partition.** The handshake context is large: a 16 KB plaintext record buffer plus a 4 KB message buffer plus key material, roughly 21 KB per handshake on top of the existing ~50 KB BearSSL footprint. Fits, but it wants checking that we're not stacking two big contexts at once during fallback.
- **`uint64_t` record sequence number.** The record layer builds the nonce as IV XOR seq and increments seq per record. CW8 PPC has a known `long long` codegen bug (multiply-by-constant), so audit that the increment and XOR paths are safe (they should be, the bug was specific to shift-multiply, but this is exactly the kind of thing that bites silently).
- Cooperative pump, BearSSL primitives only, no new dependencies.

## Stages (hardware-gated, like macEntropy)

### Stage 0 — Design lock
This doc. Decision: port-and-adapt Certainly's 1.3 onto macTLS, 1.2 fallback to the existing engine, X25519 + SHA-256 AEAD only. *(this commit)*

### Stage A — Key schedule  *(DONE, verified on host 2026-05-29)*
Ported as `os9/ostls_tls13_keysched.{c,h}`. Host test `tests/host/test_tls13_keysched.c` checks it against RFC 8446/8448 vectors (SHA-256(""), no-PSK Early Secret, RFC 8448 Handshake Secret, server handshake key + IV), all pass via `make test`. C89-clean under Retro68 (EXIT=0). No hardware needed. Not yet in any CW8 project (added at Stage D).

### Stage B — Record layer  *(DONE, verified on host 2026-05-29)*
Ported as `os9/ostls_tls13_record.{c,h}`. Host test does round-trip plus tampered-payload and tampered-tag cases for both AES-128-GCM and ChaCha20-Poly1305; all pass via `make test`, C89-clean under Retro68. Fixed a real bug in the original: its ChaCha20-Poly1305 decrypt skipped the auth tag check (accepted forged records on our primary cipher); we now compare the computed tag in constant time, which the tamper tests confirm. A captured-real-record known-answer test is deferred to the live handshake at Stage E. CW8 watch item: the uint64 sequence number's 64-bit shifts in compute_nonce.

### Stage C — Handshake state machine  *(DONE, verified on host 2026-05-29)*
Ported as `os9/ostls_tls13_handshake.{c,h}` (2331 lines, C89-clean). The host harness `tests/host/test_tls13_handshake.c` (`make handshake [HOST=...]`) drives the transport-agnostic state machine over a real socket and completes a full TLS 1.3 handshake against **google.com, cloudflare.com, and mactrove.com** — ServerHello, EncryptedExtensions, Certificate validated against our 121 anchors, CertificateVerify, server Finished verified, our Finished sent, all on ChaCha20-Poly1305 (0x1303). Two fixes from driving it live: (1) dropped the SHA-384 suite (0x1302) from the offer since its transcript-rehash path is a stub (SHA-256 only per this scope); (2) wired entropy to macEntropy (`OSTLS_InjectEntropy` for the engine PRNG). HelloRetryRequest is ported but untested (none of the three servers needed it with an X25519 offer).

### Stage D — Integration  *(DONE, G3-verified 2026-05-29)*
The 1.3 path is wired into the async public API (`ostls_async.c`): every connection arms a 1.3 handshake (lazily allocating its ~34 KB context), drives it in `OSTLS_Pump` while handshaking and runs the 1.3 record layer for `OSTLS_Read`/`OSTLS_Write` once open; on `Fallback12` it reconnects and hands off to the BearSSL T0 engine for a full 1.2 handshake. `OSTLS_SetTryTLS13(0)` forces 1.2-only. MacTLSTest **Stage D2 drives the public API against Google and logs `handshake OK TLS1.3 CHACHA20-POLY1305 0x1303` + a full read + clean close** — TLS 1.3 through the same calls MacSurf makes. No regression: D3 (1.2 resumption) still abbreviates.
**Bugs found and fixed driving it on hardware:** (1) a latent `msg_buf[4096]` overflow in `tls13_read_encrypted_hs` — large certificate chains (Google's ~4.5 KB Certificate message) overran the buffer and corrupted the struct; `msg_buf`/`plain_buf` are now sized to a full TLS record (16640) with bounds guards. (2) `br_ssl_client_reset` must NOT run before a 1.3 attempt (it competes for record buffers). (3) the ~34 KB 1.3 context is lazy-allocated, and the Stage G probe's static context too, so the tight MacTLSTest heap can fit the async 1.3 connection.
**Gate:** met — Retro68 C89-clean, builds into MacTLSTest, 1.3 negotiated through the public API on a G3, 1.2 stages intact.

### Stage E — Hardware verification *(MET — full 1.3 handshake on G3 over OT, 2026-05-29)*
**Handshake completes on real PPC.** MacTLSTest Stage G drove a full TLS 1.3 handshake over Open Transport against mactrove.com on the G3: ClientHello → ServerHello → EncryptedExtensions → Certificate (validated vs the 121 anchors) → CertificateVerify → server Finished → our Finished, negotiating ChaCha20-Poly1305 (0x1303), `code=0`. The fix that got it there: non-blocking OT recv (blocking + a 32KB buffer stalled `OTRcv` ~60s waiting to fill instead of returning the available flight). What remains is the production wiring (Stage D) and app-data over the 1.3 record layer; the protocol itself is proven on hardware.

**Crypto verified on G3 (2026-05-29):** MacTLSTest Stage F runs the key schedule + record layer against the RFC vectors plus tamper-reject on real PPC, all PASS, confirming the C89 port compiles under CodeWarrior 8 (not just Retro68) and the 64-bit record sequence number survives CW8 codegen. **Handshake-over-OT shipped (tfixes55, awaiting hardware run):** MacTLSTest Stage G (`ostls_tls13_otprobe.c`) drives the full handshake over Open Transport against mactrove.com, validated against the anchors. When that comes back complete on the G3, the milestone is met. Original gate text below.


On a real G3: negotiate 1.3 against mactrove.com (which serves 1.3), read back the response, and confirm a 1.2-only server still falls back and works. Capture the run log.
**Gate:** 1.3 handshake completes on hardware and fallback is intact. On pass, tag macTLS 1.3 v1.

### Stage G2 — App-data over 1.3 *(host-verified 2026-05-29; hardware pending tfixes56)*
Full HTTP request/response over the 1.3 record layer works on host against **mactrove.com (200 OK), google.com (301), cloudflare.com (301)**. Getting there root-caused a real bug: `hkdf_expand_label` called `br_hmac_out(&mc, out)`, but BearSSL's `br_hmac_out` **always writes the full hash length (32 bytes for SHA-256)** regardless of the requested `out_len`. The 12-byte IV expansions therefore overflowed their buffers by 20 bytes and clobbered adjacent key material. The handshake keys and the RFC 8448 vector test survived by stack-layout luck (the overflow landed on values that were about to be overwritten); the application-key layout did not, so the client app key came out wrong in its first 20 bytes — the server rejected every app record with `bad_record_mac` while the handshake still "completed." Fix: expand into a 64-byte scratch buffer and `memcpy` exactly `out_len` bytes (`os9/ostls_tls13_keysched.c`). Diagnosis was nailed by capturing the server's `CLIENT_TRAFFIC_SECRET_0` via `openssl s_server -keylogfile` and comparing the derived key to ours (IV matched, key's first 20 bytes didn't). The OT probe (`ostls_tls13_otprobe.c`) now drives a GET/response after the handshake so the same end-to-end path can be confirmed on the G3 (tfixes56). The handshake alone is NOT sufficient proof — it completed even with the broken app-key derivation.

## Reference
- Certainly: `/home/patrick/Webs/certainly` (MIT). `src/tls13_handshake.c` (2332 lines), `src/tls13_keysched.c` (345), `src/tls13_record.c` (185), `src/certainly.c` (orchestration + fallback). Their `CLAUDE.md` documents the version strategy.
- RFC 8446 (TLS 1.3), Section 4 (handshake), Section 7.1 (key schedule).
- BearSSL primitives already vendored: `bearssl_hash.h` (SHA-256, HKDF/HMAC), `bearssl_block.h` / `bearssl_aead.h` (AES-GCM, ChaCha20-Poly1305), `bearssl_ec.h` (X25519), `bearssl_x509.h` (minimal validator).
- GitHub issue: mplsllc/macTLS#1.
