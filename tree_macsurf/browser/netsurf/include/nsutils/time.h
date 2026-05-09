/*
 * Stub nsutils/time.h for Mac OS 9 syntax checking.
 * Real libnsutils is not yet built for this target.
 */

#ifndef NSUTILS_TIME_H
#define NSUTILS_TIME_H

#include <stdint.h>

typedef uint64_t nsutils_ms_t;

static inline int nsu_getmonotonic_ms(nsutils_ms_t *ms)
{
	*ms = 0;
	return 0;
}

#endif
