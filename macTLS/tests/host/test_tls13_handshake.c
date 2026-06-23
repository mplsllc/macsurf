/*
 * test_tls13_handshake.c -- drive a real TLS 1.3 handshake on the host.
 *
 * Host build only (native cc + sockets). This is the Stage C correctness
 * gate: the handshake state machine is transport-agnostic (it consumes
 * records from a caller buffer), so we can exercise the whole flow
 * against a live 1.3 server over a plain Linux socket, no Mac required.
 *
 *   ./test_tls13_handshake [host] [port]      (default mactrove.com 443)
 *
 * Setup mirrors how the eventual macTLS integration wires it: a BearSSL
 * client context for the engine PRNG + X.509 minimal validator with our
 * 121 anchors, then tls13_handshake_init and point hs.eng / hs.x509_ctx
 * at them. We deliberately do NOT call br_ssl_client_reset (the 1.3 path
 * doesn't use the engine's record I/O), same as the design.
 *
 * Success = the state machine reaches kTLS13_Complete with is_tls13 set,
 * which means ServerHello parsed, X25519 done, keys derived, the server
 * Certificate validated against our anchors, CertificateVerify checked,
 * server Finished verified, and our Finished sent.
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
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

static const char *result_name(tls13_hs_result r)
{
    switch (r) {
    case kTLS13_OK:         return "OK";
    case kTLS13_WantRead:   return "WantRead";
    case kTLS13_WantWrite:  return "WantWrite";
    case kTLS13_Fallback12: return "Fallback12";
    case kTLS13_Error:      return "Error";
    }
    return "?";
}

/*
 * After the handshake, send an HTTP GET over the 1.3 record layer and
 * read the response. The server sends NewSessionTicket(s) first (inner
 * content type handshake); we must decrypt every record in order to keep
 * read_ctx's sequence number aligned, ignore the tickets, and capture the
 * first application_data record (the HTTP response). Returns 0 on getting
 * a response. rbuf holds rlen leftover bytes from the handshake.
 */
static int do_appdata(int fd, tls13_hs_ctx *hs, const char *host,
                      unsigned char *rbuf, size_t rlen, size_t cap)
{
    char get[256];
    unsigned char ct[512];
    unsigned char wire[600];
    static unsigned char plain[16384];
    size_t getlen, clen = 0, plen = 0;
    uint8_t inner = 0;
    int glen;
    time_t deadline;

    glen = snprintf(get, sizeof get,
        "GET / HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", host);
    getlen = (size_t)glen;

    if (tls13_record_encrypt(&hs->write_ctx, get, getlen,
            TLS13_CT_APPLICATION_DATA, ct, &clen) != 0) {
        printf("FAIL: app-data encrypt\n");
        return 1;
    }
    wire[0] = 0x17; wire[1] = 0x03; wire[2] = 0x03;
    wire[3] = (unsigned char)(clen >> 8);
    wire[4] = (unsigned char)clen;
    memcpy(wire + 5, ct, clen);
    if (send(fd, wire, 5 + clen, 0) <= 0) {
        printf("FAIL: app-data send\n");
        return 1;
    }

    deadline = time(NULL) + 15;
    for (;;) {
        while (rlen >= 5) {
            size_t reclen = ((size_t)rbuf[3] << 8) | (size_t)rbuf[4];
            int dr;
            if (rlen < 5 + reclen) break;
            dr = tls13_record_decrypt(&hs->read_ctx, rbuf + 5, reclen,
                                      plain, &plen, &inner);
            memmove(rbuf, rbuf + 5 + reclen, rlen - (5 + reclen));
            rlen -= (5 + reclen);
            if (dr != 0) {
                printf("FAIL: app-data record decrypt failed\n");
                return 1;
            }
            printf("  post-hs rec: inner=%d plen=%lu\n",
                   (int)inner, (unsigned long)plen);
            if (inner == TLS13_CT_APPLICATION_DATA) {
                size_t i, n = plen < 60 ? plen : 60;
                printf("PASS: HTTP response over TLS 1.3 (%lu bytes): ",
                       (unsigned long)plen);
                for (i = 0; i < n; i++) {
                    char c = (char)plain[i];
                    putchar((c == '\r' || c == '\n') ? ' ' : c);
                }
                printf("\n");
                return 0;
            } else if (inner == TLS13_CT_ALERT) {
                printf("FAIL: server alert level=%d desc=%d (plen=%lu)\n",
                       plen > 0 ? (int)plain[0] : -1,
                       plen > 1 ? (int)plain[1] : -1,
                       (unsigned long)plen);
                return 1;
            }
            /* else handshake (NewSessionTicket / KeyUpdate): ignore. */
        }
        if (time(NULL) > deadline) {
            printf("FAIL: app-data timeout (no response record)\n");
            return 1;
        }
        {
            ssize_t n = recv(fd, rbuf + rlen, cap - rlen, 0);
            if (n <= 0) {
                printf("FAIL: app-data recv (n=%ld)\n", (long)n);
                return 1;
            }
            rlen += (size_t)n;
        }
    }
}

