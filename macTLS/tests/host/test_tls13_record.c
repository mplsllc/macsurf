/*
 * test_tls13_record.c -- TLS 1.3 record layer round-trip + authentication.
 *
 * Host build only (native cc). Round-trip proves encrypt/decrypt are
 * self-consistent; the tamper cases prove the AEAD tag is actually
 * checked. The tamper cases are the important ones: a round-trip alone
 * would pass even with no authentication, which is the bug we fixed in
 * the ChaCha20-Poly1305 decrypt path.
 */

#include <stdio.h>
#include <string.h>
#include "../../os9/ostls_tls13_record.h"

static int failures = 0;

static void check(const char *name, int cond)
{
    if (cond) {
        printf("PASS: %s\n", name);
    } else {
        printf("FAIL: %s\n", name);
        failures++;
    }
}

static void run_cipher(const char *label, uint16_t suite, size_t key_len)
{
    unsigned char key[32];
    unsigned char iv[12];
    tls13_record_ctx enc, dec;
    const char *msg = "hello tls 1.3 record layer";
    size_t pt_len = strlen(msg);
    unsigned char ct[256];
    size_t ct_len = 0;
    unsigned char pt[256];
    size_t out_len = 0;
    uint8_t out_ct = 0;
    int r;
    size_t i;
    char nm[96];

    for (i = 0; i < 32; i++) key[i] = (unsigned char)(i + 1);
    for (i = 0; i < 12; i++) iv[i] = (unsigned char)(0xA0 + i);

    /* round-trip */
    tls13_record_init(&enc, key, key_len, iv, suite);
    tls13_record_init(&dec, key, key_len, iv, suite);
    tls13_record_encrypt(&enc, msg, pt_len, TLS13_CT_HANDSHAKE, ct, &ct_len);

    sprintf(nm, "%s: ciphertext grows by 1+tag", label);
    check(nm, ct_len == pt_len + 1 + TLS13_TAG_SIZE);

    r = tls13_record_decrypt(&dec, ct, ct_len, pt, &out_len, &out_ct);
    sprintf(nm, "%s: round-trip recovers plaintext + content type", label);
    check(nm, r == 0 && out_len == pt_len &&
              out_ct == TLS13_CT_HANDSHAKE &&
              memcmp(pt, msg, pt_len) == 0);

    /* tamper a payload byte -> authentication must reject */
    {
        unsigned char bad[256];
        memcpy(bad, ct, ct_len);
        bad[0] = (unsigned char)(bad[0] ^ 0x01);
        tls13_record_init(&dec, key, key_len, iv, suite);
        r = tls13_record_decrypt(&dec, bad, ct_len, pt, &out_len, &out_ct);
        sprintf(nm, "%s: tampered payload rejected", label);
        check(nm, r == -1);
    }

    /* tamper a tag byte -> authentication must reject */
    {
        unsigned char bad[256];
        memcpy(bad, ct, ct_len);
        bad[ct_len - 1] = (unsigned char)(bad[ct_len - 1] ^ 0x01);
        tls13_record_init(&dec, key, key_len, iv, suite);
        r = tls13_record_decrypt(&dec, bad, ct_len, pt, &out_len, &out_ct);
        sprintf(nm, "%s: tampered tag rejected", label);
        check(nm, r == -1);
    }
}

int main(void)
{
    printf("=== TLS 1.3 Record Layer Tests (macTLS) ===\n\n");
    run_cipher("AES-128-GCM", 0x1301, 16);
    run_cipher("ChaCha20-Poly1305", 0x1303, 32);
    printf("\n%d test(s) failed.\n", failures);
    return failures > 0 ? 1 : 0;
}
