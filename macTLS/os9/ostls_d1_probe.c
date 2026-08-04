/*
 * ostls_d1_probe.c -- Stage D1 async OT notifier smoke. See header.
 *
 * Tests in isolation: open the endpoint async, bind it async,
 * connect it async, observe each notifier event in turn. No BearSSL
 * involvement; this is purely a "does the async OT plumbing work
 * end-to-end on this Mac?" check.
 *
 * Pattern mirrors Apple's HTTP Server sample
 * (Vinnie Moscaritolo's TNetworkAcceptor) but for an outbound
 * client. Same notifier conventions (interrupt-time, sets flags
 * only, OTRcvConnect at interrupt for T_CONNECT).
 */

#include "ostls_d1_probe.h"
#include "ostls_log.h"

#include <stdio.h>
#include <string.h>

#ifdef __MWERKS__
#include <Types.h>
#include <Events.h>
#include <Files.h>
#include <OpenTransport.h>
#include <OpenTptInternet.h>
extern OTClientContextPtr g_ostls_ot_context;
#else
/* Retro68 stubs. Not run on Linux; only needed for syntax. */
typedef long OSStatus;
typedef long OTResult;
typedef long OTEventCode;
typedef void *EndpointRef;
typedef void *OTConfigurationRef;
typedef short OTByteCount;
typedef unsigned char UInt8;
typedef unsigned long UInt32;
#ifndef true
#define true 1
#define false 0
#endif
#ifdef __MWERKS__
#endif
typedef struct { OTByteCount maxlen, len; UInt8 *buf; } TNetbuf;
typedef struct { TNetbuf addr; TNetbuf opt; long qlen; } TBind;
typedef struct { TNetbuf addr; TNetbuf opt; TNetbuf udata; long sequence; } TCall;
typedef struct { unsigned short fAddressType; char fName[1]; } DNSAddress;
typedef struct {
    long addr, options, tsdu, etsdu, connect, discon;
    long servtype, flags;
} TEndpointInfo;
typedef void (*OTNotifyProcPtr)(void *, OTEventCode, OTResult, void *);
#define noErr 0
#define kOTNoError 0
#define kOTNoDataErr (-3162)
#define T_OPENCOMPLETE 0x20000001L
#define T_BINDCOMPLETE 0x20000002L
#define T_CONNECT      0x00000002L
#define T_DISCONNECT   0x00000010L
static OTConfigurationRef OTCreateConfiguration(const char *s){(void)s;return (OTConfigurationRef)1;}
static OSStatus OTAsyncOpenEndpointInContext(OTConfigurationRef c,unsigned long f,TEndpointInfo *i,OTNotifyProcPtr p,void *x,void *ctx){(void)c;(void)f;(void)i;(void)p;(void)x;(void)ctx;return noErr;}
static OSStatus OTBind(EndpointRef e,TBind *r,TBind *o){(void)e;(void)r;(void)o;return noErr;}
static OSStatus OTConnect(EndpointRef e,TCall *c,void *r){(void)e;(void)c;(void)r;return noErr;}
static OSStatus OTSndOrderlyDisconnect(EndpointRef e){(void)e;return noErr;}
static OSStatus OTRcvConnect(EndpointRef e,TCall *c){(void)e;(void)c;return noErr;}
static OSStatus OTCloseProvider(EndpointRef e){(void)e;return noErr;}
static long OTInitDNSAddress(DNSAddress *d,const char *s){(void)d;(void)s;return 0;}
static void OTMemzero(void *p,unsigned long n){memset(p,0,n);}
static unsigned long TickCount(void){return 0;}
extern void *g_ostls_ot_context;
#endif


/* ----------------------------------------------------------------- */
/* Notifier state -- shared between interrupt and app time           */
/* ----------------------------------------------------------------- */

typedef struct {
    EndpointRef       ep;            /* set in notifier at T_OPENCOMPLETE */
    TBind             bind_req;       /* stable storage for async OTBind */
    TBind             bind_ret;
    TCall             connect_call;
    DNSAddress        dns_addr;
    TEndpointInfo     ep_info;

    volatile Boolean  open_complete;
    volatile Boolean  bind_complete;
    volatile Boolean  connect_complete;
    volatile Boolean  disconnect_seen;
    volatile OTResult last_open_result;
    volatile OTResult last_bind_result;
    volatile OTResult last_connect_result;
    volatile OTEventCode last_event;
} D1State;


