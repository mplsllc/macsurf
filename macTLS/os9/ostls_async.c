/*
 * ostls_async.c -- macTLS v0.2 non-blocking TLS stream API. See
 * os9/ostls_async.h for the public contract and
 * docs/mactls-v0.2-async-design.md for the design rationale.
 *
 * Architecture in one paragraph: every OT call goes through an
 * async dispatch path with a notifier installed at open time. The
 * notifier runs at interrupt time and just sets volatile flags on
 * the OSTLSConnection. OSTLS_Pump runs at app time, drains those
 * flags, advances the state machine, drives BearSSL one step at a
 * time, and shuttles bytes between BearSSL's record buffers and
 * the caller's plaintext ring buffers. No call blocks. No call
 * spins. The caller controls cadence by how often it invokes Pump
 * and with what max_steps budget.
 */

#include "ostls_async.h"
#include "ostls_b3_anchors.h"
#include "ostls_time.h"
#include "ostls_entropy.h"
#include "ostls_tls13_handshake.h"   /* TLS 1.3 state machine (live path) */
#include "ostls_tls13_record.h"      /* TLS 1.3 record encrypt/decrypt    */
#include "ostls_ticket_cache.h"      /* TLS 1.3 resumption ticket cache   */

#include "bearssl_ssl.h"
#include "bearssl_x509.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __MWERKS__
#include <Types.h>
#include <Memory.h>            /* NewPtrClear, DisposePtr */
#include <Events.h>            /* TickCount */
#include <Files.h>
#include <OpenTransport.h>
#include <OpenTptInternet.h>
extern OTClientContextPtr g_ostls_ot_context;
#else
/*
 * Non-CW8 Retro68 syntax-check stubs. Real linkage happens only on
 * CW8 against CarbonLib; these are enough to make the syntax check
 * happy on the Linux side.
 */
typedef long OTResult;
typedef long OTEventCode;
typedef void *EndpointRef;
typedef void *OTConfigurationRef;
typedef short OTByteCount;
typedef unsigned char UInt8;
typedef unsigned short InetPort;
typedef unsigned long InetHost;
#ifndef true
#define true 1
#define false 0
#endif
typedef int Boolean;
typedef struct { OTByteCount maxlen, len; UInt8 *buf; } TNetbuf;
typedef struct { TNetbuf addr; TNetbuf opt; long qlen; } TBind;
typedef struct { TNetbuf addr; TNetbuf opt; TNetbuf udata; long sequence; } TCall;
typedef struct { unsigned short fAddressType; char fName[1]; } DNSAddress;
typedef struct {
    unsigned short fAddressType;
    InetPort       fPort;
    InetHost       fHost;
    UInt8          fUnused[8];
} InetAddress;
typedef struct {
    long addr, options, tsdu, etsdu, connect, discon;
    long servtype, flags;
} TEndpointInfo;
typedef void (*OTNotifyProcPtr)(void *, OTEventCode, OTResult, void *);
/* `pascal` is already a Retro68 built-in calling-convention keyword;
 * do not redefine. */
#define noErr            0
#define kOTNoError       0
#define kOTNoDataErr  (-3162)
#define kOTFlowErr    (-3161)
#define kOTLookErr    (-3158)
#define T_OPENCOMPLETE  0x20000001L
#define T_BINDCOMPLETE  0x20000002L
#define T_CONNECT       0x00000002L
#define T_DATA          0x00000004L
#define T_EXDATA        0x00000008L
#define T_DISCONNECT    0x00000010L
#define T_ORDREL        0x00000080L
/* OT endpoint-state values (OTGetEndpointState) */
#define T_OUTCON        3
#define T_DATAXFER      5
#define T_OUTREL        6
static OTConfigurationRef OTCreateConfiguration(const char *s){(void)s;return (OTConfigurationRef)1;}
static OSStatus OTAsyncOpenEndpointInContext(OTConfigurationRef c,unsigned long f,TEndpointInfo *i,OTNotifyProcPtr p,void *x,void *ctx){(void)c;(void)f;(void)i;(void)p;(void)x;(void)ctx;return noErr;}
static OSStatus OTBind(EndpointRef e,TBind *r,TBind *o){(void)e;(void)r;(void)o;return noErr;}
static OSStatus OTConnect(EndpointRef e,TCall *c,void *r){(void)e;(void)c;(void)r;return noErr;}
static OTResult OTSnd(EndpointRef e,void *b,long n,long f){(void)e;(void)b;(void)f;return n;}
static OTResult OTRcv(EndpointRef e,void *b,long n,long *f){(void)e;(void)b;(void)n;(void)f;return 0;}
static OSStatus OTSndOrderlyDisconnect(EndpointRef e){(void)e;return noErr;}
static OSStatus OTRcvOrderlyDisconnect(EndpointRef e){(void)e;return noErr;}
static OSStatus OTRcvDisconnect(EndpointRef e,void *p){(void)e;(void)p;return noErr;}
static OSStatus OTSndDisconnect(EndpointRef e,TCall *c){(void)e;(void)c;return noErr;}
static OSStatus OTRcvConnect(EndpointRef e,TCall *c){(void)e;(void)c;return noErr;}
static OTResult OTLook(EndpointRef e){(void)e;return 0;}
static OTResult OTGetEndpointState(EndpointRef e){(void)e;return T_DATAXFER;}
static OSStatus OTCloseProvider(EndpointRef e){(void)e;return noErr;}
static long OTInitDNSAddress(DNSAddress *d,const char *s){(void)d;(void)s;return 0;}
static void OTInitInetAddress(InetAddress *a, InetPort p, InetHost h){(void)a;(void)p;(void)h;}
/* fixes252 — TCP_NODELAY option stubs for Retro68 syntax check */
typedef struct { unsigned long len; unsigned long level; unsigned long name; unsigned long status; unsigned long value[1]; } TOption;
typedef struct { TNetbuf opt; long flags; } TOptMgmt;
#define INET_TCP 6
#define TCP_NODELAY 1
#define T_NEGOTIATE 0x0004
static OSStatus OTOptionManagement(EndpointRef e, TOptMgmt *r, TOptMgmt *o){(void)e;(void)r;(void)o;return noErr;}
static void OTMemzero(void *p,unsigned long n){memset(p,0,n);}
static char *NewPtrClear(unsigned long n){return (char *)calloc(1, n);}
static void DisposePtr(char *p){free(p);}
static unsigned long TickCount(void){return 0;}
extern void *g_ostls_ot_context;
#endif


/* ----------------------------------------------------------------- */
/* Internal connection structure                                     */
/* ----------------------------------------------------------------- */

/* fixes414 — widen the decrypted-read ring so a full TLS-1.3 plaintext
 * record (up to 16 KB) drains in one deliver_plain pass instead of four.
 * Big resources (mactrove PNGs) were crawling at ~4 KB per poll tick. */
#define OSTLS_READ_BUF_SIZE   16384
#define OSTLS_WRITE_BUF_SIZE  4096
#define OSTLS_DEFAULT_TIMEOUT 1800UL  /* 30s @ 60Hz */

/*
 * Master switch for the TLS 1.3 path. On by default: every connection
 * offers 1.3 (with 1.2 suites advertised for fallback). A consumer can
 * force 1.2-only by calling OSTLS_SetTryTLS13(0) before OSTLS_Start.
 */
static int g_ostls_try_tls13 = 1;

/*
 * Internal phase markers for the connect sub-state machine. Visible
 * publicly only as kOSTLSStateConnecting; we track the substeps
 * here so Pump knows what notifier event to wait for next.
 */
enum {
    kConnectPhase_NeedsOpen     = 0,
    kConnectPhase_OpenInFlight  = 1,
    kConnectPhase_NeedsBind     = 2,
    kConnectPhase_BindInFlight  = 3,
    kConnectPhase_NeedsConnect  = 4,
    kConnectPhase_ConnectInFlight = 5,
    kConnectPhase_Done          = 6
};

/*
 * TLS version path for a connection. Every connect starts in
 * kT13Mode_Trying (a 1.3 ClientHello that also advertises 1.2 suites);
 * if the server picks 1.3 we go to kT13Mode_Open and the 1.3 record
 * layer carries the data, otherwise the handshake returns Fallback12
 * and we reconnect with kT13Mode_Off so BearSSL's T0 engine drives a
 * full 1.2 handshake (matching Certainly's version strategy).
 */
enum {
    kT13Mode_Off    = 0,   /* BearSSL TLS 1.2 engine drives everything */
    kT13Mode_Trying = 1,   /* 1.3 handshake in progress                */
    kT13Mode_Open   = 2    /* 1.3 established; our record layer is live */
};

struct OSTLSConnection {
    /* ----- Public-facing state ----- */
    OSTLSState state;
    OSErr      os_err;
    OSStatus   ot_err;
    int        br_err;
    UInt16     cipher_suite;
    int        resumed;         /* 1 if session ticket was accepted by server */

    /* ----- Caller-supplied config ----- */
    char    host[256];
    char    server_name[256];
    char    host_port[280];        /* "host:port" string for OTInitDNSAddress */
    UInt16  port;
    UInt32  connect_timeout_ticks;
    UInt32  handshake_timeout_ticks;
    void   *user_refcon;

    /* ----- Lifecycle bookkeeping ----- */
    UInt32  start_ticks;
    UInt32  handshake_start_ticks;
    int     connect_phase;          /* one of kConnectPhase_* */
    Boolean started;
    Boolean disposed;
    Boolean close_requested;        /* caller called OSTLS_Close */
    Boolean peer_closed;

    /* ----- OT machinery (stable buffers; outlive OT calls) ----- */
    EndpointRef ep;
    Boolean     ep_open;
    TEndpointInfo ep_info;
    DNSAddress  dns_addr;
    TBind       bind_req;
    TBind       bind_ret;
    InetAddress bound_addr;
    TCall       connect_call;
    Boolean     ord_consumed;       /* OTRcvOrderlyDisconnect called once */
    Boolean     disc_consumed;      /* OTRcvDisconnect called once */

    /* ----- Notifier-set flags (volatile; interrupt-time writes) ----- */
    volatile Boolean nf_open_complete;
    volatile Boolean nf_bind_complete;
    volatile Boolean nf_connect_complete;
    volatile Boolean nf_data_pending;
    volatile Boolean nf_ord_release;
    volatile Boolean nf_disconnect;
    volatile OTResult nf_last_result;

    /* ----- BearSSL ----- */
    br_ssl_client_context     sc;
    br_x509_minimal_context   xc;
    unsigned char             ssl_iobuf[BR_SSL_BUFSIZE_BIDI];
    Boolean                   bearssl_initialised;
    Boolean                   handshake_done;
    Boolean                   close_notify_sent;

