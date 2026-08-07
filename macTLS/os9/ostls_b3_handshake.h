/*
 * ostls_b3_handshake.h
 *
 * Stage B3: BearSSL TLS handshake with REAL X.509 validation against
 * embedded trust anchors. Builds on B2's record I/O loop but replaces
 * the insecure validator with br_x509_minimal seeded from the five
 * embedded roots in ostls_b3_anchors. Time comes from
 * OSTLS_GetBearSSLTime so notBefore / notAfter and the chain's
 * validity windows are actually checked.
 *
 * Acceptance: handshake completes against a public host whose chain
 * terminates in one of the embedded roots (google.com is the
 * primary B3 probe target).
 *
 * Distinguished failure modes:
 *   - clock before 2000     -> kOSTLSB3_ClockBefore2000
 *   - unknown CA            -> kOSTLSB3_X509UnknownCA
 *   - expired cert          -> kOSTLSB3_X509Expired
 *   - hostname mismatch     -> kOSTLSB3_X509HostnameMismatch
 *   - bad signature         -> kOSTLSB3_X509BadSignature
 *   - any other BR_ERR_X509_* -> kOSTLSB3_X509Other (with code in out_msg)
 *   - non-X509 BearSSL err  -> kOSTLSB3_BearSSLError (with code)
 *   - OT failures, timeouts -> as in B2
 */

#ifndef OSTLS_B3_HANDSHAKE_H
#define OSTLS_B3_HANDSHAKE_H

#if defined(__MWERKS__) || defined(__RETRO68__)  /* Mac target, either toolchain */
#include <Types.h>      /* OSErr */
#else
#ifndef OSTLS_OSERR_DEFINED
#define OSTLS_OSERR_DEFINED
typedef short OSErr;
#endif
#endif

#include <stddef.h>     /* size_t */

enum {
    kOSTLSB3_OK                    = 0,
    kOSTLSB3_BadArgs               = 500,
    kOSTLSB3_ClockBefore2000       = 501,
    kOSTLSB3_OTConfigFail          = 502,
    kOSTLSB3_OTOpenEndptFail       = 503,
    kOSTLSB3_OTBindFail            = 504,
    kOSTLSB3_OTDnsAddrFail         = 505,
    kOSTLSB3_OTConnectFail         = 506,
    kOSTLSB3_EntropyFail           = 507,
    kOSTLSB3_ClientResetFail       = 508,
    kOSTLSB3_OTSndFail             = 509,
    kOSTLSB3_OTRcvFail             = 510,
    kOSTLSB3_PeerClosedEarly       = 511,
    kOSTLSB3_HandshakeTimeout      = 512,
    kOSTLSB3_X509NotTrusted        = 513,   /* BR_ERR_X509_NOT_TRUSTED */
    kOSTLSB3_X509Expired           = 514,   /* BR_ERR_X509_EXPIRED */
    kOSTLSB3_X509HostnameMismatch  = 515,   /* BR_ERR_X509_BAD_SERVER_NAME */
    kOSTLSB3_X509BadSignature      = 516,   /* BR_ERR_X509_BAD_SIGNATURE */
    kOSTLSB3_X509TimeUnknown       = 517,   /* BR_ERR_X509_TIME_UNKNOWN */
    kOSTLSB3_X509Other             = 518,   /* any other BR_ERR_X509_* */
    kOSTLSB3_BearSSLError          = 519    /* non-X509 BR_ERR_* */
};

/*
 * Run the Stage B3 validated handshake probe.
 *
 *   target_host_port: e.g. "google.com:443"
 *   server_name:      e.g. "google.com"
 *   out_msg:          status string with raw BearSSL error code if any
 *
 * Returns kOSTLSB3_OK on success.
 */
OSErr OSTLS_B3_Validated_Probe(const char *target_host_port,
                               const char *server_name,
                               char *out_msg, size_t out_msg_len);

#endif /* OSTLS_B3_HANDSHAKE_H */
