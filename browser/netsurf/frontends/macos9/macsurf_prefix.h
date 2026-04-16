/*
 * MacSurf prefix header — included before every translation unit
 * when building with CodeWarrior 8 in C89 mode.
 *
 * Overrides that work around CW8/C89 incompatibilities with the
 * NetSurf headers live here so we never patch upstream files.
 */

#ifndef MACSURF_PREFIX_H
#define MACSURF_PREFIX_H

/* MSL core types — stddef.h and stdlib.h are safe.
 * Do NOT include <string.h> here: CW8 finds libhubbub's internal
 * string.h instead of MSL's. MSL string.h is pulled in later via
 * MacTypes.h → MacMemory.h → string.h transitively. */
#include <stddef.h>
#include <stdlib.h>

/* POSIX type foundation.
 * Do NOT use #include <time.h> — CW8 may not find MSL's version
 * reliably through the access paths. Provide time_t and struct tm
 * manually. */
#ifndef _TIME_T
#define _TIME_T
typedef long time_t;
#endif

/* struct tm — needed by localtime(), strftime() etc.
 * Set _TIME_H too so ns_time.c's own guard blocks its redeclaration. */
#define _TIME_H
#ifndef _STRUCT_TM
#define _STRUCT_TM
struct tm {
	int tm_sec;
	int tm_min;
	int tm_hour;
	int tm_mday;
	int tm_mon;
	int tm_year;
	int tm_wday;
	int tm_yday;
	int tm_isdst;
};
extern struct tm *localtime(const time_t *);
extern struct tm *gmtime(const time_t *);
extern time_t mktime(struct tm *);
extern size_t strftime(char *, size_t, const char *, const struct tm *);
extern time_t time(time_t *);
#endif

#ifndef _MODE_T
#define _MODE_T
typedef unsigned long mode_t;
#endif

#include <MacTypes.h>

/* Globally block MSL's C++ inttypes from ruining the C build */
#define _STDINT_H
#define _CSTDINT
#define _INTTYPES_H
#define _CINTTYPES

/* Provide integer types via our C89-safe shim. The shim has per-type
 * guards (#ifndef _INT8_T etc.) so it's safe even if MacTypes.h or
 * MSL have already typedef'd some of these.  After this include,
 * block future re-inclusion of the shim itself. */
#include "stdint.h"
#define MACOS9_SHIMS_STDINT_H
#define MACSURF_INTTYPES_H

/* NetSurf error type — needed by almost every header. Include early
 * so nserror is defined before any NetSurf header is processed. */
#include "utils/errors.h"

/* Map C99 _Bool to CodeWarrior */
#ifndef _Bool
#define _Bool unsigned char
#endif

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
/* stat.h, fcntl.h, string.h, mac_dirent.h for POSIX types.
 * string.h is safe now that libhubbub's internal string.h was
 * renamed to hub_string.h to avoid shadowing MSL's version. */
#include <string.h>
#include <stat.h>
#include <fcntl.h>
#include "mac_dirent.h"
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
 * NSLOG — variadic logging macro.
 *
 * CW8 8.3 may support C99 __VA_ARGS__ as an extension even in C89
 * mode. If it does, this macro correctly forwards all arguments
 * to nslog_log. If CW8 rejects ..., we'll need a different approach.
 */
#define NSLOG(catname, level, ...) \
	nslog_log(__FILE__, "", __LINE__, __VA_ARGS__)

/*
 * nslog_ensure_t — defined in log.h which we block above.
 * log.c needs this typedef for nslog_init().
 */
typedef unsigned char (nslog_ensure_t)(FILE *fptr);

/* C99 restrict keyword — not supported by CW8 C89 */
#ifndef restrict
#define restrict
#endif

/* _ALIGNED — GCC alignment attribute macro used in libcss stylesheet.h
 * (appears as "} _ALIGNED;" after a struct). Without this define, CW8
 * parses it as a global variable of the anonymous struct type, which
 * creates a multiply-defined symbol in every TU that includes the header. */
#ifndef _ALIGNED
#define _ALIGNED
#endif

/* UNUSED / N_ELEMENTS — defined per-library in utils.h files, but on the
 * shared CW8 access paths the wrong utils.h often wins. Define globally
 * so every TU has them regardless. */
#ifndef UNUSED
#define UNUSED(x) ((void)(x))
#endif
#ifndef N_ELEMENTS
#define N_ELEMENTS(x) (sizeof((x)) / sizeof((x)[0]))
#endif

/* MSL's <assert.h> ends up referencing an __assertion_failed symbol that's
 * not available in the Carbon MSL build. Replace assert() globally with a
 * no-op so every TU compiles and links without needing MSL's debug hooks. */
#ifndef NDEBUG
#define NDEBUG 1
#endif
#define _ASSERT_H_
#define __ASSERT_H__
#define assert(x) ((void)0)

/* Block system stdbool.h — MacTypes.h already provides bool/true/false
 * and libdom internal headers had #include <stdbool.h> which we've
 * removed, but other code may still try to include it. */
#ifndef _STDBOOL_H
#define _STDBOOL_H
#endif

/* isascii — not in MSL */
#ifndef isascii
#define isascii(c) ((unsigned)(c) <= 0x7F)
#endif

/* MSL string.h may be shadowed by utils/string.h on access paths —
 * forward-declare the standard string routines we rely on so callers
 * don't default them to int(). */
extern char *strtok(char *, const char *);
extern char *strchr(const char *, int);
extern char *strrchr(const char *, int);
extern char *strstr(const char *, const char *);
extern char *strcpy(char *, const char *);
extern char *strncpy(char *, const char *, size_t);
extern char *strcat(char *, const char *);
extern char *strncat(char *, const char *, size_t);
extern char *strdup(const char *);

/* --- Additional build defines --- */

#ifndef __MACOS9__
#define __MACOS9__ 1
#endif

/* JavaScript tiered strategy (per CLAUDE.md):
 *   - Base tier builds with WITH_DUKTAPE active; Duktape is linked
 *     in and NetSurf's js_thread API is backed by macsurf_js.c.
 *   - To compile out JS entirely, flip the sense here: #define
 *     WITHOUT_DUKTAPE 1 and remove WITH_DUKTAPE. The js_stub.c stubs
 *     still exist on disk and will resolve the js_* symbols. */
#ifndef WITH_DUKTAPE
#define WITH_DUKTAPE 1
#endif

/* Image decoders — libnsbmp and libnsgif are in the source tree. */
#ifndef WITH_BMP
#define WITH_BMP 1
#endif
#ifndef WITH_GIF
#define WITH_GIF 1
#endif

/* libparserutils: disable iconv-based input filter (no iconv on OS 9).
 * Keeps the library's own UTF/8859/ASCII codecs active. */
#ifndef WITHOUT_ICONV_FILTER
#define WITHOUT_ICONV_FILTER 1
#endif

/* libparserutils uses `static inline` in headers and several .c files.
 * CW8 C89 has no `inline` — neutralize it everywhere. */
#define inline

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
