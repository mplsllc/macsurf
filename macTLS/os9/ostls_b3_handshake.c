/*
 * ostls_b3_handshake.c -- Stage B3 validated handshake. See header.
 *
 * Identical OT setup and record I/O loop to B2; the only real
 * difference is the X.509 validator. B3 uses BearSSL's
 * br_x509_minimal_context populated from the embedded anchors and
 * given the current Mac time, so chain validation, expiration, and
 * hostname matching all run for real.
 *
 * Error decoding: we read br_ssl_engine_last_error after the engine
 * closes; if it's BR_ERR_X509_* we map it to the targeted B3 code,
 * otherwise to kOSTLSB3_BearSSLError with the raw code in out_msg.
 */

#include "ostls_b3_handshake.h"
#include "ostls_b3_anchors.h"
#include "ostls_time.h"
#include "ostls_entropy.h"

#include "bearssl_ssl.h"
#include "bearssl_x509.h"

#include <stdio.h>
#include <string.h>

#ifdef __MWERKS__
#include <Types.h>
#include <Events.h>             /* TickCount */
#include <Files.h>
#include <OpenTransport.h>
#include <OpenTptInternet.h>
extern OTClientContextPtr g_ostls_ot_context;
#else
/* Non-CW8 Retro68 syntax-check stubs. None of this runs under Linux;
 * the file is C89-clean at type level so pre-flight catches drift. */
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

static br_ssl_client_context        gB3Client;
static br_x509_minimal_context      gB3X509;
static unsigned char                gB3IoBuf[BR_SSL_BUFSIZE_BIDI];


/* ----------------------------------------------------------------- */
/* Helpers                                                           */
/* ----------------------------------------------------------------- */

static void
b3_status(char *out, size_t out_len, const char *prefix, long err)
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


/*
 * Map a BearSSL error code to a targeted B3 result code. Falls back
 * to kOSTLSB3_BearSSLError for non-X.509 errors and kOSTLSB3_X509Other
 * for any X.509 error that doesn't have a dedicated bucket.
 */
static OSErr
b3_classify_bearssl_error(int ssl_err)
{
    switch (ssl_err) {
    case BR_ERR_X509_NOT_TRUSTED:        return (OSErr)kOSTLSB3_X509NotTrusted;
    case BR_ERR_X509_EXPIRED:            return (OSErr)kOSTLSB3_X509Expired;
    case BR_ERR_X509_BAD_SERVER_NAME:    return (OSErr)kOSTLSB3_X509HostnameMismatch;
    case BR_ERR_X509_BAD_SIGNATURE:      return (OSErr)kOSTLSB3_X509BadSignature;
    case BR_ERR_X509_TIME_UNKNOWN:       return (OSErr)kOSTLSB3_X509TimeUnknown;
    default:
        /* BR_ERR_X509_* are codes 32..63 per bearssl_x509.h's enum
         * grouping convention. Anything in that range is an X.509
         * failure we don't have a dedicated bucket for. */
        if (ssl_err >= 32 && ssl_err <= 63) {
            return (OSErr)kOSTLSB3_X509Other;
        }
        return (OSErr)kOSTLSB3_BearSSLError;
    }
}


/* ----------------------------------------------------------------- */
/* Probe                                                             */
/* ----------------------------------------------------------------- */

