/*
 * macsurf_timebase.h — PowerPC Time Base register access.
 *
 * The PPC Time Base (TBL/TBU) is a 64-bit counter readable from
 * user mode without any Toolbox call.  It ticks at bus_speed / 4:
 *   G3 iMac  (66 MHz bus)  → 16.5 MHz →  ~60 ns / tick
 *   G4       (100 MHz bus) → 25.0 MHz →  ~40 ns / tick
 *   G4       (133 MHz bus) → 33.25 MHz → ~30 ns / tick
 *
 * CW8 inline assembly syntax: `asm { mftb  lo }` and `asm { mftbu hi }`
 * write directly to a named local C variable without any Toolbox overhead.
 *
 * The 32-bit TBL alone covers measurements up to ~260 s at 16.5 MHz,
 * which is more than enough for any per-page or per-layout stamp.  The
 * retry loop in macsurf_tb_read handles the rare TBL wrap that would
 * make TBU and TBL inconsistent.
 *
 * Calibration (macsurf_tb_calibrate) runs once at startup, empirically
 * measuring TB ticks per microsecond against a TickCount boundary so the
 * profiler does not need to know the bus speed at compile time.
 *
 * Falls back gracefully on Linux / non-PPC: read returns {0,0} and
 * to_us returns 0, keeping the non-Mac build valid for syntax checks.
 */

#ifndef MACSURF_TIMEBASE_H
#define MACSURF_TIMEBASE_H

/* ------------------------------------------------------------------ */
/* Exported struct                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    unsigned long hi; /* TBU -- upper 32 bits */
    unsigned long lo; /* TBL -- lower 32 bits  */
} macsurf_tb64;

/* ------------------------------------------------------------------ */
/* Core API                                                            */
/* ------------------------------------------------------------------ */

/* Read the time base atomically (retry loop handles TBU/TBL rollover). */
macsurf_tb64 macsurf_tb_read(void);

/* Calibrate TB frequency against a TickCount boundary.  Call once at
 * startup.  Subsequent calls are no-ops (guarded by a static flag). */
void macsurf_tb_calibrate(void);

/* Convert a [start, end] interval to microseconds.
 * Valid for intervals < ~260 s (32-bit TBL only). */
unsigned long macsurf_tb_to_us(macsurf_tb64 start, macsurf_tb64 end);

/* Raw ticks-per-microsecond value (0 until calibrated). */
unsigned long macsurf_tb_ticks_per_us(void);

/* Milliseconds elapsed since macsurf_tb_calibrate() was called.
 * Uses double to sidestep the CW8 PPC long-long miscompile.
 * Returns 0.0 before calibration or on non-PPC builds. */
double macsurf_tb_elapsed_ms(void);

/* ------------------------------------------------------------------ */
/* JS time provider hooks (performance.now / Date.now backends)       */
/* ------------------------------------------------------------------ */

/* Date.now() backend: ms since Unix epoch, sub-ms precision from
 * Microseconds(). */
double macsurf_date_get_now(void);

/* performance.now() backend: ms since macsurf_tb_calibrate(), ~60 ns
 * resolution on G3 via mftb. */
double macsurf_monotonic_ms(void);

#endif /* MACSURF_TIMEBASE_H */
