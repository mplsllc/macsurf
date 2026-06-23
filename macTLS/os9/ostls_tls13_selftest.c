/*
 * ostls_tls13_selftest.c -- on-device TLS 1.3 crypto self-test.
 *
 * Mirrors the host tests (tests/host/test_tls13_keysched.c and
 * test_tls13_record.c) but logs through the macTLS channel so it can run
 * inside MacTLSTest on real hardware. See the header for rationale.
 */

#include "ostls_tls13_selftest.h"
#include "ostls_tls13_keysched.h"
#include "ostls_tls13_record.h"
#include "ostls_log.h"
#include "bearssl_hash.h"
#include <string.h>

static int g_tls13_fail;

static void t_chk(const char *name, int ok)
{
    if (ok) {
        OSTLS_LogLinef("  TLS13 PASS: %s", name);
    } else {
        OSTLS_LogLinef("  TLS13 FAIL: %s", name);
        g_tls13_fail++;
    }
}

static int hexnib(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

static void hexbytes(const char *hex, unsigned char *out, int len)
{
    int i;
    for (i = 0; i < len; i++) {
        out[i] = (unsigned char)((hexnib(hex[2 * i]) << 4) |
                                  hexnib(hex[2 * i + 1]));
    }
}

static void test_keysched(void)
{
    tls13_keysched ks;
    unsigned char exp[32];
    unsigned char shared[32];
    unsigned char th[32];
    unsigned char ck[32], civ[12], sk[32], siv[12];

    tls13_ks_init(&ks, &br_sha256_vtable);
    hexbytes("e3b0c44298fc1c149afbf4c8996fb924"
             "27ae41e4649b934ca495991b7852b855", exp, 32);
    t_chk("ks SHA-256 empty hash", memcmp(exp, ks.empty_hash, 32) == 0);

    tls13_ks_extract_early(&ks);
    hexbytes("33ad0a1c607ec03b09e6cd9893680ce2"
             "10adf300aa1f2660e1b22e10f170f92a", exp, 32);
    t_chk("ks early secret (no PSK)", memcmp(exp, ks.secret, 32) == 0);

    hexbytes("8bd4054fb55b9d63fdfbacf9f04b9f0d"
             "35e6d63f537563efd46272900f89492d", shared, 32);
    tls13_ks_extract_handshake(&ks, shared, 32);
    hexbytes("1dc826e93606aa6fdc0aadc12f741b01"
             "046aa6b99f691ed221a9f0ca043fbeac", exp, 32);
    t_chk("ks handshake secret", memcmp(exp, ks.secret, 32) == 0);

    hexbytes("860c06edc07858ee8e78f0e7428c58ed"
             "d6b43f2ca3e6e95f02ed063cf0e1cad8", th, 32);
    tls13_ks_derive_handshake_keys(&ks, th, 16, ck, civ, sk, siv, 0, 0);
    hexbytes("3fce516009c21727d0f2e4e86ee403bc", exp, 16);
    t_chk("ks server hs key", memcmp(exp, sk, 16) == 0);
    hexbytes("5d313eb2671276ee13000b30", exp, 12);
    t_chk("ks server hs iv", memcmp(exp, siv, 12) == 0);
}

static void test_record_cipher(const char *label, uint16_t suite,
                               size_t key_len)
{
    unsigned char key[32], iv[12];
    unsigned char ct[128], pt[128], bad[128];
    tls13_record_ctx enc, dec;
    const char *msg = "macTLS 1.3 record layer";
    size_t mlen = strlen(msg);
    size_t clen = 0, olen = 0;
    unsigned char octype = 0;
    int i, r, ok;

    for (i = 0; i < 32; i++) key[i] = (unsigned char)(i + 3);
    for (i = 0; i < 12; i++) iv[i] = (unsigned char)(0x40 + i);

    /* round-trip */
    tls13_record_init(&enc, key, key_len, iv, suite);
    tls13_record_init(&dec, key, key_len, iv, suite);
    tls13_record_encrypt(&enc, msg, mlen, TLS13_CT_HANDSHAKE, ct, &clen);
    r = tls13_record_decrypt(&dec, ct, clen, pt, &olen, &octype);
    ok = (r == 0 && olen == mlen && octype == TLS13_CT_HANDSHAKE &&
          memcmp(pt, msg, mlen) == 0);
    OSTLS_LogLinef("  TLS13 %s: roundtrip %s", label, ok ? "PASS" : "FAIL");
    if (!ok) g_tls13_fail++;

    /* tampered payload must be rejected */
    memcpy(bad, ct, clen);
    bad[0] = (unsigned char)(bad[0] ^ 0x01);
    tls13_record_init(&dec, key, key_len, iv, suite);
    r = tls13_record_decrypt(&dec, bad, clen, pt, &olen, &octype);
    OSTLS_LogLinef("  TLS13 %s: tamper-reject %s", label,
                   (r == -1) ? "PASS" : "FAIL");
    if (r != -1) g_tls13_fail++;
}

int OSTLS_TLS13_SelfTest(void)
{
    g_tls13_fail = 0;
    test_keysched();
    test_record_cipher("AES-128-GCM", 0x1301, 16);
    test_record_cipher("ChaCha20-Poly1305", 0x1303, 32);
    return g_tls13_fail;
}
