/*
 * test_tls13_keysched.c -- verify the TLS 1.3 key schedule against
 * RFC 8446 / RFC 8448 test vectors.
 *
 * Host build only (native cc, not CodeWarrior). The module under test
 * (os9/ostls_tls13_keysched.c) is C89 for CW8; this harness is plain
 * host C and just needs to link against BearSSL's hash + HMAC.
 *
 * Vectors adapted from Certainly's tests/host/test_keysched.c.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../../os9/ostls_tls13_keysched.h"

static int failures = 0;

static void assert_bytes(const char *name, const void *expected,
                         const void *actual, size_t len)
{
    if (memcmp(expected, actual, len) != 0) {
        size_t i;
        printf("FAIL: %s\n", name);
        printf("  expected: ");
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
        sscanf(hex + 2*i, "%02x", &b);
        out[i] = (unsigned char)b;
    }
}

/* SHA-256("") should produce the known hash. */
static void test_empty_hash(void)
{
    tls13_keysched ks;
    unsigned char expected[32];

    hex_to_bytes(
        "e3b0c44298fc1c149afbf4c8996fb924"
        "27ae41e4649b934ca495991b7852b855",
        expected, 32);

    tls13_ks_init(&ks, &br_sha256_vtable);
    assert_bytes("SHA-256 empty hash", expected, ks.empty_hash, 32);
}

/* Early Secret from zeros (no PSK):
 * early_secret = HKDF-Extract(salt=0x00*32, IKM=0x00*32). */
static void test_early_secret(void)
{
    tls13_keysched ks;
    unsigned char expected[32];

    hex_to_bytes(
        "33ad0a1c607ec03b09e6cd9893680ce2"
        "10adf300aa1f2660e1b22e10f170f92a",
        expected, 32);

    tls13_ks_init(&ks, &br_sha256_vtable);
    tls13_ks_extract_early(&ks);
    assert_bytes("Early Secret (no PSK)", expected, ks.secret, 32);
}

/* Full key schedule with the RFC 8448 example handshake trace. */
static void test_handshake_secret(void)
{
    tls13_keysched ks;
    unsigned char shared_secret[32];
    unsigned char transcript_hash[32];
    unsigned char client_key[32], client_iv[12];
    unsigned char server_key[32], server_iv[12];

    /* DH shared secret from RFC 8448 Section 3 */
    hex_to_bytes(
        "8bd4054fb55b9d63fdfbacf9f04b9f0d"
        "35e6d63f537563efd46272900f89492d",
        shared_secret, 32);

    /* Transcript hash of ClientHello + ServerHello from RFC 8448 */
    hex_to_bytes(
        "860c06edc07858ee8e78f0e7428c58ed"
        "d6b43f2ca3e6e95f02ed063cf0e1cad8",
        transcript_hash, 32);

    tls13_ks_init(&ks, &br_sha256_vtable);
    tls13_ks_extract_early(&ks);
    tls13_ks_extract_handshake(&ks, shared_secret, 32);

    {
        unsigned char expected_hs[32];
        hex_to_bytes(
            "1dc826e93606aa6fdc0aadc12f741b01"
            "046aa6b99f691ed221a9f0ca043fbeac",
            expected_hs, 32);
        assert_bytes("Handshake Secret", expected_hs, ks.secret, 32);
    }

    /* Derive handshake keys. RFC 8448 uses AES-128-GCM (16-byte key),
     * server handshake key 3fce516009c21727d0f2e4e86ee403bc. */
    tls13_ks_derive_handshake_keys(&ks, transcript_hash, 16,
                                    client_key, client_iv,
                                    server_key, server_iv,
                                    NULL, NULL);
    {
        unsigned char expected_skey[16];
        unsigned char expected_siv[12];
        hex_to_bytes("3fce516009c21727d0f2e4e86ee403bc",
                     expected_skey, 16);
        hex_to_bytes("5d313eb2671276ee13000b30", expected_siv, 12);
        assert_bytes("Server handshake key", expected_skey, server_key, 16);
        assert_bytes("Server handshake IV", expected_siv, server_iv, 12);
    }

    /* Client handshake key/iv from RFC 8448 -- this is the side we
     * encrypt the client Finished with, and never verified before. */
    {
        unsigned char expected_ckey[16];
        unsigned char expected_civ[12];
        hex_to_bytes("dbfaa693d1762c5b666af5d950258d01",
                     expected_ckey, 16);
        hex_to_bytes("5bd3c71b836e0b76bb73265f", expected_civ, 12);
        assert_bytes("Client handshake key", expected_ckey, client_key, 16);
        assert_bytes("Client handshake IV", expected_civ, client_iv, 12);
    }
}

