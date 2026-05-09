/*
 * Copyright 2003 James Bursa <bursa@users.sourceforge.net>
 * Copyright 2004 John Tytgat <joty@netsurf-browser.org>
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

#ifndef NETSURF_LOG_H
#define NETSURF_LOG_H

#include <stdio.h>
#include <stdbool.h>

#include "utils/errors.h"

extern bool verbose_log;

/**
 * Ensures the FILE handle is available to write logging to.
 *
 * This is provided by the frontends if required
 */
typedef bool(nslog_ensure_t)(FILE *fptr);

/**
 * Initialise the logging system.
 *
 * Sets up everything required for logging. Processes the argv passed
 * to remove the -v switch for verbose logging. If necessary ensures
 * the output file handle is available.
 */
extern nserror nslog_init(nslog_ensure_t *ensure, int *pargc, char **argv);

/**
 * Shut down the logging system.
 *
 * Shuts down the logging subsystem, resetting to verbose logging and output
 * to stderr.  Note, if logging is done after calling this, it will be sent
 * to stderr without filtering.
 */
extern void nslog_finalise(void);

/**
 * Set the logging filter.
 *
 * Compiles and enables the given logging filter.
 */
extern nserror nslog_set_filter(const char *filter);

/**
 * Set the logging filter according to the options.
 */
extern nserror nslog_set_filter_by_options(void);

/* ensure a logging level is defined */
#ifndef NETSURF_LOG_LEVEL
#define NETSURF_LOG_LEVEL INFO
#endif

#define NSLOG_LVL(level) NSLOG_LEVEL_ ## level
#define NSLOG_EVL(level) NSLOG_LVL(level)
#define NSLOG_COMPILED_MIN_LEVEL NSLOG_EVL(NETSURF_LOG_LEVEL)

#ifdef WITH_NSLOG

#include <nslog/nslog.h>

NSLOG_DECLARE_CATEGORY(netsurf);
NSLOG_DECLARE_CATEGORY(llcache);
NSLOG_DECLARE_CATEGORY(fetch);
NSLOG_DECLARE_CATEGORY(plot);
NSLOG_DECLARE_CATEGORY(schedule);
NSLOG_DECLARE_CATEGORY(fbtk);
NSLOG_DECLARE_CATEGORY(layout);
NSLOG_DECLARE_CATEGORY(flex);
NSLOG_DECLARE_CATEGORY(dukky);
NSLOG_DECLARE_CATEGORY(jserrors);

#else /* WITH_NSLOG */

enum nslog_level {
	NSLOG_LEVEL_DEEPDEBUG = 0,
	NSLOG_LEVEL_DEBUG = 1,
	NSLOG_LEVEL_VERBOSE = 2,
	NSLOG_LEVEL_INFO = 3,
	NSLOG_LEVEL_WARNING = 4,
	NSLOG_LEVEL_ERROR = 5,
	NSLOG_LEVEL_CRITICAL = 6
};

extern void nslog_log(const char *file, const char *func, int ln, const char *format, ...) __attribute__ ((format (printf, 4, 5)));

#  ifdef __GNUC__
#    define LOG_FN __PRETTY_FUNCTION__
#    define LOG_LN __LINE__
#  elif defined(__CC_NORCROFT)
#    define LOG_FN __func__
#    define LOG_LN __LINE__
#  else
#    define LOG_FN ""
#    define LOG_LN __LINE__
#  endif

#define NSLOG(catname, level, logmsg, args...)				\
	do {								\
		if (NSLOG_LEVEL_##level >= NSLOG_COMPILED_MIN_LEVEL) {	\
			nslog_log(__FILE__, LOG_FN, LOG_LN, logmsg , ##args); \
		}							\
	} while(0)

#endif  /* WITH_NSLOG */

#endif
