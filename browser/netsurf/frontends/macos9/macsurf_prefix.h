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

#define inline
#define __MACOS9__ 1
#define WITHOUT_ICONV_FILTER 1
#define NO_IPV6 1
#define PATH_MAX 256

#endif /* MACSURF_PREFIX_H */