/* Session resumption derivation (macTLS#2 Stage A).
 *
 * resumption_master_secret = Derive-Secret(Master, "res master", transcript)
 * PSK                      = HKDF-Expand-Label(res_master, "resumption",
 *                                              nonce, hash_len)
 *
 * The two new functions are thin wrappers over hkdf_expand_label /
 * derive_secret, which the tests above already prove correct against
 * RFC 8448. This case pins the label strings + lengths against an
 * independent HKDF-Expand-Label reference (synthetic inputs: master =
 * 0x01*32, transcript = 0x02*32, ticket_nonce = 0x0000). Expected values
 * computed offline with a stand-alone HMAC-SHA256 HKDF-Expand-Label. */
static void test_resumption(void)
{
    tls13_keysched ks;
    unsigned char master[32];
    unsigned char transcript[32];
    unsigned char nonce[2];
    unsigned char res_master[32];
    unsigned char psk[32];
    unsigned char expected_res_master[32];
    unsigned char expected_psk[32];

    memset(master, 0x01, sizeof master);
    memset(transcript, 0x02, sizeof transcript);
    nonce[0] = 0x00; nonce[1] = 0x00;

    hex_to_bytes(
        "588dbc357ac38bc9ca9e2453bd20586a"
        "27bddda0f77e64a320e0369b27926e5e",
        expected_res_master, 32);
    hex_to_bytes(
        "56a90b755ea996212c7e870b390a6163"
        "53d34eb6314b80e1147057b778a5c264",
        expected_psk, 32);

    tls13_ks_init(&ks, &br_sha256_vtable);
    memcpy(ks.secret, master, 32);   /* stand in for the Master Secret */

    tls13_ks_derive_resumption_master(&ks, transcript, res_master);
    assert_bytes("Resumption master secret", expected_res_master,
                 res_master, 32);

    tls13_ks_derive_resumption_psk(&ks, res_master, nonce, 2, psk);
    assert_bytes("Resumption PSK", expected_psk, psk, 32);
}

/* PSK binder key schedule (macTLS#2 Stage C):
 *   Early Secret = HKDF-Extract(0, PSK)
 *   binder_key   = Derive-Secret(Early Secret, "res binder", "")
 *   finished_key = HKDF-Expand-Label(binder_key, "finished", "", L)
 * Pinned against an independent HKDF reference (psk = 0x04*32). */
static void test_binder_key(void)
{
    tls13_keysched ks;
    unsigned char psk[32];
    unsigned char binder_key[32];
    unsigned char finished_key[32];
    unsigned char expected_early[32];
    unsigned char expected_binder_key[32];
    unsigned char expected_finished_key[32];

    memset(psk, 0x04, sizeof psk);

    hex_to_bytes(
        "25b9badd15b98488199798d0f48f1d9e"
        "f93c88def354a699c69abd20051052e6",
        expected_early, 32);
    hex_to_bytes(
        "e38cc202daa10c8e4b3260cfdd941529"
        "d6112c8760b5085259cfcc1951d0cd63",
        expected_binder_key, 32);
    hex_to_bytes(
        "2ef2ec0326ee1824ac1e20ba75eba636"
        "7c2e2d4110cf0d0e74a24099e0ca705e",
        expected_finished_key, 32);

    tls13_ks_init(&ks, &br_sha256_vtable);
    tls13_ks_extract_early_psk(&ks, psk, 32);
    assert_bytes("Early Secret (from PSK)", expected_early, ks.secret, 32);

    tls13_ks_derive_binder_key(&ks, binder_key);
    assert_bytes("Binder key", expected_binder_key, binder_key, 32);

    tls13_ks_derive_finished_key(&ks, binder_key, finished_key);
    assert_bytes("Binder finished key", expected_finished_key,
                 finished_key, 32);
}

int main(void)
{
    printf("=== TLS 1.3 Key Schedule Tests (macTLS) ===\n\n");

    test_empty_hash();
    test_early_secret();
    test_handshake_secret();
    test_resumption();
    test_binder_key();

    printf("\n%d test(s) failed.\n", failures);
    return failures > 0 ? 1 : 0;
}