static pascal void
d1_notifier(void *context, OTEventCode event,
            OTResult result, void *cookie)
{
    D1State *st;

    st = (D1State *)context;
    if (st == NULL) return;
    st->last_event = event;

    switch (event) {
    case T_OPENCOMPLETE:
        st->ep = (EndpointRef)cookie;
        st->last_open_result = result;
        st->open_complete = true;
        break;
    case T_BINDCOMPLETE:
        st->last_bind_result = result;
        st->bind_complete = true;
        break;
    case T_CONNECT:
        /* Consume the connect event at interrupt time per Apple
         * convention. Safe per OT docs. */
        OTRcvConnect(st->ep, NULL);
        st->last_connect_result = result;
        st->connect_complete = true;
        break;
    case T_DISCONNECT:
        st->disconnect_seen = true;
        break;
    default:
        break;
    }
}


/* ----------------------------------------------------------------- */
/* Helper: spin on a flag with timeout                               */
/* ----------------------------------------------------------------- */

static int
d1_wait_flag(volatile Boolean *flag, UInt32 timeout_ticks,
             volatile Boolean *abort_on_disconnect)
{
    UInt32 deadline;

    deadline = (UInt32)TickCount() + timeout_ticks;
    while (!(*flag)) {
        if ((UInt32)TickCount() > deadline) {
            return -1;  /* timeout */
        }
        if (abort_on_disconnect != NULL && *abort_on_disconnect) {
            return -2;  /* disconnect fired */
        }
    }
    return 0;
}


/* ----------------------------------------------------------------- */
/* Probe                                                             */
/* ----------------------------------------------------------------- */

