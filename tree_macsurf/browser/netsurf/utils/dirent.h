/*
 * Copyright 2016 Vincent Sanders <vince@netsurf-browser.org>
 *
 * This file is part of NetSurf, http://www.netsurf-browser.org/
 *
 * NetSurf is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * NetSurf is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * \file
 * directory traversal and entry
 *
 * This allows the obtaining of standard directory entry routines
 * functions in a uniform way despite any oddities in headers and
 * supported API between OS.
 *
 * \note This functionality was previously provided as a side effect of the
 *  utils config header include.
 */

#ifndef NETSURF_UTILS_DIRENT_H
#define NETSURF_UTILS_DIRENT_H

#include "utils/config.h"

#include <dirent.h>

#ifndef HAVE_SCANDIR

int alphasort(const struct dirent **d1, const struct dirent **d2);
int scandir(const char *dir, struct dirent ***namelist,
		int (*sel)(const struct dirent *),
		int (*compar)(const struct dirent **, const struct dirent **));
#endif

#endif
