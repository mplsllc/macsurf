/*
 * ostls_fetch.c -- macTLS public library implementation. See
 * os9/ostls_fetch.h for the API contract.
 *
 * Extracted from the verified Stage B4 probe path
 * (formerly os9/ostls_b4_https_get.c). Same OT + BearSSL + embedded
 * anchors + clock-conversion machinery; new public signature.
 *
 * Flow:
 *   1. Build "host:port" string for OTInitDNSAddress (OT's DNS
 *      resolver takes one string, not separate host+port).
 *   2. Read clock; bail with kOSTLSFetch_ClockBefore2000 if the Mac
 *      hasn't been set past 2000-01-01 (PRAM-dead-battery guard).
 *   3. Open OT endpoint, bind NULL (qlen=0 outbound), OTConnect.
 *   4. Init BearSSL client with br_x509_minimal + 10 embedded roots.
 *      Inject Stage A entropy (insecure stub -- v1 limit, called out
 *      in docs/mactls-library-api.md).
 *   5. Reset against server_name (drives SNI + hostname match).
 *   6. Drive handshake + app-data send/recv via SENDREC/RECVREC /
 *      SENDAPP/RECVAPP against OTSnd/OTRcv until handshake completes,
 *      request bytes are sent, response is captured up to out_cap.
 *   7. Tear down cleanly. Return kOSTLSFetch_OK + out_msg + out_len.
 */

#include "ostls_fetch.h"
#include "ostls_b3_anchors.h"
#include "ostls_time.h"
#include "ostls_entropy.h"

#include "bearssl_ssl.h"
#include "bearssl_x509.h"

#include <stdio.h>
#include <string.h>

#if defined(__MWERKS__) || defined(__RETRO68__)  /* Mac target, either toolchain */
#include <Types.h>
#include <Events.h>             /* TickCount */
#include <Files.h>
#include <OpenTransport.h>
#include <OpenTptInternet.h>
extern OTClientContextPtr g_ostls_ot_context;
#else
/* Non-CW8 Retro68 syntax-check stubs. */
typedef long OSStatus;
typedef long OTResult;
typedef void *EndpointRef;
typedef void *OTConfigurationRef;
typedef short OTByteCount;
typedef unsigned char UInt8;
typedef struct { OTByteCount maxlen, len; UInt8 *buf; } TNetbuf;
typedef struct { TNetbuf addr, opt, udata; long sequence; } TCall;
typedef struct { unsigned short fAddressType; char fName[1]; } DNSAddress;
#define noErr 0
static unsigned long TickCount(void) { return 0; }
static OTConfigurationRef OTCreateConfiguration(const char *s){(void)s;return (OTConfigurationRef)1;}
static EndpointRef OTOpenEndpointInContext(OTConfigurationRef c,unsigned long f,void *p,OSStatus *e,void *x){(void)c;(void)f;(void)p;(void)x;*e=noErr;return (EndpointRef)1;}
static OSStatus OTSetSynchronous(EndpointRef e){(void)e;return noErr;}
static OSStatus OTSetBlocking(EndpointRef e){(void)e;return noErr;}
static OSStatus OTBind(EndpointRef e,void *a,void *b){(void)e;(void)a;(void)b;return noErr;}
static OSStatus OTConnect(EndpointRef e,TCall *c,void *r){(void)e;(void)c;(void)r;return noErr;}
static OTResult OTSnd(EndpointRef e,void *b,long n,long f){(void)e;(void)b;(void)f;return n;}
static OTResult OTRcv(EndpointRef e,void *b,long n,long *f){(void)e;(void)b;(void)f;return n;}
static OSStatus OTSndOrderlyDisconnect(EndpointRef e){(void)e;return noErr;}
static OSStatus OTCloseProvider(EndpointRef e){(void)e;return noErr;}
static long OTInitDNSAddress(DNSAddress *d,const char *s){(void)d;(void)s;return 0;}
static void OTMemzero(void *p,unsigned long n){memset(p,0,n);}
extern void *g_ostls_ot_context;
#endif


/* ----------------------------------------------------------------- */
/* Static BearSSL state                                              */
/* ----------------------------------------------------------------- */

/*
 * One context per call; serialised by virtue of being synchronous.
 * v2 may move these onto the caller's stack or heap when reentrancy
 * matters (e.g. for streaming or parallel fetches).
 */
