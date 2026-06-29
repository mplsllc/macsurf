/*
 * macsurf_timebase.c -- PowerPC Time Base register access.
 *
 * Provides:
 *   macsurf_tb_read()          -- atomic 64-bit TBL/TBU read
 *   macsurf_tb_calibrate()     -- TickCount-boundary frequency calibration
 *   macsurf_tb_to_us()         -- interval to microseconds
 *   macsurf_tb_ticks_per_us()  -- raw calibration value
 *   macsurf_tb_elapsed_ms()    -- ms since calibration (monotonic)
 *   macsurf_date_get_now()  -- Date.now() backend (ms since epoch)
 *   macsurf_monotonic_ms()  -- performance.now() backend (monotonic ms)
 *
 * The PPC Time Base (TBL SPR 268, TBU SPR 269) ticks at bus_speed/4 and
 * is readable from user mode without any Toolbox call.  On a 66 MHz-bus
 * G3 iMac this is ~16.5 MHz (~60 ns/tick).  macsurf_tb_calibrate() measures
 * the exact frequency once at startup by spanning a TickCount boundary
 * (1/60.15 s = 16 628 us).
 *
 * All PPC assembly is inside #ifdef __MWERKS__ guards; Linux syntax checks
 * compile through the else branch which returns 0 / 0.0.
 *
 * C89, CW8-clean.  No inline, no // comments, no C99.
 */

#include "macsurf_timebase.h"

#ifdef __MWERKS__
#include <DateTimeUtils.h>   /* GetDateTime */
#include <Timer.h>           /* Microseconds, UnsignedWide */
#include <Events.h>          /* TickCount */
#endif

/* Mac epoch is 1904-01-01; Unix epoch is 1970-01-01.
 * Difference: 2 082 844 800 seconds. */
#define TB_MAC_EPOCH_OFFSET 2082844800.0

/* One TickCount tick = 1/60.15 s = ~16 628 us (NTSC).
 * Used as divisor in calibration: delta_ticks / 16628 = ticks-per-us. */
#define TB_TICKS_PER_60HZ 16628UL

/* File-statics set by macsurf_tb_calibrate(). */
static unsigned long g_tb_ticks_per_us = 0;
static macsurf_tb64  g_tb_start;
static int           g_tb_calibrated = 0;

/* ------------------------------------------------------------------
 * macsurf_tb_read
 *
 * Read TBL and TBU atomically using a retry loop that re-reads TBU
 * after TBL and retries if TBU changed (i.e., TBL wrapped).  On a
 * G3 at 16.5 MHz, the wrap takes ~260 s; the retry loop fires at
 * most once per 260-second interval.
 * ------------------------------------------------------------------ */
macsurf_tb64 macsurf_tb_read(void)
{
    macsurf_tb64 t;
#ifdef __MWERKS__
    register unsigned long hi, lo, hi2;
    do {
        asm { mftbu hi }
        asm { mftb  lo }
        asm { mftbu hi2 }
    } while (hi != hi2);
    t.hi = hi;
    t.lo = lo;
#else
    t.hi = 0;
    t.lo = 0;
#endif
    return t;
}

/* ------------------------------------------------------------------
 * macsurf_tb_calibrate
 *
 * Measure TBL ticks across one TickCount boundary (~16 628 us) to
 * derive g_tb_ticks_per_us.  Also records g_tb_start for elapsed-ms
 * calculations.  Safe to call more than once; only the first call
 * runs the measurement.
 * ------------------------------------------------------------------ */
void macsurf_tb_calibrate(void)
{
#ifdef __MWERKS__
    long tc0;
    macsurf_tb64 tb_a, tb_b;
    unsigned long delta_lo;
#endif

    if (g_tb_calibrated)
        return;

#ifdef __MWERKS__
    /* Wait for a clean tick boundary so we sample exactly one tick. */
    tc0 = TickCount();
    while (TickCount() == tc0) {}
    tb_a = macsurf_tb_read();
    g_tb_start = tb_a;

    /* Span exactly one more tick. */
    tc0 = TickCount();
    while (TickCount() == tc0) {}
    tb_b = macsurf_tb_read();

    /* delta_lo is the TB count for one 1/60.15 s tick. */
    delta_lo = tb_b.lo - tb_a.lo;
    g_tb_ticks_per_us = delta_lo / TB_TICKS_PER_60HZ;

    /* Safe floor: 16 ticks/us is ~16 MHz, the lowest any production
     * G3/G4 runs (66 MHz bus / 4).  Prevents division by zero. */
    if (g_tb_ticks_per_us < 1)
        g_tb_ticks_per_us = 16;
#else
    g_tb_start.hi = 0;
    g_tb_start.lo = 0;
    g_tb_ticks_per_us = 1;
#endif

    g_tb_calibrated = 1;
}

