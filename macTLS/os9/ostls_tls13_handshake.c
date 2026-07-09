/*
 * tls13_handshake.c — TLS 1.3 handshake state machine
 *
 * This file implements the TLS 1.3 handshake message construction
 * and parsing, bypassing BearSSL's T0-based handshake engine entirely.
 * We use BearSSL only for crypto primitives (HKDF, X25519, AEAD)
 * and buffer I/O.
 */

#include "ostls_tls13_handshake.h"
#include "ostls_entropy.h"
#include <string.h>

/* ── Wire Format Helpers ── */

/*
 * Write a 16-bit big-endian value into a buffer.
 * TLS uses network byte order (big-endian) for all multi-byte integers.
 */
static void put_u16(unsigned char *p, uint16_t v)
{
    p[0] = (unsigned char)(v >> 8);
    p[1] = (unsigned char)(v);
}

/*
 * Read a 16-bit big-endian value from a buffer.
 */
static uint16_t get_u16(const unsigned char *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

/*
 * Read a 24-bit big-endian value from a buffer.
 */
static uint32_t get_u24(const unsigned char *p)
{
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2];
}

/*
 * Write a 24-bit big-endian value into a buffer.
 * Used for handshake message lengths (3-byte length field).
 */
static void put_u24(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v >> 16);
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v);
}

/*
 * Securely zero sensitive memory.
 * Uses volatile pointer to prevent compiler from optimizing away the wipe.
 */
static void secure_wipe(void *ptr, size_t len)
{
    volatile unsigned char *p = (volatile unsigned char *)ptr;
    size_t i;
    for (i = 0; i < len; i++) p[i] = 0;
}

/* ── Transcript Hashing ── */

void tls13_transcript_init(tls13_transcript *t, const br_hash_class *hash)
{
    t->hash = hash;
    if (hash == &br_sha256_vtable) {
        t->hash_len = 32;
        br_sha256_init(&t->sha256);
    } else {
        t->hash_len = 48;
        br_sha384_init(&t->sha384);
    }
}

void tls13_transcript_update(tls13_transcript *t,
                             const void *data, size_t len)
{
    if (t->hash == &br_sha256_vtable) {
        br_sha256_update(&t->sha256, data, len);
    } else {
        br_sha384_update(&t->sha384, data, len);
    }
}

void tls13_transcript_snapshot(const tls13_transcript *t,
                               void *out_hash)
{
    /* Copy the hash context and finalize the copy.
     * The original continues accumulating. */
    if (t->hash == &br_sha256_vtable) {
        br_sha256_context copy = t->sha256;
        br_sha256_out(&copy, out_hash);
    } else {
        br_sha384_context copy = t->sha384;
        br_sha384_out(&copy, out_hash);
    }
}

/* fixes701 (#206) — handshake failure-site markers. Written into
 * hs->fail_site at each critical failure return so a hardware log shows
 * exactly WHERE a P-384/HRR handshake dies, not just a bare br_err.
 * Surfaced via OSTLSDiagnostics.hs_fail_site -> the fetcher's TLS-FAIL log. */
#define TLS13_FAIL_HRR_GROUP_UNSUPP  1  /* HRR named a group we don't support */
#define TLS13_FAIL_HRR_DOUBLE        2  /* second HRR (RFC forbids)            */
#define TLS13_FAIL_SH_NO_KEYSHARE    3  /* ServerHello lacked a key_share      */
#define TLS13_FAIL_SH_HASH_MISMATCH  4  /* suite hash != transcript hash       */
#define TLS13_FAIL_SH_GROUP_MISMATCH 5  /* server share group != our share     */
#define TLS13_FAIL_ECDH_COMPUTE      6  /* tls13_ecdh_shared failed (curve math)*/
#define TLS13_FAIL_KEYGEN            7  /* tls13_generate_keypair failed        */
#define TLS13_FAIL_CH_BUILD          8  /* tls13_build_client_hello failed      */

void tls13_transcript_reset_for_hrr(tls13_transcript *t)
{
    /*
     * RFC 8446 Section 4.4.1: When HRR is received, the transcript
     * is replaced with a synthetic message_hash:
     *
     *   Hash(message_hash || 0x00 || uint24(Hash.length) || Hash(CH1))
     *
     * Where Hash(CH1) is the current transcript hash (of just ClientHello1).
     */
    unsigned char ch1_hash[64];
    unsigned char synthetic[4 + 64]; /* header + hash */

    tls13_transcript_snapshot(t, ch1_hash);

    synthetic[0] = TLS13_HT_MESSAGE_HASH;  /* type 254 */
    synthetic[1] = 0;
    synthetic[2] = 0;
    synthetic[3] = (unsigned char)t->hash_len;
    memcpy(synthetic + 4, ch1_hash, t->hash_len);

    /* Re-init and feed the synthetic message */
    tls13_transcript_init(t, t->hash);
    tls13_transcript_update(t, synthetic, 4 + t->hash_len);
}

void tls13_handshake_init(tls13_hs_ctx *hs)
{
    memset(hs, 0, sizeof(tls13_hs_ctx));
    hs->state = kTLS13_SendClientHello;
}

/* ── X25519 Key Generation ── */

/*
 * Generate an X25519 ephemeral key pair for the key_share extension.
 *
 * Uses BearSSL's EC implementation (br_ec_c25519_m15) which works well
 * on the 68K/PPC Mac. The private key is 32 bytes of random data
 * (with clamping applied by BearSSL internally), and the public key
 * is the X25519 public point (also 32 bytes).
 *
 * The PRNG is seeded from our entropy pool via a standalone
 * br_hmac_drbg_context — we can't use BearSSL's engine PRNG because
 * the engine hasn't started its own handshake.
 */
/* Map a TLS named-group to BearSSL's curve id (0 = unsupported). */
static int tls13_br_curve_for_group(uint16_t group)
{
    switch (group) {
    case TLS13_GROUP_X25519:    return BR_EC_curve25519;
    case TLS13_GROUP_SECP256R1: return BR_EC_secp256r1;
    case TLS13_GROUP_SECP384R1: return BR_EC_secp384r1;
    default:                    return 0;
    }
}

/* ECDHE shared-secret length (the field/X-coordinate size) for a group. */
static size_t tls13_ecdh_secret_len(uint16_t group)
{
    switch (group) {
    case TLS13_GROUP_X25519:    return 32;
    case TLS13_GROUP_SECP256R1: return 32;
    case TLS13_GROUP_SECP384R1: return 48;
    default:                    return 0;
    }
}

/*
 * Generate an ephemeral ECDHE key pair for `group` (X25519, P-256, or
 * P-384). Stores the private scalar and the public point (X25519: 32-byte
 * u-coordinate; NIST: 0x04||X||Y uncompressed) plus their lengths and the
 * group, in hs. Uses br_ec_get_default() which dispatches per curve.
 */
static int tls13_generate_keypair(tls13_hs_ctx *hs, uint16_t group,
                                  br_hmac_drbg_context *rng)
{
    const br_ec_impl *ec = br_ec_get_default();
    br_ec_private_key sk;
    unsigned char kbuf_priv[BR_EC_KBUF_PRIV_MAX_SIZE];
    unsigned char kbuf_pub[BR_EC_KBUF_PUB_MAX_SIZE];
    size_t priv_len, pub_len;
    int curve = tls13_br_curve_for_group(group);

    if (curve == 0) return -1;

    priv_len = br_ec_keygen(&rng->vtable, ec, &sk, kbuf_priv, curve);
    if (priv_len == 0) return -1;

    pub_len = br_ec_compute_pub(ec, NULL, kbuf_pub, &sk);
    if (pub_len == 0 ||
        sk.xlen > sizeof(hs->ecdhe_secret) ||
        pub_len > sizeof(hs->ecdhe_public)) {
        secure_wipe(kbuf_priv, sizeof(kbuf_priv));
        return -1;
    }

    memcpy(hs->ecdhe_secret, sk.x, sk.xlen);
    hs->ecdhe_secret_len = sk.xlen;
    memcpy(hs->ecdhe_public, kbuf_pub, pub_len);
    hs->ecdhe_public_len = pub_len;
    hs->ecdhe_group = group;

    secure_wipe(kbuf_priv, sizeof(kbuf_priv));
    return 0;
}

/* ── ClientHello Builder ── */

/*
 * TLS 1.2 cipher suites to include for fallback compatibility.
 * These match the suites configured in certainly.c setup_bearssl().
 * Listed in preference order: ChaCha20 first (faster on PPC without
 * AES hardware), ECDSA interleaved before RSA.
 */
static const uint16_t tls12_cipher_suites[] = {
    0xCCA9,  /* TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256 */
    0xCCA8,  /* TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256 */
    0xC02B,  /* TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256 */
    0xC02F   /* TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256 */
    /*
     * fixes412 -- SHA-256 suites only. AES_256_GCM_SHA384 (0xC02C/0xC030)
     * REMOVED for the same reason the TLS 1.3 list above omits 0x1302: the
     * SHA-384 path is unsound on CW8 PPC (see the sha2big.c miscompile fixed
     * in fixes411). With those offered, servers that prefer AES-256 ran the
     * TLS 1.2 key schedule / PRF through SHA-384 and derived WRONG keys, so
     * macTLS could not decrypt the rest of the handshake -- the cipher_dec
     * ~4867 stall, 4s no-progress timeout, then retry: the slow-load
     * regression on otherwise-valid sites (mactrove, wikipedia, ...).
     * ChaCha20 / AES-128-GCM (SHA-256 transcript) are unaffected and
     * plenty strong. Restore AES-256 once the SHA-384 self-test passes on
     * hardware.
     */
};

/*
 * TLS 1.3 cipher suites. Listed in preference order.
 * ChaCha20 first for PPC performance.
 */
static const uint16_t tls13_cipher_suites[] = {
    /* SHA-256 suites only. We deliberately do NOT offer
     * TLS_AES_256_GCM_SHA384 (0x1302): the transcript hash is SHA-256 and
     * the SHA-384 re-hash path is a stub, so offering it would let a
     * server pick a suite we'd then reject. Per TLS13_SCOPE.md this build
     * is SHA-256 only. */
    TLS13_CHACHA20_POLY1305_SHA256,  /* 0x1303 */
    TLS13_AES_128_GCM_SHA256        /* 0x1301 */
};

/*
 * Signature algorithms for the signature_algorithms extension.
 * These tell the server which signature schemes we can verify.
 */
static const uint16_t sig_algorithms[] = {
    TLS13_SIG_RSA_PSS_RSAE_SHA256,      /* 0x0804 */
    TLS13_SIG_RSA_PSS_RSAE_SHA384,      /* 0x0805 */
    TLS13_SIG_ECDSA_SECP256R1_SHA256,   /* 0x0403 */
    TLS13_SIG_ECDSA_SECP384R1_SHA384,   /* 0x0503 */
    TLS13_SIG_RSA_PKCS1_SHA256,         /* 0x0401 — for cert chain only */
    TLS13_SIG_RSA_PKCS1_SHA384,         /* 0x0501 — for cert chain only */
};

/*
 * Build a ClientHello message into hs->msg_buf.
 *
 * The message is wrapped in a TLS record (5-byte header + handshake message).
 * On return, hs->msg_buf[0..hs->msg_len-1] contains the complete TLS record
 * ready to send on the wire.
 *
 * The handshake portion (excluding the 5-byte record header) is fed into
 * the transcript hash.
 *
 * Parameters:
 *   hs       — handshake context (key pair must already be generated)
 *   hostname — server name for SNI extension
 *   rng      — PRNG for client_random and legacy_session_id
 *
 * Returns 0 on success, -1 if the message doesn't fit in msg_buf.
 */
/*
 * Compute a PSK binder (macTLS#2 Stage C2). See header. Uses private
 * copies of the transcript and key schedule so the caller's running
 * state is untouched -- the binder is auxiliary, derived purely from the
 * resumption PSK and the binder-less ClientHello prefix.
 */
void tls13_compute_binder(const tls13_keysched *ks,
                          const unsigned char *psk, size_t psk_len,
                          const tls13_transcript *base,
                          const unsigned char *truncated_ch, size_t trunc_len,
                          unsigned char *out_binder)
{
    tls13_keysched bks;
    tls13_transcript tmp;
    unsigned char binder_key[64];
    unsigned char finished_key[64];
    unsigned char binder_hash[64];
    br_hmac_key_context hk;
    br_hmac_context hc;

    /* Transcript-Hash(truncated ClientHello), continuing from base. */
    tmp = *base;
    tls13_transcript_update(&tmp, truncated_ch, trunc_len);
    tls13_transcript_snapshot(&tmp, binder_hash);

    /* Early Secret from the PSK -> binder_key ("res binder") -> finished. */
    bks = *ks;
    tls13_ks_extract_early_psk(&bks, psk, psk_len);
    tls13_ks_derive_binder_key(&bks, binder_key);
    tls13_ks_derive_finished_key(&bks, binder_key, finished_key);

    /* binder = HMAC(finished_key, binder_hash); writes hash_len bytes. */
    br_hmac_key_init(&hk, bks.hash, finished_key, bks.hash_len);
    br_hmac_init(&hc, &hk, 0);
    br_hmac_update(&hc, binder_hash, bks.hash_len);
    br_hmac_out(&hc, out_binder);

    secure_wipe(binder_key, sizeof binder_key);
    secure_wipe(finished_key, sizeof finished_key);
    secure_wipe(binder_hash, sizeof binder_hash);
}