OSErr
OSTLS_B3_Validated_Probe(const char *target_host_port,
                         const char *server_name,
                         char *out_msg, size_t out_msg_len)
{
    OTConfigurationRef cfg;
    EndpointRef ep;
    OSStatus oterr;
    DNSAddress dns;
    TCall call;
    long dns_len;

    const br_x509_trust_anchor *anchors;
    size_t anchors_count;

    UInt32 br_days;
    UInt32 br_seconds;
    OSErr time_err;

    unsigned ssl_state;
    int ssl_err;
    int reset_ok;
    int entropy_err;

    unsigned long deadline_ticks;
    unsigned long now_ticks;
    int handshake_done;

    if (target_host_port == NULL || server_name == NULL
        || target_host_port[0] == '\0' || server_name[0] == '\0') {
        b3_status(out_msg, out_msg_len, "B3: bad args", 0);
        return (OSErr)kOSTLSB3_BadArgs;
    }

    /* ----- 0. Read clock first; bail loudly on PRAM-dead Macs ----- */

    time_err = OSTLS_GetBearSSLTime(&br_days, &br_seconds);
    if (time_err == kOSTLSTimeClockBefore2000) {
        b3_status(out_msg, out_msg_len,
            "B3: system clock before 2000 -- set the date in Date & Time",
            0);
        return (OSErr)kOSTLSB3_ClockBefore2000;
    }
    if (time_err != kOSTLSTimeOK) {
        b3_status(out_msg, out_msg_len,
            "B3: time helper failure", (long)time_err);
        return (OSErr)kOSTLSB3_ClockBefore2000;
    }

    /* ----- 1. OT endpoint setup (mirror B1) ----- */

    cfg = OTCreateConfiguration("tcp");
    if (cfg == NULL || cfg == (OTConfigurationRef)-1L) {
        b3_status(out_msg, out_msg_len,
            "B3: OTCreateConfiguration FAIL", 0);
        return (OSErr)kOSTLSB3_OTConfigFail;
    }

    oterr = noErr;
    ep = OTOpenEndpointInContext(cfg, 0, NULL, &oterr, g_ostls_ot_context);
    if (oterr != noErr || ep == NULL) {
        b3_status(out_msg, out_msg_len,
            "B3: OTOpenEndpoint FAIL", (long)oterr);
        return (OSErr)kOSTLSB3_OTOpenEndptFail;
    }
    OTSetSynchronous(ep);
    OTSetBlocking(ep);

    oterr = OTBind(ep, NULL, NULL);
    if (oterr != noErr) {
        b3_status(out_msg, out_msg_len, "B3: OTBind FAIL", (long)oterr);
        OTCloseProvider(ep);
        return (OSErr)kOSTLSB3_OTBindFail;
    }

    OTMemzero(&call, sizeof(call));
    dns_len = OTInitDNSAddress(&dns, (char *)target_host_port);
    if (dns_len <= 0) {
        b3_status(out_msg, out_msg_len,
            "B3: OTInitDNSAddress FAIL", dns_len);
        OTCloseProvider(ep);
        return (OSErr)kOSTLSB3_OTDnsAddrFail;
    }
    call.addr.buf = (UInt8 *)&dns;
    call.addr.len = (short)dns_len;

    oterr = OTConnect(ep, &call, NULL);
    if (oterr != noErr) {
        b3_status(out_msg, out_msg_len, "B3: OTConnect FAIL", (long)oterr);
        OTCloseProvider(ep);
        return (OSErr)kOSTLSB3_OTConnectFail;
    }

    /* ----- 2. BearSSL setup with REAL trust anchors ----- */

    OSTLS_B3_GetAnchors(&anchors, &anchors_count);
    br_ssl_client_init_full(&gB3Client, &gB3X509, anchors, anchors_count);
    br_x509_minimal_set_time(&gB3X509, br_days, br_seconds);

    entropy_err = OSTLS_InjectEntropy(&gB3Client.eng);
    if (entropy_err != 0) {
        b3_status(out_msg, out_msg_len, "B3: entropy inject FAIL", 0);
        OTSndOrderlyDisconnect(ep);
        OTCloseProvider(ep);
        return (OSErr)kOSTLSB3_EntropyFail;
    }

    br_ssl_engine_set_buffer(&gB3Client.eng, gB3IoBuf, sizeof gB3IoBuf, 1);

    reset_ok = br_ssl_client_reset(&gB3Client, server_name, 0);
    if (reset_ok == 0) {
        b3_status(out_msg, out_msg_len,
            "B3: br_ssl_client_reset returned 0",
            (long)br_ssl_engine_last_error(&gB3Client.eng));
        OTSndOrderlyDisconnect(ep);
        OTCloseProvider(ep);
        return (OSErr)kOSTLSB3_ClientResetFail;
    }

    /* ----- 3. Drive the handshake (same loop as B2) ----- */

    deadline_ticks = TickCount() + (unsigned long)(60UL * 60UL);
    handshake_done = 0;

    while (!handshake_done) {
        now_ticks = TickCount();
        if (now_ticks > deadline_ticks) {
            b3_status(out_msg, out_msg_len,
                "B3: handshake timeout (60 s)", 0);
            OTSndOrderlyDisconnect(ep);
            OTCloseProvider(ep);
            return (OSErr)kOSTLSB3_HandshakeTimeout;
        }

        ssl_state = br_ssl_engine_current_state(&gB3Client.eng);

        if ((ssl_state & BR_SSL_CLOSED) != 0) {
            ssl_err = br_ssl_engine_last_error(&gB3Client.eng);
            if (ssl_err == BR_ERR_OK) {
                b3_status(out_msg, out_msg_len,
                    "B3: engine CLOSED before handshake", 0);
                OTSndOrderlyDisconnect(ep);
                OTCloseProvider(ep);
                return (OSErr)kOSTLSB3_PeerClosedEarly;
            }
            /* Map BearSSL's error to a targeted B3 result code so the
             * result window can show which validation gate failed. */
            b3_status(out_msg, out_msg_len,
                "B3: TLS validation FAIL", (long)ssl_err);
            OTSndOrderlyDisconnect(ep);
            OTCloseProvider(ep);
            return b3_classify_bearssl_error(ssl_err);
        }

        if ((ssl_state & (BR_SSL_SENDAPP | BR_SSL_RECVAPP)) != 0) {
            handshake_done = 1;
            break;
        }

        if ((ssl_state & BR_SSL_SENDREC) != 0) {
            unsigned char *sbuf;
            size_t slen;
            OTResult sent;

            sbuf = br_ssl_engine_sendrec_buf(&gB3Client.eng, &slen);
            if (slen == 0) {
                continue;
            }
            sent = OTSnd(ep, sbuf, (long)slen, 0);
            if (sent < 0) {
                b3_status(out_msg, out_msg_len,
                    "B3: OTSnd FAIL", (long)sent);
                OTSndOrderlyDisconnect(ep);
                OTCloseProvider(ep);
                return (OSErr)kOSTLSB3_OTSndFail;
            }
            br_ssl_engine_sendrec_ack(&gB3Client.eng, (size_t)sent);
            continue;
        }

        if ((ssl_state & BR_SSL_RECVREC) != 0) {
            unsigned char *rbuf;
            size_t rlen;
            OTResult got;

            rbuf = br_ssl_engine_recvrec_buf(&gB3Client.eng, &rlen);
            if (rlen == 0) {
                continue;
            }
            got = OTRcv(ep, rbuf, (long)rlen, NULL);
            if (got < 0) {
                b3_status(out_msg, out_msg_len,
                    "B3: OTRcv FAIL", (long)got);
                OTSndOrderlyDisconnect(ep);
                OTCloseProvider(ep);
                return (OSErr)kOSTLSB3_OTRcvFail;
            }
            if (got == 0) {
                b3_status(out_msg, out_msg_len,
                    "B3: peer closed before handshake", 0);
                OTSndOrderlyDisconnect(ep);
                OTCloseProvider(ep);
                return (OSErr)kOSTLSB3_PeerClosedEarly;
            }
            br_ssl_engine_recvrec_ack(&gB3Client.eng, (size_t)got);
            continue;
        }
    }

    /* Handshake succeeded WITH validation. Report the negotiated
     * cipher suite in the compact B2 format. */
    {
        const br_ssl_session_parameters *sess;
        unsigned suite;
        const char *label;

        sess = &gB3Client.eng.session;
        suite = (unsigned)(sess->cipher_suite & 0xFFFFU);
        switch (suite) {
        case 0xC02B: label = "TLS1.2 ECDHE-ECDSA AES128-GCM"; break;
        case 0xC02C: label = "TLS1.2 ECDHE-ECDSA AES256-GCM"; break;
        case 0xC02F: label = "TLS1.2 ECDHE-RSA AES128-GCM";   break;
        case 0xC030: label = "TLS1.2 ECDHE-RSA AES256-GCM";   break;
        case 0xCCA8: label = "TLS1.2 ECDHE-RSA CHACHA20";     break;
        case 0xCCA9: label = "TLS1.2 ECDHE-ECDSA CHACHA20";   break;
        default:     label = "TLS1.2";                        break;
        }
        sprintf(out_msg, "B3 OK %s 0x%04X", label, suite);
        out_msg[out_msg_len - 1] = '\0';
    }

    OTSndOrderlyDisconnect(ep);
    OTCloseProvider(ep);
    return (OSErr)kOSTLSB3_OK;
}
