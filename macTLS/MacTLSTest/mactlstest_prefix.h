/*
 * mactlstest_prefix.h
 *
 * CodeWarrior 8 project prefix for the MacTLSTest standalone Carbon
 * app. This file is injected before every translation unit in
 * MacTLSTest.mcp -- including all 253 vendored BearSSL .c files and
 * the two macTLS/os9 sources.
 *
 * Mirrors the relevant subset of MacSurf's macsurf_prefix.h, minus
 * NetSurf-specific defines. Adds the macTLS CW8 compatibility shim so
 * BearSSL builds cleanly without upstream edits.
 *
 * In the CW8 IDE: Edit -> MacTLSTest Settings -> C/C++ Language ->
 * Prefix File: mactlstest_prefix.h
 */

#ifndef MACTLSTEST_PREFIX_H
#define MACTLSTEST_PREFIX_H

/*
 * MacTypes.h must come first to lock in Apple's bool / true / false
 * definitions before any other header tries to redefine them. Same
 * discipline MacSurf uses.
 */
#include <MacTypes.h>

/*
 * Block any Threads.h include from ever processing. CoreServices.h
 * (pulled in by Carbon.h on some setups) does `#include <Threads.h>`.
 * On CW8 installations that have Java SDK headers reachable via the
 * system search path, a HotSpot JVM `Threads.h` can be found before
 * Apple's, which cascades into oobj.h / typedefs_md.h / winnt.h /
 * winbase.h errors (hundreds of them).
 *
 * Pre-define every plausible include guard so whatever `Threads.h`
 * CW8 finds is processed once-as-empty. We don't use the Thread
 * Manager anywhere in macTLS or BearSSL, so an empty Threads.h is
 * harmless. The stub typedefs below cover the small surface that
 * CoreServices.h's downstream headers reference by name.
 */
#define __THREADS__              /* Apple's Threads.h guard           */
#define _THREADS_H_              /* common JVM-era guard variant      */
#define _THREADS_H_INCLUDED      /* common JVM-era guard variant      */
#define _JAVASOFT_THREADS_H_     /* HotSpot guard variant             */
#define _SYS_THREADS_H_          /* SysV-era guard variant            */

/* Stub typedefs for the Apple Thread Manager types that downstream
 * CarbonCore headers (e.g. NSLCore.h) reference by name. Intentionally
 * minimal --- code that genuinely uses the Thread Manager will fail to
 * link, which is what we want; we have no business calling
 * YieldToAnyThread() etc. from inside macTLS. */
typedef long        ThreadID;
typedef long        ThreadStyle;
typedef short       ThreadOptions;
typedef long        ThreadState;
typedef long        ThreadTaskRef;

/* Thread Manager callback pointers. NSLCore.h uses ThreadEntryProcPtr
 * in its NSLDoServerSearchByEntity parameter list. The classic Apple
 * signature is `void *(*ThreadEntryProcPtr)(void *threadParam)`; the
 * other *ProcPtr names appear in the same Thread Manager header so we
 * stub them too. */
typedef void *      (*ThreadEntryProcPtr)(void *threadParam);
typedef void        (*ThreadSchedulerProcPtr)(void);
typedef void        (*ThreadSwitchProcPtr)(ThreadID threadBeingSwitched, void *switchProcParam);
typedef void        (*ThreadTerminationProcPtr)(ThreadID terminatedThread, void *terminationProcParam);
typedef void        (*ThreadEntryTPP)(void);
typedef ThreadEntryProcPtr        ThreadEntryUPP;
typedef ThreadSchedulerProcPtr    ThreadSchedulerUPP;
typedef ThreadSwitchProcPtr       ThreadSwitchUPP;
typedef ThreadTerminationProcPtr  ThreadTerminationUPP;

#ifndef __MACOS9__
#define __MACOS9__              1
#endif

/* CW8 needs TARGET_API_MAC_CARBON=1 before any system header sees it;
 * Retro68's multiversal headers preset it to 0. Guard the define so
 * either toolchain proceeds without redefinition warnings. */
#ifndef TARGET_API_MAC_CARBON
#define TARGET_API_MAC_CARBON   1
#endif

/* No IPv6 on classic Mac OS. */
#ifndef NO_IPV6
#define NO_IPV6                 1
#endif

/*
 * Pull in the BearSSL CW8 compatibility shim. This sets `#define inline`
 * to empty (CW8 C89 does not accept the inline keyword) and pins every
 * BR_* platform-config macro so BearSSL's autodetection cannot drag in
 * x86 / POWER8 / Linux paths.
 */
#include "ostls_cw8_prefix.h"

#endif /* MACTLSTEST_PREFIX_H */
