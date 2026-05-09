/*
 * iconv.h — compatibility wrapper
 *
 * NetSurf core includes <iconv.h>. Redirect to mac_iconv.h which
 * provides the iconv shim for Mac OS 9.
 */
#ifndef ICONV_H
#include "mac_iconv.h"
#endif
