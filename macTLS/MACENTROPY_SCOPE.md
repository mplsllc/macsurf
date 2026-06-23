# macEntropy — production entropy for macTLS

**Status: macEntropy v1.0 — hardware-validated on a real G3 (2026-05-29).**
Stages A, C, B, E all confirmed. The `OSTLS_ENTROPY_STAGE_A_INSECURE`
reminder has been removed. Stage D (host stir seam) rides along with the
MacSurf fold-in (Stage F), which is the only remaining work.

**Goal (closes the project):** macTLS seeds BearSSL with cryptographically
defensible entropy on a real Power Macintosh G3, including on the *first*
handshake after a clean boot, with statistical validation to back the
claim. When this lands, the `OSTLS_ENTROPY_STAGE_A_INSECURE` compile-time
reminder is removed and macTLS is honestly shippable to end users.

This is a macTLS-internal effort. Nothing here touches MacSurf until the
explicit fold-in stage (F), which is gated on hardware validation (E).

---

## Why this is the next macTLS work

macTLS reached first-light HTTPS on real hardware, but its RNG is the
honest weak point. Today ([os9/ostls_entropy.c](os9/ostls_entropy.c)):

- The pool accumulator is a **rotate-1-bit + XOR mix** — not a
  cryptographic hash. It satisfies BearSSL's "did you give me anything"
  check; it does not produce unpredictable seed material.
- **No seed-file persistence.** The first handshake after a cold boot —
  before any mouse movement or network traffic has accumulated jitter —
  is the thinnest the pool ever is. Nothing carries entropy across boots.
- **Narrow sources.** Only `TickCount`, `Microseconds`, mouse position,
  and stack address. No OT packet-arrival jitter (macTLS *owns* the
  endpoints — free, high-quality timing), no key-down latency.
- **No accounting.** Nothing tracks how much real entropy was gathered
  before a handshake fires, so the cold-start moment is unguarded.

Every future macTLS consumer inherits this: a mail client's password,
a vault's key material, a signing operation. The RNG is the floor under
all of it, so it goes first.

---

## Design decision (locked at Stage 0)

**We do not roll our own DRBG.** BearSSL's SSL engine already runs an
internal HMAC-DRBG that is seeded by `br_ssl_engine_inject_entropy`. Our
job is to deliver a high-quality 32-byte seed and to keep re-seeding —
not to invent a generator.

So macEntropy is **a good seed accumulator + persistence + source
breadth + health accounting**, built on primitives BearSSL already links:

- Accumulator: a running **`br_sha256`** pool (replaces rotate-XOR).
- Seed output: `SHA-256(pool-state || counter || fresh-samples)`, with
  the output folded back into the pool so successive seeds differ.
- For non-TLS consumers later (the vault will need raw random bytes):
  expose `OSTLS_RandomBytes()` backed by **`br_hmac_drbg`** seeded from
  the same pool. Flagged as a Stage-D+ extension, out of the v1.0 gate.

This stays inside the macsurf-first compatibility contract: CW8 C89,
no new dependencies (BearSSL `br_sha256` / `br_hmac_drbg` already linked),
small fixed memory, cooperative (no threads).

---

## Compatibility floors (from the macsurf-first contract)

Every stage below must hold all of these or it doesn't land:

- **CW8 C89** — compiles clean under CodeWarrior 8, C89, no `//`, no
  designated initializers, no VLAs, vars at block top. Retro68 PPC GCC
  pre-flight (`-std=c89 -pedantic-errors`) is the Linux-side gate.
- **No new dependencies** — BearSSL + Toolbox only.
- **Cooperative** — no threads; collection runs from the host idle loop
  and macTLS's own OT notifier path.
- **Small, fixed memory** — pool + DRBG context are static, kilobytes,
  well within the 16 MB Carbon partition.
- **API stays additive** — `OSTLS_CollectEntropy` / `OSTLS_InjectEntropy`
  signatures are preserved; new capability arrives as new calls.

---

## Stages (hardware-gated; failed stages archived with post-mortems)

### Stage 0 — Design lock
**Deliverable:** this document. Decision: SHA-256 accumulator feeding
BearSSL's engine DRBG; no home-grown generator; seed-file + breadth +
health as the v1.0 scope; `OSTLS_RandomBytes` deferred.
**Gate:** doc published, no code. *(this commit)*

### Stage A — Cryptographic accumulator  *(VERIFIED on G3, 2026-05-29)*
Replace `entropy_mix`'s rotate-XOR with a `br_sha256`-based pool. Keep a
persistent `br_sha256_context`; `update` it with each source sample; on
inject, finalize a copy to produce the 32-byte seed, then fold that seed
back in as the new state. `OSTLS_CollectEntropy` / `OSTLS_InjectEntropy`
keep their signatures.
**Gate:** builds under CW8 + Retro68 clean; `MacTLSTest` HTTPS handshake
against a live host still succeeds on a real G3 (no regression vs the
current weak mix).

