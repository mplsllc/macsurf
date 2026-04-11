/*
 * MacSurf shim — shims/strings.h
 *
 * <strings.h> is the POSIX header that declares strcasecmp / strncasecmp.
 * CW8/MSL does not provide it: on the Mac those functions live in
 * <string.h> instead. Forward there so libhubbub (and any other
 * <strings.h>-using NetSurf component) compiles unchanged.
 *
 * Licensed under GPL v2.
 */

#ifndef MACOS9_SHIMS_STRINGS_H
#define MACOS9_SHIMS_STRINGS_H

#include <string.h>

#endif
