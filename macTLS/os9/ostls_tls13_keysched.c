/*
 * ostls_tls13_keysched.c -- TLS 1.3 key schedule (RFC 8446 Section 7.1)
 *
 * The key schedule runs the DH shared secret through three stages:
 *   1. Early Secret     (from PSK, or zeros if no PSK)
 *   2. Handshake Secret (from the DH shared secret)
 *   3. Master Secret    (from zeros -- just advances the chain)
 *
 * Between each stage we do Derive-Secret(current, "derived", Hash(""))
 * then HKDF-Extract with the next input. That "derived" step with an
 * empty transcript hash is what keeps the Early, Handshake, and Master
 * secrets cryptographically independent even though they're chained.
 *
 * At each stage we derive traffic keys with Derive-Secret over the real
 * transcript hash, which binds the keys to the exact handshake messages.
 *
 * Adapted for macTLS (CW8, C89) from Certainly by minorbug
 * (https://github.com/minorbug/certainly), MIT licensed. Logic unchanged.
 */

#include "ostls_tls13_keysched.h"
#include "bearssl_hmac.h"
#include <string.h>

/*
 * HKDF-Expand-Label (RFC 8446 Section 7.1). Wraps HKDF-Expand with the
 * TLS 1.3 label structure:
 *   struct {
 *       uint16 length;
 *       opaque label<7..255>;    "tls13 " + label
 *       opaque context<0..255>;  usually a transcript hash
 *   } HkdfLabel;
 *
 * BearSSL has no standalone Expand, so we run it via HMAC directly. For
 * TLS 1.3 the output is always <= hash_len, so only T(1) is needed:
 *   T(1) = HMAC(PRK, info || 0x01)
 */
static void hkdf_expand_label(
    const br_hash_class *hash,
    const void *secret, size_t secret_len,
    const char *label, size_t label_len,
    const void *context, size_t context_len,
    void *out, size_t out_len)
{
    unsigned char info[512];
    size_t info_len;
    size_t tls_label_len;

    tls_label_len = 6 + label_len; /* "tls13 " prefix */

    info[0] = (unsigned char)(out_len >> 8);
    info[1] = (unsigned char)(out_len);
    info[2] = (unsigned char)(tls_label_len);
    memcpy(info + 3, "tls13 ", 6);
    memcpy(info + 9, label, label_len);
    info[9 + label_len] = (unsigned char)(context_len);
    if (context_len > 0) {
        memcpy(info + 10 + label_len, context, context_len);
    }
    info_len = 10 + label_len + context_len;

    {
        br_hmac_key_context kc;
        br_hmac_context mc;
        unsigned char one = 0x01;
        unsigned char tmp[64];  /* holds full HMAC output (SHA-256=32, SHA-384=48) */

        br_hmac_key_init(&kc, hash, secret, secret_len);
        br_hmac_init(&mc, &kc, 0);
        br_hmac_update(&mc, info, info_len);
        br_hmac_update(&mc, &one, 1);

        /* br_hmac_out ALWAYS writes hash_len bytes. out_len is often
         * smaller than hash_len (e.g. a 12-byte IV from a 32-byte hash),
         * so we must NOT write straight into the caller's buffer -- doing
         * so overflows it by (hash_len - out_len) bytes and corrupts
         * adjacent key material. Write the full digest into a scratch
         * buffer, then copy exactly out_len bytes. TLS 1.3 only ever
         * expands to <= hash_len (single HMAC block, no T(2)). */
        br_hmac_out(&mc, tmp);
        memcpy(out, tmp, out_len);
    }
}

/*
 * Derive-Secret(Secret, Label, Messages) =
 *     HKDF-Expand-Label(Secret, Label, Hash(Messages), Hash.length)
 * The caller passes the precomputed transcript hash as the context.
 */
static void derive_secret(
    const br_hash_class *hash, size_t hash_len,
    const void *secret,
    const char *label, size_t label_len,
    const void *transcript_hash,
    void *out)
{
    hkdf_expand_label(hash, secret, hash_len,
                      label, label_len,
                      transcript_hash, hash_len,
                      out, hash_len);
}

/*
 * HKDF-Extract(salt, IKM) = HMAC(salt, IKM), via BearSSL's HMAC. The
 * salt is the previous secret (or zeros for the first Extract); the IKM
 * is the new secret material.
 */
static void hkdf_extract(
    const br_hash_class *hash, size_t hash_len,
    const void *salt, size_t salt_len,
    const void *ikm, size_t ikm_len,
    void *out)
{
    br_hmac_key_context kc;
    br_hmac_context mc;

    (void)hash_len;

    br_hmac_key_init(&kc, hash, salt, salt_len);
    br_hmac_init(&mc, &kc, 0);
    br_hmac_update(&mc, ikm, ikm_len);
    br_hmac_out(&mc, out);
}

