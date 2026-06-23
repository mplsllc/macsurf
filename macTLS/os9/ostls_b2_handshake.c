/*
 * ostls_b2_handshake.c -- Stage B2 BearSSL handshake over OT. See header.
 *
 * The flow mirrors B1's OT setup -- OTOpenEndpointInContext, sync +
 * blocking, OTBind, OTInitDNSAddress, OTConnect -- and then drives
 * the BearSSL state machine until handshake completion, error, or
 * the per-probe deadline.
 *
 * Insecure X.509 validator:
 *   - start_chain  : reset br_x509_decoder for fresh chain
 *   - start_cert   : remember whether this is the leaf (1st cert)
 *   - append       : feed leaf bytes into the decoder, skip the rest
 *   - end_cert     : mark leaf as done after the first cert
 *   - end_chain    : return 0 (accept) unconditionally
 *   - get_pkey     : return decoded leaf pkey
 *
 * The result: the handshake completes provided the server's leaf cert
 * decodes cleanly. There is NO chain validation, NO date checking, NO
 * hostname matching. Stage B3 replaces this with br_x509_minimal +
 * embedded trust anchors and that's where this becomes real TLS.
 */

#include "ostls_b2_handshake.h"
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
/* Non-CW8 syntax check stubs. None of this is executed under Retro68;
 * the file is C89-clean here so the pre-flight catches real issues. */
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
/* Insecure X.509 validator                                          */
/* ----------------------------------------------------------------- */

struct ostls_insecure_x509 {
    const br_x509_class *vtable;
    br_x509_decoder_context dec;
    int leaf_seen;              /* 1 once start_cert has been called once */
    int leaf_done;              /* 1 once end_cert closed the first cert  */
};


static void
insecure_start_chain(const br_x509_class **ctx, const char *server_name)
{
    struct ostls_insecure_x509 *xc;

    xc = (struct ostls_insecure_x509 *)ctx;
    (void)server_name;
    /* Initialise (or re-initialise) the decoder for a fresh leaf cert. */
    br_x509_decoder_init(&xc->dec, NULL, NULL);
    xc->leaf_seen = 0;
    xc->leaf_done = 0;
}


static void
insecure_start_cert(const br_x509_class **ctx, uint32_t length)
{
    struct ostls_insecure_x509 *xc;

    xc = (struct ostls_insecure_x509 *)ctx;
    (void)length;
    /* Only the first cert is the leaf. We still get start_cert calls
     * for the intermediates / root in the chain but we ignore their
     * bytes in append(). */
    xc->leaf_seen = 1;
}


static void
insecure_append(const br_x509_class **ctx,
                const unsigned char *buf, size_t len)
{
    struct ostls_insecure_x509 *xc;

    xc = (struct ostls_insecure_x509 *)ctx;
    if (xc->leaf_seen && !xc->leaf_done) {
        br_x509_decoder_push(&xc->dec, buf, len);
    }
}


static void
insecure_end_cert(const br_x509_class **ctx)
{
    struct ostls_insecure_x509 *xc;

    xc = (struct ostls_insecure_x509 *)ctx;
    if (xc->leaf_seen && !xc->leaf_done) {
        xc->leaf_done = 1;
    }
}


static unsigned
insecure_end_chain(const br_x509_class **ctx)
{
    (void)ctx;
    /* Accept everything. NO trust anchors, NO date checks, NO hostname
     * verification. Stage B3 replaces this entirely. */
    return 0;
}


static const br_x509_pkey *
insecure_get_pkey(const br_x509_class *const *ctx, unsigned *usages)
{
    struct ostls_insecure_x509 *xc;

    xc = (struct ostls_insecure_x509 *)ctx;
    if (usages != NULL) {
        /* Permit both signature and key-exchange usage; we have no
         * extension to consult for restriction. */
        *usages = BR_KEYTYPE_KEYX | BR_KEYTYPE_SIGN;
    }
    return br_x509_decoder_get_pkey(&xc->dec);
}


static const br_x509_class insecure_x509_vtable = {
    sizeof(struct ostls_insecure_x509),
    insecure_start_chain,
    insecure_start_cert,
    insecure_append,
    insecure_end_cert,
    insecure_end_chain,
    insecure_get_pkey
};


/* ----------------------------------------------------------------- */
/* Static BearSSL state                                              */
/* ----------------------------------------------------------------- */

/*
 * Lives in BSS, not on the stack. br_ssl_client_context is ~10 KB and
 * BR_SSL_BUFSIZE_BIDI is 33 178 bytes; together this is the bulk of
 * MacTLSTest's resident memory once Stage B is active. The minimal
 * x509 context is created but unused -- br_ssl_client_init_full
 * requires one for cipher-suite setup, after which we substitute the
 * vtable via br_ssl_engine_set_x509.
 */
