/*
 * MacTLSTest -- standalone Carbon app for Stage A validation.
 *
 * What it does (and ONLY does):
 *   1. Toolbox / Appearance init (Carbon-style, skip the deprecated
 *      InitGraf/InitFonts/etc. inits per CarbonLib convention).
 *   2. Call OSTLS_SmokeTest() once.
 *   3. Open a small window and DrawString the result code.
 *   4. Wait for a mouse click or key press, then quit.
 *
 * What it deliberately does NOT do:
 *   - No Open Transport (smoke test is non-network by design).
 *   - No QuickTime, no Appearance themes beyond default, no menus.
 *   - No file I/O, no preferences, no settings, no resources beyond
 *     the 'carb' Carbon-fragment marker in MacTLSTest.rsrc.
 *
 * Once the smoke test passes on real OS 9 hardware (and the Stage A.5
 * mul64 probe passes too), this file evolves into the seed of
 * MacTLS Proxy: replace the "show dialog + quit" tail with a TCP
 * listener + HTTP proxy parser + BearSSL upstream fetch.
 */

#include "mactlstest_prefix.h"

/*
 * Avoid the <Carbon.h> umbrella. Carbon.h chains through CoreServices.h
 * which pulls in Threads.h. On CW8 installations where the Java SDK
 * support folder is on the system search path, a HotSpot JVM Threads.h
 * can be found before Apple's, producing a cascade of JVM-internal
 * errors (oobj.h, typedefs.h, winnt.h, winbase.h ...). Including the
 * specific Toolbox headers we actually need avoids that chain entirely
 * and is also faster to compile.
 */
#include <Quickdraw.h>
#include <Windows.h>
#include <Events.h>
#include <Fonts.h>
#include <Gestalt.h>
#include <Sound.h>
#ifdef __MWERKS__
#include <Appearance.h>     /* CarbonLib only; absent from Retro68 pre-flight */
#include <Files.h>          /* Open Transport pulls in Files types */
#include <OpenTransport.h>  /* InitOpenTransportInContext / OTClientContextPtr */
#include <OpenTptInternet.h>/* TCP configuration constants */
#endif

#include <stdio.h>
#include <string.h>

#include "ostls_smoketest.h"
#include "ostls_mul64_probe.h"
#include "ostls_b1_tcp.h"
#include "ostls_b2_handshake.h"
#include "ostls_b3_handshake.h"
#include "ostls_fetch.h"
#include "ostls_d1_probe.h"
#include "ostls_async.h"
#include "ostls_entropy.h"
#include "ostls_tls13_selftest.h"
#include "ostls_tls13_otprobe.h"
#include "ostls_log.h"


/*
 * Stage B targets.
 *   B1 (TCP only):           example.com:443 -- public TCP sanity check
 *   B2 (insecure handshake): example.com:443 -- isolates the record I/O
 *                            loop from any validation question
 *   B3 (validated handshake): google.com:443 -- chains through GTS Root R1
 *                             (or GTS Root R4 for the ECDSA path), both
 *                             embedded in ostls_b3_anchors. Picked over
 *                             example.com so we're validating macTLS's
 *                             X.509 path, not chasing IANA cert rotation.
 */
#define OSTLS_B1_TARGET     "example.com:443"
#define OSTLS_B2_TARGET     "example.com:443"
#define OSTLS_B2_SERVERNAME "example.com"
#define OSTLS_B3_TARGET     "google.com:443"
#define OSTLS_B3_SERVERNAME "google.com"

/*
 * Stage D library-call target. Same host MacTLSTest has been
 * exercising end-to-end since Stage B4 -- google.com:443 over the
 * embedded GTS Root R4 trust anchor.
 */
#define OSTLS_D_HOST        "google.com"
#define OSTLS_D_PORT        443
#define OSTLS_D_SERVERNAME  "google.com"
#define OSTLS_D_PATH        "/"

/*
 * Stage D1 (async OT notifier smoke) target. Same host as D / D2;
 * D1 tears down right after T_CONNECT without TLS, so it's just
 * a TCP-level test of the async-OT plumbing.
 */
#define OSTLS_D1_TARGET     "google.com:443"

/*
 * Stage D2 (full async TLS) targets. Drives the OSTLSConnection
 * state machine directly via OSTLS_New/Start/Pump/Write/Read.
 */
#define OSTLS_D2_HOST       "google.com"
#define OSTLS_D2_PORT       443
#define OSTLS_D2_SERVERNAME "google.com"
#define OSTLS_D2_PATH       "/"


/*
 * Open Transport client context, populated by InitOpenTransportInContext
 * at startup and torn down at shutdown. The B1 probe and any later
 * Stage B code that opens endpoints reference this via
 *   extern OTClientContextPtr g_ostls_ot_context;
 * in their own source files.
 *
 * CW8's OT headers don't always define kInitOTForApplicationMask; per
 * the MacSurf workaround we pin it to the documented bit value when
 * the macro is missing.
 */
#ifdef __MWERKS__
OTClientContextPtr g_ostls_ot_context = NULL;
#ifndef kInitOTForApplicationMask
#define kInitOTForApplicationMask 0x00000002L
#endif
#else
void *g_ostls_ot_context = NULL;
#endif


/* ------------------------------------------------------------------- */
/* Helpers                                                             */
/* ------------------------------------------------------------------- */

/*
 * Convert a C string to a Pascal string in-place into the supplied
 * buffer. Truncates at 254 chars. CW8 has c2pstr() but it mutates the
 * source buffer; this version is non-destructive and safer.
 */
static void
c_to_pstr(const char *src, Str255 dst)
{
    size_t n = strlen(src);
    if (n > 254) {
        n = 254;
    }
    dst[0] = (unsigned char)n;
    memcpy(&dst[1], src, n);
}


/*
 * Decode an OSTLS smoke-test or Mul64-probe result code into a
 * human-readable label. Codes correspond to kOSTLSSmoke* in
 * os9/ostls_smoketest.h and kOSTLSMul64* in os9/ostls_mul64_probe.h.
 */
