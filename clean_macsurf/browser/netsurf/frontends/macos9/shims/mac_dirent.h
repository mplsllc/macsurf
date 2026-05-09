/*
 * MacSurf — Mac OS 9 frontend for NetSurf
 * mac_dirent.h — POSIX directory iteration shim using Carbon File Manager
 *
 * This file is part of MacSurf, built on the NetSurf engine.
 * Licensed under GPL v2.
 */

#ifndef MAC_DIRENT_H
#define MAC_DIRENT_H

#include <stddef.h>
#include "mac_types.h"

#define NAME_MAX 255

/* Standard POSIX dirent interface */
struct dirent {
    char d_name[NAME_MAX + 1];
};

typedef struct _DIR DIR;

DIR    *opendir(const char *path);
struct dirent *readdir(DIR *dir);
int     closedir(DIR *dir);
void    rewinddir(DIR *dir);

#endif /* MAC_DIRENT_H */
