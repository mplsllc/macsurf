/*
 * ostls_tls13_record.h -- TLS 1.3 record encryption/decryption
 *
 * The TLS 1.3 record format:
 *  - Outer content type is always 0x17 (application_data)
 *  - The real content type is the last non-zero byte of the decrypted
 *    payload (everything after it is zero padding)
 *  - Nonce = base IV XOR sequence number (no explicit per-record nonce)
 *  - AAD = the 5-byte outer header (type + legacy version + length)
 *
 * Adapted for macTLS (CW8, C89) from Certainly by minorbug
 * (https://github.com/minorbug/certainly), MIT licensed. The ChaCha20-
 * Poly1305 decrypt path here adds the authentication-tag check that the
 * original skipped. See TLS13_SCOPE.md.
 */

#ifndef OSTLS_TLS13_RECORD_H
#define OSTLS_TLS13_RECORD_H

#include <stdint.h>
#include <stddef.h>

/* TLS record content types */
#define TLS13_CT_CHANGE_CIPHER_SPEC  20
#define TLS13_CT_ALERT               21
#define TLS13_CT_HANDSHAKE           22
#define TLS13_CT_APPLICATION_DATA    23

/* AEAD tag size (AES-GCM and ChaCha20-Poly1305 both use 16 bytes) */
#define TLS13_TAG_SIZE  16

/*
 * Record context. One per direction (read and write), each with its own
 * key, IV, and sequence counter.
 */
typedef struct {
    unsigned char key[32];      /* encryption key (16 or 32 bytes) */
    size_t        key_len;      /* 16 for AES-128, 32 for AES-256/ChaCha20 */
    unsigned char iv[12];       /* base IV */
    uint64_t      seq;          /* record sequence number, starts at 0 */
    uint16_t      cipher_suite; /* which cipher to use */
} tls13_record_ctx;

/* Initialize a record context with key, IV, and cipher suite. */
void tls13_record_init(tls13_record_ctx *ctx,
                       const void *key, size_t key_len,
                       const void *iv,
                       uint16_t cipher_suite);

/*
 * Encrypt a record for sending.
 *   plaintext / pt_len: data to encrypt
 *   ct:      inner content type (TLS13_CT_HANDSHAKE etc.)
 *   out:     output buffer, at least pt_len + 1 + TLS13_TAG_SIZE
 *   out_len: receives the ciphertext length
 * The content type byte is appended before encryption.
 * Returns 0 on success, -1 on error.
 */
int tls13_record_encrypt(tls13_record_ctx *ctx,
                         const void *plaintext, size_t pt_len,
                         uint8_t ct,
                         void *out, size_t *out_len);

/*
 * Decrypt a received record (the payload after the 5-byte header).
 *   ciphertext / ct_len: encrypted payload incl. content-type byte + tag
 *   out:     output buffer, at least ct_len
 *   out_len: receives plaintext length (excluding the content-type byte)
 *   out_ct:  receives the real inner content type
 * Returns 0 on success, -1 on authentication or format failure.
 */
int tls13_record_decrypt(tls13_record_ctx *ctx,
                         const void *ciphertext, size_t ct_len,
                         void *out, size_t *out_len,
                         uint8_t *out_ct);

#endif /* OSTLS_TLS13_RECORD_H */
