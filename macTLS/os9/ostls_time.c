/*
 * ostls_time.c -- OS 9 -> BearSSL time conversion. See header.
 *
 * Both subtractions are done in unsigned 32-bit arithmetic. The Mac
 * epoch starts in 1904; GetDateTime returns UInt32 seconds-since-1904
 * which overflows around 2040. As long as the user's clock reads
 * between 2000 and ~2040 the conversion produces correct values; we
 * cap the explicit failure path at the lower bound only.
 */

#include "ostls_time.h"

#include <string.h>

#ifdef __MWERKS__
#include <Types.h>
#include <DateTimeUtils.h>      /* GetDateTime */
#else
/* Non-CW8 syntax check. GetDateTime is a Toolbox call -- under
 * Retro68 we stub it to a fixed value sufficient to make the file
 * parse and exercise the math at the type-system level. */
static void GetDateTime(UInt32 *p) { *p = 0; }
#endif


/*
 * Number of seconds between the Mac epoch (1904-01-01) and the
 * Unix epoch (1970-01-01).
 *
 *   66 years * 365 days = 24090
 *   + 17 leap days inside the range [1904, 1968]
 *   = 24107 days * 86400 s/day = 2082844800 s
 */
#define OSTLS_MAC_TO_UNIX_OFFSET    2082844800UL

/*
 * Day count of the Unix epoch (1970-01-01) under BearSSL's
 * "proleptic Gregorian from 0000-01-01" day numbering. Documented
 * in bearssl_x509.h next to br_x509_minimal_set_time. We add this
 * after converting unix_seconds to whole days.
 */
#define OSTLS_BEARSSL_DAYS_UNIX_EPOCH  719528UL

/*
 * Number of seconds between Unix epoch and 2000-01-01. Used only
 * for the "clock-too-old" guard. 30 years + 7 leap days = 10957
 * days * 86400 s = 946684800 s.
 */
#define OSTLS_UNIX_TO_2000_OFFSET    946684800UL


OSErr
OSTLS_GetBearSSLTime(UInt32 *out_days, UInt32 *out_seconds)
{
    UInt32 mac_seconds;
    UInt32 unix_seconds;
    UInt32 unix_days;
    UInt32 seconds_of_day;

    if (out_days == NULL || out_seconds == NULL) {
        return (OSErr)kOSTLSTimeBadArgs;
    }

    mac_seconds = 0;
    GetDateTime(&mac_seconds);

    /* If the clock reads before 1970 the subtraction would underflow
     * in unsigned arithmetic; catch that as the same "clock before
     * 2000" failure since we won't get a usable date in either case. */
    if (mac_seconds < OSTLS_MAC_TO_UNIX_OFFSET) {
        *out_days = 0;
        *out_seconds = 0;
        return (OSErr)kOSTLSTimeClockBefore2000;
    }
    unix_seconds = mac_seconds - OSTLS_MAC_TO_UNIX_OFFSET;

    /* Reject Mac clocks set before 2000-01-01 -- post-2000-issued
     * certs would look not-yet-valid against such a "now" and the
     * resulting validation cascade hides the root cause. */
    if (unix_seconds < OSTLS_UNIX_TO_2000_OFFSET) {
        *out_days = 0;
        *out_seconds = 0;
        return (OSErr)kOSTLSTimeClockBefore2000;
    }

    /* Split into whole days + seconds-of-day, then shift the day
     * count into BearSSL's "since 0 AD" numbering. */
    unix_days       = unix_seconds / 86400UL;
    seconds_of_day  = unix_seconds % 86400UL;

    *out_days    = unix_days + OSTLS_BEARSSL_DAYS_UNIX_EPOCH;
    *out_seconds = seconds_of_day;
    return (OSErr)kOSTLSTimeOK;
}
