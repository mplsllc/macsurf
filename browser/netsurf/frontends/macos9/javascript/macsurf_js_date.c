/*
 * MacSurf — macsurf_js_date.c
 *
 * Date provider for Duktape on Mac OS 9.  Duktape calls this to seed
 * Date.now() and related builtins with a wall-clock value.  OS 9 stores
 * time as seconds since 1904-01-01; JS wants milliseconds since Unix
 * epoch (1970-01-01).  Difference is exactly 2082844800 seconds.
 *
 * Linux cross-check path uses time(NULL) directly.
 */

#include "duktape.h"
#include "macsurf_js.h"

/* Difference between Mac epoch (1904-01-01 00:00:00 UTC) and
 * Unix epoch (1970-01-01 00:00:00 UTC) in seconds. */
#define MACSURF_MAC_EPOCH_OFFSET 2082844800UL

#ifdef __MWERKS__
#include <DateTimeUtils.h>

duk_double_t
macsurf_js_get_now(void)
{
	unsigned long secs = 0;
	GetDateTime(&secs);
	/* GetDateTime returns seconds since Mac epoch. Convert to Unix
	 * milliseconds.  Multiplication stays in duk_double_t to avoid
	 * 32-bit overflow on the intermediate product. */
	return (duk_double_t)(secs - MACSURF_MAC_EPOCH_OFFSET) * 1000.0;
}
#else
#include <time.h>

duk_double_t
macsurf_js_get_now(void)
{
	time_t t = time(NULL);
	return (duk_double_t)t * 1000.0;
}
#endif
