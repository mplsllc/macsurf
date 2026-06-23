/*
 * ostls_c1_listener.c -- Stage C1 OT TCP listener probe. See header.
 *
 * OT server pattern (synchronous + blocking):
 *
 *   1. listener = OTOpenEndpointInContext("tcp", ...)
 *   2. OTSetSynchronous / OTSetBlocking
 *   3. OTInitInetAddress(&addr, port, 0)  -- 0 = INADDR_ANY
 *   4. TBind req = { .qlen = 1, .addr = { &addr, sizeof addr } }
 *      OTBind(listener, &req, NULL)
 *   5. TCall call = { .addr = { peer_addr_buf, sizeof peer_addr_buf } }
 *      OTListen(listener, &call)     -- blocks until peer arrives
 *   6. child = OTOpenEndpointInContext("tcp", ...)
 *   7. OTBind(child, NULL, NULL)    -- ephemeral local addr
 *   8. OTAccept(listener, child, &call)
 *   9. OTRcv(child, buf, len, NULL) repeatedly until 0 or buf full
 *  10. OTSndOrderlyDisconnect / OTCloseProvider on both endpoints
 *
 * The InetAddress and TBind structs come from OpenTptInternet.h.
 * 0.0.0.0:port is the safest INADDR_ANY equivalent on classic OT;
 * binding strictly to 127.0.0.1 is unreliable across OT versions.
 * Firewalls and AppleTalk filters are the operator's concern.
 */

#include "ostls_c1_listener.h"
#include "ostls_log.h"

#include <stdio.h>
#include <string.h>

#ifdef __MWERKS__
#include <Files.h>
#include <OpenTransport.h>
#include <OpenTptInternet.h>
extern OTClientContextPtr g_ostls_ot_context;
#else
/* Non-CW8 syntax-check stubs. */
typedef long OSStatus;
typedef long OTResult;
typedef void *EndpointRef;
typedef void *OTConfigurationRef;
typedef short OTByteCount;
typedef unsigned char UInt8;
typedef unsigned short InetPort;
typedef unsigned long InetHost;
typedef struct { OTByteCount maxlen, len; UInt8 *buf; } TNetbuf;
typedef struct { TNetbuf addr; TNetbuf opt; long qlen; } TBind;
typedef struct { TNetbuf addr; TNetbuf opt; TNetbuf udata; long sequence; } TCall;
typedef struct {
    long addr, options, tsdu, etsdu, connect, discon;
    long servtype, flags;
} TEndpointInfo;
typedef struct {
    unsigned short fAddressType;
    InetPort       fPort;
    InetHost       fHost;
    char           fUnused[8];
} InetAddress;
#define noErr 0
#define AF_INET 2
#define kAFInet AF_INET
static OTConfigurationRef OTCreateConfiguration(const char *s){(void)s;return (OTConfigurationRef)1;}
static EndpointRef OTOpenEndpointInContext(OTConfigurationRef c,unsigned long f,void *p,OSStatus *e,void *x){(void)c;(void)f;(void)p;(void)x;*e=noErr;return (EndpointRef)1;}
static EndpointRef OTOpenEndpoint(OTConfigurationRef c,unsigned long f,void *p,OSStatus *e){(void)c;(void)f;(void)p;*e=noErr;return (EndpointRef)1;}
static OSStatus OTSetSynchronous(EndpointRef e){(void)e;return noErr;}
static OSStatus OTSetBlocking(EndpointRef e){(void)e;return noErr;}
static OSStatus OTBind(EndpointRef e,TBind *r,TBind *o){(void)e;(void)r;(void)o;return noErr;}
static OSStatus OTUnbind(EndpointRef e){(void)e;return noErr;}
static OSStatus OTListen(EndpointRef e,TCall *c){(void)e;(void)c;return noErr;}
static OSStatus OTAccept(EndpointRef l,EndpointRef c,TCall *call){(void)l;(void)c;(void)call;return noErr;}
static OTResult OTRcv(EndpointRef e,void *b,long n,long *f){(void)e;(void)b;(void)n;(void)f;return 0;}
static OSStatus OTSndOrderlyDisconnect(EndpointRef e){(void)e;return noErr;}
static OSStatus OTCloseProvider(EndpointRef e){(void)e;return noErr;}
static void OTInitInetAddress(InetAddress *a, InetPort p, InetHost h){(void)a;(void)p;(void)h;}
static void OTMemzero(void *p,unsigned long n){memset(p,0,n);}
/* pascal is already a Retro68 built-in keyword; no #define needed */
typedef long OTEventCode;
typedef void (*OTNotifyProcPtr)(void *, OTEventCode, OTResult, void *);
#define T_BINDCOMPLETE 4
#define kOTNoError 0
#define kDefaultInetInterface 0
typedef struct {
    InetHost fAddress;
    InetHost fNetmask;
    InetHost fBroadcastAddr;
    InetHost fDefaultGatewayAddr;
    InetHost fDNSAddr;
    unsigned short fVersion;
    unsigned short fHWAddrLen;
    unsigned char *fHWAddr;
    unsigned long fIfMTU;
    unsigned char *fReservedPtrs[2];
    char fDomainName[256];
    unsigned long fIPSecondaryCount;
    unsigned char fReserved[252];
} InetInterfaceInfo;
static OSStatus OTInstallNotifier(EndpointRef e, OTNotifyProcPtr p, void *c){(void)e;(void)p;(void)c;return noErr;}
static OSStatus OTSetAsynchronous(EndpointRef e){(void)e;return noErr;}
static OSStatus OTInetGetInterfaceInfo(InetInterfaceInfo *info, long which){(void)info;(void)which;return noErr;}
static unsigned long TickCount(void){return 0;}
extern void *g_ostls_ot_context;
#endif


