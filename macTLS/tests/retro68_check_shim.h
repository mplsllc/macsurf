/*
 * retro68_check_shim.h -- Linux pre-flight ONLY. NOT shipped, NOT built.
 *
 * Retro68's "multiversal" CIncludes are a leaner clone of Apple's
 * Universal Interfaces and omit a few types that CodeWarrior 8's real
 * Universal Interfaces (the actual target compiler) provides. That makes a
 * bare `powerpc-apple-macos-gcc -fsyntax-only` throw false errors on code
 * that compiles cleanly under CW8. Force-include this with `-include` so
 * the pre-flight syntax check exercises the real code instead of tripping
 * on header-vintage gaps:
 *
 *   powerpc-apple-macos-gcc -std=c89 -pedantic-errors -Wall -Wno-long-long \
 *     -Wno-unused -Dinline= -fsyntax-only -include tests/retro68_check_shim.h \
 *     -Ibearssl/inc -Ios9 -IMacTLSTest <file>.c
 *
 * SCOPE: use this ONLY when syntax-checking the CW8-only harness
 * (MacTLSTest/main.c), whose prefix defines __MWERKS__ and so routes
 * ostls_async.h to <MacTypes.h> (which on Retro68 lacks OSStatus). Do NOT
 * apply it to the os9/ LIBRARY files: those take ostls_async.h's portable
 * else-branch, which already typedefs OSStatus itself, so the shim would
 * collide. Check os9/*.c bare; check MacTLSTest/main.c with this shim.
 */
#ifndef RETRO68_CHECK_SHIM_H
#define RETRO68_CHECK_SHIM_H

/* OSStatus: SInt32 on classic Mac (Carbon). Present in CW8's MacTypes.h,
 * absent from Retro68's. Used by ostls_async.h and MacTLSTest/main.c. */
#ifndef __OSStatus_shimmed__
#define __OSStatus_shimmed__
typedef long OSStatus;
#endif

#endif /* RETRO68_CHECK_SHIM_H */