int main(int argc, char **argv)
{
    const char *host = (argc > 1) ? argv[1] : "mactrove.com";
    const char *port = (argc > 2) ? argv[2] : "443";

    br_ssl_client_context sc;
    br_x509_minimal_context xc;
    const br_x509_trust_anchor *tas;
    size_t tas_n;
    static unsigned char iobuf[BR_SSL_BUFSIZE_BIDI];
    tls13_hs_ctx hs;

    int fd;
    unsigned char recv_buf[32768];
    size_t recv_len = 0;
    int done = 0, ok = 0;
    uint32_t days, secs;
    time_t now;
    struct timeval tv;
    int steps = 0;

    printf("=== TLS 1.3 handshake against %s:%s ===\n\n", host, port);

    /* --- BearSSL setup (engine PRNG + X.509 validator with our anchors) --- */
    OSTLS_B3_GetAnchors(&tas, &tas_n);
    br_ssl_client_init_full(&sc, &xc, tas, tas_n);
    br_ssl_engine_set_buffer(&sc.eng, iobuf, sizeof iobuf, 1);

    /* X.509 needs the wall clock to check cert validity. BearSSL time is
     * (days since 0 AD, seconds in day); Unix day 0 = BearSSL day 719528. */
    now = time(NULL);
    days = (uint32_t)(now / 86400) + 719528U;
    secs = (uint32_t)(now % 86400);
    br_x509_minimal_set_time(&xc, days, secs);

    /* Seed the engine PRNG via macEntropy (the handshake draws its random
     * from eng->rng). On host this is weak entropy, fine for a functional
     * test. */
    OSTLS_InjectEntropy(&sc.eng);

    tls13_handshake_init(&hs);
    hs.x509_ctx = (const br_x509_class **)&xc.vtable;
    hs.eng = &sc.eng;

    /* --- connect --- */
    fd = tcp_connect(host, port);
    if (fd < 0) {
        printf("FAIL: cannot connect to %s:%s\n", host, port);
        return 1;
    }
    tv.tv_sec = 10; tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    /* --- drive the handshake --- */
    signal(SIGPIPE, SIG_IGN);   /* report send errors instead of dying */
    while (!done && steps < 400) {
        tls13_hs_result r = tls13_handshake_step(&hs, recv_buf, &recv_len, host);
        steps++;
        fprintf(stderr, "  step %d: state=%d r=%d msglen=%lu recvlen=%lu group=0x%04x hrr=%d\n",
                steps, (int)hs.state, (int)r,
                (unsigned long)hs.msg_len, (unsigned long)recv_len,
                (unsigned)hs.ecdhe_group, (int)hs.hrr_received);

        /* flush any outgoing message fully before advancing */
        while (hs.msg_offset < hs.msg_len) {
            ssize_t n = send(fd, hs.msg_buf + hs.msg_offset,
                             hs.msg_len - hs.msg_offset, 0);
            if (n <= 0) { printf("FAIL: send error\n"); done = 1; break; }
            hs.msg_offset += (size_t)n;
        }
        if (done) break;

        /* Completion is signaled by STATE (kTLS13_Complete is a state,
         * not a result; the result enum has no Complete). Once we reach
         * it and our Finished is fully flushed, the handshake is done. */
        if (hs.state == kTLS13_Complete && hs.msg_offset >= hs.msg_len) {
            ok = 1; done = 1; break;
        }

        switch (r) {
        case kTLS13_Error:
            printf("FAIL: handshake Error (br_err=%d, state=%d, cipher=0x%04X, is_tls13=%d)\n",
                   hs.error, (int)hs.state, (unsigned)hs.cipher_suite, hs.is_tls13);
            done = 1;
            break;
        case kTLS13_Fallback12:
            printf("NOTE: server selected TLS 1.2 (fallback path)\n");
            done = 1;
            break;
        case kTLS13_WantRead: {
            ssize_t n = recv(fd, recv_buf + recv_len,
                             sizeof(recv_buf) - recv_len, 0);
            if (n <= 0) {
                printf("FAIL: recv error/timeout (n=%ld, state=%d)\n",
                       (long)n, (int)hs.state);
                done = 1;
            } else {
                recv_len += (size_t)n;
            }
            break;
        }
        case kTLS13_WantWrite:
        case kTLS13_OK:
            break;
        }
    }

    if (ok) {
        int ad;
        printf("PASS: TLS 1.3 handshake complete in %d steps\n", steps);
        printf("  is_tls13=%d  cipher_suite=0x%04X\n",
               hs.is_tls13, (unsigned)hs.cipher_suite);
        ad = do_appdata(fd, &hs, host, recv_buf, recv_len, sizeof recv_buf);
        close(fd);
        return (hs.is_tls13 && hs.state == kTLS13_Complete && ad == 0) ? 0 : 1;
    }
    close(fd);
    printf("RESULT: handshake did not complete (last result above)\n");
    return 1;
}
