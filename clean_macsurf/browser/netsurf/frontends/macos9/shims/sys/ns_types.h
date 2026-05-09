/*
 * MacSurf shim — shims/sys/ns_types.h
 *
 * Forwards to the existing frontends/macos9/sys/ns_types.h shim, which
 * provides off_t/ssize_t/mode_t/time_t via mac_types.h.
 *
 * Reachable via the shims/ search path. Prevents the Linux cross-check
 * from finding glibc's sys/ns_types.h, which pulls in bits/stdint-intn.h
 * and conflicts with our shims/stdint.h typedefs.
 *
 * Licensed under GPL v2.
 */
#ifndef MACOS9_SHIMS_SYS_TYPES_H
#define MACOS9_SHIMS_SYS_TYPES_H

#include <stddef.h>
#include "mac_types.h"

#endif