static int tls13_build_client_hello(tls13_hs_ctx *hs,
                                    const char *hostname,
                                    br_hmac_drbg_context *rng)
{
    unsigned char *buf = hs->msg_buf;
    size_t buf_size = sizeof(hs->msg_buf);
    size_t pos = 0;
    size_t hostname_len = strlen(hostname);
    size_t num_tls13_suites = sizeof(tls13_cipher_suites) / sizeof(tls13_cipher_suites[0]);
    size_t num_tls12_suites = sizeof(tls12_cipher_suites) / sizeof(tls12_cipher_suites[0]);
    size_t total_suites = num_tls13_suites + num_tls12_suites;
    size_t ext_start, ext_len_pos;
    size_t hs_len_pos;
    size_t i;

    /*
     * TLS Record Header (5 bytes)
     * Will be filled in at the end once we know the total length.
     * content_type = 22 (handshake)
     * version = 0x0301 (TLS 1.0 — required by RFC 8446 for compat)
     * length = (filled in later)
     */
    if (pos + 5 > buf_size) return -1;
    buf[pos++] = TLS13_CT_HANDSHAKE;   /* content_type = 22 */
    buf[pos++] = 0x03;                  /* version high = 3 */
    buf[pos++] = 0x01;                  /* version low = 1 (TLS 1.0) */
    pos += 2;                           /* skip length (filled later) */

    /*
     * Handshake Header (4 bytes)
     * type = 1 (ClientHello)
     * uint24 length = (filled in later)
     */
    if (pos + 4 > buf_size) return -1;
    buf[pos++] = TLS13_HT_CLIENT_HELLO;  /* type = 1 */
    hs_len_pos = pos;
    pos += 3;  /* skip uint24 length (filled later) */

    /*
     * ClientHello body starts here
     */

    /* legacy_version: 0x0303 (TLS 1.2) — required for middlebox compat */
    if (pos + 2 > buf_size) return -1;
    put_u16(buf + pos, TLS12_VERSION);
    pos += 2;

    /* Random: 32 bytes of cryptographic randomness.
     * fixes691 (#206): a post-HRR ClientHello (CH2) MUST carry the SAME
     * Random as CH1 (RFC 8446 4.1.2). Generate once on the first build and
     * replay from hs->ch_random on the HRR resend, rather than regenerating
     * (the old bug that made CH2 malformed and forced the http downgrade). */
    if (pos + 32 > buf_size) return -1;
    if (!hs->ch_fields_valid) {
        br_hmac_drbg_generate(rng, hs->ch_random, 32);
    }
    memcpy(buf + pos, hs->ch_random, 32);
    pos += 32;

    /*
     * legacy_session_id: 32 bytes.
     * RFC 8446 Section 4.1.2: MUST be set to a 32-byte value by a client
     * that wishes to use middlebox compatibility mode (we do), and — like
     * Random — MUST be echoed verbatim in the post-HRR CH2. Same replay
     * discipline as above (fixes691, #206).
     */
    if (pos + 1 + 32 > buf_size) return -1;
    buf[pos++] = 32;  /* session_id length */
    if (!hs->ch_fields_valid) {
        br_hmac_drbg_generate(rng, hs->ch_session_id, 32);
    }
    memcpy(buf + pos, hs->ch_session_id, 32);
    pos += 32;
    hs->ch_fields_valid = 1;  /* CH1 identity now captured; CH2 replays it */

    /*
     * Cipher suites: TLS 1.3 suites first, then TLS 1.2 suites.
     * TLS 1.3 suites go first so the server prefers them.
     * TLS 1.2 suites are included for fallback compatibility.
     */
    if (pos + 2 + total_suites * 2 > buf_size) return -1;
    put_u16(buf + pos, (uint16_t)(total_suites * 2));
    pos += 2;
    for (i = 0; i < num_tls13_suites; i++) {
        put_u16(buf + pos, tls13_cipher_suites[i]);
        pos += 2;
    }
    for (i = 0; i < num_tls12_suites; i++) {
        put_u16(buf + pos, tls12_cipher_suites[i]);
        pos += 2;
    }

    /*
     * legacy_compression_methods: just null compression (0x00).
     * TLS 1.3 does not support compression.
     */
    if (pos + 2 > buf_size) return -1;
    buf[pos++] = 1;   /* length of compression methods */
    buf[pos++] = 0;   /* null compression */

    /*
     * Extensions
     * We write a 2-byte placeholder for the total extensions length,
     * then each extension, then go back and fill in the length.
     */
    if (pos + 2 > buf_size) return -1;
    ext_len_pos = pos;
    pos += 2;  /* skip extensions length (filled later) */
    ext_start = pos;

    /* ── Extension: SNI (Server Name Indication) — type 0 ── */
    {
        /*
         * Format:
         *   ext_type (2)
         *   ext_data_length (2)
         *   server_name_list_length (2)
         *   name_type (1) = 0 (hostname)
         *   name_length (2)
         *   hostname (variable)
         */
        size_t sni_ext_len = 2 + 1 + 2 + hostname_len;  /* list_len + type + name_len + name */
        if (pos + 4 + sni_ext_len > buf_size) return -1;
        put_u16(buf + pos, TLS13_EXT_SERVER_NAME);
        pos += 2;
        put_u16(buf + pos, (uint16_t)sni_ext_len);
        pos += 2;
        put_u16(buf + pos, (uint16_t)(sni_ext_len - 2));  /* server_name_list length */
        pos += 2;
        buf[pos++] = 0;  /* name_type = hostname */
        put_u16(buf + pos, (uint16_t)hostname_len);
        pos += 2;
        memcpy(buf + pos, hostname, hostname_len);
        pos += hostname_len;
    }

    /* ── Extension: Supported Groups — type 10 ── */
    {
        /*
         * Advertise X25519 (preferred), then the NIST curves P-256 and
         * P-384 so servers that disable X25519 (FIPS/compliance zones)
         * can still negotiate — they'll HelloRetryRequest us to the curve
         * they require. Format: ext_type, ext_data_len, list_len, then the
         * 2-byte named groups.
         */
        if (pos + 4 + 2 + 6 > buf_size) return -1;
        put_u16(buf + pos, TLS13_EXT_SUPPORTED_GROUPS);
        pos += 2;
        put_u16(buf + pos, 8);  /* ext_data_length: list_len(2) + 3*2 */
        pos += 2;
        put_u16(buf + pos, 6);  /* named_group_list_length: 3 groups */
        pos += 2;
        put_u16(buf + pos, TLS13_GROUP_X25519);
        pos += 2;
        put_u16(buf + pos, TLS13_GROUP_SECP256R1);
        pos += 2;
        put_u16(buf + pos, TLS13_GROUP_SECP384R1);
        pos += 2;
    }

    /* ── Extension: Signature Algorithms — type 13 ── */
    {
        /*
         * Format:
         *   ext_type (2)
         *   ext_data_length (2)
         *   sig_alg_list_length (2)
         *   signature_scheme (2) * N
         */
        size_t num_sigs = sizeof(sig_algorithms) / sizeof(sig_algorithms[0]);
        size_t list_len = num_sigs * 2;
        if (pos + 4 + 2 + list_len > buf_size) return -1;
        put_u16(buf + pos, TLS13_EXT_SIGNATURE_ALGORITHMS);
        pos += 2;
        put_u16(buf + pos, (uint16_t)(2 + list_len));  /* ext_data_length */
        pos += 2;
        put_u16(buf + pos, (uint16_t)list_len);  /* sig_alg_list_length */
        pos += 2;
        for (i = 0; i < num_sigs; i++) {
            put_u16(buf + pos, sig_algorithms[i]);
            pos += 2;
        }
    }

    /* ── Extension: Supported Versions — type 43 ── */
    {
        /*
         * Format:
         *   ext_type (2)
         *   ext_data_length (2)
         *   versions_length (1)
         *   version (2) = 0x0304 (TLS 1.3)
         *   version (2) = 0x0303 (TLS 1.2)
         *
         * This is how we signal TLS 1.3 support. The legacy_version field
         * in the ClientHello body stays at 0x0303 for middlebox compat,
         * and the server looks HERE for the actual version list.
         */
        if (pos + 4 + 5 > buf_size) return -1;
        put_u16(buf + pos, TLS13_EXT_SUPPORTED_VERSIONS);
        pos += 2;
        put_u16(buf + pos, 5);  /* ext_data_length */
        pos += 2;
        buf[pos++] = 4;  /* versions_length (2 versions * 2 bytes) */
        put_u16(buf + pos, TLS13_VERSION);   /* 0x0304 — TLS 1.3 */
        pos += 2;
        put_u16(buf + pos, TLS12_VERSION);   /* 0x0303 — TLS 1.2 */
        pos += 2;
    }

    /* ── Extension: Key Share — type 51 ── */
    {
        /*
         * One KeyShareEntry for the group we generated a keypair for
         * (hs->ecdhe_group): X25519 on the first flight, or the
         * server-requested NIST curve on a HelloRetryRequest retry. The
         * server completes the exchange from this share, or HRRs us to a
         * group we advertised but didn't share.
         * Format: ext_type, ext_data_len, client_shares_len, named_group,
         *   key_exchange_len, key_exchange(public point).
         */
        size_t klen = hs->ecdhe_public_len;
        if (pos + 4 + 2 + 2 + 2 + klen > buf_size) return -1;
        put_u16(buf + pos, TLS13_EXT_KEY_SHARE);
        pos += 2;
        put_u16(buf + pos, (uint16_t)(6 + klen));  /* ext_data_length: 2+2+2+klen */
        pos += 2;
        put_u16(buf + pos, (uint16_t)(4 + klen));  /* client_shares_length: 2+2+klen */
        pos += 2;
        put_u16(buf + pos, hs->ecdhe_group);
        pos += 2;
        put_u16(buf + pos, (uint16_t)klen);  /* key_exchange_length */
        pos += 2;
        memcpy(buf + pos, hs->ecdhe_public, klen);
        pos += klen;
    }

    /* ── Extension: Cookie — type 44 (only on HelloRetryRequest retry) ── */
    if (hs->cookie_len > 0) {
        /*
         * When the server sends HelloRetryRequest with a cookie,
         * we must echo it back in the retry ClientHello. This handles
         * the Cloudflare DoS protection case.
         *
         * Format:
         *   ext_type (2)
         *   ext_data_length (2)
         *   cookie_length (2)
         *   cookie (variable)
         */
        if (pos + 4 + 2 + hs->cookie_len > buf_size) return -1;
        put_u16(buf + pos, TLS13_EXT_COOKIE);
        pos += 2;
        put_u16(buf + pos, (uint16_t)(2 + hs->cookie_len));
        pos += 2;
        put_u16(buf + pos, (uint16_t)hs->cookie_len);
        pos += 2;
        memcpy(buf + pos, hs->cookie, hs->cookie_len);
        pos += hs->cookie_len;
    }

    /* ── Extension: ALPN — type 16 ──
     * macTLS#196: Needed for HTTP/2-only edges (e.g. Cloudflare) */
    if (pos + 4 + 11 > buf_size) return -1;
    put_u16(buf + pos, TLS13_EXT_ALPN); pos += 2;
    put_u16(buf + pos, 11); pos += 2;             /* ext_data_length */
    put_u16(buf + pos, 9); pos += 2;              /* protocol_name_list_length */
    buf[pos++] = 8;                               /* protocol_name_length */
    memcpy(buf + pos, "http/1.1", 8); pos += 8;   /* protocol_name */

    /* ── Extension: psk_key_exchange_modes — type 45 ──
     * Sent on EVERY ClientHello (macTLS#2 Stage E fix): a server will not
     * issue NewSessionTicket unless the client advertised PSK support via
     * this extension (RFC 8446 4.2.9). Without it we'd never receive a
     * ticket to resume with. Value: single mode psk_dhe_ke (1). */
    if (pos + 6 > buf_size) return -1;
    put_u16(buf + pos, TLS13_EXT_PSK_KEY_EXCHANGE_MODES); pos += 2;
    put_u16(buf + pos, 2); pos += 2;
    buf[pos++] = 1;                               /* psk_ke_modes length */
    buf[pos++] = 1;                               /* psk_dhe_ke */

    /* ── Extension: pre_shared_key — type 41 (resumption only) ──
     * MUST be the final extension. The binder is HMAC'd over the transcript
     * of this ClientHello truncated just before the binders, with every
     * length field still counting the binders (RFC 8446 4.2.11.2). */
    if (hs->resuming && hs->offer_ticket != NULL &&
        hs->offer_ticket->psk_len == hs->transcript.hash_len) {
        const tls13_session_ticket *tk = hs->offer_ticket;
        size_t id_len = tk->ticket_len;
        size_t blen = tk->psk_len;                /* binder length = PSK hash_len */
        size_t identities_block = 2 + id_len + 4; /* one identity entry */
        size_t binders_block = 1 + blen;          /* one binder entry */
        size_t psk_ext_data = 2 + identities_block + 2 + binders_block;
        size_t binders_section = 2 + binders_block; /* binders list len + entry */
        size_t binder_off;
        /* hs->ks isn't set up until ServerHello, so the binder uses a
         * keysched keyed to the TICKET's hash (matched to the transcript
         * hash above; v1 is SHA-256). */
        tls13_keysched binder_ks;
        const br_hash_class *psk_hash =
            (blen == 48) ? &br_sha384_vtable : &br_sha256_vtable;

        if (pos + 4 + psk_ext_data > buf_size) return -1;
        put_u16(buf + pos, TLS13_EXT_PRE_SHARED_KEY); pos += 2;
        put_u16(buf + pos, (uint16_t)psk_ext_data); pos += 2;
        put_u16(buf + pos, (uint16_t)identities_block); pos += 2;
        put_u16(buf + pos, (uint16_t)id_len); pos += 2;
        memcpy(buf + pos, tk->ticket, id_len); pos += id_len;
        buf[pos++] = (unsigned char)(hs->offer_obfuscated_age >> 24);
        buf[pos++] = (unsigned char)(hs->offer_obfuscated_age >> 16);
        buf[pos++] = (unsigned char)(hs->offer_obfuscated_age >> 8);
        buf[pos++] = (unsigned char)(hs->offer_obfuscated_age);
        put_u16(buf + pos, (uint16_t)binders_block); pos += 2;
        binder_off = pos + 1;                     /* after the binder_len byte */
        buf[pos++] = (unsigned char)blen;
        memset(buf + pos, 0, blen);               /* binder placeholder */
        pos += blen;

        /* Backpatch lengths NOW — they count the binders. */
        put_u16(buf + ext_len_pos, (uint16_t)(pos - ext_start));
        put_u24(buf + hs_len_pos, (uint32_t)(pos - hs_len_pos - 3));
        put_u16(buf + 3, (uint16_t)(pos - 5));

        /* Binder over the message truncated before the binders, continuing
         * the running transcript (handles HRR: CH1 + HRR already hashed).
         * binder_ks is keyed to the ticket's hash (hs->ks not ready yet). */
        tls13_ks_init(&binder_ks, psk_hash);
        tls13_compute_binder(&binder_ks, tk->psk, tk->psk_len,
                             &hs->transcript,
                             buf + 5, (pos - 5) - binders_section,
                             buf + binder_off);

        hs->msg_len = pos;
        hs->msg_offset = 0;
        /* Now feed the FULL ClientHello (real binder) into the transcript. */
        tls13_transcript_update(&hs->transcript, buf + 5, pos - 5);
        return 0;
    }

    /*
     * Backpatch lengths.
     *
     * We now know the total size of everything, so fill in the
     * length fields we skipped earlier.
     */

    /* Extensions length (2 bytes at ext_len_pos) */
    put_u16(buf + ext_len_pos, (uint16_t)(pos - ext_start));

    /* Handshake message length (3 bytes at hs_len_pos) */
    put_u24(buf + hs_len_pos, (uint32_t)(pos - hs_len_pos - 3));

    /* TLS record length (2 bytes at offset 3) */
    put_u16(buf + 3, (uint16_t)(pos - 5));

    hs->msg_len = pos;
    hs->msg_offset = 0;

    /*
     * Update the transcript hash with the handshake message.
     * The transcript includes everything from the handshake header
     * (type + uint24 length) through the end — NOT the 5-byte
     * TLS record header.
     */
    tls13_transcript_update(&hs->transcript, buf + 5, pos - 5);

    return 0;
}

