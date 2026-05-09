/*
 * MacSurf — Mac OS 9 frontend for NetSurf
 * mac_stat.h — POSIX stat/fstat/access shim using Carbon File Manager
 *
 * This file is part of MacSurf, built on the NetSurf engine.
 * Licensed under GPL v2.
 */

#ifndef MAC_STAT_H
#define MAC_STAT_H

#include "mac_types.h"

/*
 * Functions declared in mac_types.h:
 *   mac_stat, mac_fstat, mac_access
 *
 * Constants defined in mac_types.h:
 *   F_OK, R_OK, W_OK, X_OK
 */

#define MAC_EPOCH_OFFSET 2082844800L

#endif /* MAC_STAT_H */
