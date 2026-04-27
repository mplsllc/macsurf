/*
 * MacSurf stub — nsutils/ns_unistd.h
 * Minimal C89-compatible stub for CodeWarrior 8 compilation.
 *
 * Symbols stubbed:
 *   funcs: nsu_pread, nsu_pwrite
 */

#ifndef NSUTILS_UNISTD_H
#define NSUTILS_UNISTD_H

#include <stddef.h>

extern long nsu_pread(int fd, void *buf, size_t count, long offset);
extern long nsu_pwrite(int fd, const void *buf, size_t count, long offset);

#endif /* NSUTILS_UNISTD_H */
