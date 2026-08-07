/*
 * ostls_b1_tcp.c -- Stage B1: standalone OT TCP connect probe.
 *
 * See ostls_b1_tcp.h. Mirrors MacSurf's macos9_http_fetcher.c endpoint
 * setup pattern (OTOpenEndpointInContext + sync + blocking) but without
 * the pool, the keep-alive headers, or any application bytes.
 *
 *   1. OTCreateConfiguration("tcp")
 *   2. OTOpenEndpointInContext(cfg, 0, NULL, &err, ctx)
 *   3. OTSetSynchronous + OTSetBlocking
 *   4. OTBind(ep, NULL, NULL)
 *   5. OTInitDNSAddress(&dns, "host:port")
 *   6. OTConnect(ep, &call, NULL)
 *   7. OTSndOrderlyDisconnect + OTCloseProvider
 *
 * Step 6 either succeeds (we're connected) or fails with a non-noErr
 * OSStatus -- the underlying OT code goes straight into out_msg so the
 * result window points at exactly what broke.
 *
 * Notifier policy: none for B1. The connect is synchronous and there's
 * no other Thread Manager work for a notifier to yield to. Stage B2's
 * BearSSL record I/O loop will install one if it needs cooperative
 * yielding around long OTRcv calls.
 */

#include "ostls_b1_tcp.h"

#include <stdio.h>
#include <string.h>

#if defined(__MWERKS__) || defined(__RETRO68__)  /* Mac target, either toolchain */
#include <Files.h>
#include <OpenTransport.h>
#include <OpenTptInternet.h>
/*
 * Deliberately NOT including <Threads.h> -- on some CW8 installs an
 * unrelated HotSpot JVM Threads.h is reachable via the system search
 * path and cascades into oobj.h / typedefs.h / winnt.h / winbase.h
 * errors. mactlstest_prefix.h pre-defines several known JVM guards
 * to neutralise that file, but the HotSpot variant on this machine
 * uses a guard outside that set. Since B1 doesn't call any Thread
 * Manager functions, the simplest fix is to not pull Threads.h in
 * at all. Stage B2 will revisit if cooperative yielding is needed.
 */
extern OTClientContextPtr g_ostls_ot_context;
#else
/* Non-CW8 path (Linux Retro68 syntax check). Stub the OT types and
 * functions just enough for the file to parse. Real linkage is
 * CW8-only. */
typedef long OSStatus;
typedef void *EndpointRef;
typedef void *OTConfigurationRef;
typedef short OTByteCount;
typedef struct {
    OTByteCount maxlen;
    OTByteCount len;
    unsigned char *buf;
} TNetbuf;
typedef struct {
    TNetbuf addr;
    TNetbuf opt;
    TNetbuf udata;
    long sequence;
} TCall;
typedef struct {
    unsigned short fAddressType;
    char fName[1];
} DNSAddress;
#define noErr 0
static OTConfigurationRef OTCreateConfiguration(const char *s){(void)s;return (OTConfigurationRef)1;}
static EndpointRef OTOpenEndpointInContext(OTConfigurationRef c,unsigned long f,void *p,OSStatus *e,void *x){(void)c;(void)f;(void)p;(void)x;*e=noErr;return (EndpointRef)1;}
static OSStatus OTSetSynchronous(EndpointRef e){(void)e;return noErr;}
static OSStatus OTSetBlocking(EndpointRef e){(void)e;return noErr;}
static OSStatus OTBind(EndpointRef e,void *a,void *b){(void)e;(void)a;(void)b;return noErr;}
static OSStatus OTConnect(EndpointRef e,TCall *c,void *r){(void)e;(void)c;(void)r;return noErr;}
static OSStatus OTSndOrderlyDisconnect(EndpointRef e){(void)e;return noErr;}
static OSStatus OTCloseProvider(EndpointRef e){(void)e;return noErr;}
static long OTInitDNSAddress(DNSAddress *d,const char *s){(void)d;(void)s;return 0;}
static void OTMemzero(void *p,unsigned long n){memset(p,0,n);}
extern void *g_ostls_ot_context;
#endif


/*
 * Format an OT/OSStatus value into a status message buffer. Caller
 * provides the buffer; we always NUL-terminate. The prefix is bounded
 * by %.Ns so a runaway prefix can't overrun out_len, even though every
 * call site supplies a short literal. C89 has no snprintf.
 */
