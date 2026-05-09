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

#include <stdarg.h>
#include <stdio.h>

#include "utils/config.h"
#include "utils/nsoption.h"
#include "utils/sys_time.h"
#include "utils/utsname.h"
#include "desktop/version.h"

#include "utils/log.h"

/** flag to enable verbose logging */
unsigned char verbose_log = false;

/** The stream to which logging is sent */
static FILE *logfile;

/** Subtract the `struct timeval' values X and Y
 *
 * \param result The timeval structure to store the result in
 * \param x The first value
 * \param y The second value
 * \return 1 if the difference is negative, otherwise 0.
 */
static int
timeval_subtract(struct timeval *result, struct timeval *x, struct timeval *y)
{
	/* Perform the carry for the later subtraction by updating y. */
	if (x->tv_usec < y->tv_usec) {
		int nsec = (int)(y->tv_usec - x->tv_usec) / 1000000 + 1;
		y->tv_usec -= 1000000 * nsec;
		y->tv_sec += nsec;
	}
	if ((int)(x->tv_usec - y->tv_usec) > 1000000) {
		int nsec = (int)(x->tv_usec - y->tv_usec) / 1000000;
		y->tv_usec += 1000000 * nsec;
		y->tv_sec -= nsec;
	}

	/* Compute the time remaining to wait.
	   tv_usec is certainly positive. */
	result->tv_sec = x->tv_sec - y->tv_sec;
	result->tv_usec = x->tv_usec - y->tv_usec;

	/* Return 1 if result is negative. */
	return x->tv_sec < y->tv_sec;
}

/**
 * Obtain a formatted string suitable for prepending to a log message
 *
 * \return formatted string of the time since first log call
 */
static const char *nslog_gettime(void)
{
	static struct timeval start_tv;
	static char buff[32];

	struct timeval tv;
	struct timeval now_tv;

	if (!timerisset(&start_tv)) {
		gettimeofday(&start_tv, NULL);
	}
	gettimeofday(&now_tv, NULL);

	timeval_subtract(&tv, &now_tv, &start_tv);

	snprintf(buff, sizeof(buff),"(%ld.%06ld)",
		 (long)tv.tv_sec, (long)tv.tv_usec);

	return buff;
}

#ifdef WITH_NSLOG

NSLOG_DEFINE_CATEGORY(netsurf, "NetSurf default logging");
NSLOG_DEFINE_CATEGORY(llcache, "Low level cache");
NSLOG_DEFINE_CATEGORY(fetch, "Object fetching");
NSLOG_DEFINE_CATEGORY(plot, "Rendering system");
NSLOG_DEFINE_CATEGORY(schedule, "Scheduler");
NSLOG_DEFINE_CATEGORY(fbtk, "Framebuffer toolkit");
NSLOG_DEFINE_CATEGORY(layout, "Layout");
NSLOG_DEFINE_CATEGORY(flex, "Flex");
NSLOG_DEFINE_CATEGORY(dukky, "Duktape JavaScript Binding");
NSLOG_DEFINE_CATEGORY(jserrors, "JavaScript error messages");

static void
netsurf_render_log(void *_ctx,
		   nslog_entry_context_t *ctx,
		   const char *fmt,
		   va_list args)
{
	fprintf(logfile,
		"%s [%s %.*s] %.*s:%i %.*s: ",
		nslog_gettime(),
		nslog_short_level_name(ctx->level),
		ctx->category->namelen,
		ctx->category->name,
		ctx->filenamelen,
		ctx->filename,
		ctx->lineno,
		ctx->funcnamelen,
		ctx->funcname);

	vfprintf(logfile, fmt, args);

	/* Log entries aren't newline terminated add one for clarity */
	fputc('\n', logfile);
}

/* exported interface documented in utils/log.h */
nserror
nslog_set_filter(const char *filter)
{
	nslog_error err;
	nslog_filter_t *filt = NULL;

	err = nslog_filter_from_text(filter, &filt);
	if (err != NSLOG_NO_ERROR) {
		if (err == NSLOG_NO_MEMORY)
			return NSERROR_NOMEM;
		else
			return NSERROR_INVALID;
	}

	err = nslog_filter_set_active(filt, NULL);
	filt = nslog_filter_unref(filt);
	if (err != NSLOG_NO_ERROR) {
		return NSERROR_NOSPACE;
	}

	return NSERROR_OK;
}

