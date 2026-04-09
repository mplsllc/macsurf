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

/* Exact-width signed integer types */
#ifndef _INT8_T
#define _INT8_T
typedef signed char        int8_t;
#endif

#ifndef _INT16_T
#define _INT16_T
typedef short              int16_t;
#endif

#ifndef _INT32_T
#define _INT32_T
typedef long               int32_t;
#endif

#ifndef _INT64_T
#define _INT64_T
typedef long long          int64_t;
#endif

/* Exact-width unsigned integer types */
#ifndef _UINT8_T
#define _UINT8_T
typedef unsigned char      uint8_t;
#endif

#ifndef _UINT16_T
#define _UINT16_T
typedef unsigned short     uint16_t;
#endif

#ifndef _UINT32_T
#define _UINT32_T
typedef unsigned long      uint32_t;
#endif

#ifndef _UINT64_T
#define _UINT64_T
typedef unsigned long long uint64_t;
#endif

/* Pointer-sized integer types */
#ifndef _UINTPTR_T
#define _UINTPTR_T
typedef unsigned long      uintptr_t;
#endif

#ifndef _INTPTR_T
#define _INTPTR_T
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

#endif /* MACOS9_SHIMS_STDINT_H */
