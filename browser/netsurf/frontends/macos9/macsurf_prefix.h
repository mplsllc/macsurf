/*
 * MacSurf prefix header
 */

#ifndef MACSURF_PREFIX_H
#define MACSURF_PREFIX_H

/* ============================================================
 * fixes173 — Non-fatal assertions.
 *
 * NetSurf core ships with hundreds of assert(...) calls in
 * layout, CSS cascade, libdom, hubbub, etc. With NDEBUG not
 * defined, every one of them aborts the process on failure.
 *
 * On constrained classic Mac targets that means: any modern
 * page whose DOM or CSS produces an unexpected-but-recoverable
 * box-tree state (Apple, BBC, HuffPost, Pinterest) terminates
 * MacSurf via abort() before a single pixel is drawn.
 *
 * Real shipped browsers (Mozilla, Chrome, WebKit, Classilla)
 * compile assertions out in release builds or downgrade them
 * to log-and-continue. MacSurf does the same here: override
 * the C library assert() macro to write a one-line breadcrumb
 * to the debug log instead of calling abort(). The browser
 * continues; the offending subtree may render oddly; the
 * page loads.
 *
 * Must be defined BEFORE any standard header includes <assert.h>
 * so MSL's <assert.h> never installs its own macro.
 *
 * Tradeoff: assertions that detect actual memory corruption
 * no longer halt execution. That's the same tradeoff every
 * shipping browser makes. Diagnostic builds can re-enable the
 * real assert by defining MACSURF_REAL_ASSERTS=1. */
#ifndef MACSURF_REAL_ASSERTS
#ifndef NDEBUG
#define NDEBUG 1
#endif
/* Belt-and-braces: define our own assert macro in case some TU
 * already pulled in <assert.h> before this prefix takes effect.
 * The macro evaluates the expression for side effects (some
 * NetSurf asserts do meaningful work), logs on failure if the
 * debug log is available, and returns to the caller. */