static br_ssl_client_context        gFetchClient;
static br_x509_minimal_context      gFetchX509;
static unsigned char                gFetchIoBuf[BR_SSL_BUFSIZE_BIDI];


/* ----------------------------------------------------------------- */
/* Helpers                                                           */
/* ----------------------------------------------------------------- */

static void
fetch_status(char *out, UInt32 out_len, const char *prefix, long err)
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


static const char *
fetch_suite_label(unsigned suite)
{
    switch (suite) {
    case 0xC02B: return "TLS1.2 ECDHE-ECDSA AES128-GCM";
    case 0xC02C: return "TLS1.2 ECDHE-ECDSA AES256-GCM";
    case 0xC02F: return "TLS1.2 ECDHE-RSA AES128-GCM";
    case 0xC030: return "TLS1.2 ECDHE-RSA AES256-GCM";
    case 0xCCA8: return "TLS1.2 ECDHE-RSA CHACHA20";
    case 0xCCA9: return "TLS1.2 ECDHE-ECDSA CHACHA20";
    default:     return "TLS1.2";
    }
}


/* ----------------------------------------------------------------- */
/* OSTLS_Fetch                                                       */
/* ----------------------------------------------------------------- */

OSErr
OSTLS_Fetch(const char *host,
            UInt16      port,
            const char *server_name,
            const char *path,
            void       *out_buf,
            UInt32      out_cap,
            UInt32     *out_len,
            char       *out_msg,
            UInt32      out_msg_len)
{
    OTConfigurationRef cfg;
    EndpointRef ep;
    OSStatus oterr;
    DNSAddress dns;
    TCall call;
    long dns_len;

    char target_host_port[128];
    char request_line[256];

    const br_x509_trust_anchor *anchors;
    size_t anchors_count;

    UInt32 br_days;
    UInt32 br_seconds;
    OSErr time_err;

    size_t request_total;
    size_t request_sent;

    unsigned ssl_state;
    int ssl_err;
    int reset_ok;
    int entropy_err;

    unsigned long deadline_ticks;
    unsigned long now_ticks;
    int handshake_done;
    UInt32 response_collected;
    unsigned char *out_bytes;

    /* Initialise the output pointers up front so error paths leave
     * the caller's slots in a defined state. */
    if (out_len != NULL) {
        *out_len = 0;
    }

    if (host == NULL || server_name == NULL || path == NULL ||
        out_buf == NULL || out_cap < 1 || port == 0) {
        fetch_status(out_msg, out_msg_len, "OSTLS_Fetch: bad args", 0);
        return (OSErr)kOSTLSFetch_BadArgs;
    }

    out_bytes = (unsigned char *)out_buf;
    response_collected = 0;

    /* Build the host:port string for OTInitDNSAddress. The whole
     * point of the new public signature is splitting host + port,
     * but OT's resolver internally takes a single combined string,
     * so we recombine here. */
    sprintf(target_host_port, "%.80s:%u",
            host, (unsigned)port);
    target_host_port[sizeof target_host_port - 1] = '\0';

    /* Build the GET request. 256 bytes fits the line + a couple of
     * headers + a comfortable host/path length budget. */
    sprintf(request_line,
        "GET %.80s HTTP/1.0\r\n"
        "Host: %.80s\r\n"
        "User-Agent: macTLS/0.1\r\n"
        "Connection: close\r\n"
        "\r\n",
        path, server_name);
    request_line[sizeof request_line - 1] = '\0';
    request_total = strlen(request_line);

    /* ----- 1. Clock ----- */

    time_err = OSTLS_GetBearSSLTime(&br_days, &br_seconds);
    if (time_err == kOSTLSTimeClockBefore2000) {
        fetch_status(out_msg, out_msg_len,
            "OSTLS_Fetch: system clock before 2000", 0);
        return (OSErr)kOSTLSFetch_ClockBefore2000;
    }
    if (time_err != kOSTLSTimeOK) {
        fetch_status(out_msg, out_msg_len,
            "OSTLS_Fetch: time helper failure", (long)time_err);
        return (OSErr)kOSTLSFetch_ClockBefore2000;
    }

    /* ----- 2. OT endpoint open + outbound connect ----- */

    cfg = OTCreateConfiguration("tcp");
    if (cfg == NULL || cfg == (OTConfigurationRef)-1L) {
        fetch_status(out_msg, out_msg_len,
            "OSTLS_Fetch: OTCreateConfiguration FAIL", 0);
        return (OSErr)kOSTLSFetch_OTConfigFail;
    }

    oterr = noErr;
    ep = OTOpenEndpointInContext(cfg, 0, NULL, &oterr, g_ostls_ot_context);
    if (oterr != noErr || ep == NULL) {
        fetch_status(out_msg, out_msg_len,
            "OSTLS_Fetch: OTOpenEndpoint FAIL", (long)oterr);
        return (OSErr)kOSTLSFetch_OTOpenEndptFail;
    }
    OTSetSynchronous(ep);
    OTSetBlocking(ep);

    oterr = OTBind(ep, NULL, NULL);
    if (oterr != noErr) {
        fetch_status(out_msg, out_msg_len,
            "OSTLS_Fetch: OTBind FAIL", (long)oterr);
        OTCloseProvider(ep);
        return (OSErr)kOSTLSFetch_OTBindFail;
    }

    OTMemzero(&call, sizeof(call));
    dns_len = OTInitDNSAddress(&dns, (char *)target_host_port);
    if (dns_len <= 0) {
        fetch_status(out_msg, out_msg_len,
            "OSTLS_Fetch: OTInitDNSAddress FAIL", dns_len);
        OTCloseProvider(ep);
        return (OSErr)kOSTLSFetch_OTDnsAddrFail;
    }
    call.addr.buf = (UInt8 *)&dns;
    call.addr.len = (short)dns_len;

    oterr = OTConnect(ep, &call, NULL);
    if (oterr != noErr) {
        fetch_status(out_msg, out_msg_len,
            "OSTLS_Fetch: OTConnect FAIL", (long)oterr);
        OTCloseProvider(ep);
        return (OSErr)kOSTLSFetch_OTConnectFail;
    }

    /* ----- 3. BearSSL with embedded anchors + real time ----- */

    OSTLS_B3_GetAnchors(&anchors, &anchors_count);
    br_ssl_client_init_full(&gFetchClient, &gFetchX509,
                            anchors, anchors_count);
    br_x509_minimal_set_time(&gFetchX509, br_days, br_seconds);

    entropy_err = OSTLS_InjectEntropy(&gFetchClient.eng);
    if (entropy_err != 0) {
        fetch_status(out_msg, out_msg_len,
            "OSTLS_Fetch: entropy inject FAIL", 0);
        OTSndOrderlyDisconnect(ep);
        OTCloseProvider(ep);
        return (OSErr)kOSTLSFetch_EntropyFail;
    }

    br_ssl_engine_set_buffer(&gFetchClient.eng,
                             gFetchIoBuf, sizeof gFetchIoBuf, 1);

    reset_ok = br_ssl_client_reset(&gFetchClient, server_name, 0);
    if (reset_ok == 0) {
        fetch_status(out_msg, out_msg_len,
            "OSTLS_Fetch: br_ssl_client_reset returned 0",
            (long)br_ssl_engine_last_error(&gFetchClient.eng));
        OTSndOrderlyDisconnect(ep);
        OTCloseProvider(ep);
        return (OSErr)kOSTLSFetch_ClientResetFail;
    }

    /* ----- 4. Drive handshake + GET + response capture ----- */

    handshake_done = 0;
    request_sent = 0;
    deadline_ticks = TickCount() + (unsigned long)(60UL * 60UL);

    for (;;) {
        now_ticks = TickCount();
        if (now_ticks > deadline_ticks) {
            fetch_status(out_msg, out_msg_len,
                "OSTLS_Fetch: handshake/IO timeout (60 s)", 0);
            OTSndOrderlyDisconnect(ep);
            OTCloseProvider(ep);
            return (OSErr)kOSTLSFetch_HandshakeTimeout;
        }

        ssl_state = br_ssl_engine_current_state(&gFetchClient.eng);

        if ((ssl_state & BR_SSL_CLOSED) != 0) {
            ssl_err = br_ssl_engine_last_error(&gFetchClient.eng);
            if (ssl_err == BR_ERR_OK) {
                if (response_collected > 0) {
                    break;
                }
                fetch_status(out_msg, out_msg_len,
                    "OSTLS_Fetch: peer closed before any plaintext", 0);
                OTCloseProvider(ep);
                return (OSErr)kOSTLSFetch_NoBytesReceived;
            }
            fetch_status(out_msg, out_msg_len,
                "OSTLS_Fetch: BearSSL error during I/O",
                (long)ssl_err);
            OTSndOrderlyDisconnect(ep);
            OTCloseProvider(ep);
            return (OSErr)kOSTLSFetch_BearSSLError;
        }

        if (!handshake_done &&
            (ssl_state & (BR_SSL_SENDAPP | BR_SSL_RECVAPP)) != 0) {
            handshake_done = 1;
        }

        /* Push the GET request body once handshake is up. */
        if (handshake_done &&
            request_sent < request_total &&
            (ssl_state & BR_SSL_SENDAPP) != 0) {
            unsigned char *abuf;
            size_t alen;
            size_t to_copy;

            abuf = br_ssl_engine_sendapp_buf(&gFetchClient.eng, &alen);
            if (alen == 0) {
                continue;
            }
            to_copy = request_total - request_sent;
            if (to_copy > alen) {
                to_copy = alen;
            }
            memcpy(abuf, request_line + request_sent, to_copy);
            br_ssl_engine_sendapp_ack(&gFetchClient.eng, to_copy);
            request_sent += to_copy;
            if (request_sent >= request_total) {
                br_ssl_engine_flush(&gFetchClient.eng, 0);
            }
            continue;
        }

        /* Drain decrypted response into out_buf. */
        if (handshake_done &&
            (ssl_state & BR_SSL_RECVAPP) != 0 &&
            response_collected < out_cap) {
            unsigned char *abuf;
            size_t alen;
            UInt32 room;
            size_t to_copy;

            abuf = br_ssl_engine_recvapp_buf(&gFetchClient.eng, &alen);
            if (alen == 0) {
                continue;
            }
            room = out_cap - response_collected;
            to_copy = alen;
            if ((UInt32)to_copy > room) {
                to_copy = (size_t)room;
            }
            memcpy(out_bytes + response_collected, abuf, to_copy);
            response_collected += (UInt32)to_copy;
            br_ssl_engine_recvapp_ack(&gFetchClient.eng, to_copy);

            if (response_collected >= out_cap) {
                break;
            }
            continue;
        }

        if ((ssl_state & BR_SSL_SENDREC) != 0) {
            unsigned char *sbuf;
            size_t slen;
            OTResult sent;

            sbuf = br_ssl_engine_sendrec_buf(&gFetchClient.eng, &slen);
            if (slen == 0) {
                continue;
            }
            sent = OTSnd(ep, sbuf, (long)slen, 0);
            if (sent < 0) {
                fetch_status(out_msg, out_msg_len,
                    "OSTLS_Fetch: OTSnd FAIL", (long)sent);
                OTSndOrderlyDisconnect(ep);
                OTCloseProvider(ep);
                return (OSErr)kOSTLSFetch_OTSndFail;
            }
            br_ssl_engine_sendrec_ack(&gFetchClient.eng, (size_t)sent);
            continue;
        }

        if ((ssl_state & BR_SSL_RECVREC) != 0) {
            unsigned char *rbuf;
            size_t rlen;
            OTResult got;

            rbuf = br_ssl_engine_recvrec_buf(&gFetchClient.eng, &rlen);
            if (rlen == 0) {
                continue;
            }
            got = OTRcv(ep, rbuf, (long)rlen, NULL);
            if (got < 0) {
                fetch_status(out_msg, out_msg_len,
                    "OSTLS_Fetch: OTRcv FAIL", (long)got);
                OTSndOrderlyDisconnect(ep);
                OTCloseProvider(ep);
                return (OSErr)kOSTLSFetch_OTRcvFail;
            }
            if (got == 0) {
                br_ssl_engine_recvrec_ack(&gFetchClient.eng, 0);
                continue;
            }
            br_ssl_engine_recvrec_ack(&gFetchClient.eng, (size_t)got);
            continue;
        }
    }

    /* ----- 5. Success ----- */

    if (out_len != NULL) {
        *out_len = response_collected;
    }

    {
        const br_ssl_session_parameters *sess;
        unsigned suite;
        sess = &gFetchClient.eng.session;
        suite = (unsigned)(sess->cipher_suite & 0xFFFFU);
        sprintf(out_msg, "OK %s %luB body",
                fetch_suite_label(suite),
                (unsigned long)response_collected);
        out_msg[out_msg_len - 1] = '\0';
    }

    OTSndOrderlyDisconnect(ep);
    OTCloseProvider(ep);
    return (OSErr)kOSTLSFetch_OK;
}
