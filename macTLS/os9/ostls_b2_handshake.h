/*
 * ostls_b2_handshake.h
 *
 * Stage B2: BearSSL TLS client handshake driven over Open Transport.
 *
 * Builds on Stage B1's verified OT TCP transport. Opens a fresh endpoint,
 * brings up a BearSSL br_ssl_client_context with an intentionally
 * permissive ("insecure") X.509 validator (accepts any chain; extracts
 * the leaf certificate's public key via br_x509_decoder so handshake
 * signatures can still verify against the server's real key), and
 * drives the SSL engine record I/O state machine over OT until either
 *
 *   - the handshake completes (engine reaches BR_SSL_SENDAPP /
 *     BR_SSL_RECVAPP without BR_SSL_CLOSED), or
 *   - the engine closes with an error (BR_SSL_CLOSED set), or
 *   - an OT error occurs, or
 *   - the per-probe deadline (60 s) expires.
 *
 * NO certificate trust anchors are consulted in B2. Anyone-on-the-path
 * can substitute their own cert and we will happily complete the
 * handshake against it. This is for isolating the BearSSL record I/O
 * loop from the trust-chain wiring; Stage B3 swaps the validator for
 * br_x509_minimal with embedded anchors and that's where real TLS
 * starts.
 *
 * Acceptance for B2: handshake completes against example.com:443.
 *
 * Caller's responsibility:
 *   - InitOpenTransportInContext has already succeeded (main.c)
 *   - target_host_port is "host:port" for OTInitDNSAddress
 *   - server_name is the bare hostname for SNI (matches host in target)
 */

#ifndef OSTLS_B2_HANDSHAKE_H
#define OSTLS_B2_HANDSHAKE_H

#ifdef __MWERKS__
#include <Types.h>      /* OSErr */
#else
typedef short OSErr;
#endif

#include <stddef.h>     /* size_t */

/*
 * Stage B2 result codes. Range 400..420 so they're disjoint from
 * earlier stages.
 *
 * On failure, out_msg holds a human-readable description with the
 * underlying OT or BearSSL error code so the result window points at
 * exactly which step broke and what the engine / network reported.
 */
enum {
    kOSTLSB2_OK                = 0,
    kOSTLSB2_BadArgs           = 400,
    kOSTLSB2_OTConfigFail      = 401,
    kOSTLSB2_OTOpenEndptFail   = 402,
    kOSTLSB2_OTBindFail        = 403,
    kOSTLSB2_OTDnsAddrFail     = 404,
    kOSTLSB2_OTConnectFail     = 405,
    kOSTLSB2_EntropyFail       = 406,
    kOSTLSB2_ClientResetFail   = 407,
    kOSTLSB2_OTSndFail         = 408,
    kOSTLSB2_OTRcvFail         = 409,
    kOSTLSB2_PeerClosedEarly   = 410,
    kOSTLSB2_HandshakeTimeout  = 411,
    kOSTLSB2_BearSSLError      = 412
};

/*
 * Run the Stage B2 handshake probe.
 *
 *   target_host_port: e.g. "example.com:443" (for OTInitDNSAddress)
 *   server_name:      e.g. "example.com"     (for SNI + reset)
 *   out_msg:          buffer for status string; always NUL-terminated.
 *
 * Returns kOSTLSB2_OK if the handshake completes.
 */
OSErr OSTLS_B2_Handshake_Probe(const char *target_host_port,
                               const char *server_name,
                               char *out_msg, size_t out_msg_len);

#endif /* OSTLS_B2_HANDSHAKE_H */
