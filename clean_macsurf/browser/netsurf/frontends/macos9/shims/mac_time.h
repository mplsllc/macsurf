/*
 * MacSurf — Mac OS 9 frontend for NetSurf
 * mac_time.h — POSIX time function shim using Mac OS 9 DateTimeUtils
 *
 * This file is part of MacSurf, built on the NetSurf engine.
 * Licensed under GPL v2.
 */

#ifndef MAC_TIME_H
#define MAC_TIME_H

#include "mac_types.h"

/*
 * Types declared in mac_types.h:
 *   struct mac_timeval, struct mac_tm
 *
 * Functions declared in mac_types.h:
 *   mac_gettimeofday, mac_time, mac_localtime, mac_gmtime,
 *   mac_mktime, mac_strftime
 */

#define MAC_EPOCH_OFFSET 2082844800L

#endif /* MAC_TIME_H */
