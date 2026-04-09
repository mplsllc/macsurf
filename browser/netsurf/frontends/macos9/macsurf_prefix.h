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

#ifdef __MWERKS__
#include <MacTypes.h>
/*
 * Include MSL's stat.h and fcntl.h early so their definitions of
 * mode_t, off_t, struct stat, S_IFDIR, O_RDONLY, etc. come first.
 * Our mac_types.h has #ifndef guards that will skip duplicates.
 */
#include <stat.h>
#include <fcntl.h>
#endif
#include "mac_types.h"

/*
 * CW8 MacTypes.h bool = unsigned char, but log.c may see a
 * different bool via stdbool.h. Use unsigned char directly
 * to match both sides.
 */
extern unsigned char verbose_log;

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

/*
 * nslog_ensure_t — defined in log.h which we block above.
 * log.c needs this typedef for nslog_init().
 */
typedef unsigned char (nslog_ensure_t)(FILE *fptr);

/* C99 restrict keyword — not supported by CW8 C89 */
#ifndef restrict
#define restrict
#endif

/* --- Additional build defines --- */

#ifndef __MACOS9__
#define __MACOS9__ 1
#endif

#ifndef WITHOUT_DUKTAPE
#define WITHOUT_DUKTAPE 1
#endif

#ifndef NO_IPV6
#define NO_IPV6 1
#endif

#ifndef PATH_MAX
#define PATH_MAX 256
#endif

#ifndef NETSURF_BUILTIN_LOG_FILTER
#define NETSURF_BUILTIN_LOG_FILTER "level:WARNING"
#endif

#ifndef NETSURF_BUILTIN_VERBOSE_FILTER
#define NETSURF_BUILTIN_VERBOSE_FILTER "level:VERBOSE"
#endif

#endif /* MACSURF_PREFIX_H */
