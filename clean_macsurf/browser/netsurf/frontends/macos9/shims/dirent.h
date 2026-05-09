#ifndef MACSURF_DIRENT_H
#define MACSURF_DIRENT_H

/* Redirect to mac_dirent.h which provides the canonical definitions */
#include "mac_dirent.h"

int alphasort(const struct dirent **d1, const struct dirent **d2);
int scandir(const char *dirp, struct dirent ***namelist,
  int (*filter)(const struct dirent *),
  int (*compar)(const struct dirent **, const struct dirent **));

#endif