static const char *
smoke_label(OSErr code)
{
    switch ((short)code) {
    case noErr:                            return "OK";
    case kOSTLSSmokeBadEngineAfterReset:   return "engine error after reset";
    case kOSTLSSmokeNoSendrecAfterReset:   return "engine not asking to send";
    case kOSTLSSmokeClientResetReturned0:  return "br_ssl_client_reset = 0";
    case kOSTLSSmokeEntropyInjectFailed:   return "entropy inject failed";
    case kOSTLSMul64FailA_Raw:             return "mul64 A raw -- 0x12345678 * 0x9ABCDEF0";
    case kOSTLSMul64FailA_CT:              return "mul64 A CT  -- |0x80000000 pattern A";
    case kOSTLSMul64FailB_Raw:             return "mul64 B raw -- 0x7FFFFFFF^2";
    case kOSTLSMul64FailB_CT:              return "mul64 B CT  -- |0x80000000 pattern B";
    case kOSTLSMul64FailC_Raw:             return "mul64 C raw -- 0x00010001^2";
    case kOSTLSMul64FailC_CT:              return "mul64 C CT  -- |0x80000000 pattern C";
    case kOSTLSMul64FailD_Raw:             return "mul64 D raw -- 0xDEADBEEF * 0xCAFEBABE";
    case kOSTLSMul64FailD_CT:              return "mul64 D CT  -- |0x80000000 pattern D";
    case kOSTLSB1_BadArgs:                 return "B1 bad args";
    case kOSTLSB1_OTConfigFail:            return "B1 OTCreateConfiguration failed";
    case kOSTLSB1_OTOpenEndptFail:         return "B1 OTOpenEndpointInContext failed";
    case kOSTLSB1_OTBindFail:              return "B1 OTBind failed";
    case kOSTLSB1_OTDnsAddrFail:           return "B1 OTInitDNSAddress failed";
    case kOSTLSB1_OTConnectFail:           return "B1 OTConnect failed";
    case kOSTLSB2_BadArgs:                 return "B2 bad args";
    case kOSTLSB2_OTConfigFail:            return "B2 OTCreateConfiguration failed";
    case kOSTLSB2_OTOpenEndptFail:         return "B2 OTOpenEndpoint failed";
    case kOSTLSB2_OTBindFail:              return "B2 OTBind failed";
    case kOSTLSB2_OTDnsAddrFail:           return "B2 OTInitDNSAddress failed";
    case kOSTLSB2_OTConnectFail:           return "B2 OTConnect failed";
    case kOSTLSB2_EntropyFail:             return "B2 entropy inject failed";
    case kOSTLSB2_ClientResetFail:         return "B2 br_ssl_client_reset failed";
    case kOSTLSB2_OTSndFail:               return "B2 OTSnd failed mid-handshake";
    case kOSTLSB2_OTRcvFail:               return "B2 OTRcv failed mid-handshake";
    case kOSTLSB2_PeerClosedEarly:         return "B2 peer closed before handshake";
    case kOSTLSB2_HandshakeTimeout:        return "B2 handshake timed out (60s)";
    case kOSTLSB2_BearSSLError:            return "B2 BearSSL engine error";
    case kOSTLSB3_BadArgs:                 return "B3 bad args";
    case kOSTLSB3_ClockBefore2000:         return "B3 system clock before 2000";
    case kOSTLSB3_OTConfigFail:            return "B3 OTCreateConfiguration failed";
    case kOSTLSB3_OTOpenEndptFail:         return "B3 OTOpenEndpoint failed";
    case kOSTLSB3_OTBindFail:              return "B3 OTBind failed";
    case kOSTLSB3_OTDnsAddrFail:           return "B3 OTInitDNSAddress failed";
    case kOSTLSB3_OTConnectFail:           return "B3 OTConnect failed";
    case kOSTLSB3_EntropyFail:             return "B3 entropy inject failed";
    case kOSTLSB3_ClientResetFail:         return "B3 br_ssl_client_reset failed";
    case kOSTLSB3_OTSndFail:               return "B3 OTSnd failed mid-handshake";
    case kOSTLSB3_OTRcvFail:               return "B3 OTRcv failed mid-handshake";
    case kOSTLSB3_PeerClosedEarly:         return "B3 peer closed before handshake";
    case kOSTLSB3_HandshakeTimeout:        return "B3 handshake timed out";
    case kOSTLSB3_X509NotTrusted:          return "B3 X509 unknown CA / chain not trusted";
    case kOSTLSB3_X509Expired:             return "B3 X509 cert expired / not yet valid";
    case kOSTLSB3_X509HostnameMismatch:    return "B3 X509 hostname mismatch";
    case kOSTLSB3_X509BadSignature:        return "B3 X509 bad signature";
    case kOSTLSB3_X509TimeUnknown:         return "B3 X509 time unknown (clock?)";
    case kOSTLSB3_X509Other:               return "B3 X509 other validation error";
    case kOSTLSB3_BearSSLError:            return "B3 BearSSL engine error";
    case kOSTLSFetch_BadArgs:              return "Fetch bad args";
    case kOSTLSFetch_ClockBefore2000:      return "Fetch system clock before 2000";
    case kOSTLSFetch_OTConfigFail:         return "Fetch OTCreateConfiguration failed";
    case kOSTLSFetch_OTOpenEndptFail:      return "Fetch OTOpenEndpoint failed";
    case kOSTLSFetch_OTBindFail:           return "Fetch OTBind failed";
    case kOSTLSFetch_OTDnsAddrFail:        return "Fetch OTInitDNSAddress failed";
    case kOSTLSFetch_OTConnectFail:        return "Fetch OTConnect failed";
    case kOSTLSFetch_EntropyFail:          return "Fetch entropy inject failed";
    case kOSTLSFetch_ClientResetFail:      return "Fetch br_ssl_client_reset failed";
    case kOSTLSFetch_OTSndFail:            return "Fetch OTSnd failed";
    case kOSTLSFetch_OTRcvFail:            return "Fetch OTRcv failed";
    case kOSTLSFetch_HandshakeTimeout:     return "Fetch handshake/IO timed out";
    case kOSTLSFetch_BearSSLError:         return "Fetch BearSSL engine error";
    case kOSTLSFetch_RequestTooBig:        return "Fetch request too big";
    case kOSTLSFetch_NoBytesReceived:      return "Fetch peer closed before any plaintext";
    case kOSTLSD1_BadArgs:                 return "D1 bad args";
    case kOSTLSD1_OTConfigFail:            return "D1 OTCreateConfiguration failed";
    case kOSTLSD1_OTAsyncOpenFail:         return "D1 OTAsyncOpenEndpoint failed";
    case kOSTLSD1_OpenTimeout:             return "D1 T_OPENCOMPLETE timeout";
    case kOSTLSD1_OpenBadResult:           return "D1 T_OPENCOMPLETE bad result";
    case kOSTLSD1_OTBindSubmitFail:        return "D1 OTBind submit failed";
    case kOSTLSD1_BindTimeout:             return "D1 T_BINDCOMPLETE timeout";
    case kOSTLSD1_BindBadResult:           return "D1 T_BINDCOMPLETE bad result";
    case kOSTLSD1_OTDnsAddrFail:           return "D1 OTInitDNSAddress failed";
    case kOSTLSD1_OTConnectSubmitFail:     return "D1 OTConnect submit failed";
    case kOSTLSD1_ConnectTimeout:          return "D1 T_CONNECT timeout";
    case kOSTLSD1_ConnectBadResult:        return "D1 T_CONNECT bad result";
    case kOSTLSD1_DisconnectArrived:       return "D1 T_DISCONNECT during connect";
    case kOSTLSAsync_BadArgs:              return "Async bad args";
    case kOSTLSAsync_NoMemory:             return "Async NewPtrClear failed";
    case kOSTLSAsync_ClockBefore2000:      return "Async system clock before 2000";
    case kOSTLSAsync_OTConfigFail:         return "Async OTCreateConfiguration failed";
    case kOSTLSAsync_OTOpenFail:           return "Async OTAsyncOpenEndpoint failed";
    case kOSTLSAsync_OTBindFail:           return "Async OTBind failed";
    case kOSTLSAsync_OTConnectFail:        return "Async OTConnect failed";
    case kOSTLSAsync_OTNotifierFail:       return "Async notifier install failed";
    case kOSTLSAsync_OTSndFail:            return "Async OTSnd failed";
    case kOSTLSAsync_OTRcvFail:            return "Async OTRcv failed";
    case kOSTLSAsync_EntropyFail:          return "Async entropy inject failed";
    case kOSTLSAsync_ClientResetFail:      return "Async br_ssl_client_reset failed";
    case kOSTLSAsync_BearSSLError:         return "Async BearSSL engine error";
    case kOSTLSAsync_ConnectTimeout:       return "Async connect timeout";
    case kOSTLSAsync_HandshakeTimeout:     return "Async handshake timeout";
    case kOSTLSAsync_PeerClosed:           return "Async peer closed before handshake";
    case kOSTLSAsync_WrongState:           return "Async wrong-state API call";
    case kOSTLSAsync_Disposed:             return "Async operation on disposed conn";
    }
    return "unknown failure";
}