    /* ----- TLS 1.3 (live path) -----
     * hs13 holds the 1.3 handshake state plus the read/write record
     * contexts after completion. The raw inbound record bytes are
     * buffered in ssl_iobuf (reused: BearSSL's record engine is dormant
     * while we drive 1.3), tracked by tls13_recv_len. Decrypted app-data
     * lands in hs13.plain_buf and is fed into read_buf across pumps via
     * t13_plain_off..t13_plain_len. Outbound app-data is encrypted into
     * t13_send_rec and flushed across pumps via t13_send_off..len. */
    tls13_hs_ctx *hs13;                /* lazily NewPtr'd only while 1.3 is attempted; NULL otherwise */
    tls13_session_ticket offer_ticket_storage; /* E2: cached ticket we offer this connection (hs13->offer_ticket points here) */
    int           tls13_mode;          /* one of kT13Mode_* */
    Boolean       tls13_disabled;      /* set after a Fallback12 reconnect */
    UInt32        tls13_recv_len;      /* raw inbound bytes held in ssl_iobuf */
    UInt32        t13_plain_len;       /* decrypted app-data in hs13.plain_buf */
    UInt32        t13_plain_off;       /* bytes of plain_buf already delivered */
    unsigned char t13_send_rec[OSTLS_WRITE_BUF_SIZE + 64]; /* one outbound record */
    UInt32        t13_send_len;
    UInt32        t13_send_off;
    UInt32        t13_dbg_state;       /* last-logged hs13 state (crash forensics) */

    /* ----- Plaintext ring buffer (read side: BearSSL -> caller) ----- */
    unsigned char read_buf[OSTLS_READ_BUF_SIZE];
    UInt32        read_pos;
    UInt32        read_avail;

    /* ----- Plaintext queue (write side: caller -> BearSSL) ----- */
    unsigned char write_buf[OSTLS_WRITE_BUF_SIZE];
    UInt32        write_pos;
    UInt32        write_avail;

    /* ----- Diagnostic counters (exposed via OSTLS_GetDiagnostics) ----- */
    UInt32 dbg_ot_send_calls;
    UInt32 dbg_ot_send_bytes;
    UInt32 dbg_ot_send_zero;    /* OTSnd returned 0 (try again)        */
    UInt32 dbg_ot_send_flow;    /* OTSnd returned kOTFlowErr           */
    UInt32 dbg_ot_recv_calls;
    UInt32 dbg_ot_recv_bytes;
    UInt32 dbg_ot_recv_nodata;  /* OTRcv returned kOTNoDataErr         */
    UInt32 dbg_pump_calls;
    UInt32 br_state_last;       /* last br_ssl_engine_current_state    */
};


/* ----------------------------------------------------------------- */
/* Notifier                                                          */
/* ----------------------------------------------------------------- */

/*
 * Runs at INTERRUPT TIME. Strict constraints:
 *   - No memory allocation (no NewPtr / NewHandle / etc.)
 *   - No Toolbox calls (no logging, no UI)
 *   - Only OT calls documented as deferred-task-safe (OTRcvConnect)
 *
 * Touches only the volatile nf_* fields on the connection.
 */
static pascal void
ostls_notifier(void *context, OTEventCode event,
               OTResult result, void *cookie)
{
    OSTLSConnection *conn;

    conn = (OSTLSConnection *)context;
    if (conn == NULL) return;

    conn->nf_last_result = result;

    switch (event) {
    case T_OPENCOMPLETE:
        /* cookie is the new EndpointRef. Stash it for Pump to pick up. */
        conn->ep = (EndpointRef)cookie;
        conn->nf_open_complete = true;
        break;

    case T_BINDCOMPLETE:
        conn->nf_bind_complete = true;
        break;

    case T_CONNECT:
        /*
         * Do NOT call OTRcvConnect here at interrupt time. Stash the
         * flag and let Pump handle it at app-time where it's safe
         * to handle errors and state transitions.
         */
        conn->nf_connect_complete = true;
        break;

    case T_DATA:
    case T_EXDATA:
        conn->nf_data_pending = true;
        break;

    case T_ORDREL:
        conn->nf_ord_release = true;
        break;

    case T_DISCONNECT:
        conn->nf_disconnect = true;
        break;

    default:
        /* Unhandled events are intentionally ignored; the only ones
         * we need for a TCP client+TLS path are above. */
        break;
    }
}


/* ----------------------------------------------------------------- */
/* Lifecycle: New / Dispose                                          */
/* ----------------------------------------------------------------- */

OSErr
OSTLS_New(OSTLSConnection **out_conn, const OSTLSConfig *config)
{
    OSTLSConnection *conn;
    size_t host_len;
    size_t sni_len;

    if (out_conn == NULL) {
        return (OSErr)kOSTLSAsync_BadArgs;
    }
    *out_conn = NULL;

    if (config == NULL || config->host == NULL ||
        config->server_name == NULL || config->port == 0) {
        return (OSErr)kOSTLSAsync_BadArgs;
    }

    host_len = strlen(config->host);
    sni_len  = strlen(config->server_name);
    if (host_len == 0 || host_len >= sizeof conn->host ||
        sni_len  == 0 || sni_len  >= sizeof conn->server_name) {
        return (OSErr)kOSTLSAsync_BadArgs;
    }

    conn = (OSTLSConnection *)NewPtrClear((unsigned long)sizeof *conn);
    if (conn == NULL) {
        return (OSErr)kOSTLSAsync_NoMemory;
    }

    /* NewPtrClear zeroes everything, but be explicit about the
     * fields that matter. */
    conn->state = kOSTLSStateIdle;
    conn->os_err = noErr;
    conn->ot_err = 0x7FFFFFFF;  /* Sentinel: not set yet */
    conn->br_err = 0;
    conn->cipher_suite = 0;

    memcpy(conn->host, config->host, host_len);
    conn->host[host_len] = '\0';
    memcpy(conn->server_name, config->server_name, sni_len);
    conn->server_name[sni_len] = '\0';
    conn->port = config->port;
    sprintf(conn->host_port, "%.250s:%u",
            conn->host, (unsigned)conn->port);
    conn->host_port[sizeof conn->host_port - 1] = '\0';

    conn->connect_timeout_ticks =
        (config->connect_timeout_ticks > 0)
        ? config->connect_timeout_ticks : OSTLS_DEFAULT_TIMEOUT;
    conn->handshake_timeout_ticks =
        (config->handshake_timeout_ticks > 0)
        ? config->handshake_timeout_ticks : OSTLS_DEFAULT_TIMEOUT;

    conn->user_refcon = config->user_refcon;

    conn->connect_phase = kConnectPhase_NeedsOpen;
    conn->started = false;
    conn->disposed = false;
    conn->close_requested = false;
    conn->ord_consumed = false;
    conn->disc_consumed = false;

    *out_conn = conn;
    return (OSErr)kOSTLSAsync_OK;
}


void
OSTLS_Dispose(OSTLSConnection *conn)
{
    if (conn == NULL) return;
    if (conn->disposed) return;

    /* If we still have an OT endpoint, tear it down. We don't care
     * whether the caller called Close first -- Dispose is the
     * final teardown regardless of state. */
    if (conn->ep_open && conn->ep != NULL) {
        /* If we're mid-connection, an orderly disconnect attempt is
         * harmless. If we're already torn down, OT will return an
         * error which we ignore here. */
        OTSndOrderlyDisconnect(conn->ep);
        OTCloseProvider(conn->ep);
        conn->ep = NULL;
        conn->ep_open = false;
    }

    if (conn->hs13 != NULL) {
        DisposePtr((char *)conn->hs13);
        conn->hs13 = NULL;
    }

    conn->disposed = true;
    DisposePtr((char *)conn);
}


/* ----------------------------------------------------------------- */
/* Lifecycle: Start                                                  */
/* ----------------------------------------------------------------- */

OSErr
OSTLS_Start(OSTLSConnection *conn)
{
    OTConfigurationRef cfg;
    OSStatus oterr;
    OSErr time_err;
    UInt32 br_days;
    UInt32 br_seconds;

    if (conn == NULL || conn->disposed) {
        return (OSErr)kOSTLSAsync_BadArgs;
    }
    if (conn->started) {
        return (OSErr)kOSTLSAsync_WrongState;
    }
    if (conn->state != kOSTLSStateIdle) {
        return (OSErr)kOSTLSAsync_WrongState;
    }

    /* Validate the system clock before we kick off any TLS work --
     * a pre-2000 clock will make every cert fail validation later
     * and the user will see a useless "handshake failed" instead
     * of an actionable "set your clock" message. */
    time_err = OSTLS_GetBearSSLTime(&br_days, &br_seconds);
    if (time_err == kOSTLSTimeClockBefore2000) {
        conn->state = kOSTLSStateFailed;
        conn->os_err = (OSErr)kOSTLSAsync_ClockBefore2000;
        return (OSErr)kOSTLSAsync_ClockBefore2000;
    }
    if (time_err != kOSTLSTimeOK) {
        conn->state = kOSTLSStateFailed;
        conn->os_err = (OSErr)kOSTLSAsync_ClockBefore2000;
        return (OSErr)kOSTLSAsync_ClockBefore2000;
    }

    cfg = OTCreateConfiguration("tcp");
    if (cfg == NULL || cfg == (OTConfigurationRef)-1L) {
        conn->state = kOSTLSStateFailed;
        conn->os_err = (OSErr)kOSTLSAsync_OTConfigFail;
        return (OSErr)kOSTLSAsync_OTConfigFail;
    }

    /*
     * Async open: returns immediately. T_OPENCOMPLETE will fire on
     * the notifier when the endpoint is ready; that handler stashes
     * the EndpointRef into conn->ep.
     */
    OTMemzero(&conn->ep_info, sizeof conn->ep_info);
    oterr = OTAsyncOpenEndpointInContext(cfg, 0, &conn->ep_info,
                                         (OTNotifyProcPtr)ostls_notifier,
                                         conn,
                                         g_ostls_ot_context);
    if (oterr != noErr) {
        conn->state = kOSTLSStateFailed;
        conn->os_err = (OSErr)kOSTLSAsync_OTOpenFail;
        conn->ot_err = oterr;
        return (OSErr)kOSTLSAsync_OTOpenFail;
    }

    conn->state = kOSTLSStateConnecting;
    conn->connect_phase = kConnectPhase_OpenInFlight;
    conn->start_ticks = (UInt32)TickCount();
    conn->started = true;

    return (OSErr)kOSTLSAsync_OK;
}


/* ----------------------------------------------------------------- */
/* Pump helpers                                                      */
/* ----------------------------------------------------------------- */

