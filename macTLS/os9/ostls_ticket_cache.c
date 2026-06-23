/*
 * ostls_ticket_cache.c -- host-keyed TLS 1.3 resumption ticket cache.
 * See ostls_ticket_cache.h. macTLS#2 Stage E2.
 *
 * C89 / CodeWarrior 8 clean: no inline, no // comments, decls at block top.
 */

#include "ostls_ticket_cache.h"
#include <string.h>

typedef struct {
    int                  used;
    char                 host[256];
    uint32_t             received;   /* caller clock at store time */
    tls13_session_ticket t;
} ostls_ticket_slot;

static ostls_ticket_slot g_slots[OSTLS_TICKET_CACHE_SLOTS];

void OSTLS_TicketCacheReset(void)
{
    memset(g_slots, 0, sizeof g_slots);
}

/* Case-sensitive host compare bounded to the slot field. Hosts are
 * already normalised lowercase by the caller (the fetcher). */
static int host_eq(const char *a, const char *b)
{
    return strncmp(a, b, 255) == 0;
}

void OSTLS_TicketCachePut(const char *host, const tls13_session_ticket *t,
                          uint32_t now)
{
    int i;
    int target;
    uint32_t oldest_recv;

    if (host == NULL || t == NULL || !t->valid || t->ticket_len == 0) {
        return;
    }

    /* Prefer an existing entry for this host. */
    target = -1;
    for (i = 0; i < OSTLS_TICKET_CACHE_SLOTS; i++) {
        if (g_slots[i].used && host_eq(g_slots[i].host, host)) {
            target = i;
            break;
        }
    }

    /* Else an empty slot. */
    if (target < 0) {
        for (i = 0; i < OSTLS_TICKET_CACHE_SLOTS; i++) {
            if (!g_slots[i].used) {
                target = i;
                break;
            }
        }
    }

    /* Else evict the least-recently-stored. */
    if (target < 0) {
        target = 0;
        oldest_recv = g_slots[0].received;
        for (i = 1; i < OSTLS_TICKET_CACHE_SLOTS; i++) {
            if (g_slots[i].received < oldest_recv) {
                oldest_recv = g_slots[i].received;
                target = i;
            }
        }
    }

    g_slots[target].used = 1;
    strncpy(g_slots[target].host, host, 255);
    g_slots[target].host[255] = '\0';
    g_slots[target].received = now;
    g_slots[target].t = *t;
}

int OSTLS_TicketCacheGet(const char *host, uint32_t now, uint32_t ticks_per_sec,
                         tls13_session_ticket *out, uint32_t *out_age_ms)
{
    int i;

    if (host == NULL || out == NULL || ticks_per_sec == 0) {
        return 0;
    }

    for (i = 0; i < OSTLS_TICKET_CACHE_SLOTS; i++) {
        if (g_slots[i].used && host_eq(g_slots[i].host, host)) {
            uint32_t elapsed_ticks = now - g_slots[i].received; /* wraps OK */
            uint32_t elapsed_sec = elapsed_ticks / ticks_per_sec;

            if (elapsed_sec >= g_slots[i].t.lifetime) {
                /* Expired: free the slot and report a miss. */
                g_slots[i].used = 0;
                return 0;
            }

            *out = g_slots[i].t;
            if (out_age_ms != NULL) {
                /* ms since received; guard the multiply against overflow
                 * (elapsed_sec is well under the ticket lifetime cap). */
                *out_age_ms = elapsed_sec * 1000U +
                              ((elapsed_ticks % ticks_per_sec) * 1000U)
                                  / ticks_per_sec;
            }
            return 1;
        }
    }
    return 0;
}
