#ifndef MACSURFX_NATIVE_PROBE_PREFIX_H
#define MACSURFX_NATIVE_PROBE_PREFIX_H

#define TARGET_API_MAC_CARBON 1

#ifndef __NOEXTENSIONS__
#define __NOEXTENSIONS__
#endif

#ifndef __CF_USE_FRAMEWORK_INCLUDES__
#define __CF_USE_FRAMEWORK_INCLUDES__
#endif

/*
 * CodeWarrior's mw_stdarg.h defines va_list but does not set the guard
 * expected by Tiger's stdio.h.  Both headers define the type as char * for
 * this compiler, so publish the missing guard after including stdarg.h.
 */
#include <stdarg.h>
#ifndef _VA_LIST
#define _VA_LIST
#endif

#include <Carbon/Carbon.h>

#endif
