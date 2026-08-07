/*
 * ostls_smoketest.h
 *
 * Stage A smoke test entry point.
 *
 * Verifies that the vendored BearSSL subset compiles and statically
 * initializes inside CodeWarrior 8 on PowerPC Mac OS 9. NO networking,
 * NO Open Transport, NO certificate validation, NO actual handshake.
 *
 * The smoke test answers five questions:
 *
 *   1. Does CW8 link the selected BearSSL .c files into a MacSurf.mcp
 *      build without missing symbols?
 *   2. Does static allocation of br_ssl_client_context +
 *      br_x509_minimal_context succeed without trashing the partition?
 *   3. Does br_ssl_client_init_full() return cleanly with zero trust
 *      anchors?
 *   4. Does br_ssl_engine_set_buffer() accept the BR_SSL_BUFSIZE_BIDI
 *      buffer?
 *   5. Does br_ssl_client_reset("test.invalid", 0) leave the engine in
 *      a sane "wants to send ClientHello" state with no error code?
 *
 * Stage B / C / D will replace this with progressively richer tests
 * (real OT transport, real handshake, real cert validation).
 *
 * Stage A exit condition: OSTLS_SmokeTest() returns noErr on real OS 9
 * PowerPC hardware (G3 first, G4 second).
 */

#ifndef OSTLS_SMOKETEST_H
#define OSTLS_SMOKETEST_H

#if defined(__MWERKS__) || defined(__RETRO68__)  /* Mac target, either toolchain */
#include <Types.h>      /* OSErr */
#else
typedef short OSErr;
#endif

/*
 * Smoke test result codes. Negative values are reserved for OSErr-style
 * Toolbox errors; positive values are smoke-test-specific failures so
 * the caller can MS_LOG which gate failed.
 */
enum {
    kOSTLSSmokeOK                     =   0,
    kOSTLSSmokeBadEngineAfterReset    = 100,  /* engine reports error */
    kOSTLSSmokeNoSendrecAfterReset    = 101,  /* engine not asking to send */
    kOSTLSSmokeClientResetReturned0   = 102,  /* br_ssl_client_reset failed */
    kOSTLSSmokeEntropyInjectFailed    = 103   /* our stub returned non-zero */
};

/*
 * Run the smoke test. Returns kOSTLSSmokeOK (0) on success, a non-zero
 * code on failure. Idempotent and safe to call multiple times.
 *
 * Allocates ~70 KB of static state (one bidi I/O buffer + the two
 * BearSSL contexts). Call once near MacSurf startup, after toolbox init
 * but before any network use.
 */
OSErr OSTLS_SmokeTest(void);

#endif /* OSTLS_SMOKETEST_H */
