/*
 * ostls_c1_listener.h
 *
 * Stage C1: open an Open Transport TCP listener on a local port,
 * accept exactly ONE incoming connection, read whatever the client
 * sends (up to a caller-supplied buffer), and close cleanly. No
 * parsing, no TLS, no upstream fetch. The narrow gate is:
 *
 *   Does Mac OS 9 + Open Transport let MacTLSTest act as a TCP server
 *   from inside the same Carbon CFM binary that already works as a
 *   TCP client?
 *
 * Once this gate passes:
 *   - C2 adds an absolute-form GET parser
 *   - C3 routes the parsed request through Stage B4's fetch path
 *   - C4 wires a real classic-browser HTTP-proxy round trip
 *
 * Stage C1 acceptance: this probe binds on 127.0.0.1:PORT (or 0.0.0.0
 * if 127.0.0.1 binding turns out to be quirky on classic OT -- the
 * implementation uses 0.0.0.0 for portability and lets the firewall
 * decide), the operator connects from another Mac / from the same
 * Mac via telnet, sends any bytes, those bytes are captured and
 * logged, the listener closes.
 *
 * The probe is synchronous + blocking and short-lived; OT's internal
 * timeout (~30 s on most stacks) is the only "no client showed up"
 * bound. The result code distinguishes every step that can fail so
 * the log/result-window points at exactly which call broke.
 */

#ifndef OSTLS_C1_LISTENER_H
#define OSTLS_C1_LISTENER_H

#ifdef __MWERKS__
#include <Types.h>
#else
#ifndef OSTLS_OSERR_DEFINED
#define OSTLS_OSERR_DEFINED
typedef short OSErr;
#endif
#endif

#include <stddef.h>     /* size_t */

/*
 * Result codes 700..720, disjoint from earlier stages.
 */
enum {
    kOSTLSC1_OK                = 0,
    kOSTLSC1_BadArgs           = 700,
    kOSTLSC1_OTConfigFail      = 701,
    kOSTLSC1_OTOpenEndptFail   = 702,
    kOSTLSC1_OTBindFail        = 703,
    kOSTLSC1_OTListenFail      = 704,
    kOSTLSC1_OTAcceptOpenFail  = 705,   /* failed to open the child endpoint */
    kOSTLSC1_OTAcceptBindFail  = 706,   /* failed to bind the child for accept */
    kOSTLSC1_OTAcceptFail      = 707,   /* OTAccept itself returned an error */
    kOSTLSC1_OTRcvFail         = 708,
    kOSTLSC1_PeerClosedEmpty   = 709    /* client connected but sent no bytes */
};

/*
 * Run the C1 listener probe.
 *
 *   port:           e.g. 8765
 *   out_request:    caller buffer for the bytes the client sent
 *                   (NUL-terminated; capacity must be >= 2)
 *   out_request_cap : size of out_request
 *   out_msg:        status string; always NUL-terminated
 *
 * The function:
 *   1. Opens a TCP listener endpoint via OTOpenEndpointInContext
 *   2. Builds an InetAddress for 0.0.0.0:port (INADDR_ANY)
 *   3. OTBind with TBind.qlen = 1 (single pending connection)
 *   4. OTListen -- blocks until a client connects
 *   5. Opens a child endpoint, OTBinds it, OTAccepts the connection
 *   6. OTRcvs on the child until 0 (peer closed) or buffer full
 *   7. Tears both endpoints down cleanly
 *
 * Returns kOSTLSC1_OK on success.
 */
OSErr OSTLS_C1_Listener_Probe(unsigned short port,
                              char *out_request, size_t out_request_cap,
                              char *out_msg, size_t out_msg_len);

#endif /* OSTLS_C1_LISTENER_H */