static void
status_set(char *out, size_t out_len, const char *prefix, long ot_err)
{
    if (out == NULL || out_len < 4) {
        return;
    }
    if (ot_err == 0) {
        sprintf(out, "%.120s", prefix);
    } else {
        sprintf(out, "%.100s (ot_err=%ld)", prefix, ot_err);
    }
    out[out_len - 1] = '\0';
}


OSErr
OSTLS_B1_TCP_Probe(const char *target_host_port,
                   char *out_msg, size_t out_msg_len)
{
    OTConfigurationRef cfg;
    EndpointRef ep;
    OSStatus oterr;
    DNSAddress dns;
    TCall call;
    long dns_len;

    if (target_host_port == NULL || target_host_port[0] == '\0') {
        status_set(out_msg, out_msg_len, "B1: bad args (no target)", 0);
        return (OSErr)kOSTLSB1_BadArgs;
    }

    /* OTCreateConfiguration: returns kOTInvalidConfigurationPtr on
     * failure. In CW8 OT headers that constant may not be defined, so
     * compare against the well-documented sentinel of "any value that
     * is not a valid configuration." OT spec: NULL is reserved as a
     * possible failure return when memory is short. */
    cfg = OTCreateConfiguration("tcp");
    if (cfg == NULL || cfg == (OTConfigurationRef)-1L) {
        status_set(out_msg, out_msg_len,
            "B1: OTCreateConfiguration FAIL", 0);
        return (OSErr)kOSTLSB1_OTConfigFail;
    }

    oterr = noErr;
    ep = OTOpenEndpointInContext(cfg, 0, NULL, &oterr, g_ostls_ot_context);
    if (oterr != noErr || ep == NULL) {
        status_set(out_msg, out_msg_len,
            "B1: OTOpenEndpoint FAIL", (long)oterr);
        return (OSErr)kOSTLSB1_OTOpenEndptFail;
    }

    /* Sync + blocking matches MacSurf's connect path. OTConnect will
     * not return until the TCP three-way handshake completes or the
     * stack times out internally (~30s for unreachable hosts on most
     * OT stacks). */
    OTSetSynchronous(ep);
    OTSetBlocking(ep);

    oterr = OTBind(ep, NULL, NULL);
    if (oterr != noErr) {
        status_set(out_msg, out_msg_len, "B1: OTBind FAIL", (long)oterr);
        OTCloseProvider(ep);
        return (OSErr)kOSTLSB1_OTBindFail;
    }

    /* OTInitDNSAddress fills a DNSAddress with the host:port string
     * and returns the address length to put in TCall.addr.len. It
     * does NOT actually resolve DNS at this point -- that happens
     * inside OTConnect when the OT TCP module hands the address to
     * the resolver. Return value is the byte length; OT examples
     * indicate it never fails per se, but a zero return is suspect. */
    OTMemzero(&call, sizeof(call));
    dns_len = OTInitDNSAddress(&dns, (char *)target_host_port);
    if (dns_len <= 0) {
        status_set(out_msg, out_msg_len,
            "B1: OTInitDNSAddress FAIL", dns_len);
        OTCloseProvider(ep);
        return (OSErr)kOSTLSB1_OTDnsAddrFail;
    }
    call.addr.buf = (unsigned char *)&dns;
    call.addr.len = (short)dns_len;

    /* The actual connect. Blocks until success or OT-level failure. */
    oterr = OTConnect(ep, &call, NULL);
    if (oterr != noErr) {
        status_set(out_msg, out_msg_len, "B1: OTConnect FAIL", (long)oterr);
        OTCloseProvider(ep);
        return (OSErr)kOSTLSB1_OTConnectFail;
    }

    /* Connect succeeded. Tear down cleanly. We do NOT send any
     * bytes -- this is purely a transport-layer reachability probe.
     * Orderly disconnect on our side closes the half-stream; the peer
     * will likely close its half too on seeing no application data,
     * but we don't wait for it. OTCloseProvider releases the endpoint
     * back to OT regardless of peer state. */
    status_set(out_msg, out_msg_len, "B1: OT TCP connect OK", 0);

    OTSndOrderlyDisconnect(ep);
    OTCloseProvider(ep);

    return (OSErr)kOSTLSB1_OK;
}
