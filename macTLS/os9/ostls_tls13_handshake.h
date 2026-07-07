/*
 * tls13_handshake.h — TLS 1.3 handshake state machine types
 */

#ifndef OSTLS_TLS13_HANDSHAKE_H
#define OSTLS_TLS13_HANDSHAKE_H

#include "bearssl.h"
#include <stdint.h>
#include <stddef.h>
#include "ostls_tls13_keysched.h"
#include "ostls_tls13_record.h"

/* Handshake message types (RFC 8446 Section 4) */
#define TLS13_HT_CLIENT_HELLO          1
#define TLS13_HT_SERVER_HELLO          2
#define TLS13_HT_NEW_SESSION_TICKET    4
#define TLS13_HT_ENCRYPTED_EXTENSIONS  8
#define TLS13_HT_CERTIFICATE          11
#define TLS13_HT_CERTIFICATE_REQUEST  13
#define TLS13_HT_CERTIFICATE_VERIFY   15
#define TLS13_HT_FINISHED             20
#define TLS13_HT_KEY_UPDATE           24
#define TLS13_HT_MESSAGE_HASH        254

/* TLS versions */
#define TLS13_VERSION  0x0304
#define TLS12_VERSION  0x0303

/* TLS 1.3 cipher suite IDs */
#define TLS13_AES_128_GCM_SHA256       0x1301
#define TLS13_AES_256_GCM_SHA384       0x1302
#define TLS13_CHACHA20_POLY1305_SHA256 0x1303

/* Extension types */
#define TLS13_EXT_SERVER_NAME           0
#define TLS13_EXT_SUPPORTED_GROUPS     10
#define TLS13_EXT_SIGNATURE_ALGORITHMS 13
#define TLS13_EXT_ALPN                 16
#define TLS13_EXT_PRE_SHARED_KEY       41
#define TLS13_EXT_SUPPORTED_VERSIONS   43
#define TLS13_EXT_COOKIE               44
#define TLS13_EXT_PSK_KEY_EXCHANGE_MODES 45
#define TLS13_EXT_KEY_SHARE            51

/* Signature schemes */
#define TLS13_SIG_RSA_PSS_RSAE_SHA256      0x0804
#define TLS13_SIG_RSA_PSS_RSAE_SHA384      0x0805
#define TLS13_SIG_ECDSA_SECP256R1_SHA256   0x0403
#define TLS13_SIG_ECDSA_SECP384R1_SHA384   0x0503
#define TLS13_SIG_RSA_PKCS1_SHA256         0x0401
#define TLS13_SIG_RSA_PKCS1_SHA384         0x0501

/* Named groups (supported_groups / key_share). X25519 is preferred; the
 * NIST curves are required by FIPS/compliance servers that disable X25519
 * (e.g. some Cloudflare zones), reached via HelloRetryRequest. */
#define TLS13_GROUP_SECP256R1  0x0017
#define TLS13_GROUP_SECP384R1  0x0018
#define TLS13_GROUP_X25519     0x001D

/* Handshake state machine states */
typedef enum {
    kTLS13_SendClientHello,
    kTLS13_SendCCS,
    kTLS13_RecvServerHello,
    kTLS13_RecvEncryptedExtensions,
    kTLS13_RecvCertRequestOrCert,
    kTLS13_RecvCertificate,
    kTLS13_RecvCertificateVerify,
    kTLS13_RecvFinished,
    kTLS13_SendFinished,
    kTLS13_Complete
} tls13_hs_state;

/* Return values from handshake state handlers */
typedef enum {
    kTLS13_OK,          /* state completed, advance to next */
    kTLS13_WantRead,    /* need more data from network */
    kTLS13_WantWrite,   /* have data to send */
    kTLS13_Fallback12,  /* server chose TLS 1.2, fall back */
    kTLS13_Error        /* handshake failed */
} tls13_hs_result;

/* Transcript hash context */
typedef struct {
    br_sha256_context sha256;
    br_sha384_context sha384;
    const br_hash_class *hash;
    size_t hash_len;
} tls13_transcript;

void tls13_transcript_init(tls13_transcript *t, const br_hash_class *hash);
void tls13_transcript_update(tls13_transcript *t,
                             const void *data, size_t len);
void tls13_transcript_snapshot(const tls13_transcript *t,
                               void *out_hash);
