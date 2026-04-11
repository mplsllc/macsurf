#ifndef MACSURF_INTTYPES_H
#define MACSURF_INTTYPES_H

/* On CW8 the prefix file provides the integer types globally and predefines
 * _STDINT_H to block MSL's C++ cstdint chain. On Linux (gcc cross-check) the
 * prefix isn't injected, so we pull in our own stdint shim, which provides
 * the same int8_t..uint64_t types via plain C. Either way the types are
 * defined by the time libparserutils' headers get to use them. */
#include "stdint.h"

/* printf format macros — guarded to avoid redefinition */
#ifndef PRId16
#define PRId16 "d"
#endif
#ifndef PRIu16
#define PRIu16 "u"
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
#ifndef PRIX32
#define PRIX32 "lX"
#endif
#ifndef PRId64
#define PRId64 "lld"
#endif
#ifndef PRIu64
#define PRIu64 "llu"
#endif
#ifndef PRIx64
#define PRIx64 "llx"
#endif
#ifndef PRIxPTR
#define PRIxPTR "lx"
#endif

/* size_t/ssize_t format macros — CW8 PPC uses unsigned long for size_t */
#ifndef PRIsizet
#define PRIsizet "lu"
#endif
#ifndef PRIssizet
#define PRIssizet "ld"
#endif

/* scanf format macros (needed by nsoption.c) */
#ifndef SCNx32
#define SCNx32 "lx"
#endif
#ifndef SCNd32
#define SCNd32 "ld"
#endif
#ifndef SCNu32
#define SCNu32 "lu"
#endif

#endif
