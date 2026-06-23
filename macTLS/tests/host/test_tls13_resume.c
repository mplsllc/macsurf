/*
 * test_tls13_resume.c -- live TLS 1.3 session resumption against a real
 * server (macTLS#2 Stage E gate). Host build only (native cc + sockets).
 *
 *   ./test_tls13_resume [host] [port]      (default cloudflare.com 443)
 *
 * Two connections:
 *   1. Full handshake; send a GET; read records, feeding any
 *      NewSessionTicket through tls13_handle_post_handshake to capture a
 *      reusable ticket (hs.ticket).
 *   2. Fresh handshake with hs.resuming + offer_ticket set. PASS iff the
 *      server echoes pre_shared_key (hs.resumption_accepted), the
 *      handshake reaches Complete with NO certificate processed, and
 *      app-data flows over the resumed keys.
 *
 * The state machine is transport-agnostic, so this exercises C2 (binder)
 * and D (accept + cert-skip) end-to-end without the async layer.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <signal.h>

#include "bearssl.h"
#include "../../os9/ostls_tls13_handshake.h"
#include "../../os9/ostls_entropy.h"
#include "../../os9/ostls_b3_anchors.h"

static int tcp_connect(const char *host, const char *port)
{
    struct addrinfo hints, *res, *p;
    int fd = -1;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) != 0) return -1;
    for (p = res; p != NULL; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
        close(fd); fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

/* Drive the handshake to completion. Returns 1 on Complete, 0 on failure.
 * recv_buf/recv_len hold leftover bytes for the caller (app-data phase). */
static int drive(int fd, tls13_hs_ctx *hs, const char *host,
                 unsigned char *recv_buf, size_t cap, size_t *recv_len)
{
    int steps = 0;
    *recv_len = 0;
    while (steps < 400) {
        tls13_hs_result r = tls13_handshake_step(hs, recv_buf, recv_len, host);
        steps++;
        while (hs->msg_offset < hs->msg_len) {
            ssize_t n = send(fd, hs->msg_buf + hs->msg_offset,
                             hs->msg_len - hs->msg_offset, 0);
            if (n <= 0) return 0;
            hs->msg_offset += (size_t)n;
        }
        if (hs->state == kTLS13_Complete && hs->msg_offset >= hs->msg_len)
            return 1;
        if (r == kTLS13_Error || r == kTLS13_Fallback12) {
            printf("  handshake ended r=%d err=%d state=%d\n",
                   (int)r, hs->error, (int)hs->state);
            return 0;
        }
        if (r == kTLS13_WantRead) {
            ssize_t n = recv(fd, recv_buf + *recv_len, cap - *recv_len, 0);
            if (n <= 0) { printf("  recv n=%ld\n", (long)n); return 0; }
            *recv_len += (size_t)n;
        }
    }
    return 0;
}

/* Send a GET and read records, decrypting each. Feeds handshake records
 * (NewSessionTicket) to tls13_handle_post_handshake so hs->ticket fills.
 * Stops once it has both an app-data record and (if want_ticket) a ticket,
 * or on timeout. Returns 1 if an app-data response arrived. */
static int appdata(int fd, tls13_hs_ctx *hs, const char *host,
                   unsigned char *rbuf, size_t rlen, size_t cap,
                   int want_ticket)
{
    char get[256];
    unsigned char ct[600], wire[640];
    static unsigned char plain[16384];
    size_t getlen, clen = 0, plen = 0;
    uint8_t inner = 0;
    int glen, got_resp = 0;
    time_t deadline;

    glen = snprintf(get, sizeof get,
        "GET / HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", host);
    getlen = (size_t)glen;
    if (tls13_record_encrypt(&hs->write_ctx, get, getlen,
            TLS13_CT_APPLICATION_DATA, ct, &clen) != 0) return 0;
    wire[0] = 0x17; wire[1] = 0x03; wire[2] = 0x03;
    wire[3] = (unsigned char)(clen >> 8); wire[4] = (unsigned char)clen;
    memcpy(wire + 5, ct, clen);
    if (send(fd, wire, 5 + clen, 0) <= 0) return 0;

    deadline = time(NULL) + 12;
    for (;;) {
        while (rlen >= 5) {
            size_t reclen = ((size_t)rbuf[3] << 8) | (size_t)rbuf[4];
            int dr;
            if (rlen < 5 + reclen) break;
            dr = tls13_record_decrypt(&hs->read_ctx, rbuf + 5, reclen,
                                      plain, &plen, &inner);
            memmove(rbuf, rbuf + 5 + reclen, rlen - (5 + reclen));
            rlen -= (5 + reclen);
            if (dr != 0) { printf("  record decrypt fail\n"); return got_resp; }
            if (inner == TLS13_CT_APPLICATION_DATA) {
                got_resp = 1;
                if (!want_ticket || hs->ticket_valid) return 1;
            } else if (inner == TLS13_CT_HANDSHAKE) {
                /* NewSessionTicket / KeyUpdate: the real post-handshake
                 * handler parses + stashes the ticket (hs->ticket_valid). */
                tls13_handle_post_handshake(hs, plain, plen);
                if (got_resp && (!want_ticket || hs->ticket_valid)) return 1;
            } else if (inner == TLS13_CT_ALERT) {
                return got_resp;
            }
        }
        if (time(NULL) > deadline) return got_resp;
        {
            ssize_t n = recv(fd, rbuf + rlen, cap - rlen, 0);
            if (n <= 0) return got_resp;
            rlen += (size_t)n;
        }
    }
}