/* Reset for HRR: replace transcript with Hash(message_hash construct) */
void tls13_transcript_reset_for_hrr(tls13_transcript *t);

/* A parsed, ready-to-reuse session ticket (macTLS#2 Stage B). The
 * resumption PSK is already derived (nonce folded in), so the nonce is
 * not retained. `ticket` is the opaque blob we echo back as the PSK
 * identity on a resumed ClientHello. The cache layer stamps received_ms
 * and pairs this with a host; `valid` gates use.
 *
 * 512 (was 1024): real TLS 1.3 tickets are small (68kmla ~32B, most
 * servers <512B), and this struct is embedded in tls13_hs_ctx (the ~35KB
 * context whose NewPtrClear is marginal on a tight app partition) AND in
 * each of the 6 cache slots, so the cap directly drives footprint.
 * Oversize tickets are skipped gracefully (full handshake next time). */
#define TLS13_MAX_TICKET_LEN 512
typedef struct {
    int           valid;
    unsigned char psk[64];          /* resumption PSK (psk_len bytes) */
    size_t        psk_len;          /* = hash_len of cipher_suite */
    uint16_t      cipher_suite;     /* suite the ticket was minted under */
    uint32_t      lifetime;         /* ticket_lifetime, seconds */
    uint32_t      age_add;          /* ticket_age_add (obfuscation addend) */
    unsigned char ticket[TLS13_MAX_TICKET_LEN];
    size_t        ticket_len;
} tls13_session_ticket;

/* Full TLS 1.3 handshake context */
typedef struct {
    tls13_hs_state      state;
    tls13_keysched      ks;
    tls13_transcript    transcript;
    tls13_record_ctx    read_ctx;    /* decrypt incoming records */
    tls13_record_ctx    write_ctx;   /* encrypt outgoing records */

    /* Ephemeral ECDHE key pair. Sized for the largest curve we offer
     * (P-384: 48-byte scalar, 97-byte uncompressed public point); X25519
     * uses 32/32. ecdhe_group is the named-group the current keypair is
     * for; *_len are the actual byte lengths in use. */
    unsigned char       ecdhe_secret[64];
    unsigned char       ecdhe_public[133];
    uint16_t            ecdhe_group;     /* TLS13_GROUP_* of the current keypair */
    size_t              ecdhe_secret_len;
    size_t              ecdhe_public_len;
    uint16_t            hrr_group;       /* group requested by HelloRetryRequest (0 = none) */
    int                 hrr_pending;     /* transient: an HRR was just parsed, resend CH now */

    /* Traffic secrets (kept for Finished key derivation) */
    unsigned char       client_hs_secret[64];
    unsigned char       server_hs_secret[64];

    /*
     * Buffer for one handshake message: our outgoing ClientHello /
     * Finished, AND each INCOMING message extracted from a decrypted
     * record (EncryptedExtensions, Certificate, CertificateVerify,
     * Finished). An incoming message can be as large as a full TLS
     * record plaintext, so this must be at least as big as plain_buf
     * (16640 = 2^14 + 256, the RFC 8446 max TLSCiphertext fragment).
     * It was 4096, which overflowed on servers with large certificate
     * chains (e.g. Google's ~4.5 KB Certificate message) -- the cert
     * data ran past the end of the buffer and corrupted the struct.
     */
    unsigned char       msg_buf[16640];
    size_t              msg_len;
    size_t              msg_offset;

    /*
     * Length of a pre-read handshake message sitting in msg_buf
     * (for the RecvCertRequestOrCert handler to hand off to
     * RecvCertificate). SEPARATE from msg_len to avoid triggering
     * the outgoing-send pump path.
     */
    size_t              pending_recv_msg_len;

    /*
     * Plaintext buffer for decrypted incoming handshake records.
     * TLS 1.3 servers commonly pack multiple handshake messages into
     * a single encrypted record (EncryptedExtensions + Certificate +
     * CertificateVerify + Finished). We decrypt the full record into
     * this buffer, then consume handshake messages one at a time from
     * here, advancing plain_offset. When plain_offset == plain_len,
     * we decrypt the next record.
     */
    unsigned char       plain_buf[16640];   /* >= max TLSCiphertext fragment (2^14 + 256) */
    size_t              plain_len;
    size_t              plain_offset;

    /* HRR cookie */
    unsigned char       cookie[256];
    size_t              cookie_len;

    /* Negotiated cipher suite */
    uint16_t            cipher_suite;
    int                 is_tls13;
    int                 hrr_received;

    int                 cert_request_received;

    /* X.509 validation context (pointer to MacTLS_Context's xc) */
    const br_x509_class **x509_ctx;

    /*
     * BearSSL engine pointer — used only for PRNG seeding during
     * ClientHello construction. The handshake does NOT drive record
     * I/O through this engine; that is done directly against a
     * caller-supplied recv buffer. This field is allowed to be NULL
     * if the caller arranges RNG seeding some other way.
     */
    br_ssl_engine_context *eng;

    /* Server's public key (from certificate, for CertificateVerify) */
    br_x509_pkey        server_pkey;
    /* Backing storage for server_pkey's pointer fields */
    unsigned char       server_pkey_data[520]; /* BR_X509_BUFSIZE_KEY */

    /* Error code from BearSSL (if handshake fails) */
    int                 error;

    /* Session resumption (macTLS#2). res_master is the
     * resumption_master_secret, derived at SendFinished while ks.secret
     * still holds the Master Secret; it lives for the connection so any
     * NewSessionTicket can mint its PSK. A parsed ticket lands in
     * `ticket` with ticket_valid set, for the cache layer to harvest. */
    unsigned char         res_master[64];
    int                   res_master_valid;
    tls13_session_ticket  ticket;
    int                   ticket_valid;

    /* Outbound resumption (macTLS#2 Stage C2): when `resuming` is set the
     * cache layer points offer_ticket at a live cached ticket and supplies
     * the obfuscated_ticket_age; the ClientHello then carries
     * psk_key_exchange_modes + pre_shared_key with a computed binder. */
    int                          resuming;
    const tls13_session_ticket  *offer_ticket;
    uint32_t                     offer_obfuscated_age;
    int                          resumption_accepted; /* server echoed PSK */
} tls13_hs_ctx;

