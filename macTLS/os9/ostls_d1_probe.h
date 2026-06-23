/*
 * ostls_d1_probe.h -- Stage D1 async OT notifier smoke
 *
 * Self-contained probe that exercises only the async Open Transport
 * mechanism + notifier dispatch -- no BearSSL, no library context.
 * Proves before fixes42 ships that
 *
 *   OTAsyncOpenEndpointInContext + OTInstallNotifier-equivalent
 *   + async OTBind / OTConnect
 *
 * reliably delivers T_OPENCOMPLETE / T_BINDCOMPLETE / T_CONNECT to
 * a notifier callback running at interrupt time, on Carbon CFM
 * with CarbonLib's OT wrapper.
 *
 * This is the test that the C1 listener investigation should have
 * had first -- it would have isolated the passive-bind failure to
 * the explicit-address codepath much earlier. Keeping it permanent
 * in MacTLSTest gives us a low-cost regression for the async OT
 * path that the v0.2 OSTLSConnection depends on.
 */

#ifndef OSTLS_D1_PROBE_H
#define OSTLS_D1_PROBE_H

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
 * D1 result codes. Range 800..819 so they don't collide with B-stage
 * (100..619), C1 abandoned (700..709), or async (2000..2019) codes.
 */
enum {
    kOSTLSD1_OK                = 0,
    kOSTLSD1_BadArgs           = 800,
    kOSTLSD1_OTConfigFail      = 801,
    kOSTLSD1_OTAsyncOpenFail   = 802,
    kOSTLSD1_OpenTimeout       = 803,
    kOSTLSD1_OpenBadResult     = 804,
    kOSTLSD1_OTBindSubmitFail  = 805,
    kOSTLSD1_BindTimeout       = 806,
    kOSTLSD1_BindBadResult     = 807,
    kOSTLSD1_OTDnsAddrFail     = 808,
    kOSTLSD1_OTConnectSubmitFail = 809,
    kOSTLSD1_ConnectTimeout    = 810,
    kOSTLSD1_ConnectBadResult  = 811,
    kOSTLSD1_DisconnectArrived = 812
};

/*
 * Run the D1 probe. target_host_port is "host:port" e.g.
 * "google.com:443" -- the actual TLS handshake never starts; we
 * tear down right after the TCP three-way handshake completes.
 *
 * out_msg is a caller-allocated buffer for a status string. Always
 * NUL-terminated.
 *
 * Returns kOSTLSD1_OK on success.
 */
OSErr OSTLS_D1_AsyncNotifier_Probe(const char *target_host_port,
                                   char *out_msg, size_t out_msg_len);

#endif /* OSTLS_D1_PROBE_H */
