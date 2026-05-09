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
 * BSD style timeval macros
 *
 * BSD added macros for manipulating timeval which have become standard on
 *  modern c libraries but for compatability where they are missing it is
 *  necessary to provide fallbacks.
 */

#ifndef NETSURF_UTILS_SYS_TIME_H_
#define NETSURF_UTILS_SYS_TIME_H_

#include <sys/time.h>

#ifndef timerclear
#define	timerclear(a) ((a)->tv_sec = (a)->tv_usec = 0)
#endif

#ifndef timerisset
#define	timerisset(a) ((a)->tv_sec || (a)->tv_usec)
#endif

#ifndef timeradd
#define timeradd(a, aa, result)						\
	do {								\
		(result)->tv_sec = (a)->tv_sec + (aa)->tv_sec;		\
		(result)->tv_usec = (a)->tv_usec + (aa)->tv_usec;	\
		if ((result)->tv_usec >= 1000000) {			\
			++(result)->tv_sec;				\
			(result)->tv_usec -= 1000000;			\
		}							\
	} while (0)
#endif

#ifndef timersub
#define timersub(a, aa, result)						\
	do {								\
		(result)->tv_sec = (a)->tv_sec - (aa)->tv_sec;		\
		(result)->tv_usec = (a)->tv_usec - (aa)->tv_usec;	\
		if ((result)->tv_usec < 0) {				\
			--(result)->tv_sec;				\
			(result)->tv_usec += 1000000;			\
		}							\
	} while (0)
#endif

#ifndef timercmp
#define timercmp(a, aa, cmp)						\
	(((a)->tv_sec cmp (aa)->tv_sec) ||				\
	 ((a)->tv_sec == (aa)->tv_sec && (a)->tv_usec cmp (aa)->tv_usec))
#endif

#endif
