/*
 * MacSurf prefix header
 */

#ifndef MACSURF_PREFIX_H
#define MACSURF_PREFIX_H

/* NSLOG must be defined before ANYTHING else in this prefix can pull
 * in (directly or transitively) utils/log.h. Define it first so the
 * real log.h's `#ifndef NSLOG` always evaluates false and skips its
 * own GCC-`args...` / `,##__VA_ARGS__` definition that CW8 cannot
 * parse cleanly. The redundant block at the bottom of this file is
 * a belt-and-suspenders re-define in case any include between here
 * and there manages to #undef it. */
#undef NSLOG
#define NSLOG(cat, level, ...) do{}while(0)

#include <stddef.h>

/* Block MSL C++ headers */
#define _CSTDINT
#define _CINTTYPES
#define _CSTDBOOL
#define _CSTDDEF
#define _CSTDIO
#define _CSTDLIB
#define _CSTRING
#define _CTIME
#define _CMATH
#define _CSTDARG
#define _CSETJMP
#define _CSIGNAL
#define _CCTYPE
#define _CLIMITS
#define _CWCHAR
#define _CWCTYPE

/* Force MacTypes.h first for true/false enum */
#ifdef __MWERKS__
  #ifndef __MACTYPES__
    #include <MacTypes.h>
  #endif
#endif

/* Duktape cube root fallback */
#define DUK_CBRT(x) macsurf_js_cbrt(x)
extern double macsurf_js_cbrt(double x);

/* Float64: MacTypes.h owns this on CW8 (defines it as 'short double').
 * If it's already defined, skip the NetSurf core definition. */
#define __float64_defined

#define inline
#ifndef restrict
#define restrict
#endif
#define __MACOS9__ 1

/* MSL / POSIX headers */
#include <sys/types.h>
#include <stat.h>
#include <fcntl.h>
#include <time.h>
#include <stdbool.h>

#define WITHOUT_ICONV_FILTER 1
#define NO_IPV6 1
#define PATH_MAX 256
#define HAVE_UTSNAME 1

/* NSLOG: force the prefix's no-op definition to win unconditionally.
 * #undef before #define so any earlier definition (whether from a
 * stale build artifact, an out-of-order include, or log.h itself
 * being processed first) is wiped before ours lands. log.h still
 * processes for nslog_log/verbose_log/nslog_init declarations, but
 * its own #ifndef NSLOG block now sees ours defined and skips. */
#undef NSLOG
#define NSLOG(cat, level, ...) do{}while(0)

/* libhubbub/libcss/libparserutils source files include "utils/utils.h",
 * but the access path resolves to NetSurf's utils.h first, which lacks
 * N_ELEMENTS / UNUSED. Provide them globally to avoid undefined-symbol
 * cascades from the lib internals. */
#ifndef N_ELEMENTS
#define N_ELEMENTS(arr) (sizeof(arr) / sizeof((arr)[0]))
#endif

#ifndef UNUSED
#define UNUSED(x) ((x) = (x))
#endif

/* MACSURF_DEBUG; without this define, macsurf_debug_log_init() is the
 * only channel. MS_LOG and friends will be no-ops. */
#ifndef MACSURF_DEBUG
#define MACSURF_DEBUG 1
#endif

/* Duktape JS engine. WITH_DUKTAPE enables the real Duktape engine.
 * The prefix file may be included in TUs that have their own
 * preprocessor override — if so that will suppress the WITH_DUKTAPE block
 * and they will link against the no-op stubs in js_stub.c instead. */
#ifndef WITH_DUKTAPE
#define WITH_DUKTAPE 1
#endif

/* libcss int64_t multiply workaround for CW8 PPC.
 * Defining _ALIGNED as empty because CW8's MSL headers use it
 * as a global variable declaration and every TU emits a duplicate
 * symbol at link time. Define _ALIGNED as empty so the token is
 * harmless wherever it appears. */
#ifndef _ALIGNED
#define _ALIGNED
#endif

#endif /* MACSURF_PREFIX_H */
