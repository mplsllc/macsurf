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
#include <OSUtils.h>            /* ReadLocation, MachineLocation */
#else
/* Non-CW8 syntax check. GetDateTime / ReadLocation are Toolbox calls --
 * under Retro68 we stub them so the file parses and the math is exercised
 * at the type-system level. */
static void GetDateTime(UInt32 *p) { *p = 0; }
typedef struct {
    long latitude;
    long longitude;
    union { char dlsDelta; long gmtDelta; } u;
} MachineLocation;
static void ReadLocation(MachineLocation *l) { l->u.gmtDelta = 0; }
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

    /* fixes834: GetDateTime returns the Mac's LOCAL time, but X.509
     * notBefore/notAfter are in GMT and br_x509_minimal_set_time expects
     * a GMT "now". Using local time as GMT skews validation by the
     * machine's timezone offset -- normally harmless (certs last months)
     * but on a FRESH cert whose notBefore is within that offset of now,
     * a machine behind GMT (e.g. US timezones) sees it as not-yet-valid
     * and fails with X509_EXPIRED (br_err=54), while a machine at/ahead
     * of GMT loads it fine. Convert local -> GMT with the machine's
     * gmtDelta (seconds EAST of GMT; local = GMT + gmtDelta, so
     * GMT = local - gmtDelta). Low 24 bits of u.gmtDelta hold the signed
     * offset; the high byte is dlsDelta. If unset, gmtDelta is 0 -> no
     * change (correct for a GMT machine). Done in unsigned arithmetic
     * because mac_seconds exceeds LONG_MAX for post-2038 Mac epochs. */
    {
        MachineLocation loc;
        long gmtDelta;
        memset(&loc, 0, sizeof(loc));
        ReadLocation(&loc);
        gmtDelta = loc.u.gmtDelta & 0x00FFFFFFL;   /* low 24 bits */
        if (gmtDelta & 0x00800000L) {              /* sign-extend 24-bit */
            gmtDelta -= 0x01000000L;
        }
        if (gmtDelta >= 0) {
            if (mac_seconds >= (UInt32)gmtDelta) {
                mac_seconds -= (UInt32)gmtDelta;
            }
        } else {
            mac_seconds += (UInt32)(-gmtDelta);
        }
    }

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
