/*
 * MacSurf — macsurf_debug.h
 *
 * Lightweight debug instrumentation wrapping MacsBug primitives.
 * All calls compile to nothing when MACSURF_DEBUG is not defined.
 */

#ifndef MACSURF_DEBUG_H
#define MACSURF_DEBUG_H

#ifdef MACSURF_DEBUG

#ifdef __MACOS9__

/* Drop into MacsBug with a message. */
#define MS_BREAK(msg)        DebugStr("\p" msg)

/* Drop into MacsBug only if condition is false. */
#define MS_ASSERT(cond, msg) do { if (!(cond)) DebugStr("\p" msg); } while(0)

/* Log a checkpoint message to MacsBug without stopping.
 * DebugStr with a leading semicolon logs and continues. */
#define MS_LOG(msg)          DebugStr("\p;" msg)

#else
/* Linux: print to stderr for cross-check builds. */
#include <stdio.h>
#define MS_BREAK(msg)        fprintf(stderr, "MS_BREAK: %s\n", msg)
#define MS_ASSERT(cond, msg) do { if (!(cond)) fprintf(stderr, "MS_ASSERT FAIL: %s\n", msg); } while(0)
#define MS_LOG(msg)          fprintf(stderr, "MS_LOG: %s\n", msg)
#endif

/* Log a checkpoint with a decimal integer value. */
void macsurf_debug_log_int(const char *label, long value);

/* Log a checkpoint with a string value. */
void macsurf_debug_log_str(const char *label, const char *value);

#else

#define MS_BREAK(msg)
#define MS_ASSERT(cond, msg)
#define MS_LOG(msg)
#define macsurf_debug_log_int(label, value)
#define macsurf_debug_log_str(label, value)

#endif /* MACSURF_DEBUG */

#endif /* MACSURF_DEBUG_H */