/*
 * Set the connection to Failed with the given codes. Idempotent.
 * Returns the kOSTLSEventFailed value so callers can chain
 *   *event = ostls_fail(conn, ...);
 */
static OSTLSEvent
ostls_fail(OSTLSConnection *conn, OSErr os_err,
           OSStatus ot_err, int br_err)
{
    /* Trap for the "ghost" 2009 error with ot=0. */
    if (os_err == 2009 && ot_err == 0) {
        OSTLS_LogLine("TRAP: ostls_fail called with os_err=2009 and ot=0");
    }

    /* Always update error codes if they haven't been set yet, even
     * if we're already in the Failed state. This helps capture the
     * root cause if a failure cascades. */
    if (conn->os_err == noErr) conn->os_err = os_err;
    if (conn->ot_err == 0x7FFFFFFF) conn->ot_err = ot_err;
    if (br_err != 0 && conn->br_err == 0) conn->br_err = br_err;

    if (conn->state == kOSTLSStateFailed ||
        conn->state == kOSTLSStateClosed) {
        return kOSTLSEventNone;
    }
    conn->state = kOSTLSStateFailed;
    return kOSTLSEventFailed;
}


/*
 * Bump `best_event` only if `candidate` has higher precedence.
 * Precedence order (high to low): Failed > Closed > HandshakeDone
 * > Connected > Readable > Writable > None.
 */
static int
event_priority(OSTLSEvent e)
{
    switch (e) {
    case kOSTLSEventFailed:        return 7;
    case kOSTLSEventClosed:        return 6;
    case kOSTLSEventHandshakeDone: return 5;
    case kOSTLSEventConnected:     return 4;
    case kOSTLSEventReadable:      return 3;
    case kOSTLSEventWritable:      return 2;
    case kOSTLSEventNone:          return 0;
    }
    return 0;
}


static void
event_bump(OSTLSEvent *current, OSTLSEvent candidate)
{
    if (event_priority(candidate) > event_priority(*current)) {
        *current = candidate;
    }
}


/*
 * Initialise BearSSL on this connection after the TCP connect has
 * completed. Once-per-connection setup.
 */
/* ---------- TLS session cache (fixes254) ----------
 *
 * Session resumption (RFC 5246 §F.1.4). After a successful handshake,
 * BearSSL exposes a br_ssl_session_parameters carrying the session ID,
 * master secret, cipher suite, and version. If we present that to the
 * server on a subsequent ClientHello and the server still has the
 * session in its cache, the handshake is abbreviated: server skips the
 * Certificate, ServerKeyExchange, and ServerHelloDone messages and
 * jumps straight to ChangeCipherSpec + Finished. Saves ~700-1200 ms
 * of CPU + network round-trips per resumption on a 233 MHz G3.
 *
 * Cache scope: session lifetime, in-RAM only. Disk persistence across
 * launches would be a follow-on (need to serialise the params struct).
 * Keyed by "host:port" so subdomains and explicit ports don't collide.
 * FIFO eviction at 16 entries. Per-entry size is ~340 bytes; total
 * cache budget is ~5.5 KB.
 *
 * Server support: most modern HTTPS servers (nginx, Cloudflare, Apache
 * with mod_ssl session cache) support session ID resumption out of
 * the box, typically with a 10-300 min cache lifetime. If a server
 * doesn't recognize the session ID, it falls through to a full
 * handshake — no harm done. */
#define OSTLS_SESS_MAX 16
#define OSTLS_SESS_KEY_LEN 280

typedef struct {
    char    key[OSTLS_SESS_KEY_LEN];
    br_ssl_session_parameters params;
} ostls_session_entry;

static ostls_session_entry s_sess_cache[OSTLS_SESS_MAX];
static int                 s_sess_cache_count = 0;

static int
sess_cache_find(const char *key)
{
    int i;
    if (key == NULL || key[0] == '\0') return -1;
    for (i = 0; i < s_sess_cache_count; i++) {
        if (strcmp(s_sess_cache[i].key, key) == 0) return i;
    }
    return -1;
}

static int
sess_cache_get(const char *key, br_ssl_session_parameters *out)
{
    int idx;
    if (out == NULL) return 0;
    idx = sess_cache_find(key);
    if (idx < 0) return 0;
    *out = s_sess_cache[idx].params;
    return 1;
}

static void
sess_cache_put(const char *key, const br_ssl_session_parameters *p)
{
    int idx;
    int i;
    if (key == NULL || key[0] == '\0' || p == NULL) return;
    idx = sess_cache_find(key);
    if (idx >= 0) {
        s_sess_cache[idx].params = *p;
        return;
    }
    if (s_sess_cache_count >= OSTLS_SESS_MAX) {
        /* FIFO evict the oldest entry. */
        for (i = 0; i < OSTLS_SESS_MAX - 1; i++) {
            s_sess_cache[i] = s_sess_cache[i + 1];
        }
        s_sess_cache_count--;
    }
    strncpy(s_sess_cache[s_sess_cache_count].key, key,
            sizeof s_sess_cache[0].key - 1);
    s_sess_cache[s_sess_cache_count].key[
        sizeof s_sess_cache[0].key - 1] = '\0';
    s_sess_cache[s_sess_cache_count].params = *p;
    s_sess_cache_count++;
}

/* Compose the "host:port" cache key into out (cap bytes). */
static void
sess_cache_make_key(const OSTLSConnection *conn, char *out, int cap)
{
    int n;
    if (out == NULL || cap <= 0) return;
    n = sprintf(out, "%s:%d", conn->host, (int)conn->port);
    if (n < 0 || n >= cap) out[0] = '\0';
}

static OSTLSEvent
ostls_setup_bearssl(OSTLSConnection *conn)
{
    const br_x509_trust_anchor *anchors;
    size_t anchors_count;
    UInt32 br_days;
    UInt32 br_seconds;
    OSErr time_err;
    int entropy_err;

    /* Re-read the clock; the user could have ticked over midnight
     * between Start and the first handshake step (unlikely on a
     * fresh connect, but defensive). */
    time_err = OSTLS_GetBearSSLTime(&br_days, &br_seconds);
    if (time_err != kOSTLSTimeOK) {
        return ostls_fail(conn, (OSErr)kOSTLSAsync_ClockBefore2000,
                          noErr, 0);
    }

    OSTLS_B3_GetAnchors(&anchors, &anchors_count);
    br_ssl_client_init_full(&conn->sc, &conn->xc,
                            anchors, anchors_count);
    br_x509_minimal_set_time(&conn->xc, br_days, br_seconds);

    entropy_err = OSTLS_InjectEntropy(&conn->sc.eng);
    if (entropy_err != 0) {
        return ostls_fail(conn, (OSErr)kOSTLSAsync_EntropyFail,
                          noErr, 0);
    }

    br_ssl_engine_set_buffer(&conn->sc.eng,
                             conn->ssl_iobuf,
                             sizeof conn->ssl_iobuf, 1);

    conn->bearssl_initialised = true;
    conn->handshake_start_ticks = (UInt32)TickCount();

    conn->tls13_recv_len = 0;
    conn->t13_plain_len = 0;
    conn->t13_plain_off = 0;
    conn->t13_send_len = 0;
    conn->t13_send_off = 0;

    /*
     * Arm the TLS 1.3 path unless this is a post-fallback retry. The 1.3
     * handshake uses the engine ONLY as a seeded PRNG (keygen) plus the
     * X.509 validator configured above -- it must NOT br_ssl_client_reset
     * the engine. Resetting starts BearSSL's own T0 (1.2) handshake, which
     * generates a ClientHello into ssl_iobuf and competes for the record
     * buffers the 1.3 path is about to use; Certainly documents the same
     * "do not reset before 1.3" rule. br_ssl_client_reset + session
     * resumption happen below ONLY on the 1.2 path (1.3 disabled, a
     * context-allocation failure, or a Fallback12 reconnect).
     */
    if (g_ostls_try_tls13 && !conn->tls13_disabled) {
        if (conn->hs13 == NULL) {
            conn->hs13 = (tls13_hs_ctx *)
                NewPtrClear((unsigned long)sizeof(tls13_hs_ctx));
        }
        if (conn->hs13 != NULL) {
            tls13_handshake_init(conn->hs13);
            conn->hs13->eng = &conn->sc.eng;
            conn->hs13->x509_ctx = (const br_x509_class **)&conn->xc.vtable;
            /* macTLS#2 E2: if we hold a live ticket for this host, offer it
             * so the handshake resumes (skips the cert verify + most of the
             * ECDHE cost). TickCount() is 60/sec on OS 9. On a miss the
             * normal full handshake runs. */
            {
                /* uint32_t (NOT UInt32): the cache API takes uint32_t*, and
                 * CW8 treats unsigned long* (UInt32*) and unsigned int*
                 * (uint32_t*) as incompatible pointer types. Match exactly. */
                uint32_t age_ms = 0;
                if (OSTLS_TicketCacheGet(conn->host, (uint32_t)TickCount(), 60,
                                         &conn->offer_ticket_storage,
                                         &age_ms)) {
                    conn->hs13->resuming = 1;
                    conn->hs13->offer_ticket = &conn->offer_ticket_storage;
                    conn->hs13->offer_obfuscated_age =
                        age_ms + conn->offer_ticket_storage.age_add;
                    OSTLS_LogLinef("T13 async: resuming %s (cached ticket)",
                                   conn->host);
                }
            }
            conn->tls13_mode = kT13Mode_Trying;
            conn->t13_dbg_state = 0xFFFFFFFFUL;
            OSTLS_LogLinef("T13 async: armed (server=%s)", conn->server_name);
        } else {
            OSTLS_LogLine("T13 async: 1.3 context alloc FAILED -- using TLS 1.2 (low memory)");
            conn->tls13_mode = kT13Mode_Off;
        }
    } else {
        conn->tls13_mode = kT13Mode_Off;
    }

    if (conn->tls13_mode == kT13Mode_Off) {
        /* TLS 1.2 path: offer a cached session for an abbreviated
         * handshake (resume flag must be non-zero or BearSSL ignores the
         * injected session -- the original fixes254 bug), then reset the
         * engine so its T0 program drives the whole handshake. */
        int resume_session = 0;
        char key[OSTLS_SESS_KEY_LEN];
        br_ssl_session_parameters cached;
        int reset_ok;

        sess_cache_make_key(conn, key, sizeof key);
        if (sess_cache_get(key, &cached)) {
            br_ssl_engine_set_session_parameters(&conn->sc.eng, &cached);
            resume_session = 1;
            OSTLS_LogLinef("macTLS resume: offering cached session %s", key);
        }

        reset_ok = br_ssl_client_reset(&conn->sc, conn->server_name,
                                       resume_session);
        if (reset_ok == 0) {
            int br_err = br_ssl_engine_last_error(&conn->sc.eng);
            return ostls_fail(conn, (OSErr)kOSTLSAsync_ClientResetFail,
                              noErr, br_err);
        }
    }

    return kOSTLSEventNone;
}


