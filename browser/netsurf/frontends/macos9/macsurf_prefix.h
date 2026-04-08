/*
 * MacSurf prefix header — included before every translation unit
 * when building with CodeWarrior 8 in C89 mode.
 *
 * Overrides that work around CW8/C89 incompatibilities with the
 * NetSurf headers live here so we never patch upstream files.
 */

#ifndef MACSURF_PREFIX_H
#define MACSURF_PREFIX_H

/*
 * Fix 1 — NSLOG variadic macro.
 *
 * NetSurf's log.h defines NSLOG with GNU-style variadic macros
 * (args..., ##args) which CW8 C89 cannot parse at all.
 * We block log.h via its include guard and provide a C89-safe
 * NSLOG replacement.  On Linux (gcc cross-check) this file is
 * never included, so the real log.h is used instead.
 */

/* Prevent log.h from being processed — CW8 chokes on args... syntax */
#define NETSURF_LOG_H

#include <stdio.h>

extern bool verbose_log;

enum nslog_level {
	NSLOG_LEVEL_DEEPDEBUG = 0,
	NSLOG_LEVEL_DEBUG = 1,
	NSLOG_LEVEL_VERBOSE = 2,
	NSLOG_LEVEL_INFO = 3,
	NSLOG_LEVEL_WARNING = 4,
	NSLOG_LEVEL_ERROR = 5,
	NSLOG_LEVEL_CRITICAL = 6
};

#ifndef NETSURF_LOG_LEVEL
#define NETSURF_LOG_LEVEL INFO
#endif

#define NSLOG_LVL(level) NSLOG_LEVEL_ ## level
#define NSLOG_EVL(level) NSLOG_LVL(level)
#define NSLOG_COMPILED_MIN_LEVEL NSLOG_EVL(NETSURF_LOG_LEVEL)

extern void nslog_log(const char *file, const char *func,
		int ln, const char *format, ...);

/*
 * C89-compatible NSLOG — three fixed arguments, no variadic macro.
 * Calls with extra printf arguments must call nslog_log() directly.
 */
#define NSLOG(catname, level, logmsg) \
	do { \
		if (NSLOG_LEVEL_##level >= NSLOG_COMPILED_MIN_LEVEL) { \
			nslog_log(__FILE__, "", __LINE__, logmsg); \
		} \
	} while(0)

#endif /* MACSURF_PREFIX_H */
