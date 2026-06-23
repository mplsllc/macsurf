/*
 * ostls_b4_https_get.c -- Stage B4 tiny HTTPS GET. See header.
 *
 * Mirrors Stage B3's OT setup, BearSSL init (br_x509_minimal with
 * embedded anchors), entropy injection, time set, and reset. The new
 * work is everything after handshake completion:
 *
 *   - SENDAPP : copy GET request bytes into the engine's app buffer,
 *               ack what was copied, br_ssl_engine_flush(0)
 *   - SENDREC : drain the encrypted record onto OT via OTSnd, ack
 *   - RECVREC : pull encrypted bytes from OT via OTRcv into the
 *               engine's recv buffer, ack
 *   - RECVAPP : copy plaintext from the engine into the caller's
 *               result prefix until full or peer closes, ack
 *
 * Loop terminates when:
 *   - we have collected out_prefix_cap-1 bytes of plaintext, OR
 *   - the engine reaches BR_SSL_CLOSED (peer closed; clean if
 *     last_error == BR_ERR_OK), OR
 *   - the per-probe deadline fires.
 *
 * No HTTP parsing. The captured bytes are opaque -- they happen to
 * start with "HTTP/" because we asked for it.
 */

#include "ostls_b4_https_get.h"
#include "ostls_b3_anchors.h"
#include "ostls_time.h"
#include "ostls_entropy.h"
#include "ostls_log.h"

#include "bearssl_ssl.h"
#include "bearssl_x509.h"

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

static br_ssl_client_context        gB4Client;
static br_x509_minimal_context      gB4X509;
static unsigned char                gB4IoBuf[BR_SSL_BUFSIZE_BIDI];


/* ----------------------------------------------------------------- */
/* Helpers                                                           */
/* ----------------------------------------------------------------- */

static void
b4_status(char *out, size_t out_len, const char *prefix, long err)
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
 * Render the captured response prefix into the log with control
 * characters made visible -- CR shows as \r, LF as \n. The plain
 * write goes into a single line in the log file.
 */
static void
log_response_prefix(const unsigned char *bytes, size_t len)
{
    char buf[512];
    size_t pos = 0;
    size_t i;

    if (bytes == NULL || len == 0) {
        OSTLS_LogLine("Stage B4   response prefix          : (empty)");
        return;
    }

    pos += sprintf(buf + pos, "Stage B4   response prefix [%lu]   : ",
                   (unsigned long)len);

    for (i = 0; i < len && pos < sizeof buf - 4; i++) {
        unsigned char c = bytes[i];
        if (c == 0x0D) {
            buf[pos++] = '\\';
            buf[pos++] = 'r';
        } else if (c == 0x0A) {
            buf[pos++] = '\\';
            buf[pos++] = 'n';
        } else if (c == 0x09) {
            buf[pos++] = '\\';
            buf[pos++] = 't';
        } else if (c >= 0x20 && c < 0x7F) {
            buf[pos++] = (char)c;
        } else {
            /* Non-ASCII / control byte -- show as \xNN */
            if (pos + 4 < sizeof buf) {
                static const char hex[] = "0123456789ABCDEF";
                buf[pos++] = '\\';
                buf[pos++] = 'x';
                buf[pos++] = hex[(c >> 4) & 0xF];
                buf[pos++] = hex[c & 0xF];
            }
        }
    }
    buf[pos] = '\0';
    OSTLS_LogLine(buf);
}


static OSErr
b4_classify_bearssl_error(int ssl_err)
{
    (void)ssl_err;
    return (OSErr)kOSTLSB4_BearSSLError;
}


/* ----------------------------------------------------------------- */
/* Probe                                                             */
/* ----------------------------------------------------------------- */