/*
 * Advance the connect phase by one step. Returns the event observed
 * (if any) and increments *steps_used by 1 if it did something.
 */
static OSTLSEvent
pump_connect_step(OSTLSConnection *conn, UInt32 *steps_used)
{
    OSStatus oterr;
    long dns_len;
    UInt32 now;

    now = (UInt32)TickCount();
    if (now - conn->start_ticks > conn->connect_timeout_ticks) {
        return ostls_fail(conn, (OSErr)kOSTLSAsync_ConnectTimeout,
                          noErr, 0);
    }

    if (conn->nf_disconnect) {
        conn->nf_disconnect = false;
        return ostls_fail(conn, (OSErr)kOSTLSAsync_OTConnectFail,
                          conn->nf_last_result, 0);
    }

    switch (conn->connect_phase) {
    case kConnectPhase_OpenInFlight:
        if (!conn->nf_open_complete) return kOSTLSEventNone;
        conn->nf_open_complete = false;
        if (conn->nf_last_result != noErr) {
            return ostls_fail(conn, (OSErr)kOSTLSAsync_OTOpenFail,
                              conn->nf_last_result, 0);
        }
        if (conn->ep == NULL) {
            /* Shouldn't happen if T_OPENCOMPLETE arrived with
             * noErr, but be defensive. */
            return ostls_fail(conn, (OSErr)kOSTLSAsync_OTOpenFail,
                              -1, 0);
        }
        conn->ep_open = true;
        conn->connect_phase = kConnectPhase_NeedsBind;
        (*steps_used)++;
        return kOSTLSEventNone;

    case kConnectPhase_NeedsBind:
        /*
         * Outbound client bind: addr=NULL, qlen=0. Verified safe in
         * v0.1 (B1-B4 all use this exact form) and explicitly NOT
         * the broken Carbon CFM caller-chosen-address case.
         */
        OTMemzero(&conn->bind_req, sizeof conn->bind_req);
        OTMemzero(&conn->bind_ret, sizeof conn->bind_ret);
        oterr = OTBind(conn->ep, NULL, NULL);
        if (oterr != noErr && oterr != kOTNoError) {
            return ostls_fail(conn, (OSErr)kOSTLSAsync_OTBindFail,
                              oterr, 0);
        }
        conn->connect_phase = kConnectPhase_BindInFlight;
        (*steps_used)++;
        return kOSTLSEventNone;

    case kConnectPhase_BindInFlight:
        if (!conn->nf_bind_complete) return kOSTLSEventNone;
        conn->nf_bind_complete = false;
        if (conn->nf_last_result != noErr) {
            return ostls_fail(conn, (OSErr)kOSTLSAsync_OTBindFail,
                              conn->nf_last_result, 0);
        }
        conn->connect_phase = kConnectPhase_NeedsConnect;
        (*steps_used)++;
        return kOSTLSEventNone;

    case kConnectPhase_NeedsConnect:
        /* Build the connect address from host:port via DNSAddress. */
        OTMemzero(&conn->dns_addr, sizeof conn->dns_addr);
        OTMemzero(&conn->connect_call, sizeof conn->connect_call);
        dns_len = OTInitDNSAddress(&conn->dns_addr,
                                   (char *)conn->host_port);
        if (dns_len <= 0) {
            return ostls_fail(conn, (OSErr)kOSTLSAsync_OTConnectFail,
                              dns_len, 0);
        }
        conn->connect_call.addr.buf = (UInt8 *)&conn->dns_addr;
        conn->connect_call.addr.len = (short)dns_len;
        oterr = OTConnect(conn->ep, &conn->connect_call, NULL);
        if (oterr != noErr && oterr != kOTNoError &&
            oterr != kOTNoDataErr) {
            /* kOTNoDataErr after an async OTConnect means "in
             * flight" -- that's expected. Any other error is
             * fatal. */
            return ostls_fail(conn, (OSErr)kOSTLSAsync_OTConnectFail,
                              oterr, 0);
        }
        conn->connect_phase = kConnectPhase_ConnectInFlight;
        (*steps_used)++;
        return kOSTLSEventNone;

    case kConnectPhase_ConnectInFlight:
        if (!conn->nf_connect_complete) return kOSTLSEventNone;
        conn->nf_connect_complete = false;
        if (conn->nf_last_result != noErr) {
            return ostls_fail(conn, (OSErr)kOSTLSAsync_OTConnectFail,
                              conn->nf_last_result, 0);
        }

        /* Consume the connect event at app-time. */
        oterr = OTRcvConnect(conn->ep, NULL);
        if (oterr != noErr) {
            return ostls_fail(conn, (OSErr)kOSTLSAsync_OTConnectFail,
                              oterr, 0);
        }

        conn->connect_phase = kConnectPhase_Done;

        /* fixes252 — disable Nagle's algorithm via TCP_NODELAY. The
         * interaction with TCP delayed-ACK can cost ~200 ms on a cold
         * connection: handshake last message (client Finished) is
         * small, server delays the ACK, Nagle holds back our HTTP
         * request waiting for the unACKed handshake bytes. NODELAY
         * means every TLS record we hand to OTSnd goes on the wire
         * immediately. Pool-reused connections didn't suffer (no
         * recent unACKed data), so the win is per cold connection.
         * Best-effort: failure is non-fatal (Nagle stays on). */
        {
            TOption  opt;
            TOptMgmt req;
            TOptMgmt ret;

            opt.len      = sizeof opt;
            opt.level    = INET_TCP;
            opt.name     = TCP_NODELAY;
            opt.status   = 0;
            opt.value[0] = 1;

            req.opt.buf    = (UInt8 *)&opt;
            req.opt.len    = sizeof opt;
            req.opt.maxlen = sizeof opt;
            req.flags      = T_NEGOTIATE;

            ret.opt.buf    = (UInt8 *)&opt;
            ret.opt.len    = 0;
            ret.opt.maxlen = sizeof opt;
            ret.flags      = 0;

            (void)OTOptionManagement(conn->ep, &req, &ret);
        }

        /* Transition to handshaking. */
        conn->state = kOSTLSStateHandshaking;
        {
            OSTLSEvent setup_ev = ostls_setup_bearssl(conn);
            if (setup_ev == kOSTLSEventFailed) return kOSTLSEventFailed;
        }
        (*steps_used)++;
        return kOSTLSEventConnected;

    default:
        return kOSTLSEventNone;
    }
}


/*
 * Read up to one OTRcv worth of bytes from the wire and hand them
 * to BearSSL's recvrec buffer. Returns 1 if work was done, 0 if no
 * data was available, -1 on terminal failure (connection moved to
 * Failed/Closed).
 */
static int
pump_ot_recv_into_bearssl(OSTLSConnection *conn)
{
    unsigned char *rbuf;
    size_t rlen;
    OTResult got;
    OTResult state;

    rbuf = br_ssl_engine_recvrec_buf(&conn->sc.eng, &rlen);
    if (rlen == 0) return 0;

    state = OTGetEndpointState(conn->ep);
    if (state != T_DATAXFER && state != T_OUTREL) {
        /* Not in a state where we can receive data. */
        br_ssl_engine_recvrec_ack(&conn->sc.eng, 0);
        conn->nf_data_pending = false;
        if (conn->state == kOSTLSStateOpen || conn->state == kOSTLSStateClosing) {
            br_ssl_engine_close(&conn->sc.eng);
        }
        return 1;
    }

    got = OTRcv(conn->ep, rbuf, (long)rlen, NULL);
    conn->dbg_ot_recv_calls++;

    if (got > 0) {
        br_ssl_engine_recvrec_ack(&conn->sc.eng, (size_t)got);
        conn->nf_data_pending = false;  /* drained for now */
        conn->dbg_ot_recv_bytes += (UInt32)got;
        /* macEntropy Stage B: fold packet-arrival timing into the pool.
         * The moment bytes land off the wire is genuinely jittery. */
        OSTLS_StirTimer((unsigned long)got);
        return 1;
    } else if (got == kOTNoDataErr) {
        if (conn->nf_ord_release && !conn->ord_consumed) {
            /* Drained all data; now we can safely consume the release. */
            OTRcvOrderlyDisconnect(conn->ep);
            conn->ord_consumed = true;
            conn->peer_closed = true;
            return 1;
        }
        conn->nf_data_pending = false;
        conn->dbg_ot_recv_nodata++;
        return 0;
    } else if (got == 0) {
        /* Just in case some OT implementations return 0 for EOF. */
        if (!conn->ord_consumed) {
            OTRcvOrderlyDisconnect(conn->ep);
            conn->ord_consumed = true;
        }
        conn->peer_closed = true;
        conn->nf_data_pending = false;
        return 1;
    } else if (got == kOTLookErr) {
        /* An OT event is pending. Look it up. */
        OTResult look = OTLook(conn->ep);
        if (look == T_ORDREL) {
            if (!conn->ord_consumed) {
                OTRcvOrderlyDisconnect(conn->ep);
                conn->ord_consumed = true;
            }
            conn->peer_closed = true;
            conn->nf_data_pending = false;
            return 1;
        } else if (look == T_DISCONNECT) {
            if (!conn->disc_consumed) {
                OTRcvDisconnect(conn->ep, NULL);
                conn->disc_consumed = true;
            }
            ostls_fail(conn, (OSErr)kOSTLSAsync_PeerClosed, look, 0);
            return -1;
        }
        return 0;
    } else {
        /* Fatal error. */
        OSTLS_LogLinef("FAIL: pump_ot_recv got=%ld", (long)got);
        ostls_fail(conn, (OSErr)kOSTLSAsync_OTRcvFail, got, 0);
        return -1;
    }
}


/*
 * Push whatever BearSSL has queued to send onto the wire. Returns
 * 1 if work was done, 0 if nothing to send, -1 on terminal failure.
 */
