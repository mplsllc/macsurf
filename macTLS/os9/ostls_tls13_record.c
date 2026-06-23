/*
 * ostls_tls13_record.c -- TLS 1.3 record encryption/decryption
 *
 * Per-record nonce = base_IV XOR sequence_number, where the 64-bit
 * sequence number is right-aligned in the 12-byte nonce. AAD is the
 * 5-byte outer header: content_type (0x17) + legacy version (0x0303) +
 * ciphertext length (which includes the inner content-type byte and the
 * 16-byte AEAD tag).
 *
 * Adapted for macTLS (CW8, C89) from Certainly by minorbug
 * (https://github.com/minorbug/certainly), MIT licensed.
 *
 * Difference from the original: the ChaCha20-Poly1305 decrypt path here
 * actually checks the authentication tag. br_poly1305_ctmul_run computes
 * the tag but does not verify it (per BearSSL's contract), so we compare
 * it ourselves in constant time. The original set ok=1 unconditionally,
 * which accepted forged records.
 */

#include "ostls_tls13_record.h"
#include "bearssl_block.h"
#include "bearssl_aead.h"
#include "bearssl_hash.h"
#include <string.h>

#define TLS_AES_128_GCM_SHA256       0x1301
#define TLS_AES_256_GCM_SHA384       0x1302
#define TLS_CHACHA20_POLY1305_SHA256 0x1303

/*
 * Per-record nonce: copy the base IV, then XOR the 64-bit sequence
 * number into the low 8 bytes (the high 4 bytes of the IV are left
 * alone, i.e. XORed with zero).
 *
 * NOTE (CW8 watch item for Stage E): seq is uint64_t and this uses
 * 64-bit right shifts. CW8 PPC has a known long-long codegen bug on
 * multiply-by-constant; shifts are a different path and expected to be
 * fine, but verify the nonce on hardware. In practice seq stays small
 * (you won't send 2^32 records on one connection).
 */
static void compute_nonce(const unsigned char *iv, uint64_t seq,
                          unsigned char *nonce)
{
    memcpy(nonce, iv, 12);
    nonce[4]  ^= (unsigned char)(seq >> 56);
    nonce[5]  ^= (unsigned char)(seq >> 48);
    nonce[6]  ^= (unsigned char)(seq >> 40);
    nonce[7]  ^= (unsigned char)(seq >> 32);
    nonce[8]  ^= (unsigned char)(seq >> 24);
    nonce[9]  ^= (unsigned char)(seq >> 16);
    nonce[10] ^= (unsigned char)(seq >> 8);
    nonce[11] ^= (unsigned char)(seq);
}

/* AAD = content_type(0x17) || legacy_version(0x0303) || ciphertext_len */
static void build_aad(size_t ciphertext_len, unsigned char *aad)
{
    aad[0] = 0x17;  /* application_data */
    aad[1] = 0x03;  /* TLS 1.2 legacy version */
    aad[2] = 0x03;
    aad[3] = (unsigned char)(ciphertext_len >> 8);
    aad[4] = (unsigned char)(ciphertext_len);
}

/* Constant-time 16-byte tag compare. Returns 1 if equal, 0 otherwise. */
static int tag_equal(const unsigned char *a, const unsigned char *b)
{
    unsigned char diff = 0;
    int i;
    for (i = 0; i < TLS13_TAG_SIZE; i++) {
        diff = (unsigned char)(diff | (unsigned char)(a[i] ^ b[i]));
    }
    return diff == 0;
}

void tls13_record_init(tls13_record_ctx *ctx,
                       const void *key, size_t key_len,
                       const void *iv,
                       uint16_t cipher_suite)
{
    memcpy(ctx->key, key, key_len);
    ctx->key_len = key_len;
    memcpy(ctx->iv, iv, 12);
    ctx->seq = 0;
    ctx->cipher_suite = cipher_suite;
}