/* ── CCS (Change Cipher Spec) Builder ── */

/*
 * Build a Change Cipher Spec record into a buffer.
 *
 * CCS is NOT a TLS 1.3 concept — it's sent purely for middlebox
 * compatibility (RFC 8446 Appendix D.4). Middleboxes (corporate
 * firewalls, load balancers) that were designed for TLS 1.2 expect
 * to see a CCS message after the ClientHello. Without it, they may
 * drop the connection thinking something is wrong.
 *
 * The CCS is sent in the clear (before any encryption starts) and
 * the server ignores it. It's just one byte: 0x01.
 *
 * Format:
 *   content_type (1) = 20
 *   version (2) = 0x0303 (TLS 1.2)
 *   length (2) = 1
 *   payload (1) = 0x01
 *
 * The buffer must be at least 6 bytes. Returns the record length (6).
 */
static size_t tls13_build_ccs(unsigned char *buf)
{
    buf[0] = TLS13_CT_CHANGE_CIPHER_SPEC;  /* content_type = 20 */
    buf[1] = 0x03;                          /* version high */
    buf[2] = 0x03;                          /* version low (TLS 1.2) */
    buf[3] = 0x00;                          /* length high */
    buf[4] = 0x01;                          /* length low */
    buf[5] = 0x01;                          /* CCS payload */
    return 6;
}

/* ── ServerHello Parser and Key Exchange ── */

/*
 * HelloRetryRequest sentinel random value (RFC 8446 Section 4.1.3).
 * If the ServerHello random field matches this value exactly, the
 * message is actually a HelloRetryRequest, not a real ServerHello.
 */
static const unsigned char hrr_random[32] = {
    0xCF, 0x21, 0xAD, 0x74, 0xE5, 0x9A, 0x61, 0x11,
    0xBE, 0x1D, 0x8C, 0x02, 0x1E, 0x65, 0xB8, 0x91,
    0xC2, 0xA2, 0x11, 0x16, 0x7A, 0xBB, 0x8C, 0x5E,
    0x07, 0x9E, 0x09, 0xE2, 0xC8, 0xA8, 0x33, 0x9C
};

/*
 * Compute the ECDHE shared secret for `group`.
 *
 * Scalar-multiplies the peer's public point by our private key (in place),
 * then extracts the shared secret: for X25519 the whole 32-byte result; for
 * the NIST curves the X coordinate of the resulting 0x04||X||Y point. Writes
 * the secret to out and its length to *out_len.
 *
 * Returns 0 on success, -1 on error (unsupported group, bad point, overflow).
 */
static int tls13_ecdh_shared(uint16_t group,
                             const unsigned char *priv, size_t priv_len,
                             const unsigned char *peer_pub, size_t peer_pub_len,
                             unsigned char *out, size_t *out_len)
{
    const br_ec_impl *ec = br_ec_get_default();
    unsigned char point[133];   /* worst case: P-521 uncompressed (133) */
    uint32_t result;
    int curve = tls13_br_curve_for_group(group);
    size_t flen = tls13_ecdh_secret_len(group);

    if (curve == 0 || flen == 0 || peer_pub_len == 0 ||
        peer_pub_len > sizeof(point)) {
        return -1;
    }

    /* mul() works in place on the point buffer. */
    memcpy(point, peer_pub, peer_pub_len);
    result = ec->mul(point, peer_pub_len, priv, priv_len, curve);
    if (result == 0) {
        secure_wipe(point, sizeof(point));
        return -1;
    }

    if (group == TLS13_GROUP_X25519) {
        /* The whole 32-byte result is the shared secret. */
        memcpy(out, point, flen);
    } else {
        /* NIST: shared point is 0x04 || X || Y; secret is X. */
        if (peer_pub_len < 1 + flen) {
            secure_wipe(point, sizeof(point));
            return -1;
        }
        memcpy(out, point + 1, flen);
    }
    *out_len = flen;
    secure_wipe(point, sizeof(point));
    return 0;
}

/*
 * Determine the AEAD key length for a TLS 1.3 cipher suite.
 * AES-128-GCM uses 16-byte keys; AES-256-GCM and ChaCha20-Poly1305 use 32.
 */
static size_t tls13_key_len_for_suite(uint16_t suite)
{
    switch (suite) {
    case TLS13_AES_128_GCM_SHA256:
        return 16;
    case TLS13_AES_256_GCM_SHA384:
    case TLS13_CHACHA20_POLY1305_SHA256:
        return 32;
    default:
        return 0;
    }
}

/*
 * Get the hash class for a TLS 1.3 cipher suite.
 * AES-256-GCM-SHA384 uses SHA-384; everything else uses SHA-256.
 */
static const br_hash_class *tls13_hash_for_suite(uint16_t suite)
{
    if (suite == TLS13_AES_256_GCM_SHA384) {
        return &br_sha384_vtable;
    }
    return &br_sha256_vtable;
}

/*
 * Parse a ServerHello message and extract key material.
 *
 * The message buffer starts at the handshake header (type byte),
 * NOT the TLS record header. Format:
 *
 *   HandshakeType (1) = 2 (ServerHello)
 *   uint24 length (3)
 *   ProtocolVersion legacy_version (2) = 0x0303
 *   Random (32)
 *   opaque legacy_session_id_echo<0..32>
 *   CipherSuite (2)
 *   uint8 legacy_compression_method (1) = 0
 *   Extension extensions<6..2^16-1>
 *
 * Returns:
 *   kTLS13_OK         — TLS 1.3 ServerHello parsed, shared secret computed
 *   kTLS13_Fallback12 — server chose TLS 1.2
 *   kTLS13_WantRead   — this is an HRR; cookie saved, must resend ClientHello
 *   kTLS13_Error      — parse error
 */
