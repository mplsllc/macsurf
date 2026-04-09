/*
 * MacSurf stub -- shims/stat.h
 * Redirect to mac_types.h which already provides struct stat,
 * stat-related macros (S_ISDIR, S_ISREG, S_IFDIR, S_IFREG),
 * mode_t, and forward declarations for mac_stat/mac_fstat/mac_access.
 *
 * C89-compatible.  Licensed under GPL v2.
 */

#ifndef MACOS9_SHIMS_STAT_H
#define MACOS9_SHIMS_STAT_H

#include "mac_types.h"

#ifndef S_IRWXU
#define S_IRWXU  0000700
#endif
#ifndef S_IRUSR
#define S_IRUSR  0000400
#endif
#ifndef S_IWUSR
#define S_IWUSR  0000200
#endif
#ifndef S_IXUSR
#define S_IXUSR  0000100
#endif

#endif /* MACOS9_SHIMS_STAT_H */
