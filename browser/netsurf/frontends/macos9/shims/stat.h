#ifndef MACSURF_STAT_H
#define MACSURF_STAT_H

#ifdef __RETRO68__
#include <sys/stat.h>

#elif defined(__MWERKS__)
/*
 * CW8: this file is found via the macsurf shims access path before
 * MSL's own <stat.h>, so #include <stat.h> from here recurses back
 * into this same guard and yields nothing.  Define every type and
 * constant the rest of the project needs from stat.h directly,
 * matching MSL's underlying types so we don't conflict if MSL's
 * stat.h is reached through some other path later.
 */

#include <stddef.h>

#ifndef _MODE_T
#define _MODE_T
typedef unsigned long mode_t;
#endif

#ifndef _OFF_T
#define _OFF_T
typedef long off_t;
#endif

#ifndef _SSIZE_T
#define _SSIZE_T
typedef long ssize_t;
#endif

#ifndef _TIME_T
#define _TIME_T
typedef long time_t;
#endif

#ifndef _STRUCT_STAT_DEFINED
#define _STRUCT_STAT_DEFINED
struct stat {
	off_t           st_size;
	time_t          st_mtime;
	time_t          st_atime;
	time_t          st_ctime;
	unsigned long   st_mode;
	unsigned long   st_nlink;
	unsigned long   st_dev;
	unsigned long   st_ino;
	unsigned long   st_uid;
	unsigned long   st_gid;
	unsigned long   st_rdev;
};
#endif

#ifndef S_IFMT
#define S_IFMT		0170000
#endif
#ifndef S_IFREG
#define S_IFREG		0100000
#endif
#ifndef S_IFDIR
#define S_IFDIR		0040000
#endif
#ifndef S_IFLNK
#define S_IFLNK		0120000
#endif
#ifndef S_IFCHR
#define S_IFCHR		0020000
#endif
#ifndef S_IFBLK
#define S_IFBLK		0060000
#endif
#ifndef S_IFIFO
#define S_IFIFO		0010000
#endif

#ifndef S_ISREG
#define S_ISREG(m)	(((m) & S_IFMT) == S_IFREG)
#endif
#ifndef S_ISDIR
#define S_ISDIR(m)	(((m) & S_IFMT) == S_IFDIR)
#endif
#ifndef S_ISLNK
#define S_ISLNK(m)	(((m) & S_IFMT) == S_IFLNK)
#endif

extern int stat(const char *path, struct stat *buf);
extern int fstat(int fd, struct stat *buf);

#else
/* Linux cross-check / unknown host: original minimal shim. */
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

#endif /* MACSURF_STAT_H */
