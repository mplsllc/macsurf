/*
 * MacSurf -- macsurf_debug_log.h  (fixes149)
 *
 * Persistent file-backed diagnostic channel. Writes one line per
 * log call to "MacSurf Debug.log" on the Desktop, flushing after
 * every write so the file survives hard crashes. Used to get a
 * post-crash back trace without a MacsBug / ADB keyboard on
 * hardware we cannot attach a debugger to.
 *
 * Paired with the title-bar MS_LOG channel in macsurf_debug.h.
 * MS_LOG now calls both paths -- title bar for live feedback,
 * log file for the durable record.
 *
 * On non-__MACOS9__ builds (Linux cross-check) these calls are
 * implemented as no-ops so syntax checking does not pull in the
 * Mac File Manager.
 */

#ifndef MACSURF_DEBUG_LOG_H
#define MACSURF_DEBUG_LOG_H

void macsurf_debug_log_init(void);
void macsurf_debug_log_close(void);
void macsurf_debug_log_write(const char *msg);
/* fixes96 — explicit volume flush. _write no longer FlushVols per
 * line (~10-50 ms HFS sync per call was destroying perf — ~80
 * MS_LOG calls / page load = 1-4 s of disk wait). Call this only
 * after critical messages that must survive a crash. Clean shutdown
 * via _close already flushes the volume implicitly. */
void macsurf_debug_log_flush(void);

/*
 * Minimal printf-style helper. Supports only the format specifiers
 * used by MacSurf instrumentation:
 *   %d   -- int, decimal, signed
 *   %ld  -- long, decimal, signed
 *   %p   -- pointer, hex, 8 digits, no 0x prefix
 *   %s   -- const char * (NULL printed as "(null)")
 *   %%   -- literal percent
 * Unrecognised specifiers are echoed verbatim. Output is truncated
 * to 255 bytes; no overflow. C89 <stdarg.h> semantics.
 */
void macsurf_debug_log_writef(const char *fmt, ...);

/*
 * fixes366h -- runtime-gated high-frequency trace channel.
 *
 * Same formatting as macsurf_debug_log_writef, but every call is
 * suppressed unless macsurf_debug_log_trace_enabled is non-zero
 * (default 0). Use this -- NOT writef -- for any per-element or
 * per-property diagnostic that fires inside cascade / layout / paint
 * loops.
 *
 * Motivation: the fixes347d var() resolution trace emitted 77,001 of
 * the 80,601 lines in a single mactrove load (95.5%), firing inside
 * every one of the page's 14 recascades. Even with buffered writes
 * (fixes261) that formatting cost is folded into the measured cascade
 * time, so profiling captures could not separate real cascade cost
 * from logging overhead. Gating these off by default keeps the phase
 * stamps and crash-forensic log clean and the cascade timing honest;
 * flip macsurf_debug_log_trace_enabled = 1 when a var()/grid bug
 * actually needs the firehose back.
 */
extern int macsurf_debug_log_trace_enabled;
void macsurf_debug_log_tracef(const char *fmt, ...);

/*
 * fixes366a -- profile timer. Captures a Microseconds() timestamp
 * into a static UnsignedWide t0, and on each stamp call writes one
 * log line "[+NNNNNus] LABEL" where N is integer microseconds since
 * t0. Built on top of the existing file-backed log channel so the
 * timing trail survives crashes.
 *
 * Delta is computed in double precision (CW8 PPC miscompiles 64-bit
 * integer arithmetic -- project_cw8_longlong_codegen). PPC's FPU
 * handles this in single-digit cycles.
 *
 * macsurf_profile_reset() is called at startup and at every
 * navigation entry so each page load has a fresh phase clock.
 * macsurf_profile_stamp() auto-resets on first call if reset was
 * never invoked.
 */
void macsurf_profile_reset(void);
void macsurf_profile_stamp(const char *label);

/* fixes369 (#167) — per-load page-weight + resource-count measurement,
 * the size dimension the timing stamps lacked. Reset by
 * macsurf_profile_reset(). The fetchers call _add_bytes() for each chunk
 * of body delivered and _count_resource() once per fetch started; main.c
 * calls _emit(url) at first-paint so perf/scrape.py can fold total bytes
 * and sub-resource count into perf/history.csv alongside the timings. */
void macsurf_profile_add_bytes(long n);
void macsurf_profile_count_resource(void);
void macsurf_profile_emit(const char *url);

#endif
