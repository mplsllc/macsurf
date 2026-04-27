/*
 * MacSurf shim — dirent.h
 * Self-contained POSIX directory iteration interface for CW8/Mac OS 9.
 *
 * CW8 cannot resolve relative includes from within headers found via
 * access paths, so this file is self-contained rather than including
 * mac_dirent.h (which lives in the same shims/ directory).
 */

#ifndef MACSURF_DIRENT_H
#define MACSURF_DIRENT_H

#include <stddef.h>

#ifndef NAME_MAX
#define NAME_MAX 255
#endif

/* macsurf_prefix.h includes mac_dirent.h for every TU on CW8, which defines
 * struct dirent and MAC_DIR under guard MAC_DIRENT_H.  Skip our own copies
 * if mac_dirent.h already ran; also claim MAC_DIRENT_H if we run first so
 * mac_dirent.h will skip itself and avoid the redefinition. */
#ifndef MAC_DIRENT_H
#define MAC_DIRENT_H

struct dirent {
    char d_name[NAME_MAX + 1];
};

/* Use MAC_DIR to avoid collision with libhubbub element-type.h's DIR enum.
 * NetSurf code that uses DIR * (ns_file.c) uses MAC_DIR * instead. */
typedef struct _DIR MAC_DIR;

MAC_DIR       *opendir(const char *path);
struct dirent *readdir(MAC_DIR *dir);
int            closedir(MAC_DIR *dir);
void           rewinddir(MAC_DIR *dir);

#endif /* MAC_DIRENT_H */

int alphasort(const struct dirent **d1, const struct dirent **d2);
int scandir(const char *dirp, struct dirent ***namelist,
            int (*filter)(const struct dirent *),
            int (*compar)(const struct dirent **, const struct dirent **));

#endif /* MACSURF_DIRENT_H */