#else

void
nslog_log(const char *file, const char *func, int ln, const char *format, ...)
{
	va_list ap;

	if (verbose_log) {
		fprintf(logfile,
			"%s %s:%i %s: ",
			nslog_gettime(),
			file,
			ln,
			func);

		va_start(ap, format);

		vfprintf(logfile, format, ap);

		va_end(ap);

		fputc('\n', logfile);
	}
}

/* exported interface documented in utils/log.h */
nserror
nslog_set_filter(const char *filter)
{
	(void)(filter);
	return NSERROR_OK;
}


#endif

nserror nslog_init(nslog_ensure_t *ensure, int *pargc, char **argv)
{
	struct utsname utsname;
	nserror ret = NSERROR_OK;

	if (((*pargc) > 1) &&
	    (argv[1][0] == '-') &&
	    (argv[1][1] == 'v') &&
	    (argv[1][2] == 0)) {
		int argcmv;

		/* verbose logging to stderr */
		logfile = stderr;

		/* remove -v from argv list */
		for (argcmv = 2; argcmv < (*pargc); argcmv++) {
			argv[argcmv - 1] = argv[argcmv];
		}
		(*pargc)--;

		/* ensure we actually show logging */
		verbose_log = true;
	} else if (((*pargc) > 2) &&
		   (argv[1][0] == '-') &&
		   (argv[1][1] == 'V') &&
		   (argv[1][2] == 0)) {
		int argcmv;

		/* verbose logging to file */
		logfile = fopen(argv[2], "a+");

		/* remove -V and filename from argv list */
		for (argcmv = 3; argcmv < (*pargc); argcmv++) {
			argv[argcmv - 2] = argv[argcmv];
		}
		(*pargc) -= 2;

		if (logfile == NULL) {
			/* could not open log file for output */
			ret = NSERROR_NOT_FOUND;
			verbose_log = false;
		} else {

			/* ensure we actually show logging */
			verbose_log = true;
		}
	} else {
		/* default is logging to stderr */
		logfile = stderr;
	}

	/* ensure output file handle is correctly configured */
	if ((verbose_log == true) &&
	    (ensure != NULL) &&
	    (ensure(logfile) == false)) {
		/* failed to ensure output configuration */
		ret = NSERROR_INIT_FAILED;
		verbose_log = false;
	}

#ifdef WITH_NSLOG

	if (nslog_set_filter(verbose_log ?
			     NETSURF_BUILTIN_VERBOSE_FILTER :
			     NETSURF_BUILTIN_LOG_FILTER) != NSERROR_OK) {
		ret = NSERROR_INIT_FAILED;
		verbose_log = false;
	} else if (nslog_set_render_callback(netsurf_render_log, NULL) != NSLOG_NO_ERROR) {
		ret = NSERROR_INIT_FAILED;
		verbose_log = false;
	} else if (nslog_uncork() != NSLOG_NO_ERROR) {
		ret = NSERROR_INIT_FAILED;
		verbose_log = false;
	}

#endif

	/* sucessfull logging initialisation so log system info */
	if (ret == NSERROR_OK) {
		nslog_log(__FILE__, "", __LINE__, "NetSurf version '%s'", netsurf_version);
		if (uname(&utsname) < 0) {
			NSLOG(netsurf, INFO,
			      "Failed to extract machine information");
		} else {
			nslog_log(__FILE__, "", __LINE__,
				      "NetSurf on <%s>, node <%s>, release <%s>, version <%s>, machine <%s>",
				      utsname.sysname,
				      utsname.nodename,
				      utsname.release,
				      utsname.version,
				      utsname.machine);
		}
	}

	return ret;
}

/* exported interface documented in utils/log.h */
nserror
nslog_set_filter_by_options(void)
{
	if (verbose_log)
		return nslog_set_filter(nsoption_charp(verbose_filter));
	else
		return nslog_set_filter(nsoption_charp(log_filter));
}

/* exported interface documented in utils/log.h */
void
nslog_finalise(void)
{
	NSLOG(netsurf, INFO,
	      "Finalising logging, please report any further messages");
	verbose_log = true;
	if (logfile != stderr) {
		fclose(logfile);
		logfile = stderr;
	}
#ifdef WITH_NSLOG
	nslog_cleanup();
#endif
}