int tls13_record_encrypt(tls13_record_ctx *ctx,
                         const void *plaintext, size_t pt_len,
                         uint8_t ct,
                         void *out, size_t *out_len)
{
    unsigned char nonce[12];
    unsigned char aad[5];
    size_t total_ct_len;
    unsigned char *out_buf = (unsigned char *)out;

    total_ct_len = pt_len + 1 + TLS13_TAG_SIZE;

    compute_nonce(ctx->iv, ctx->seq, nonce);
    build_aad(total_ct_len, aad);

    /* Copy plaintext and append the inner content-type byte. */
    memcpy(out_buf, plaintext, pt_len);
    out_buf[pt_len] = ct;

    if (ctx->cipher_suite == TLS_CHACHA20_POLY1305_SHA256) {
        br_poly1305_ctmul_run(ctx->key, nonce,
                              out_buf, pt_len + 1,
                              aad, sizeof(aad),
                              out_buf + pt_len + 1, /* tag output */
                              br_chacha20_ct_run,
                              1 /* encrypt */);
    } else {
        br_aes_ct_ctr_keys aes_ctx;
        br_gcm_context gcm;

        br_aes_ct_ctr_init(&aes_ctx, ctx->key, ctx->key_len);
        br_gcm_init(&gcm, &aes_ctx.vtable, br_ghash_ctmul32);
        br_gcm_reset(&gcm, nonce, 12);
        br_gcm_aad_inject(&gcm, aad, sizeof(aad));
        br_gcm_flip(&gcm);
        br_gcm_run(&gcm, 1 /* encrypt */, out_buf, pt_len + 1);
        br_gcm_get_tag(&gcm, out_buf + pt_len + 1);
    }

    *out_len = total_ct_len;
    ctx->seq++;
    return 0;
}

int tls13_record_decrypt(tls13_record_ctx *ctx,
                         const void *ciphertext, size_t ct_len,
                         void *out, size_t *out_len,
                         uint8_t *out_ct)
{
    unsigned char nonce[12];
    unsigned char aad[5];
    unsigned char *dec_buf = (unsigned char *)out;
    size_t payload_len;
    int ok;

    if (ct_len < 1 + TLS13_TAG_SIZE) return -1;

    payload_len = ct_len - TLS13_TAG_SIZE;

    compute_nonce(ctx->iv, ctx->seq, nonce);
    build_aad(ct_len, aad);

    /* Copy the whole ciphertext (payload + received tag) for in-place
     * decryption. dec_buf[payload_len .. ct_len) holds the received tag. */
    memcpy(dec_buf, ciphertext, ct_len);

    if (ctx->cipher_suite == TLS_CHACHA20_POLY1305_SHA256) {
        unsigned char calc_tag[TLS13_TAG_SIZE];

        /* Decrypts in place and writes the COMPUTED tag to calc_tag.
         * BearSSL does not verify, so we compare it against the received
         * tag ourselves. */
        br_poly1305_ctmul_run(ctx->key, nonce,
                              dec_buf, payload_len,
                              aad, sizeof(aad),
                              calc_tag,
                              br_chacha20_ct_run,
                              0 /* decrypt */);
        ok = tag_equal(calc_tag, dec_buf + payload_len);
    } else {
        br_aes_ct_ctr_keys aes_ctx;
        br_gcm_context gcm;

        br_aes_ct_ctr_init(&aes_ctx, ctx->key, ctx->key_len);
        br_gcm_init(&gcm, &aes_ctx.vtable, br_ghash_ctmul32);
        br_gcm_reset(&gcm, nonce, 12);
        br_gcm_aad_inject(&gcm, aad, sizeof(aad));
        br_gcm_flip(&gcm);
        br_gcm_run(&gcm, 0 /* decrypt */, dec_buf, payload_len);
        ok = br_gcm_check_tag(&gcm, dec_buf + payload_len);
    }

    if (!ok) return -1;

    /* Real content type = last non-zero byte; everything after it is
     * zero padding (RFC 8446 Section 5.4). */
    {
        size_t i = payload_len;
        while (i > 0 && dec_buf[i - 1] == 0) i--;
        if (i == 0) return -1; /* all zeros -- invalid */
        *out_ct = dec_buf[i - 1];
        *out_len = i - 1;
    }

    ctx->seq++;
    return 0;
}
