/*
 * MacSurf  -  content/macsurf_nav_seed.h
 *
 * MacSurf Trace, Phase 1a: navigation-identity handoff across the
 * browser_window -> hlcache -> llcache -> fetch boundary WITHOUT widening
 * any exported NetSurf signature (see reference_cw8_no_signature_widening:
 * fixes917-919 were reverted byte-for-byte over exactly that).
 *
 * A "seed" is ambient ONLY between two adjacent synchronous calls: the caller
 * sets it immediately before the boundary call; the callee consumes-and-clears
 * it at its first executable lines, before any allocation or early return, and
 * copies it into durable per-operation state (ctx->nav_id, object->fetch.nav_id,
 * fetch->nav_id). No asynchronous callback ever reads a pending seed. This is
 * categorically different from a persistent "current nav" global.
 *
 * nav_id 0 == unattributed (chrome / favicon / built-in stylesheet). That is a
 * valid answer, not an error.
 *
 * The seed state lives in the always-linked TUs it guards:
 *   macsurf_hlcache_seed_nav / _consume  -> content/hlcache.c
 *   macsurf_llcache_seed_nav  / _consume  -> content/llcache.c
 *   macsurf_fetch_seed        / _consume  -> content/fetch.c
 * plus macsurf_next_request_id() (monotonic, session-local) in content/fetch.c.
 */

#ifndef MACSURF_NAV_SEED_H
#define MACSURF_NAV_SEED_H

/* --- hlcache boundary (top-level document; child fetches carry nav_id on
 *     hlcache_child_context instead) --- */
void macsurf_hlcache_seed_nav(unsigned long nav_id);
unsigned long macsurf_hlcache_consume_seed(void);

/* --- llcache boundary --- */
void macsurf_llcache_seed_nav(unsigned long nav_id);
unsigned long macsurf_llcache_consume_seed(void);

/* --- fetch boundary --- */
void macsurf_fetch_seed(unsigned long nav_id, unsigned long redirect_from);
/* out params may be NULL; both are zeroed if no seed is pending */
void macsurf_fetch_consume_seed(unsigned long *nav_id_out,
			       unsigned long *redirect_from_out);

/* Monotonic session-local wire-request id. Never returns 0. */
unsigned long macsurf_next_request_id(void);

#endif /* MACSURF_NAV_SEED_H */