void tls13_ks_init(tls13_keysched *ks, const br_hash_class *hash)
{
    br_sha256_context sha256;
    br_sha384_context sha384;

    ks->hash = hash;
    ks->hash_len = (hash == &br_sha256_vtable) ? 32 : 48;

    memset(ks->secret, 0, sizeof(ks->secret));

    /* Precompute Hash("") for the Derive-Secret("derived","") steps. */
    if (hash == &br_sha256_vtable) {
        br_sha256_init(&sha256);
        br_sha256_out(&sha256, ks->empty_hash);
    } else {
        br_sha384_init(&sha384);
        br_sha384_out(&sha384, ks->empty_hash);
    }
}

void tls13_ks_extract_early(tls13_keysched *ks)
{
    unsigned char zeros[64];
    memset(zeros, 0, ks->hash_len);

    /* Early Secret = HKDF-Extract(salt=0, IKM=0) */
    hkdf_extract(ks->hash, ks->hash_len,
                 zeros, ks->hash_len,  /* salt: zeros */
                 zeros, ks->hash_len,  /* IKM: zeros (no PSK) */
                 ks->secret);
}

void tls13_ks_extract_handshake(tls13_keysched *ks,
                                const void *shared_secret, size_t len)
{
    unsigned char derived[64];

    /* Derive-Secret(Early Secret, "derived", "") */
    derive_secret(ks->hash, ks->hash_len,
                  ks->secret,
                  "derived", 7,
                  ks->empty_hash,
                  derived);

    /* Handshake Secret = HKDF-Extract(salt=derived, IKM=shared_secret) */
    hkdf_extract(ks->hash, ks->hash_len,
                 derived, ks->hash_len,
                 shared_secret, len,
                 ks->secret);

    memset(derived, 0, sizeof(derived));
}

void tls13_ks_derive_handshake_keys(tls13_keysched *ks,
                                    const void *transcript_hash,
                                    size_t key_len,
                                    void *client_key, void *client_iv,
                                    void *server_key, void *server_iv,
                                    void *client_secret_out,
                                    void *server_secret_out)
{
    unsigned char client_secret[64];
    unsigned char server_secret[64];

    derive_secret(ks->hash, ks->hash_len,
                  ks->secret,
                  "c hs traffic", 12,
                  transcript_hash,
                  client_secret);

    derive_secret(ks->hash, ks->hash_len,
                  ks->secret,
                  "s hs traffic", 12,
                  transcript_hash,
                  server_secret);

    if (client_secret_out != NULL) {
        memcpy(client_secret_out, client_secret, ks->hash_len);
    }
    if (server_secret_out != NULL) {
        memcpy(server_secret_out, server_secret, ks->hash_len);
    }

    hkdf_expand_label(ks->hash, client_secret, ks->hash_len,
                      "key", 3, NULL, 0,
                      client_key, key_len);
    hkdf_expand_label(ks->hash, client_secret, ks->hash_len,
                      "iv", 2, NULL, 0,
                      client_iv, 12);

    hkdf_expand_label(ks->hash, server_secret, ks->hash_len,
                      "key", 3, NULL, 0,
                      server_key, key_len);
    hkdf_expand_label(ks->hash, server_secret, ks->hash_len,
                      "iv", 2, NULL, 0,
                      server_iv, 12);

    memset(client_secret, 0, sizeof(client_secret));
    memset(server_secret, 0, sizeof(server_secret));
}

void tls13_ks_extract_master(tls13_keysched *ks)
{
    unsigned char derived[64];
    unsigned char zeros[64];

    memset(zeros, 0, ks->hash_len);

    /* Derive-Secret(Handshake Secret, "derived", "") */
    derive_secret(ks->hash, ks->hash_len,
                  ks->secret,
                  "derived", 7,
                  ks->empty_hash,
                  derived);

    /* Master Secret = HKDF-Extract(salt=derived, IKM=0) */
    hkdf_extract(ks->hash, ks->hash_len,
                 derived, ks->hash_len,
                 zeros, ks->hash_len,
                 ks->secret);

    memset(derived, 0, sizeof(derived));
}