static tls13_hs_result tls13_parse_server_hello(tls13_hs_ctx *hs,
                                                const unsigned char *msg,
                                                size_t msg_len)
{
    size_t pos = 0;
    uint32_t hs_msg_len;
    const unsigned char *random;
    uint8_t session_id_len;
    uint16_t cipher_suite;
    uint16_t ext_total_len;
    size_t ext_end;
    int is_hrr;
    int found_supported_versions = 0;
    int found_key_share = 0;
    unsigned char server_public[133];   /* up to P-521 uncompressed point */
    size_t server_public_len = 0;
    uint16_t server_share_group = 0;    /* group the server chose / requested (HRR) */
    uint16_t negotiated_version = 0;

    /*
     * Minimum ServerHello size:
     * 4 (hs header) + 2 (version) + 32 (random) + 1 (session_id_len) +
     * 2 (cipher suite) + 1 (compression) + 2 (extensions length) = 44
     */
    if (msg_len < 44) {
        hs->error = BR_ERR_BAD_PARAM;
        return kTLS13_Error;
    }

    /* Handshake header: type must be ServerHello (2) */
    if (msg[0] != TLS13_HT_SERVER_HELLO) {
        hs->error = BR_ERR_UNEXPECTED;
        return kTLS13_Error;
    }
    hs_msg_len = get_u24(msg + 1);
    pos = 4;

    /* Sanity check: declared length must match available data */
    if (hs_msg_len + 4 > msg_len) {
        hs->error = BR_ERR_BAD_PARAM;
        return kTLS13_Error;
    }

    /* legacy_version: must be 0x0303 (TLS 1.2) */
    if (get_u16(msg + pos) != TLS12_VERSION) {
        hs->error = BR_ERR_BAD_VERSION;
        return kTLS13_Error;
    }
    pos += 2;

    /* Random (32 bytes) — save pointer for HRR check */
    random = msg + pos;
    pos += 32;

    /* Check for HelloRetryRequest by comparing random to sentinel */
    is_hrr = (memcmp(random, hrr_random, 32) == 0);

    /* legacy_session_id_echo: variable length, skip it */
    session_id_len = msg[pos];
    pos += 1;
    if (pos + session_id_len > msg_len) {
        hs->error = BR_ERR_BAD_PARAM;
        return kTLS13_Error;
    }
    pos += session_id_len;

    /* CipherSuite (2 bytes) */
    if (pos + 2 > msg_len) {
        hs->error = BR_ERR_BAD_PARAM;
        return kTLS13_Error;
    }
    cipher_suite = get_u16(msg + pos);
    pos += 2;

    /* legacy_compression_method: must be 0 */
    if (pos >= msg_len || msg[pos] != 0) {
        hs->error = BR_ERR_BAD_PARAM;
        return kTLS13_Error;
    }
    pos += 1;

    /* Extensions — parse to find supported_versions, key_share, cookie */
    if (pos + 2 > msg_len) {
        /* No extensions at all — this is a TLS 1.2 ServerHello */
        return kTLS13_Fallback12;
    }
    ext_total_len = get_u16(msg + pos);
    pos += 2;
    ext_end = pos + ext_total_len;

    if (ext_end > msg_len) {
        hs->error = BR_ERR_BAD_PARAM;
        return kTLS13_Error;
    }

    while (pos + 4 <= ext_end) {
        uint16_t ext_type = get_u16(msg + pos);
        uint16_t ext_len = get_u16(msg + pos + 2);
        const unsigned char *ext_data = msg + pos + 4;
        pos += 4;

        if (pos + ext_len > ext_end) {
            hs->error = BR_ERR_BAD_PARAM;
            return kTLS13_Error;
        }

        switch (ext_type) {
        case TLS13_EXT_SUPPORTED_VERSIONS:
            /*
             * In ServerHello, supported_versions contains exactly one
             * ProtocolVersion (2 bytes) — the version selected by the server.
             * (Unlike ClientHello which sends a list.)
             */
            if (ext_len != 2) {
                hs->error = BR_ERR_BAD_PARAM;
                return kTLS13_Error;
            }
            negotiated_version = get_u16(ext_data);
            found_supported_versions = 1;
            break;

        case TLS13_EXT_KEY_SHARE:
            /*
             * Two forms:
             *  - HelloRetryRequest: NamedGroup group (2), no key. The
             *    server is telling us which group to use on the retry.
             *  - ServerHello: NamedGroup group (2), uint16 key_len (2),
             *    key_exchange (key_len) — the server's public point.
             * We distinguish by ext_len (2 = HRR, >2 = real share).
             */
            if (ext_len < 2) {
                hs->error = BR_ERR_BAD_PARAM;
                return kTLS13_Error;
            }
            {
                uint16_t group = get_u16(ext_data);
                server_share_group = group;
                if (ext_len == 2) {
                    /* HRR group selection — no key present. */
                    found_key_share = 1;
                } else {
                    uint16_t ke_len = get_u16(ext_data + 2);
                    if (tls13_br_curve_for_group(group) == 0) {
                        hs->error = BR_ERR_BAD_PARAM;
                        return kTLS13_Error;
                    }
                    if (ke_len == 0 || ext_len != 4 + ke_len ||
                        ke_len > sizeof(server_public)) {
                        hs->error = BR_ERR_BAD_PARAM;
                        return kTLS13_Error;
                    }
                    memcpy(server_public, ext_data + 4, ke_len);
                    server_public_len = ke_len;
                    found_key_share = 1;
                }
            }
            break;

        case TLS13_EXT_COOKIE:
            /*
             * Cookie extension (only in HelloRetryRequest):
             *   uint16 cookie_length
             *   opaque cookie[cookie_length]
             */
            if (ext_len < 2) {
                hs->error = BR_ERR_BAD_PARAM;
                return kTLS13_Error;
            }
            {
                uint16_t cookie_len = get_u16(ext_data);
                if (cookie_len + 2 != ext_len) {
                    hs->error = BR_ERR_BAD_PARAM;
                    return kTLS13_Error;
                }
                if (cookie_len > sizeof(hs->cookie)) {
                    hs->error = BR_ERR_TOO_LARGE;
                    return kTLS13_Error;
                }
                memcpy(hs->cookie, ext_data + 2, cookie_len);
                hs->cookie_len = cookie_len;
            }
            break;

        case TLS13_EXT_PRE_SHARED_KEY:
            /*
             * ServerHello pre_shared_key is a single uint16
             * selected_identity. Its presence means the server accepted
             * our resumption PSK; we offer exactly one identity (index 0),
             * so any echo is an accept. Absence => declined => the normal
             * full handshake runs (Certificate path). macTLS#2 Stage D.
             */
            if (ext_len != 2) {
                hs->error = BR_ERR_BAD_PARAM;
                return kTLS13_Error;
            }
            hs->resumption_accepted = 1;
            break;

        default:
            /* Ignore unknown extensions */
            break;
        }

        pos += ext_len;
    }

    /*
     * Version negotiation: if supported_versions is absent or contains
     * 0x0303, this is TLS 1.2. If it contains 0x0304, this is TLS 1.3.
     */
    if (!found_supported_versions || negotiated_version == TLS12_VERSION) {
        return kTLS13_Fallback12;
    }
    if (negotiated_version != TLS13_VERSION) {
        hs->error = BR_ERR_BAD_VERSION;
        return kTLS13_Error;
    }

    /* Save negotiated cipher suite */
    hs->cipher_suite = cipher_suite;
    hs->is_tls13 = 1;

    /*
     * Validate cipher suite — must be one we offered.
     */
    if (tls13_key_len_for_suite(cipher_suite) == 0) {
        hs->error = BR_ERR_BAD_CIPHER_SUITE;
        return kTLS13_Error;
    }

    /*
     * Handle HelloRetryRequest.
     */
    if (is_hrr) {
        if (hs->hrr_received) {
            /* RFC 8446: only one HRR allowed */
            hs->error = BR_ERR_UNEXPECTED;
            hs->fail_site = TLS13_FAIL_HRR_DOUBLE;
            return kTLS13_Error;
        }
        hs->hrr_received = 1;

        /*
         * Record the group the server wants. The retry ClientHello will
         * generate a key_share for it (see the SendClientHello handler).
         * If the server named a group we don't support, fail cleanly.
         */
        if (tls13_br_curve_for_group(server_share_group) == 0) {
            hs->error = BR_ERR_BAD_PARAM;
            hs->fail_site = TLS13_FAIL_HRR_GROUP_UNSUPP;
            return kTLS13_Error;
        }
        hs->hrr_group = server_share_group;
        hs->hrr_pending = 1;   /* transient: tells the dispatcher to resend now */

        /*
         * Update transcript for HRR per RFC 8446 Section 4.4.1:
         *
         * 1. Current transcript has just ClientHello1
         * 2. Reset: replace with synthetic message_hash(Hash(CH1))
         * 3. Feed the HRR (ServerHello) into the reset transcript
         *
         * After this, transcript = Hash(message_hash || HRR),
         * ready for the retry ClientHello to be appended.
         */
        tls13_transcript_reset_for_hrr(&hs->transcript);
        tls13_transcript_update(&hs->transcript, msg, hs_msg_len + 4);

        /* Signal caller to resend ClientHello */
        return kTLS13_WantRead;  /* reused as "need to retry" signal */
    }

    /*
     * Regular ServerHello — we need a key_share extension.
     */
    if (!found_key_share) {
        hs->error = BR_ERR_BAD_PARAM;
        hs->fail_site = TLS13_FAIL_SH_NO_KEYSHARE;
        return kTLS13_Error;
    }

    /*
     * Re-initialize key schedule if the negotiated hash differs from
     * our default (SHA-256). This happens if the server chose
     * TLS_AES_256_GCM_SHA384.
     */
    {
        const br_hash_class *suite_hash = tls13_hash_for_suite(cipher_suite);
        if (suite_hash != hs->transcript.hash) {
            /*
             * The server chose a cipher suite with a different hash.
             * We need to re-hash the transcript from scratch with the
             * new hash. For simplicity, since we only support SHA-256
             * and SHA-384, and our default is SHA-256, this only happens
             * for AES-256-GCM-SHA384.
             *
             * TODO: In a full implementation, we'd need to re-hash from
             * the original ClientHello bytes. For now, treat this as an
             * error since our preferred suites all use SHA-256.
             */
            hs->error = BR_ERR_BAD_CIPHER_SUITE;
            hs->fail_site = TLS13_FAIL_SH_HASH_MISMATCH;
            return kTLS13_Error;
        }
    }

    /*
     * Update transcript hash with the full ServerHello handshake message.
     * This must happen before we derive keys, since the transcript hash
     * at this point (Hash(CH || SH)) is used for key derivation.
     */
    tls13_transcript_update(&hs->transcript, msg, hs_msg_len + 4);

    /*
     * Compute the ECDHE shared secret on the negotiated curve.
     */
    {
        unsigned char shared_secret[64];   /* up to P-384's 48-byte secret */
        unsigned char transcript_hash[64];
        unsigned char client_key[32], client_iv[12];
        unsigned char server_key[32], server_iv[12];
        size_t key_len;
        size_t shared_len = 0;
        int ret;

        /* The server must answer with the group we key_shared. */
        if (server_share_group != hs->ecdhe_group) {
            hs->error = BR_ERR_BAD_PARAM;
            hs->fail_site = TLS13_FAIL_SH_GROUP_MISMATCH;
            return kTLS13_Error;
        }
        ret = tls13_ecdh_shared(hs->ecdhe_group,
                                hs->ecdhe_secret, hs->ecdhe_secret_len,
                                server_public, server_public_len,
                                shared_secret, &shared_len);
        if (ret != 0) {
            hs->error = BR_ERR_BAD_PARAM;
            hs->fail_site = TLS13_FAIL_ECDH_COMPUTE;
            return kTLS13_Error;
        }

        /*
         * Derive handshake keys.
         *
         * Key schedule progression:
         * 1. Extract Early Secret (from zeros — no PSK)
         * 2. Extract Handshake Secret (from shared secret)
         * 3. Derive traffic keys from transcript hash
         */
        tls13_ks_init(&hs->ks, hs->transcript.hash);
        /* macTLS#2 Stage D: on an accepted resumption the Early Secret
         * comes from the resumption PSK, not zeros. If we didn't offer a
         * PSK, or the server declined (no pre_shared_key echo), use the
         * normal no-PSK path. The Handshake Secret still folds in the
         * fresh ECDHE shared secret (psk_dhe_ke) either way. */
        if (hs->resumption_accepted && hs->offer_ticket != NULL) {
            tls13_ks_extract_early_psk(&hs->ks, hs->offer_ticket->psk,
                                       hs->offer_ticket->psk_len);
        } else {
            tls13_ks_extract_early(&hs->ks);
        }
        tls13_ks_extract_handshake(&hs->ks, shared_secret, shared_len);

        /* Wipe the shared secret immediately after use */
        secure_wipe(shared_secret, sizeof(shared_secret));

        /* Get transcript hash: Hash(ClientHello || ServerHello) */
        tls13_transcript_snapshot(&hs->transcript, transcript_hash);

        /* Derive handshake traffic keys + save raw traffic secrets */
        key_len = tls13_key_len_for_suite(cipher_suite);
        tls13_ks_derive_handshake_keys(&hs->ks, transcript_hash,
                                       key_len,
                                       client_key, client_iv,
                                       server_key, server_iv,
                                       hs->client_hs_secret,
                                       hs->server_hs_secret);

        /* Initialize record contexts for encrypted communication */
        tls13_record_init(&hs->read_ctx,
                          server_key, key_len, server_iv, cipher_suite);
        tls13_record_init(&hs->write_ctx,
                          client_key, key_len, client_iv, cipher_suite);

        /* Wipe intermediate key material from the stack */
        secure_wipe(transcript_hash, sizeof(transcript_hash));
        secure_wipe(client_key, sizeof(client_key));
        secure_wipe(client_iv, sizeof(client_iv));
        secure_wipe(server_key, sizeof(server_key));
        secure_wipe(server_iv, sizeof(server_iv));
    }

    /* Wipe the private key — no longer needed after key exchange */
    secure_wipe(hs->ecdhe_secret, sizeof(hs->ecdhe_secret));

    return kTLS13_OK;
}

/*
 * Handle the kTLS13_RecvServerHello state.
 *
 * Reads TLS records from the engine's recvrec buffer. Ignores CCS
 * records (middlebox compatibility). When a Handshake record arrives,
 * parses it as ServerHello.
 *
 * Possible outcomes:
 *   - TLS 1.3 ServerHello: compute shared secret, derive handshake keys,
 *     advance to kTLS13_RecvEncryptedExtensions
 *   - HelloRetryRequest: save cookie, reset transcript, go back to
 *     kTLS13_SendClientHello
 *   - TLS 1.2 fallback: return kTLS13_Fallback12 to let the caller
 *     restart with BearSSL's T0 engine
 */
static tls13_hs_result tls13_state_recv_server_hello(tls13_hs_ctx *hs,
                                                     unsigned char *recv_buf,
                                                     size_t *recv_len)
{
    uint8_t record_type;
    uint16_t record_len;
    size_t total;
    tls13_hs_result result;

    /*
     * Parse the TLS record header (5 bytes):
     *   content_type (1)
     *   legacy_record_version (2) — 0x0301 or 0x0303
     *   length (2)
     */
    if (*recv_len < 5) {
        return kTLS13_WantRead;
    }

    record_type = recv_buf[0];
    record_len = get_u16(recv_buf + 3);
    total = (size_t)5 + record_len;

    /* Check we have the full record */
    if (*recv_len < total) {
        return kTLS13_WantRead;
    }

    /*
     * Handle CCS records (type 20).
     * These are expected for middlebox compatibility and are silently
     * ignored. The CCS is just one byte: 0x01.
     */
    if (record_type == TLS13_CT_CHANGE_CIPHER_SPEC) {
        memmove(recv_buf, recv_buf + total, *recv_len - total);
        *recv_len -= total;
        return kTLS13_WantRead;
    }

    /*
     * Handle Alert records (type 21).
     *
     * If we see a plaintext alert at the ServerHello stage, the server
     * rejected our ClientHello outright. Some servers (notably older
     * AWS/nginx deployments like httpbin.org) refuse TLS 1.3 by closing
     * with close_notify instead of gracefully downgrading via a
     * legacy TLS 1.2 ServerHello. Treat any pre-handshake alert as a
     * "retry with TLS 1.2" signal — BearSSL's T0 engine will send a
     * pure TLS 1.2 ClientHello (no supported_versions extension) on
     * the reconnect, which most servers accept.
     *
     * We still record the alert in hs->error (0x1LLDD encoding) so
     * diagnostics are available if the TLS 1.2 retry also fails.
     */
    if (record_type == TLS13_CT_ALERT) {
        if (record_len >= 2) {
            hs->error = 0x10000 | ((int)recv_buf[5] << 8) | (int)recv_buf[6];
        } else {
            hs->error = 0x1F000 | (int)record_len;
        }
        /* Consume the record so the pump's reconnect starts clean. */
        memmove(recv_buf, recv_buf + total, *recv_len - total);
        *recv_len -= total;
        return kTLS13_Fallback12;
    }

    /* We expect a Handshake record (type 22) */
    if (record_type != TLS13_CT_HANDSHAKE) {
        /* Encode type for diagnostics */
        hs->error = 0x2000 | (int)record_type;
        return kTLS13_Error;
    }

    /*
     * Parse the handshake message.
     * The record payload (after the 5-byte header) is the handshake
     * message starting with the type byte.
     */
    result = tls13_parse_server_hello(hs, recv_buf + 5, record_len);

    /* Consume the record from the front of the buffer */
    memmove(recv_buf, recv_buf + total, *recv_len - total);
    *recv_len -= total;

    return result;
}

