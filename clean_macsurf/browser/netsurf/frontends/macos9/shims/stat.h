#ifndef MACSURF_SHIMS_STAT_H
#define MACSURF_SHIMS_STAT_H

/*
 * Block MSL's stat.h from ever being included after us.
 * MSL uses one of these guards depending on version.
 */
#define _MSL_STAT_H
#define __stat_h
#define _STAT_H

typedef unsigned long dev_t;
typedef unsigned long ino_t;
typedef short nlink_t;
typedef unsigned long uid_t;
typedef unsigned long gid_t;
typedef long off_t;

struct stat {
    dev_t st_dev;
    ino_t st_ino;
    unsigned long st_mode;
    nlink_t st_nlink;
    uid_t st_uid;
    gid_t st_gid;
    dev_t st_rdev;
    off_t st_size;
    long st_atime;
    long st_mtime;
    long st_ctime;
    long st_blksize;
    long st_blocks;
};

/* Use MSL's hex values to match exactly if MSL somehow loads */
#ifndef S_IFMT
#define S_IFMT   0xF000
#endif
#ifndef S_IFDIR
#define S_IFDIR  0x4000
#endif
#ifndef S_IFCHR
#define S_IFCHR  0x2000
#endif
#ifndef S_IFBLK
#define S_IFBLK  0x6000
#endif
#ifndef S_IFREG
#define S_IFREG  0x8000
#endif
#ifndef S_IFIFO
#define S_IFIFO  0x1000
#endif
#ifndef S_IFLNK
#define S_IFLNK  0xA000
#endif
#ifndef S_IFSOCK
#define S_IFSOCK 0xE000
#endif

#ifndef S_ISDIR
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#endif
#ifndef S_ISREG
#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#endif

/* User permission bits — MSL doesn't provide these */
#ifndef S_IRUSR
#define S_IRUSR  0x0100
#endif
#ifndef S_IWUSR
#define S_IWUSR  0x0080
#endif
#ifndef S_IXUSR
#define S_IXUSR  0x0040
#endif
#ifndef S_IRWXU
#define S_IRWXU  (S_IRUSR | S_IWUSR | S_IXUSR)
#endif

int stat(const char *path, struct stat *buf);
int mkdir(const char *path, unsigned long mode);

#endif /* MACSURF_SHIMS_STAT_H */
