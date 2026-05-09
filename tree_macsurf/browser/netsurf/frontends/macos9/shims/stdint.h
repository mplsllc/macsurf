/*
 * MacSurf stub -- shims/stdint.h
 * C89-compatible integer type definitions for CodeWarrior 8.
 *
 * MSL's <stdint.h> pulls in <cstdint> (a C++ header) which causes
 * "illegal name overloading" errors when compiling C code.
 * This shim provides the needed types directly without the C++ chain.
 *
 * Licensed under GPL v2.
 */

#ifndef MACOS9_SHIMS_STDINT_H
#define MACOS9_SHIMS_STDINT_H

/*
 * Block MSL's stdint.h from being included after us.
 * MSL's version pulls in <cstdint> (C++) causing name overloading errors.
 */
#define _MSL_STDINT_H
#define _STDINT_H
#define __STDINT_H__
#define _STDINT

/* Exact-width signed integer types.
 * Guard against both our own guards and CW8/MSL/MacTypes.h
 * alternatives to prevent "illegal name overloading" errors. */
#if !defined(_INT8_T) && !defined(__int8_t_defined)
#define _INT8_T
#define __int8_t_defined
typedef signed char        int8_t;
#endif

#if !defined(_INT16_T) && !defined(__int16_t_defined)
#define _INT16_T
#define __int16_t_defined
typedef short              int16_t;
#endif

#if !defined(_INT32_T) && !defined(__int32_t_defined)
#define _INT32_T
#define __int32_t_defined
typedef int                int32_t;
#endif

#if !defined(_INT64_T) && !defined(__int64_t_defined)
#define _INT64_T
#define __int64_t_defined
typedef long long          int64_t;
#endif

/* Exact-width unsigned integer types */
#if !defined(_UINT8_T) && !defined(__uint8_t_defined)
#define _UINT8_T
#define __uint8_t_defined
typedef unsigned char      uint8_t;
#endif

#if !defined(_UINT16_T) && !defined(__uint16_t_defined)
#define _UINT16_T
#define __uint16_t_defined
typedef unsigned short     uint16_t;
#endif

#if !defined(_UINT32_T) && !defined(__uint32_t_defined)
#define _UINT32_T
#define __uint32_t_defined
typedef unsigned int       uint32_t;
#endif

#if !defined(_UINT64_T) && !defined(__uint64_t_defined)
#define _UINT64_T
#define __uint64_t_defined
typedef unsigned long long uint64_t;
#endif

/* Pointer-sized integer types */
#if !defined(_UINTPTR_T) && !defined(__uintptr_t_defined)
#define _UINTPTR_T
#define __uintptr_t_defined
typedef unsigned long      uintptr_t;
#endif

#if !defined(_INTPTR_T) && !defined(__intptr_t_defined)
#define _INTPTR_T
#define __intptr_t_defined
typedef long               intptr_t;
#endif

/* Limits */
#ifndef SIZE_MAX
#define SIZE_MAX  ((size_t)-1)
#endif

#ifndef UINT8_MAX
#define UINT8_MAX  255U
#endif

#ifndef UINT16_MAX
#define UINT16_MAX 65535U
#endif

#ifndef UINT32_MAX
#define UINT32_MAX 4294967295UL
#endif

#ifndef UINT64_MAX
#define UINT64_MAX 18446744073709551615ULL
#endif

#ifndef INT32_MAX
#define INT32_MAX  2147483647L
#endif

#ifndef INT32_MIN
#define INT32_MIN  (-2147483647L - 1)
#endif

#ifndef INT64_MAX
#define INT64_MAX  9223372036854775807LL
#endif

/* Format macros for printf — C89 compatible */
#ifndef PRId32
#define PRId32  "ld"
#endif

#ifndef PRIu32
#define PRIu32  "lu"
#endif

#ifndef PRIx32
#define PRIx32  "lx"
#endif

#ifndef PRId64
#define PRId64  "lld"
#endif

#ifndef PRIu64
#define PRIu64  "llu"
#endif

#ifndef PRIx64
#define PRIx64  "llx"
#endif

#ifndef PRIX32
#define PRIX32  "lX"
#endif

#ifndef SCNx32
#define SCNx32  "lx"
#endif

#ifndef SCNd32
#define SCNd32  "ld"
#endif

#ifndef SCNu32
#define SCNu32  "lu"
#endif

#endif /* MACOS9_SHIMS_STDINT_H */
