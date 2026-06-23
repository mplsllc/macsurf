/*
 * ostls_ticket_cache.h -- host-keyed TLS 1.3 resumption ticket cache
 * (macTLS#2 Stage E2).
 *
 * A small fixed-slot RAM cache (no disk persistence in v1) that remembers
 * one NewSessionTicket per host so the next connection to that host can
 * resume instead of doing a full handshake. LRU eviction, lifetime expiry.
 *
 * The clock is supplied by the caller so the module is host-testable:
 * pass (now, ticks_per_sec) where now is in whatever unit the caller
 * uses (Mac: TickCount(), 60 ticks/sec; host test: seconds, 1 tick/sec).
 */

#ifndef OSTLS_TICKET_CACHE_H
#define OSTLS_TICKET_CACHE_H

#include "ostls_tls13_handshake.h"   /* tls13_session_ticket */
#include <stdint.h>

#define OSTLS_TICKET_CACHE_SLOTS 6

/* Drop all cached tickets (also the initial state at startup). */
void OSTLS_TicketCacheReset(void);

/* Store ticket `t` for `host`, stamped at `now`. Overwrites an existing
 * entry for the same host; otherwise fills an empty slot, else evicts the
 * least-recently-stored. No-op if t is NULL/invalid. */
void OSTLS_TicketCachePut(const char *host, const tls13_session_ticket *t,
                          uint32_t now);

/* Look up a live (non-expired) ticket for `host`. Returns 1 and fills
 * *out (and *out_age_ms = milliseconds since the ticket was received, for
 * obfuscated_ticket_age) on a hit; returns 0 on miss or expiry.
 * elapsed_seconds = (now - received) / ticks_per_sec. */
int OSTLS_TicketCacheGet(const char *host, uint32_t now, uint32_t ticks_per_sec,
                         tls13_session_ticket *out, uint32_t *out_age_ms);

#endif /* OSTLS_TICKET_CACHE_H */