### Stage B — Source breadth + entropy accounting  *(VERIFIED on G3, 2026-05-29)*
*OT packet-arrival jitter (OSTLS_StirTimer at OTRcv) + sample accounting
shipped. Note: the stir fires on the async pump only; the synchronous
v0.1 handshake/fetch paths get clock/stack/seed/counter entropy but no
packet jitter. Async is the production (macsurf) path, so this is an
accepted boundary; stirring the sync path is optional follow-up.*

Add the sources macTLS can gather itself:
- **OT packet-arrival jitter** — mix `Microseconds`/`TickCount` low bits
  at each `OTRcv` in the async pump (macTLS owns the endpoint; this is
  free high-rate timing entropy during any fetch).
- **Key-down latency jitter** (when the host feeds it — see Stage D).
- A **sample counter + coarse entropy estimate**, and a health query
  (`OSTLS_EntropyHealth` or similar) so a caller/handshake can tell
  whether the pool crossed a minimum-samples threshold.
**Gate:** on G3, counter shows samples accumulating across an idle
interval and across a fetch; handshake proceeds only past threshold
(or logs a cold-start warning when below it).

### Stage C — Seed-file persistence (the cold-start fix)  *(VERIFIED on G3, 2026-05-29)*
Read a seed file at first use (`FindFolder` → Preferences folder), mix
its bytes into the pool — **never trust the file alone**, always combine
with live samples. Rewrite the file periodically and at clean shutdown
with fresh seed output. This is what makes the first handshake after a
cold boot trustworthy.
**Gate:** on G3, a clean relaunch's first handshake demonstrably draws on
the persisted seed (file written and re-read; pool/seed differs run to
run; cold-boot output is not constant).

### Stage D — Host stir seam (macsurf integration contract)
Add `OSTLS_StirEntropy(const void *bytes, UInt32 len)` so a host event
loop can feed mouse-delta + key-latency + event-arrival jitter. **Self-
gather stays the fallback** — a host that feeds nothing still gets a
working (thinner) pool; macsurf's feed is the reference path the design
is tuned against. Document the macsurf event-loop call sites here, but
**do not edit macsurf** — that is Stage F.
**Gate:** `MacTLSTest` feeds synthetic stir samples; pool reflects them;
handshake unaffected.

### Stage E — Hardware statistical validation  *(v1.0 milestone gate — PASSED on G3, 2026-05-29)*
*Two MacTLSTest runs: both PASS within-run (dups=0, buckets=256/256,
maxbkt 17/20, bit balance tight), and the batch fingerprints differed
across launches (94A7251B vs 52AB2050) — the cross-run entropy proof.
`OSTLS_ENTROPY_STAGE_A_INSECURE` removed; tagged macEntropy v1.0.*

On a real G3: capture seed outputs across many handshakes and across
multiple clean boots; run basic statistical sanity — no constant or
stuck bytes, reasonable byte-value spread, run-to-run variance, and
distinct cold-boot seeds. Deliverable: a validation log under
`docs/runs/`.
**Gate:** outputs pass sanity and cold-start seeds differ across clean
boots. On pass: **remove `OSTLS_ENTROPY_STAGE_A_INSECURE`**, tag macTLS
**macEntropy v1.0**.

### Stage F — Fold into MacSurf  *(separate, macsurf-side, held until E)*
Bump the MacSurf submodule pointer to the validated macTLS commit; add
the `OSTLS_StirEntropy` calls in MacSurf's `WaitNextEvent` loop; add any
new `.c` to `MacSurf.mcp` (flagged in the handoff, never edited directly).
**Gate:** MacSurf native HTTPS still works on hardware, now on production
entropy.

---

## Out of scope for v1.0 (deferred)
- `OSTLS_RandomBytes()` general-purpose RNG (HMAC-DRBG) for non-TLS
  consumers — the vault wants this; it's a clean Stage-D+ extension once
  the pool is trustworthy.
- Hardware RNG sources (none exist on this platform).
- Entropy daemon / background collection (no threads on OS 9).

## Reference points
- Current module: [os9/ostls_entropy.c](os9/ostls_entropy.c),
  [os9/ostls_entropy.h](os9/ostls_entropy.h)
- Inject site: `ostls_setup_bearssl` in
  [os9/ostls_async.c](os9/ostls_async.c) (and the v0.1 fetch path)
- BearSSL primitives: `bearssl_hash.h` (`br_sha256`),
  `bearssl_rand.h` (`br_hmac_drbg`), `br_ssl_engine_inject_entropy`
- Standalone test harness: [MacTLSTest/](MacTLSTest/)