/* ── Encrypted Handshake Message Processing ── */

/*
 * Read and decrypt an encrypted handshake message from the server.
 *
 * All messages after ServerHello are encrypted with the server handshake
 * key. This function:
 * 1. Reads a record from BearSSL's recvrec buffer
 * 2. Skips CCS records (middlebox compatibility)
 * 3. Decrypts using tls13_record_decrypt() with hs->read_ctx
 * 4. Returns the handshake message type and decrypted data
 *
 * The decrypted handshake message (type + uint24 length + body) is placed
 * in out_data. out_hs_type receives the handshake type byte.
 *
 * Returns:
 *   kTLS13_OK       — message decrypted, out_hs_type and out_data filled
 *   kTLS13_WantRead — need more data from network
 *   kTLS13_Error    — decryption or parse failure
 */
static tls13_hs_result tls13_read_encrypted_hs(
    tls13_hs_ctx *hs, unsigned char *recv_buf, size_t *recv_len,
    uint8_t *out_hs_type, unsigned char *out_data, size_t *out_len)
{
    uint8_t record_type;
    uint16_t record_len;
    size_t total;
    uint8_t inner_ct;
    int ret;

    for (;;) {
        /*
         * If the plaintext buffer has leftover data from a previous
         * decryption, extract the next handshake message from it.
         * TLS 1.3 servers often pack multiple handshake messages
         * (EncryptedExtensions, Certificate, CertificateVerify,
         * Finished) into a single encrypted record.
         */
        if (hs->plain_offset < hs->plain_len) {
            size_t remaining = hs->plain_len - hs->plain_offset;
            uint32_t hs_body_len;
            size_t total_hs;

            /*
             * Extract a complete handshake message if both the 4-byte
             * header AND the full body are buffered. If either is
             * incomplete, the message spans multiple TLS records: rather
             * than fail (the old "not supported in v1" path), we compact
             * the partial bytes to the front of plain_buf and fall
             * through to read + APPEND the next record, then retry.
             *
             * fixes370 (#167): Facebook's Certificate message carries a
             * large RSA-intermediate chain that the server splits across
             * record boundaries, so the Certificate handshake message
             * does not fit in one decrypted record. mactrove's smaller
             * all-ECDSA chain fits in a single record, which is why it
             * worked and Facebook hit BR_ERR_BAD_PARAM here at state 4.
             */
            if (remaining >= 4) {
                *out_hs_type = hs->plain_buf[hs->plain_offset];
                hs_body_len = get_u24(hs->plain_buf + hs->plain_offset + 1);
                total_hs = 4 + (size_t)hs_body_len;

                /*
                 * out_data is hs->msg_buf. A single handshake message
                 * larger than our reassembly buffer is genuinely
                 * unsupported (would overflow msg_buf). plain_buf and
                 * msg_buf are the same size, so this also bounds how much
                 * we can reassemble below.
                 */
                if (total_hs > sizeof(hs->msg_buf)) {
                    hs->error = BR_ERR_BAD_PARAM;
                    return kTLS13_Error;
                }

                if (total_hs <= remaining) {
                    /* Whole message present — hand it up. */
                    memcpy(out_data, hs->plain_buf + hs->plain_offset,
                           total_hs);
                    *out_len = total_hs;
                    hs->plain_offset += total_hs;
                    if (hs->plain_offset >= hs->plain_len) {
                        hs->plain_len = 0;
                        hs->plain_offset = 0;
                    }
                    return kTLS13_OK;
                }
            }

            /*
             * Incomplete message (split header or split body). Slide the
             * leftover to the front of plain_buf so the next record's
             * plaintext appends right after it, then drop into the
             * record-read path below and loop back to retry extraction.
             */
            if (hs->plain_offset > 0) {
                memmove(hs->plain_buf,
                        hs->plain_buf + hs->plain_offset, remaining);
            }
            hs->plain_len = remaining;
            hs->plain_offset = 0;
        }

        /* Plaintext buffer empty — read the next record from the network */
        if (*recv_len < 5) {
            return kTLS13_WantRead;
        }

        record_type = recv_buf[0];
        record_len = get_u16(recv_buf + 3);
        total = (size_t)5 + record_len;

        if (*recv_len < total) {
            return kTLS13_WantRead;
        }

        /*
         * Decrypt writes (record_len - tag) bytes into plain_buf, so the
         * record must fit. A compliant server's TLSCiphertext fragment is
         * <= 2^14 + 256 (= sizeof plain_buf); reject anything larger
         * rather than overflowing the buffer.
         */
        if ((size_t)record_len > sizeof(hs->plain_buf)) {
            hs->error = BR_ERR_BAD_PARAM;
            return kTLS13_Error;
        }

        /* Skip CCS records (middlebox compatibility) */
        if (record_type == TLS13_CT_CHANGE_CIPHER_SPEC) {
            memmove(recv_buf, recv_buf + total, *recv_len - total);
            *recv_len -= total;
            continue;
        }

        /*
         * TLS 1.3 encrypted records have outer type 0x17 (application_data).
         * Handshake records sent in the clear (type 22) should not appear
         * after ServerHello.
         */
        if (record_type != TLS13_CT_APPLICATION_DATA) {
            hs->error = BR_ERR_UNEXPECTED;
            return kTLS13_Error;
        }

        /*
         * Decrypt the record into the plaintext buffer. The full
         * decrypted payload may contain multiple handshake messages;
         * we consume them one at a time via the loop above.
         */
        {
            size_t pt_len = 0;
            /*
             * fixes370 (#167) — APPEND the decrypted plaintext after any
             * leftover partial handshake message (plain_offset is 0 here;
             * the extraction block compacted the leftover to the front and
             * set plain_len to its size, or plain_len is 0 for a fresh
             * buffer). Bound the append so multi-record reassembly can
             * never overflow plain_buf.
             */
            if ((size_t)hs->plain_len + (size_t)record_len >
                sizeof(hs->plain_buf)) {
                hs->error = BR_ERR_BAD_PARAM;
                return kTLS13_Error;
            }
            ret = tls13_record_decrypt(&hs->read_ctx,
                                       recv_buf + 5, record_len,
                                       hs->plain_buf + hs->plain_len,
                                       &pt_len, &inner_ct);
            if (ret != 0) {
                hs->error = BR_ERR_BAD_MAC;
                return kTLS13_Error;
            }
            hs->plain_len += pt_len;
        }

        /* Consume the record from the front of the recv buffer */
        memmove(recv_buf, recv_buf + total, *recv_len - total);
        *recv_len -= total;

        /* The inner content type must be handshake (22) */
        if (inner_ct != TLS13_CT_HANDSHAKE) {
            hs->error = BR_ERR_UNEXPECTED;
            return kTLS13_Error;
        }

        /* Loop back to the top to extract the first handshake message */
    }
}

/*
 * Handle the kTLS13_RecvEncryptedExtensions state.
 *
 * Parse the EncryptedExtensions message. For our client, we silently
 * ignore all extensions (including early_data if present).
 *
 * Format:
 *   HandshakeType = 8 (encrypted_extensions)
 *   uint24 length
 *   Extension extensions<0..2^16-1>
 */
static tls13_hs_result tls13_state_recv_encrypted_extensions(
    tls13_hs_ctx *hs, unsigned char *recv_buf, size_t *recv_len)
{
    uint8_t hs_type;
    size_t msg_len;
    uint32_t body_len;
    tls13_hs_result r;

    r = tls13_read_encrypted_hs(hs, recv_buf, recv_len, &hs_type,
                                hs->msg_buf, &msg_len);
    if (r != kTLS13_OK) return r;

    /* Must be EncryptedExtensions (type 8) */
    if (hs_type != TLS13_HT_ENCRYPTED_EXTENSIONS) {
        hs->error = BR_ERR_UNEXPECTED;
        return kTLS13_Error;
    }

    /* Validate length field */
    body_len = get_u24(hs->msg_buf + 1);
    if (4 + body_len > msg_len) {
        hs->error = BR_ERR_BAD_PARAM;
        return kTLS13_Error;
    }

    /*
     * We silently ignore all extensions in EncryptedExtensions.
     * Per the spec, we don't support early data or any extensions
     * that would require processing here.
     *
     * Just verify the extensions list length field is consistent.
     */
    if (body_len >= 2) {
        uint16_t ext_list_len = get_u16(hs->msg_buf + 4);
        if (ext_list_len + 2 > body_len) {
            hs->error = BR_ERR_BAD_PARAM;
            return kTLS13_Error;
        }
    }

    /* Update transcript hash with the full handshake message */
    tls13_transcript_update(&hs->transcript, hs->msg_buf, 4 + body_len);

    /* macTLS#2 Stage D: on an accepted resumption the server sends no
     * Certificate / CertificateVerify — EncryptedExtensions is followed
     * directly by the server Finished. Otherwise take the normal
     * Certificate path. */
    hs->state = hs->resumption_accepted ? kTLS13_RecvFinished
                                        : kTLS13_RecvCertRequestOrCert;
    return kTLS13_OK;
}

/*
 * Handle the kTLS13_RecvCertRequestOrCert state.
 *
 * Peek at the handshake message type:
 * - If type 13 (CertificateRequest): parse and skip (we don't do
 *   client auth), set cert_request_received, advance to RecvCertificate
 * - If type 11 (Certificate): don't consume, fall through to certificate
 *   handler
 */
static tls13_hs_result tls13_state_recv_cert_request_or_cert(
    tls13_hs_ctx *hs, unsigned char *recv_buf, size_t *recv_len)
{
    uint8_t hs_type;
    size_t msg_len;
    uint32_t body_len;
    tls13_hs_result r;

    r = tls13_read_encrypted_hs(hs, recv_buf, recv_len, &hs_type,
                                hs->msg_buf, &msg_len);
    if (r != kTLS13_OK) return r;

    if (hs_type == TLS13_HT_CERTIFICATE_REQUEST) {
        /*
         * CertificateRequest received. We don't support client
         * certificates, so we just note the request and skip it.
         * We'll send an empty Certificate message later (Task 7).
         */
        body_len = get_u24(hs->msg_buf + 1);
        if (4 + body_len > msg_len) {
            hs->error = BR_ERR_BAD_PARAM;
            return kTLS13_Error;
        }

        hs->cert_request_received = 1;

        /* Update transcript with the CertificateRequest message */
        tls13_transcript_update(&hs->transcript, hs->msg_buf, 4 + body_len);

        hs->state = kTLS13_RecvCertificate;
        return kTLS13_OK;
    }

    if (hs_type == TLS13_HT_CERTIFICATE) {
        /*
         * No CertificateRequest — this is the Certificate directly.
         * The message is already in msg_buf; we set state to
         * RecvCertificate and process it there. We store the data
         * so the certificate handler can use it.
         *
         * CRITICAL: We stash the length in a SEPARATE field, NOT in
         * hs->msg_len. hs->msg_len is the OUTGOING send queue length,
         * and setting it here would make the pump try to send the
         * received Certificate back to the server as if it were our
         * own data. That breaks the connection catastrophically.
         */
        hs->pending_recv_msg_len = msg_len;
        hs->state = kTLS13_RecvCertificate;
        return kTLS13_OK;
    }

    /* Unexpected message type */
    hs->error = BR_ERR_UNEXPECTED;
    return kTLS13_Error;
}

/*
 * Deep-copy a br_x509_pkey into our context, using server_pkey_data
 * as backing storage for the pointer fields.
 */
static void tls13_copy_pkey(tls13_hs_ctx *hs, const br_x509_pkey *src)
{
    hs->server_pkey.key_type = src->key_type;
    if (src->key_type == BR_KEYTYPE_RSA) {
        size_t off = 0;
        memcpy(hs->server_pkey_data + off, src->key.rsa.n, src->key.rsa.nlen);
        hs->server_pkey.key.rsa.n = hs->server_pkey_data + off;
        hs->server_pkey.key.rsa.nlen = src->key.rsa.nlen;
        off += src->key.rsa.nlen;
        memcpy(hs->server_pkey_data + off, src->key.rsa.e, src->key.rsa.elen);
        hs->server_pkey.key.rsa.e = hs->server_pkey_data + off;
        hs->server_pkey.key.rsa.elen = src->key.rsa.elen;
    } else if (src->key_type == BR_KEYTYPE_EC) {
        memcpy(hs->server_pkey_data, src->key.ec.q, src->key.ec.qlen);
        hs->server_pkey.key.ec.curve = src->key.ec.curve;
        hs->server_pkey.key.ec.q = hs->server_pkey_data;
        hs->server_pkey.key.ec.qlen = src->key.ec.qlen;
    }
}

