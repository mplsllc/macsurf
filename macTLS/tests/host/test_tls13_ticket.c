/*
 * test_tls13_ticket.c -- verify NewSessionTicket parsing + PSK minting
 * (macTLS#2 Stage B). Host build only (native cc, not CodeWarrior).
 *
 * Builds a synthetic NewSessionTicket message by hand, runs it through
 * tls13_parse_new_session_ticket with a fixed res_master, and checks
 * that every field is extracted correctly and the resumption PSK matches
 * an independent HKDF-Expand-Label reference. Also exercises the
 * malformed-message rejection paths.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../../os9/ostls_tls13_handshake.h"
#include "../../os9/ostls_tls13_keysched.h"

static int failures = 0;

static void check(const char *name, int ok)
{
    printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) failures++;
}

static void assert_bytes(const char *name, const void *expected,
                         const void *actual, size_t len)
{
    if (memcmp(expected, actual, len) != 0) {
        size_t i;
        printf("FAIL: %s\n  expected: ", name);
        for (i = 0; i < len; i++) printf("%02x", ((unsigned char*)expected)[i]);
        printf("\n  actual:   ");
        for (i = 0; i < len; i++) printf("%02x", ((unsigned char*)actual)[i]);
        printf("\n");
        failures++;
    } else {
        printf("PASS: %s\n", name);
    }
}

static void hex_to_bytes(const char *hex, unsigned char *out, size_t len)
{
    size_t i;
    for (i = 0; i < len; i++) {
        unsigned int b;
        sscanf(hex + 2 * i, "%02x", &b);
        out[i] = (unsigned char)b;
    }
}

/* A hand-built NewSessionTicket (header included):
 *   type=4, u24 len=20
 *   lifetime   = 0x00015180 (86400)
 *   age_add    = 0xAABBCCDD
 *   nonce      = {00 00}      (len 2)
 *   ticket     = {11 22 33 44 55} (len 5)
 *   extensions = {}           (len 0) */
static const unsigned char NST[] = {
    0x04, 0x00, 0x00, 0x14,                 /* type + u24 len = 20 */
    0x00, 0x01, 0x51, 0x80,                 /* ticket_lifetime */
    0xAA, 0xBB, 0xCC, 0xDD,                 /* ticket_age_add */
    0x02, 0x00, 0x00,                       /* nonce<2> = 00 00 */
    0x00, 0x05, 0x11, 0x22, 0x33, 0x44, 0x55, /* ticket<5> */
    0x00, 0x00                              /* extensions<0> */
};

static void setup_hs(tls13_hs_ctx *hs)
{
    memset(hs, 0, sizeof *hs);
    tls13_ks_init(&hs->ks, &br_sha256_vtable);
    hs->cipher_suite = 0x1301;              /* AES_128_GCM_SHA256 */
    memset(hs->res_master, 0x03, 32);
    hs->res_master_valid = 1;
}

static void test_parse_ok(void)
{
    tls13_hs_ctx hs;
    tls13_session_ticket t;
    unsigned char expected_ticket[5];
    unsigned char expected_psk[32];

    setup_hs(&hs);
    memset(&t, 0, sizeof t);

    check("parse returns 0", tls13_parse_new_session_ticket(
              &hs, NST, sizeof NST, &t) == 0);
    check("valid flag set", t.valid == 1);
    check("lifetime = 86400", t.lifetime == 86400UL);
    check("age_add = 0xAABBCCDD", t.age_add == 0xAABBCCDDUL);
    check("ticket_len = 5", t.ticket_len == 5);
    check("cipher_suite = 0x1301", t.cipher_suite == 0x1301);
    check("psk_len = 32", t.psk_len == 32);

    hex_to_bytes("1122334455", expected_ticket, 5);
    assert_bytes("ticket bytes", expected_ticket, t.ticket, 5);

    hex_to_bytes(
        "b89f1d81404530f7c18af2257a6ffb96"
        "5912ba29aaa5bc896e6aa8c98c5842c3",
        expected_psk, 32);
    assert_bytes("minted resumption PSK", expected_psk, t.psk, 32);
}

static void test_rejects(void)
{
    tls13_hs_ctx hs;
    tls13_session_ticket t;
    unsigned char bad[sizeof NST];

    /* no res_master -> refuse */
    setup_hs(&hs);
    hs.res_master_valid = 0;
    check("reject when no res_master",
          tls13_parse_new_session_ticket(&hs, NST, sizeof NST, &t) == -1);

    /* truncated message (declared body longer than buffer) */
    setup_hs(&hs);
    check("reject truncated",
          tls13_parse_new_session_ticket(&hs, NST, 10, &t) == -1);

    /* wrong handshake type */
    setup_hs(&hs);
    memcpy(bad, NST, sizeof NST);
    bad[0] = 0x08;                          /* EncryptedExtensions, not NST */
    check("reject wrong type",
          tls13_parse_new_session_ticket(&hs, bad, sizeof bad, &t) == -1);
}

/* PSK binder computation (macTLS#2 Stage C2). Drives tls13_compute_binder
 * with an empty base transcript and a fixed 50-byte "truncated ClientHello"
 * so binder = HMAC(finished_key, SHA-256(truncated)); pinned against an
 * independent HKDF reference (psk = 0x04*32). */
static void test_binder(void)
{
    tls13_keysched ks;
    tls13_transcript base;
    unsigned char psk[32];
    unsigned char trunc[50];
    unsigned char binder[32];
    unsigned char expected[32];
    int i;

    memset(psk, 0x04, sizeof psk);
    for (i = 0; i < 50; i++) trunc[i] = (unsigned char)(i + 1);

    tls13_ks_init(&ks, &br_sha256_vtable);
    tls13_transcript_init(&base, &br_sha256_vtable);   /* empty base */

    tls13_compute_binder(&ks, psk, 32, &base, trunc, sizeof trunc, binder);

    hex_to_bytes(
        "3e85d371f1b161430a3ea4b9d60ed6e2"
        "20a4a3963d00b050c92dd56277e16ff1",
        expected, 32);
    assert_bytes("PSK binder", expected, binder, 32);
}

int main(void)
{
    printf("=== TLS 1.3 NewSessionTicket Parse Tests (macTLS) ===\n\n");
    test_parse_ok();
    test_rejects();
    test_binder();
    printf("\n%d test(s) failed.\n", failures);
    return failures > 0 ? 1 : 0;
}