void tls13_ks_derive_app_keys(tls13_keysched *ks,
                              const void *transcript_hash,
                              size_t key_len,
                              void *client_key, void *client_iv,
                              void *server_key, void *server_iv)
{
    unsigned char client_secret[64];
    unsigned char server_secret[64];

    derive_secret(ks->hash, ks->hash_len,
                  ks->secret,
                  "c ap traffic", 12,
                  transcript_hash,
                  client_secret);

    derive_secret(ks->hash, ks->hash_len,
                  ks->secret,
                  "s ap traffic", 12,
                  transcript_hash,
                  server_secret);

    hkdf_expand_label(ks->hash, client_secret, ks->hash_len,
                      "key", 3, NULL, 0,
                      client_key, key_len);
    hkdf_expand_label(ks->hash, client_secret, ks->hash_len,
                      "iv", 2, NULL, 0,
                      client_iv, 12);

    hkdf_expand_label(ks->hash, server_secret, ks->hash_len,
                      "key", 3, NULL, 0,
                      server_key, key_len);
    hkdf_expand_label(ks->hash, server_secret, ks->hash_len,
                      "iv", 2, NULL, 0,
                      server_iv, 12);

    memset(client_secret, 0, sizeof(client_secret));
    memset(server_secret, 0, sizeof(server_secret));
}

void tls13_ks_derive_finished_key(tls13_keysched *ks,
                                  const void *base_key,
                                  void *finished_key)
{
    /* finished_key = HKDF-Expand-Label(BaseKey, "finished", "", Hash.length) */
    hkdf_expand_label(ks->hash, base_key, ks->hash_len,
                      "finished", 8, NULL, 0,
                      finished_key, ks->hash_len);
}

/*
 * Session resumption (RFC 8446 Section 7.1, macTLS#2 Stage A).
 *
 * resumption_master_secret = Derive-Secret(Master Secret, "res master",
 *                                ClientHello..client Finished)
 *
 * MUST be called while ks->secret still holds the Master Secret (it does:
 * tls13_ks_derive_app_keys reads but never overwrites ks->secret). The
 * transcript_hash argument is Hash() over the full handshake THROUGH the
 * client's Finished -- note that is one message later than the app-keys
 * transcript (which stops at the server Finished), so the caller takes a
 * fresh transcript snapshot right after sending its own Finished.
 */
void tls13_ks_derive_resumption_master(tls13_keysched *ks,
                                       const void *transcript_hash,
                                       void *res_master_out)
{
    derive_secret(ks->hash, ks->hash_len,
                  ks->secret,
                  "res master", 10,
                  transcript_hash,
                  res_master_out);
}

/*
 * Per-ticket resumption PSK:
 *   PSK = HKDF-Expand-Label(resumption_master_secret, "resumption",
 *                           ticket_nonce, Hash.length)
 *
 * Each NewSessionTicket carries its own nonce, so this is computed once
 * per ticket from the single resumption_master_secret. The nonce is
 * variable length (not a hash), so we call hkdf_expand_label directly
 * rather than derive_secret. Output is hash_len bytes -- the PSK that
 * feeds HKDF-Extract as the Early Secret IKM on the resumed connection.
 */
void tls13_ks_derive_resumption_psk(tls13_keysched *ks,
                                     const void *res_master,
                                     const void *ticket_nonce,
                                     size_t nonce_len,
                                     void *psk_out)
{
    hkdf_expand_label(ks->hash, res_master, ks->hash_len,
                      "resumption", 10,
                      ticket_nonce, nonce_len,
                      psk_out, ks->hash_len);
}

/*
 * Early Secret from a resumption PSK (macTLS#2 Stage C). Same as
 * tls13_ks_extract_early but the IKM is the PSK instead of zeros:
 *   Early Secret = HKDF-Extract(salt=0, IKM=PSK)
 * Leaves the Early Secret in ks->secret so tls13_ks_derive_binder_key
 * (and, later, the resumed early/handshake chain) can build on it.
 */
void tls13_ks_extract_early_psk(tls13_keysched *ks,
                                const void *psk, size_t psk_len)
{
    unsigned char zeros[64];
    memset(zeros, 0, ks->hash_len);

    hkdf_extract(ks->hash, ks->hash_len,
                 zeros, ks->hash_len,   /* salt: zeros */
                 psk, psk_len,          /* IKM: the resumption PSK */
                 ks->secret);
}

/*
 * Binder key (RFC 8446 Section 7.1): the key that authenticates a PSK
 * identity in the pre_shared_key extension's binder. For a resumption
 * PSK the label is "res binder" (an external PSK would use "ext binder").
 *   binder_key = Derive-Secret(Early Secret, "res binder", "")
 * MUST be called while ks->secret holds the Early Secret (i.e. right
 * after tls13_ks_extract_early_psk). The actual binder is then
 * HMAC(HKDF-Expand-Label(binder_key, "finished", "", L), transcript) --
 * derive the finished key via tls13_ks_derive_finished_key(binder_key).
 */
void tls13_ks_derive_binder_key(tls13_keysched *ks, void *binder_key_out)
{
    derive_secret(ks->hash, ks->hash_len,
                  ks->secret,
                  "res binder", 10,
                  ks->empty_hash,
                  binder_key_out);
}