/*
 * Handle the kTLS13_RecvCertificate state.
 *
 * TLS 1.3 Certificate message format:
 *   HandshakeType = 11 (certificate)
 *   uint24 length
 *   opaque certificate_request_context<0..2^8-1>  (empty for server certs)
 *   CertificateEntry certificate_list<0..2^24-1>
 *
 * Each CertificateEntry:
 *   opaque cert_data<1..2^24-1>  (DER-encoded X.509)
 *   Extension extensions<0..2^16-1>  (per-cert extensions, ignore)
 *
 * Certificates are streamed through BearSSL's X.509 validator to avoid
 * buffering the entire chain. The validated public key is deep-copied
 * into hs->server_pkey for use in CertificateVerify.
 */
static tls13_hs_result tls13_state_recv_certificate(
    tls13_hs_ctx *hs, unsigned char *recv_buf, size_t *recv_len,
    const char *hostname)
{
    uint8_t hs_type;
    size_t msg_len;
    uint32_t body_len;
    size_t pos;
    uint8_t ctx_len;
    uint32_t cert_list_len;
    size_t cert_list_end;
    const br_x509_class *xc;
    unsigned x509_err;
    const br_x509_pkey *pkey;
    tls13_hs_result r;

    /*
     * If pending_recv_msg_len was pre-loaded by the CertRequestOrCert
     * handler, use that. Otherwise read a new encrypted message.
     */
    if (hs->pending_recv_msg_len > 0) {
        msg_len = hs->pending_recv_msg_len;
        hs->pending_recv_msg_len = 0;
        hs_type = hs->msg_buf[0];
    } else {
        r = tls13_read_encrypted_hs(hs, recv_buf, recv_len, &hs_type,
                                    hs->msg_buf, &msg_len);
        if (r != kTLS13_OK) return r;
    }

    if (hs_type != TLS13_HT_CERTIFICATE) {
        hs->error = BR_ERR_UNEXPECTED;
        return kTLS13_Error;
    }

    body_len = get_u24(hs->msg_buf + 1);
    if (4 + body_len > msg_len) {
        hs->error = BR_ERR_BAD_PARAM;
        return kTLS13_Error;
    }

    pos = 4; /* past handshake header */

    /* certificate_request_context: should be empty for server certs */
    if (pos >= 4 + body_len) {
        hs->error = BR_ERR_BAD_PARAM;
        return kTLS13_Error;
    }
    ctx_len = hs->msg_buf[pos];
    pos += 1 + ctx_len;

    /* certificate_list length (uint24) */
    if (pos + 3 > 4 + body_len) {
        hs->error = BR_ERR_BAD_PARAM;
        return kTLS13_Error;
    }
    cert_list_len = get_u24(hs->msg_buf + pos);
    pos += 3;
    cert_list_end = pos + cert_list_len;

    if (cert_list_end > 4 + body_len) {
        hs->error = BR_ERR_BAD_PARAM;
        return kTLS13_Error;
    }

    /* Empty certificate list = server sent no certs */
    if (cert_list_len == 0) {
        hs->error = BR_ERR_X509_NOT_TRUSTED;
        return kTLS13_Error;
    }

    /*
     * Stream certificates through the X.509 validator.
     */
    xc = *hs->x509_ctx;
    xc->start_chain(hs->x509_ctx, hostname);

    while (pos < cert_list_end) {
        uint32_t cert_data_len;
        uint16_t cert_ext_len;

        /* cert_data<1..2^24-1> */
        if (pos + 3 > cert_list_end) {
            hs->error = BR_ERR_BAD_PARAM;
            return kTLS13_Error;
        }
        cert_data_len = get_u24(hs->msg_buf + pos);
        pos += 3;

        if (pos + cert_data_len > cert_list_end) {
            hs->error = BR_ERR_BAD_PARAM;
            return kTLS13_Error;
        }

        /* Feed this certificate through the X.509 validator */
        xc->start_cert(hs->x509_ctx, cert_data_len);
        xc->append(hs->x509_ctx, hs->msg_buf + pos, cert_data_len);
        xc->end_cert(hs->x509_ctx);

        pos += cert_data_len;

        /* Per-certificate extensions<0..2^16-1> — skip them */
        if (pos + 2 > cert_list_end) {
            hs->error = BR_ERR_BAD_PARAM;
            return kTLS13_Error;
        }
        cert_ext_len = get_u16(hs->msg_buf + pos);
        pos += 2 + cert_ext_len;
    }

    /* Finish the certificate chain validation */
    x509_err = xc->end_chain(hs->x509_ctx);
    if (x509_err != 0) {
        hs->error = (int)x509_err;
        return kTLS13_Error;
    }

    /* Extract and deep-copy the server's public key */
    pkey = xc->get_pkey(hs->x509_ctx, NULL);
    if (pkey == NULL) {
        hs->error = BR_ERR_X509_NOT_TRUSTED;
        return kTLS13_Error;
    }
    tls13_copy_pkey(hs, pkey);

    /* Update transcript with the full Certificate message */
    tls13_transcript_update(&hs->transcript, hs->msg_buf, 4 + body_len);

    hs->state = kTLS13_RecvCertificateVerify;
    return kTLS13_OK;
}

/*
 * TLS 1.3 CertificateVerify content construction.
 *
 * The signed content for CertificateVerify is NOT just the transcript hash.
 * It's a constructed message:
 *   0x20 repeated 64 times (space padding)
 *   "TLS 1.3, server CertificateVerify" (33 bytes, ASCII)
 *   0x00 (separator)
 *   transcript_hash (32 or 48 bytes)
 *
 * Total: 64 + 33 + 1 + hash_len = 98 + hash_len bytes.
 */
static void tls13_build_verify_content(const unsigned char *transcript_hash,
                                       size_t hash_len,
                                       unsigned char *out,
                                       size_t *out_len)
{
    static const char context_string[] = "TLS 1.3, server CertificateVerify";
    size_t pos = 0;

    /* 64 bytes of 0x20 (space) */
    memset(out, 0x20, 64);
    pos = 64;

    /* Context string (33 bytes, no NUL terminator in the wire format) */
    memcpy(out + pos, context_string, 33);
    pos += 33;

    /* Separator byte */
    out[pos++] = 0x00;

    /* Transcript hash */
    memcpy(out + pos, transcript_hash, hash_len);
    pos += hash_len;

    *out_len = pos;
}

/*
 * Constant-time comparison of two buffers.
 * Returns 0 if equal, non-zero if different.
 */
static int tls13_ct_memcmp(const void *a, const void *b, size_t len)
{
    const unsigned char *pa = (const unsigned char *)a;
    const unsigned char *pb = (const unsigned char *)b;
    unsigned char acc = 0;
    size_t i;
    for (i = 0; i < len; i++) {
        acc |= pa[i] ^ pb[i];
    }
    return acc;
}

/*
 * Handle the kTLS13_RecvCertificateVerify state.
 *
 * The server signs the transcript hash to prove it owns the certificate's
 * private key.
 *
 * CertificateVerify format:
 *   HandshakeType = 15 (certificate_verify)
 *   uint24 length
 *   SignatureScheme algorithm (2 bytes)
 *   opaque signature<0..2^16-1>
 *
 * The signed content is a constructed message containing the transcript
 * hash, NOT the raw hash.
 */
static tls13_hs_result tls13_state_recv_certificate_verify(
    tls13_hs_ctx *hs, unsigned char *recv_buf, size_t *recv_len)
{
    uint8_t hs_type;
    size_t msg_len;
    uint32_t body_len;
    uint16_t sig_scheme;
    uint16_t sig_len;
    const unsigned char *sig_data;
    unsigned char transcript_hash[64];
    unsigned char verify_content[64 + 33 + 1 + 64]; /* max 162 bytes */
    size_t verify_content_len;
    unsigned char content_hash[64];
    tls13_hs_result r;

    r = tls13_read_encrypted_hs(hs, recv_buf, recv_len, &hs_type,
                                hs->msg_buf, &msg_len);
    if (r != kTLS13_OK) return r;

    if (hs_type != TLS13_HT_CERTIFICATE_VERIFY) {
        hs->error = BR_ERR_UNEXPECTED;
        return kTLS13_Error;
    }

    body_len = get_u24(hs->msg_buf + 1);
    if (4 + body_len > msg_len || body_len < 4) {
        hs->error = BR_ERR_BAD_PARAM;
        return kTLS13_Error;
    }

    /* Parse signature scheme and signature */
    sig_scheme = get_u16(hs->msg_buf + 4);
    sig_len = get_u16(hs->msg_buf + 6);
    sig_data = hs->msg_buf + 8;

    if (sig_len + 4 > body_len) {
        hs->error = BR_ERR_BAD_PARAM;
        return kTLS13_Error;
    }

    /*
     * Snapshot the transcript hash BEFORE including this message.
     * CertificateVerify signs the transcript up to but not including
     * the CertificateVerify itself.
     */
    tls13_transcript_snapshot(&hs->transcript, transcript_hash);

    /* Build the verify content (the data that was signed) */
    tls13_build_verify_content(transcript_hash, hs->transcript.hash_len,
                               verify_content, &verify_content_len);

    /*
     * Verify the signature based on the signature scheme.
     */
    switch (sig_scheme) {
    case TLS13_SIG_RSA_PSS_RSAE_SHA256:
    case TLS13_SIG_RSA_PSS_RSAE_SHA384: {
        /*
         * RSA-PSS verification.
         * We hash the verify content ourselves, then pass the hash
         * to the RSA-PSS verify function.
         */
        const br_hash_class *sig_hash;
        size_t sig_hash_len;
        br_rsa_pss_vrfy pss_vrfy;
        uint32_t ok;

        if (hs->server_pkey.key_type != BR_KEYTYPE_RSA) {
            hs->error = BR_ERR_BAD_PARAM;
            return kTLS13_Error;
        }

        if (sig_scheme == TLS13_SIG_RSA_PSS_RSAE_SHA256) {
            sig_hash = &br_sha256_vtable;
            sig_hash_len = 32;
        } else {
            sig_hash = &br_sha384_vtable;
            sig_hash_len = 48;
        }

        /* Hash the verify content */
        if (sig_hash == &br_sha256_vtable) {
            br_sha256_context hc;
            br_sha256_init(&hc);
            br_sha256_update(&hc, verify_content, verify_content_len);
            br_sha256_out(&hc, content_hash);
        } else {
            br_sha384_context hc;
            br_sha384_init(&hc);
            br_sha384_update(&hc, verify_content, verify_content_len);
            br_sha384_out(&hc, content_hash);
        }

        pss_vrfy = br_rsa_pss_vrfy_get_default();
        ok = pss_vrfy(sig_data, sig_len,
                      sig_hash, sig_hash, content_hash,
                      sig_hash_len, &hs->server_pkey.key.rsa);
        if (ok != 1) {
            hs->error = BR_ERR_BAD_SIGNATURE;
            return kTLS13_Error;
        }
        break;
    }

    case TLS13_SIG_ECDSA_SECP256R1_SHA256:
    case TLS13_SIG_ECDSA_SECP384R1_SHA384: {
        /*
         * ECDSA verification.
         * Hash the verify content, then verify with the EC public key.
         * TLS 1.3 uses ASN.1 DER-encoded ECDSA signatures.
         */
        const br_hash_class *sig_hash;
        size_t sig_hash_len;
        br_ecdsa_vrfy ecdsa_vrfy;
        const br_ec_impl *ec;
        uint32_t ok;

        if (hs->server_pkey.key_type != BR_KEYTYPE_EC) {
            hs->error = BR_ERR_BAD_PARAM;
            return kTLS13_Error;
        }

        if (sig_scheme == TLS13_SIG_ECDSA_SECP256R1_SHA256) {
            sig_hash = &br_sha256_vtable;
            sig_hash_len = 32;
        } else {
            sig_hash = &br_sha384_vtable;
            sig_hash_len = 48;
        }

        /* Hash the verify content */
        if (sig_hash == &br_sha256_vtable) {
            br_sha256_context hc;
            br_sha256_init(&hc);
            br_sha256_update(&hc, verify_content, verify_content_len);
            br_sha256_out(&hc, content_hash);
        } else {
            br_sha384_context hc;
            br_sha384_init(&hc);
            br_sha384_update(&hc, verify_content, verify_content_len);
            br_sha384_out(&hc, content_hash);
        }

        ec = br_ec_get_default();
        ecdsa_vrfy = br_ecdsa_vrfy_asn1_get_default();
        ok = ecdsa_vrfy(ec, content_hash, sig_hash_len,
                        &hs->server_pkey.key.ec, sig_data, sig_len);
        if (ok != 1) {
            hs->error = BR_ERR_BAD_SIGNATURE;
            return kTLS13_Error;
        }
        break;
    }

    default:
        /* Unsupported signature scheme */
        hs->error = BR_ERR_BAD_PARAM;
        return kTLS13_Error;
    }

    /* Wipe sensitive intermediates */
    secure_wipe(transcript_hash, sizeof(transcript_hash));
    secure_wipe(verify_content, sizeof(verify_content));
    secure_wipe(content_hash, sizeof(content_hash));

    /* Update transcript with the CertificateVerify message */
    tls13_transcript_update(&hs->transcript, hs->msg_buf, 4 + body_len);

    hs->state = kTLS13_RecvFinished;
    return kTLS13_OK;
}