static int
pump_ot_send_from_bearssl(OSTLSConnection *conn)
{
    unsigned char *sbuf;
    size_t slen;
    OTResult sent;
    OTResult state;

    sbuf = br_ssl_engine_sendrec_buf(&conn->sc.eng, &slen);
    if (slen == 0) return 0;

    state = OTGetEndpointState(conn->ep);
    if (state != T_DATAXFER && state != T_OUTCON) {
        /* Not in a state to send (e.g., T_IDLE after disconnect).
         * BearSSL wants to send a close_notify but the socket is gone.
         * Just ack the bytes to drain the buffer and let the state
         * machine progress to CLOSED. */
        br_ssl_engine_sendrec_ack(&conn->sc.eng, slen);
        return 1;
    }

    sent = OTSnd(conn->ep, sbuf, (long)slen, 0);
    conn->dbg_ot_send_calls++;
    if (sent >= 0) {
        if ((size_t)sent > 0) {
            br_ssl_engine_sendrec_ack(&conn->sc.eng, (size_t)sent);
            conn->dbg_ot_send_bytes += (UInt32)sent;
            return 1;
        }
        conn->dbg_ot_send_zero++;
        return 0;
    }
    if (sent == kOTFlowErr) {
        /* OT send buffer full; try again next tick. */
        conn->dbg_ot_send_flow++;
        return 0;
    }
    if (sent == kOTLookErr) {
        OTResult look = OTLook(conn->ep);
        if (look == T_ORDREL) {
            if (!conn->ord_consumed) {
                OTRcvOrderlyDisconnect(conn->ep);
                conn->ord_consumed = true;
            }
            conn->peer_closed = true;
            /* Acknowledge BearSSL's send since we can't push it. */
            br_ssl_engine_sendrec_ack(&conn->sc.eng, slen);
            return 1;
        } else if (look == T_DISCONNECT) {
            if (!conn->disc_consumed) {
                OTRcvDisconnect(conn->ep, NULL);
                conn->disc_consumed = true;
            }
            ostls_fail(conn, (OSErr)kOSTLSAsync_PeerClosed, look, 0);
            return -1;
        }
        return 0;
    }
    ostls_fail(conn, (OSErr)kOSTLSAsync_OTSndFail, sent, 0);
    return -1;
}


/*
 * Copy decrypted bytes from BearSSL's recvapp buffer into the
 * connection's internal read ring. Back-pressure: if the ring
 * is full, we don't ack BearSSL, which causes its recvapp_buf to
 * stay non-empty, which eventually halts further record decoding,
 * which closes the TCP receive window on the peer. Caller must
 * Read promptly.
 */
static int
pump_bearssl_recvapp_to_ring(OSTLSConnection *conn,
                             OSTLSEvent *best_event)
{
    unsigned char *abuf;
    size_t alen;
    UInt32 room;
    UInt32 free_at_end;
    size_t to_copy;

    abuf = br_ssl_engine_recvapp_buf(&conn->sc.eng, &alen);
    if (alen == 0) return 0;

    if (conn->read_avail >= sizeof conn->read_buf) {
        /* Ring is full; back-pressure to BearSSL. */
        return 0;
    }
    room = (UInt32)sizeof conn->read_buf - conn->read_avail;

    /*
     * The read buffer is a logical ring but we keep it linear:
     * read_pos is the next-byte-to-give-caller index, read_avail
     * is the count of bytes after that. To accept new bytes we
     * write at (read_pos + read_avail) mod sizeof. For a 4 KB
     * buffer with typical chunk sizes the wrap doesn't bite often;
     * we handle it explicitly below.
     */
    {
        UInt32 write_idx = (conn->read_pos + conn->read_avail)
            % (UInt32)sizeof conn->read_buf;
        free_at_end = (UInt32)sizeof conn->read_buf - write_idx;
        if (free_at_end > room) free_at_end = room;
        to_copy = alen;
        if ((UInt32)to_copy > free_at_end) {
            to_copy = (size_t)free_at_end;
        }
        memcpy(conn->read_buf + write_idx, abuf, to_copy);
        conn->read_avail += (UInt32)to_copy;
    }
    br_ssl_engine_recvapp_ack(&conn->sc.eng, to_copy);

    event_bump(best_event, kOSTLSEventReadable);
    return 1;
}


/*
 * Move bytes from the caller's write queue into BearSSL's sendapp
 * buffer. Returns 1 if any work happened.
 */
static int
pump_ring_to_bearssl_sendapp(OSTLSConnection *conn,
                             OSTLSEvent *best_event)
{
    unsigned char *abuf;
    size_t alen;
    size_t to_copy;

    if (conn->write_avail == 0) return 0;

    abuf = br_ssl_engine_sendapp_buf(&conn->sc.eng, &alen);
    if (alen == 0) return 0;

    to_copy = conn->write_avail;
    if (to_copy > alen) to_copy = alen;

    memcpy(abuf, conn->write_buf + conn->write_pos, to_copy);
    br_ssl_engine_sendapp_ack(&conn->sc.eng, to_copy);
    conn->write_pos += (UInt32)to_copy;
    conn->write_avail -= (UInt32)to_copy;
    if (conn->write_avail == 0) {
        conn->write_pos = 0;
        br_ssl_engine_flush(&conn->sc.eng, 0);
        event_bump(best_event, kOSTLSEventWritable);
    }
    return 1;
}


/* ----------------------------------------------------------------- */
/* TLS 1.3 path (live)                                               */
/* ----------------------------------------------------------------- */

/*
 * Send up to `len` raw bytes over OT. *out_sent receives how many
 * actually went (may be 0 under flow control). Returns 0 on success,
 * -1 on terminal failure (connection moved to Failed). Mirrors the
 * edge-case handling of pump_ot_send_from_bearssl.
 */
static int
ostls_send_raw(OSTLSConnection *conn, const unsigned char *buf,
               UInt32 len, UInt32 *out_sent)
{
    OTResult sent;
    OTResult state;

    *out_sent = 0;
    if (len == 0) return 0;

    state = OTGetEndpointState(conn->ep);
    if (state != T_DATAXFER && state != T_OUTCON) {
        return 0;  /* can't send in this state; try again next tick */
    }

    sent = OTSnd(conn->ep, (void *)buf, (long)len, 0);
    conn->dbg_ot_send_calls++;
    if (sent >= 0) {
        *out_sent = (UInt32)sent;
        conn->dbg_ot_send_bytes += (UInt32)sent;
        if (sent == 0) conn->dbg_ot_send_zero++;
        return 0;
    }
    if (sent == kOTFlowErr) { conn->dbg_ot_send_flow++; return 0; }
    if (sent == kOTLookErr) {
        OTResult look = OTLook(conn->ep);
        if (look == T_ORDREL) {
            if (!conn->ord_consumed) {
                OTRcvOrderlyDisconnect(conn->ep);
                conn->ord_consumed = true;
            }
            conn->peer_closed = true;
            return 0;
        } else if (look == T_DISCONNECT) {
            if (!conn->disc_consumed) {
                OTRcvDisconnect(conn->ep, NULL);
                conn->disc_consumed = true;
            }
            ostls_fail(conn, (OSErr)kOSTLSAsync_PeerClosed, look, 0);
            return -1;
        }
        return 0;
    }
    ostls_fail(conn, (OSErr)kOSTLSAsync_OTSndFail, sent, 0);
    return -1;
}

/*
 * Read one OTRcv worth of bytes into ssl_iobuf (the raw inbound record
 * buffer for the 1.3 path; BearSSL's record engine is dormant). Returns
 * 1 if bytes arrived / a close was observed, 0 if no data, -1 fatal.
 */
static int
pump_ot_recv_tls13(OSTLSConnection *conn)
{
    unsigned char *rbuf;
    long rcap;
    OTResult got;
    OTResult state;

    if (conn->tls13_recv_len >= (UInt32)sizeof conn->ssl_iobuf) {
        return 0;  /* full; a complete record must be consumed first */
    }
    rbuf = conn->ssl_iobuf + conn->tls13_recv_len;
    rcap = (long)((UInt32)sizeof conn->ssl_iobuf - conn->tls13_recv_len);

    state = OTGetEndpointState(conn->ep);
    if (state != T_DATAXFER && state != T_OUTREL) {
        conn->nf_data_pending = false;
        return 0;
    }

    got = OTRcv(conn->ep, rbuf, rcap, NULL);
    conn->dbg_ot_recv_calls++;
    if (got > 0) {
        conn->tls13_recv_len += (UInt32)got;
        conn->nf_data_pending = false;
        conn->dbg_ot_recv_bytes += (UInt32)got;
        OSTLS_StirTimer((unsigned long)got);
        return 1;
    } else if (got == kOTNoDataErr) {
        if (conn->nf_ord_release && !conn->ord_consumed) {
            OTRcvOrderlyDisconnect(conn->ep);
            conn->ord_consumed = true;
            conn->peer_closed = true;
            return 1;
        }
        conn->nf_data_pending = false;
        conn->dbg_ot_recv_nodata++;
        return 0;
    } else if (got == 0) {
        if (!conn->ord_consumed) {
            OTRcvOrderlyDisconnect(conn->ep);
            conn->ord_consumed = true;
        }
        conn->peer_closed = true;
        conn->nf_data_pending = false;
        return 1;
    } else if (got == kOTLookErr) {
        OTResult look = OTLook(conn->ep);
        if (look == T_ORDREL) {
            if (!conn->ord_consumed) {
                OTRcvOrderlyDisconnect(conn->ep);
                conn->ord_consumed = true;
            }
            conn->peer_closed = true;
            conn->nf_data_pending = false;
            return 1;
        } else if (look == T_DISCONNECT) {
            if (!conn->disc_consumed) {
                OTRcvDisconnect(conn->ep, NULL);
                conn->disc_consumed = true;
            }
            ostls_fail(conn, (OSErr)kOSTLSAsync_PeerClosed, look, 0);
            return -1;
        }
        return 0;
    }
    ostls_fail(conn, (OSErr)kOSTLSAsync_OTRcvFail, got, 0);
    return -1;
}

/*
 * Server declined TLS 1.3. Close the current endpoint and reconnect
 * to the same host:port with tls13_disabled set, so the reconnect's
 * setup_bearssl leaves tls13_mode Off and BearSSL's T0 engine drives
 * a full 1.2 handshake. Matches Certainly's fallback strategy.
 */
