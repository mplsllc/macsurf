#ifndef MACSURF_STAT_H
#define MACSURF_STAT_H

#ifdef __RETRO68__
#include <sys/stat.h>
#else
/* Original shim content */
#include <stddef.h>
#include "mac_types.h"
typedef long off_t;
struct stat {
    off_t st_size;
    long st_mtime;
};
int stat(const char *path, struct stat *buf);
int fstat(int fd, struct stat *buf);
#endif

#endif
