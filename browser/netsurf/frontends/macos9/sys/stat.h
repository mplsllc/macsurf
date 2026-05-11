/*
 * MacSurf shim — sys/stat.h
 * Inlined copy of shims/stat.h so MSL's <sys/stat.h> isn't pulled in
 * (which chains to time.h with a conflicting struct tm).
 */
#ifndef MACSURF_SYS_STAT_H
#define MACSURF_SYS_STAT_H

#ifndef MACSURF_STAT_H
#define MACSURF_STAT_H

#include <stddef.h>

typedef long off_t_;
struct stat {
    off_t_ st_size;
    long st_mtime;
    unsigned long st_mode;
};

#ifndef S_IFMT
#define S_IFMT  0xF000
#endif
#ifndef S_IFDIR
#define S_IFDIR 0x4000
#endif
#ifndef S_IFREG
#define S_IFREG 0x8000
#endif
#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#endif
#ifndef S_ISREG
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#endif
#ifndef S_IRUSR
#define S_IRUSR 0400
#endif
#ifndef S_IWUSR
#define S_IWUSR 0200
#endif
#ifndef S_IXUSR
#define S_IXUSR 0100
#endif
#ifndef S_IRWXU
#define S_IRWXU (S_IRUSR|S_IWUSR|S_IXUSR)
#endif

int stat(const char *path, struct stat *buf);
int fstat(int fd, struct stat *buf);

#endif /* MACSURF_STAT_H */

#endif /* MACSURF_SYS_STAT_H */