/* ------------------------------------------------------------------
 * macsurf_tb_ticks_per_us
 *
 * Return the calibrated ticks-per-microsecond count.  Returns 0 if
 * macsurf_tb_calibrate() has not been called yet.
 * ------------------------------------------------------------------ */
unsigned long macsurf_tb_ticks_per_us(void)
{
    return g_tb_ticks_per_us;
}

/* ------------------------------------------------------------------
 * macsurf_tb_to_us
 *
 * Convert a [start, end] time base pair to elapsed microseconds.
 * Uses only the low 32 bits of each stamp, so the maximum measurable
 * interval is ~260 s at 16.5 MHz.  Returns 0 if not calibrated.
 * ------------------------------------------------------------------ */
unsigned long macsurf_tb_to_us(macsurf_tb64 start, macsurf_tb64 end)
{
    unsigned long delta;
    if (!g_tb_calibrated || g_tb_ticks_per_us == 0)
        return 0;
    delta = end.lo - start.lo;   /* unsigned subtraction handles single wrap */
    return delta / g_tb_ticks_per_us;
}

/* ------------------------------------------------------------------
 * macsurf_tb_elapsed_ms
 *
 * Return milliseconds elapsed since macsurf_tb_calibrate() was
 * called.  Returns 0.0 on non-PPC builds or before calibration.
 *
 * Uses double to avoid the CW8 PPC long-long multiply miscompile.
 * IEEE 754 double has 52-bit mantissa which covers all int32 values
 * exactly.  At ~16 000 ticks/us, the TBL value at 260 s is ~4.2e9
 * which fits comfortably in double.
 * ------------------------------------------------------------------ */
double macsurf_tb_elapsed_ms(void)
{
#ifdef __MWERKS__
    macsurf_tb64 now;
    unsigned long delta;
    if (!g_tb_calibrated || g_tb_ticks_per_us == 0)
        return 0.0;
    now = macsurf_tb_read();
    delta = now.lo - g_tb_start.lo;
    return (double)delta / ((double)g_tb_ticks_per_us * 1000.0);
#else
    return 0.0;
#endif
}

/* ------------------------------------------------------------------
 * macsurf_date_get_now
 *
 * Date.now() backend.
 * Returns milliseconds since the Unix epoch (Jan 1, 1970) as a
 * double, with sub-millisecond precision from Microseconds().
 *
 * Algorithm:
 *   epoch_ms = (GetDateTime() - mac_epoch_offset) * 1000
 *             + (Microseconds().lo % 1000000) / 1000
 *
 * GetDateTime returns whole seconds since 1904; Microseconds gives
 * the fractional-second component as microseconds-since-boot % 1e6,
 * matching the approach in shims/mac_time.c:mac_gettimeofday.
 * ------------------------------------------------------------------ */
double macsurf_date_get_now(void)
{
#ifdef __MWERKS__
    unsigned long secs;
    UnsignedWide usecs;
    double epoch_ms;
    unsigned long sub_us;

    GetDateTime(&secs);
    Microseconds(&usecs);
    sub_us = usecs.lo % 1000000UL;

    epoch_ms = ((double)secs - TB_MAC_EPOCH_OFFSET) * 1000.0
               + (double)sub_us / 1000.0;
    return epoch_ms;
#else
    return 0.0;
#endif
}

/* ------------------------------------------------------------------
 * macsurf_monotonic_ms
 *
 * Backing implementation for JS performance.now().
 *
 * Returns milliseconds elapsed since macsurf_tb_calibrate() ran,
 * using the PPC TBL register directly for ~60 ns resolution on G3
 * (vs ~16 667 us for TickCount, ~1 us for Microseconds).
 * ------------------------------------------------------------------ */
double macsurf_monotonic_ms(void)
{
    return macsurf_tb_elapsed_ms();
}
