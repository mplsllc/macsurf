#ifndef MACOS9_SYS_TYPES_H
#define MACOS9_SYS_TYPES_H
#ifdef __RETRO68__
/* Include the real Multiversal sys/types.h (provides pid_t, etc.) */
#include "/home/patrick/Retro68/toolchain/powerpc-apple-macos/include/sys/types.h"
#else
#include <stddef.h>
#include "shims/mac_types.h"
#endif
#endif