static OSTLSEvent
ostls_fallback_to_tls12(OSTLSConnection *conn)
{
    OTConfigurationRef cfg;
    OSStatus oterr;

    OSTLS_LogLine("macTLS: server declined 1.3 -- reconnecting for TLS 1.2");

    if (conn->ep_open && conn->ep != NULL) {
        OTSndOrderlyDisconnect(conn->ep);
        OTCloseProvider(conn->ep);
        conn->ep = NULL;
        conn->ep_open = false;
    }

    conn->nf_open_complete = false;
    conn->nf_bind_complete = false;
    conn->nf_connect_complete = false;
    conn->nf_data_pending = false;
    conn->nf_ord_release = false;
    conn->nf_disconnect = false;
    conn->ord_consumed = false;
    conn->disc_consumed = false;
    conn->peer_closed = false;
    conn->bearssl_initialised = false;
    conn->handshake_done = false;
    conn->close_notify_sent = false;
    conn->tls13_mode = kT13Mode_Off;
    conn->tls13_disabled = true;
    conn->tls13_recv_len = 0;
    /* 1.2 doesn't need the 1.3 context; free the ~21 KB now. */
    if (conn->hs13 != NULL) {
        DisposePtr((char *)conn->hs13);
        conn->hs13 = NULL;
    }

    cfg = OTCreateConfiguration("tcp");
    if (cfg == NULL || cfg == (OTConfigurationRef)-1L) {
        return ostls_fail(conn, (OSErr)kOSTLSAsync_OTConfigFail, noErr, 0);
    }
    OTMemzero(&conn->ep_info, sizeof conn->ep_info);
    oterr = OTAsyncOpenEndpointInContext(cfg, 0, &conn->ep_info,
                                         (OTNotifyProcPtr)ostls_notifier,
                                         conn, g_ostls_ot_context);
    if (oterr != noErr) {
        return ostls_fail(conn, (OSErr)kOSTLSAsync_OTOpenFail, oterr, 0);
    }
    conn->state = kOSTLSStateConnecting;
    conn->connect_phase = kConnectPhase_OpenInFlight;
    conn->start_ticks = (UInt32)TickCount();
    return kOSTLSEventNone;
}

/*
 * Push staged 1.3 plaintext (hs13.plain_buf[t13_plain_off..t13_plain_len))
 * into the read ring, same ring discipline as pump_bearssl_recvapp_to_ring.
 * Returns 1 if any bytes were delivered, 0 if nothing pending or ring full.
 */
static int
pump_tls13_deliver_plain(OSTLSConnection *conn, OSTLSEvent *best_event)
{
    UInt32 pending;
    UInt32 room;
    UInt32 write_idx;
    UInt32 free_at_end;
    UInt32 to_copy;

    pending = conn->t13_plain_len - conn->t13_plain_off;
    if (pending == 0) return 0;
    if (conn->read_avail >= (UInt32)sizeof conn->read_buf) return 0;

    room = (UInt32)sizeof conn->read_buf - conn->read_avail;
    write_idx = (conn->read_pos + conn->read_avail)
        % (UInt32)sizeof conn->read_buf;
    free_at_end = (UInt32)sizeof conn->read_buf - write_idx;
    if (free_at_end > room) free_at_end = room;
    to_copy = pending;
    if (to_copy > free_at_end) to_copy = free_at_end;

    memcpy(conn->read_buf + write_idx,
           conn->hs13->plain_buf + conn->t13_plain_off, to_copy);
    conn->read_avail += to_copy;
    conn->t13_plain_off += to_copy;
    event_bump(best_event, kOSTLSEventReadable);
    return 1;
}

/*
 * If a complete TLS record sits at the front of ssl_iobuf, decrypt it
 * with the 1.3 read context. App-data is staged in plain_buf for
 * delivery; post-handshake handshake records (NewSessionTicket /
 * KeyUpdate) are discarded; alerts close or fail. Returns 1 if a record
 * was consumed (or a terminal event set), 0 if not enough bytes yet.
 */
static int
pump_tls13_consume_record(OSTLSConnection *conn, OSTLSEvent *best_event)
{
    UInt32 reclen;
    UInt32 total;
    int dr;
    uint8_t inner;
    size_t plen;

    if (conn->tls13_recv_len < 5) return 0;
    reclen = ((UInt32)conn->ssl_iobuf[3] << 8) | (UInt32)conn->ssl_iobuf[4];
    total = 5 + reclen;
    if (conn->tls13_recv_len < total) return 0;  /* await the rest */

    plen = 0;
    inner = 0;
    dr = tls13_record_decrypt(&conn->hs13->read_ctx,
                              conn->ssl_iobuf + 5, (size_t)reclen,
                              conn->hs13->plain_buf, &plen, &inner);

    /* Slide the consumed record off the front of the buffer. */
    memmove(conn->ssl_iobuf, conn->ssl_iobuf + total,
            conn->tls13_recv_len - total);
    conn->tls13_recv_len -= total;

    if (dr != 0) {
        ostls_fail(conn, (OSErr)kOSTLSAsync_BearSSLError, noErr, BR_ERR_BAD_MAC);
        *best_event = kOSTLSEventFailed;
        return 1;
    }
    if (inner == TLS13_CT_APPLICATION_DATA) {
        conn->t13_plain_len = (UInt32)plen;
        conn->t13_plain_off = 0;
        (void)pump_tls13_deliver_plain(conn, best_event);
        return 1;
    }
    if (inner == TLS13_CT_ALERT) {
        int desc = (plen >= 2) ? (int)conn->hs13->plain_buf[1] : -1;
        if (desc == 0) {            /* close_notify */
            conn->peer_closed = true;
            if (conn->state == kOSTLSStateOpen ||
                conn->state == kOSTLSStateClosing) {
                conn->state = kOSTLSStateClosed;
                event_bump(best_event, kOSTLSEventClosed);
            }
        } else {
            ostls_fail(conn, (OSErr)kOSTLSAsync_BearSSLError, noErr, desc);
            *best_event = kOSTLSEventFailed;
        }
        return 1;
    }
    /* Post-handshake handshake record (NewSessionTicket / KeyUpdate).
     * Parse it; if it produced a resumption ticket, cache it for this host
     * so the next connection can resume (macTLS#2 E2). The return value is
     * intentionally ignored — an unsupported KeyUpdate stays a no-op, as
     * before. */
    if (inner == TLS13_CT_HANDSHAKE) {
        (void)tls13_handle_post_handshake(conn->hs13,
                                          conn->hs13->plain_buf, plen);
        if (conn->hs13->ticket_valid) {
            OSTLS_TicketCachePut(conn->host, &conn->hs13->ticket,
                                 (uint32_t)TickCount());
            conn->hs13->ticket_valid = 0;   /* consumed */
            OSTLS_LogLinef("T13 async: cached resumption ticket for %s",
                           conn->host);
        }
        return 1;
    }
    /* anything else: ignore. */
    return 1;
}

/*
 * Encrypt one outbound app-data record from the write queue into
 * t13_send_rec, if the queue is non-empty and no record is in flight.
 * Returns 1 if a record was built.
 */
static int
pump_tls13_build_send(OSTLSConnection *conn)
{
    size_t clen;
    UInt32 chunk;

    if (conn->t13_send_off < conn->t13_send_len) return 0;  /* in flight */
    if (conn->write_avail == 0) return 0;

    chunk = conn->write_avail;
    if (chunk > (UInt32)OSTLS_WRITE_BUF_SIZE) chunk = (UInt32)OSTLS_WRITE_BUF_SIZE;

    clen = 0;
    if (tls13_record_encrypt(&conn->hs13->write_ctx,
                             conn->write_buf + conn->write_pos, (size_t)chunk,
                             TLS13_CT_APPLICATION_DATA,
                             conn->t13_send_rec + 5, &clen) != 0) {
        return 0;
    }
    conn->t13_send_rec[0] = 0x17;
    conn->t13_send_rec[1] = 0x03;
    conn->t13_send_rec[2] = 0x03;
    conn->t13_send_rec[3] = (unsigned char)(clen >> 8);
    conn->t13_send_rec[4] = (unsigned char)clen;
    conn->t13_send_len = (UInt32)(5 + clen);
    conn->t13_send_off = 0;

    conn->write_pos += chunk;
    conn->write_avail -= chunk;
    if (conn->write_avail == 0) conn->write_pos = 0;
    return 1;
}

/*
 * One pump step for the TLS 1.3 path. Drives the handshake state
 * machine while Handshaking, and the record layer while Open/Closing.
 * Returns >0 if work was done, 0 if idle (wait for the notifier),
 * -1 on terminal failure. Same return contract as pump_bearssl_step.
 */
static int
pump_tls13_step(OSTLSConnection *conn, OSTLSEvent *best_event)
{
    /* 1) Flush outbound handshake bytes (our ClientHello / Finished).
     * The state machine must NOT be re-stepped until the current message
     * is fully on the wire, or it would advance / overwrite msg_buf with
     * bytes still unsent. So if anything remains, return now (1 if we made
     * progress this slice, 0 if flow-controlled and idle). */
    if (conn->hs13->msg_offset < conn->hs13->msg_len) {
        UInt32 sent = 0;
        int r = ostls_send_raw(conn,
                               conn->hs13->msg_buf + conn->hs13->msg_offset,
                               (UInt32)(conn->hs13->msg_len - conn->hs13->msg_offset),
                               &sent);
        if (r < 0) return -1;
        conn->hs13->msg_offset += sent;
        if (conn->hs13->msg_offset < conn->hs13->msg_len) {
            return (sent > 0) ? 1 : 0;
        }
        /* fully flushed -- fall through to advance the state machine */
    }

    /* 2) Flush an outbound app-data record already in flight. */
    if (conn->t13_send_off < conn->t13_send_len) {
        UInt32 sent = 0;
        int r = ostls_send_raw(conn,
                               conn->t13_send_rec + conn->t13_send_off,
                               conn->t13_send_len - conn->t13_send_off, &sent);
        if (r < 0) return -1;
        conn->t13_send_off += sent;
        if (conn->t13_send_off >= conn->t13_send_len) {
            conn->t13_send_len = 0;
            conn->t13_send_off = 0;
            event_bump(best_event, kOSTLSEventWritable);
        }
        if (sent > 0) return 1;
    }

    /* 3) Handshake phase. */
    if (conn->state == kOSTLSStateHandshaking &&
        conn->tls13_mode == kT13Mode_Trying) {
        tls13_hs_result r;
        size_t rl = (size_t)conn->tls13_recv_len;

        r = tls13_handshake_step(conn->hs13, conn->ssl_iobuf,
                                 &rl, conn->server_name);
        conn->tls13_recv_len = (UInt32)rl;

        if ((UInt32)conn->hs13->state != conn->t13_dbg_state) {
            conn->t13_dbg_state = (UInt32)conn->hs13->state;
            OSTLS_LogLinef("T13 async: state=%d r=%d rlen=%lu",
                           (int)conn->hs13->state, (int)r,
                           (unsigned long)conn->tls13_recv_len);
        }

        if (conn->hs13->state == kTLS13_Complete &&
            conn->hs13->msg_offset >= conn->hs13->msg_len) {
            conn->handshake_done = true;
            conn->state = kOSTLSStateOpen;
            conn->tls13_mode = kT13Mode_Open;
            conn->cipher_suite = (UInt16)conn->hs13->cipher_suite;
            conn->resumed      = conn->hs13->resumption_accepted;
            OSTLS_LogLinef("T13 async: COMPLETE cipher=0x%04X resumed=%d",
                           (unsigned)conn->cipher_suite, conn->resumed);
            event_bump(best_event, kOSTLSEventHandshakeDone);
            return 1;
        }
        switch (r) {
        case kTLS13_WantRead:
            return pump_ot_recv_tls13(conn);
        case kTLS13_WantWrite:
        case kTLS13_OK:
            return 1;
        case kTLS13_Fallback12: {
            OSTLSEvent ev = ostls_fallback_to_tls12(conn);
            if (ev == kOSTLSEventFailed) *best_event = kOSTLSEventFailed;
            return 1;
        }
        case kTLS13_Error:
        default:
            OSTLS_LogLinef("T13 async: ERROR br=%d state=%d",
                           conn->hs13->error, (int)conn->hs13->state);
            ostls_fail(conn, (OSErr)kOSTLSAsync_BearSSLError,
                       noErr, conn->hs13->error);
            *best_event = kOSTLSEventFailed;
            return 1;
        }
    }

    /* 4) Open / Closing: drive the record layer. */
    if (conn->state == kOSTLSStateOpen ||
        conn->state == kOSTLSStateClosing) {
        /* Deliver staged plaintext before decrypting the next record
         * (plain_buf is the decrypt target -- don't overwrite it). */
        if (conn->t13_plain_off < conn->t13_plain_len) {
            return pump_tls13_deliver_plain(conn, best_event);
        }
        if (conn->write_avail > 0 &&
            conn->t13_send_off >= conn->t13_send_len) {
            if (pump_tls13_build_send(conn)) return 1;
        }
        if (pump_tls13_consume_record(conn, best_event)) return 1;
        if (!conn->peer_closed) {
            int got = pump_ot_recv_tls13(conn);
            if (got != 0) return got;
        }
        if (conn->peer_closed &&
            conn->t13_plain_off >= conn->t13_plain_len &&
            conn->tls13_recv_len < 5 &&
            conn->state != kOSTLSStateClosed &&
            conn->state != kOSTLSStateFailed) {
            conn->state = kOSTLSStateClosed;
            event_bump(best_event, kOSTLSEventClosed);
            return 1;
        }
        return 0;
    }

    return 0;
}


