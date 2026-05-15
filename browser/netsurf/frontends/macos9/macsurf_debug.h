/*
 * MacSurf — macsurf_debug.h
 *
 * Debug instrumentation. MS_LOG writes the message to the front
 * window's title bar so you can see pipeline progress without
 * MacsBug and without stopping in CW8's debugger.
 */

#ifndef MACSURF_DEBUG_H
#define MACSURF_DEBUG_H

/* fixes65: MS_LOG / MS_BREAK / MS_ASSERT now unconditionally route to
 * macsurf_debug_log_write / _writef regardless of MACSURF_DEBUG. The runtime
 * gate is in macsurf_debug_log.c (g_log_open check inside the real
 * implementation). Previously the MS_LOG macro was empty when MACSURF_DEBUG
 * wasn't seen for a given TU; this caused main.c init log lines to silently
 * drop while direct _writef calls in other files (window.c,
 * macos9_http_fetcher.c) still wrote — making the channel look broken in
 * an inconsistent way. The _writef path always worked because nobody routed
 * through a macro gate. */
#include "macsurf_debug_log.h"

void macsurf_debug_set_title(const char *msg);
void macsurf_debug_log_int(const char *label, long value);
void macsurf_debug_log_str(const char *label, const char *value);
void macsurf_debug_set_title_force(const char *msg);
void macsurf_debug_log_int_force(const char *label, long value);
int  macsurf_debug_title_is_locked(void);
void macsurf_debug_probe_append(const char *msg);
void macsurf_debug_probe_append_int(const char *label, long value);
void macsurf_debug_probe_reset(void);

/*
 * fixes149 -- MS_LOG was originally dual-channel (title bar + file
 *   log) so pipeline trace could be watched live.
 * fixes167 -- MS_LOG is now file-only. Dual-channel meant every
 *   pipeline-trace string ("gui inv", "resize done", etc.) clobbered
 *   the window title, making the real page title invisible and
 *   exposing internal strings to the user. The file log is sufficient
 *   for trace; for intentional probe-output in the title, use
 *   MS_LOG_STICKY which calls macsurf_debug_set_title_force. MS_BREAK
 *   and MS_ASSERT remain dual-channel since they only fire on actual
 *   failures (worth surfacing immediately).
 */
#define MS_LOG(msg)          do { macsurf_debug_log_write(msg); } while(0)
#define MS_BREAK(msg)        do { macsurf_debug_log_write(msg); macsurf_debug_set_title(msg); } while(0)
#define MS_ASSERT(cond, msg) do { if (!(cond)) { macsurf_debug_log_write(msg); macsurf_debug_set_title(msg); } } while(0)

#define MS_LOG_STICKY(msg)   macsurf_debug_set_title_force(msg)

#endif /* MACSURF_DEBUG_H */
