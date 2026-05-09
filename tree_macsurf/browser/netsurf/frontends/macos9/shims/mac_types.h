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

/*
 * Type guards: use both our own and MSL-compatible guards
 * to prevent redefinition when MSL stat.h or other headers
 * have already defined these types.
 */
/*
 * Match MSL's types exactly to avoid redeclaration errors.
 * MSL stat.h defines: mode_t = unsigned long, off_t = long.
 */
#if !defined(_MAC_OFF_T) && !defined(_OFF_T_DEFINED) && !defined(_OFF_T)
#define _MAC_OFF_T
#define _OFF_T
typedef long		off_t;
#endif

#if !defined(_MAC_SSIZE_T) && !defined(_SSIZE_T_DEFINED) && !defined(_SSIZE_T)
#define _MAC_SSIZE_T
#define _SSIZE_T
typedef long		ssize_t;
#endif

/* mode_t and time_t are provided by MSL's <stat.h> and <time.h>
 * which are included globally via the prefix file.
 * Do NOT redefine them here — causes "illegal name overloading". */

/* FSIORefNum — provided by Files.h in Carbon, but we may be parsed
 * before Files.h is included.  Use short (== SInt16 on PPC). */
#ifndef __FILES__
typedef short		FSIORefNum;
#endif

/*
 * fcntl.h constants (O_RDONLY, O_WRONLY, etc.) and
 * sys/stat.h constants (S_IFDIR, S_IFREG, struct stat, etc.)
 * are intentionally NOT defined here — MSL provides them.
 * Defining them here conflicts with MSL's fcntl.h and stat.h.
 */

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

/*
 * struct stat is provided by MSL's stat.h — not defined here.
 * On CW8, #include <stat.h> in the prefix file provides it.
 * On Linux, pull in the system header so struct stat is visible.
 */
#ifndef __MWERKS__
#include <sys/stat.h>
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
