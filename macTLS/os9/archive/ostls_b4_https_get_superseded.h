/*
 * ostls_b4_https_get.h
 *
 * Stage B4: a tiny HTTPS GET. Builds on Stage B3's validated
 * handshake and adds the post-handshake application-data round
 * trip:
 *
 *   - GET / HTTP/1.0  (with Host:, User-Agent:, Connection: close)
 *   - drive br_ssl_engine_sendapp / sendrec until the request is on
 *     the wire
 *   - drive recvrec / recvapp until at least N bytes of plaintext
 *     response are captured or the peer closes the channel
 *   - log the first N bytes for diagnosis
 *
 * Connection: close avoids needing a chunked / Content-Length
 * decoder; the server closes after the response, which arrives as
 * BR_SSL_CLOSED with BR_ERR_OK.
 *
 * No HTTP parsing. We treat the response as opaque bytes -- the
 * caller can read "HTTP/1.0 301 Moved Permanently..." or
 * "HTTP/1.1 200 OK..." in the captured prefix and that proves
 * decrypted application bytes flowed end-to-end.
 *
 * Stage B5 then writes the integration notes for replacing or
 * augmenting MacSurf's HTTP fetcher with this stack -- text only,
 * no code wiring in this milestone.
 */

#ifndef OSTLS_B4_HTTPS_GET_H
#define OSTLS_B4_HTTPS_GET_H

#ifdef __MWERKS__
#include <Types.h>
#else
#ifndef OSTLS_OSERR_DEFINED
#define OSTLS_OSERR_DEFINED
typedef short OSErr;
#endif
#endif

#include <stddef.h>     /* size_t */

enum {
    kOSTLSB4_OK                    = 0,
    kOSTLSB4_BadArgs               = 600,
    kOSTLSB4_ClockBefore2000       = 601,
    kOSTLSB4_OTConfigFail          = 602,
    kOSTLSB4_OTOpenEndptFail       = 603,
    kOSTLSB4_OTBindFail            = 604,
    kOSTLSB4_OTDnsAddrFail         = 605,
    kOSTLSB4_OTConnectFail         = 606,
    kOSTLSB4_EntropyFail           = 607,
    kOSTLSB4_ClientResetFail       = 608,
    kOSTLSB4_OTSndFail             = 609,
    kOSTLSB4_OTRcvFail             = 610,
    kOSTLSB4_HandshakeTimeout      = 611,
    kOSTLSB4_BearSSLError          = 612,   /* covers all post-handshake BR_ERR_* */
    kOSTLSB4_RequestTooBig         = 613,   /* GET line doesn't fit in sendapp buf */
    kOSTLSB4_NoBytesReceived       = 614    /* peer closed without any plaintext */
};

/*
 * Run B4:
 *
 *   target_host_port    "google.com:443"
 *   server_name         "google.com"  (used for SNI + cert hostname match)
 *   request_path        "/"           (passed verbatim into the GET line)
 *   out_prefix          buffer for the first bytes of the decrypted
 *                       HTTP response (NUL-terminated)
 *   out_prefix_cap      capacity of out_prefix
 *   out_msg             status string; always NUL-terminated
 *
 * Returns kOSTLSB4_OK on success. The result window / log can then
 * show both the negotiated cipher suite and the response prefix.
 */
OSErr OSTLS_B4_HTTPS_Get_Probe(const char *target_host_port,
                               const char *server_name,
                               const char *request_path,
                               char *out_prefix, size_t out_prefix_cap,
                               char *out_msg, size_t out_msg_len);

#endif /* OSTLS_B4_HTTPS_GET_H */
