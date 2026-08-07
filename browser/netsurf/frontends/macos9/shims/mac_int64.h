/*
 * mac_int64.h — spelling 64-bit Toolbox arguments under both toolchains.
 *
 * Apple's MacTypes.h makes SInt64/UInt64 either a real 64-bit scalar or a
 * struct wide { SInt32 hi; UInt32 lo; }, depending on TYPE_LONGLONG, and
 * MacTypes.h itself says so:
 *
 *     "wide and UnsignedWide must always be structs for source code
 *      compatibility. On the other hand UInt64 and SInt64 can be either a
 *      struct or a long long, depending on the compiler."
 *
 * ConditionalMacros.h only sets TYPE_LONGLONG from __option(longlong), i.e.
 * only for Metrowerks. Under CodeWarrior it is therefore 1 and these types
 * are scalars; under Retro68's GCC it falls through to the generic branch,
 * TYPE_LONGLONG is 0, and they are structs. Forcing -DTYPE_LONGLONG=1 does
 * not work -- ConditionalMacros.h defines it unconditionally, so a
 * command-line value is simply overridden (verified).
 *
 * The File Manager calls in the shim layer (FSReadFork, FSWriteFork,
 * FSSetForkSize, FSGetForkSize) all take or return one of these, which is why
 * every one of them failed to compile the first time shims/ was added to the
 * Retro68 build.
 *
 * MacTypes.h points at Math64.h for this, but under TYPE_LONGLONG == 0 those
 * are real out-of-line calls resolved against CarbonLib, so using them would
 * make a compile-time type question into a link-time availability question.
 * These macros keep it at compile time and cost nothing.
 */

#ifndef MACSURF_MAC_INT64_H
#define MACSURF_MAC_INT64_H

#if TYPE_LONGLONG

#define MAC_S64_ZERO       ((SInt64)0)
#define MAC_S64_LOW(x)     ((unsigned long)(x))

#else

/* struct wide is { SInt32 hi; UInt32 lo; } on big-endian, which PPC always
 * is. C89 has no compound literals, so the zero is a named constant. */
static const SInt64 macsurf_s64_zero_ = { 0, 0 };
#define MAC_S64_ZERO       macsurf_s64_zero_
#define MAC_S64_LOW(x)     ((unsigned long)((x).lo))

#endif /* TYPE_LONGLONG */

#endif /* MACSURF_MAC_INT64_H */
