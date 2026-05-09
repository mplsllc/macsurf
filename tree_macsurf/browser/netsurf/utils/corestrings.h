/*
 * Copyright 2012 Michael Drake <tlsa@netsurf-browser.org>
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

/** \file
 * Useful interned string pointers (interface).
 */

#ifndef NETSURF_UTILS_CORESTRINGS_H_
#define NETSURF_UTILS_CORESTRINGS_H_

#include <libwapcaplet/libwapcaplet.h>
#include "utils/errors.h"

/** File url prefix. */
#define FILE_SCHEME_PREFIX "file:///"

/** File url prefix length. */
#define FILE_SCHEME_PREFIX_LEN 8

/**
 * Initialise the core string tables
 *
 * \return NSERROR_OK on success else appropriate error code
 */
nserror corestrings_init(void);

/**
 * free resources of core string tables.
 *
 * \return NSERROR_OK on success else appropriate error code
 */
nserror corestrings_fini(void);

struct dom_string;

/* declare corestrings */
#define CORESTRING_LWC_VALUE(NAME,VALUE)		\
	extern lwc_string *corestring_lwc_##NAME
#define CORESTRING_DOM_VALUE(NAME,VALUE)		\
	extern struct dom_string *corestring_dom_##NAME
#define CORESTRING_NSURL(NAME,VALUE) \
	extern struct nsurl *corestring_nsurl_##NAME
#include "utils/corestringlist.h"
#undef CORESTRING_LWC_VALUE
#undef CORESTRING_DOM_VALUE
#undef CORESTRING_NSURL

#endif
