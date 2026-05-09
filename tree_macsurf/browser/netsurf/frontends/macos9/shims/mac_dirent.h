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

/* Use MAC_DIR to avoid collision with libhubbub's element-type.h
 * enum which defines DIR as an HTML element type constant.
 * NetSurf code that uses DIR * (file.c, utils.c) is patched to
 * use MAC_DIR * instead. */
typedef struct _DIR MAC_DIR;

MAC_DIR    *opendir(const char *path);
struct dirent *readdir(MAC_DIR *dir);
int     closedir(MAC_DIR *dir);
void    rewinddir(MAC_DIR *dir);

#endif /* MAC_DIRENT_H */