/* ------------------------------------------------------------------- */
/* Toolbox init (Carbon discipline)                                    */
/* ------------------------------------------------------------------- */

/*
 * Mirror MacSurf's pattern: skip InitGraf/InitFonts/InitWindows/
 * InitMenus/TEInit/InitDialogs under Carbon, but keep InitCursor() and
 * FlushEvents(). Register the Appearance Manager only if Gestalt
 * confirms it is present.
 */
static void
toolbox_init(void)
{
    InitCursor();
    FlushEvents(everyEvent, 0);

#ifdef __MWERKS__
    /* Gestalt-gated Appearance init. Skipped under Retro68 pre-flight
     * (multiversal headers don't ship Appearance.h); CW8's Carbon.h
     * pulls in everything we need. */
    {
        long response;
        if (Gestalt(gestaltAppearanceAttr, &response) == noErr) {
            RegisterAppearanceClient();
        }
    }
#endif
}


/* ------------------------------------------------------------------- */
/* Result display                                                      */
/* ------------------------------------------------------------------- */

/*
 * Open a single modeless window, draw the result, and pump events
 * until the user clicks or types. No menu bar, no fancy chrome.
 *
 * Caller supplies the three text lines and the title. main() owns the
 * stage-result logic; this function just renders.
 */
static void
show_result_and_wait(const char *title_c,
                     const char *line1_c,
                     const char *line2_c)
{
    WindowRef win;
    Rect bounds;
    EventRecord ev;
    Boolean done = false;
    Str255 title;
    Str255 line1;
    Str255 line2;
    Str255 line3;

    c_to_pstr(title_c, title);
    c_to_pstr(line1_c, line1);
    c_to_pstr(line2_c, line2);
    c_to_pstr("Click mouse or press any key to quit.", line3);

    /* Open the window. */
    SetRect(&bounds, 60, 60, 540, 220);
    win = NewCWindow(NULL, &bounds, title, true,
        noGrowDocProc, (WindowRef)-1L, true, 0L);
    if (win == NULL) {
        SysBeep(30);
        return;
    }
    SetPortWindowPort(win);

    /* Pump events until quit. */
    while (!done) {
        OSTLS_CollectEntropy();
        WaitNextEvent(everyEvent, &ev, 30, NULL);
        switch (ev.what) {
        case mouseDown:
        case keyDown:
            done = true;
            break;
        case updateEvt:
            BeginUpdate((WindowRef)ev.message);
            if ((WindowRef)ev.message == win) {
                Rect r;
                GetWindowPortBounds(win, &r);
                EraseRect(&r);

                TextFont(kFontIDGeneva);
                TextSize(12);
                TextFace(bold);
                MoveTo(20, 40);
                DrawString(line1);

                TextFace(0);
                MoveTo(20, 70);
                DrawString(line2);

                TextSize(10);
                MoveTo(20, 130);
                DrawString(line3);
            }
            EndUpdate((WindowRef)ev.message);
            break;
        }
    }

    DisposeWindow(win);
}


/* ------------------------------------------------------------------- */
/* Entry point                                                         */
/* ------------------------------------------------------------------- */

/*
 * Open Transport lifecycle. Initialised once at startup, closed once at
 * shutdown. The B1 probe (and any subsequent Stage B work) opens its
 * own endpoints against this context. Failure is non-fatal -- we still
 * run the in-memory probes (A.5, A) so the user sees a useful result
 * window even on a machine where OT is missing or broken.
 */
static OSStatus
ostls_ot_init(void)
{
#ifdef __MWERKS__
    return InitOpenTransportInContext(kInitOTForApplicationMask,
                                      &g_ostls_ot_context);
#else
    return 0;
#endif
}

static void
ostls_ot_close(void)
{
#ifdef __MWERKS__
    if (g_ostls_ot_context != NULL) {
        CloseOpenTransportInContext(g_ostls_ot_context);
        g_ostls_ot_context = NULL;
    }
#endif
}


/*
 * Stage D3 helper: one async connect + handshake to host:port, then
 * close. Returns the bytes received during the handshake (0 on failure).
 * A full handshake pulls the server cert chain (several KB); a resumed
 * one skips it, so a much smaller number on a repeat connect to the same
 * host is the signal that session resumption kicked in.
 */
static UInt32
d3_handshake_recv(const char *host, UInt16 port, const char *sni)
{
    OSTLSConnection *conn;
    OSTLSConfig      cfg;
    OSTLSEvent       ev;
    OSTLSState       st;
    OSTLSDiagnostics diag;
    OSErr            err;
    UInt32           deadline;
    UInt32           recv_bytes;

    memset(&cfg, 0, sizeof cfg);
    cfg.host = host;
    cfg.port = port;
    cfg.server_name = sni;

    conn = NULL;
    if (OSTLS_New(&conn, &cfg) != kOSTLSAsync_OK || conn == NULL) {
        return 0;
    }
    if (OSTLS_Start(conn) != kOSTLSAsync_OK) {
        OSTLS_Dispose(conn);
        return 0;
    }

    recv_bytes = 0;
    deadline = (UInt32)TickCount() + 60UL * 60UL;   /* 60s */
    for (;;) {
        if ((UInt32)TickCount() > deadline) {
            OSTLS_Close(conn);
            OSTLS_Dispose(conn);
            return 0;
        }
        err = OSTLS_Pump(conn, 6, &ev);
        {
            EventRecord nullev;
            WaitNextEvent(everyEvent, &nullev, 1, NULL);
        }
        if (err != kOSTLSAsync_OK) {
            OSTLS_Close(conn);
            OSTLS_Dispose(conn);
            return 0;
        }
        st = OSTLS_GetState(conn);
        if (st == kOSTLSStateFailed) {
            OSTLS_Dispose(conn);
            return 0;
        }
        if (st == kOSTLSStateOpen) {
            OSTLS_GetDiagnostics(conn, &diag);
            recv_bytes = diag.ot_recv_bytes;
            break;
        }
    }

    OSTLS_Close(conn);
    OSTLS_Dispose(conn);
    return recv_bytes;
}