/*
 * Handle the kTLS13_RecvFinished state.
 *
 * The server sends a Finished message containing an HMAC of the transcript.
 *
 * Finished format:
 *   HandshakeType = 20 (finished)
 *   uint24 length
 *   opaque verify_data[Hash.length]
 *
 * Verification:
 * 1. Derive finished_key from server handshake traffic secret
 * 2. Compute expected verify_data = HMAC(finished_key, transcript_hash)
 *    where transcript_hash is everything up to but NOT including this Finished
 * 3. Constant-time compare with received verify_data
 * 4. On success, derive application keys from the transcript hash that
 *    now includes this Finished message
 */
static tls13_hs_result tls13_state_recv_finished(
    tls13_hs_ctx *hs, unsigned char *recv_buf, size_t *recv_len)
{
    uint8_t hs_type;
    size_t msg_len;
    uint32_t body_len;
    unsigned char finished_key[64];
    unsigned char transcript_hash[64];
    unsigned char expected_verify[64];
    br_hmac_key_context hmac_kc;
    br_hmac_context hmac_ctx;
    size_t hash_len;
    unsigned char app_transcript_hash[64];
    unsigned char client_key[32], client_iv[12];
    unsigned char server_key[32], server_iv[12];
    size_t key_len;
    tls13_hs_result r;

    r = tls13_read_encrypted_hs(hs, recv_buf, recv_len, &hs_type,
                                hs->msg_buf, &msg_len);
    if (r != kTLS13_OK) return r;

    if (hs_type != TLS13_HT_FINISHED) {
        hs->error = BR_ERR_UNEXPECTED;
        return kTLS13_Error;
    }

    body_len = get_u24(hs->msg_buf + 1);
    hash_len = hs->transcript.hash_len;

    if (4 + body_len > msg_len) {
        hs->error = BR_ERR_BAD_PARAM;
        return kTLS13_Error;
    }

    /* verify_data must be exactly Hash.length */
    if (body_len != hash_len) {
        hs->error = BR_ERR_BAD_PARAM;
        return kTLS13_Error;
    }

    /*
     * Step 1: Derive the finished key from the server handshake secret.
     */
    tls13_ks_derive_finished_key(&hs->ks, hs->server_hs_secret,
                                 finished_key);

    /*
     * Step 2: Snapshot the transcript hash BEFORE including Finished.
     * The Finished verify_data is HMAC(finished_key, transcript_hash)
     * where transcript_hash includes everything up to but NOT including
     * this Finished message.
     */
    tls13_transcript_snapshot(&hs->transcript, transcript_hash);

    /*
     * Step 3: Compute expected verify_data.
     */
    br_hmac_key_init(&hmac_kc, hs->transcript.hash, finished_key, hash_len);
    br_hmac_init(&hmac_ctx, &hmac_kc, 0);
    br_hmac_update(&hmac_ctx, transcript_hash, hash_len);
    br_hmac_out(&hmac_ctx, expected_verify);

    /*
     * Step 4: Constant-time comparison of received vs expected verify_data.
     */
    if (tls13_ct_memcmp(hs->msg_buf + 4, expected_verify, hash_len) != 0) {
        secure_wipe(finished_key, sizeof(finished_key));
        secure_wipe(transcript_hash, sizeof(transcript_hash));
        secure_wipe(expected_verify, sizeof(expected_verify));
        hs->error = BR_ERR_BAD_FINISHED;
        return kTLS13_Error;
    }

    /*
     * Step 5: Update transcript with the Finished message, then
     * derive application traffic keys.
     */
    tls13_transcript_update(&hs->transcript, hs->msg_buf, 4 + body_len);

    /* Snapshot transcript hash including server Finished */
    tls13_transcript_snapshot(&hs->transcript, app_transcript_hash);

    /* Extract Master Secret */
    tls13_ks_extract_master(&hs->ks);

    /* Derive application traffic keys */
    key_len = tls13_key_len_for_suite(hs->cipher_suite);
    tls13_ks_derive_app_keys(&hs->ks, app_transcript_hash,
                             key_len,
                             client_key, client_iv,
                             server_key, server_iv);

    /*
     * Install server application keys for reading.
     * Client application keys will be installed after we send
     * our own Finished message.
     */
    tls13_record_init(&hs->read_ctx,
                      server_key, key_len, server_iv, hs->cipher_suite);

    /*
     * Pre-derive the client finished key BEFORE overwriting
     * client_hs_secret with the app keys. The client Finished
     * message needs HMAC(finished_key, transcript_hash) where
     * finished_key is derived from the client handshake secret.
     * We stash the finished key in server_hs_secret (which is
     * no longer needed — the server's finished key was a local
     * variable).
     */
    {
        unsigned char client_finished_key[64];
        tls13_ks_derive_finished_key(&hs->ks, hs->client_hs_secret,
                                     client_finished_key);
        memcpy(hs->server_hs_secret, client_finished_key,
               hs->transcript.hash_len);
        secure_wipe(client_finished_key, sizeof(client_finished_key));
    }

    /*
     * Save client app keys temporarily in client_hs_secret area
     * (the original handshake secret is no longer needed — we just
     * derived the finished key above). SendFinished will use these
     * to set up write_ctx after sending client Finished.
     * Layout: [key (up to 32 bytes)] [iv (12 bytes)].
     */
    memcpy(hs->client_hs_secret, client_key, key_len);
    memcpy(hs->client_hs_secret + key_len, client_iv, 12);

    /* Wipe all intermediate key material */
    secure_wipe(finished_key, sizeof(finished_key));
    secure_wipe(transcript_hash, sizeof(transcript_hash));
    secure_wipe(expected_verify, sizeof(expected_verify));
    secure_wipe(app_transcript_hash, sizeof(app_transcript_hash));
    secure_wipe(client_key, sizeof(client_key));
    secure_wipe(client_iv, sizeof(client_iv));
    secure_wipe(server_key, sizeof(server_key));
    secure_wipe(server_iv, sizeof(server_iv));

    hs->state = kTLS13_SendFinished;
    return kTLS13_OK;
}

/* ── Client Finished + Application Key Switch ── */

/*
 * Handle the kTLS13_SendFinished state.
 *
 * Sends our Finished message encrypted with the client handshake keys,
 * then switches write_ctx to application keys.
 *
 * Steps:
 * 1. Derive client finished_key from client handshake traffic secret
 * 2. Snapshot transcript hash (includes everything through server Finished)
 * 3. Compute verify_data = HMAC(finished_key, transcript_hash)
 * 4. Build the Finished handshake message (type 20 + uint24 length + verify_data)
 * 5. Encrypt it with client handshake keys
 * 6. Write the encrypted record (with 5-byte TLS record header) to BearSSL's sendrec buffer
 * 7. Update transcript with the unencrypted Finished message
 * 8. Switch write_ctx to application keys
 * 9. Advance state to kTLS13_Complete
 *
 * Note: The client application keys were stashed in client_hs_secret
 * by the RecvFinished handler (the handshake secret is no longer needed
 * at that point). Layout: [key (key_len bytes)] [iv (12 bytes)].
 */
static tls13_hs_result tls13_state_send_finished(tls13_hs_ctx *hs)
{
    unsigned char finished_key[64];
    unsigned char transcript_hash[64];
    unsigned char verify_data[64];
    br_hmac_key_context hmac_kc;
    br_hmac_context hmac_ctx;
    size_t hash_len;
    size_t key_len;
    unsigned char hs_msg[4 + 64]; /* type(1) + uint24 len(3) + verify_data(up to 64) */
    size_t hs_msg_len;
    unsigned char ciphertext[4 + 64 + 1 + TLS13_TAG_SIZE]; /* encrypted payload */
    size_t ct_len;
    int ret;

    hash_len = hs->transcript.hash_len;
    key_len = tls13_key_len_for_suite(hs->cipher_suite);

    /*
     * Step 1: Retrieve the client finished_key.
     *
     * RecvFinished pre-derived this from the client handshake secret
     * and stashed it in server_hs_secret[0..hash_len-1], because
     * client_hs_secret was about to be overwritten with the application
     * key material.
     */
    memcpy(finished_key, hs->server_hs_secret, hash_len);

    /*
     * Step 2: Snapshot the transcript hash.
     * This hash includes everything through the server's Finished message.
     */
    tls13_transcript_snapshot(&hs->transcript, transcript_hash);

    /*
     * Step 3: Compute verify_data = HMAC(finished_key, transcript_hash).
     */
    br_hmac_key_init(&hmac_kc, hs->transcript.hash, finished_key, hash_len);
    br_hmac_init(&hmac_ctx, &hmac_kc, 0);
    br_hmac_update(&hmac_ctx, transcript_hash, hash_len);
    br_hmac_out(&hmac_ctx, verify_data);

    /*
     * Step 4: Build the Finished handshake message.
     * Format: type(1) + uint24_length(3) + verify_data(hash_len)
     */
    hs_msg[0] = TLS13_HT_FINISHED;      /* type = 20 */
    put_u24(hs_msg + 1, (uint32_t)hash_len);
    memcpy(hs_msg + 4, verify_data, hash_len);
    hs_msg_len = 4 + hash_len;

    /*
     * Step 5: Encrypt the Finished message using the client handshake keys.
     * The write_ctx still has the client handshake keys at this point.
     */
    ret = tls13_record_encrypt(&hs->write_ctx,
                               hs_msg, hs_msg_len,
                               TLS13_CT_HANDSHAKE,
                               ciphertext, &ct_len);
    if (ret != 0) {
        hs->error = BR_ERR_BAD_STATE;
        secure_wipe(finished_key, sizeof(finished_key));
        secure_wipe(transcript_hash, sizeof(transcript_hash));
        secure_wipe(verify_data, sizeof(verify_data));
        return kTLS13_Error;
    }

    /*
     * Step 6: Write the encrypted record directly into hs->msg_buf.
     * Format: content_type(0x17) + version(0x0303) + length(2) + ciphertext
     *
     * The pump loop will drain hs->msg_buf to OT transport.
     */
    if (5 + ct_len > sizeof(hs->msg_buf)) {
        secure_wipe(finished_key, sizeof(finished_key));
        secure_wipe(transcript_hash, sizeof(transcript_hash));
        secure_wipe(verify_data, sizeof(verify_data));
        hs->error = BR_ERR_TOO_LARGE;
        return kTLS13_Error;
    }

    /* Build the TLS record: outer type 0x17 (application_data), version 0x0303 */
    hs->msg_buf[0] = TLS13_CT_APPLICATION_DATA;  /* 0x17 */
    hs->msg_buf[1] = 0x03;
    hs->msg_buf[2] = 0x03;
    put_u16(hs->msg_buf + 3, (uint16_t)ct_len);
    memcpy(hs->msg_buf + 5, ciphertext, ct_len);
    hs->msg_len = 5 + ct_len;
    hs->msg_offset = 0;

    /*
     * Step 7: Update transcript with the unencrypted Finished message.
     * The transcript includes the handshake message bytes (type + length
     * + verify_data), NOT the TLS record header or encryption.
     */
    tls13_transcript_update(&hs->transcript, hs_msg, hs_msg_len);

    /*
     * Step 7.5 (macTLS#2): derive the resumption_master_secret now that
     * the transcript includes the client Finished. hs->ks.secret still
     * holds the Master Secret (extract_master ran in RecvFinished and
     * nothing has overwritten it), so Derive-Secret(Master, "res master",
     * Hash(..client Finished)) is valid here. Reuses the transcript_hash
     * scratch (re-snapshotted to include the Finished just added; it is
     * wiped with the rest below). Kept for the connection lifetime so any
     * NewSessionTicket can mint its PSK.
     */
    tls13_transcript_snapshot(&hs->transcript, transcript_hash);
    tls13_ks_derive_resumption_master(&hs->ks, transcript_hash,
                                      hs->res_master);
    hs->res_master_valid = 1;

    /*
     * Step 8: Switch write_ctx to application keys.
     * The client application keys were stashed in client_hs_secret by
     * RecvFinished: [key (key_len bytes)] [iv (12 bytes)].
     */
    tls13_record_init(&hs->write_ctx,
                      hs->client_hs_secret, key_len,
                      hs->client_hs_secret + key_len,
                      hs->cipher_suite);

    /* Wipe all sensitive intermediates */
    secure_wipe(finished_key, sizeof(finished_key));
    secure_wipe(transcript_hash, sizeof(transcript_hash));
    secure_wipe(verify_data, sizeof(verify_data));
    secure_wipe(hs->client_hs_secret, sizeof(hs->client_hs_secret));
    secure_wipe(hs->server_hs_secret, sizeof(hs->server_hs_secret));

    /*
     * Step 9: Advance state to Complete.
     * The caller must still drain msg_buf to the network.
     */
    hs->state = kTLS13_Complete;
    return kTLS13_WantWrite;
}