/*
 * Notifier-driven async bind state.
 *
 * Apple's HTTP Server sample uses pure-async OT throughout: every
 * call returns immediately, completion arrives via a notifier
 * callback that runs at interrupt time. The notifier can only
 * touch globals -- no Toolbox calls, no memory allocation.
 *
 * For Stage C1's single-shot probe we use a hybrid: open and most
 * calls remain synchronous, but the bind specifically is performed
 * in async mode (OTSetAsynchronous before OTBind, then poll a
 * flag the notifier sets at T_BINDCOMPLETE). Five rounds of sync
 * OTBind returning -3150 against a textbook-correct address point
 * to CarbonLib's OT sync wrapper rejecting passive binds; async
 * bind goes through a different code path internally.
 */
static volatile int g_c1_bind_complete = 0;
static volatile long g_c1_bind_result  = 0;
static volatile long g_c1_last_event   = 0;


/*
 * OT notifier callback. Runs at INTERRUPT TIME.
 *
 *   - MUST NOT call Toolbox (no OSTLS_LogLinef, no memory ops).
 *   - MAY set volatile globals.
 *   - MAY copy small amounts of data.
 *
 * We only care about T_BINDCOMPLETE in this probe -- it signals
 * the result of the async OTBind we kicked off.
 */
static pascal void
c1_bind_notifier(void *context, OTEventCode event,
                 OTResult result, void *cookie)
{
    (void)context;
    (void)cookie;
    g_c1_last_event = (long)event;
    if (event == T_BINDCOMPLETE) {
        g_c1_bind_result   = (long)result;
        g_c1_bind_complete = 1;
    }
}


/* ----------------------------------------------------------------- */
/* Status helper                                                     */
/* ----------------------------------------------------------------- */

static void
c1_status(char *out, size_t out_len, const char *prefix, long err)
{
    if (out == NULL || out_len < 4) {
        return;
    }
    if (err == 0) {
        sprintf(out, "%.140s", prefix);
    } else {
        sprintf(out, "%.110s (err=%ld)", prefix, err);
    }
    out[out_len - 1] = '\0';
}


/* ----------------------------------------------------------------- */
/* Probe                                                             */
/* ----------------------------------------------------------------- */