OSErr
OSTLS_B4_HTTPS_Get_Probe(const char *target_host_port,
                         const char *server_name,
                         const char *request_path,
                         char *out_prefix, size_t out_prefix_cap,
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

    char request_line[256];
    size_t request_total;
    size_t request_sent;

    unsigned ssl_state;
    int ssl_err;
    int reset_ok;
    int entropy_err;

    unsigned long deadline_ticks;
    unsigned long now_ticks;
    int handshake_done;
    size_t response_collected;

    if (target_host_port == NULL || server_name == NULL
        || request_path == NULL
        || out_prefix == NULL || out_prefix_cap < 2) {
        b4_status(out_msg, out_msg_len, "B4: bad args", 0);
        return (OSErr)kOSTLSB4_BadArgs;
    }

    out_prefix[0] = '\0';
    response_collected = 0;

    /* ----- 0. Build the GET request first; bail early if it won't fit ----- */

    /*
     * The full request must fit in br_ssl_engine_sendapp_buf, which
     * returns whatever room is currently available in the engine's
     * output buffer -- BR_SSL_BUFSIZE_BIDI's app slice is at least
     * 16KB. 256 bytes is plenty for "GET / HTTP/1.0\r\nHost: ...".
     */
    sprintf(request_line,
        "GET %.80s HTTP/1.0\r\n"
        "Host: %.80s\r\n"
        "User-Agent: MacTLSTest/0.1\r\n"
        "Connection: close\r\n"
        "\r\n",
        request_path, server_name);
    request_line[sizeof request_line - 1] = '\0';
    request_total = strlen(request_line);

    /* ----- 1. Clock ----- */

    time_err = OSTLS_GetBearSSLTime(&br_days, &br_seconds);
    if (time_err == kOSTLSTimeClockBefore2000) {
        b4_status(out_msg, out_msg_len,
            "B4: system clock before 2000", 0);
        return (OSErr)kOSTLSB4_ClockBefore2000;
    }
    if (time_err != kOSTLSTimeOK) {
        b4_status(out_msg, out_msg_len,
            "B4: time helper failure", (long)time_err);
        return (OSErr)kOSTLSB4_ClockBefore2000;
    }

    /* ----- 2. OT endpoint setup (mirror B1/B2/B3) ----- */

    cfg = OTCreateConfiguration("tcp");
    if (cfg == NULL || cfg == (OTConfigurationRef)-1L) {
        b4_status(out_msg, out_msg_len,
            "B4: OTCreateConfiguration FAIL", 0);
        return (OSErr)kOSTLSB4_OTConfigFail;
    }

    oterr = noErr;
    ep = OTOpenEndpointInContext(cfg, 0, NULL, &oterr, g_ostls_ot_context);
    if (oterr != noErr || ep == NULL) {
        b4_status(out_msg, out_msg_len,
            "B4: OTOpenEndpoint FAIL", (long)oterr);
        return (OSErr)kOSTLSB4_OTOpenEndptFail;
    }
    OTSetSynchronous(ep);
    OTSetBlocking(ep);

    oterr = OTBind(ep, NULL, NULL);
    if (oterr != noErr) {
        b4_status(out_msg, out_msg_len, "B4: OTBind FAIL", (long)oterr);
        OTCloseProvider(ep);
        return (OSErr)kOSTLSB4_OTBindFail;
    }

    OTMemzero(&call, sizeof(call));
    dns_len = OTInitDNSAddress(&dns, (char *)target_host_port);
    if (dns_len <= 0) {
        b4_status(out_msg, out_msg_len,
            "B4: OTInitDNSAddress FAIL", dns_len);
        OTCloseProvider(ep);
        return (OSErr)kOSTLSB4_OTDnsAddrFail;
    }
    call.addr.buf = (UInt8 *)&dns;
    call.addr.len = (short)dns_len;

    oterr = OTConnect(ep, &call, NULL);
    if (oterr != noErr) {
        b4_status(out_msg, out_msg_len, "B4: OTConnect FAIL", (long)oterr);
        OTCloseProvider(ep);
        return (OSErr)kOSTLSB4_OTConnectFail;
    }

    /* ----- 3. BearSSL with embedded anchors + real time ----- */

    OSTLS_B3_GetAnchors(&anchors, &anchors_count);
    br_ssl_client_init_full(&gB4Client, &gB4X509, anchors, anchors_count);
    br_x509_minimal_set_time(&gB4X509, br_days, br_seconds);

    entropy_err = OSTLS_InjectStageAEntropy(&gB4Client.eng);
    if (entropy_err != 0) {
        b4_status(out_msg, out_msg_len, "B4: entropy inject FAIL", 0);
        OTSndOrderlyDisconnect(ep);
        OTCloseProvider(ep);
        return (OSErr)kOSTLSB4_EntropyFail;
    }

    br_ssl_engine_set_buffer(&gB4Client.eng, gB4IoBuf, sizeof gB4IoBuf, 1);

    reset_ok = br_ssl_client_reset(&gB4Client, server_name, 0);
    if (reset_ok == 0) {
        b4_status(out_msg, out_msg_len,
            "B4: br_ssl_client_reset returned 0",
            (long)br_ssl_engine_last_error(&gB4Client.eng));
        OTSndOrderlyDisconnect(ep);
        OTCloseProvider(ep);
        return (OSErr)kOSTLSB4_ClientResetFail;
    }

    /* ----- 4. Drive handshake AND application I/O in one loop -----
     *
     * The same SENDREC/RECVREC plumbing services both handshake and
     * app-data records. The new state transitions vs B3:
     *
     *   - SENDAPP and request_sent < request_total : copy more bytes
     *       into the engine's sendapp buf, ack, flush
     *   - RECVAPP : pull plaintext into out_prefix until full
     *   - CLOSED with BR_ERR_OK : peer closed cleanly after sending
     *       its response; not an error if we collected any bytes
     */

    handshake_done = 0;
    request_sent = 0;
    deadline_ticks = TickCount() + (unsigned long)(60UL * 60UL);

    for (;;) {
        now_ticks = TickCount();
        if (now_ticks > deadline_ticks) {
            b4_status(out_msg, out_msg_len,
                "B4: handshake/io timeout (60 s)", 0);
            OTSndOrderlyDisconnect(ep);
            OTCloseProvider(ep);
            return (OSErr)kOSTLSB4_HandshakeTimeout;
        }

        ssl_state = br_ssl_engine_current_state(&gB4Client.eng);

        if ((ssl_state & BR_SSL_CLOSED) != 0) {
            ssl_err = br_ssl_engine_last_error(&gB4Client.eng);
            if (ssl_err == BR_ERR_OK) {
                /* Peer closed cleanly. If we have bytes, success. */
                if (response_collected > 0) {
                    break;
                }
                b4_status(out_msg, out_msg_len,
                    "B4: peer closed before any plaintext", 0);
                OTCloseProvider(ep);
                return (OSErr)kOSTLSB4_NoBytesReceived;
            }
            b4_status(out_msg, out_msg_len,
                "B4: BearSSL error during I/O", (long)ssl_err);
            OTSndOrderlyDisconnect(ep);
            OTCloseProvider(ep);
            return b4_classify_bearssl_error(ssl_err);
        }

        /*
         * Once handshake completes, BR_SSL_SENDAPP becomes visible.
         * Latch that and start pushing the GET. We continue driving
         * SENDREC/RECVREC because BearSSL still has internal
         * processing (record framing, change-cipher, etc.) to do.
         */
        if (!handshake_done
            && (ssl_state & (BR_SSL_SENDAPP | BR_SSL_RECVAPP)) != 0) {
            handshake_done = 1;
        }

        /* Try to push more of the request. */
        if (handshake_done
            && request_sent < request_total
            && (ssl_state & BR_SSL_SENDAPP) != 0) {
            unsigned char *abuf;
            size_t alen;
            size_t to_copy;

            abuf = br_ssl_engine_sendapp_buf(&gB4Client.eng, &alen);
            if (alen == 0) {
                /* SENDAPP set but no room? Loop and let SENDREC drain. */
                continue;
            }
            to_copy = request_total - request_sent;
            if (to_copy > alen) {
                to_copy = alen;
            }
            memcpy(abuf, request_line + request_sent, to_copy);
            br_ssl_engine_sendapp_ack(&gB4Client.eng, to_copy);
            request_sent += to_copy;
            if (request_sent >= request_total) {
                /* Force the engine to package the request now rather
                 * than waiting for more bytes that aren't coming. */
                br_ssl_engine_flush(&gB4Client.eng, 0);
            }
            continue;
        }

        /* Pull as much plaintext as the caller's buffer can hold. */
        if (handshake_done
            && (ssl_state & BR_SSL_RECVAPP) != 0
            && response_collected + 1 < out_prefix_cap) {
            unsigned char *abuf;
            size_t alen;
            size_t room;
            size_t to_copy;

            abuf = br_ssl_engine_recvapp_buf(&gB4Client.eng, &alen);
            if (alen == 0) {
                continue;
            }
            room = (out_prefix_cap - 1) - response_collected;
            to_copy = alen;
            if (to_copy > room) {
                to_copy = room;
            }
            memcpy(out_prefix + response_collected, abuf, to_copy);
            response_collected += to_copy;
            out_prefix[response_collected] = '\0';
            br_ssl_engine_recvapp_ack(&gB4Client.eng, to_copy);

            /* If the prefix is full, we have what we wanted -- but
             * we must keep draining the engine until it closes so
             * BearSSL doesn't get stuck mid-record. Simpler: stop
             * here and tear down. The peer will see the connection
             * drop, which is fine for a one-shot probe. */
            if (response_collected + 1 >= out_prefix_cap) {
                break;
            }
            continue;
        }

        if ((ssl_state & BR_SSL_SENDREC) != 0) {
            unsigned char *sbuf;
            size_t slen;
            OTResult sent;

            sbuf = br_ssl_engine_sendrec_buf(&gB4Client.eng, &slen);
            if (slen == 0) {
                continue;
            }
            sent = OTSnd(ep, sbuf, (long)slen, 0);
            if (sent < 0) {
                b4_status(out_msg, out_msg_len,
                    "B4: OTSnd FAIL", (long)sent);
                OTSndOrderlyDisconnect(ep);
                OTCloseProvider(ep);
                return (OSErr)kOSTLSB4_OTSndFail;
            }
            br_ssl_engine_sendrec_ack(&gB4Client.eng, (size_t)sent);
            continue;
        }

        if ((ssl_state & BR_SSL_RECVREC) != 0) {
            unsigned char *rbuf;
            size_t rlen;
            OTResult got;

            rbuf = br_ssl_engine_recvrec_buf(&gB4Client.eng, &rlen);
            if (rlen == 0) {
                continue;
            }
            got = OTRcv(ep, rbuf, (long)rlen, NULL);
            if (got < 0) {
                b4_status(out_msg, out_msg_len,
                    "B4: OTRcv FAIL", (long)got);
                OTSndOrderlyDisconnect(ep);
                OTCloseProvider(ep);
                return (OSErr)kOSTLSB4_OTRcvFail;
            }
            if (got == 0) {
                /* Peer hung up. Engine will surface this as CLOSED
                 * on the next state check. */
                br_ssl_engine_recvrec_ack(&gB4Client.eng, 0);
                continue;
            }
            br_ssl_engine_recvrec_ack(&gB4Client.eng, (size_t)got);
            continue;
        }

        /* Engine has no flag set: either we've reached app-data
         * stability and there's nothing to do until the peer sends
         * more, or we're in a transitional state. Loop. */
    }

    /* ----- 5. Success: log + summarize ----- */

    log_response_prefix((const unsigned char *)out_prefix,
                        response_collected);

    {
        const br_ssl_session_parameters *sess;
        unsigned suite;
        const char *label;

        sess = &gB4Client.eng.session;
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
        sprintf(out_msg, "B4 OK %s %luB body", label,
                (unsigned long)response_collected);
        out_msg[out_msg_len - 1] = '\0';
    }

    OTSndOrderlyDisconnect(ep);
    OTCloseProvider(ep);
    return (OSErr)kOSTLSB4_OK;
}