/* ── Post-Handshake Message Handling ── */

/*
 * Handle post-handshake messages received after the handshake completes.
 *
 * After the TLS 1.3 handshake finishes, the server may send encrypted
 * handshake messages during the application data phase:
 *
 *   - NewSessionTicket (type 4): Session resumption data. We don't
 *     support session resumption, so these are silently discarded.
 *
 *   - KeyUpdate (type 24): Requests re-keying. We don't support
 *     post-handshake key updates, so we treat this as a fatal error
 *     (connection close). In practice, most servers don't send KeyUpdate
 *     within the first few requests.
 *
 * Parameters:
 *   data     — decrypted record payload (inner content type already stripped)
 *   data_len — length of the decrypted payload
 *
 * Returns:
 *   kTLS13_OK    — message handled (e.g., NewSessionTicket discarded)
 *   kTLS13_Error — unrecoverable error (e.g., KeyUpdate or unknown type)
 */
/*
 * Parse a NewSessionTicket (RFC 8446 4.6.1), header included:
 *   uint32 ticket_lifetime; uint32 ticket_age_add;
 *   opaque ticket_nonce<0..255>; opaque ticket<1..2^16-1>;
 *   Extension extensions<0..2^16-2>;
 * Fills `out` with a ready-to-reuse ticket (PSK already minted from
 * res_master + nonce). Bounds-checked against the message's own declared
 * length so trailing bytes / coalesced messages can't be over-read.
 * Returns 0 on success, -1 on malformed / oversize / no res_master.
 * macTLS#2 Stage B.
 */
int tls13_parse_new_session_ticket(tls13_hs_ctx *hs,
                                    const unsigned char *msg, size_t msg_len,
                                    tls13_session_ticket *out)
{
    size_t off, lim;
    uint32_t body_len;
    uint8_t nonce_len;
    const unsigned char *nonce;
    uint16_t tlen;
    size_t hash_len;

    if (!hs->res_master_valid) return -1;     /* nothing to mint the PSK from */
    if (msg_len < 4) return -1;
    if (msg[0] != TLS13_HT_NEW_SESSION_TICKET) return -1;

    body_len = get_u24(msg + 1);
    lim = (size_t)body_len + 4;               /* end of this message's body */
    if (lim > msg_len) return -1;             /* truncated */

    off = 4;
    if (off + 8 > lim) return -1;
    out->lifetime = ((uint32_t)msg[off] << 24) | ((uint32_t)msg[off + 1] << 16) |
                    ((uint32_t)msg[off + 2] << 8) | (uint32_t)msg[off + 3];
    off += 4;
    out->age_add = ((uint32_t)msg[off] << 24) | ((uint32_t)msg[off + 1] << 16) |
                   ((uint32_t)msg[off + 2] << 8) | (uint32_t)msg[off + 3];
    off += 4;

    /* ticket_nonce<0..255> */
    if (off + 1 > lim) return -1;
    nonce_len = msg[off];
    off += 1;
    if (off + nonce_len > lim) return -1;
    nonce = msg + off;
    off += nonce_len;

    /* ticket<1..2^16-1> */
    if (off + 2 > lim) return -1;
    tlen = get_u16(msg + off);
    off += 2;
    if (tlen == 0 || (size_t)off + tlen > lim) return -1;
    if (tlen > TLS13_MAX_TICKET_LEN) return -1;   /* too big to cache; skip */
    memcpy(out->ticket, msg + off, tlen);
    out->ticket_len = tlen;
    /* extensions<0..2^16-2> follow but are ignored (no 0-RTT in v1). */

    hash_len = hs->ks.hash_len;               /* 32 (SHA-256) or 48 (SHA-384) */
    out->cipher_suite = hs->cipher_suite;
    out->psk_len = hash_len;
    tls13_ks_derive_resumption_psk(&hs->ks, hs->res_master,
                                   nonce, nonce_len, out->psk);
    out->valid = 1;
    return 0;
}

tls13_hs_result tls13_handle_post_handshake(tls13_hs_ctx *hs,
                                            const unsigned char *data,
                                            size_t data_len)
{
    uint8_t msg_type;

    if (data_len < 1) {
        hs->error = BR_ERR_BAD_PARAM;
        return kTLS13_Error;
    }

    msg_type = data[0];

    switch (msg_type) {
    case TLS13_HT_NEW_SESSION_TICKET:
        /*
         * NewSessionTicket (macTLS#2) — parse and stash for the cache
         * layer to harvest (it pairs this with the host and stamps the
         * arrival time). A parse failure (malformed / oversize ticket /
         * no res_master) is non-fatal: we simply won't be able to resume,
         * and fall back to a full handshake next time. The server sends
         * these speculatively, so ignoring a bad one is always valid.
         */
        if (tls13_parse_new_session_ticket(hs, data, data_len,
                                           &hs->ticket) == 0) {
            hs->ticket_valid = 1;
        }
        return kTLS13_OK;

    case TLS13_HT_KEY_UPDATE:
        /*
         * KeyUpdate — treat as connection close.
         * Supporting key updates would require re-deriving traffic keys
         * mid-connection. For a simple REST client making short-lived
         * connections, this is unnecessary complexity.
         */
        hs->error = BR_ERR_UNEXPECTED;
        return kTLS13_Error;

    default:
        /* Unknown post-handshake message type */
        hs->error = BR_ERR_UNEXPECTED;
        return kTLS13_Error;
    }
}

/* ── Handshake State Machine ── */

/*
 * Handle the kTLS13_SendClientHello state.
 *
 * Generates the X25519 key pair, builds the ClientHello message
 * (with all extensions), initializes the transcript hash, and
 * writes the complete TLS record into msg_buf for sending.
 *
 * After this returns kTLS13_WantWrite, the caller drains msg_buf
 * to the network (via ot_transport_send or BearSSL's sendrec),
 * then advances to kTLS13_SendCCS.
 */
static tls13_hs_result tls13_state_send_client_hello(tls13_hs_ctx *hs,
                                                     const char *hostname)
{
    br_ssl_engine_context *eng = hs->eng;
    br_hmac_drbg_context rng;
    unsigned char seed[32];
    int ret;

    if (eng == NULL) {
        hs->error = BR_ERR_BAD_STATE;
        return kTLS13_Error;
    }

    /*
     * Initialize a standalone PRNG for key generation and random bytes.
     *
     * We can't use BearSSL's engine PRNG because the engine hasn't
     * started its own handshake (we're bypassing T0). Instead, we
     * create our own HMAC_DRBG seeded from the entropy pool.
     *
     * We extract seed material by injecting entropy into the engine
     * (which has already been done by certainly.c) and then using
     * the engine's PRNG to generate our seed. If the engine's PRNG
     * is available, use it; otherwise seed from the raw entropy pool.
     */
    if (eng->rng_init_done) {
        /* Engine PRNG is seeded — use it to seed our standalone PRNG */
        br_hmac_drbg_generate(&eng->rng, seed, sizeof(seed));
    } else {
        /*
         * Engine PRNG not ready — seed the engine first, then use it.
         * entropy_seed_engine() injects pool data into the engine's
         * HMAC_DRBG.
         */
        (void)OSTLS_InjectEntropy(eng);
        br_hmac_drbg_generate(&eng->rng, seed, sizeof(seed));
    }

    br_hmac_drbg_init(&rng, &br_sha256_vtable, seed, sizeof(seed));

    /* Wipe the seed from the stack */
    secure_wipe(seed, sizeof(seed));

    /*
     * Initialize transcript hash.
     * Default to SHA-256; will be confirmed when we receive ServerHello
     * and learn the negotiated cipher suite. SHA-256 is used by
     * TLS_CHACHA20_POLY1305_SHA256 and TLS_AES_128_GCM_SHA256 (our
     * top two preferences), so this is almost always correct.
     */
    if (!hs->hrr_received) {
        tls13_transcript_init(&hs->transcript, &br_sha256_vtable);
    }
    /* If HRR was received, transcript was already reset by the HRR handler */

    /* Generate the ephemeral key pair. First flight: X25519. After a
     * HelloRetryRequest: the curve the server asked for (P-256/P-384).
     * A fresh keypair is generated each time — the retry must NOT reuse
     * the X25519 key, or it'll just get rejected again. */
    {
        uint16_t kg = hs->hrr_received ? hs->hrr_group : TLS13_GROUP_X25519;
        ret = tls13_generate_keypair(hs, kg, &rng);
        if (ret != 0) {
            hs->error = BR_ERR_BAD_STATE;
            hs->fail_site = TLS13_FAIL_KEYGEN;
            return kTLS13_Error;
        }
    }

    /* Build the ClientHello message */
    ret = tls13_build_client_hello(hs, hostname, &rng);
    if (ret != 0) {
        hs->error = BR_ERR_TOO_LARGE;
        hs->fail_site = TLS13_FAIL_CH_BUILD;
        return kTLS13_Error;
    }

    /* Wipe the standalone PRNG */
    memset(&rng, 0, sizeof(rng));

    return kTLS13_WantWrite;
}

/*
 * Handle the kTLS13_SendCCS state.
 *
 * Builds the CCS record into msg_buf for sending. The CCS is NOT
 * part of the TLS 1.3 handshake — it's purely for middlebox
 * compatibility and is not included in the transcript hash.
 */
static tls13_hs_result tls13_state_send_ccs(tls13_hs_ctx *hs)
{
    hs->msg_len = tls13_build_ccs(hs->msg_buf);
    hs->msg_offset = 0;
    return kTLS13_WantWrite;
}

/*
 * Main handshake state machine driver.
 *
 * Called repeatedly from the pump loop. Returns:
 *   kTLS13_WantWrite — msg_buf has data to send, call again after sending
 *   kTLS13_WantRead  — need more data from network, call again after recv
 *   kTLS13_OK        — state completed, call again immediately
 *   kTLS13_Complete  — handshake finished successfully
 *   kTLS13_Fallback12 — server chose TLS 1.2, fall back
 *   kTLS13_Error     — handshake failed, check hs->error
 */
tls13_hs_result tls13_handshake_step(tls13_hs_ctx *hs,
                                     unsigned char *recv_buf,
                                     size_t *recv_len,
                                     const char *hostname)
{
    switch (hs->state) {
    case kTLS13_SendClientHello: {
        tls13_hs_result r = tls13_state_send_client_hello(hs, hostname);
        if (r == kTLS13_WantWrite) {
            /*
             * Skip SendCCS here — per RFC 8446 Appendix D.4, the CCS
             * should be sent "immediately before the second flight",
             * i.e. right before the client Finished message, not
             * right after ClientHello. Sending it too early confuses
             * some servers.
             */
            hs->state = kTLS13_RecvServerHello;
        }
        return r;
    }

    case kTLS13_SendCCS: {
        tls13_hs_result r = tls13_state_send_ccs(hs);
        if (r == kTLS13_WantWrite) {
            hs->state = kTLS13_RecvServerHello;
        }
        return r;
    }

    case kTLS13_RecvServerHello: {
        tls13_hs_result r = tls13_state_recv_server_hello(hs, recv_buf, recv_len);
        if (r == kTLS13_OK) {
            /* TLS 1.3 ServerHello processed — advance to encrypted phase */
            hs->state = kTLS13_RecvEncryptedExtensions;
            return kTLS13_OK;
        }
        if (r == kTLS13_WantRead && hs->hrr_pending) {
            /*
             * A HelloRetryRequest was just parsed — resend the ClientHello
             * with the server-requested key share. hrr_pending is cleared
             * so the WantRead we'll return while waiting for the server's
             * response to the retry isn't mistaken for another HRR (which
             * caused an infinite SendClientHello<->RecvServerHello loop).
             */
            hs->hrr_pending = 0;
            hs->state = kTLS13_SendClientHello;
            return kTLS13_OK;
        }
        return r;
    }

    case kTLS13_RecvEncryptedExtensions:
        return tls13_state_recv_encrypted_extensions(hs, recv_buf, recv_len);

    case kTLS13_RecvCertRequestOrCert:
        return tls13_state_recv_cert_request_or_cert(hs, recv_buf, recv_len);

    case kTLS13_RecvCertificate:
        return tls13_state_recv_certificate(hs, recv_buf, recv_len, hostname);

    case kTLS13_RecvCertificateVerify:
        return tls13_state_recv_certificate_verify(hs, recv_buf, recv_len);

    case kTLS13_RecvFinished:
        return tls13_state_recv_finished(hs, recv_buf, recv_len);

    case kTLS13_SendFinished:
        return tls13_state_send_finished(hs);

    case kTLS13_Complete:
        return kTLS13_OK;

    default:
        hs->error = BR_ERR_BAD_STATE;
        return kTLS13_Error;
    }
}