/*
 * Main handshake driver — call repeatedly from the pump loop.
 *
 * recv_buf / recv_len is a caller-owned buffer holding raw bytes received
 * from the network. The handshake consumes complete TLS records from the
 * front of the buffer and slides the remaining bytes down, updating
 * *recv_len to reflect what's still unread. Callers should append newly
 * received bytes to recv_buf at offset *recv_len before calling this.
 */
tls13_hs_result tls13_handshake_step(tls13_hs_ctx *hs,
                                     unsigned char *recv_buf,
                                     size_t *recv_len,
                                     const char *hostname);

/* Initialize handshake context */
void tls13_handshake_init(tls13_hs_ctx *hs);

/* Handle post-handshake messages (NewSessionTicket, KeyUpdate).
 * Called when an encrypted record with inner content type handshake (22)
 * is received during the application data phase. */
tls13_hs_result tls13_handle_post_handshake(tls13_hs_ctx *hs,
                                            const unsigned char *data,
                                            size_t data_len);

/* Parse a NewSessionTicket handshake message (macTLS#2 Stage B), incl.
 * the 4-byte handshake header, and fill `out` with a ready-to-reuse
 * ticket: the resumption PSK is derived from hs->res_master + the
 * ticket's nonce, the opaque ticket blob is copied, and lifetime /
 * age_add / cipher_suite are captured. Returns 0 on success, -1 on a
 * malformed message, oversize ticket, or missing res_master. */
int tls13_parse_new_session_ticket(tls13_hs_ctx *hs,
                                    const unsigned char *msg, size_t msg_len,
                                    tls13_session_ticket *out);

/* Compute a PSK binder (macTLS#2 Stage C2). Clones `base` (the running
 * transcript, so the HRR case with CH1+HRR already hashed works), feeds
 * the truncated ClientHello bytes (handshake message through the PSK
 * identities, WITHOUT the binders), snapshots, then HMACs that with the
 * finished key derived from the resumption PSK via the "res binder" path.
 * Writes ks->hash_len bytes to out_binder. */
void tls13_compute_binder(const tls13_keysched *ks,
                          const unsigned char *psk, size_t psk_len,
                          const tls13_transcript *base,
                          const unsigned char *truncated_ch, size_t trunc_len,
                          unsigned char *out_binder);

#endif /* OSTLS_TLS13_HANDSHAKE_H */
