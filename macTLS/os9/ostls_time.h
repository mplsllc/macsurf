/*
 * ostls_time.h
 *
 * Convert the OS 9 system clock into the (days-since-2000,
 * seconds-into-day) pair that br_x509_minimal_set_time() expects.
 * Used by Stage B3 to feed current time into BearSSL's cert validator
 * so notBefore / notAfter checks are meaningful.
 *
 * Three epochs are involved:
 *
 *   Classic Mac : seconds since 1904-01-01 00:00:00 (UInt32, returned
 *                 by Toolbox GetDateTime()).
 *   Unix        : seconds since 1970-01-01 00:00:00.
 *   BearSSL X509: days since 0000-01-01 (PROLEPTIC GREGORIAN!) plus
 *                 seconds-into-current-day.
 *
 * BearSSL's epoch is THE YEAR 0 AD, not 2000. From bearssl_x509.h:
 *   "The 'Unix Epoch' (1970-01-01) corresponds to days=719528 and
 *    seconds=0."
 *
 * Conversion path:
 *
 *   Mac to Unix : subtract 2 082 844 800
 *                 (66 years 1904->1970, including the 17 leap days
 *                 inside that range -- 1904, 1908, ..., 1968)
 *   Unix to BearSSL days : divide unix_seconds by 86400 and ADD
 *                          719 528 (Unix epoch in BearSSL day count)
 *
 * Clocks before 2000-01-01 are still rejected (separately, after the
 * conversion) because BearSSL would treat such a "now" as making
 * post-2000-issued certs look not-yet-valid -- confusing to debug.
 * PRAM-battery-dead Macs commonly reset to 1904 or 1956; better to
 * surface that as a distinct error code and a clear log message than
 * to chase phantom "expired cert" errors.
 */

#ifndef OSTLS_TIME_H
#define OSTLS_TIME_H

#ifdef __MWERKS__
#include <Types.h>      /* OSErr, UInt32 */
#else
/* Guard against duplicate typedefs when multiple ostls_*.h are
 * included in the same translation unit under the non-CW8 syntax
 * check path. CW8 picks these up from <Types.h> so the guard
 * doesn't matter there. */
#ifndef OSTLS_OSERR_DEFINED
#define OSTLS_OSERR_DEFINED
typedef short OSErr;
#endif
#ifndef OSTLS_UINT32_DEFINED
#define OSTLS_UINT32_DEFINED
typedef unsigned long UInt32;
#endif
#endif

/*
 * Result codes. Disjoint from B1/B2/B3 so the result window can
 * distinguish "OT works but clock is wrong" from "TLS handshake
 * failed for some other reason".
 */
enum {
    kOSTLSTimeOK              = 0,
    kOSTLSTimeBadArgs         = 250,
    kOSTLSTimeClockBefore2000 = 251
};

/*
 * Populate *days and *seconds from the current system clock.
 *
 *   *days    = days from 2000-01-01 to today (UTC; OS 9 has no
 *              timezone concept, so this is effectively local time;
 *              cert validity is forgiving of one-day skew in
 *              practice).
 *   *seconds = seconds within today, in [0, 86399].
 *
 * Returns kOSTLSTimeOK on success. Returns kOSTLSTimeClockBefore2000
 * if the Mac clock reads earlier than 2000-01-01 -- caller should
 * report the condition clearly rather than attempting validation
 * with a meaningless time base.
 */
OSErr OSTLS_GetBearSSLTime(UInt32 *out_days, UInt32 *out_seconds);

#endif /* OSTLS_TIME_H */
