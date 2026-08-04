#ifndef MACOS9_SHIMS_STDINT_H
#define MACOS9_SHIMS_STDINT_H
#ifdef __RETRO68__
/* Use GCC's real <stdint.h> */
#include "/home/patrick/Retro68/toolchain/lib/gcc/powerpc-apple-macos/12.2.0/include/stdint.h"
#else
/* CW8/MSL path unchanged */
#define _MSL_STDINT_H
#define _STDINT_H
#define __STDINT_H__
#define _STDINT
#if !defined(_INT8_T) && !defined(__int8_t_defined)
#define _INT8_T
#define __int8_t_defined
typedef signed char int8_t;
#endif
#if !defined(_INT16_T) && !defined(__int16_t_defined)
#define _INT16_T
#define __int16_t_defined
typedef signed short int16_t;
#endif
#if !defined(_INT32_T) && !defined(__int32_t_defined)
#define _INT32_T
#define __int32_t_defined
typedef signed long int32_t;
#endif
#if !defined(_UINT8_T) && !defined(__uint8_t_defined)
#define _UINT8_T
#define __uint8_t_defined
typedef unsigned char uint8_t;
#endif
#if !defined(_UINT16_T) && !defined(__uint16_t_defined)
#define _UINT16_T
#define __uint16_t_defined
typedef unsigned short uint16_t;
#endif
#if !defined(_UINT32_T) && !defined(__uint32_t_defined)
#define _UINT32_T
#define __uint32_t_defined
typedef unsigned long uint32_t;
#endif
#endif /* __RETRO68__ */
#endif