static br_ssl_client_context        gB2Client;
static br_x509_minimal_context      gB2X509Min;       /* unused; required by init_full */
static struct ostls_insecure_x509   gB2InsecureX509;
static unsigned char                gB2IoBuf[BR_SSL_BUFSIZE_BIDI];


/* ----------------------------------------------------------------- */
/* OT helpers                                                        */
/* ----------------------------------------------------------------- */

/*
 * Format a status string with optional OT or BearSSL error code.
 * Bounded to keep CW8 C89's sprintf safe.
 */
static void
b2_status(char *out, size_t out_len, const char *prefix, long err)
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
OSTLS_B2_Handshake_Probe(const char *target_host_port,
                         const char *server_name,
                         char *out_msg, size_t out_msg_len)
{
    OTConfigurationRef cfg;
    EndpointRef ep;
    OSStatus oterr;
    DNSAddress dns;
    TCall call;
    long dns_len;

    unsigned ssl_state;
    int ssl_err;
    int reset_ok;
    int entropy_err;

    unsigned long deadline_ticks;
    unsigned long now_ticks;
    int handshake_done;

    if (target_host_port == NULL || server_name == NULL
        || target_host_port[0] == '\0' || server_name[0] == '\0') {
        b2_status(out_msg, out_msg_len, "B2: bad args", 0);
        return (OSErr)kOSTLSB2_BadArgs;
    }

    /* ----- 1. OT endpoint setup (mirror B1) ----- */

    cfg = OTCreateConfiguration("tcp");
    if (cfg == NULL || cfg == (OTConfigurationRef)-1L) {
        b2_status(out_msg, out_msg_len,
            "B2: OTCreateConfiguration FAIL", 0);
        return (OSErr)kOSTLSB2_OTConfigFail;
    }

    oterr = noErr;
    ep = OTOpenEndpointInContext(cfg, 0, NULL, &oterr, g_ostls_ot_context);
    if (oterr != noErr || ep == NULL) {
        b2_status(out_msg, out_msg_len,
            "B2: OTOpenEndpoint FAIL", (long)oterr);
        return (OSErr)kOSTLSB2_OTOpenEndptFail;
    }
    OTSetSynchronous(ep);
    OTSetBlocking(ep);

    oterr = OTBind(ep, NULL, NULL);
    if (oterr != noErr) {
        b2_status(out_msg, out_msg_len, "B2: OTBind FAIL", (long)oterr);
        OTCloseProvider(ep);
        return (OSErr)kOSTLSB2_OTBindFail;
    }

    OTMemzero(&call, sizeof(call));
    dns_len = OTInitDNSAddress(&dns, (char *)target_host_port);
    if (dns_len <= 0) {
        b2_status(out_msg, out_msg_len,
            "B2: OTInitDNSAddress FAIL", dns_len);
        OTCloseProvider(ep);
        return (OSErr)kOSTLSB2_OTDnsAddrFail;
    }
    call.addr.buf = (UInt8 *)&dns;
    call.addr.len = (short)dns_len;

    oterr = OTConnect(ep, &call, NULL);
    if (oterr != noErr) {
        b2_status(out_msg, out_msg_len, "B2: OTConnect FAIL", (long)oterr);
        OTCloseProvider(ep);
        return (OSErr)kOSTLSB2_OTConnectFail;
    }

    /* ----- 2. BearSSL setup ----- */

    /* init_full wires the full cipher-suite / hash table and points
     * the engine at the supplied br_x509_minimal context. We then
     * replace the X.509 hook with our insecure validator. */
    br_ssl_client_init_full(&gB2Client, &gB2X509Min,
        (const br_x509_trust_anchor *)0, 0);
    br_ssl_engine_set_x509(&gB2Client.eng,
        (const br_x509_class **)&gB2InsecureX509.vtable);
    gB2InsecureX509.vtable = &insecure_x509_vtable;

    entropy_err = OSTLS_InjectEntropy(&gB2Client.eng);
    if (entropy_err != 0) {
        b2_status(out_msg, out_msg_len, "B2: entropy inject FAIL", 0);
        OTSndOrderlyDisconnect(ep);
        OTCloseProvider(ep);
        return (OSErr)kOSTLSB2_EntropyFail;
    }

    br_ssl_engine_set_buffer(&gB2Client.eng,
        gB2IoBuf, sizeof gB2IoBuf, 1);

    reset_ok = br_ssl_client_reset(&gB2Client, server_name, 0);
    if (reset_ok == 0) {
        b2_status(out_msg, out_msg_len,
            "B2: br_ssl_client_reset returned 0",
            (long)br_ssl_engine_last_error(&gB2Client.eng));
        OTSndOrderlyDisconnect(ep);
        OTCloseProvider(ep);
        return (OSErr)kOSTLSB2_ClientResetFail;
    }

    /* ----- 3. Drive the handshake ----- */

    /* 60 seconds at 60 Hz. A real TLS handshake is single-digit
     * seconds on a healthy LAN; 60 s gives plenty of headroom for
     * slow networks or busy servers without making a stuck probe
     * hang forever. */
    deadline_ticks = TickCount() + (unsigned long)(60UL * 60UL);
    handshake_done = 0;

    while (!handshake_done) {
        now_ticks = TickCount();
        if (now_ticks > deadline_ticks) {
            b2_status(out_msg, out_msg_len,
                "B2: handshake timeout (60 s)", 0);
            OTSndOrderlyDisconnect(ep);
            OTCloseProvider(ep);
            return (OSErr)kOSTLSB2_HandshakeTimeout;
        }

        ssl_state = br_ssl_engine_current_state(&gB2Client.eng);

        if ((ssl_state & BR_SSL_CLOSED) != 0) {
            ssl_err = br_ssl_engine_last_error(&gB2Client.eng);
            if (ssl_err == BR_ERR_OK) {
                /* Engine closed cleanly without producing app data --
                 * effectively a peer-side close before completion. */
                b2_status(out_msg, out_msg_len,
                    "B2: engine CLOSED before handshake", 0);
                OTSndOrderlyDisconnect(ep);
                OTCloseProvider(ep);
                return (OSErr)kOSTLSB2_PeerClosedEarly;
            }
            b2_status(out_msg, out_msg_len,
                "B2: BearSSL handshake error", (long)ssl_err);
            OTSndOrderlyDisconnect(ep);
            OTCloseProvider(ep);
            return (OSErr)kOSTLSB2_BearSSLError;
        }

        /* Handshake completes when the engine first signals readiness
         * to either send or receive application data without being
         * closed. Either flag alone is sufficient -- BearSSL exposes
         * SENDAPP as soon as the local side finishes its Finished and
         * may produce app records; RECVAPP appears once the peer's
         * Finished has been processed. */
        if ((ssl_state & (BR_SSL_SENDAPP | BR_SSL_RECVAPP)) != 0) {
            handshake_done = 1;
            break;
        }

        if ((ssl_state & BR_SSL_SENDREC) != 0) {
            unsigned char *sbuf;
            size_t slen;
            OTResult sent;

            sbuf = br_ssl_engine_sendrec_buf(&gB2Client.eng, &slen);
            if (slen == 0) {
                /* Shouldn't happen given the flag was set, but guard. */
                continue;
            }
            sent = OTSnd(ep, sbuf, (long)slen, 0);
            if (sent < 0) {
                b2_status(out_msg, out_msg_len,
                    "B2: OTSnd FAIL", (long)sent);
                OTSndOrderlyDisconnect(ep);
                OTCloseProvider(ep);
                return (OSErr)kOSTLSB2_OTSndFail;
            }
            /* Blocking OTSnd should write all requested bytes; ack
             * exactly what it claims to have sent. */
            br_ssl_engine_sendrec_ack(&gB2Client.eng, (size_t)sent);
            continue;
        }

        if ((ssl_state & BR_SSL_RECVREC) != 0) {
            unsigned char *rbuf;
            size_t rlen;
            OTResult got;

            rbuf = br_ssl_engine_recvrec_buf(&gB2Client.eng, &rlen);
            if (rlen == 0) {
                continue;
            }
            got = OTRcv(ep, rbuf, (long)rlen, NULL);
            if (got < 0) {
                b2_status(out_msg, out_msg_len,
                    "B2: OTRcv FAIL", (long)got);
                OTSndOrderlyDisconnect(ep);
                OTCloseProvider(ep);
                return (OSErr)kOSTLSB2_OTRcvFail;
            }
            if (got == 0) {
                /* Peer orderly-disconnected. The engine has not yet
                 * reached app-data state, so this is an early close. */
                b2_status(out_msg, out_msg_len,
                    "B2: peer closed before handshake", 0);
                OTSndOrderlyDisconnect(ep);
                OTCloseProvider(ep);
                return (OSErr)kOSTLSB2_PeerClosedEarly;
            }
            br_ssl_engine_recvrec_ack(&gB2Client.eng, (size_t)got);
            continue;
        }

        /* No SEND/RECV/CLOSED flags set: engine is idle, probably
         * waiting for an internal transition. Loop and re-poll. */
    }

    /* Handshake succeeded. Report the negotiated cipher suite using a
     * short label where we recognise the suite ID, otherwise emit the
     * raw hex. Keep the string under ~50 chars so the MacTLSTest result
     * window doesn't truncate it. */
    {
        const br_ssl_session_parameters *sess;
        unsigned suite;
        const char *label;

        sess = &gB2Client.eng.session;
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
        sprintf(out_msg, "B2 OK %s 0x%04X", label, suite);
        out_msg[out_msg_len - 1] = '\0';
    }

    OTSndOrderlyDisconnect(ep);
    OTCloseProvider(ep);
    return (OSErr)kOSTLSB2_OK;
}
