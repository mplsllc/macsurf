/*
 * MacSurf — Mac OS 9 frontend for NetSurf
 * mac_types.h — POSIX type definitions and forward declarations
 *
 * This file is part of MacSurf, built on the NetSurf engine.
 * Licensed under GPL v2.
 */

#ifndef MAC_TYPES_H
#define MAC_TYPES_H

#include <stddef.h>

/* --- sys/types.h replacements ---
 * Always define these types so the header is self-contained.
 * On Mac OS 9 these replace the missing POSIX headers;
 * on a Linux cross-check they shadow the system types harmlessly.
 */

#ifndef _MAC_OFF_T
#define _MAC_OFF_T
typedef long long	off_t;
#endif

#ifndef _MAC_SSIZE_T
#define _MAC_SSIZE_T
typedef long		ssize_t;
#endif

#ifndef _MAC_MODE_T
#define _MAC_MODE_T
typedef unsigned short	mode_t;
#endif

#ifndef _MAC_TIME_T
#define _MAC_TIME_T
typedef long		time_t;
#endif

/* FSIORefNum — provided by Files.h in Carbon, but we may be parsed
 * before Files.h is included.  Use short (== SInt16 on PPC). */
#ifndef __FILES__
typedef short		FSIORefNum;
#endif

/* --- fcntl.h constants --- */

#ifndef O_RDONLY
#define O_RDONLY	0
#endif
#ifndef O_WRONLY
#define O_WRONLY	1
#endif
#ifndef O_RDWR
#define O_RDWR		2
#endif
#ifndef O_CREAT
#define O_CREAT		0x0200
#endif
#ifndef O_TRUNC
#define O_TRUNC		0x0400
#endif

/* --- sys/stat.h constants --- */

#ifndef S_IFDIR
#define S_IFDIR		0040000
#endif
#ifndef S_IFREG
#define S_IFREG		0100000
#endif
#ifndef S_ISDIR
#define S_ISDIR(m)	(((m) & 0170000) == S_IFDIR)
#endif
#ifndef S_ISREG
#define S_ISREG(m)	(((m) & 0170000) == S_IFREG)
#endif

/* --- errno values --- */

#ifndef EINVAL
#define EINVAL		22
#endif
#ifndef E2BIG
#define E2BIG		7
#endif
#ifndef EILSEQ
#define EILSEQ		84
#endif

/* --- access() mode constants --- */

#ifndef F_OK
#define F_OK		0
#endif
#ifndef R_OK
#define R_OK		4
#endif
#ifndef W_OK
#define W_OK		2
#endif
#ifndef X_OK
#define X_OK		1
#endif

/* --- struct stat (minimal) --- */

#ifndef _MAC_STRUCT_STAT
#define _MAC_STRUCT_STAT
struct stat {
	mode_t	st_mode;
	off_t	st_size;
	long	st_mtime;
};
#endif

/* --- Forward declarations: mac_file_io.c --- */

int	mac_open(const char *path, int flags, ...);
int	mac_close(int fd);
ssize_t	mac_read(int fd, void *buf, size_t count);
ssize_t	mac_write(int fd, const void *buf, size_t count);
int	mac_unlink(const char *path);
int	mac_fd_get_refnum(int fd, void *out_refnum, void *out_fsref);

/* --- Forward declarations: mac_stat.c --- */

int	mac_stat(const char *path, struct stat *buf);
int	mac_fstat(int fd, struct stat *buf);
int	mac_access(const char *path, int mode);

/* --- Forward declarations: mac_dirent.c --- */

typedef struct mac_dir mac_DIR;

struct mac_dirent {
	char		d_name[256];
	unsigned char	d_type;
};

#ifndef DT_DIR
#define DT_DIR		4
#endif
#ifndef DT_REG
#define DT_REG		8
#endif

mac_DIR		*mac_opendir(const char *path);
struct mac_dirent *mac_readdir(mac_DIR *dir);
int		mac_closedir(mac_DIR *dir);

/* --- Forward declarations: mac_time.c --- */

struct mac_timeval {
	long tv_sec;
	long tv_usec;
};

struct mac_tm {
	int tm_sec;
	int tm_min;
	int tm_hour;
	int tm_mday;
	int tm_mon;
	int tm_year;
	int tm_wday;
	int tm_yday;
	int tm_isdst;
};

int		mac_gettimeofday(struct mac_timeval *tv, void *tz);
long		mac_time(long *t);
struct mac_tm	*mac_localtime(const long *timep);
struct mac_tm	*mac_gmtime(const long *timep);
long		mac_mktime(struct mac_tm *tm);
size_t		mac_strftime(char *s, size_t max, const char *fmt,
			     const struct mac_tm *tm);

/* --- bool compatibility ---
 * MacTypes.h provides:  enum { false = 0, true = 1 };
 * We must NOT #define true/false as macros or the enum breaks.
 * Force-include MacTypes.h first so true/false are already
 * available as enum constants, then only typedef bool.
 */
#ifdef __MWERKS__
#ifndef __MACTYPES__
#include <MacTypes.h>
#endif
#endif

#ifndef bool
typedef unsigned char bool;
#endif

/* On non-Mac hosts (Linux cross-check), provide true/false if
 * MacTypes.h was not pulled in. */
#ifndef __MACTYPES__
#ifndef true
#define true  1
#endif
#ifndef false
#define false 0
#endif
#endif

#endif /* MAC_TYPES_H */
