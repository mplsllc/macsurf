/*
 * MacSurf prefix header
 */

#ifndef MACSURF_PREFIX_H
#define MACSURF_PREFIX_H

/* Block MSL C++ headers */
#define _CSTDINT
#define _CINTTYPES
#define _CSTDBOOL

/* Force MacTypes.h first for true/false enum */
#ifdef __MWERKS__
  #ifndef __MACTYPES__
    #include <MacTypes.h>
  #endif
#endif

/* Float64: MacTypes.h owns this on CW8 (defines it as 'short double').
   Only define it ourselves on non-CW8 hosts (Linux syntax-check, etc.). */
#ifndef __MWERKS__
  #ifndef _Float64_DEFINED
  #define _Float64_DEFINED
  typedef double Float64;
  #endif
#endif

#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* POSIX types foundation */
#ifndef __RETRO68__
  #ifndef _TIME_T
    #define _TIME_T
    typedef long time_t;
  #endif
  #ifndef _MODE_T
    #define _MODE_T
    typedef unsigned long mode_t;
  #endif
#else
  #include <sys/types.h>
  #include <sys/stat.h>
  #include <time.h>
#endif

#ifndef __func__
  #define __func__ "unknown"
#endif

#include <stdint.h> 
#include <inttypes.h>
#include <stdbool.h>

#ifndef _Bool
#define _Bool unsigned char
#endif

#define NETSURF_LOG_H

/* CW8 C89: use __VA_ARGS__ (CW8 extension, pre-C99). Category and level tokens
 * are consumed as macro params and never evaluated as expressions. */
#define NSLOG(cat, level, ...) do {} while(0)

/* log.h is suppressed by NETSURF_LOG_H above (it has GNU __attribute__ and
 * GCC-varargs NSLOG).  Provide the typedef log.c needs so it can compile. */
typedef bool(nslog_ensure_t)(FILE *fptr);

/* nsoption.c / options.h expect these to be defined. */
#ifndef NETSURF_BUILTIN_LOG_FILTER
#define NETSURF_BUILTIN_LOG_FILTER NULL
#endif
#ifndef NETSURF_BUILTIN_VERBOSE_FILTER
#define NETSURF_BUILTIN_VERBOSE_FILTER NULL
#endif

#ifdef __MWERKS__
#include <stat.h>
#include <fcntl.h>
#include "mac_dirent.h"
#endif
#include "mac_types.h"

#ifndef restrict
#define restrict
#endif

#ifndef isascii
#define isascii(c) ((unsigned)(c) <= 0x7F)
#endif

/* MSL <string.h> is sometimes shadowed by internal lib string.h on the
 * user access paths. Forward-declare the standard string routines so
 * callers don't default them to int(). */
extern char *strtok(char *, const char *);
extern char *strchr(const char *, int);
extern char *strrchr(const char *, int);
extern char *strstr(const char *, const char *);
extern char *strcpy(char *, const char *);
extern char *strncpy(char *, const char *, size_t);
extern char *strcat(char *, const char *);
extern char *strncat(char *, const char *, size_t);
extern char *strdup(const char *);

#define inline
#define __MACOS9__ 1
#define WITHOUT_ICONV_FILTER 1
#define NO_IPV6 1
#define PATH_MAX 256

/* libhubbub/libcss/libparserutils source files include "utils/utils.h",
 * but the access path resolves to NetSurf's utils.h first, which lacks
 * N_ELEMENTS / UNUSED. Provide them globally to avoid undefined-symbol
 * cascades from the lib internals. */
#ifndef N_ELEMENTS
#define N_ELEMENTS(x) (sizeof((x)) / sizeof((x)[0]))
#endif
#ifndef UNUSED
#define UNUSED(x) ((void)(x))
#endif

/* FLEX_ARRAY_LEN_DECL — defined in netsurf/utils/utils.h, but if a
 * libcss/libdom/libhubbub utils.h wins on the access path it won't
 * be visible. CW8 accepts an empty flex array length. */
#ifndef FLEX_ARRAY_LEN_DECL
#define FLEX_ARRAY_LEN_DECL
#endif

/* fallthrough — defined in netsurf/utils/utils.h with C++/C2x detection,
 * but on CW8 the wrong utils.h often wins and fallthrough stays undefined.
 * Make it a harmless no-op globally. */
#ifndef fallthrough
#define fallthrough do {} while(0)
#endif

/* Duktape JS engine. WITH_DUKTAPE enables the real Duktape engine.
 * The MacSurf.mcp may have WITHOUT_DUKTAPE defined as a CW8 language
 * preprocessor override — if so that will suppress the WITH_DUKTAPE block
 * via the #ifndef guard below, keeping both control points in sync. */
#ifndef WITHOUT_DUKTAPE
#ifndef WITH_DUKTAPE
#define WITH_DUKTAPE 1
#endif
#endif

/* fixes305a: enable the file-backed diagnostic log channel by default.
 * macsurf_debug_log.c and macsurf_debug.c gate their real bodies on
 * MACSURF_DEBUG; without this define, macsurf_debug_log_init() is the
 * empty release stub at macsurf_debug_log.c:352 and every MS_LOG() call
 * compiles out, so MacSurf Debug.log never appears on the Desktop.
 * Define for everything except an explicit MACSURF_RELEASE build. */
#ifndef MACSURF_RELEASE
#ifndef MACSURF_DEBUG
#define MACSURF_DEBUG 1
#endif
#endif

/* libcss stylesheet.h once tagged a struct close with `} _ALIGNED;`.
 * If any header is still found with that token undefined, CW8 reads
 * it as a global variable declaration and every TU emits a duplicate
 * symbol at link time. Define _ALIGNED as empty so the token is
 * harmless wherever it appears. */
#ifndef _ALIGNED
#define _ALIGNED
#endif

#endif /* MACSURF_PREFIX_H */
