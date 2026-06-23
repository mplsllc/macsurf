/*
 * test_tls13_ticket_cache.c -- unit tests for the resumption ticket cache
 * (macTLS#2 Stage E2). Host build only.
 *
 * Uses ticks_per_sec = 1 so "now" is in seconds for readable expiry math.
 */

#include <stdio.h>
#include <string.h>
#include "../../os9/ostls_ticket_cache.h"

static int failures = 0;
static void check(const char *name, int ok)
{
    printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) failures++;
}

/* Build a minimal valid ticket with a recognisable first PSK byte +
 * lifetime, so we can tell cache entries apart. */
static tls13_session_ticket mk(unsigned char tag, uint32_t lifetime)
{
    tls13_session_ticket t;
    memset(&t, 0, sizeof t);
    t.valid = 1;
    t.psk_len = 32;
    t.psk[0] = tag;
    t.cipher_suite = 0x1301;
    t.lifetime = lifetime;
    t.ticket_len = 4;
    t.ticket[0] = tag;
    return t;
}

/* Helper: stash via a temp (can't take the address of mk()'s rvalue). */
static void put(const char *host, unsigned char tag, uint32_t lifetime,
                uint32_t now)
{
    tls13_session_ticket t = mk(tag, lifetime);
    OSTLS_TicketCachePut(host, &t, now);
}

int main(void)
{
    tls13_session_ticket out;
    uint32_t age;
    int i;
    char host[32];

    printf("=== TLS 1.3 Ticket Cache Tests (macTLS) ===\n\n");
    OSTLS_TicketCacheReset();

    /* miss on empty */
    check("miss on empty cache",
          OSTLS_TicketCacheGet("a.com", 100, 1, &out, &age) == 0);

    /* put + hit, with correct payload + age */
    put("a.com", 0xA1, 600, 100);
    check("hit after put",
          OSTLS_TicketCacheGet("a.com", 105, 1, &out, &age) == 1);
    check("returns the stored ticket", out.psk[0] == 0xA1 && out.ticket[0] == 0xA1);
    check("age_ms = 5000 at +5s", age == 5000U);

    /* miss for a different host */
    check("miss for unknown host",
          OSTLS_TicketCacheGet("b.com", 105, 1, &out, &age) == 0);

    /* overwrite same host */
    put("a.com", 0xA2, 600, 200);
    check("overwrite same host wins",
          OSTLS_TicketCacheGet("a.com", 201, 1, &out, &age) == 1 &&
          out.psk[0] == 0xA2);

    /* expiry: lifetime 10s, query at +11s -> miss */
    OSTLS_TicketCacheReset();
    put("exp.com", 0xEE, 10, 1000);
    check("live just before expiry",
          OSTLS_TicketCacheGet("exp.com", 1009, 1, &out, &age) == 1);
    check("expired at +11s",
          OSTLS_TicketCacheGet("exp.com", 1011, 1, &out, &age) == 0);
    check("expired slot freed (still miss)",
          OSTLS_TicketCacheGet("exp.com", 1011, 1, &out, &age) == 0);

    /* LRU eviction: fill all slots, then one more; oldest must be gone */
    OSTLS_TicketCacheReset();
    for (i = 0; i < OSTLS_TICKET_CACHE_SLOTS; i++) {
        sprintf(host, "h%d.com", i);
        put(host, (unsigned char)(0x10 + i), 3600,
            (uint32_t)(500 + i));   /* h0 oldest */
    }
    put("new.com", 0x99, 3600, 600);
    check("LRU: oldest (h0) evicted",
          OSTLS_TicketCacheGet("h0.com", 600, 1, &out, &age) == 0);
    check("LRU: newest present",
          OSTLS_TicketCacheGet("new.com", 600, 1, &out, &age) == 1 &&
          out.psk[0] == 0x99);
    sprintf(host, "h%d.com", OSTLS_TICKET_CACHE_SLOTS - 1);
    check("LRU: a non-oldest survivor present",
          OSTLS_TicketCacheGet(host, 600, 1, &out, &age) == 1);

    /* invalid inputs are no-ops / safe */
    OSTLS_TicketCacheReset();
    OSTLS_TicketCachePut("x.com", NULL, 1);
    check("put NULL ticket is a no-op",
          OSTLS_TicketCacheGet("x.com", 2, 1, &out, &age) == 0);

    printf("\n%d test(s) failed.\n", failures);
    return failures > 0 ? 1 : 0;
}
