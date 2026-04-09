#ifndef MACSURF_STAT_H
#define MACSURF_STAT_H
#include <MacTypes.h>
#include <time.h>
typedef unsigned short mode_t;
typedef long off_t;
typedef long dev_t;
typedef long ino_t;
typedef long uid_t;
typedef long gid_t;
struct stat {
    dev_t st_dev;
    ino_t st_ino;
    mode_t st_mode;
    short st_nlink;
    uid_t st_uid;
    gid_t st_gid;
    dev_t st_rdev;
    off_t st_size;
    time_t st_atime;
    time_t st_mtime;
    time_t st_ctime;
    long st_blksize;
    long st_blocks;
};
#endif
