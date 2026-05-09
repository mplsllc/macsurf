/*
 * MacSurf stub — nsutils/ns_time.h
 * Minimal C89-compatible stub for CodeWarrior 8 compilation.
 *
 * nsutils_ms_t must be unsigned long long (uint64_t) to match
 * the real nsutils and callers that pass uint64_t pointers.
 */

#ifndef NSUTILS_TIME_H
#define NSUTILS_TIME_H

typedef unsigned long long nsutils_ms_t;

extern int nsu_getmonotonic_ms(nsutils_ms_t *ms);

#endif /* NSUTILS_TIME_H */
