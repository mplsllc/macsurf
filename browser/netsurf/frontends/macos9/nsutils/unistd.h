/*
 * MacSurf stub — nsutils/unistd.h
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

/*
 * access() + its mode bits.
 *
 * This file is reached as plain <unistd.h> (frontends/macos9/nsutils is on
 * the include path, so a bare #include <unistd.h> lands here rather than on
 * the C library's). NetSurf's utils/filepath.c calls access(path, R_OK), and
 * with neither the prototype nor R_OK declared it compiled as an implicit
 * int-returning function against an undefined constant.
 *
 * Backed by mac_access() in shims/mac_stat.c, which answers "does this path
 * resolve?" -- Classic Mac OS has no Unix permission bits, so the mode
 * argument is accepted and ignored.
 */
#ifndef F_OK
#define F_OK 0
#endif
#ifndef X_OK
#define X_OK 1
#endif
#ifndef W_OK
#define W_OK 2
#endif
#ifndef R_OK
#define R_OK 4
#endif

extern int access(const char *path, int mode);

#endif /* NSUTILS_UNISTD_H */
