# TLS 1.3 Session Resumption for macTLS

## Why this, why now

The single biggest per-page cost on a G3 is the TLS handshake: an ECDHE key
exchange (P-384 is brutal on a 233–450 MHz PPC) **plus** an RSA/ECDSA
certificate-chain verification, paid fresh on every connection — and a page
opens many (HTML + each CSS/image/font). Resumption lets us reconnect to a
site we've already talked to by presenting a "ticket" the server handed us,
skipping the cert verify entirely and, in the common case, most of the key
exchange work. It's the only purely-performance lever in the macTLS roadmap.

## The bet (locked at Stage 0)

- **`psk_dhe_ke` only.** Always run a fresh ECDHE alongside the PSK (keeps
  forward secrecy, and the server still picks the curve). Skip `psk_ke`.
- **1-RTT resumption only. No 0-RTT / `early_data` in v1.** 0-RTT adds replay
  risk and a second key schedule for marginal extra savings; defer.
- **One ticket per host, cached in RAM** (small fixed table, LRU). Not
  persisted to disk in v1 — survives within a browsing session, not across
  relaunches.
- **SHA-256 suites only at first.** The resumption PSK is hash-bound; a ticket
  minted under AES_128_GCM_SHA256 can't resume into a SHA-384 suite. Cache the
  suite/hash with the ticket and only offer the PSK to a matching suite.
- **Graceful fallback always.** A missing/expired/rejected ticket just means a
  normal full handshake. Resumption is never load-bearing for correctness.

## Scope

In: derive resumption secret, parse + cache NewSessionTicket, build a
resumption ClientHello (`psk_key_exchange_modes` + `pre_shared_key` + binder),
resumed key schedule, ServerHello accept/reject handling, async-layer wiring.
Out (v1): 0-RTT/early_data, on-disk ticket persistence, multiple tickets per
host, KeyUpdate, post-quantum.

## Compatibility floors

Real servers that issue tickets (mactrove/Cloudflare/nginx, 68kmla). Same
hardware floor as 1.3 (G3 iMac OS 9.2.2). Must not regress the full-handshake
path when no ticket is present.

## Current state (what 1.3 already gives us)

- Key schedule derives Early → Handshake → Master and app/finished keys
  ([ostls_tls13_keysched.c]); **no** resumption_master_secret yet — add it.
- NewSessionTicket is **silently discarded** in `tls13_handle_post_handshake`
  ([ostls_tls13_handshake.c] ~L2207) — replace with parse + store.
- Connection struct has `host[256]` and a lazy `hs13` context
  ([ostls_async.c]) — natural cache key + where to consult/populate.
- HKDF-Expand-Label + transcript machinery already exist and are correct
  (the [[project_mactls_hkdf_expand_overflow]] scratch-buffer rule applies to
  any sub-digest output here too).

## Stages (hardware-gated, like 1.3 / macEntropy)

### Stage 0 — Design lock
The bet above. No code.

### Stage A — Resumption secret derivation  *(DONE, host KAT 2026-05-29)*
`tls13_ks_derive_resumption_master` (Derive-Secret(Master, "res master",
transcript-through-client-Finished)) + `tls13_ks_derive_resumption_psk`
(HKDF-Expand-Label(res_master, "resumption", ticket_nonce, hash_len)) in
[ostls_tls13_keysched.c]. Known-answer test in
tests/host/test_tls13_keysched.c (`test_resumption`) pins both against an
independent HKDF-Expand-Label reference; passes alongside the existing
RFC 8448 vectors.

### Stage B — Ticket parse + PSK mint  *(DONE, host test 2026-05-29)*
`tls13_parse_new_session_ticket` ([ostls_tls13_handshake.c]) parses the
message (lifetime, age_add, nonce, ticket, extensions skipped), mints the
resumption PSK from `res_master` + nonce, and fills a `tls13_session_ticket`.
res_master is derived at SendFinished (Step 7.5) while the Master Secret is
still live and stashed on the hs ctx; the post-handshake handler stashes the
parsed ticket in `hs->ticket`/`ticket_valid`. Host test
tests/host/test_tls13_ticket.c verifies field extraction, the minted PSK (vs
an HKDF reference), and the malformed/oversize/no-res_master rejection paths.
**The host-keyed RAM cache that stores tickets across connections moved to
Stage E** (its only consumer is OSTLS_Start, so it lives with the async
wiring).

