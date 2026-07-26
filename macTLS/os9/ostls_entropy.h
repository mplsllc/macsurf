/*
 * ostls_entropy.h
 *
 * macEntropy v1.0 -- production entropy for macTLS. See MACENTROPY_SCOPE.md.
 *
 * Hardware-validated on a real Power Macintosh G3 (2026-05-29).
 *
 * The pool is a running BearSSL SHA-256 context that every source sample
 * is folded into; seed extraction clones the pool, mixes a domain-
 * separation tag, finalises 32 bytes, and folds the result back so
 * successive seeds are independent. The seed is injected into BearSSL's
 * engine HMAC-DRBG (macTLS supplies the seed material, BearSSL runs the
 * generator). Sources: high-resolution clock, mouse jitter, stack noise,
 * OT packet-arrival timing during fetches, and a Preferences-folder seed
 * file persisted across boots so the first handshake after a cold boot
 * is not thin. The Stage E self-test confirmed non-degenerate output and
 * distinct seed streams across separate launches.
 */

#ifndef OSTLS_ENTROPY_H
#define OSTLS_ENTROPY_H

#include "bearssl_ssl.h"

/*
 * Inject entropy into a BearSSL SSL engine.
 *
 * In v0.1/v0.2 this was an insecure stub (Stage A).
 * In v1.0 this gathers data from the global entropy pool.
 */
int OSTLS_InjectEntropy(br_ssl_engine_context *eng);

/*
 * Gathers entropy from jittery sources:
 *   - Mouse position/delta
 *   - Keyboard latency jitter (if called from event loop)
 *   - System clock (TickCount/Microseconds)
 *
 * Should be called periodically (e.g. 60Hz from the app's idle loop).
 */
void OSTLS_CollectEntropy(void);

/*
 * Cold-start seed persistence (Stage C).
 *
 * OSTLS_LoadSeed reads the persisted seed file (Preferences folder) and
 * folds it into the pool. It is called automatically the first time the
 * pool is used, so cold-start benefit needs no host cooperation; it is
 * also exported for a host that wants to load explicitly at startup.
 *
 * OSTLS_SaveSeed extracts a fresh, domain-separated 32-byte seed and
 * writes it back. It fires automatically once per process on the first
 * entropy injection. A host SHOULD also call it at a clean shutdown to
 * persist the full session's accumulated entropy. The seed file is only
 * ever mixed in alongside live samples -- never trusted alone.
 */
void OSTLS_LoadSeed(void);
void OSTLS_SaveSeed(void);

/*
 * Source breadth + accounting (Stage B).
 *
 * OSTLS_StirTimer folds a fresh high-resolution timestamp (Microseconds
 * + TickCount) plus a caller hint into the pool. macTLS calls it at each
 * OTRcv that delivers bytes, so the unpredictable arrival timing of
 * every network packet during a fetch becomes entropy -- a high-rate
 * source that is genuinely jittery and needs no host cooperation. Safe
 * to call frequently; it is pure computation plus two clock reads.
 *
 * OSTLS_EntropySampleCount returns the number of source samples folded
 * into the pool so far -- a coarse accounting signal (not a true
 * entropy estimate) for telling whether the pool has been fed.
 */
void OSTLS_StirTimer(unsigned long hint);
unsigned long OSTLS_EntropySampleCount(void);

/*
 * Host stir seam (Stage D). A host event loop calls this with whatever
 * jittery bytes it has on hand -- typically an EventRecord (mouse
 * location, event time in ticks, key code, message) -- to fold them into
 * the pool alongside a fresh timestamp. This is how key-press latency and
 * mouse-delta entropy reach macTLS. Self-gathered sources (clock, OT
 * packet-arrival jitter, the seed file) work without it; this just adds
 * the richest source when a host is present. data may be NULL.
 */
void OSTLS_StirEntropy(const void *data, unsigned long len);

/*
 * Statistical self-test (Stage E). Extracts a batch of seeds and checks
 * for non-degenerate output: successive seeds differ, byte values spread
 * across the range, bit balance near 50%. Returns 0 on pass, nonzero on
 * fail, and writes a 32-bit fingerprint of the batch to *out_fp (NULL is
 * allowed). Compare the fingerprint across separate launches: it MUST
 * differ, which is the real proof that per-run entropy is incorporated.
 * The within-run checks only guard against degenerate output, since a
 * SHA-256 hash chain looks random regardless of input entropy.
 */
int OSTLS_EntropySelfTest(unsigned long *out_fp);

/*
 * fixes1069 — fill out[0..len) with pool output, for callers outside the
 * TLS handshake (crypto.getRandomValues / crypto.randomUUID). Uses its own
 * domain-separation tag, so this stream is independent of the TLS seed and
 * of the persisted seed file. Any length; the pool ratchets between blocks.
 * No-op if out is NULL or len is 0.
 */
void OSTLS_RandomBytes(void *out, unsigned long len);

#endif /* OSTLS_ENTROPY_H */