/*
 * Stage D4 helper: async connect + full TLS 1.3, then send a GET and keep
 * reading for a few seconds. Reading PAST the handshake is the point: the
 * server's post-handshake NewSessionTicket arrives during the response and
 * gets harvested into the resumption cache, so a second connect to the same
 * host can resume. Returns handshake recv bytes (0 on failure); a resumed
 * handshake skips the server cert chain, so a much smaller value on the
 * second connect is the signal. macTLS#2 Stage F.
 */
static UInt32
d4_resume_recv(const char *host, UInt16 port, const char *sni)
{
    OSTLSConnection *conn;
    OSTLSConfig      cfg;
    OSTLSEvent       ev;
    OSTLSState       st;
    OSTLSDiagnostics diag;
    OSErr            err;
    UInt32           deadline;
    UInt32           recv_bytes;
    UInt32           read_until;
    int              sent;
    int              open_seen;
    char             req[160];
    char             buf[512];

    memset(&cfg, 0, sizeof cfg);
    cfg.host = host;
    cfg.port = port;
    cfg.server_name = sni;

    conn = NULL;
    if (OSTLS_New(&conn, &cfg) != kOSTLSAsync_OK || conn == NULL) return 0;
    if (OSTLS_Start(conn) != kOSTLSAsync_OK) { OSTLS_Dispose(conn); return 0; }

    sprintf(req, "GET / HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", host);
    recv_bytes = 0; sent = 0; open_seen = 0; read_until = 0;
    deadline = (UInt32)TickCount() + 60UL * 60UL;     /* 60s overall */
    for (;;) {
        EventRecord nullev;
        if ((UInt32)TickCount() > deadline) break;
        err = OSTLS_Pump(conn, 6, &ev);
        WaitNextEvent(everyEvent, &nullev, 1, NULL);
        if (err != kOSTLSAsync_OK) break;
        st = OSTLS_GetState(conn);
        if (st == kOSTLSStateFailed) { OSTLS_Dispose(conn); return 0; }
        if (!open_seen && st == kOSTLSStateOpen) {
            OSTLS_GetDiagnostics(conn, &diag);
            recv_bytes = diag.ot_recv_bytes;
            open_seen = 1;
            read_until = (UInt32)TickCount() + 5UL * 60UL; /* drain ~5s */
        }
        if (st == kOSTLSStateOpen && !sent) {
            UInt32 w = 0;
            OSTLS_Write(conn, req, (UInt32)strlen(req), &w);
            if (w == (UInt32)strlen(req)) sent = 1;
        }
        if (open_seen) {
            UInt32 nread;
            for (;;) {
                nread = 0;
                if (OSTLS_Read(conn, buf, (UInt32)sizeof buf, &nread)
                        != kOSTLSAsync_OK || nread == 0) break;
            }
        }
        if (st == kOSTLSStateClosed) break;
        if (open_seen && (UInt32)TickCount() > read_until) break;
    }
    OSTLS_Close(conn);
    OSTLS_Dispose(conn);
    return recv_bytes;
}


