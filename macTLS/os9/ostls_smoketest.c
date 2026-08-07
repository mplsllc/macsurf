/*
 * ostls_smoketest.c -- Stage A smoke test. See ostls_smoketest.h.
 *
 * No sockets, no Open Transport, no cert parsing, no real handshake.
 * Just enough BearSSL state machine work to prove the build links and
 * the engine reaches a sane "ready to send ClientHello" state.
 *
 * On success the engine is left in BR_SSL_SENDREC state -- meaning if
 * we were to drive it, the next thing it wants to do is hand us bytes
 * to put on the wire. We do NOT drain that buffer. The test ends here.
 *
 * Stage B's job is to start consuming sendrec/recvrec from a real OT
 * endpoint.
 */

#include "ostls_smoketest.h"
#include "ostls_entropy.h"

#include "bearssl_ssl.h"
#include "bearssl_x509.h"

#if defined(__MWERKS__) || defined(__RETRO68__)  /* Mac target, either toolchain */
#include <Types.h>
#else
typedef short OSErr;
#define noErr 0
#endif


/*
 * Static state. Lives in the application's BSS / globals segment, NOT
 * on the stack. The cryanc port documented that classic-Mac TLS
 * implementations can blow past the default stack with deep crypto
 * call chains; pulling these large structures off the stack is the
 * first defensive measure.
 *
 *   br_ssl_client_context   ~10 KB (varies by config)
 *   br_x509_minimal_context ~5 KB
 *   I/O buffer              33 178 bytes (BR_SSL_BUFSIZE_BIDI)
 *
 * Total roughly 50-70 KB resident from the moment the smoke test runs.
 * Acceptable inside the 16 MB MacSurf Carbon partition.
 */
static br_ssl_client_context   gSmokeClient;
static br_x509_minimal_context gSmokeX509;
static unsigned char           gSmokeIOBuf[BR_SSL_BUFSIZE_BIDI];


OSErr
OSTLS_SmokeTest(void)
{
    unsigned state;
    int reset_ok;
    int last_err;
    int entropy_err;

    /*
     * Step 1: Initialize the SSL client context with the "full" profile
     * (all supported cipher suites, hashes, curves) but zero trust
     * anchors. We don't care about cert validation here -- the engine
     * never gets that far. We just need a valid initialization that
     * exercises br_ssl_client_zero(), the cipher-suite registration,
     * and the x509-minimal vtable wiring.
     */
    br_ssl_client_init_full(&gSmokeClient, &gSmokeX509,
        (const br_x509_trust_anchor *)0, 0);

    /*
     * Step 2: Inject the Stage A insecure entropy. Must happen before
     * br_ssl_client_reset() or that call will fail with BR_ERR_NO_RANDOM.
     */
    entropy_err = OSTLS_InjectEntropy(&gSmokeClient.eng);
    if (entropy_err != 0) {
        return (OSErr)kOSTLSSmokeEntropyInjectFailed;
    }

    /*
     * Step 3: Attach the bidirectional I/O buffer. The '1' means
     * full-duplex (separate input and output regions). On a real OS 9
     * build this buffer is the single largest BearSSL allocation;
     * keeping it static lets us measure the real partition footprint
     * up front rather than discovering it under handshake load.
     */
    br_ssl_engine_set_buffer(&gSmokeClient.eng,
        gSmokeIOBuf, sizeof gSmokeIOBuf, 1);

    /*
     * Step 4: Reset against a dummy hostname. "test.invalid" is the
     * RFC 2606 reserved TLD that guarantees no real host will ever
     * resolve. Zero resume_session means no session-resumption attempt.
     */
    reset_ok = br_ssl_client_reset(&gSmokeClient, "test.invalid", 0);
    if (reset_ok == 0) {
        return (OSErr)kOSTLSSmokeClientResetReturned0;
    }

    /*
     * Step 5: Inspect the engine state. After a successful reset, the
     * client engine should have BR_SSL_SENDREC set -- it has a ClientHello
     * queued and wants to hand it to the transport layer. It should NOT
     * have BR_SSL_CLOSED set (would mean we hit an immediate fatal error
     * inside reset).
     */
    state = br_ssl_engine_current_state(&gSmokeClient.eng);
    last_err = br_ssl_engine_last_error(&gSmokeClient.eng);

    if (last_err != BR_ERR_OK || (state & BR_SSL_CLOSED) != 0) {
        return (OSErr)kOSTLSSmokeBadEngineAfterReset;
    }

    if ((state & BR_SSL_SENDREC) == 0) {
        return (OSErr)kOSTLSSmokeNoSendrecAfterReset;
    }

    /*
     * That's it. The engine is alive, has a ClientHello ready to send,
     * and would happily drive a real handshake if we wired it to OT.
     * We don't. Stage B handles that.
     */
    return noErr;
}
