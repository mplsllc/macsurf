/*
 * Copyright 2017 Vincent Sanders <vince@netsurf-browser.org>
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
 * Netsurf additional integer type formatting macros.
 */

#ifndef NETSURF_INTTYPES_H
#define NETSURF_INTTYPES_H

#include <inttypes.h>

#ifndef PRIxPTR
#define PRIxPTR "x"
#endif

#ifndef PRId64
#define PRId64 "lld"
#endif

#ifndef PRIu64
#define PRIu64 "llu"
#endif

/* Windows does not have sizet formating codes */
#if defined(_WIN32)

/** windows printf formatting for size_t type */
#define PRIsizet "Iu"

/** windows printf formatting for ssize_t type */
#define PRIssizet "Id"

#else

/** c99 standard printf formatting for size_t type */
#ifndef PRIsizet
#define PRIsizet "zu"
#endif

#if defined(__riscos__)
/** riscos/unixlib defines ssize_t as a long int */
#ifndef PRIssizet
#define PRIssizet "ld"
#endif
#else
/** c99 standard printf formatting for ssize_t type */
#ifndef PRIssizet
#define PRIssizet "zd"
#endif
#endif

#endif



#endif

