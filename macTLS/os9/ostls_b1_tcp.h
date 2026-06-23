/*
 * ostls_b1_tcp.h
 *
 * Stage B1: standalone Open Transport TCP connect probe.
 *
 * Proves that the MacTLSTest binary can open a TCP endpoint, bind it,
 * resolve a hostname via OTInitDNSAddress, and complete an OTConnect
 * to a real Internet host using the same OT pattern MacSurf's HTTP
 * fetcher uses today (OTOpenEndpointInContext, sync + blocking, no
 * notifier-mediated I/O for the connect itself).
 *
 * No TLS, no BearSSL, no application bytes. We connect, optionally
 * orderly-disconnect, close the endpoint. The probe answers one
 * question: does OT actually deliver a working TCP connection in this
 * Carbon CFM binary, on this machine, against this host:port?
 *
 * Stage B2 builds the BearSSL record I/O loop on top of this.
 *
 * Caller is responsible for having called InitOpenTransportInContext
 * before invoking the probe. The probe does NOT open or close OT
 * itself -- that lifecycle belongs to main.c, mirroring MacSurf's
 * convention.
 */

#ifndef OSTLS_B1_TCP_H
#define OSTLS_B1_TCP_H

#ifdef __MWERKS__
#include <Types.h>      /* OSErr */
#else
typedef short OSErr;
#endif

#include <stddef.h>     /* size_t */

/*
 * Stage B1 result codes. Range 300..320 so they're disjoint from
 * Stage A (100..103) and Stage A.5 (200..207).
 *
 * On failure, out_msg holds a human-readable string with the
 * underlying OT error code so the result window points at exactly
 * which call broke and what return value it produced.
 */
enum {
    kOSTLSB1_OK                = 0,
    kOSTLSB1_BadArgs           = 300,
    kOSTLSB1_OTConfigFail      = 301,
    kOSTLSB1_OTOpenEndptFail   = 302,
    kOSTLSB1_OTBindFail        = 303,
    kOSTLSB1_OTDnsAddrFail     = 304,
    kOSTLSB1_OTConnectFail     = 305
};

/*
 * Run the B1 probe against the given "host:port" target. The probe
 * opens a fresh endpoint per call -- no pooling -- because Stage B1
 * is about proving the connect path, not about performance.
 *
 * On entry:
 *   target_host_port: NUL-terminated "host:port" e.g. "example.com:443"
 *   out_msg / out_msg_len: caller-provided buffer for status string;
 *                          the function always writes a NUL-terminated
 *                          message, truncating if needed.
 *
 * Returns kOSTLSB1_OK on success, a kOSTLSB1_* code on failure.
 *
 * Requires InitOpenTransportInContext to have succeeded already (see
 * MacTLSTest/main.c). Does not require BearSSL to be initialised.
 */
OSErr OSTLS_B1_TCP_Probe(const char *target_host_port,
                         char *out_msg, size_t out_msg_len);

#endif /* OSTLS_B1_TCP_H */