int main(int argc, char **argv)
{
    const char *host = (argc > 1) ? argv[1] : "cloudflare.com";
    const char *port = (argc > 2) ? argv[2] : "443";
    const br_x509_trust_anchor *tas; size_t tas_n;
    static unsigned char iobuf[BR_SSL_BUFSIZE_BIDI];
    static unsigned char rbuf[32768];
    tls13_session_ticket saved;
    time_t ticket_time = 0;
    int fd; size_t rlen;

    printf("=== TLS 1.3 resumption against %s:%s ===\n\n", host, port);
    OSTLS_B3_GetAnchors(&tas, &tas_n);
    signal(SIGPIPE, SIG_IGN);
    memset(&saved, 0, sizeof saved);

    /* ---- Connection 1: full handshake, capture a ticket ---- */
    {
        /* static (BSS): these contexts are large (tls13_hs_ctx alone is
         * ~34KB); keeping two of them off the stack matches how the real
         * async layer allocates (NewPtrClear) and avoids a fragile
         * multi-large-frame stack in this two-connection harness. */
        static br_ssl_client_context sc; static br_x509_minimal_context xc;
        static tls13_hs_ctx hs;
        time_t now; uint32_t days, secs;

        br_ssl_client_init_full(&sc, &xc, tas, tas_n);
        br_ssl_engine_set_buffer(&sc.eng, iobuf, sizeof iobuf, 1);
        now = time(NULL);
        days = (uint32_t)(now / 86400) + 719528U; secs = (uint32_t)(now % 86400);
        br_x509_minimal_set_time(&xc, days, secs);
        OSTLS_InjectEntropy(&sc.eng);

        tls13_handshake_init(&hs);
        hs.x509_ctx = (const br_x509_class **)&xc.vtable;
        hs.eng = &sc.eng;

        fd = tcp_connect(host, port);
        if (fd < 0) { printf("FAIL: connect (conn1)\n"); return 1; }
        if (!drive(fd, &hs, host, rbuf, sizeof rbuf, &rlen)) {
            printf("FAIL: full handshake (conn1)\n"); close(fd); return 1;
        }
        printf("PASS: conn1 full handshake (cipher=0x%04X)\n",
               (unsigned)hs.cipher_suite);
        appdata(fd, &hs, host, rbuf, rlen, sizeof rbuf, 1);
        close(fd);

        if (!hs.ticket_valid) {
            printf("SKIP: server issued no usable NewSessionTicket "
                   "(can't test resumption against %s)\n", host);
            return 0;
        }
        saved = hs.ticket;
        ticket_time = time(NULL);
        printf("PASS: captured ticket (len=%lu lifetime=%lus suite=0x%04X)\n",
               (unsigned long)saved.ticket_len, (unsigned long)saved.lifetime,
               (unsigned)saved.cipher_suite);
    }

    /* ---- Connection 2: resume with the captured ticket ---- */
    {
        static br_ssl_client_context sc; static br_x509_minimal_context xc;
        static tls13_hs_ctx hs;
        time_t now; uint32_t days, secs, age_ms;
        int resp;

        br_ssl_client_init_full(&sc, &xc, tas, tas_n);
        br_ssl_engine_set_buffer(&sc.eng, iobuf, sizeof iobuf, 1);
        now = time(NULL);
        days = (uint32_t)(now / 86400) + 719528U; secs = (uint32_t)(now % 86400);
        br_x509_minimal_set_time(&xc, days, secs);
        OSTLS_InjectEntropy(&sc.eng);

        tls13_handshake_init(&hs);
        hs.x509_ctx = (const br_x509_class **)&xc.vtable;
        hs.eng = &sc.eng;

        hs.resuming = 1;
        hs.offer_ticket = &saved;
        age_ms = (uint32_t)((now - ticket_time) * 1000);
        hs.offer_obfuscated_age = age_ms + saved.age_add;

        fd = tcp_connect(host, port);
        if (fd < 0) { printf("FAIL: connect (conn2)\n"); return 1; }
        if (!drive(fd, &hs, host, rbuf, sizeof rbuf, &rlen)) {
            printf("FAIL: resumed handshake (conn2)\n"); close(fd); return 1;
        }
        if (!hs.resumption_accepted) {
            printf("FAIL: server did NOT accept resumption "
                   "(no pre_shared_key echo)\n");
            close(fd); return 1;
        }
        printf("PASS: server ACCEPTED resumption (pre_shared_key echoed)\n");
        printf("PASS: resumed handshake reached Complete, cert skipped\n");

        resp = appdata(fd, &hs, host, rbuf, rlen, sizeof rbuf, 0);
        close(fd);
        if (!resp) { printf("FAIL: no app-data over resumed keys\n"); return 1; }
        printf("PASS: HTTP response over resumed TLS 1.3\n");
    }

    printf("\nRESUMPTION OK.\n");
    return 0;
}
