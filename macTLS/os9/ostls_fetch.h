/*
 * ostls_fetch.h -- macTLS public library API
 *
 * Single-shot validated HTTPS GET for classic Mac OS 9 / PowerPC,
 * built on BearSSL over Open Transport. This is the stable surface
 * macTLS exposes to callers (MacSurf, future tools); the
 * Stage A-B internals (entropy, B1-B4 probes) are no longer the
 * public face of the project.
 *
 * Stage D pivot rationale: Carbon CFM `OTOpenEndpointInContext`
 * categorically rejects passive bind to caller-chosen addresses
 * (see docs/carbon-ot-passive-bind-finding.md). The original
 * "local-proxy-on-port-8765" design is therefore impossible on
 * this platform. macTLS now ships as a library a host app
 * links directly -- no listener, no port, no Carbon CFM
 * passive-bind problem.
 *
 * Verified end-to-end on real OS 9.1 / G3 hardware. Cipher suite
 * negotiated and validated handshake completes against the
 * 10 embedded trust anchors (Amazon, DigiCert G2/G3, GTS R1-R4,
 * ISRG X1/X2, Starfield Services G2).
 *
 * Status: v1 -- synchronous, blocking, single request per call,
 * HTTP/1.0 + Connection: close, no streaming callback, no POST,
 * no redirect handling. Intentionally narrow surface to keep the
 * first ship stable; v2 will extend.
 */

#ifndef OSTLS_FETCH_H
#define OSTLS_FETCH_H

#ifdef __MWERKS__
#include <MacTypes.h>      /* OSErr, UInt16, UInt32 */
#else
#ifndef OSTLS_OSERR_DEFINED
#define OSTLS_OSERR_DEFINED
typedef short OSErr;
#endif
#ifndef OSTLS_UINT16_DEFINED
#define OSTLS_UINT16_DEFINED
typedef unsigned short UInt16;
#endif
#ifndef OSTLS_UINT32_DEFINED
#define OSTLS_UINT32_DEFINED
typedef unsigned long UInt32;
#endif
#endif

/*
 * Library result codes. Range 1000..1019 so they're disjoint from
 * the Stage A-B probe codes (100..619).
 */
enum {
    kOSTLSFetch_OK                  = 0,
    kOSTLSFetch_BadArgs             = 1000,
    kOSTLSFetch_ClockBefore2000     = 1001,
    kOSTLSFetch_OTConfigFail        = 1002,
    kOSTLSFetch_OTOpenEndptFail     = 1003,
    kOSTLSFetch_OTBindFail          = 1004,
    kOSTLSFetch_OTDnsAddrFail       = 1005,
    kOSTLSFetch_OTConnectFail       = 1006,
    kOSTLSFetch_EntropyFail         = 1007,
    kOSTLSFetch_ClientResetFail     = 1008,
    kOSTLSFetch_OTSndFail           = 1009,
    kOSTLSFetch_OTRcvFail           = 1010,
    kOSTLSFetch_HandshakeTimeout    = 1011,
    kOSTLSFetch_BearSSLError        = 1012,
    kOSTLSFetch_RequestTooBig       = 1013,
    kOSTLSFetch_NoBytesReceived     = 1014
};

/*
 * Perform a single validated HTTPS GET.
 *
 * Parameters
 *   host          NUL-terminated hostname OR dotted-quad IP.
 *                 e.g. "google.com" or "10.42.0.145".
 *   port          TCP port (typically 443 for HTTPS).
 *   server_name   NUL-terminated SNI / cert-validation name. May differ
 *                 from `host` if the caller is connecting to a CDN by
 *                 IP but expecting a specific cert. For a normal fetch,
 *                 pass the same string as `host`.
 *   path          HTTP request path, e.g. "/" or "/api/v1/posts".
 *                 Passed verbatim into the GET line.
 *   out_buf       Caller-allocated buffer for the response prefix.
 *                 Treated as raw bytes (void *), not NUL-terminated.
 *   out_cap       Capacity of out_buf in bytes.
 *   out_len       Optional: pointer set to the actual number of
 *                 response bytes copied into out_buf. May be NULL.
 *   out_msg       Caller-allocated buffer for a human-readable
 *                 status string. Always NUL-terminated on return.
 *   out_msg_len   Capacity of out_msg in bytes (recommend >= 180).
 *
 * Behaviour
 *   - Synchronous + blocking. The call returns when the handshake
 *     and the GET have completed (or failed).
 *   - Opens its own OT endpoint per call; closes it on return.
 *     No connection pooling in v1.
 *   - Sends:
 *       GET <path> HTTP/1.0
 *       Host: <server_name>
 *       User-Agent: macTLS/0.1
 *       Connection: close
 *   - Captures up to out_cap bytes of the decrypted response into
 *     out_buf. Stops at either out_cap or peer close.
 *   - 60-second total deadline including handshake + transfer.
 *   - Caller is responsible for InitOpenTransportInContext being
 *     in effect before the first call to OSTLS_Fetch, and for
 *     CloseOpenTransportInContext at app shutdown.
 *
 * Result
 *   kOSTLSFetch_OK on success. A non-zero kOSTLSFetch_* code on
 *   failure. out_msg always carries a short diagnostic describing
 *   what happened (including BearSSL or OT error codes where
 *   applicable).
 */
OSErr OSTLS_Fetch(const char *host,
                  UInt16      port,
                  const char *server_name,
                  const char *path,
                  void       *out_buf,
                  UInt32      out_cap,
                  UInt32     *out_len,
                  char       *out_msg,
                  UInt32      out_msg_len);

#endif /* OSTLS_FETCH_H */