OSErr
OSTLS_D1_AsyncNotifier_Probe(const char *target_host_port,
                             char *out_msg, size_t out_msg_len)
{
    D1State st;
    OTConfigurationRef cfg;
    OSStatus oterr;
    long dns_len;
    int wait_rc;

    if (target_host_port == NULL || target_host_port[0] == '\0' ||
        out_msg == NULL || out_msg_len < 4) {
        if (out_msg != NULL && out_msg_len > 0) {
            sprintf(out_msg, "D1: bad args");
            out_msg[out_msg_len - 1] = '\0';
        }
        return (OSErr)kOSTLSD1_BadArgs;
    }

    memset(&st, 0, sizeof st);

    cfg = OTCreateConfiguration("tcp");
    if (cfg == NULL || cfg == (OTConfigurationRef)-1L) {
        sprintf(out_msg, "D1: OTCreateConfiguration FAIL");
        out_msg[out_msg_len - 1] = '\0';
        return (OSErr)kOSTLSD1_OTConfigFail;
    }

    /* Kick off async open. The notifier will receive T_OPENCOMPLETE
     * with the new EndpointRef in `cookie`. */
    oterr = OTAsyncOpenEndpointInContext(cfg, 0, &st.ep_info,
                                         NewOTNotifyUPP(d1_notifier),
                                         &st,
                                         g_ostls_ot_context);
    if (oterr != noErr) {
        sprintf(out_msg, "D1: OTAsyncOpenEndpoint FAIL (err=%ld)",
                (long)oterr);
        out_msg[out_msg_len - 1] = '\0';
        return (OSErr)kOSTLSD1_OTAsyncOpenFail;
    }

    /* Wait for T_OPENCOMPLETE -- 30 s deadline. */
    wait_rc = d1_wait_flag(&st.open_complete, 30UL * 60UL,
                           &st.disconnect_seen);
    if (wait_rc == -1) {
        sprintf(out_msg, "D1: T_OPENCOMPLETE timeout (30s)");
        out_msg[out_msg_len - 1] = '\0';
        return (OSErr)kOSTLSD1_OpenTimeout;
    }
    OSTLS_LogLinef("D1 diag    T_OPENCOMPLETE event=0x%lX result=%ld ep=%p",
                   (long)st.last_event,
                   (long)st.last_open_result,
                   (void *)st.ep);
    if (st.last_open_result != noErr || st.ep == NULL) {
        sprintf(out_msg, "D1: T_OPENCOMPLETE result=%ld (no ep)",
                (long)st.last_open_result);
        out_msg[out_msg_len - 1] = '\0';
        return (OSErr)kOSTLSD1_OpenBadResult;
    }

    /* Async bind with NULL,NULL -- outbound client. */
    oterr = OTBind(st.ep, NULL, NULL);
    if (oterr != noErr && oterr != kOTNoError) {
        sprintf(out_msg, "D1: OTBind submit FAIL (err=%ld)",
                (long)oterr);
        out_msg[out_msg_len - 1] = '\0';
        OTCloseProvider(st.ep);
        return (OSErr)kOSTLSD1_OTBindSubmitFail;
    }

    wait_rc = d1_wait_flag(&st.bind_complete, 30UL * 60UL,
                           &st.disconnect_seen);
    if (wait_rc == -1) {
        sprintf(out_msg, "D1: T_BINDCOMPLETE timeout (30s)");
        out_msg[out_msg_len - 1] = '\0';
        OTCloseProvider(st.ep);
        return (OSErr)kOSTLSD1_BindTimeout;
    }
    OSTLS_LogLinef("D1 diag    T_BINDCOMPLETE event=0x%lX result=%ld",
                   (long)st.last_event,
                   (long)st.last_bind_result);
    if (st.last_bind_result != noErr) {
        sprintf(out_msg, "D1: T_BINDCOMPLETE result=%ld",
                (long)st.last_bind_result);
        out_msg[out_msg_len - 1] = '\0';
        OTCloseProvider(st.ep);
        return (OSErr)kOSTLSD1_BindBadResult;
    }

    /* Build DNS address from "host:port" string. */
    OTMemzero(&st.dns_addr, sizeof st.dns_addr);
    OTMemzero(&st.connect_call, sizeof st.connect_call);
    dns_len = OTInitDNSAddress(&st.dns_addr, (char *)target_host_port);
    if (dns_len <= 0) {
        sprintf(out_msg, "D1: OTInitDNSAddress FAIL (%ld)", dns_len);
        out_msg[out_msg_len - 1] = '\0';
        OTSndOrderlyDisconnect(st.ep);
        OTCloseProvider(st.ep);
        return (OSErr)kOSTLSD1_OTDnsAddrFail;
    }
    st.connect_call.addr.buf = (UInt8 *)&st.dns_addr;
    st.connect_call.addr.len = (short)dns_len;

    /* Async connect. */
    oterr = OTConnect(st.ep, &st.connect_call, NULL);
    if (oterr != noErr && oterr != kOTNoError &&
        oterr != kOTNoDataErr) {
        sprintf(out_msg, "D1: OTConnect submit FAIL (err=%ld)",
                (long)oterr);
        out_msg[out_msg_len - 1] = '\0';
        OTSndOrderlyDisconnect(st.ep);
        OTCloseProvider(st.ep);
        return (OSErr)kOSTLSD1_OTConnectSubmitFail;
    }

    wait_rc = d1_wait_flag(&st.connect_complete, 30UL * 60UL,
                           &st.disconnect_seen);
    if (wait_rc == -2) {
        sprintf(out_msg, "D1: T_DISCONNECT during connect");
        out_msg[out_msg_len - 1] = '\0';
        OTCloseProvider(st.ep);
        return (OSErr)kOSTLSD1_DisconnectArrived;
    }
    if (wait_rc == -1) {
        sprintf(out_msg, "D1: T_CONNECT timeout (30s)");
        out_msg[out_msg_len - 1] = '\0';
        OTSndOrderlyDisconnect(st.ep);
        OTCloseProvider(st.ep);
        return (OSErr)kOSTLSD1_ConnectTimeout;
    }
    OSTLS_LogLinef("D1 diag    T_CONNECT event=0x%lX result=%ld",
                   (long)st.last_event,
                   (long)st.last_connect_result);
    if (st.last_connect_result != noErr) {
        sprintf(out_msg, "D1: T_CONNECT result=%ld",
                (long)st.last_connect_result);
        out_msg[out_msg_len - 1] = '\0';
        OTSndOrderlyDisconnect(st.ep);
        OTCloseProvider(st.ep);
        return (OSErr)kOSTLSD1_ConnectBadResult;
    }

    /* Success path: tear down cleanly. */
    sprintf(out_msg, "D1 OK async notifier dispatched 3/3 events (%s)",
            target_host_port);
    out_msg[out_msg_len - 1] = '\0';

    OTSndOrderlyDisconnect(st.ep);
    OTCloseProvider(st.ep);
    return (OSErr)kOSTLSD1_OK;
}
