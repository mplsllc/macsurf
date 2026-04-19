/*
 * MacSurf — macsurf_debug.h
 *
 * Debug instrumentation. MS_LOG writes the message to the front
 * window's title bar so you can see pipeline progress without
 * MacsBug and without stopping in CW8's debugger.
 */

#ifndef MACSURF_DEBUG_H
#define MACSURF_DEBUG_H

#ifdef MACSURF_DEBUG

void macsurf_debug_set_title(const char *msg);

#define MS_LOG(msg)          macsurf_debug_set_title(msg)
#define MS_BREAK(msg)        macsurf_debug_set_title(msg)
#define MS_ASSERT(cond, msg) do { if (!(cond)) macsurf_debug_set_title(msg); } while(0)

void macsurf_debug_log_int(const char *label, long value);
void macsurf_debug_log_str(const char *label, const char *value);

/* Sticky variants: bypass the global title lock to set the title, then
 * re-engage the lock so subsequent non-force MS_LOG / log_int calls are
 * no-ops. Used by diagnostic probes that would otherwise be overwritten
 * by downstream pipeline logging (e.g. the redraw counter). */
void macsurf_debug_set_title_force(const char *msg);
void macsurf_debug_log_int_force(const char *label, long value);

/* Predicate: non-zero when a probe has latched the title. Non-debug
 * title-setting paths (e.g. macos9_window_set_title pushing the page
 * title / URL via SetWTitle) should consult this and skip the update
 * so probe output stays visible. */
int  macsurf_debug_title_is_locked(void);

/* Probe-accumulating variants: append to a persistent probe buffer and
 * sticky-set the window title to the accumulated text. Lets multiple
 * probes (across files / pipeline stages) contribute to one visible
 * title string, rather than overwriting each other. */
void macsurf_debug_probe_append(const char *msg);
void macsurf_debug_probe_append_int(const char *label, long value);
void macsurf_debug_probe_reset(void);

#define MS_LOG_STICKY(msg)   macsurf_debug_set_title_force(msg)

#else

#define MS_LOG(msg)
#define MS_BREAK(msg)
#define MS_ASSERT(cond, msg)
#define macsurf_debug_log_int(label, value)
#define macsurf_debug_log_str(label, value)
#define macsurf_debug_set_title_force(msg)
#define macsurf_debug_log_int_force(label, value)
#define macsurf_debug_probe_append(msg)
#define macsurf_debug_probe_append_int(label, value)
#define macsurf_debug_probe_reset()
#define macsurf_debug_title_is_locked() 0
#define MS_LOG_STICKY(msg)

#endif
#endif