/*
 * Advance BearSSL by exactly one record-pump step. Looks at the
 * engine state and does the single most-useful action available:
 * sendrec, recvrec, sendapp, or recvapp.
 */
static int
pump_bearssl_step(OSTLSConnection *conn, OSTLSEvent *best_event)
{
    unsigned state;
    int br_err;

    state = br_ssl_engine_current_state(&conn->sc.eng);

    if ((state & BR_SSL_CLOSED) != 0) {
        br_err = br_ssl_engine_last_error(&conn->sc.eng);
        if (br_err == BR_ERR_OK) {
            /* Clean close. Drain any buffered plaintext first. */
            if (conn->state == kOSTLSStateOpen ||
                conn->state == kOSTLSStateClosing) {
                conn->state = kOSTLSStateClosed;
                event_bump(best_event, kOSTLSEventClosed);
            } else {
                /* Closed before handshake finished. */
                ostls_fail(conn, (OSErr)kOSTLSAsync_PeerClosed,
                           noErr, 0);
                *best_event = kOSTLSEventFailed;
            }
        } else {
            ostls_fail(conn, (OSErr)kOSTLSAsync_BearSSLError,
                       noErr, br_err);
            *best_event = kOSTLSEventFailed;
        }
        return 1;
    }

    /* Handshake just completed? */
    if (!conn->handshake_done &&
        conn->state == kOSTLSStateHandshaking &&
        (state & (BR_SSL_SENDAPP | BR_SSL_RECVAPP)) != 0) {
        const br_ssl_session_parameters *sess;
        conn->handshake_done = true;
        conn->state = kOSTLSStateOpen;
        sess = &conn->sc.eng.session;
        conn->cipher_suite =
            (UInt16)(sess->cipher_suite & 0xFFFFU);
        /* fixes254 — cache the session parameters for resumption on
         * the next connect to this host:port. Server-side support
         * (modern nginx / Apache / Cloudflare) makes resumption a
         * 1-RTT abbreviated handshake; saves ~700-1200 ms per repeat. */
        {
            char key[OSTLS_SESS_KEY_LEN];
            sess_cache_make_key(conn, key, sizeof key);
            sess_cache_put(key, sess);
            OSTLS_LogLinef("macTLS resume: cached session %s (suite 0x%04X)",
                           key, (unsigned)conn->cipher_suite);
        }
        event_bump(best_event, kOSTLSEventHandshakeDone);
        return 1;
    }

    if ((state & BR_SSL_SENDREC) != 0) {
        int r = pump_ot_send_from_bearssl(conn);
        if (r != 0) return r;
    }
    if ((state & BR_SSL_RECVREC) != 0 && !conn->peer_closed) {
        int r = pump_ot_recv_into_bearssl(conn);
        if (r != 0) return r;
    }
    if (conn->state == kOSTLSStateOpen || conn->state == kOSTLSStateClosing) {
        if ((state & BR_SSL_RECVAPP) != 0) {
            int r = pump_bearssl_recvapp_to_ring(conn, best_event);
            if (r != 0) return r;
        }
        if (conn->write_avail > 0 &&
            (state & BR_SSL_SENDAPP) != 0) {
            int r = pump_ring_to_bearssl_sendapp(conn, best_event);
            if (r != 0) return r;
        }
    }

    if (conn->peer_closed && conn->state != kOSTLSStateClosed && conn->state != kOSTLSStateFailed) {
        if ((state & BR_SSL_RECVAPP) != 0) {
            /* Still draining application data */
            return 0;
        }
        /* No more app data and peer closed. Force close. */
        conn->state = kOSTLSStateClosed;
        event_bump(best_event, kOSTLSEventClosed);
        return 1;
    }

    return 0;
}


/*
 * Handshake-phase timeout. Should the orderly-release event have
 * arrived during connect or handshake, that's also an early-close.
 */
static OSTLSEvent
pump_handshake_or_open_pre(OSTLSConnection *conn)
{
    UInt32 now;

    if (conn->state == kOSTLSStateHandshaking) {
        now = (UInt32)TickCount();
        if (now - conn->handshake_start_ticks >
            conn->handshake_timeout_ticks) {
            return ostls_fail(conn,
                (OSErr)kOSTLSAsync_HandshakeTimeout, noErr, 0);
        }
    }
    if (conn->nf_disconnect && !conn->disc_consumed) {
        OTRcvDisconnect(conn->ep, NULL);
        conn->disc_consumed = true;
        return ostls_fail(conn, (OSErr)kOSTLSAsync_OTSndFail,
                          conn->nf_last_result, 0);
    }
    return kOSTLSEventNone;
}


/* ----------------------------------------------------------------- */
/* OSTLS_Pump                                                        */
/* ----------------------------------------------------------------- */

OSErr
OSTLS_Pump(OSTLSConnection *conn, UInt32 max_steps, OSTLSEvent *out_event)
{
    OSTLSEvent best_event;
    UInt32 steps_used;

    if (out_event != NULL) *out_event = kOSTLSEventNone;
    if (conn == NULL) return (OSErr)kOSTLSAsync_BadArgs;
    if (conn->disposed) return (OSErr)kOSTLSAsync_Disposed;

    conn->dbg_pump_calls++;
    if (conn->bearssl_initialised) {
        conn->br_state_last =
            (UInt32)br_ssl_engine_current_state(&conn->sc.eng);
    }

    best_event = kOSTLSEventNone;
    steps_used = 0;

    /* Terminal-state fast paths: still report the event one time
     * each, with no work. */
    if (conn->state == kOSTLSStateFailed) {
        if (out_event != NULL) *out_event = kOSTLSEventFailed;
        return noErr;
    }
    if (conn->state == kOSTLSStateClosed) {
        if (out_event != NULL) {
            *out_event = (conn->read_avail > 0)
                ? kOSTLSEventReadable
                : kOSTLSEventClosed;
        }
        return noErr;
    }
    if (max_steps == 0) {
        return noErr;
    }

    while (steps_used < max_steps) {
        OSTLSEvent step_ev;

        if (conn->state == kOSTLSStateFailed ||
            conn->state == kOSTLSStateClosed) {
            break;
        }

        /* Pre-step bookkeeping: timeouts, orderly-release. */
        step_ev = pump_handshake_or_open_pre(conn);
        if (step_ev == kOSTLSEventFailed) {
            event_bump(&best_event, kOSTLSEventFailed);
            break;
        }

        if (conn->state == kOSTLSStateConnecting) {
            UInt32 before = steps_used;
            step_ev = pump_connect_step(conn, &steps_used);
            event_bump(&best_event, step_ev);
            if (steps_used == before) {
                /* No progress this iteration; nothing to do until
                 * the next notifier callback. */
                break;
            }
            continue;
        }

        if (conn->state == kOSTLSStateHandshaking ||
            conn->state == kOSTLSStateOpen ||
            conn->state == kOSTLSStateClosing) {
            int did;
            if (conn->tls13_mode != kT13Mode_Off) {
                did = pump_tls13_step(conn, &best_event);
            } else {
                did = pump_bearssl_step(conn, &best_event);
            }
            if (did <= 0) {
                /* Nothing more to do this slice. */
                break;
            }
            steps_used++;
            continue;
        }

        break;
    }

    if (out_event != NULL) *out_event = best_event;
    return noErr;
}


/* ----------------------------------------------------------------- */
/* OSTLS_Write / OSTLS_Read                                          */
/* ----------------------------------------------------------------- */

OSErr
OSTLS_Write(OSTLSConnection *conn, const void *buf,
            UInt32 len, UInt32 *out_written)
{
    UInt32 free_room;
    UInt32 to_copy;

    if (out_written != NULL) *out_written = 0;
    if (conn == NULL || buf == NULL) {
        return (OSErr)kOSTLSAsync_BadArgs;
    }
    if (conn->disposed) return (OSErr)kOSTLSAsync_Disposed;
    if (conn->state != kOSTLSStateOpen) {
        return (OSErr)kOSTLSAsync_WrongState;
    }
    if (len == 0) return noErr;

    /*
     * Append to the linear write_buf. We compact when write_avail
     * drains to 0 (in pump_ring_to_bearssl_sendapp). If there's
     * stale write_pos > 0 and write_avail > 0, compact now to
     * make room.
     */
    if (conn->write_pos > 0 && conn->write_avail > 0) {
        memmove(conn->write_buf,
                conn->write_buf + conn->write_pos,
                conn->write_avail);
        conn->write_pos = 0;
    }
    free_room = (UInt32)sizeof conn->write_buf - conn->write_avail;
    to_copy = (len < free_room) ? len : free_room;
    if (to_copy == 0) {
        /* Buffer full; tell the caller to retry after Pump. */
        return noErr;
    }
    memcpy(conn->write_buf + conn->write_avail,
           buf, to_copy);
    conn->write_avail += to_copy;
    if (out_written != NULL) *out_written = to_copy;
    return noErr;
}