OSErr
OSTLS_C1_Listener_Probe(unsigned short port,
                        char *out_request, size_t out_request_cap,
                        char *out_msg, size_t out_msg_len)
{
    OTConfigurationRef cfg_listener;
    OTConfigurationRef cfg_child;
    EndpointRef listener_ep;
    EndpointRef child_ep;
    OSStatus oterr;
    InetAddress local_addr;
    InetAddress peer_addr;
    TBind bind_req;
    TCall call;
    size_t received;
    OTResult got;

    if (out_request == NULL || out_request_cap < 2 || port == 0) {
        c1_status(out_msg, out_msg_len, "C1: bad args", 0);
        return (OSErr)kOSTLSC1_BadArgs;
    }
    out_request[0] = '\0';
    received = 0;

    /* ----- 1. Open listener endpoint -----
     *
     * Plain "tcp" -- NOT "tilisten,tcp". Apple TN1145
     * ("Living in a Dynamic TCP/IP Environment") shows the canonical
     * OT TCP server bind in DoIncomingBindOT, and it uses plain
     * "tcp" for the listener. tilisten is a Solaris-derived STREAMS
     * connection-orderer module; on classic Mac OS its presence
     * in the stack actually breaks passive binds because it
     * rewrites the address-format expectations on its lower service
     * interface. The "passive" behavior of an OT TCP endpoint comes
     * from qlen >= 1 at bind time, not from a different protocol
     * module on top.
     *
     * Earlier rounds (fixes16 plain "tcp", fixes21 "tilisten,tcp")
     * both failed with -3150 -- the protocol string wasn't the
     * issue, but tilisten was definitely the wrong path. Going
     * back to plain "tcp" with the corrected bind request below.
     */
    cfg_listener = OTCreateConfiguration("tcp");
    if (cfg_listener == NULL || cfg_listener == (OTConfigurationRef)-1L) {
        c1_status(out_msg, out_msg_len,
            "C1: OTCreateConfiguration FAIL (listener)", 0);
        return (OSErr)kOSTLSC1_OTConfigFail;
    }

    oterr = noErr;
    {
        TEndpointInfo ep_info;
        OTMemzero(&ep_info, sizeof ep_info);
        /*
         * Back to OTOpenEndpointInContext -- fixes28 tried plain
         * OTOpenEndpoint but the Carbon CFM linker can't resolve it
         * (CarbonLib only exports the InContext variant). The Carbon
         * application context set up by InitOpenTransportInContext
         * is what we have to live with.
         */
        listener_ep = OTOpenEndpointInContext(cfg_listener, 0, &ep_info,
                                              &oterr, g_ostls_ot_context);
        if (oterr != noErr || listener_ep == NULL) {
            OSTLS_LogLinef("C1 diag    OTOpenEndpoint failed err=%ld",
                           (long)oterr);
            c1_status(out_msg, out_msg_len,
                "C1: OTOpenEndpoint FAIL (listener)", (long)oterr);
            return (OSErr)kOSTLSC1_OTOpenEndptFail;
        }
        OSTLS_LogLinef("C1 diag    endpoint info: addr=%ld options=%ld tsdu=%ld",
                       (long)ep_info.addr,
                       (long)ep_info.options,
                       (long)ep_info.tsdu);
        OSTLS_LogLinef("C1 diag    endpoint info: etsdu=%ld connect=%ld discon=%ld",
                       (long)ep_info.etsdu,
                       (long)ep_info.connect,
                       (long)ep_info.discon);
        OSTLS_LogLinef("C1 diag    endpoint info: servtype=%ld (2=T_COTS_ORD) flags=0x%lX",
                       (long)ep_info.servtype,
                       (long)ep_info.flags);
    }
    /*
     * Diagnostic: query the IP stack's current interface info BEFORE
     * binding. The OT documentation says kOTBadAddressErr from OTBind
     * for TCP means "the address does not exist in the specified
     * domain" -- in practical terms, the IP stack has no interface
     * configured for the address we asked for. If fAddress == 0 here,
     * the TCP/IP control panel isn't fully up and INADDR_ANY has
     * literally no domain to bind to. (Outbound B1-B4 succeeds anyway
     * because OTConnect resolves through DNS / OT's outbound path
     * which doesn't validate the local bind address the same way.)
     */
    {
        InetInterfaceInfo ifc;
        OSStatus iferr;
        OTMemzero(&ifc, sizeof ifc);
        iferr = OTInetGetInterfaceInfo(&ifc, kDefaultInetInterface);
        OSTLS_LogLinef("C1 diag    OTInetGetInterfaceInfo err=%ld fAddress=0x%08lX",
                       (long)iferr, (unsigned long)ifc.fAddress);
        OSTLS_LogLinef("C1 diag      netmask=0x%08lX gateway=0x%08lX dns=0x%08lX",
                       (unsigned long)ifc.fNetmask,
                       (unsigned long)ifc.fDefaultGatewayAddr,
                       (unsigned long)ifc.fDNSAddr);
    }

    /*
     * Install the bind-completion notifier first. We then switch
     * to async mode so OTBind goes through OT's async dispatch
     * path -- five rounds of sync OTBind returned -3150 against
     * textbook-correct addresses; the only major behavioural
     * difference vs. Apple's working sample is the sync wrapper.
     */
    g_c1_bind_complete = 0;
    g_c1_bind_result   = 0;
    g_c1_last_event    = 0;

    oterr = OTInstallNotifier(listener_ep,
                              (OTNotifyProcPtr)c1_bind_notifier, NULL);
    if (oterr != noErr) {
        OSTLS_LogLinef("C1 diag    OTInstallNotifier err=%ld", (long)oterr);
        c1_status(out_msg, out_msg_len, "C1: OTInstallNotifier FAIL",
                  (long)oterr);
        OTCloseProvider(listener_ep);
        return (OSErr)kOSTLSC1_OTBindFail;
    }

    oterr = OTSetAsynchronous(listener_ep);
    if (oterr != noErr) {
        OSTLS_LogLinef("C1 diag    OTSetAsynchronous err=%ld", (long)oterr);
        c1_status(out_msg, out_msg_len, "C1: OTSetAsynchronous FAIL",
                  (long)oterr);
        OTCloseProvider(listener_ep);
        return (OSErr)kOSTLSC1_OTBindFail;
    }

    /* ----- 2. Bind on 0.0.0.0:port with backlog 1 ----- */

    OTMemzero(&local_addr, sizeof local_addr);
    /*
     * Mirror Apple TN1145 "DoIncomingBindOT":
     *   OTInitInetAddress(&reqAddr, port, kOTAnyInetAddress);
     *
     * kOTAnyInetAddress (0) is the bind-any address. Earlier rounds
     * tried 127.0.0.1 — TN1145 explicitly says some OT TCP stack
     * revisions reject loopback on the passive bind path because
     * that address isn't on a real interface alias at bind time.
     */
    OTInitInetAddress(&local_addr, (InetPort)port,
                      (InetHost)0UL);

    /*
     * Bind REQUEST side. Set BOTH len AND maxlen to sizeof(InetAddress).
     *
     * TN1145 prose says "maxlen is ignored on input for the request
     * address" -- that's technically true per the OT spec, BUT Apple's
     * own DTS HTTP Server sample (Vinnie Moscaritolo's TAddr::ToNetbuf
     * in /Networking/Http_Server.sit) sets maxlen = sizeof(InetAddress)
     * = 16 on the request side too. Some OT STREAMS plumbing validates
     * len <= maxlen, and when len=16, maxlen=0 the check fails with
     * kOTBadAddressErr (-3150).
     *
     * The previous round (fixes26) was wrong to take TN1145 literally.
     * Apple's actual working code is the authoritative reference.
     */
    OTMemzero(&bind_req, sizeof bind_req);
    bind_req.addr.buf    = (UInt8 *)&local_addr;
    bind_req.addr.len    = (OTByteCount)sizeof local_addr;
    bind_req.addr.maxlen = (OTByteCount)sizeof local_addr;
    bind_req.qlen        = 1L;

    /*
     * Diagnostic: log the exact bytes we're handing OT before the
     * bind call. Two consecutive -3150 (kOTBadAddressErr) failures
     * on hardware after the tilisten-vs-tcp switch made the address
     * itself the next suspect. The log line shows sizeof's plus the
     * 16-byte InetAddress layout so we can confirm fAddressType,
     * fPort and fHost are what we believe we wrote.
     */
    {
        const unsigned char *p = (const unsigned char *)&local_addr;
        OSTLS_LogLinef("C1 diag    sizeof(InetAddress)=%lu sizeof(TBind)=%lu qlen=%ld",
                       (unsigned long)sizeof local_addr,
                       (unsigned long)sizeof bind_req,
                       (long)bind_req.qlen);
        OSTLS_LogLinef("C1 diag    addr.maxlen=%ld addr.len=%ld addr.buf=%p",
                       (long)bind_req.addr.maxlen,
                       (long)bind_req.addr.len,
                       (void *)bind_req.addr.buf);
        OSTLS_LogLinef("C1 diag    addr[0-7]  %02X %02X %02X %02X %02X %02X %02X %02X",
                       (unsigned)p[0], (unsigned)p[1], (unsigned)p[2], (unsigned)p[3],
                       (unsigned)p[4], (unsigned)p[5], (unsigned)p[6], (unsigned)p[7]);
        OSTLS_LogLinef("C1 diag    addr[8-15] %02X %02X %02X %02X %02X %02X %02X %02X",
                       (unsigned)p[8],  (unsigned)p[9],  (unsigned)p[10], (unsigned)p[11],
                       (unsigned)p[12], (unsigned)p[13], (unsigned)p[14], (unsigned)p[15]);
    }

    /*
     * Provide a returned-bind buffer rather than NULL. Some classic
     * OT versions document NULL as acceptable but reject it in
     * practice for passive (qlen >= 1) binds; supplying a valid
     * sink for the actual-bound-address write costs us 24 bytes of
     * stack and removes one ambiguity from the failure modes.
     */
    {
        TBind bind_ret;
        InetAddress bound_addr;
        OTMemzero(&bind_ret, sizeof bind_ret);
        OTMemzero(&bound_addr, sizeof bound_addr);
        bind_ret.addr.buf    = (UInt8 *)&bound_addr;
        bind_ret.addr.maxlen = (OTByteCount)sizeof bound_addr;
        bind_ret.addr.len    = 0;

        /*
         * Async OTBind: returns immediately with kOTNoError (or an
         * immediate validation failure). The actual bind result
         * arrives via the notifier at T_BINDCOMPLETE.
         */
        oterr = OTBind(listener_ep, &bind_req, &bind_ret);
        OSTLS_LogLinef("C1 diag    OTBind submit err=%ld (async dispatch)",
                       (long)oterr);
        if (oterr != noErr && oterr != kOTNoError) {
            c1_status(out_msg, out_msg_len, "C1: OTBind submit FAIL",
                      (long)oterr);
            OTCloseProvider(listener_ep);
            return (OSErr)kOSTLSC1_OTBindFail;
        }

        /*
         * Spin waiting for the notifier to set g_c1_bind_complete.
         * Bound by 30 seconds at 60Hz = 1800 ticks. On a healthy
         * stack T_BINDCOMPLETE fires within milliseconds.
         */
        {
            unsigned long deadline = TickCount() + 30UL * 60UL;
            while (!g_c1_bind_complete) {
                if (TickCount() > deadline) {
                    OSTLS_LogLinef("C1 diag    bind notifier timeout 30s last_event=%ld",
                                   (long)g_c1_last_event);
                    c1_status(out_msg, out_msg_len,
                              "C1: bind notifier timeout (30s)", 0);
                    OTCloseProvider(listener_ep);
                    return (OSErr)kOSTLSC1_OTBindFail;
                }
                /* yield to the OS so the notifier can fire */
            }
        }

        OSTLS_LogLinef("C1 diag    bind notifier fired event=%ld result=%ld",
                       (long)g_c1_last_event, (long)g_c1_bind_result);

        if (g_c1_bind_result != noErr) {
            /*
             * Fallback diagnostic: try OTBind(ep, NULL, NULL) on the
             * SAME endpoint. NULL,NULL gives qlen=0 + OT-assigned
             * local address -- effectively a client-style bind. If
             * this succeeds, the endpoint itself is bindable and the
             * issue is specifically passive (qlen >= 1) + explicit
             * address. If THIS also fails -3150, the endpoint can't
             * be bound at all and the failure is deeper than our
             * code path.
             *
             * Switch back to sync mode for this probe since async
             * notifier already returned the real bind failure above.
             */
            OSStatus probe_err;
            OTSetSynchronous(listener_ep);

            /* Probe A: qlen=0 + NULL addr (client mode, OT picks port). */
            probe_err = OTBind(listener_ep, NULL, NULL);
            OSTLS_LogLinef("C1 diag    probeA OTBind(NULL,NULL) qlen=0 err=%ld %s",
                           (long)probe_err,
                           probe_err == noErr ? "OK" : "FAIL");
            if (probe_err == noErr) {
                /* Need to unbind before next probe. */
                OTUnbind(listener_ep);
            }

            /* Probe B: qlen=0 + EXPLICIT addr (specific port + client mode). */
            {
                TBind probeB_req;
                TBind probeB_ret;
                InetAddress probeB_bound;
                OTMemzero(&probeB_req, sizeof probeB_req);
                probeB_req.addr.buf    = (UInt8 *)&local_addr;
                probeB_req.addr.len    = (OTByteCount)sizeof local_addr;
                probeB_req.addr.maxlen = (OTByteCount)sizeof local_addr;
                probeB_req.qlen        = 0L;
                OTMemzero(&probeB_ret, sizeof probeB_ret);
                OTMemzero(&probeB_bound, sizeof probeB_bound);
                probeB_ret.addr.buf    = (UInt8 *)&probeB_bound;
                probeB_ret.addr.maxlen = (OTByteCount)sizeof probeB_bound;
                probe_err = OTBind(listener_ep, &probeB_req, &probeB_ret);
                OSTLS_LogLinef("C1 diag    probeB OTBind(explicit,qlen=0) err=%ld port=%u",
                               (long)probe_err,
                               (unsigned)probeB_bound.fPort);
                if (probe_err == noErr) {
                    OTUnbind(listener_ep);
                }
            }

            /* Probe C: qlen=1 + NULL addr (server mode, OT-assigned port). */
            {
                TBind probeC_req;
                TBind probeC_ret;
                InetAddress probeC_bound;
                OTMemzero(&probeC_req, sizeof probeC_req);
                probeC_req.addr.buf    = NULL;
                probeC_req.addr.len    = 0;
                probeC_req.addr.maxlen = 0;
                probeC_req.qlen        = 1L;
                OTMemzero(&probeC_ret, sizeof probeC_ret);
                OTMemzero(&probeC_bound, sizeof probeC_bound);
                probeC_ret.addr.buf    = (UInt8 *)&probeC_bound;
                probeC_ret.addr.maxlen = (OTByteCount)sizeof probeC_bound;
                probe_err = OTBind(listener_ep, &probeC_req, &probeC_ret);
                OSTLS_LogLinef("C1 diag    probeC OTBind(NULL,qlen=1) err=%ld port=%u",
                               (long)probe_err,
                               (unsigned)probeC_bound.fPort);
                if (probe_err == noErr) {
                    OTUnbind(listener_ep);
                }
            }

            /*
             * Probe D: bind to the DISCOVERED LAN IP (10.42.0.145 in
             * this Mac's case), not INADDR_ANY. The OT documentation
             * for kOTBadAddressErr on TCP literally says "the address
             * does not exist in the specified domain." INADDR_ANY may
             * not satisfy that for the InContext path. A real
             * configured interface address definitely exists.
             *
             * The local IP came from the OTInetGetInterfaceInfo call
             * earlier; re-fetch it here for the probe (interface_info
             * is local-scope above and not reachable here).
             */
            {
                InetInterfaceInfo ifc;
                TBind probeD_req;
                TBind probeD_ret;
                InetAddress probeD_addr;
                InetAddress probeD_bound;
                OSStatus iferr;

                OTMemzero(&ifc, sizeof ifc);
                iferr = OTInetGetInterfaceInfo(&ifc, kDefaultInetInterface);

                OTMemzero(&probeD_addr, sizeof probeD_addr);
                OTInitInetAddress(&probeD_addr, (InetPort)port,
                                  (InetHost)ifc.fAddress);

                OTMemzero(&probeD_req, sizeof probeD_req);
                probeD_req.addr.buf    = (UInt8 *)&probeD_addr;
                probeD_req.addr.len    = (OTByteCount)sizeof probeD_addr;
                probeD_req.addr.maxlen = (OTByteCount)sizeof probeD_addr;
                probeD_req.qlen        = 1L;

                OTMemzero(&probeD_ret, sizeof probeD_ret);
                OTMemzero(&probeD_bound, sizeof probeD_bound);
                probeD_ret.addr.buf    = (UInt8 *)&probeD_bound;
                probeD_ret.addr.maxlen = (OTByteCount)sizeof probeD_bound;

                OSTLS_LogLinef("C1 diag    probeD bind to fAddress=0x%08lX port=%u",
                               (unsigned long)ifc.fAddress, (unsigned)port);
                probe_err = OTBind(listener_ep, &probeD_req, &probeD_ret);
                OSTLS_LogLinef("C1 diag    probeD OTBind(LAN-IP,qlen=1) err=%ld port=%u",
                               (long)probe_err,
                               (unsigned)probeD_bound.fPort);
            }

            c1_status(out_msg, out_msg_len,
                      "C1: OTBind async FAIL",
                      (long)g_c1_bind_result);
            OTCloseProvider(listener_ep);
            return (OSErr)kOSTLSC1_OTBindFail;
        }

        OSTLS_LogLinef("C1 diag    OTBind OK actual port=%u host=0x%08lX",
                       (unsigned)bound_addr.fPort,
                       (unsigned long)bound_addr.fHost);

        /* Switch back to sync mode for the remaining OTListen /
         * OTAccept / OTRcv calls -- those work fine sync in
         * outbound code (we just don't normally use them inbound). */
        OTSetSynchronous(listener_ep);
        OTSetBlocking(listener_ep);
    }

    /* ----- 3. OTListen -- blocks until a peer arrives ----- */

    OTMemzero(&peer_addr, sizeof peer_addr);
    OTMemzero(&call, sizeof call);
    call.addr.buf    = (UInt8 *)&peer_addr;
    call.addr.maxlen = (OTByteCount)sizeof peer_addr;
    call.addr.len    = 0;

    oterr = OTListen(listener_ep, &call);
    if (oterr != noErr) {
        c1_status(out_msg, out_msg_len, "C1: OTListen FAIL", (long)oterr);
        OTCloseProvider(listener_ep);
        return (OSErr)kOSTLSC1_OTListenFail;
    }

    /* ----- 4. Open child endpoint, bind it, accept ----- */

    cfg_child = OTCreateConfiguration("tcp");
    oterr = noErr;
    child_ep = OTOpenEndpointInContext(cfg_child, 0, NULL,
                                       &oterr, g_ostls_ot_context);
    if (oterr != noErr || child_ep == NULL) {
        c1_status(out_msg, out_msg_len,
            "C1: OTOpenEndpoint FAIL (child)", (long)oterr);
        OTCloseProvider(listener_ep);
        return (OSErr)kOSTLSC1_OTAcceptOpenFail;
    }
    OTSetSynchronous(child_ep);
    OTSetBlocking(child_ep);

    oterr = OTBind(child_ep, NULL, NULL);
    if (oterr != noErr) {
        c1_status(out_msg, out_msg_len,
            "C1: OTBind FAIL (child)", (long)oterr);
        OTCloseProvider(child_ep);
        OTCloseProvider(listener_ep);
        return (OSErr)kOSTLSC1_OTAcceptBindFail;
    }

    oterr = OTAccept(listener_ep, child_ep, &call);
    if (oterr != noErr) {
        c1_status(out_msg, out_msg_len, "C1: OTAccept FAIL", (long)oterr);
        OTCloseProvider(child_ep);
        OTCloseProvider(listener_ep);
        return (OSErr)kOSTLSC1_OTAcceptFail;
    }

    /* ----- 5. Drain client request into out_request ----- */

    while (received + 1 < out_request_cap) {
        long want = (long)((out_request_cap - 1) - received);
        got = OTRcv(child_ep, out_request + received, want, NULL);
        if (got < 0) {
            c1_status(out_msg, out_msg_len, "C1: OTRcv FAIL", (long)got);
            OTSndOrderlyDisconnect(child_ep);
            OTCloseProvider(child_ep);
            OTCloseProvider(listener_ep);
            return (OSErr)kOSTLSC1_OTRcvFail;
        }
        if (got == 0) {
            /* Peer closed. */
            break;
        }
        received += (size_t)got;
    }
    out_request[received] = '\0';

    if (received == 0) {
        c1_status(out_msg, out_msg_len,
            "C1: peer connected but sent no bytes", 0);
        OTSndOrderlyDisconnect(child_ep);
        OTCloseProvider(child_ep);
        OTCloseProvider(listener_ep);
        return (OSErr)kOSTLSC1_PeerClosedEmpty;
    }

    /* ----- 6. Success ----- */

    {
        unsigned int peer_h_a, peer_h_b, peer_h_c, peer_h_d;
        peer_h_a = (unsigned int)((peer_addr.fHost >> 24) & 0xFFU);
        peer_h_b = (unsigned int)((peer_addr.fHost >> 16) & 0xFFU);
        peer_h_c = (unsigned int)((peer_addr.fHost >>  8) & 0xFFU);
        peer_h_d = (unsigned int)((peer_addr.fHost      ) & 0xFFU);
        sprintf(out_msg,
            "C1 OK port=%u peer=%u.%u.%u.%u:%u bytes=%lu",
            (unsigned)port,
            peer_h_a, peer_h_b, peer_h_c, peer_h_d,
            (unsigned)peer_addr.fPort,
            (unsigned long)received);
        out_msg[out_msg_len - 1] = '\0';
    }

    OTSndOrderlyDisconnect(child_ep);
    OTCloseProvider(child_ep);
    OTCloseProvider(listener_ep);
    return (OSErr)kOSTLSC1_OK;
}