#define assert(expr) \
	((void)((expr) ? 0 : \
		(macsurf_assert_failed_(#expr, __FILE__, __LINE__), 0)))

/* Declared early so every TU can call it through the assert
 * macro. The implementation is in macsurf_debug_log.c. */
void macsurf_assert_failed_(const char *expr, const char *file, int line);
#endif

/* Block MSL C++ headers */
#define _CSTDINT
#define _CINTTYPES
#define _CSTDBOOL

/*
 * fixes951 — DECLARE THIS A CARBON BUILD, and do it BEFORE MacTypes.h.
 *
 * MacSurf has been compiling as a NON-Carbon (classic) app against Carbon
 * headers, while linking CarbonLib and calling Carbon APIs. Proven, not
 * inferred: two CW8 build errors this session showed
 * ACCESSOR_CALLS_ARE_FUNCTIONS == 0 (the Carbon accessor prototypes for
 * GetPortBitMapForCopyBits / GetPortGrafProcs / SetPortGrafProcs were
 * invisible), and ConditionalMacros.h documents that flag -- along with
 * OPAQUE_TOOLBOX_STRUCTS and OPAQUE_UPP_TYPES -- as "default: false, TRUE FOR
 * CARBON BUILDS".
 *
 * The ordering is what did it. This prefix is injected into EVERY translation
 * unit and includes <MacTypes.h> immediately below, which pulls in
 * ConditionalMacros.h; that header computes the whole Carbon triplet at that
 * moment and then guards itself. TARGET_API_MAC_CARBON was only being set
 * later, in macos9.h:27-29, by which time the decision was already made and
 * unchangeable. So every file compiled with classic struct layouts.
 *
 * On Mac OS 9 that is survivable -- CarbonLib's structures are close enough to
 * classic that the browser has shipped and worked for a year, which is exactly
 * why this went unnoticed. On Mac OS X the real Carbon structures differ, so
 * reading a window port yields garbage: an all-NULL grafProcs table
 * (measured: procs0-7 all 00000000), a GDevice whose colour table is unmapped,
 * and StdRectWithPort faulting inside CheckPortPictureRecording. Every OS X
 * crash from fixes942 through fixes949 traces back here.
 *
 * NOTE the knock-on effects, which are the point rather than a side effect:
 *   OPAQUE_TOOLBOX_STRUCTS   -> direct GrafPort/WindowRecord field access is
 *                               now illegal; use the accessors.
 *   ACCESSOR_CALLS_ARE_FUNCTIONS -> those accessors are finally declared, so
 *                               the local externs this frontend carries for
 *                               them become redundant (harmless if the
 *                               signatures match; remove once confirmed).
 *   OPAQUE_UPP_TYPES         -> UPPs become opaque. Given the fixes147 UPP /
 *                               MixedMode history, watch the scroll-bar and
 *                               event paths closely on OS 9.
 */
#ifndef TARGET_API_MAC_CARBON
#define TARGET_API_MAC_CARBON 1
#endif

/*
 * fixes951b — SUPPRESS <Endian.h> and supply its big-endian typedefs here.
 *
 * REPLACES fixes951a, which was wrong and broke the whole build (6582 errors,
 * 30 files, nothing compiled). fixes951a did "#include <Endian.h>" from this
 * prefix. That does NOT reach the SDK header:
 *
 *   browser/libparserutils/src/utils/endian.h  IS on a CW8 access path
 *   (MacSurfSource/libparserutils/src/utils), HFS is CASE-INSENSITIVE, and
 *   AlwaysSearchUserPaths=true searches USER paths BEFORE system paths.
 *
 * So every "#include <Endian.h>" in this project -- including the one inside
 * the SDK's own Events.h -- resolves to libparserutils' endian.h, which uses
 * "static inline" and is not C89/CW8-clean. Injecting it from the prefix put
 * it into every translation unit at once. This is exactly the documented
 * sys/time.h interception trap in CLAUDE.md, and it also explains the ORIGINAL
 * fixes951 cascade: libparserutils' header guards itself with
 * "parserutils_endian_h_", never __ENDIAN__, so Events.h's own
 * "#ifndef __ENDIAN__ #include <Endian.h>" fetched the wrong file, left
 * __ENDIAN__ unset, and BigEndianLong was never defined.
 *
 * Fix: claim __ENDIAN__ ourselves so no Toolbox header can pull the shadowed
 * file, then provide the seven types the SDK would have. This is the same
 * suppress-and-supply pattern macos9.h already uses for __ALIASES__,
 * __KEYCHAINCORE__ and __INTERNETCONFIG__.
 *
 * Values are the SDK's big-endian branch verbatim (PPC is big-endian, so each
 * is a plain typedef; the struct-wrapped forms are the little-endian branch
 * and cannot apply here). Nothing can depend on Endian.h's byte-swap
 * FUNCTIONS, because that header has never once been reachable in this
 * project -- the shadow has always won.
 *
 * The guard is claimed BEFORE MacTypes.h (which may include Endian.h), while
 * the typedefs come AFTER it, since Fixed/UnsignedFixed/OSType come from
 * MacTypes.h.
 */
#ifdef __MWERKS__
  #ifndef __ENDIAN__
    #define __ENDIAN__
  #endif
#endif

/* Force MacTypes.h first for true/false enum */
#ifdef __MWERKS__
  #ifndef __MACTYPES__
    #include <MacTypes.h>
  #endif
  /* fixes951b — the BigEndian* types Carbon's Events.h needs for KeyMap[4].
   * See the note above for why we define these instead of including them. */
  typedef long            BigEndianLong;
  typedef unsigned long   BigEndianUnsignedLong;
  typedef short           BigEndianShort;
  typedef unsigned short  BigEndianUnsignedShort;
  typedef Fixed           BigEndianFixed;
  typedef UnsignedFixed   BigEndianUnsignedFixed;
  typedef OSType          BigEndianOSType;
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

/* Bulletproof allocator intercept -- prevents NULL-write-to-$0000
 * crash on fragmented heaps. Must come AFTER <stdlib.h> so MSL's
 * declarations parse before the macros rename the symbols.
 * macsurf_memory.c #undefs these to call MSL directly. */
extern void *macsurf_safe_alloc(size_t size);
extern void *macsurf_safe_calloc(size_t count, size_t size);
extern void *macsurf_safe_realloc(void *ptr, size_t size);
#define malloc  macsurf_safe_alloc
#define calloc  macsurf_safe_calloc
#define realloc macsurf_safe_realloc

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
  /* Lock struct tm definition here so MSL <time.h> and our sys/time.h
   * shim can't both define it (they use different guards otherwise). */
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
  extern time_t time(time_t *);
  extern char *ctime(const time_t *);
  extern size_t strftime(char *, size_t, const char *, const struct tm *);
  #endif
  /* Block MSL <time.h> entirely — its struct tm conflicts. */
  #ifndef _TIME_H
  #define _TIME_H
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
/* Block MSL's stat.h variants — our shim defines struct stat. */
#ifndef _STAT_H
#define _STAT_H
#endif
#ifndef _STAT_H_
#define _STAT_H_
#endif
#ifndef __STAT_H__
#define __STAT_H__
#endif
#ifndef _SYS_STAT_H
#define _SYS_STAT_H
#endif
#ifndef _SYS_STAT_H_
#define _SYS_STAT_H_
#endif
#include "stat.h"
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
extern int   strcmp(const char *, const char *);
extern int   strncmp(const char *, const char *, size_t);
extern int   strcasecmp(const char *, const char *);
extern int   strncasecmp(const char *, const char *, size_t);
extern size_t strlen(const char *);
extern void *memcpy(void *, const void *, size_t);
extern void *memmove(void *, const void *, size_t);
extern void *memset(void *, int, size_t);
extern int   memcmp(const void *, const void *, size_t);

#define inline
#define __MACOS9__ 1
#define WITHOUT_ICONV_FILTER 1
#define NO_IPV6 1
#define PATH_MAX 256

/* lodepng config: we only need the in-memory PNG decoder. Strip
 * encoder, disk I/O and C++ bindings to keep the binary small. */
#define LODEPNG_NO_COMPILE_ENCODER 1
#define LODEPNG_NO_COMPILE_DISK 1
#define LODEPNG_NO_COMPILE_CPP 1

/* libhubbub/libcss/libparserutils source files include "utils/utils.h",
 * but the access path resolves to NetSurf's utils.h first, which lacks
 * N_ELEMENTS / UNUSED. Provide them globally to avoid undefined-symbol
 * cascades from the lib internals. */
#ifndef N_ELEMENTS
#define N_ELEMENTS(x) (sizeof((x)) / sizeof((x)[0]))
#endif
#ifndef NOF_ELEMENTS
#define NOF_ELEMENTS(x) (sizeof((x)) / sizeof((x)[0]))
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

/* netsurf/inttypes.h's PRI* macros sometimes don't reach every TU.
 * Provide CW8-compatible fallbacks (no z/Z modifiers — CW8 MSL printf
 * doesn't accept them; use long-equivalents). */
#ifndef PRIsizet
#define PRIsizet "lu"
#endif
#ifndef PRIssizet
#define PRIssizet "ld"
#endif
#ifndef PRId32
#define PRId32 "ld"
#endif
#ifndef PRIu32
#define PRIu32 "lu"
#endif
#ifndef PRIx32
#define PRIx32 "lx"
#endif
#ifndef PRIu16
#define PRIu16 "u"
#endif
#ifndef PRId16
#define PRId16 "d"
#endif
#ifndef PRId64
#define PRId64 "lld"
#endif
#ifndef PRIu64
#define PRIu64 "llu"
#endif
#ifndef PRIxPTR
#define PRIxPTR "lx"
#endif

/* JS engine = QuickJS (macsurf_qjs.c owns js_initialise/js_newheap/js_exec).
 * js_stub.c provides no-op stubs when WITH_QUICKJS is not defined. */
#ifndef WITH_QUICKJS
#define WITH_QUICKJS 1
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

/* fixes91: verbose per-paint plotter logs. The plot_clip writef at
 * plotters.c:243 fires for every QuickDraw clip operation, which on a
 * real Drupal page emits ~30,000 log lines per redraw — every line
 * forces an HFS FlushVol, costing 5-15% of session time. Off by default;
 * define MACSURF_VERBOSE_PLOTLOG only for layout debugging. */
/* #define MACSURF_VERBOSE_PLOTLOG 1 */

/* libcss stylesheet.h once tagged a struct close with `} _ALIGNED;`.
 * If any header is still found with that token undefined, CW8 reads
 * it as a global variable declaration and every TU emits a duplicate
 * symbol at link time. Define _ALIGNED as empty so the token is
 * harmless wherever it appears. */
#ifndef _ALIGNED
#define _ALIGNED
#endif

/* macTLS Phase 1 — pull in BearSSL's CW8 build configuration so every
 * vendored BearSSL TU compiles with the right BR_* macros (32-bit
 * arithmetic, no autodetected platform paths, no GCC/MSC builtins, no
 * OS-side entropy/time). The prefix is __MWERKS__-gated internally so
 * it's a no-op under Linux syntax-check. The `#define inline` it
 * emits is harmless duplicate of line 204 above; the BR_* defines
 * are the load-bearing part. */
#include "ostls_cw8_prefix.h"

#endif /* MACSURF_PREFIX_H */