int
main(void)
{
    OSErr     a5_result;
    OSErr     a_result;
    OSErr     b1_result;
    OSStatus  ot_init_status;
    char      b1_msg[160];
    char      line1_buf[200];
    char      line2_buf[200];
    char      title_buf[80];

    toolbox_init();

    /*
     * Bring up the file-backed logger first so every probe result
     * is captured on disk in MacTLSTest.log on the Desktop. The log
     * is the durable record we read back over scp; the result window
     * is just the live UI.
     */
    (void)OSTLS_LogInit();
    OSTLS_LogLine("==== MacTLSTest run ====");

    b1_msg[0] = '\0';

    /*
     * Stage A.5: 32x32->64 multiply codegen probe. If the underlying
     * arithmetic is wrong on this hardware, every Stage A or Stage B
     * pass below would be meaningless. Probe the kernel first.
     */
    OSTLS_LogLine("Stage A.5  mul64 probe              ...");
    a5_result = OSTLS_Mul64Probe();
    OSTLS_LogLinef("Stage A.5  mul64 probe              -> code=%d %s",
                   (int)a5_result,
                   (a5_result == noErr) ? "OK" : smoke_label(a5_result));
    if (a5_result != noErr) {
        sprintf(title_buf, "MacTLSTest -- mul64 probe FAILED");
        sprintf(line1_buf, "Stage A.5 mul64 FAILED (code %d)",
                (int)a5_result);
        sprintf(line2_buf, "Gate: %s", smoke_label(a5_result));
        show_result_and_wait(title_buf, line1_buf, line2_buf);
        OSTLS_LogClose();
        return 0;
    }

    /*
     * Stage A: BearSSL build / init / state machine reaches SENDREC.
     * Pure in-memory; does not touch OT. If this fails the link is
     * fine but BearSSL itself can't get out of the starting blocks.
     */
    OSTLS_LogLine("Stage A    BearSSL smoke            ...");
    a_result = OSTLS_SmokeTest();
    OSTLS_LogLinef("Stage A    BearSSL smoke            -> code=%d %s",
                   (int)a_result,
                   (a_result == noErr) ? "OK (engine reports SENDREC)"
                                       : smoke_label(a_result));
    if (a_result != noErr) {
        sprintf(title_buf, "MacTLSTest -- Stage A smoke FAILED");
        sprintf(line1_buf, "Stage A smoke FAILED (code %d)",
                (int)a_result);
        sprintf(line2_buf, "Gate: %s", smoke_label(a_result));
        show_result_and_wait(title_buf, line1_buf, line2_buf);
        OSTLS_LogClose();
        return 0;
    }

    /*
     * Stage E: entropy statistical self-test. In-memory; validates the
     * macEntropy pool produces non-degenerate output (distinct seeds,
     * byte-value spread, bit balance near 50%). Non-fatal -- logs
     * PASS/FAIL plus a 32-bit batch fingerprint, then continues so the
     * network stages still run. The fingerprint MUST differ across
     * separate launches; that cross-run difference is the real entropy
     * proof (the within-run checks only catch degenerate output).
     */
    {
        unsigned long e_fp = 0;
        int e_fail;
        OSTLS_LogLine("Stage E    entropy selftest         ...");
        e_fail = OSTLS_EntropySelfTest(&e_fp);
        OSTLS_LogLinef("Stage E    entropy selftest         -> %s (fp=%08lX)",
                       e_fail ? "FAIL" : "PASS", e_fp);
    }

    /*
     * Stage F: TLS 1.3 crypto self-test. In-memory; runs the key schedule
     * and record layer against RFC 8446/8448 vectors plus a record
     * round-trip and tamper-reject. This is the first time the ported
     * TLS 1.3 code runs on real PPC -- it confirms the C89 port compiles
     * under CW8 and produces correct results (incl. the 64-bit record
     * sequence number). No network. The handshake itself is exercised
     * later once it's wired into the OT transport (Stage D integration).
     */
    {
        int f_fail;
        OSTLS_LogLine("Stage F    TLS 1.3 crypto self-test  ...");
        f_fail = OSTLS_TLS13_SelfTest();
        OSTLS_LogLinef("Stage F    TLS 1.3 crypto self-test  -> %s (%d failure(s))",
                       f_fail ? "FAIL" : "PASS", f_fail);
    }

    /*
     * Stage B1: raw Open Transport TCP connect probe. First time this
     * binary touches the network. OT must be initialised before the
     * probe runs; we report initialisation failure distinctly so a
     * "no OT on this machine" result doesn't get conflated with "OT
     * fine, but this host is unreachable".
     */
    OSTLS_LogLine("OT init    InitOpenTransportInContext...");
    ot_init_status = ostls_ot_init();
    OSTLS_LogLinef("OT init    InitOpenTransportInContext-> ot_err=%ld%s",
                   (long)ot_init_status,
                   (ot_init_status == noErr) ? " OK" : " FAIL");
    if (ot_init_status != noErr) {
        sprintf(title_buf, "MacTLSTest -- OT init FAILED");
        sprintf(line1_buf,
                "InitOpenTransportInContext FAILED (ot_err=%ld)",
                (long)ot_init_status);
        sprintf(line2_buf,
                "Stage A + A.5 OK; OT unavailable so B1 was skipped.");
        show_result_and_wait(title_buf, line1_buf, line2_buf);
        OSTLS_LogClose();
        return 0;
    }

    OSTLS_LogLinef("Stage B1   OT TCP connect target=%s ...",
                   OSTLS_B1_TARGET);
    b1_result = OSTLS_B1_TCP_Probe(OSTLS_B1_TARGET, b1_msg, sizeof b1_msg);
    OSTLS_LogLinef("Stage B1   OT TCP connect           -> code=%d %s",
                   (int)b1_result, b1_msg);

    if (b1_result != kOSTLSB1_OK) {
        sprintf(title_buf, "MacTLSTest -- Stage B1 FAILED");
        sprintf(line1_buf, "Stage B1 FAILED (code %d): %.140s",
                (int)b1_result, b1_msg);
        sprintf(line2_buf,
                "Gate: %s (target=%s)",
                smoke_label(b1_result), OSTLS_B1_TARGET);
        show_result_and_wait(title_buf, line1_buf, line2_buf);
        ostls_ot_close();
        OSTLS_LogClose();
        return 0;
    }

    /*
     * Stage B2: BearSSL handshake (insecure validator) over OT against
     * the same target. First time TLS bytes actually flow on this
     * platform. Uses gB2-static contexts so the partition footprint is
     * predictable up front.
     */
    {
        OSErr b2_result;
        char  b2_msg[180];

        OSTLS_LogLinef("Stage B2   TLS handshake (insecure) target=%s ...",
                       OSTLS_B2_TARGET);
        b2_result = OSTLS_B2_Handshake_Probe(
            OSTLS_B2_TARGET, OSTLS_B2_SERVERNAME,
            b2_msg, sizeof b2_msg);
        OSTLS_LogLinef("Stage B2   TLS handshake (insecure) -> code=%d %s",
                       (int)b2_result, b2_msg);

        if (b2_result != kOSTLSB2_OK) {
            sprintf(title_buf, "MacTLSTest -- Stage B2 FAILED");
            sprintf(line1_buf, "Stage B2 FAILED (code %d): %.140s",
                (int)b2_result, b2_msg);
            sprintf(line2_buf,
                "Gate: %s (target=%s)",
                smoke_label(b2_result), OSTLS_B2_TARGET);
            show_result_and_wait(title_buf, line1_buf, line2_buf);
            ostls_ot_close();
            OSTLS_LogClose();
            return 0;
        }
    }

    /*
     * Stage B3: validated TLS handshake against google.com:443. Real
     * br_x509_minimal with embedded trust anchors and the Mac system
     * clock fed in. Failures here distinguish CA / expiry / hostname
     * / clock / signature errors via dedicated result codes so the
     * window points at exactly which validation gate broke.
     */
    {
        OSErr b3_result;
        char  b3_msg[180];

        OSTLS_LogLinef("Stage B3   TLS handshake (validated) target=%s ...",
                       OSTLS_B3_TARGET);
        b3_result = OSTLS_B3_Validated_Probe(
            OSTLS_B3_TARGET, OSTLS_B3_SERVERNAME,
            b3_msg, sizeof b3_msg);
        OSTLS_LogLinef("Stage B3   TLS handshake (validated) -> code=%d %s",
                       (int)b3_result, b3_msg);

        if (b3_result != kOSTLSB3_OK) {
            sprintf(title_buf, "MacTLSTest -- Stage B3 FAILED");
            sprintf(line1_buf, "Stage B3 FAILED (code %d): %.140s",
                (int)b3_result, b3_msg);
            sprintf(line2_buf,
                "Gate: %s (target=%s)",
                smoke_label(b3_result), OSTLS_B3_TARGET);
            OSTLS_LogBlank();
            OSTLS_LogLinef("==== Stage B3 FAILED (code=%d) ====",
                           (int)b3_result);
            show_result_and_wait(title_buf, line1_buf, line2_buf);
            ostls_ot_close();
            OSTLS_LogClose();
            return 0;
        }
    }

    /*
     * Stage D1: async OT notifier smoke. Proves OTAsyncOpenEndpoint +
     * notifier dispatch works for Open / Bind / Connect events on
     * Carbon CFM. No BearSSL. Permanent regression -- the v0.2
     * async TLS path (Stage D2) depends on this being green.
     */
    {
        OSErr d1_result;
        char  d1_msg[200];

        OSTLS_LogLinef("Stage D1   async OT notifier smoke target=%s ...",
                       OSTLS_D1_TARGET);
        d1_result = OSTLS_D1_AsyncNotifier_Probe(OSTLS_D1_TARGET,
                                                 d1_msg, sizeof d1_msg);
        OSTLS_LogLinef("Stage D1   async OT notifier smoke    -> code=%d %s",
                       (int)d1_result, d1_msg);

        if (d1_result != kOSTLSD1_OK) {
            sprintf(title_buf, "MacTLSTest -- Stage D1 FAILED");
            sprintf(line1_buf, "Stage D1 FAILED (code %d): %.140s",
                (int)d1_result, d1_msg);
            sprintf(line2_buf, "Gate: %s (target=%s)",
                smoke_label(d1_result), OSTLS_D1_TARGET);
            OSTLS_LogBlank();
            OSTLS_LogLinef("==== Stage D1 FAILED (code=%d) ====",
                           (int)d1_result);
            show_result_and_wait(title_buf, line1_buf, line2_buf);
            ostls_ot_close();
            OSTLS_LogClose();
            return 0;
        }
    }

    /*
     * Stage D: exercise the macTLS v0.1 PUBLIC BLOCKING API.
     *
     * OSTLS_Fetch(host, port, server_name, path, out_buf, out_cap,
     *             out_len, out_msg, out_msg_len)
     *
     * was the first stable public entry point. Kept as the
     * blocking regression baseline so any breakage in the v0.2
     * async path stays distinguishable from a v0.1 regression.
     */
    {
        OSErr  d_result;
        char   d_msg[180];
        unsigned char d_response[96];
        UInt32 d_len = 0;

        OSTLS_LogLinef("Stage D    OSTLS_Fetch %s:%u%s ...",
                       OSTLS_D_HOST, (unsigned)OSTLS_D_PORT, OSTLS_D_PATH);
        d_result = OSTLS_Fetch(
            OSTLS_D_HOST,
            (UInt16)OSTLS_D_PORT,
            OSTLS_D_SERVERNAME,
            OSTLS_D_PATH,
            d_response, (UInt32)sizeof d_response,
            &d_len,
            d_msg, (UInt32)sizeof d_msg);
        OSTLS_LogLinef("Stage D    OSTLS_Fetch                -> code=%d %s",
                       (int)d_result, d_msg);

        if (d_result == kOSTLSFetch_OK) {
            char short_resp[60];
            UInt32 n_short;
            UInt32 i;

            /* Log the captured response bytes with escapes so the log
             * file shows the HTTP response intact. */
            {
                char dbg[320];
                size_t pos;
                UInt32 j;
                pos = (size_t)sprintf(dbg,
                    "Stage D    response prefix [%lu]   : ",
                    (unsigned long)d_len);
                for (j = 0; j < d_len && pos < sizeof dbg - 4; j++) {
                    unsigned char c = d_response[j];
                    if (c == 0x0D) {
                        dbg[pos++] = '\\';
                        dbg[pos++] = 'r';
                    } else if (c == 0x0A) {
                        dbg[pos++] = '\\';
                        dbg[pos++] = 'n';
                    } else if (c >= 0x20 && c < 0x7F) {
                        dbg[pos++] = (char)c;
                    } else {
                        static const char hex[] = "0123456789ABCDEF";
                        if (pos + 4 < sizeof dbg) {
                            dbg[pos++] = '\\';
                            dbg[pos++] = 'x';
                            dbg[pos++] = hex[(c >> 4) & 0xF];
                            dbg[pos++] = hex[c & 0xF];
                        }
                    }
                }
                dbg[pos] = '\0';
                OSTLS_LogLine(dbg);
            }

            /* Short ASCII version for the result-window line 2. */
            n_short = d_len;
            if (n_short > (UInt32)(sizeof short_resp - 4)) {
                n_short = (UInt32)(sizeof short_resp - 4);
            }
            for (i = 0; i < n_short; i++) {
                unsigned char c = d_response[i];
                if (c == '\r' || c == '\n' || c < 0x20 || c >= 0x7F) {
                    short_resp[i] = ' ';
                } else {
                    short_resp[i] = (char)c;
                }
            }
            short_resp[n_short] = '\0';

            /* Don't emit "ALL STAGES OK" yet -- Stage D2 still to
             * run. The line2 / title are placeholders that Stage D2
             * will overwrite on its own success path. */
            sprintf(title_buf, "MacTLSTest -- D OK (blocking lib)");
            sprintf(line1_buf, "OSTLS_Fetch %s", d_msg);
            sprintf(line2_buf, "Resp: %.140s", short_resp);
        } else {
            sprintf(title_buf, "MacTLSTest -- Stage D FAILED");
            sprintf(line1_buf, "OSTLS_Fetch FAILED (code %d): %.140s",
                (int)d_result, d_msg);
            sprintf(line2_buf,
                "Gate: %s (target=%s:%u)",
                smoke_label(d_result),
                OSTLS_D_HOST, (unsigned)OSTLS_D_PORT);
            OSTLS_LogBlank();
            OSTLS_LogLinef("==== Stage D FAILED (code=%d) ====",
                           (int)d_result);
            show_result_and_wait(title_buf, line1_buf, line2_buf);
            ostls_ot_close();
            OSTLS_LogClose();
            return 0;
        }
    }

    /*
     * Stage D2: exercise the macTLS v0.2 ASYNC PUBLIC API directly.
     *
     * Drives OSTLSConnection through its lifecycle:
     *   OSTLS_New + OSTLS_Start
     *   poll-Pump until handshake done
     *   OSTLS_Write the GET request
     *   poll-Pump + OSTLS_Read until peer closes or buffer fills
     *   OSTLS_Close + OSTLS_Dispose
     *
     * Bounded max_steps per Pump (we use 6) so each Pump call returns
     * within milliseconds; in a real UI app you'd interleave with
     * WaitNextEvent here.
     */
    {
        OSTLSConnection *conn;
        OSTLSConfig      cfg;
        OSTLSEvent       ev;
        OSTLSState       st;
        OSErr            d2_err;
        UInt32           written;
        UInt32           total_read;
        UInt32           chunk_count;
        UInt32           pump_count;
        UInt32           pump_deadline;
        char             d2_request[256];
        char             d2_msg[180];
        unsigned char    d2_chunk[64];
        Boolean          request_sent;
        Boolean          handshake_logged;
        Boolean          done;

        OSTLS_LogLinef("Stage D2   async open %s:%u%s ...",
                       OSTLS_D2_HOST, (unsigned)OSTLS_D2_PORT,
                       OSTLS_D2_PATH);

        memset(&cfg, 0, sizeof cfg);
        cfg.host = OSTLS_D2_HOST;
        cfg.port = (UInt16)OSTLS_D2_PORT;
        cfg.server_name = OSTLS_D2_SERVERNAME;
        cfg.connect_timeout_ticks = 0;    /* use default 30s */
        cfg.handshake_timeout_ticks = 0;
        cfg.user_refcon = NULL;

        conn = NULL;
        d2_err = OSTLS_New(&conn, &cfg);
        if (d2_err != kOSTLSAsync_OK || conn == NULL) {
            sprintf(d2_msg, "OSTLS_New FAIL code=%d", (int)d2_err);
            goto d2_failed;
        }

        d2_err = OSTLS_Start(conn);
        if (d2_err != kOSTLSAsync_OK) {
            sprintf(d2_msg, "OSTLS_Start FAIL code=%d", (int)d2_err);
            OSTLS_Dispose(conn);
            goto d2_failed;
        }

        /* Build the GET request. */
        sprintf(d2_request,
            "GET %s HTTP/1.0\r\n"
            "Host: %s\r\n"
            "User-Agent: macTLS/0.2\r\n"
            "Connection: close\r\n"
            "\r\n",
            OSTLS_D2_PATH, OSTLS_D2_SERVERNAME);

        request_sent = false;
        handshake_logged = false;
        done = false;
        total_read = 0;
        chunk_count = 0;
        pump_count = 0;
        pump_deadline = (UInt32)TickCount() + 60UL * 60UL;  /* 60s */

        while (!done) {
            if ((UInt32)TickCount() > pump_deadline) {
                OSTLSDiagnostics diag;
                OSTLS_GetDiagnostics(conn, &diag);
                OSTLS_LogLinef(
                    "D2 stall  pump=%lu st=%d br=0x%lX "
                    "snd_calls=%lu snd_bytes=%lu snd_zero=%lu snd_flow=%lu",
                    (unsigned long)pump_count,
                    (int)diag.state,
                    (unsigned long)diag.br_state_last,
                    (unsigned long)diag.ot_send_calls,
                    (unsigned long)diag.ot_send_bytes,
                    (unsigned long)diag.ot_send_zero,
                    (unsigned long)diag.ot_send_flow);
                OSTLS_LogLinef(
                    "D2 stall  rcv_calls=%lu rcv_bytes=%lu rcv_nodata=%lu",
                    (unsigned long)diag.ot_recv_calls,
                    (unsigned long)diag.ot_recv_bytes,
                    (unsigned long)diag.ot_recv_nodata);
                sprintf(d2_msg, "D2 deadline pump=%lu br=0x%lX rcv=%lu",
                    (unsigned long)pump_count,
                    (unsigned long)diag.br_state_last,
                    (unsigned long)diag.ot_recv_calls);
                OSTLS_Close(conn);
                OSTLS_Dispose(conn);
                d2_err = (OSErr)kOSTLSAsync_HandshakeTimeout;
                goto d2_failed;
            }

            d2_err = OSTLS_Pump(conn, 6, &ev);
            pump_count++;

            /* Periodic progress log (every ~10s at 60 Hz with 1-tick
             * yields below). Helps diagnose stalls without spamming. */
            if ((pump_count % 600UL) == 0UL) {
                OSTLSDiagnostics diag;
                OSTLS_GetDiagnostics(conn, &diag);
                OSTLS_LogLinef(
                    "D2 tick   pump=%lu st=%d br=0x%lX "
                    "snd=%lu/%luB rcv=%lu/%luB nodata=%lu",
                    (unsigned long)pump_count,
                    (int)diag.state,
                    (unsigned long)diag.br_state_last,
                    (unsigned long)diag.ot_send_calls,
                    (unsigned long)diag.ot_send_bytes,
                    (unsigned long)diag.ot_recv_calls,
                    (unsigned long)diag.ot_recv_bytes,
                    (unsigned long)diag.ot_recv_nodata);
            }

            /* Yield so Carbon's OT deferred tasks get CPU time.
             * Also collect entropy while we wait. */
            {
                EventRecord nullev;
                OSTLS_CollectEntropy();
                WaitNextEvent(everyEvent, &nullev, 1, NULL);
            }

            if (d2_err != kOSTLSAsync_OK) {
                sprintf(d2_msg, "OSTLS_Pump FAIL code=%d", (int)d2_err);
                OSTLS_Close(conn);
                OSTLS_Dispose(conn);
                goto d2_failed;
            }

            st = OSTLS_GetState(conn);
            if (st == kOSTLSStateFailed) {
                OSTLSDiagnostics diag;
                OSTLS_GetDiagnostics(conn, &diag);
                sprintf(d2_msg,
                    "D2 Failed os_err=%d ot=%ld br=%d",
                    (int)diag.os_err, (long)diag.ot_err,
                    diag.br_err);
                d2_err = diag.os_err;
                OSTLS_Dispose(conn);
                goto d2_failed;
            }

            if (!handshake_logged && st == kOSTLSStateOpen) {
                UInt16 suite = OSTLS_GetCipherSuite(conn);
                const char *label;
                switch (suite) {
                case 0x1301: label = "TLS1.3 AES128-GCM-SHA256";      break;
                case 0x1303: label = "TLS1.3 CHACHA20-POLY1305";      break;
                case 0xC02B: label = "TLS1.2 ECDHE-ECDSA AES128-GCM"; break;
                case 0xC02C: label = "TLS1.2 ECDHE-ECDSA AES256-GCM"; break;
                case 0xC02F: label = "TLS1.2 ECDHE-RSA AES128-GCM";   break;
                case 0xC030: label = "TLS1.2 ECDHE-RSA AES256-GCM";   break;
                case 0xCCA8: label = "TLS1.2 ECDHE-RSA CHACHA20";     break;
                case 0xCCA9: label = "TLS1.2 ECDHE-ECDSA CHACHA20";   break;
                default:     label = "TLS?";                          break;
                }
                OSTLS_LogLinef("Stage D2   handshake OK %s 0x%04X",
                               label, (unsigned)suite);
                handshake_logged = true;
            }

            /* Once Open, push the request once. */
            if (st == kOSTLSStateOpen && !request_sent) {
                size_t req_len = strlen(d2_request);
                written = 0;
                d2_err = OSTLS_Write(conn, d2_request,
                                     (UInt32)req_len, &written);
                if (d2_err != kOSTLSAsync_OK) {
                    sprintf(d2_msg, "OSTLS_Write FAIL code=%d",
                            (int)d2_err);
                    OSTLS_Close(conn);
                    OSTLS_Dispose(conn);
                    goto d2_failed;
                }
                if (written == (UInt32)req_len) {
                    OSTLS_LogLinef("Stage D2   wrote %lu request bytes",
                                   (unsigned long)written);
                    request_sent = true;
                }
            }

            /* Drain any decrypted bytes available. Multiple Reads
             * per Pump are legal -- drain until out_read==0. */
            if (st == kOSTLSStateOpen ||
                st == kOSTLSStateClosing ||
                st == kOSTLSStateClosed) {
                UInt32 nread;
                for (;;) {
                    nread = 0;
                    d2_err = OSTLS_Read(conn, d2_chunk,
                                        (UInt32)sizeof d2_chunk,
                                        &nread);
                    if (d2_err != kOSTLSAsync_OK || nread == 0) {
                        break;
                    }
                    chunk_count++;
                    total_read += nread;
                    if (chunk_count <= 10) {
                        OSTLS_LogLinef(
                            "Stage D2   read chunk %lu: %lu bytes",
                            (unsigned long)chunk_count,
                            (unsigned long)nread);
                    } else if (chunk_count == 11) {
                        OSTLS_LogLine(
                            "Stage D2   read chunks beyond 10 suppressed");
                    }
                }
            }

            if (ev == kOSTLSEventClosed || st == kOSTLSStateClosed) {
                done = true;
            }
        }

        OSTLS_LogLinef("Stage D2   read total: %lu bytes in %lu chunk(s); %lu Pump call(s)",
                       (unsigned long)total_read,
                       (unsigned long)chunk_count,
                       (unsigned long)pump_count);

        OSTLS_Close(conn);
        OSTLS_Dispose(conn);

        OSTLS_LogLine("Stage D2   closed OK");
        sprintf(d2_msg, "D2 OK %luB body in %lu chunks",
                (unsigned long)total_read,
                (unsigned long)chunk_count);

        sprintf(title_buf, "MacTLSTest -- A..D2 OK (async)");
        sprintf(line1_buf, "%.140s", d2_msg);
        sprintf(line2_buf,
                "D blocking OK; D2 async OK; Pump bounded.");
        OSTLS_LogBlank();
        OSTLS_LogLine("==== ALL STAGES OK ====");

        goto d2_done;

    d2_failed: {
        OSTLSDiagnostics diag;
        OSTLS_GetDiagnostics(conn, &diag);
        sprintf(title_buf, "MacTLSTest -- Stage D2 FAILED");
        sprintf(line1_buf, "OSTLS_Async FAILED (code %d, ot %ld, br %d)",
            (int)d2_err, (long)diag.ot_err, (int)diag.br_err);
        sprintf(line2_buf,
            "Gate: %s (target=%s:%u)",
            smoke_label(d2_err),
            OSTLS_D2_HOST, (unsigned)OSTLS_D2_PORT);
        OSTLS_LogBlank();
        OSTLS_LogLinef("==== Stage D2 FAILED (code=%d, ot=%ld, br=%d) ====",
                       (int)d2_err, (long)diag.ot_err, (int)diag.br_err);
    }
    d2_done: ;
    }

    /*
     * Stage D3: TLS session resumption. Connect to mactrove.com twice
     * and compare handshake bytes. The first handshake is full and pulls
     * the cert chain; the second should be abbreviated if the server does
     * session-ID resumption (nginx with ssl_session_cache on, served
     * directly rather than fronted by a CDN that prefers tickets). Watch
     * for the 'macTLS resume: cached session' / 'offering cached session'
     * lines in the log. BearSSL is session-ID only, no tickets, so big
     * CDNs (Google, Cloudflare) will decline; a cooperating origin server
     * is the right target to prove the abbreviated path.
     */
    {
        UInt32 r1, r2;
        OSTLS_LogLine("Stage D3   session resumption (2x mactrove.com) ...");
        /* Session-ID resumption is a TLS 1.2 feature in this stack (1.3
         * PSK/ticket resumption is out of scope). Force the 1.2 path so
         * this stage measures what it's meant to; restore 1.3 after. */
        OSTLS_SetTryTLS13(0);
        r1 = d3_handshake_recv("mactrove.com", 443, "mactrove.com");
        r2 = d3_handshake_recv("mactrove.com", 443, "mactrove.com");
        OSTLS_SetTryTLS13(1);
        OSTLS_LogLinef("Stage D3   connect 1 handshake recv=%lu bytes (full)",
                       (unsigned long)r1);
        OSTLS_LogLinef("Stage D3   connect 2 handshake recv=%lu bytes (resume?)",
                       (unsigned long)r2);
        if (r1 > 0 && r2 > 0 && r2 < (r1 / 2)) {
            OSTLS_LogLine("Stage D3   session resumption    -> PASS (2nd handshake abbreviated)");
        } else if (r1 > 0 && r2 > 0) {
            OSTLS_LogLine("Stage D3   session resumption    -> NO RESUME (server declined or full handshake)");
        } else {
            OSTLS_LogLine("Stage D3   session resumption    -> FAIL (connect error)");
        }
    }

    /*
     * Stage D4: TLS 1.3 session resumption (PSK / NewSessionTicket) over
     * the async path -- the macTLS#2 hardware gate. Two 1.3 connects to a
     * ticketing host. Connect 1 does a full handshake and, while reading
     * the response, harvests the server's NewSessionTicket into the cache
     * (look for 'T13 async: cached resumption ticket'). Connect 2 offers it;
     * on accept the handshake skips the certificate ('T13 async: resuming'
     * + 'COMPLETE') and pulls far fewer bytes. 68kmla tickets reliably.
     */
    {
        UInt32 r1, r2;
        OSTLS_LogLine("Stage D4   TLS 1.3 resumption (2x 68kmla.org) ...");
        r1 = d4_resume_recv("68kmla.org", 443, "68kmla.org");
        r2 = d4_resume_recv("68kmla.org", 443, "68kmla.org");
        OSTLS_LogLinef("Stage D4   connect 1 handshake recv=%lu bytes (full)",
                       (unsigned long)r1);
        OSTLS_LogLinef("Stage D4   connect 2 handshake recv=%lu bytes (resume?)",
                       (unsigned long)r2);
        if (r1 > 0 && r2 > 0 && r2 < (r1 / 2)) {
            OSTLS_LogLine("Stage D4   TLS 1.3 resumption  -> PASS (2nd handshake abbreviated, cert skipped)");
        } else if (r1 > 0 && r2 > 0) {
            OSTLS_LogLine("Stage D4   TLS 1.3 resumption  -> NO RESUME (see 'resuming' log line / server declined)");
        } else {
            OSTLS_LogLine("Stage D4   TLS 1.3 resumption  -> FAIL (connect error)");
        }
    }

    /*
     * Stage G: TLS 1.3 handshake over Open Transport. The on-device proof
     * that the ported handshake completes on real PPC against a live 1.3
     * server (mactrove serves TLSv1.2+1.3), validating the chain against
     * the embedded anchors. Same handshake driver verified on host; here
     * the bytes move over OT. Not wired into the fetch path yet -- that's
     * the Stage D async integration.
     */
    {
        OSErr  g_result;
        char   g_msg[180];
        UInt16 g_cipher = 0;
        OSTLS_LogLine("Stage G    TLS 1.3 handshake over OT (mactrove.com) ...");
        g_result = OSTLS_TLS13_OTProbe("mactrove.com:443", "mactrove.com",
                                       g_msg, sizeof g_msg, &g_cipher);
        OSTLS_LogLinef("Stage G    TLS 1.3 over OT          -> code=%d %s cipher=0x%04X",
                       (int)g_result, g_msg, (unsigned)g_cipher);
    }

    show_result_and_wait(title_buf, line1_buf, line2_buf);

    ostls_ot_close();
    OSTLS_LogClose();
    return 0;
}