### Stage C — Resumption ClientHello + binder
Split in two: **C1 binder key schedule (DONE, host KAT 2026-05-29)** —
`tls13_ks_extract_early_psk` (Early Secret = HKDF-Extract(0, PSK)) +
`tls13_ks_derive_binder_key` ("res binder") in [ostls_tls13_keysched.c];
binder finished key reuses `tls13_ks_derive_finished_key`. KAT in
`test_binder_key` pins Early/binder_key/finished_key vs an HKDF reference.
**C2 ClientHello assembly (DONE, host test 2026-05-29):** when `hs->resuming`
+ `offer_ticket` are set, the builder appends `psk_key_exchange_modes(psk_dhe_ke)`
then `pre_shared_key` last (identity = ticket + obfuscated_ticket_age, binder
placeholder), backpatches all length fields to count the binders, then patches
the binder via `tls13_compute_binder` over the message truncated before the
binders (continuing the running transcript so HRR works). `tls13_compute_binder`
has a host KAT in test_tls13_ticket.c (vs HKDF reference). The byte-assembly's
true gate is a server accepting the binder — verified end-to-end at Stage E.

### Stage D — Resumed key schedule + accept/reject  *(code DONE 2026-05-30; live gate at E)*
ServerHello parser now handles `pre_shared_key` (single uint16
selected_identity) and sets `hs->resumption_accepted`. When accepted, the
Early Secret comes from the PSK (`tls13_ks_extract_early_psk`) instead of
zeros — the Handshake Secret still folds in the fresh ECDHE (psk_dhe_ke) —
and EncryptedExtensions advances straight to RecvFinished, skipping
Certificate/CertificateVerify. When the server declines (no echo) everything
falls through to the existing full-handshake path untouched. C89-clean, full
host suite still green; correctness proven by the live resume at Stage E.

### Stage E — Live resume gate + ticket cache + async integration
**E1 live resume gate (DONE, 2026-05-30):** `tests/host/test_tls13_resume.c`
does two connections to a real server (`make resume RHOST=68kmla.org`):
conn1 full handshake, capture a NewSessionTicket via the post-handshake
handler; conn2 resumes. **Verified against 68kmla.org:** server echoes
`pre_shared_key` (binder accepted), the resumed handshake reaches Complete
with the certificate skipped, and an HTTP response flows over resumed keys.
ASan/MSan/UBSan all complete the full resume clean. Two real fixes fell out:
(a) `psk_key_exchange_modes` is now sent on EVERY ClientHello — servers won't
issue tickets without it (RFC 8446 4.2.9), so we never received one before;
(b) the CH binder uses a keysched keyed to the ticket's hash (`hs->ks` isn't
set up until ServerHello — using it crashed on a NULL hash). Note: servers
that don't ticket these connections (Cloudflare/Google/nginx-without-tickets)
SKIP cleanly; 68kmla (XenForo) tickets reliably.
**E2 cache + async wiring (DONE host-side 2026-05-30; hardware gate = F):**
new `ostls_ticket_cache.[ch]` — a 6-slot host-keyed RAM cache (LRU, lifetime
expiry, caller-supplied clock so it's host-testable). 13-case host test in
tests/host/test_tls13_ticket_cache.c (put/hit/miss/overwrite/expiry/LRU/age)
all pass. Wired into `ostls_async.c`: at handshake init, consult the cache for
the host and (on a live hit) set hs13->resuming + offer_ticket +
offer_obfuscated_age; in pump_tls13_consume_record, a post-handshake handshake
record now runs through tls13_handle_post_handshake and any resulting ticket
is cached for the host (TickCount, 60/sec). C89-clean. New conn field
offer_ticket_storage backs the offered ticket. The full resume-through-the-
async-layer can only be exercised on hardware → Stage F.

### Stage F — Hardware verification *(gate)*
On the G3, time full vs resumed handshake to a real ticketing server. Accept
when the resumed connection completes, app-data flows, and it's measurably
faster (cert-verify + ECDHE-verify skipped).

## Reference

RFC 8446 §2.2 (resumption + PSK), §4.2.11 (pre_shared_key), §4.2.9
(psk_key_exchange_modes), §4.6.1 (NewSessionTicket), §7.1 (key schedule incl.
resumption_master_secret). Certainly's resumption path (cloned at
/home/patrick/Webs/certainly) is the C reference, same as the 1.3 port.
