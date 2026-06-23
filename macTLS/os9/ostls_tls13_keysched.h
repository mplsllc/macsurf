/*
 * ostls_tls13_keysched.h -- TLS 1.3 key schedule (RFC 8446 Section 7.1)
 *
 * Derives the TLS 1.3 handshake and application traffic keys from the
 * X25519 shared secret, using BearSSL's HMAC/HKDF underneath.
 *
 * Adapted for macTLS (CodeWarrior 8, C89) from Certainly by minorbug
 * (https://github.com/minorbug/certainly), MIT licensed. The logic is
 * unchanged; this is a C89 + macTLS-include port. See TLS13_SCOPE.md.
 */

#ifndef OSTLS_TLS13_KEYSCHED_H
#define OSTLS_TLS13_KEYSCHED_H

#include "bearssl_hash.h"
#include <stddef.h>

/*
 * Key schedule context. Tracks the current secret as it progresses
 * through Early Secret -> Handshake Secret -> Master Secret.
 */
typedef struct {
    const br_hash_class *hash;          /* SHA-256 or SHA-384 */
    size_t              hash_len;       /* 32 or 48 */
    unsigned char       secret[64];     /* current secret (max hash len) */
    unsigned char       empty_hash[64]; /* precomputed Hash("") */
} tls13_keysched;

/* Initialize the key schedule for a given hash function. Precomputes
 * Hash("") which is used in the Derive-Secret("derived","") steps
 * between each Extract. */
void tls13_ks_init(tls13_keysched *ks, const br_hash_class *hash);

/* Step 1: Extract Early Secret from PSK (or zeros if no PSK). This
 * implementation always uses zeros (no PSK support). */
void tls13_ks_extract_early(tls13_keysched *ks);

/* Step 2: Derive-Secret(early, "derived", "") then Extract Handshake
 * Secret from the DH shared secret. Call after the X25519 exchange. */
void tls13_ks_extract_handshake(tls13_keysched *ks,
                                const void *shared_secret, size_t len);

/* Derive client and server handshake traffic keys + IVs.
 * transcript_hash is Hash(ClientHello || ServerHello). key_len is the
 * AEAD key size (16 for AES-128, 32 for AES-256/ChaCha20). If the
 * *_secret_out pointers are non-NULL, the raw traffic secrets are copied
 * there (needed later for Finished key derivation). */
void tls13_ks_derive_handshake_keys(tls13_keysched *ks,
                                    const void *transcript_hash,
                                    size_t key_len,
                                    void *client_key, void *client_iv,
                                    void *server_key, void *server_iv,
                                    void *client_secret_out,
                                    void *server_secret_out);

/* Step 3: Derive-Secret(handshake, "derived", "") then Extract Master
 * Secret (using zeros as input key material). */
void tls13_ks_extract_master(tls13_keysched *ks);

/* Derive client and server application traffic keys + IVs.
 * transcript_hash is Hash(ClientHello || ... || server Finished). */
void tls13_ks_derive_app_keys(tls13_keysched *ks,
                              const void *transcript_hash,
                              size_t key_len,
                              void *client_key, void *client_iv,
                              void *server_key, void *server_iv);

/* Derive the Finished verification key from a traffic secret. Used to
 * compute/verify the Finished HMAC. */
void tls13_ks_derive_finished_key(tls13_keysched *ks,
                                  const void *base_key,
                                  void *finished_key);

/* Session resumption (macTLS#2 Stage A).
 * resumption_master_secret = Derive-Secret(Master, "res master",
 *     Hash(ClientHello..client Finished)). Call while ks->secret still
 * holds the Master Secret, with a transcript snapshot taken AFTER the
 * client's own Finished. Output is hash_len bytes. */
void tls13_ks_derive_resumption_master(tls13_keysched *ks,
                                       const void *transcript_hash,
                                       void *res_master_out);

/* Per-ticket resumption PSK = HKDF-Expand-Label(res_master, "resumption",
 * ticket_nonce, Hash.length). Computed once per NewSessionTicket; feeds
 * HKDF-Extract as the Early Secret IKM on the resumed connection. */
void tls13_ks_derive_resumption_psk(tls13_keysched *ks,
                                     const void *res_master,
                                     const void *ticket_nonce,
                                     size_t nonce_len,
                                     void *psk_out);

/* Early Secret from a resumption PSK: HKDF-Extract(salt=0, IKM=PSK).
 * Leaves the Early Secret in ks->secret (macTLS#2 Stage C). */
void tls13_ks_extract_early_psk(tls13_keysched *ks,
                                const void *psk, size_t psk_len);

/* Binder key = Derive-Secret(Early Secret, "res binder", ""). Call while
 * ks->secret holds the Early Secret (right after extract_early_psk). The
 * PSK binder is HMAC(finished_key, transcript), where finished_key =
 * tls13_ks_derive_finished_key(binder_key). macTLS#2 Stage C. */
void tls13_ks_derive_binder_key(tls13_keysched *ks, void *binder_key_out);

#endif /* OSTLS_TLS13_KEYSCHED_H */
