/*
 * MacSurf stub -- sys/ns_types.h
 * Redirect to mac_types.h which already provides:
 *   off_t, ssize_t, mode_t, time_t, size_t (via stddef.h).
 *
 * C89-compatible.  Licensed under GPL v2.
 */

#ifndef MACOS9_SYS_TYPES_H
#define MACOS9_SYS_TYPES_H

#include <stddef.h>

/*
 * mac_types.h already defines off_t, ssize_t, mode_t, time_t.
 * Pull it in so any file that does
 *   #include <sys/ns_types.h>
 * gets the right typedefs on Mac OS 9.
 *
 * On POSIX hosts this stub is never on the include path,
 * so the real sys/ns_types.h is found instead.
 */
#include "shims/mac_types.h"

#endif /* MACOS9_SYS_TYPES_H */