OSErr
OSTLS_Read(OSTLSConnection *conn, void *buf,
           UInt32 cap, UInt32 *out_read)
{
    UInt32 first_chunk;
    UInt32 second_chunk;
    UInt32 to_copy;
    UInt32 ring_size;

    if (out_read != NULL) *out_read = 0;
    if (conn == NULL || buf == NULL) {
        return (OSErr)kOSTLSAsync_BadArgs;
    }
    if (conn->disposed) return (OSErr)kOSTLSAsync_Disposed;
    if (conn->state != kOSTLSStateOpen &&
        conn->state != kOSTLSStateClosing &&
        conn->state != kOSTLSStateClosed) {
        return (OSErr)kOSTLSAsync_WrongState;
    }
    if (cap == 0) return noErr;
    if (conn->read_avail == 0) return noErr;

    ring_size = (UInt32)sizeof conn->read_buf;
    to_copy = conn->read_avail;
    if (to_copy > cap) to_copy = cap;

    /* The ring may wrap; copy in up to two chunks. */
    first_chunk = ring_size - conn->read_pos;
    if (first_chunk > to_copy) first_chunk = to_copy;
    memcpy(buf, conn->read_buf + conn->read_pos, first_chunk);
    second_chunk = to_copy - first_chunk;
    if (second_chunk > 0) {
        memcpy((unsigned char *)buf + first_chunk,
               conn->read_buf,
               second_chunk);
    }
    conn->read_pos = (conn->read_pos + to_copy) % ring_size;
    conn->read_avail -= to_copy;
    if (out_read != NULL) *out_read = to_copy;
    return noErr;
}


/* ----------------------------------------------------------------- */
/* OSTLS_Close                                                       */
/* ----------------------------------------------------------------- */

void
OSTLS_Close(OSTLSConnection *conn)
{
    if (conn == NULL || conn->disposed) return;
    if (conn->close_requested) return;
    conn->close_requested = true;

    if (conn->state == kOSTLSStateClosed ||
        conn->state == kOSTLSStateFailed) {
        return;
    }

    if (conn->state == kOSTLSStateHandshaking ||
        conn->state == kOSTLSStateConnecting) {
        /* Mid-handshake close: hard disconnect; can't gracefully
         * close_notify because the session isn't established. */
        if (conn->ep_open && conn->ep != NULL) {
            OTSndDisconnect(conn->ep, NULL);
        }
        conn->state = kOSTLSStateFailed;
        conn->os_err = (OSErr)kOSTLSAsync_WrongState;
        return;
    }

    if (conn->state == kOSTLSStateOpen ||
        conn->state == kOSTLSStateClosing) {
        if (conn->tls13_mode == kT13Mode_Open) {
            /* 1.3 path: BearSSL's engine isn't carrying this connection,
             * so orderly-disconnect the transport. With HTTP
             * "Connection: close" the peer FINs anyway; the subsequent
             * Pumps observe the orderly release and reach Closed. */
            if (conn->ep_open && conn->ep != NULL) {
                OTSndOrderlyDisconnect(conn->ep);
            }
            conn->close_notify_sent = true;
            conn->state = kOSTLSStateClosing;
            return;
        }
        if (!conn->close_notify_sent) {
            br_ssl_engine_close(&conn->sc.eng);
            conn->close_notify_sent = true;
        }
        conn->state = kOSTLSStateClosing;
        /* The next few Pumps will drain the close_notify out and
         * transition to Closed naturally. */
    }
}


/* ----------------------------------------------------------------- */
/* Introspection                                                     */
/* ----------------------------------------------------------------- */

OSTLSState
OSTLS_GetState(OSTLSConnection *conn)
{
    if (conn == NULL || conn->disposed) return kOSTLSStateFailed;
    return conn->state;
}

OSErr
OSTLS_GetLastError(OSTLSConnection *conn)
{
    if (conn == NULL) return (OSErr)kOSTLSAsync_BadArgs;
    if (conn->disposed) return (OSErr)kOSTLSAsync_Disposed;
    return conn->os_err;
}

UInt16
OSTLS_GetCipherSuite(OSTLSConnection *conn)
{
    if (conn == NULL || conn->disposed) return 0;
    return conn->cipher_suite;
}

int
OSTLS_GetResumed(OSTLSConnection *conn)
{
    if (conn == NULL || conn->disposed) return 0;
    return conn->resumed;
}

void
OSTLS_GetDiagnostics(OSTLSConnection *conn,
                     OSTLSDiagnostics *out_diag)
{
    if (out_diag == NULL) return;
    if (conn == NULL || conn->disposed) {
        out_diag->os_err         = (OSErr)kOSTLSAsync_Disposed;
        out_diag->ot_err         = noErr;
        out_diag->br_err         = 0;
        out_diag->state          = kOSTLSStateFailed;
        out_diag->cipher_suite   = 0;
        out_diag->ot_send_calls  = 0;
        out_diag->ot_send_bytes  = 0;
        out_diag->ot_send_zero   = 0;
        out_diag->ot_send_flow   = 0;
        out_diag->ot_recv_calls  = 0;
        out_diag->ot_recv_bytes  = 0;
        out_diag->ot_recv_nodata = 0;
        out_diag->pump_calls     = 0;
        out_diag->br_state_last  = 0;
        return;
    }
    out_diag->os_err         = conn->os_err;
    out_diag->ot_err         = conn->ot_err;
    out_diag->br_err         = conn->br_err;
    out_diag->state          = conn->state;
    out_diag->cipher_suite   = conn->cipher_suite;
    out_diag->ot_send_calls  = conn->dbg_ot_send_calls;
    out_diag->ot_send_bytes  = conn->dbg_ot_send_bytes;
    out_diag->ot_send_zero   = conn->dbg_ot_send_zero;
    out_diag->ot_send_flow   = conn->dbg_ot_send_flow;
    out_diag->ot_recv_calls  = conn->dbg_ot_recv_calls;
    out_diag->ot_recv_bytes  = conn->dbg_ot_recv_bytes;
    out_diag->ot_recv_nodata = conn->dbg_ot_recv_nodata;
    out_diag->pump_calls     = conn->dbg_pump_calls;
    out_diag->br_state_last  = conn->br_state_last;
}

void *
OSTLS_GetUserRefcon(OSTLSConnection *conn)
{
    if (conn == NULL || conn->disposed) return NULL;
    return conn->user_refcon;
}

void
OSTLS_SetTryTLS13(int enabled)
{
    g_ostls_try_tls13 = enabled ? 1 : 0;
}

/*
 * fixes413 -- SHA-384 known-answer self-test. Returns 0 if SHA-384("abc")
 * equals the NIST vector (the macTLS SHA-384 core computes correctly on this
 * build), nonzero if wrong. The host logs the result at startup so we can see
 * ON-DEVICE whether the fixes411 CW8 SHA-384 fix is live and correct -- the
 * defect this gates was invisible for the life of the project precisely
 * because nothing ever checked SHA-384 on the metal.
 */
int
OSTLS_SHA384_KAT(void)
{
    br_sha384_context sc;
    unsigned char out[48];
    unsigned char msg[200];
    int i;
    static const unsigned char exp_abc[48] = {
        0xcb,0x00,0x75,0x3f,0x45,0xa3,0x5e,0x8b,
        0xb5,0xa0,0x3d,0x69,0x9a,0xc6,0x50,0x07,
        0x27,0x2c,0x32,0xab,0x0e,0xde,0xd1,0x63,
        0x1a,0x8b,0x60,0x5a,0x43,0xff,0x5b,0xed,
        0x80,0x86,0x07,0x2b,0xa1,0xe7,0xcc,0x23,
        0x58,0xba,0xec,0xa1,0x34,0xc8,0x25,0xa7
    };
    /* fixes414 -- multi-block vectors. "abc" is a single 128-byte SHA-512
     * block, which cannot expose a bug in the block-chaining loop or the
     * length-pad; a certificate's signed body is multi-block (Sectigo R36's
     * TBS is 1080 bytes = 9 blocks). 200x'a' crosses block chaining; 112x'a'
     * exercises the ptr>112 two-block padding branch in sha2big_out. If
     * either fails while "abc" passes, the multi-block SHA-384 path is the
     * reason Sectigo/SHA-384 cert chains are still rejected. */
    static const unsigned char exp_200a[48] = {
        0x06,0x91,0xb6,0xe9,0x78,0x61,0x4b,0x67,
        0xd6,0x05,0x57,0xb2,0xa2,0xcd,0xdd,0x53,
        0x40,0x65,0x08,0x52,0x2e,0xfa,0x21,0xc6,
        0x24,0xdb,0xbf,0xa8,0xab,0x6e,0x72,0x6d,
        0x5c,0x58,0x6b,0x48,0x9c,0x7c,0x09,0xf2,
        0x41,0x09,0xa6,0x4c,0x10,0x21,0x1d,0x48
    };
    static const unsigned char exp_112a[48] = {
        0x18,0x7d,0x4e,0x07,0xcb,0x30,0x61,0x03,
        0xc6,0x99,0x67,0xbf,0x54,0x4d,0x0d,0xfb,
        0xe9,0x04,0x25,0x77,0x59,0x9c,0x73,0xc3,
        0x30,0xab,0xc0,0xcb,0x64,0xc6,0x12,0x36,
        0xd5,0xed,0x56,0x5e,0xe1,0x91,0x19,0xd8,
        0xc3,0x17,0x79,0xa3,0x8f,0x79,0x1f,0xcd
    };

    br_sha384_init(&sc);
    br_sha384_update(&sc, "abc", 3);
    br_sha384_out(&sc, out);
    if (memcmp(out, exp_abc, 48) != 0) return 1;

    for (i = 0; i < 200; i++) {
        msg[i] = 0x61;  /* 'a' */
    }
    br_sha384_init(&sc);
    br_sha384_update(&sc, msg, 200);
    br_sha384_out(&sc, out);
    if (memcmp(out, exp_200a, 48) != 0) return 2;

    br_sha384_init(&sc);
    br_sha384_update(&sc, msg, 112);
    br_sha384_out(&sc, out);
    if (memcmp(out, exp_112a, 48) != 0) return 3;

    return 0;
}
