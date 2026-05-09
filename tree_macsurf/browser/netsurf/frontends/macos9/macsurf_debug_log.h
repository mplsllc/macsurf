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

#endif
